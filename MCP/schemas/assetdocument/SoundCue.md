# SoundCue Profile

Classes: /Script/Engine.SoundCue

A Sound Cue's graph as the Sound Cue editor shows it: the Output node (`Root`), every sound node (`Nodes`) and comment box (`Comments`), plus the cue's Details (`Properties`). Apply edits the graph with the editor's own operations (palette placement, wire drag, Break Link, Add / Delete Input, Delete, Details panel) and then validates the cue. Create uses the editor's New Sound Cue factory (an empty cue with its Output node). Extract and Inspect results carry `Compile: {Status, Messages}` (`UpToDate`, `UpToDateWithWarnings`, `Error`). No audio device is needed. Apply works with the cue open in the Sound Cue editor: its graph shows the applied (or, after a failure, rolled-back) cue.

```jsonc
"Properties": {"VolumeMultiplier": 0.8, "SoundClassObject": "/Game/Audio/SCL_SFX.SCL_SFX"},   // cue Details (sparse)
"Body": {
  "Root": {"Input": "Mod", "Position": [200, 0]},           // the Output node's link: a node key or null
  "Nodes": {
    "Mod":   {"Class": "Modulator", "Position": [-50, 0], "Properties": {"PitchMin": 0.9, "PitchMax": 1.1},
              "Inputs": ["Rand"]},
    "Rand":  {"Class": "Random", "Position": [-300, 0],
              "Properties": {"Weights": [1.0, 1.0, 0.5]}, "Inputs": ["Step1", "Step2", "Scuff"]},
    "Step1": {"Class": "WavePlayer", "Properties": {"SoundWaveAssetPtr": "/Game/Audio/SW_Step1.SW_Step1"}},
    "Step2": {"Class": "WavePlayer", "Properties": {"SoundWaveAssetPtr": "/Game/Audio/SW_Step2.SW_Step2"}},
    "Scuff": {"Class": "WavePlayer", "Properties": {"SoundWaveAssetPtr": "/Game/Audio/SW_Scuff.SW_Scuff"},
              "Comment": "rarer", "CommentVisible": true}
  },
  "Comments": {"Variations": {"Text": "Variations", "Position": [-650, -220], "Size": [400, 520]}}
}
```

- Keys are object names (letters, digits, `_`, `-`): Extract writes the editor's names (`SoundNodeRandom_2`); a new key creates a node named after it. Keys survive save, reload and duplication. `Root`, `Nodes` and `Comments` omitted = keep; present = complete (an unlisted node or comment is deleted, as the editor's Delete does).
- `Class`: engine nodes by short name without `SoundNode` (`WavePlayer`, `Random`, `Mixer`, `Modulator`, `ModulatorContinuous`, `Attenuation`, `Looping`, `Delay`, `Switch`, `Branch`, `Concatenator`, `Enveloper`, `Oscillator`, `Doppler`, `SoundClass`, `WaveParam`, `Mature`, `GroupControl`, `DistanceCrossFade`, `ParamCrossFade`, `QualityLevel`, `DialoguePlayer`), other classes by class path. `list_types` kind `sound_node` lists them with their inputs and defaults. A different `Class` on a key replaces the node.
- `Inputs` is positional: entry *i* is input pin *i* (a node key or null); its length is the number of input pins (Add / Delete Input). Omitted: the class's starting pins, nothing connected. Limits are the node's own (at most 32; `Branch` exactly 3; `QualityLevel` one per project quality level; players none): `SoundCue.ChildCount`. A node may feed several parents; a loop is refused (`SoundCue.ConnectionCycle`).
- `Properties` of a node are its Details values, complete: an omitted property has the value of a new node of that class (Extract lists only differences). Per-child arrays have one entry per input (`Weights`, Mixer / Concatenator `InputVolume`, crossfade `CrossFadeInput`; `GroupSizes` one less): `SoundCue.ChildArrayLength`. Reordering inputs reorders these too.
- `Position` omitted keeps the node (a new node goes left of its first parent). `Comment` / `CommentVisible` omitted: none / hidden. Comment boxes: `Text`, `Position`, `Size`, and optional `Color`, `FontSize`, `BubbleVisible`, `ColorBubble`, `MoveMode`, `Details` (omitted: a new comment's values).
- Top-level `Properties` never hold `FirstNode`, `AllNodes`, `SoundCueGraph` or the derived `Duration`, `MaxDistance`, `TotalSamples`, `bHasDelayNode`, `bHasConcatenatorNode`, `bHasPlayWhenSilent`, `CookedQualityIndex` (`SoundCue.NotAuthoredProperty`).
- Validation after each apply: errors the cue did not have before fail the apply at the node's path and roll it back (`SoundCue.ValidationFailed`: a Delay without its input, a wave that is not a Sound Wave, input / array counts the runtime would crash on); errors it already had and warnings (an empty input slot on a node that needs one, a player without a wave, nothing connected to the Output) come back as `SoundCue.ValidationError` / `SoundCue.ValidationWarning`. Nodes not reachable from `Root` are kept (info).
- Inspect adds `NodeFacts` (per key: `Class`, `InputPins` names in slot order, `MinInputs`, `MaxInputs`, `Reachable`), `Waves`, `Status`, `Derived` (`Duration`, `MaxDistance`, ...) and the project's `QualityLevels`.
