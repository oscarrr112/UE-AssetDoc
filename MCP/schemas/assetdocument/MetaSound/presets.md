# MetaSound reference: Presets

- Presets (the Content Browser's Create Preset): Create with `Body: {"Preset": {"Parent": "<MetaSound path>"}, "Inputs": {...overrides}}`; the parent is an existing MetaSound of the same class (else `MetaSound.InvalidPresetParent`). On a preset, an input listed in `Inputs` overrides the parent's default (its `Default`, else the value it has); an input left out inherits the parent's default again. `"Preset": null` is the editor's Convert From Preset: with `Graphs` / `Interfaces` / `Outputs` in the same Body they are applied to the converted MetaSound, otherwise the Body's overrides are applied first. Refused: `MetaSound.PresetGraphDerived` (`Graphs`, `Interfaces`, `Outputs`, or input settings other than `Default`, in a preset), `MetaSound.PresetParentChanged` (another parent; create a new preset), `MetaSound.CannotConvertToPreset` (`Preset` on a regular MetaSound), `MetaSound.UnknownMember` (an input the parent lacks).

```jsonc
// A preset of the source below with Pitch overridden; other inputs follow the parent.
{"SchemaVersion": 1, "Class": "/Script/MetasoundEngine.MetaSoundSource", "Target": "/Game/Audio/MSS_Blip_High", "Action": "Create",
 "Body": {"Preset": {"Parent": "/Game/Audio/MSS_Blip"}, "Metadata": {"Description": "Higher blip"},
  "Inputs": {"Pitch": {"Default": 880.0}}}}
```
