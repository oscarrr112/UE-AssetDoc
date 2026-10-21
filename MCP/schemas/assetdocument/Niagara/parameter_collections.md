# Niagara reference: Parameter collections

```jsonc
{"SchemaVersion": 1, "Target": "/Game/FX/NPC_Weather", "Class": "/Script/Niagara.NiagaraParameterCollection", "Action": "Create", "Definitions": {},
 "Properties": {"Namespace": "Weather", "SourceMaterialCollection": "/Game/Materials/MPC_Weather.MPC_Weather"},
 "Body": {"Parameters": [
   {"Name": "Wind", "Type": "Vector", "Value": {"X": 1, "Y": 0, "Z": 0}},
   {"Name": "Gust", "RenamedFrom": "Gusts", "Type": "float", "Value": 0.5},
   {"Name": "Strength", "Type": "float", "Value": 1.0, "MaterialCollection": true},
   {"Name": "Falloff", "Type": "/Script/Niagara.NiagaraDataInterfaceCurve", "Value": {"DataInterface": {"Properties": {}}}}]}}
```

- `Parameters` is complete: names without the `NPC.<Namespace>.` prefix (`Niagara.InvalidParameterName`); systems link them as `NPC.<Namespace>.<Name>` (`{"Link": "NPC.Weather.Wind"}`). A parameter not listed is deleted, a new `Name` added, `RenamedFrom` renames, another `Type` changes the type (the value goes back to the type's default unless `Value` is given). `Value` omitted keeps the value; values are those of user parameters (a data interface's `Properties` complete). Order is not authored.
- `Namespace` must be unused by other collections (`Niagara.NamespaceInUse`: the editor would rename it). The `SourceMaterialCollection`'s scalar and vector parameters join the list (`MaterialCollection: true`): they are never deleted, and changing them is refused (`Niagara.CollectionParameterOwned`).
- Apply works with the collection open in its editor: the editor stays open and its list shows the result.
