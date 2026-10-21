# Profile Policy Metadata

Classes: (all profiles; metadata for tooling)

Profile and schema inspection expose policy metadata so tooling can understand the semantic ownership of structured `Body` regions without inventing an authoring language.

- `inspect_asset_document_profile` returns `BodySections` and, where the profile publishes them, `RegionPolicies`. For a generic reflected profile `RegionPolicies` is a valid empty array. For an exact profile, each entry describes a structured region such as `Body.Tree` / `Body.BlackboardAsset` (BehaviorTree) or `Body.ParentClass` (Blueprint).
- `get_asset_document_schema` is static documentation. The Unreal `GET /assetdoc/assetdocument/schema` route additionally returns top-level `RegionPolicyPresets` for built-in policy defaults such as `DefaultDiff`, `ManagedRegion`, and `ExtensionHook`, and `RegionPolicies` on entries in `registered_profiles`.

Policy entries are metadata and semantic declarations only. They are not patch/op instructions, not an operations list, and not a patch/op DSL. Author normal AssetDocument `Properties`, `Definitions`, and `Body` fields, then use validate and diff before apply.

Stable policy fields include:

- `RegionId`: Stable dotted region id, such as `Body.Tree`.
- `BodyPath`: Body-relative path owned by the policy.
- `RegionKind`: Region shape or ownership, such as `Object`, `Array`, `Timeline`, or `Managed`.
- `DefaultSource`: Source used for default comparison, such as `CDO`.
- `ReducerMode`: Summary of how diff/default reduction is interpreted, such as `DefaultDiff` or `ManagedRegion`.
- `ApplyMode`: Summary of how the region is applied, such as `SetProperty`, `RebuildArrayRegion`, `ManagedRegion`, or `ExtensionHook`.
- `ManagedUePropertyPaths`: UE property paths managed by the region, when applicable.
- `ExtensionHookName`: Named extension hook for hook-backed policies, when applicable.
