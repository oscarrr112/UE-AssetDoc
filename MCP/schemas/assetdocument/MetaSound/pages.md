# MetaSound reference: Pages

Pages are the project's MetaSound page settings (Project Settings > Audio > MetaSound > Pages; in `Config/DefaultMetaSound.ini`: `[/Script/MetasoundEngine.MetaSoundSettings]` `+PageSettings=(UniqueId=<guid>,Name="Low")`). Inspect lists them as `ConfiguredPages`. A page the settings do not define reads with its id as name; it can be kept and edited, not added.

- `Graphs.<page>` adds the page as the Details panel's Add Page Graph does (a copy of the last page, then edited to the document); a page left out of `Graphs` is removed (its delete button), except `Default`, which every MetaSound keeps.
- Each page has its own `Nodes`, `Variables` and `Comments`; node `Id`s repeat across pages (a page starts as a copy). Members, interfaces and metadata are shared: an `Input` / `Output` node of a member is on every page.
- `Inputs.<name>.PageDefaults` `{"<page>": value}`: the input's value on that page (the page default widgets of the Members panel); a page left out uses the Default value.
- Update Node (`Version`) changes nodes of the Default page only; other pages are refused with `MetaSound.UpdateNodeNotSupported`.
- Errors: `MetaSound.UnknownPage` (a page, or a `PageDefaults` key, the project settings do not define; the message lists them), `MetaSound.InvalidBody` (`PageDefaults` with `Default`). The page that plays (`RegisteredPage`) is built into an operator after the apply.

```jsonc
// The project settings define a page "Low": a lighter graph and a lower pitch there.
{"SchemaVersion": 1, "Class": "/Script/MetasoundEngine.MetaSoundSource", "Target": "/Game/Audio/MSS_Hum", "Action": "Create",
 "Properties": {"OutputFormat": "Mono"},
 "Body": {
  "Inputs": {"Pitch": {"Type": "Float", "Default": 440.0, "PageDefaults": {"Low": 220.0}}},
  "Graphs": {
   "Default": {"Nodes": {
     "Play":  {"Class": "Input", "Member": "UE.Source.OnPlay"},
     "Pitch": {"Class": "Input", "Member": "Pitch"},
     "Osc":   {"Class": "UE.Sine.Audio", "Inputs": {"Frequency": "Pitch"}},
     "Out":   {"Class": "Output", "Member": "UE.OutputFormat.Mono.Audio:0", "Inputs": {"UE.OutputFormat.Mono.Audio:0": "Osc"}}}},
   "Low": {"Nodes": {
     "Play":   {"Class": "Input", "Member": "UE.Source.OnPlay"},
     "Pitch":  {"Class": "Input", "Member": "Pitch"},
     "Osc":    {"Class": "UE.Sine.Audio", "Inputs": {"Frequency": "Pitch"}},
     "Filter": {"Class": "UE.State Variable Filter.Audio", "Inputs": {"In": "Osc"}, "Defaults": {"Cutoff Frequency": 800.0}},
     "Out":    {"Class": "Output", "Member": "UE.OutputFormat.Mono.Audio:0", "Inputs": {"UE.OutputFormat.Mono.Audio:0": "Filter.Band Pass"}}}}}}}
```
