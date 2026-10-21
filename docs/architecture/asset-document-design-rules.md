# AssetDocument design rules

The rules the AssetDocument profiles, the support matrices and the design notes in this directory refer to. Section numbers are stable: the notes cite them as "§3", "§4", "§5".

## 1. Goal

An AI-friendly Unreal Engine MCP: every asset is read and written as an `.assetdoc.json` document (`Meta` / `Definitions` / `Properties` / `Body`). Progress is measured by what an AI can do to each asset type through MCP, how reliably, and whether it can understand and correct a failure. The internal architecture serves that and is not itself the acceptance test.

The architecture is component-based in Python: a profile's meaning is assembled from reusable Python components over generic C++ editor operations. The target is that a new asset type needs Python only, assembled from existing components.

## 2. Hard boundaries

- The engine is never modified: no source, binaries or build files of the stock engine.
- The `.assetdoc.json` shape stays compatible. The only public mutation entry is the sidecar-backed apply of a file; read-only discovery and query tools may be added.
- Python and C++ talk through versioned DTOs, stable diagnostics and opaque handles. One writable runtime owns an asset type at a time; a Python failure never silently falls back to another implementation.

## 3. Rollback: functional equivalence

A failed apply is rolled back successfully when:

1. **Same logic.** The canonical extract of the asset equals the one before the apply.
2. **Nothing broken.** The compile state is no worse; derived runtime structures (a Behavior Tree's runtime tree, a Blueprint's generated class) are rebuilt from the restored authored state; other loaded assets do not hold dangling references to it.
3. **Disk untouched.** The original package and sidecar on disk were not rewritten.
4. **No leftovers.** No new package, sidecar, registry entry or stray owned object remains.
5. The dirty flag is restored where possible; staying dirty is acceptable.

Native object identity, GUIDs (unless persisted references or visible behaviour depend on them), pin / link / container order, flags and outers need not match; an equivalent rebuild is enough.

The core provides the mechanism, in this order: restore a whole-package object snapshot, then run the provider's derived-state rebuild hook; as a last resort reload the untouched package from disk. A provider declares its mutation footprint and, if needed, a rebuild hook; it does not write an inverse for each primitive. Failure injection happens at the four phase boundaries: mid-apply, compile / repair, staged save, atomic install.

### 3.1 Editor Undo history

An AI apply does not enter the user's Undo stack and cannot be undone with Ctrl+Z (re-apply the previous document, or use version control). So that a later Ctrl+Z cannot leave an asset half before and half after an apply:

- After a successful apply that changed the asset, the Undo history is cleared if it records or references the asset's package; otherwise it is kept.
- After a failed apply that was rolled back, the asset returns to its pre-apply state with the same objects and the history is kept, unless the editor's own operations recorded transactions during the apply; those are stale and the history is cleared.
- A rejected or no-op apply leaves the history alone. Nothing is cleared while a transaction or an undo / redo is open.

### 3.2 Assets that depend on a changed type

When a user-defined struct or enum changes, the editor updates every loaded user itself (DataTable rows, Blueprint recompilation, containing structs, enum values). The rules:

- **Do what the editor does.** Call the editor's own struct / enum utilities; no migration logic of ours.
- **Refuse when a dependent has unsaved changes.** The edit is refused with a `DependentHasUnsavedChanges` diagnostic before anything changes, because their unsaved state could not be restored on failure.
- **Report on success.** Every package the editor marked dirty is reported as a warning; dependents that were saved before the apply and are predicted are left saved.
- **Restore on failure.** The core restores the type package and the dependents saved before are reloaded from disk.
- Users held in `FInstancedStruct` / property bags and actors in an open level are not predicted; they are reported when they become dirty.

## 4. Using engine APIs

- Prefer public factories, schema actions and editor utilities.
- Code that the editor itself uses, including raw construction and direct container writes, may be used the same way, in its own file with a behaviour test.
- Still forbidden: modifying the engine, including private engine headers, linking unexported symbols, friend or layout hacks.
- Where something cannot be done, return a stable diagnostic an AI can read; never fall back silently.

Behaviour rules that follow from this:

- **4.1 No modal windows.** Factories run without UI. A modal attempt discards the packages created during the call and returns `FactoryCreate.InteractiveFactory`. Factories that dereference properties only the New dialog sets return `FactoryCreate.MissingFactoryContext` when those are empty.
- **4.2 Factory configuration.** Creating an asset supplies what the New dialog would ask for (a class picked on an ancestor factory, an optional top-level `FactoryContext` object path) or refuses with a diagnostic that names what is missing.
- **4.3 Preflight touches no real asset.** Properties are pre-checked on a transient scratch clone with the same write path as the Details panel. Every non-transient package must be identical afterwards, content and dirty flag; otherwise the apply is refused (`PreflightIsolation.PackageNotRestored`). Values the editor would rewrite (clamping, renaming) are refused with `Generic.PropertyValueNotStoredExactly`.
- **4.4 Float precision.** A JSON number written to a float property is accepted as the nearest float32 value, as the Details panel does; extract writes the shortest decimal that reads back to the same float, so an extract applied unchanged is a no-op.
- **4.5 Apply, validate and diff.** The same sidecar content is applied once (the file watcher and an explicit apply do not repeat each other). Create / update is decided by whether the target exists. Reads never leave an empty package behind. Validate runs apply's checks without changing anything and says what apply would do; diff lists the smallest locatable paths with `before` (the asset) and `after` (the document). Numbers keep their written form (`1` is an integer, `1.0` a float).

## 5. Done means a support matrix

- Each asset type keeps a support matrix: read, create and modify, by Body key or common editing operation.
- What people commonly change in the Unreal Editor must be supported through MCP. Rare or edge forms may be unsupported but must return a stable diagnostic saying what is unsupported and what the AI can do instead.
- Replacing an older route must not reduce what can be done.
