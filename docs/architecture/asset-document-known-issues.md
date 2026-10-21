# AssetDocument known issues and follow-ups

Open issues that are known but not yet fixed. Each item has its impact, the current workaround and the intended fix. When an item is fixed, delete it from this file and name it in the fixing commit.

Last updated: 2026-10-21.

## Correctness

### KI-13 The Niagara Script editor cannot close without its unapplied edits
- **What:** `asset_editor close` with `discard_unapplied: true` closes the Material, Material Function, Niagara System and Niagara Emitter editors without their pending edits (a close with `EAssetEditorCloseReason::AssetForceDeleted` skips the apply prompt). `FNiagaraScriptToolkit::OnRequestClose` ignores the close reason and always asks, and its "discard" state is private, so a Niagara Script editor with pending edits refuses the close (`Assets.EditorNotClosed`).
- **Workaround:** `asset_editor apply_editor`, `asset_editor close`, then `reload_assets` with `force: true` (the refusal message names this sequence).
- **Fix:** An engine hook to close the script toolkit without its prompt.


### KI-14 The MetaSound editor resets a Float input's WidgetType when it opens (engine, UE 5.7)
- **What:** The MetaSound editor graph is transient (UE 5.7), so each first open of a MetaSound binds its member settings objects anew and `UMetasoundEditorGraphMemberDefaultFloat::Initialize` sets every Float input's `WidgetType` to the editor preference Default Input Widget Type (`RadialSlider`), whatever the asset held. The document's widget defaults are the editor's, so opening the editor changes nothing a document with default widgets shows; a document that sets another `WidgetType` (e.g. `Slider`) diffs once the editor has opened. UE 5.8 no longer counts the re-bind of existing settings as new and keeps the asset's `WidgetType`. Pinned on both engines by `MetaSound.OpenEditorWidget`; the guide says so.
- **Fix:** None on our side without changing the engine. Revisit when the test's pinned reset changes.
### KI-16 Opening a Harmonix Fusion MetaSound's editor without an audio device asserts (engine)
- **What:** In an editor without an audio device (`-NoSound`, as automation runs use), the MetaSound editor's palette registers the loaded MetaSounds' graphs when it builds (`SMetasoundPalette` -> `FAssetSchemaQueryResult::FindGraphClass` -> `FGraphBuilder::RegisterGraphWithFrontend` -> `FProxyDataCache::CreateAndCacheProxies`). For a MetaSound whose literals hold a Harmonix Fusion patch, `UFusionPatch::CreateProxyData` then hits `check(SoundWaveProxy)` (HarmonixDsp `KeyzoneSettings.cpp:48`: `USoundWave::CreateProxyData` returns null without audio) and the editor crashes. Engine only: no AssetDocument code is on the stack. Repro with the editor's own Python only: `UnrealEditor-Cmd <project> -NoSound -unattended "-ExecCmds=py repro.py"`, where the script loads `/Harmonix/Examples/Sources/MSS_MIDISequencePlayer` and `/Metasound/DefaultAssets/Sources/MSS_PlayRandom_Oneshot`, opens the Oneshot one's editor (`AssetEditorSubsystem.open_editor_for_assets`), then, on a later tick, the MIDISequencePlayer's: assert. The same run with audio does not assert.
- **Our side:** Create / Update of a MetaSound whose literals (or whose referenced MetaSounds') hold a Fusion patch are refused without audio (`MetaSound.AudioDeviceRequired`), so our applies never register one. The tests that open a MetaSound editor (`MetaSound.OpenEditor`, `OpenEditorWidget`, `PagesOpenEditor`) unload such MetaSounds first (`UnloadMetaSoundsNeedingAudio`); no other test opens a MetaSound editor.
- **Our tools:** `asset_editor open` refuses a MetaSound editor without an audio device (`Assets.EditorNeedsAudio`, test `AssetOps.AssetEditorMetaSoundNoAudio`).
- **Workaround:** run the editor with audio (no `-NoSound`) to open a MetaSound editor; documents (extract, apply) work without it.
- **Fix:** None on our side without changing the engine.

### KI-19 The editor's Duplicate of a Control Rig drops reroutes and links (engine, UE 5.8.3 and later)
- **What:** The editor's in-memory Duplicate of a Control Rig (Content Browser Duplicate, `IAssetTools::DuplicateAsset`) post-loads the copy without post-loading its rig graph and function library: `FRigVMClient::PatchModelsOnLoad` calls `ConditionalPostLoad` on them, and a duplicate does not flag an asset's default subobjects for PostLoad, so every pin's link list is still empty while the client patches and refreshes the graphs. A reroute node then reads as unlinked and is removed, with the links through it; the log shows `Cannot break link <A> -> <Reroute>.Value in package <copy>` for each, and the copy is saved without them (CR_AllKinds: the variable getter -> reroute -> If.True chain is gone). A rig whose post-load skips the graph refresh (its structure hash is unchanged, e.g. AnimatorKit `CRU_AddLocator`) keeps its links in the graph but compiles without them (4 of its 14 instructions; its construction event runs nothing) until it is reloaded from disk. UE 5.7 and 5.8.1 post-load the graphs unconditionally and are not affected.
- **Our side:** `duplicate_asset` and Validate's trial copy of a rig flag the copy's model graphs for PostLoad before it post-loads (the Control Rig duplicate fixup, `AssetDocumentDuplicateFixups.h`), so the copy post-loads as a rig loaded from disk: it extracts and compiles as the source (test `ControlRig.Duplicate`, which also runs the editor's Duplicate as the reference).
- **Workaround:** duplicate rigs with `duplicate_asset`. A copy made in the Content Browser that lost reroutes is repaired by an apply of its own extract with the source's `Graphs`, `GraphOrder` and `Functions` (`Action: Update`; the restored nodes come last in the graph's node order, reported as `ControlRig.OrderKept`, and the rig compiles as the source); one that only compiles without its links is right after a reload (`reload_assets`) or an editor restart.
- **Fix:** None on our side for the editor's own Duplicate without changing the engine.

## Quality and noise

### KI-4 Large extracts (Control Rig)
Inspect is summarized by default (the shared `inspect_summary.py`: a large `Body` as an outline, bulk facts left out, one `Summary` line; `detail: true` for the whole). Default inspect / extract payloads (compact JSON) on sample assets: `CR_Mannequin_Body` 14.9 / 403.5 KB (inspect was 866 KB, `detail` still is), Widget Blueprint `AudioButtonMatrix` 1.4 / 56.7 KB (was 57 KB), Material `M_Mannequin` 20.2 / 21.4 KB (was 39 KB), Anim Blueprint `ABP_Manny` 1.9 / 36.4 KB (was 37 KB), Physics Asset `PA_Mannequin` 7.9 / 29.7 KB, Skeleton `SK_Mannequin` 10.5 / 21.0 KB, IK Rig `IK_Mannequin` 12.9 / 4.7 KB, Niagara `SimpleExplosion` 22.0 / 12.4 KB (`detail` 140 KB), MetaSound `MSS_PlayRandom_Oneshot` 5.7 / 5.9 KB.
- **What is left:** the Control Rig extract. It is document content, not defaults: `Functions` 234 KB, `Graphs` 65 KB, `Hierarchy` 104 KB (446 elements); over its 1341 nodes: links (`Inputs`) 73 KB, `Position` 35 KB, `Order` 34 KB, node keys 31 KB, `Defaults` 25 KB, `Unit` 17 KB, `PinTypes` 17 KB, `Template` 13 KB. Pin defaults, transforms and control settings are already sparse (only what differs from a new node / identity / the defaults). Layout (`Position`, `Size`, `Expanded`) and `Order` are not defaults: an extract applied as a copy (`Action: Create` on a new `Target`) needs them for the same layout and compile order, so `diff_only` keeps them.
- **Fix:** an extract of selected sections or functions (paging), if the size proves a problem.

## Process

### KI-9 Unconfirmed: a dependent Blueprint left unsaved by a component Blueprint apply
- **What:** After `BPC_CombatCore` was applied, the dependent `BP_SamplePlayer` was unsaved, so the next apply of it failed with `DirtyExistingAssetTransactionUnsupported` (worked around by saving it through Python).
- **Status:** Not reproduced on 8547dc43. `Blueprint.ApplySafety.DependencyCompileKeepsDependentsClean` (structural change and default change of the component Blueprint, a Character parent holding it as SCS component, a child with an inherited override and calls into it, an instance placed in the editor level) keeps every dependent saved, as the editor's compilation manager does (it restores each dependent's dirty flag). An unsaved user edit is still refused.
- **Next:** If it happens again, record which dependent packages were dirty and what else was open (a Blueprint editor tab, the level), and extend that test.

### KI-18 Unconfirmed: a PIE compile leaving a Blueprint package dirty (Enhanced Input)
- **What:** After PIE, the next apply of the hero Blueprint was refused as an unsaved target. One cause is fixed: saving a custom event with a struct input (`Key`) re-ran the pin default fixup after the apply's compile, leaving the Blueprint Dirty, so PIE recompiled it (fixed: the fixup runs before the compile).
- **Status:** Not reproduced after that fix. `Level.Play.BlueprintCompileState` plays the hero, also with its status forced to Dirty, and the PIE compile leaves the package clean (the compilation manager restores the dirty flag); the apply after play succeeds.
- **Next:** If it happens again, record the Blueprint's status before play and what was open (a Blueprint editor tab), and extend that test.
