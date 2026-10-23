# WidgetBlueprint support matrix (legacy C++ profile vs Python route)

Governing rules:
[design rules](asset-document-design-rules.md) §3 (rollback = functional
equivalence), §4 (raw editor-equivalent code in its own file + behaviour test) and §5 (support
matrix, no capability regression).

`/Script/UMGEditor.WidgetBlueprint` is read, created and updated by the Python profile
`profile.widget_blueprint.v1` (`Content/Python/asset_document/profiles/widget_blueprint_runtime.py`,
`widget_blueprint_tree.py`, `widget_blueprint_animation.py`) on the `blueprint_profile` route.
The legacy C++ `FWidgetBlueprintAssetDocumentProfile` (and the other legacy C++ profiles) has
been deleted; the Python route is the only route.

Columns: **Legacy** = the retired `FWidgetBlueprintAssetDocumentProfile`; **Python** = the current profile.
Classification: **Supported** (Extract + Apply + Diff), **Read-only** (Extract/Diff only; authoring
a different value fails with a stable diagnostic), **Unsupported** (not in the document, or only
`[]` accepted; reason given).

Mechanisms: `blueprint.member.*` (shared with UBlueprint), the K2 graph executor (shared with
UBlueprint), and the new provider `umg` (`Source/AssetDocumentWidget/`):
`umg.tree.query/add/move/remove`, `umg.widget.variable/property/navigation.set`,
`umg.binding.set/remove`, `umg.desired_focus.set`, `umg.palette_category.set`,
`umg.design_size.set`, `umg.anim.query/set/remove`, `umg.anim.track.set/remove`. Raw
editor-equivalent edits are in `AssetDocumentWidgetRawEdits.{h,cpp}`, checked against the editor
by `UEAssetDoc.WidgetBlueprint.RawEdits`.

## 1. Designer: hierarchy (widget tree)

