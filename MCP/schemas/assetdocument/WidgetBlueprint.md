# WidgetBlueprint Profile

Classes: /Script/UMGEditor.WidgetBlueprint

Class `/Script/UMGEditor.WidgetBlueprint`. It has the UBlueprint members (`ParentClass`, which must derive from `UUserWidget`, `ImplementedInterfaces`, `Variables`, `ClassDefaults`, `EventDispatchers`) and graph regions with the same shapes and rules as above (`"Layout": "Auto"` and Validate's graph check included; Validate leaves a `K2:ComponentBoundEvent:...@<widget>` key to Apply, as the widget may be one the same document adds), plus the Designer regions. Every region is omitted = keep, present = complete state:

- `WidgetTree`: `{"RootWidget": widget | null, "NamedSlotBindings": {slot: widget}}`. A widget is `{"Name", "Class", "IsVariable", "Properties", "Slot", "Navigation", "Children", "NamedSlots", "RenamedFrom"?}`; `Name` is its identity (no spaces or dots; unique case-insensitively), `VariableName` if written must equal `Name`. Widgets generated as variables are not listed in `Variables`. A widget whose `Class` changes is replaced under the same name and keeps its references. A widget is renamed with `"RenamedFrom": "<old name>"` on the widget under its new `Name` (the Designer's rename: same widget and variable GUID; bindings, animation bindings, desired focus, navigation targets and graph references follow, also in other loaded Blueprints that use this one, which are then unsaved and reported as `Widget.DependentsUpdated`; if one of them already has unsaved changes the rename fails with `Widget.DependentHasUnsavedChanges`: save it first). `RenamedFrom` is ignored once the rename happened; it must not name a widget that is still listed (`Widget.InvalidRename`). Use the new name everywhere else in the document (`Bindings`, `DesiredFocus`, `Animations`, graph nodes such as `K2:Get:<name>` and `K2:ComponentBoundEvent:...@<name>`). A name that differs only by case renames the widget without `RenamedFrom`; any other new name without it is remove + add.
- `Bindings`: `[{"Widget", "Property", "Kind": "Function"|"Property", "Function"?, "SourcePath"?}]`. A `Function` binding names a function graph of this Blueprint whose signature is the property's delegate: `"Pure": true`, no inputs, one output `ReturnValue` of the property's exact type (a float property such as ProgressBar `Percent` needs `float32`, not `float`, which is double; `Text` needs `text`); another signature fails validate and apply with `Widget.InvalidBindingFunction`, which names what differs and the fix. `DesiredFocus`: widget name (`""` clears). `PaletteCategory`: string. `DesignSize`: `{"Mode": "FillScreen"|"Custom"|"CustomOnScreen"|"Desired"|"DesiredOnScreen", "Width", "Height"}`.
- `Animations`: `[{"Name", "FrameRate": {"Numerator", "Denominator"}, "PlaybackRange": {"StartFrame", "EndFrame"}, "Tracks": [{"Widget", "Property", "Type": "Float"|"Transform"|"Margin"|"Enum"|..., "Target"?: "Slot", "Keys" | "Channels": [{"Name", "Keys": [{"Frame", "Value", "Interp"?}]}]}]}]`. Tracks outside the supported one-section, plain-key shape are read-only. Animation rename is remove + add.
- Event bindings are graph nodes (`K2:ComponentBoundEvent:/Script/UMG.Button:OnClicked@OkButton`). `Components` and `Timelines` are not offered for Widget Blueprints.

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

Diff: the widget tree diffs per widget and property, e.g. `/Body/WidgetTree/RootWidget/Children/0/Properties/Text`; a widget only in the document is an `Add`, one only in the asset a `Remove`. A property the document sets that the asset has at its default (extract omits it) is a `Replace` from the default value.

Open editor: an apply works while the Widget Blueprint is open. The Designer (hierarchy, preview, selection) and the graph show the result, also after a failed apply.
