# Sidecar mirroring and bidirectional sync: design

Status: design. Governing documents: [architecture](asset-document-architecture-and-extension-principles.md) §1, §4.2,
§6, §10, §11, and the [design rules](asset-document-design-rules.md).

With mirroring on, every in-scope `/Game` asset has a `<AssetName>.assetdoc.json` next to it, and the editor keeps
the file and the `.uasset` in step: a save updates the file, and a file edit updates the asset. Agents read assets as
files (grep, no tool calls) and edit them by writing files. Mirroring is a per-project plugin setting, off by
default. **With it off, nothing in this document applies and today's behaviour stays exactly as it is.**

## 1. Terms

- **Mirror**: the diff-only extract of the saved asset (`extract_asset_document`, `diff_only: true`) in the sidecar
  file encoding, plus `_meta`. After every completed sync, the sidecar is byte-identical to the mirror.
- **Region**: the unit of change detection. The regions are `/Class`, `/Properties`, and each top-level `/Body/<Key>`
  (or the body paths the profile declares as regions). `Target`, `SchemaVersion`, `Action` and `BaseRevision` are not
  regions: `Target` is fixed by the file location, and the others are apply-time intent. `Definitions` is not a region.
  A sidecar region is hashed with its `DefinitionRef`s expanded, so editing a definition changes exactly the regions
  that use it. Reason: definitions are document-local sugar, and they have no asset-side counterpart.
- **Sync pass**: one run of §3 for one asset. All passes run one at a time on the game thread through the existing
  ingress queue, so watcher events, saves, tool calls and reconcile never interleave on one asset.

## 2. Sidecar format

### 2.1 `_meta`

```json
"_meta": {
  "sync": {
    "Version": 1,
    "Package": "<saved hash of the .uasset at the last sync>",
    "Regions": {
      "/Class":      {"Sidecar": "adsh:v1:region:…", "Asset": "adsh:v1:region:…"},
      "/Properties": {"Sidecar": "adsh:v1:region:…", "Asset": "adsh:v1:region:…"},
      "/Body/Keys":  {"Sidecar": "adsh:v1:region:…", "Asset": "adsh:v1:region:…"}
    }
  },
  "Skipped": {}
}
```

- The sidecar encoding sorts keys, so `_meta` is the last member and readers see the content first.
- The hashes are Python semantic hashes with region scope (`compute_semantic_hash`, `SemanticHashScope.REGION`). They
  are computed over typed evidence: present, explicit empty, absent by policy. `Sidecar` is over the file's region, and
  `Asset` is over the mirror's region. After a normal sync the two are equal. They are kept apart so that a change in
  how extract represents an asset (after a plugin update) is told apart from an edit.
- `Package` is the package saved hash that the engine records in the package summary and that the Asset Registry
  reports. Reading it needs no package load and no file read. It drives the cheap startup check (§4.3).
- `Skipped` is the extract's `Body._Skipped`, verbatim: what the file cannot express. It is informational.
- A missing or malformed `_meta.sync`, an unknown `Version`, or a region hash whose codec prefix is not the current
  one, means there is **no trustworthy last state** for the affected regions. The initialization rules (§3.3) apply.
  Reason: never guess a direction from state we cannot read.
- `_meta` changes only when a sync completes. A conflict never touches it.

### 2.2 How the tools treat `_meta`

- Apply, validate and diff accept a top-level `_meta` object and ignore it. The service strips it before the profiles
  run, as it does with `BaseRevision`. A `_meta` that is not an object is rejected with `Document.InvalidMeta`. This
  holds with mirroring off too, so a mirrored file can be applied or diffed anywhere.
- Extract never emits `_meta`. `extract_asset_document` stays read-only and never writes a file, even with mirroring
  on. Reason: read tools do not write, and materialize (§5) is the writing path.
- Applying an unchanged mirror is a no-op: it is an unchanged diff-only extract (an existing guarantee), and `_meta`
  is ignored.
- With mirroring off, an apply's sidecar rewrite carries no `_meta` (as today). The next sync with mirroring on then
  starts from §3.3.

## 3. The sync pass

### 3.1 Evidence

1. Read the sidecar, if there is one. Python returns the per-region sidecar evidence (see §9 for the C++/Python
   split).
2. Asset evidence: for a pass triggered by a sidecar change, if the package saved hash equals `_meta.sync.Package`,
   the asset is unchanged: reuse the recorded `Asset` hashes and do not extract. Otherwise extract (diff-only), and
   Python returns the per-region asset evidence together with the mirror bytes.
