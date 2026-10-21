// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
#pragma once
#include "Runtime/AssetDocumentPrimitiveProvider.h"
class UBehaviorTree;
struct FAssetDocumentAIBehaviorTreeScratchBundleState;
namespace UE::AssetDocumentAI::BehaviorTreeGraphInstall
{
FAssetDocumentPrimitiveOutcome InstallExactGraph(UBehaviorTree& Tree,
 TSharedRef<FAssetDocumentAIBehaviorTreeScratchBundleState> Scratch,
 IAssetDocumentHandleResolver& Handles, IAssetDocumentAtomicTransaction& Transaction);
#if WITH_DEV_AUTOMATION_TESTS
enum class EFailurePoint : uint8 { None, AfterOldGraphIsolation, AfterOldRootsIsolation,
 AfterIncomingGraphTransfer, AfterIncomingRootsTransfer, AfterFlags, AfterGraphAssignment,
 BeforeHandleIssue, InverseTransfer };
ASSETDOCUMENTAI_API void SetFailurePointForTest(EFailurePoint Point);
#endif
}
FAssetDocumentContractValidationResult ValidateInstalledGraphOwnership(UBehaviorTree& Tree);
