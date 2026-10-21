# Niagara reference: Update example

Example: rename an emitter, add a light renderer, a user parameter linked to a new module input, a Set Parameters module, and move a module.

```jsonc
{"SchemaVersion": 1, "Target": "/Game/FX/NS_Burst", "Class": "/Script/Niagara.NiagaraSystem", "Action": "Update", "Definitions": {},
 "Properties": {},
 "Body": {
   "UserParameters": [{"Name": "User.Turbulence", "Type": "float", "Value": 40.0, "Category": "Look"}],
   "Emitters": [
     {"Name": "Burst", "RenamedFrom": "DirectionalBurst",
      "Renderers": [{"Name": "Glow", "Class": "LightRenderer", "Properties": {"RadiusScale": 2.0}},
                    {"Name": "NiagaraSpriteRendererProperties_0", "Class": "SpriteRenderer"}],   // Properties omitted: kept
      "ParticleUpdate": [
        {"Name": "ParticleState", "Script": "/Niagara/Modules/Update/Lifetime/ParticleState.ParticleState"},
        {"Name": "CurlNoiseForce", "Script": "/Niagara/Modules/Update/Forces/V2/CurlNoiseForce.CurlNoiseForce",
         "Inputs": {"Noise Strength": {"Link": "User.Turbulence"}}},
        {"Name": "SolveForcesAndVelocity", "Script": "/Niagara/Modules/Solvers/SolveForcesAndVelocity.SolveForcesAndVelocity"},
        {"Name": "Tint", "Assign": [{"Target": "Particles.Color", "Type": "LinearColor"}],
         "Inputs": {"Particles.Color": {"R": 1, "G": 0.5, "B": 0.25, "A": 1}}}]},
     {"Name": "Fountain", "From": "/Niagara/DefaultAssets/Templates/Emitters/Fountain"}]}}
```