3. If either side's evidence for any region is unavailable, the pass writes nothing. Examples: invalid JSON, a profile
   that rejects the file, an extract failure, a dirty target (§6). It posts a `sidecar_sync_failed` notice with the
   diagnostic. Reason: §6 of the architecture, "evidence unavailable".

### 3.2 State table (per region, against `_meta.sync`)

| Sidecar vs last | Asset vs last | Action |
| --- | --- | --- |
| same | same | nothing |
| changed | same | sidecar → asset |
| same | changed | asset → sidecar (rewrite the region from the mirror) |
| changed | changed, and the two sides equal | converged: stamp |
| changed | changed, and different | conflict |

The classification reuses `ClassifyAssetDocumentSyncDirection` (`AssetDocumentSyncCoordinator.h`).

**Equal** means the region hashes are equal, or the sidecar restricted to that region diffs empty against the asset
(`diff_asset_document` semantics). Hand-written files that are not in mirror form therefore still compare correctly.
"Converged" is not a conflict, because there is nothing to choose. It also covers an interrupted pass (§3.4).

**Writes, when the pass has no conflict:**

1. If any region is "sidecar → asset", first rewrite the "asset → sidecar" regions in the file from the mirror.
2. Then run the existing `ApplyFile` on the file, whole document. It is unchanged: preflight, transaction, staged
   save, fresh verification, journaled install, undo trimming, open-editor handling. Step 1 makes the apply leave the
   asset-changed regions as they are.
3. Last, write the mirror of the saved asset, with the new `_meta.sync`, atomically. Do this only when bytes would
   change: an unchanged pass writes nothing.

So last-sync state advances only after the package and the sidecar are both durable (architecture §6). Region
independence holds without a region-scoped apply: a sidecar edit in one region and an editor edit in another both
survive.

**Conflict:** if any region conflicts, the pass writes nothing to the sidecar or the asset for that asset. It writes
`<AssetName>.assetdoc.asset.json` next to the sidecar: the asset's mirror, with `_meta.conflict.Regions` listing the
conflicted regions. It also posts a `sidecar_sync_conflict` notice and a log warning. Non-conflicting regions of that
asset wait until it is resolved. Reason: the apply is whole-document and file-backed, so applying while a conflicted
region sits in the file would overwrite one side. The conflict is recomputed on every pass and is not persisted, so a
restart reports it again. The conflict file is removed when the asset next syncs cleanly. It does not end in
`.assetdoc.json`, so the watcher ignores it.

### 3.3 Initialization (no trustworthy last state)

| Situation | Action |
| --- | --- |
| No sidecar; in scope; extract non-empty | write the mirror |
| No sidecar; extract empty or failed | skip; remember the reason (`status`) |
| Sidecar, region without state, equal to the asset | stamp (the file is rewritten in mirror form) |
| Sidecar, region without state, absent in the file | take the asset (an omitted region keeps its current state) |
| Sidecar, region without state, different | init conflict (§3.2 conflict handling); never guess |
| Sidecar without `_meta`; asset missing | apply (create) |
| Sidecar with `_meta.sync`; asset missing | `Sync.AssetMissing` notice; do nothing (no resurrecting assets deleted outside the editor) |

Once a sidecar exists, it stays mirrored even if the extract becomes empty. Reason: deleting files based on their
content is surprising.

### 3.4 Interruption

A crash or failure between the steps in §3.2 leaves the file either with regions already equal to the asset or with
the applied document under the old `_meta`. The next pass classifies those regions as unchanged, converged or still
sidecar-changed. It never reports a false conflict and never loses an edit.

### 3.5 Edits during a pass

A pass reads the sidecar once, then spends time in Python, the extract and the apply. An agent may write the file
again in that time. So every write of the sidecar by a pass (the merge, the mirror) and the apply first check that the
file still has the content the pass read, or the content it last wrote itself (after the apply: the apply's own
rewrite). The writer re-checks right before it replaces the file. When the content differs, the pass stops with state
`changed`: nothing more is written, the new content stays, and the asset gets another pass in the runner's queue.

- When the pass had already applied the sidecar, it keeps the state its mirror would have recorded. The next pass uses
  that state, while the package is unchanged, in place of the file's `_meta.sync`. The new content then counts as a
  sidecar change since that apply, not as a conflict with it. The state is kept in memory only: after an editor
  restart the same edit can come up as a conflict, never as a lost edit.
- The guard that hides the editor's own writes from the file watcher records the bytes the editor wrote (through the
  sidecar writers and the apply's install), not the file as it is when the guard ends. A change someone else makes
  while an apply or a pass runs therefore stays a change for the watcher.
