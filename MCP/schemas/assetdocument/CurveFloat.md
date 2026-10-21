# CurveFloat Profile

Classes: /Script/Engine.CurveFloat

Class `/Script/Engine.CurveFloat`. The curve lives in `Properties` (no `Body`):

- `Properties.FloatCurve`: the whole curve, authored as one value (omitted curve fields reset to their defaults): `{"Keys": [{"Time": 0.0, "Value": 1.0, "InterpMode": "Cubic", "TangentMode": "User", "TangentWeightMode": "None", "ArriveTangent": 0.0, "ArriveTangentWeight": 0.0, "LeaveTangent": 0.5, "LeaveTangentWeight": 0.0}], "PreInfinityExtrap": "Constant", "PostInfinityExtrap": "Cycle", "DefaultValue": 2.0}`. Only `Time` and `Value` are required per key; other key fields take the engine defaults (Linear, Auto, None, zero tangents). Enum values are the engine names without their `RCIM_` / `RCTM_` / `RCTWM_Weighted` / `RCCE_` prefix. `DefaultValue` is omitted while the curve has none. Numbers are float32 in the asset.
- `Properties.bIsEventCurve`: boolean.

Other `UCurve` properties follow the generic rules.

Open editor: an apply works while the curve is open in the Curve Asset editor; it draws the applied keys, also after a failed apply.