| Editor surface | Body location | Legacy | Python | Class | Mechanism / reason |
|---|---|---|---|---|---|
| Create a Widget Blueprint (Content Browser → User Widget, pick parent) | `ParentClass` + `Action: Create` | ✅ | ✅ | Supported | `UWidgetBlueprintFactory` through the Blueprint lifecycle row (`blueprint_factory.v1`); one apply creates and populates; parent must be `UUserWidget` or a subclass (`CanCreateBlueprintOfClass` + reparenting rules) |
| Root widget: set / replace / clear | `WidgetTree.RootWidget` (`null` clears) | ✅ | ✅ | Supported | `umg.tree.add/move/remove` |
| Add a widget from the Palette (engine widget or another User Widget) | `WidgetTree…Children[]` | ✅ | ✅ | Supported | `umg.tree.add` (`WidgetTree::ConstructWidget` + `OnVariableAdded`, like the Palette drop); a User Widget that would contain itself is rejected (`Widget.CircularReference`) |
| Remove a widget (with its children) | omit it from the tree | ✅ | ✅ | Supported | `Raw::DeleteWidget` = `FWidgetBlueprintEditorUtils::DeleteWidgets(DeleteSilently)`: drops the widget's own bindings and desired focus, graph variable nodes and variable GUIDs (a descendant's binding/focus stays, as in the editor) |
| Reparent (drag to another panel) | move the node under another parent | ✅ (rebuild) | ✅ | Supported | `Raw::Place` / `Raw::Detach` (hierarchy drag & drop); the widget object, its graph references and animation tracks survive |
| Reorder siblings | order of `Children[]` | ✅ (rebuild) | ✅ | Supported | `umg.tree.move` with index |
| Single-content widgets (Button, Border, SizeBox…) | `Children` with at most one entry | ✅ | ✅ | Supported | child capacity checked (`Widget.InvalidPlacement`) |
| Named slots of a child User Widget | `…NamedSlots: {Slot: widget}` | ➖ | ✅ | Supported | `INamedSlotInterface::SetContentForSlot` |
| Inherited named slots (parent class exposes them) | `WidgetTree.NamedSlotBindings` | ✅ | ✅ | Supported | `UWidgetTree::SetContentForSlot`, slot names from `GetInheritedAvailableNamedSlots` |
| Is Variable | `…IsVariable` | ✅ | ✅ | Supported | `umg.widget.variable.set` (+ structural recompile, like the checkbox) |
| Change a widget's class (Replace With) | same `Name`, other `Class` | ➖ (rebuild loses references) | ✅ | Supported | remove with `keep_references` + add: bindings, graph references and the variable GUID of the name are kept |
| Rename a widget (case only) | same widget, `Name` differs only by case | ➖ (rebuild) | ✅ | Supported | `umg.widget.rename.v1` = `FWidgetBlueprintEditorUtils::RenameWidget` without the editor instance: same widget and variable GUID; desired focus, bindings, animation bindings and possessables, navigation targets and graph variable nodes / bound events follow. Extract shows the new casing, re-apply is a no-op |
| Rename a widget (another name) | widget `Name` = new name + `"RenamedFrom": "<old name>"` | ➖ (rebuild) | ✅ | Supported | `umg.widget.rename.v1` = `FWidgetBlueprintEditorUtils::RenameWidget` without the editor instance: `UWidgetBlueprint::OnVariableRenamed` (the variable GUID moves to the new name), desired focus, property bindings, animation bindings and possessables, navigation targets, the UI component extension, then `FBlueprintEditorUtils::ReplaceVariableReferences` (getters / setters / bound events of this Blueprint and the references in the loaded Blueprints that depend on it, `FindDependentBlueprints`). Those dependents are left unsaved as in the editor and reported (`Widget.DependentsUpdated`); a referencing dependent with unsaved changes refuses the rename (`Widget.DependentHasUnsavedChanges`), and a failed apply restores the dependents it changed in memory from snapshots taken with the Blueprint's (`Graph.DependentsRestored`, Blueprint rebuilder; nothing is reloaded from disk). `RenamedFrom` is ignored once the rename happened; without it a new name is delete + add. E2E `WidgetBlueprint.Rename` |
| Wrap With / Replace With panel (keep children) | edit the tree | ➖ | ✅ (as a tree edit) | Supported | expressed as the resulting tree (new parent + moved children) |
| Hierarchy view state: lock, hidden in designer, expanded, display label | — | ➖ | ➖ | Unsupported | editor view state on `UWidget` (`bLockedInDesigner`, `bHiddenInDesigner`, `bExpandedInDesigner`, `DisplayLabel`): not Details-editable, not part of the widget's behaviour |
| Variable GUIDs | (legacy `WidgetVariableGuids`) | ✅ | ➖ (editor-managed) | Unsupported | `UWidgetBlueprint::WidgetVariableNameToGuidMap` is maintained by `OnVariableAdded/Removed` exactly as in the editor; the legacy key is accepted and ignored |

## 2. Designer: Details panel of a widget

