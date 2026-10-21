// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
#pragma once
#include "Runtime/AssetDocumentPrimitiveProvider.h"
struct FAssetDocumentAIBehaviorTreeGraphLease;
namespace UE::AssetDocumentAI::BehaviorTreeDecoratorInput
{
FAssetDocumentPrimitiveOutcome EnsureInputCount(const FAssetDocumentAIBehaviorTreeGraphLease& Lease,
 const FGuid& Composite, const FGuid& BoundGraph, const FGuid& Logic,
 const FString& Kind, int32 Count, IAssetDocumentAtomicTransaction& Transaction);
#if WITH_DEV_AUTOMATION_TESTS
enum class EFailurePoint : uint8 { None, AfterNodeMigration, AfterPublication };
void SetFailurePointForTest(EFailurePoint Point);
#endif
}
