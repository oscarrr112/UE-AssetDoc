// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "CoreMinimal.h"
#include "BehaviorTree/ValueOrBBKey.h"
#include "Misc/TVariant.h"
#include "Runtime/AssetDocumentPrimitiveProvider.h"
#include "Provider/BehaviorTree/AssetDocumentAIBehaviorTreeAuthoredReadbackInternal.h"
class UBehaviorTree;
namespace UE::AssetDocumentAI::BehaviorTreeValueOrKey
{
enum class EFamily : uint8 { Bool, Class, Enum, Float, Int32, Name, String, Object, Rotator, Struct, Vector };
using FExactLeafReference = TVariant<FValueOrBBKey_Bool*, FValueOrBBKey_Class*, FValueOrBBKey_Enum*,
 FValueOrBBKey_Float*, FValueOrBBKey_Int32*, FValueOrBBKey_Name*, FValueOrBBKey_String*,
 FValueOrBBKey_Object*, FValueOrBBKey_Rotator*, FValueOrBBKey_Struct*, FValueOrBBKey_Vector*>;
struct FLeaf
{
 FString Locator;
 FString Schema;
 EFamily Family = EFamily::Bool;
 FName Key;
 FAssetDocumentRuntimeValueDto Authored;
 FAssetDocumentRuntimeValueDto LocatorValue, SchemaValue;
};
struct FInventory
{
 UE::AssetDocumentAI::BehaviorTreeAuthoredReadback::EAuthoredGraphReadbackVersion ProjectionVersion = UE::AssetDocumentAI::BehaviorTreeAuthoredReadback::EAuthoredGraphReadbackVersion::V1;
 FString TreePath, PackagePath, GraphIdentity, BlackboardPath;
 FString AuthoredDigest, SemanticAuthoredDigest, TypedDigest, MappingDigest;
 // Invocation-local pure evidence. No UObject, handle or leaf address escapes.
 FAssetDocumentRuntimeValueDto AuthoredProjection, TypedProjection, MappingProjection;
 TArray<FLeaf> Leaves;
 TArray<int32> FamilyCounts = TArray<int32>({0,0,0,0,0,0,0,0,0,0,0});
};
const TCHAR* FamilyName(EFamily Family);
ASSETDOCUMENTAI_API bool HashAuthoredGraphValue(const FString& TreePath, const FAssetDocumentRuntimeValueDto& Value, FString& OutDigest,
 UE::AssetDocumentAI::BehaviorTreeAuthoredReadback::EAuthoredGraphReadbackVersion Version = UE::AssetDocumentAI::BehaviorTreeAuthoredReadback::EAuthoredGraphReadbackVersion::V1);
ASSETDOCUMENTAI_API bool Capture(UBehaviorTree& Tree, FInventory& Out, FString& Diagnostic,
 UE::AssetDocumentAI::BehaviorTreeAuthoredReadback::EAuthoredGraphReadbackVersion Version = UE::AssetDocumentAI::BehaviorTreeAuthoredReadback::EAuthoredGraphReadbackVersion::V1);
// Only the admitted/enlisted InstalledReplacement caller may invoke this stock
// initialization. Complete V2 evidence predicts and proves the exact Key change.
bool InitializeInstalledStockWaitParameters(UBehaviorTree& Tree, const FInventory& Before,
 FInventory& After, FString& Diagnostic);
// All handles and rows are local to this invocation. Nothing retains a leaf address.
// Full inventory and mapping are checked before the first immediate typed callback.
ASSETDOCUMENTAI_API bool VisitExactLeaves(UBehaviorTree& Tree, const FInventory& Expected,
 TFunctionRef<bool(FExactLeafReference&, const FLeaf&)> Visitor, FString& Diagnostic);
}