| Editor surface | Body location | Legacy | Python | Class | Mechanism / reason |
|---|---|---|---|---|---|
| Widget properties (Text, Brush, Font, Color, Visibility, RenderOpacity, RenderTransform, IsEnabled, ToolTipText, Clipping, Accessibility…) | `…Properties` (values differing from the class default; unlisted values reset) | ✅ (scalars / text / structs) | ✅ | Supported | `umg.widget.property.set` = `PreEditChange` / set / `PostEditChangeProperty`; typed JSON codec (structs, containers, object/class refs); `reset: true` restores the class default |
| Slot properties (Canvas anchors/offsets/alignment/ZOrder/AutoSize, Box padding/alignment/size, Grid row/column…) | `…Slot` | ✅ (partial) | ✅ | Supported | `umg.widget.property.set` with `target: "slot"`; after a reparent the new slot starts from its class defaults |
| Navigation (Up/Down/Left/Right/Next/Previous: Escape, Explicit + widget, Wrap, Stop, Custom/CustomBoundary + function) and routing policy | `…Navigation` | ➖ | ✅ | Supported | `umg.widget.navigation.set` (`UWidgetNavigation`, default navigation dropped like the customization) |
| Property binding: function (`Bind ▸ Create Binding` / pick a function) | `Bindings[] {Kind: "Function"}` | ✅ | ✅ | Supported | `umg.binding.set` (`FDelegateEditorBinding`, signature checked with `IsSignatureCompatibleWith`) |
| Property binding: property path (`Bind ▸ Health`) | `Bindings[] {Kind: "Property", SourcePath}` | ✅ | ✅ | Supported | `FEditorPropertyPath::Validate`; legacy `SourceProperty` / `PropertyPath` spellings accepted, `MemberGuid` ignored |
| Remove a binding | omit it from `Bindings` | ✅ | ✅ | Supported | `umg.binding.remove` |
| Widget events (`OnClicked [+]`) | `UbergraphPages` node `K2:ComponentBoundEvent:/Script/UMG.Button:OnClicked@OkButton` | ➖ | ✅ | Supported | K2 graph executor; the action database only registers bound-event spawners for component delegates, so the K2 adapter adds the same `UBlueprintBoundEventNodeSpawner` for each assignable delegate of a widget variable (what `FKismetEditorUtilities::CreateNewBoundEventForClass` creates) |
| Instanced sub-object properties (e.g. a ToolTip widget object, instanced UObject fields) | — | ➖ | ➖ | Unsupported | `CPF_InstancedReference` / `CPF_ContainsInstancedReference` properties are rejected (`Widget.PropertyNotEditable`); the editor edits them through per-class customizations that have no generic inverse yet. Navigation and slots, the common instanced ones, have dedicated keys |
| EditConst / DisableEditOnInstance / transient properties | — | ➖ | ➖ | Unsupported | not editable in the Details panel either |

## 3. Widget Blueprint itself (Details with the root selected, Designer toolbar, Class Settings / Class Defaults)

| Editor surface | Body location | Legacy | Python | Class | Mechanism / reason |
|---|---|---|---|---|---|
| Desired Focus (Details of the Blueprint) | `DesiredFocus` (`""` = none) | ➖ (rejected in ClassDefaults) | ✅ | Supported | `umg.desired_focus.set` = `FWidgetBlueprintEditorUtils::SetDesiredFocus`; legacy `ClassDefaults.DesiredFocusWidget` is moved here |
| Palette Category | `PaletteCategory` (`""` = "User Created") | ✅ (`Palette.Category`) | ✅ | Supported | `Raw::SetPaletteCategory` = `SWidgetDetailsView::HandleCategoryTextCommitted` (CDO + asset-registry copy); legacy `Palette` / `ClassDefaults.PaletteCategory` moved here |
| Design-time size (toolbar: Fill Screen / Custom / Custom on Screen / Desired / Desired on Screen, width × height) | `DesignSize {Mode, Width, Height}` | ➖ | ✅ | Supported | `Raw::SetDesignSize` = `SDesignerView::OnScreenFillRuleSelected` / `OnCustomResolution*Changed` (CDO fields, modified without recompile) |
| Preview background, other Class Defaults (`bIsFocusable`, `Priority`, `bStopAction`, `ColorAndOpacity`, `Padding`, tick / input settings…) | `ClassDefaults` | ✅ (no structs/containers) | ✅ | Supported | `blueprint.member.*` (same as UBlueprint); unlisted defaults restore the parent value |
| Class Settings (Description, Category, Namespace, `bCanCallInitializedWithoutPlayerContext`…) | `Properties` | ✅ (`EditorOptions`) | ✅ | Supported | `blueprint.member.*`; legacy `EditorOptions` merged into `Properties` |
| Reparent (Class Settings → Parent Class) | `ParentClass` | ✅ | ✅ | Supported | `blueprint.member.parent.set`, limited to `UUserWidget` subclasses by the asset's reparenting rules |
| Implemented interfaces | `ImplementedInterfaces` | ✅ | ✅ | Supported | shared with UBlueprint |
| Variables (My Blueprint) | `Variables` | ✅ | ✅ | Supported | shared with UBlueprint |
| Designer preview settings stored per editor (preview size of the viewport, zoom, DPI preview, "Show Outline") | — | ➖ | ➖ | Unsupported | editor-session / per-user settings, not asset data |
| Thumbnail settings (Class Settings → Thumbnail Size Mode / Custom Size) | `Properties` | ➖ | ✅ | Supported | `EditDefaultsOnly` properties of `UWidgetBlueprint`, edited with the other Class Settings through `blueprint.member.*` |

