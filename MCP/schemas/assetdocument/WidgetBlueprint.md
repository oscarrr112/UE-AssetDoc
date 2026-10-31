# WidgetBlueprint Profile

Classes: /Script/UMGEditor.WidgetBlueprint, /Script/Blutility.EditorUtilityWidgetBlueprint

An Editor Utility Widget is edited as a Widget Blueprint; its document keeps its own `Class`; Create of one is refused (`Widget.SubclassCreateUnsupported`). A subclass's own saved properties are kept (`UBlueprint.Unrepresented` warning); one with graphs of its own is refused (`UBlueprint.SubclassGraphsUnsupported`). It has the UBlueprint members (`ParentClass`, which must derive from `UUserWidget`, `ImplementedInterfaces`, `Variables`, `ClassDefaults`, `EventDispatchers`) and graph regions with the same shapes and rules as above (Validate leaves a `K2:ComponentBoundEvent:...@<widget>` key to Apply), plus the Designer regions. Every region is omitted = keep, present = complete state:

- `WidgetTree`: `{"RootWidget": widget | null, "NamedSlotBindings": {slot: widget}}`. A widget is `{"Name", "Class", "IsVariable", "Properties", "Slot", "Navigation", "Children", "NamedSlots", "RenamedFrom"?}`; `Name` is its identity (no spaces or dots; unique case-insensitively). Widgets generated as variables are not listed in `Variables`. A widget whose `Class` changes is replaced under the same name and keeps its references. A widget is renamed with `"RenamedFrom": "<old name>"` on the widget under its new `Name` (the Designer's rename: references follow, also in other loaded Blueprints, left unsaved (`Widget.DependentsUpdated`); one with unsaved changes fails it (`Widget.DependentHasUnsavedChanges`)). `RenamedFrom` is ignored once done and must not name a listed widget (`Widget.InvalidRename`). Use the new name everywhere else (graph nodes: `K2:Get:<name>`). A name that differs only by case renames the widget without `RenamedFrom`; any other new name without it is remove + add. Texts: Generic `FText` forms. `Navigation`: `{"Up": {"Rule", "WidgetToFocus"?}, ..., "RoutingPolicy"?, "NavigationMethod"?: {"Struct", "Properties"} | null}` (omitted method: kept).
- `Bindings`: `[{"Widget", "Property", "Kind": "Function"|"Property", "Function"?, "SourcePath"?}]`. A `Function` binding names a function graph of this Blueprint whose signature is the property's delegate: `"Pure": true`, no inputs, one output `ReturnValue` of the property's exact type (ProgressBar `Percent` needs `float32`, not `float`; `Text` needs `text`); another signature fails with `Widget.InvalidBindingFunction` (names what differs). `DesiredFocus`: widget name (`""` clears). `PaletteCategory`: string. `DesignSize`: `{"Mode": "FillScreen"|"Custom"|"CustomOnScreen"|"Desired"|"DesiredOnScreen", "Width", "Height"}`; a new `Width` or `Height` is 1..100000. A widget property is bound once (`Widget.DuplicateBinding`); a `Property` binding needs a `SourcePath` (`Widget.InvalidBinding`) of members or pure no-parameter functions (`["Inventory", "GetCount"]`); an asset already breaking these reads with a warning and re-applies unchanged.
- `Animations`: `[{"Name", "FrameRate": {"Numerator", "Denominator"}, "PlaybackRange": {"StartFrame", "EndFrame"}, "Tracks": [{"Widget", "Property", "Type": "Float"|"Transform"|"Margin"|"Enum"|"Material"|..., "Target"?: "Slot"|"Root" (the user widget; no `Widget`), "Deactivated"?, "Section"?: {"StartFrame"?, "EndFrame"?, "Active"?, "WhenFinished"?, "PreRoll"?, "PostRoll"?}, "Keys" | "Channels": [{"Name", "Keys": [{"Frame", "Value", "Interp"?}]}]}]}]`; a missing bound is infinite, an omitted `Section` is kept and grows to cover the keys. A `Material` track keys a brush's material (`Property`: `Brush`) with `"Parameters": {"Tint": "Color"}` (Scalar, Bool, Vector2D, Vector, Color; channels `TintR`..., `Offset.X`...). Other tracks are `Unsupported` with read-only `Sections`; list them unchanged. Rename is remove + add (graph references to it break). Section easing, markers, evaluation and clock settings are kept, not shown.
- Event bindings are graph nodes (`K2:ComponentBoundEvent:/Script/UMG.Button:OnClicked@OkButton`). There are no `Components` or `Timelines`. View Model bindings and UI Components are kept, not shown (`UBlueprint.Unrepresented`).

Minimal document:

```json
{
  "SchemaVersion": 1,
  "Target": "/Game/UI/WBP_Menu",
  "Class": "/Script/UMGEditor.WidgetBlueprint",
  "Action": "CreateOrUpdate",
  "Body": {
    "ParentClass": "/Script/UMG.UserWidget",
    "WidgetTree": {"RootWidget": {"Name": "Root", "Class": "/Script/UMG.CanvasPanel", "Children": [
      {"Name": "Title", "Class": "/Script/UMG.TextBlock", "IsVariable": true, "Properties": {"Text": "Welcome"}}]}}
  }
}
```

Diff: per widget and property; a widget only in the document is an `Add`, only in the asset a `Remove`; a stated property at its default on the asset is a `Replace` from the default.

Apply works with the Widget Blueprint open in its editor.