- Every saved apply (also without mirroring) ends with its own canonical rewrite of the file. That rewrite replaces
  the file only while it still has the content the apply read; otherwise the newer content stays and the watcher
  takes it as a change.

## 4. Triggers

### 4.1 Editor save → mirror

- `UPackage::PackageSavedWithContextEvent` only enqueues the package name. It never blocks the save and never extracts
  inside it.
- Events are coalesced per package and drained by the time-sliced runner (§4.4), so a Save All becomes one batch with
  progress.
- Ignored: procedural (cook) saves and autosaves; saves whose filename is not the package's own file (staging
  copies); saves of a package while a sync pass or apply for it runs (a scoped guard); commandlets (mirroring runs
  only in the interactive editor).
- When the event is processed, a package that is dirty again is skipped. Its next save mirrors it. Reason: the mirror
  reflects saved state only (§6).

### 4.2 File change → sync

The existing watcher, debounce and `FScopedSidecarWrite` suppression of our own writes stay. With mirroring on, a
changed sidecar of a mirrored asset queues a sync pass in the runner (§4.4) instead of a bare `ApplyFile` in the
event, so many changed files (a checkout, a pull) are many passes over many ticks, with garbage collection between
batches. `FCA_RescanRequired` queues every mirrored sidecar of the folder the same way, without reading them: the
runner answers each one whose file and package are as the last sync left them from the local index (§4.3), with no
pass. An explicit `apply_asset_document_file` runs the sync pass at once. Reason: one path,
so no route can silently overwrite an editor change. Overriding is `accept_sidecar` (§5). The tool returns
`Sync.Conflict` (with the regions and the conflict file) instead of applying.

### 4.3 Editor start → reconcile

- Starts once the Asset Registry has finished its initial scan and the Python runtime is ready.
- Visits existing sidecars only:
  - **Fast path:** `stat` of the sidecar plus a registry lookup of the package saved hash. A local index in
    `Saved/AssetDocument/SidecarSync/` records, per sidecar, the file size, timestamp and content digest at the last
    sync. When the file is unchanged against the index and the saved hash equals `_meta.sync.Package`, there is
    nothing to do: no Python call, no extract.
  - When the stat differs, the content digest decides. When the file differs, Python hashes the sidecar.
  - Only a differing saved hash triggers an extract.
  - The index is an optimization only. Deleting it costs time, never correctness. A fresh clone without it still skips
    extracts through `_meta.sync.Package`.
- Then, assets in scope without a sidecar (registry enumeration plus a file-existence check) are materialized
  automatically in the background through the runner (§4.4): at editor start, and when the setting is turned on.
  Assets whose extract failed or was empty are remembered in the local index with their saved hash and are not
  retried until the package changes. Cancel stops the run; the next start resumes it.
- Offline sidecar edits are applied here, and offline `.uasset` changes (a git pull) are mirrored here.
- Discovery is time-sliced too: listing one directory, checking one sidecar against the index, or checking one asset
  for a missing sidecar is one step under the runner's per-tick budget, so a large project never blocks a frame.
  Only the assets that need a pass are queued; the run then proceeds as in §4.4.

### 4.4 Runner, progress, scale

One time-sliced runner on the game thread serves saves, reconcile and materialize.

- It processes queued assets under a per-tick budget. One extract or apply is one unit.
- It shows a non-modal progress notification with Cancel, and reports `done/total` in `health_check`.
- Memory stays bounded: the runner holds no reference to the assets it extracted, and collects garbage after every
  batch (a fixed number of assets), so packages it loaded are released.
- Packages that were already loaded and dirty before the pass are skipped (`Sync.TargetDirty`). Packages loaded by the
  pass are taken as saved state.
- Other work (tool calls, watcher events) is queued between units. A tool call for one asset runs that asset's pass
  directly.

## 5. Tool: `sync_asset_document`

One new MCP tool, with `action`:

- `status`: per asset: `in_sync`, `sidecar_changed`, `asset_changed`, `conflict` (with regions and the conflict
  file), `unavailable` (with the diagnostic), `asset_missing`, `unmirrored`, `skipped` (with the reason: extract
  failed with its code, empty, too large, excluded), `excluded`, `off`. Without `targets` it gives project counts plus
  every asset with an open issue, from the runner's records. With `targets` (assets or `/Game` folders, at most 500
  assets) it checks them fresh, as cheaply as the files allow:
  - With mirroring off each asset is `off` (or `excluded`), and no pass runs.
  - An asset without a sidecar is `unmirrored`, or `skipped` while the index remembers a skip for its current saved
    state, without an extract.
  - A pair unchanged since its last sync (the reconcile fast path) is `in_sync` without a pass.
  - Only the others get a dry-run pass, and the packages those load are released after every batch.

  It also reports runner progress and the last materialize summary.