## 4. Animations (Animations tab + Sequencer)

| Editor surface | Body location | Legacy | Python | Class | Mechanism / reason |
|---|---|---|---|---|---|
| Add / delete an animation | `Animations[]` by `Name` | ✅ | ✅ | Supported | `Raw::CreateAnimation` / `Raw::DeleteAnimation` (Animations tab: `UWidgetAnimation` + `UMovieScene`, `OnVariableAdded/Removed`) |
| Display rate, playback range | `FrameRate`, `PlaybackRange` | ✅ | ✅ | Supported | `umg.anim.set` (`SetDisplayRate`, `SetPlaybackRange`, work range follows) |
| Property tracks on a widget: Float, Double, Bool, Byte, Enum, Integer, Color, 2D Transform (per channel), Margin (per channel) | `Tracks[] {Widget, Property, Type, Keys or Channels}` | Float + Transform only | ✅ | Supported | `umg.anim.track.set` (possessable + property track + one infinite section, `TMovieSceneChannelData::AddKey`, `AutoSetTangents`) |
| Tracks on a widget's slot (e.g. Canvas `LayoutData.Offsets`) | `Tracks[] {Target: "Slot"}` | ➖ | ✅ | Supported | slot possessable parented to the widget's, like Sequencer's "Slot" track |
| Keys: Linear / Constant / Cubic, tangent mode Auto / SmartAuto / User / Break with arrive / leave | `Keys[]` | Linear only | ✅ | Supported | channel key structs |
| Remove a track / a key | omit it | ✅ | ✅ | Supported | `umg.anim.track.remove` (track identity, also when its widget was deleted); unlisted keys and channels are cleared |
| Tracks with several sections, muted tracks, weighted tangents, non-constant pre/post-infinity, unknown channel types | extracted as `{…, "Unsupported": reason}` | ➖ (rejected) | Read-only | Read-only | the canonical shape is one section with plain keys; such tracks must be listed unchanged (`Widget.UnsupportedTrack`), so an apply never drops them |
| Tracks on the User Widget itself (root binding), dynamic bindings, unbound tracks, non-property tracks (event, audio, material parameter, sub-sequence) | extracted as `{…, "Unsupported": reason}` | ➖ | Read-only | Read-only | as above; no AI-friendly key shape yet |
| Sequencer folders, markers, section easing / blend type, key groups, evaluation options | — | ➖ | ➖ | Unsupported | Sequencer presentation / advanced evaluation state; preserved by updates (only listed headers and tracks are touched) |
| Rename an animation | — | ➖ | ➖ | Unsupported | identified by `Name`; rename = delete + add (graph references to the animation variable are not rewritten) |
| Animation events in the graph (Bind To Animation Started/Finished, Play Animation) | graph nodes | ✅ | ✅ | Supported | ordinary K2 call-function nodes |

## 5. Graph side (Graph mode)

