# MetaSound reference: Body shape of an extract

```jsonc
"Body": {
  "Interfaces": ["UE.Source.OneShot"],
  "Metadata": {"Description": "Short pitched blip", "Keywords": ["ui"], "Referenceable": true},
  "Inputs": {
    "Pitch": {"Type": "Float", "Default": 0.0, "SortOrder": 0,
              "Widget": {"Range": {"X": -12, "Y": 12}, "WidgetType": "Slider"}},
    "Wave":  {"Type": "WaveAsset", "Default": "/Game/Audio/SW_Blip.SW_Blip", "Constructor": true, "SortOrder": 1}
  },
  "Outputs": {},
  "Graphs": {
    "Default": {
      "Nodes": {
        "UE_Source_OnPlay": {"Class": "Input", "Member": "UE.Source.OnPlay", "Id": "…", "Position": [-500, 0]},
        "Pitch":  {"Class": "Input", "Member": "Pitch", "Id": "…", "Position": [-500, 200]},
        "Wave_Player": {"Class": "UE.Wave Player.Stereo", "Id": "…", "Position": [-200, 0],
                   "Inputs": {"Play": "UE_Source_OnPlay", "Pitch Shift": "Pitch"},
                   "Defaults": {"Loop": false}},
        "UE_OutputFormat_Stereo_Audio_0": {"Class": "Output", "Member": "UE.OutputFormat.Stereo.Audio:0", "Id": "…",
                   "Position": [200, 0], "Inputs": {"UE.OutputFormat.Stereo.Audio:0": "Wave_Player.Out Left"}}
      },
      "Variables": {"Count": {"Type": "Int32", "Default": 0}},
      "Comments": {"Comment": {"Id": "…", "Text": "Envelope", "Position": [-250, 260], "Size": [700, 260],
                               "Color": {"R": 1, "G": 1, "B": 1, "A": 1}, "FontSize": 18}}
    }
  }
}
```
