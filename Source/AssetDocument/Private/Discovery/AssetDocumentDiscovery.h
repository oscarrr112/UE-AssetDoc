// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

class UBlueprint;

// Read-only editor discovery for AI clients: what exists (Asset Registry, reflection) and what an
// asset offers to reference (skeleton slots/sockets/bones, material parameters, compile status).
// Nothing here loads a package that the Asset Registry does not list, or changes editor state.
namespace UE::AssetDocument::Discovery
{
// GET /assetdoc/assetdocument/find-assets. Query: path (package path prefix, recursive),
// class (asset class path or short name; subclasses included unless exact_class=true),
// name (substring, or wildcard with * / ?), uses (graph content, AssetDocumentUsageSearch.h: comma-separated terms
// that must all match, alternatives separated by |), limit (default 50, max 500), offset.
// Payload {Total, Offset, Assets[{ObjectPath, Class}], NextOffset?}; with uses the rows add the matching nodes and
// the payload Searched / NotSearched (AssetDocumentUsageSearchInternal.h). Redirectors are skipped.
bool FindAssets(const TMap<FString, FString>& Query, TSharedPtr<FJsonObject>& OutPayload, FString& OutError);

// GET /assetdoc/assetdocument/list-types. Query: kind (class|struct|enum, or a kind a provider module
// registered: AssetDocumentTypeListers.h, e.g. metasound_node), base (class or struct
// path / short name; subclasses of it), name, path (type path prefix, e.g. /Script/Engine or /Game),
// include_abstract (classes), schema (a schema class a provider hook knows, e.g. a StateTree schema:
// only the types that schema's node picker offers), limit, offset. Classes include unloaded Blueprint
// classes (as the class picker); struct base TableRowBase also lists User Defined Structs (as the
// DataTable row struct picker). Hidden classes, structs with meta Hidden (the node pickers' abstract
// bases) and types of test-only modules (*Test, *Tests, *TestSuite) are not listed.
// Payload {Total, Offset, Types[{Path, Abstract?, Blueprint?, UserDefined?}], NextOffset?}.
bool ListTypes(const TMap<FString, FString>& Query, TSharedPtr<FJsonObject>& OutPayload, FString& OutError);

// Schema of a native struct or enum, by /Script/... path: struct
// {Kind: "Struct", Path, Super?, RowStruct, Fields[{Name, Type, EnumValues?}]} (provider hooks may
// refine it, e.g. StateTree nodes: authored Fields, InstanceType, InstanceFields[{..., Usage}], Outputs)
// or enum {Kind: "Enum", Path, Values[]}. Null when Path names neither.
TSharedPtr<FJsonObject> DescribeType(const FString& Path);

// Adds the editor facts of a loaded asset to an Inspect payload (existing keys are kept):
// Skeleton {SlotGroups, Sockets, Bones, VirtualBones}, SkeletalMesh {Skeleton, Sockets, Bounds, MaterialSlots},
// StaticMesh {Bounds, Sockets, MaterialSlots, LODs}, MeshComponents (Actor Blueprint: DescribeMeshComponents),
// Animation {Skeleton, PlayLength, Frames?, FrameRate?, SlotNames?}, MaterialParameters[...]
// (unless the profile already reported a ParameterSchema), Compile (Blueprint family).
void AddAssetFacts(const UObject& Asset, FJsonObject& InOutPayload);

// Blueprint family: the editor's compile status as the Blueprint editor shows it, without compiling:
// {Status, Messages[{Severity, Message, Graph, Node, NodeGuid}]} from UBlueprint::Status and the
// compiler annotations on the graph nodes. Materials: {Status: UpToDate|Error|Compiling,
// Messages[{Severity, Message}], Nodes[expression names]} from the material resource of the editor's
// shader platform. Other assets: the first registered describer that handles them
// (AssetDocumentCompileStatus.h, e.g. StateTree); null otherwise.
TSharedPtr<FJsonObject> DescribeCompileStatus(const UObject& Asset);

// The mesh components of an Actor Blueprint as its class defaults hold them: native ones (C++ default
// subobjects), inherited ones (a parent Blueprint's, with this Blueprint's overrides) and its own, each
// [{Name, Scope: Native|Inherited|Owned, Class, Mesh, MaterialSlots[{Index, SlotName, Material}],
// OverrideMaterials?}]. Name and Scope are the ones the Blueprint document's Components use; MaterialSlots are
// the mesh's slots; OverrideMaterials (only when set) replace them by index. Empty for other Blueprints.
TArray<TSharedPtr<FJsonValue>> DescribeMeshComponents(const UBlueprint& Blueprint);
}
