# BlackboardData Profile

Classes: /Script/AIModule.BlackboardData

Class `/Script/AIModule.BlackboardData` (Python runtime). Allowed top-level fields: `SchemaVersion`, `Target`, `Class`, `Action`, `Definitions`, `Properties`, `Body` (anything else, including `AssetType`, fails with `BlackboardData.UnknownDocumentField`). Keep `Definitions` and `Properties` as `{}`.

`Body` must contain exactly `Parent` and `Keys` (`BlackboardData.InvalidBodyShape`):

- `Parent`: `null` (no parent; clears an existing parent) or `{"Kind": "AssetRef", "Path": "/Game/AI/BB_Base.BB_Base"}` with exactly those two fields and an object path. A Blackboard cannot be its own parent (`BlackboardData.SelfParent`); parent cycles fail with `BlackboardData.ParentCycle`.
- `Keys`: authoritative ordered array of the Blackboard's own (local) keys. Keys missing from the array are removed, new keys are added at their index, existing keys are updated in place (or have their key type replaced), and the final order matches the array. `[]` removes all local keys. Inherited keys come from `Parent` and must not be repeated locally (`BlackboardData.InheritedShadow`). The engine-created `SelfActor` key is omitted from extract; do not author it.

Key entry fields:

| Field | Required | Value |
|---|---|---|
| `Name` | yes | non-empty string, unique case-insensitively (`BlackboardData.DuplicateLocalName`). Renaming is remove + add; changing only letter case of an existing key fails (`BlackboardData.InvalidLocalName`). |
| `Category` | yes | string (may be `""`) |
| `Description` | yes | string (may be `""`) |
| `bInstanceSynced` | yes | boolean |
| `KeyTypeClass` | yes | `{"Kind": "ClassRef", "Path": "/Script/AIModule.BlackboardKeyType_Float"}` (exactly `Kind` and `Path`) |
| `KeyTypeProperties` | depends on key type | object of the key type's editable properties; defaults to `{}` |

Legacy aliases are accepted on input and canonicalized: `Type` (`"Bool"`, `"Class"`, `"Enum"`, `"Float"`, `"Int"`, `"Name"`, `"Object"`, `"Rotator"`, `"String"`, `"Struct"`, `"Vector"`, or a full class path) instead of `KeyTypeClass`; `BaseClass` and `Enum` instead of the matching `KeyTypeProperties` entries. Supplying both an alias and its canonical field fails with `BlackboardData.AliasConflict`. Prefer the canonical fields.

`KeyTypeProperties` by key type:

- `BlackboardKeyType_Object`: exactly `{"BaseClass": ClassRef, "DefaultValue": null | AssetRef}`.
- `BlackboardKeyType_Class`: exactly `{"BaseClass": ClassRef, "DefaultValue": null | ClassRef}`.
- `BlackboardKeyType_Enum`: exactly `{"EnumType": AssetRef, "EnumName": string, "DefaultValue": "<enumerator name>"}`. `EnumName` must be `""` for a `/Game/...` user enum and must equal `EnumType.Path` for a native `/Script/...` enum.
- `Bool`: exactly `{"bDefaultValue": bool}`; `Int`, `Float`, `Name`, `String`: exactly `{"DefaultValue": value}`. A missing or extra property fails validation and apply with `AIProvider.Blackboard.EntryMaterializationUnavailable`.
- Other key types: the key type's editable properties as Unreal reads them back. Extract an existing Blackboard to see the exact names (Struct, ...).
- `Vector` / `Rotator`: `{"DefaultValue": null}` (no default; `{}` means this) or `{"DefaultValue": {"X": 0, "Y": 0, "Z": 0}}` / `{"Pitch", "Roll", "Yaw"}` (default on; omitted components are 0). `bUseDefaultValue: false` is accepted and means `null`. Changing such a key replaces its key type (BehaviorTree selectors keep the key by name).

Common codes: `BlackboardData.MissingEntryField`, `BlackboardData.UnknownEntryField`, `BlackboardData.InvalidKeyTypeClass`, `BlackboardData.InvalidEntryField` (wrong or incomplete `KeyTypeProperties`), `BlackboardData.DuplicateLocalName`, `BlackboardData.InheritedShadow`, `BlackboardData.InvalidParent`, `BlackboardData.ParentCycle`, `BlackboardData.ClassMismatch`.

Example:

```json
{
  "SchemaVersion": 1,
  "Target": "/Game/AI/BB_Enemy",
  "Class": "/Script/AIModule.BlackboardData",
  "Action": "CreateOrUpdate",
  "Definitions": {},
  "Properties": {},
  "Body": {
    "Parent": { "Kind": "AssetRef", "Path": "/Game/AI/BB_Base.BB_Base" },
    "Keys": [
      {
        "Name": "TargetActor",
        "Category": "Combat",
        "Description": "Current target",
        "bInstanceSynced": false,
        "KeyTypeClass": { "Kind": "ClassRef", "Path": "/Script/AIModule.BlackboardKeyType_Object" },
        "KeyTypeProperties": {
          "BaseClass": { "Kind": "ClassRef", "Path": "/Script/Engine.Actor" },
          "DefaultValue": null
        }
      },
      {
        "Name": "bCanSeeTarget",
        "Category": "",
        "Description": "",
        "bInstanceSynced": false,
        "KeyTypeClass": { "Kind": "ClassRef", "Path": "/Script/AIModule.BlackboardKeyType_Bool" },
        "KeyTypeProperties": { "bDefaultValue": false }
      },
      {
        "Name": "AttackRange",
        "Category": "",
        "Description": "",
        "bInstanceSynced": false,
        "KeyTypeClass": { "Kind": "ClassRef", "Path": "/Script/AIModule.BlackboardKeyType_Float" },
        "KeyTypeProperties": { "DefaultValue": 150.0 }
      }
    ]
  }
}
```

Open editor: an apply works while the Blackboard is shown in a Behavior Tree editor (opened on the Blackboard or on a Behavior Tree that uses it); its Blackboard panels list the applied keys, also after a failed apply.