| Editor surface | Body location | Legacy | Python | Class | Mechanism / reason |
|---|---|---|---|---|---|
| Event graph (any node placeable from the menu, custom events, widget bound events) | `UbergraphPages` | partial | ✅ | Supported | K2 graph executor (same as UBlueprint) |
| Functions (incl. binding functions: pure, return value) | `FunctionGraphs` | ✅ | ✅ | Supported | as UBlueprint |
| Macros | `MacroGraphs` | ✅ | ✅ | Supported | as UBlueprint |
| Event dispatchers (add / remove, Inputs, Category, Tooltip; Call / Bind / Unbind / Assign / dispatcher event nodes) | `EventDispatchers`, `UbergraphPages` nodes `K2:CallDelegate:OnPicked`… | ➖ | ✅ | Supported | as UBlueprint (`blueprint.member.dispatcher.*`: member variable + signature graph together, like "+ Event Dispatcher"); `WidgetBlueprint.EventDispatchers` |
| Timelines | `Timelines` (`[]` only) | ➖ | ➖ | Unsupported | the Blueprint editor offers Timelines only in actor-based Blueprints (`FBlueprintEditorUtils::DoesSupportTimelines`); non-empty → `Widget.UnsupportedRegion` |
| Components | `Components` (`[]` only) | ➖ | ➖ | Unsupported | Widget Blueprints have no SimpleConstructionScript; widgets go in `WidgetTree` |
| Compile | (every apply) | ✅ | ✅ | Supported | one compile per apply (`graph.finalize`), compiler errors fail the apply (`Graph.FinalizeFailed`) and roll back |

## 6. Semantics

| Rule | Legacy | Python |
|---|---|---|
| Omitted Body key (`WidgetTree`, `Bindings`, `Animations`, `DesiredFocus`, `PaletteCategory`, `DesignSize`, members, graphs) | `WidgetTree` / `Bindings` / `Animations` authoritative (cleared) | **kept** |
| Present key | complete desired state | complete desired state (`null` / `{}` / `[]` clear) |
| A listed widget | whole widget | whole widget: omitted `Properties` / `Slot` / `Navigation` / `Children` / `NamedSlots` mean none (class / slot defaults); omitted `IsVariable` keeps the flag |
| Unchanged re-apply | rebuilt the tree | true no-op: exact diff, no mutating primitive call, no compile, `bSavedAsset == false`, package bytes unchanged |
| Failure messages | free text | `<Code>: <Message> (at <Path>)`, codes `Widget.*` / `Graph.*` / `UBlueprint.*` |

## 7. Rollback and derived state

* Existing asset: Core package snapshot. The Blueprint derived-state rebuilder
  (`AssetDocumentGraphBlueprintRebuilder.cpp`) also supports `UWidgetBlueprint` (`AssetDocumentWidget`
  registers an `IAssetDocumentBlueprintRebuildAdapter` for it): the widget
  tree, widgets, slots, animations and graphs are authored objects restored from the snapshot;
  the generated class, CDO, the class-side widget-tree archetype and `*_INST` animations are
  derived (recompiled), CDO values (desired focus, palette category, design size, class
  defaults) are captured and written back. Restored objects keep their identity, so a human's
  earlier undo steps stay valid after a failed apply (`WidgetBlueprint.Undo`).
* Created asset: the factory lifecycle deletes it on failure (`WidgetBlueprint.Python` step 7).
* Undo history: `AssetDocumentUndoHistory` (integration) clears history that references the asset
  after a successful apply and keeps it after a rolled-back one.
* Open editor: edited in place. The Widget Blueprint editor rebuilds its Designer preview on every Blueprint change (`OnBlueprintChangedImpl`: destroy, refresh), so no designer copy can write an old state back; the Blueprint open-editor handler (`AssetDocumentGraphBlueprintOpenEditorHandler.cpp`, `RefreshInPlace`) closes removed graphs' tabs and runs `RefreshEditors`. Test `WidgetBlueprint.OpenEditorApply` (hierarchy, preview widget tree, Designer selection, rollback, human edit + Undo).

## 8. Counts

Supported 42 · Read-only 2 · Unsupported 9 (53 rows, §1–§5).

Notable unsupported rows: animation **rename** (name is identity: delete + add without
the editor's reference fix-ups), **instanced sub-object properties**, Sequencer **folders /
markers / easing**, **Timelines** and **Components** (not offered for Widget Blueprints). Read-only: animation tracks outside the one-section / plain-key shape and
non-property / root / dynamic tracks.
