# CurveFloat, CurveVector and CurveLinearColor Profile

Classes: /Script/Engine.CurveFloat, /Script/Engine.CurveVector, /Script/Engine.CurveLinearColor

Class `/Script/Engine.CurveFloat`. The curve lives in `Properties` (no `Body`):

- `Properties.FloatCurve`: the whole curve, authored as one value (omitted curve fields reset to their defaults): `{"Keys": [{"Time": 0.0, "Value": 1.0, "InterpMode": "Cubic", "TangentMode": "User", "TangentWeightMode": "None", "ArriveTangent": 0.0, "ArriveTangentWeight": 0.0, "LeaveTangent": 0.5, "LeaveTangentWeight": 0.0}], "PreInfinityExtrap": "Constant", "PostInfinityExtrap": "Cycle", "DefaultValue": 2.0}`. Only `Time` and `Value` are required per key; other key fields take the engine defaults (Linear, Auto, None, zero tangents). Enum values are the engine names without their `RCIM_` / `RCTM_` / `RCTWM_Weighted` / `RCCE_` prefix. `DefaultValue` is omitted while the curve has none. Numbers are float32 in the asset. Key times must be strictly ascending (`CurveFloat.KeysNotAscending`); a saved curve with keys at one time (a script's `AddKey`) reads with that warning and keeps them while they are unchanged.
- `Properties.bIsEventCurve`: boolean.

`/Script/Engine.CurveVector` and `/Script/Engine.CurveLinearColor` keep one such curve per channel in `Properties.FloatCurves`, an object keyed by channel (`X`, `Y`, `Z`; `R`, `G`, `B`, `A`), each in the `FloatCurve` form: `{"R": {"Keys": [...]}, "A": {"Keys": [...]}}`. Extracts list every channel (an empty one as `{"Keys": []}`). A channel you leave out keeps its curve; `{}` or `{"Keys": []}` clears it. Unknown channels: `CurveFloat.UnknownField`.

Other `UCurve` properties (the colour curve's `Adjust*` values) follow the generic rules.

Open editor: an apply works while the curve is open in the Curve Asset editor; it draws the applied keys, also after a failed apply.
