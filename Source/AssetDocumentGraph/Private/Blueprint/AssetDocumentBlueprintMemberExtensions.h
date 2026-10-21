// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "AssetDocumentGraphPrimitives.h"

class UBlueprint;

// Member primitive groups that live in their own files and plug into the "blueprint"
// provider tables (AssetDocumentBlueprintMemberPrimitives.cpp) and its query.
namespace UE::AssetDocument::Graph
{
// The shared descriptor shape of every blueprint.member.* primitive.
FAssetDocumentPrimitiveDescriptor MakeBlueprintMemberDescriptor(const TCHAR* PrimitiveId, bool bMutating, const TCHAR* AuthoritativeApi);

// Fails the call with BlueprintMember.UnsupportedAsset unless the asset is a Blueprint.
UBlueprint* GetMemberBlueprint(FCall& Call);
// MarkBlueprintAsStructurallyModified + action database refresh (new members are spawnable at once).
void NotifyMemberStructural(UBlueprint& Blueprint);
// FKismetNameValidator check of a new member name (BlueprintMember.InvalidName).
bool ValidateNewMemberName(FCall& Call, UBlueprint& Blueprint, FName Name, FStringView Key);

// Event dispatchers: blueprint.member.dispatcher.set.v1 / .remove.v1 and the query's "dispatchers".
namespace BlueprintDispatchers
{
void AppendDescriptors(TArray<FAssetDocumentPrimitiveDescriptor>& Out);
void AppendBindings(TArray<FBinding>& Out);
FValue Query(UBlueprint& Blueprint);
}

// Case-only renames: blueprint.member.rename.v1.
namespace BlueprintRenames
{
void AppendDescriptors(TArray<FAssetDocumentPrimitiveDescriptor>& Out);
void AppendBindings(TArray<FBinding>& Out);
}

// Timelines: blueprint.member.timeline.set.v1 / .remove.v1 and the query's "timelines".
namespace BlueprintTimelines
{
void AppendDescriptors(TArray<FAssetDocumentPrimitiveDescriptor>& Out);
void AppendBindings(TArray<FBinding>& Out);
FValue Query(UBlueprint& Blueprint);
}
} // namespace UE::AssetDocument::Graph