- `accept_sidecar` / `accept_asset`: `targets` (assets) and optional `regions` (JSON Pointers; default: every
  conflicted region). Resolution **forces the direction of each accepted region whose two sides differ, then runs a
  normal pass**:
  - `accept_sidecar`: the region is "sidecar → asset" (the file is applied).
  - `accept_asset`: the region is "asset → sidecar" (the region is rewritten from the asset).
  - This holds whichever side changed since the last sync: naming a region only the other side changed still takes
    the accepted side, so a rejected edit does not stay in the file to be applied later.
  - A region whose sides are equal is converged. Regions not accepted keep their normal classification, so mixed
    choices per region need no extra machinery.
- `materialize`: `targets` (default: all of `/Game`). It queues every in-scope asset and runs §3.3 through the runner,
  also those the background run skipped as failed or empty (a re-run).
  It returns at once with `total`. Progress and the summary (written, in sync, skipped by reason, conflicts) come from
  `status` and `health_check`.

**Why one tool:** apply and extract keep their contracts (extract stays read-only; apply only gains the conflict
check); resolution and adoption are sync-state actions that fit neither; four actions behind one name add one tool.

## 6. Interaction with existing behaviour

- **BaseRevision / `Document.StaleBase`:** unchanged for documents that carry it. Mirrors never carry it, and the
  rewrite drops one an agent added. For file edits, the per-region `Asset` hashes play its role at region grain:
  conflict instead of `StaleBase`.
- **Watcher once-per-content and the apply ledger:** kept. The pass's own content check (hashes against `_meta`)
  subsumes once-per-content for mirrored files. The ledger still makes an explicit apply right after a watcher pass
  return that pass's result (`applied_by: "file_watcher"`). The ledger records the final mirror bytes as applied
  content.
- **The per-machine typed sync record** that the apply transaction writes under `Saved/AssetDocument/Runtime/Sync`:
  unchanged. It is rollback bookkeeping. Direction decisions use only `_meta.sync`.
- **EditorSync:**
  - Rename and duplicate move or copy the sidecar with its `_meta`. Region hashes do not include `Target`, so the next
    pass extracts (new saved hash), finds the regions equal and only restamps `Package`.
  - Delete still archives to `Content/_DeletedAssetDocs/`, which the sync ignores.
  - Deleting a sidecar never deletes the asset. The mirror returns on the next save or materialize.
- **Undo history:** sidecar → asset goes through `ApplyFile` and trims undo as today. Asset → sidecar touches no
  object and no undo entry.
- **Assets open in their editor:** sidecar → asset uses the existing open-editor handling (refuse, close and reopen,
  or refresh). Asset → sidecar extracts after the save, from the saved object.
- **Unsaved (dirty) assets:** the mirror reflects saved state only. A sidecar change for a dirty target keeps today's
  skip and the `AssetDocument.WatcherSkippedDirtyTarget` notice. When the user then saves, the save pass sees both
  sides changed: it reports a conflict, or stamps them if they converged.
- **Notices:** `sidecar_sync_conflict` and `sidecar_sync_failed` (per sidecar) are resolved by the asset's next clean
  pass. Apply failures keep the existing watcher-apply notice.

## 7. What agents see

Guide changes (`MCP/schemas/AssetDocument.md`, "Sidecar Files And The File Watcher", plus a new "Mirroring"
subsection):

- With mirroring on (`health_check` → `sidecar_sync.enabled`), read assets from their `.assetdoc.json` files and edit
  by writing them. The editor applies the file, then rewrites it as the mirror. A removed `Properties` line or Body key
  comes back, because omission keeps the current value. To reset a value, write its default.
- A file is current when `health_check.sidecar_sync` shows `state: "idle"` (not `reconciling` or `materializing`). A
  mirrored file always is a consistent snapshot of the saved asset at its last sync. Unsaved editor changes are not in
  it until saved. While the editor is closed, nothing is updated. Edits made then are applied when the editor next
  starts.
- `_meta` is system-maintained: leave it as it is. `_meta.Skipped` lists what the file cannot express.
- Conflicts arrive as pending notices (next tool result, `get_editor_context`). The fix: compare the sidecar with
  `<AssetName>.assetdoc.asset.json`, edit the sidecar to the wanted result, then `sync_asset_document accept_sidecar`
  (or `accept_asset`).

## 8. Settings and version control

Settings live in Project Settings → Plugins → UE-AssetDoc, in the protected settings: the user changes them, and no
tool does. They take effect immediately.

- `bMirrorSidecars` (default off).
- `MirrorExcludedPaths`: `/Game` prefixes. Default: `/Game/__ExternalActors__` and `/Game/__ExternalObjects__`
  (actor and object packages, not authored assets).
- `MirrorExcludedClasses`: subclasses included. Default: `/Script/Engine.World`. Reason: extracting a level loads it,
  and levels are edited with the level tools.
- `MaxMirrorFileSizeKB` (default 8192): larger mirrors are skipped as `too_large`. Reason: keep files usable for text
  tools and diffs.

**Version control:**

- Commit sidecars, with their `_meta`, alongside the `.uasset`. The state must travel with the files, otherwise every
  clone re-initializes.
- Ignore `Content/_DeletedAssetDocs/` and `*.assetdoc.asset.json`.
- Set `*.assetdoc.json text eol=lf` (the mirror writes LF), so Windows checkouts do not churn.
- **Merging:** `.uasset` does not merge, and sidecar text usually does.
  - Keep the `.uasset` of one side, merge the sidecar content, and take the `_meta` block from the side whose
    `.uasset` you kept. Reconcile then sees "sidecar changed, asset unchanged" and applies the merged content.
  - With the wrong `_meta`, or with `_meta` deleted, the result is a conflict or an init conflict. That fails closed,
    and `accept_sidecar` finishes the merge.

## 9. Implementation slices

Ownership follows architecture §7.7. **Python:** canonical region evidence and mirror bytes. **C++:** delegates,
queue and runner, files, `_meta` and index IO, the direction table, conflict files, notices, settings, transport.
Every slice keeps all existing tests unchanged with mirroring off.

1. **`_meta` acceptance (C++).** Strip `_meta` beside `BaseRevision`; reject a non-object `_meta`; guide. Tests:
   apply, validate and diff give identical results with and without `_meta`; `Document.InvalidMeta`; mirroring off
   unaffected.
2. **Region evidence (Python + C++ caller).** A read-only `Sync` runtime operation: document plus optional meta in,
   typed per-region evidence (and the sidecar bytes when asked) out, with `DefinitionRef` expansion on the sidecar
   side. pytest: key-order independence; present, empty and absent kinds; definitions expansion; `_meta` and `Target`
   excluded; profile-declared regions; codec prefix. C++: a mirror's evidence equals its extract's.
3. **State codec and index (C++).** `_meta.sync` and local index IO. Tests: malformed, unknown-version or
   codec-mismatched state means no state; index keys normalized (Windows backslashes and case-insensitive paths,
   macOS case-preserving paths, a moved project directory).
4. **Sync pass (C++).** §3 complete: write ordering, conflict file, notices, rebaseline resolution. Tests on a generic
   DataAsset and one multi-region exact profile, covering every row of §3.2 and §3.3: mixed directions in one pass,
   converged, evidence unavailable on each side, asset missing with and without `_meta`, an interrupted pass between
   each pair of steps (test hook), a new region after a profile change, conflict-file path checks (same folder only,
   `ResolveSidecarFile` rules for links).
5. **Triggers (C++).** Save delegate with coalescing and filters, watcher and rescan routing, explicit apply through
   the pass. Tests: one save gives one mirror write; an apply's own saves are ignored; dirty-at-processing is skipped;
   our own writes do not re-trigger; Save All batches; commandlets do nothing.
6. **Runner, reconcile, materialize, tool (C++/TS).** Time slicing, progress, cancel, GC; excludes and size limit;
   `sync_asset_document`; `health_check` fields; `tools.json`; guide. Tests: the reconcile fast path performs zero
   extracts (test-hook count); offline sidecar edit applied at start; offline package change mirrored; excluded path
   and class; too large; background materialize at start and on enabling, its cancel and batch GC; every tool action;
   the MCP manifest test.
7. **EditorSync interplay (C++).** Tests: rename and duplicate restamp without conflict; delete archived and ignored;
   a deleted sidecar returns on save. Path cases on Mac and Windows: deep folders near the Windows path limit, names
   that differ only in case, spaces and non-ASCII names, LF output on Windows.

Slices 1–3 are independent. Slice 4 needs 2 and 3. Slices 5 and 6 need 4. Slice 7 can run with 5.

## 10. Open questions

None.
