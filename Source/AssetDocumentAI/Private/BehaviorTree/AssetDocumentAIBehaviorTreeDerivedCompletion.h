// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
#pragma once
#include "Provider/BehaviorTree/AssetDocumentAIBehaviorTreeAuthoredReadbackInternal.h"
#include "BehaviorTree/AssetDocumentAIBehaviorTreeSemanticRuntimeMirrorRollback.h"
class UBehaviorTree;
enum class EAssetDocumentAIBehaviorTreeCompletionMode : uint8 { MirrorOnly, DecoratorAndMirror, ValueOrDecoratorMirror };
enum class EAssetDocumentAIBehaviorTreeCompletionLifecycle : uint8 { CurrentGraph, InstalledReplacement };
struct FAssetDocumentAIBehaviorTreeDerivedCompletionResult
{
 bool bSucceeded = false;
 int32 ValueOrLeafCount = 0;
 TMap<FString, int32> ValueOrFamilyCounts;
 FString AuthoredInventoryDigest;
 FString ResolvedMappingDigest;
 int32 DecoratorCount = 0;
 int32 BoundDecoratorCount = 0;
 FString AuthoredDecoratorDigest;
 FString DerivedSemanticDigest;
 FAssetDocumentAIBehaviorTreeSemanticRuntimeMirrorResult Mirror;
 TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
};
ASSETDOCUMENTAI_API FAssetDocumentAIBehaviorTreeDerivedCompletionResult CaptureAssetDocumentBehaviorTreeDecoratorEvidence(
 UBehaviorTree& Tree, UBehaviorTreeGraph& Graph);
FAssetDocumentAIBehaviorTreeDerivedCompletionResult RunAssetDocumentBehaviorTreeDerivedCompletion(
 UBehaviorTree& Tree, UBehaviorTreeGraph& Graph, EAssetDocumentAIBehaviorTreeCompletionMode Mode,
 const FAssetDocumentMutationFootprint& Footprint, IAssetDocumentAtomicTransaction& Transaction,
 const FString& ExpectedBlackboardPath = {}, const FString& ExpectedAuthoredDecoratorDigest = {},
 EAssetDocumentAIBehaviorTreeCompletionLifecycle Lifecycle = EAssetDocumentAIBehaviorTreeCompletionLifecycle::CurrentGraph,
 UE::AssetDocumentAI::BehaviorTreeAuthoredReadback::EAuthoredGraphReadbackVersion ProjectionVersion = UE::AssetDocumentAI::BehaviorTreeAuthoredReadback::EAuthoredGraphReadbackVersion::V1,
 bool AllowNullBlackboard = false);
FAssetDocumentContractValidationResult CaptureAndRegisterInstalledReplacementCompletion(
 UBehaviorTree& Tree, IAssetDocumentAtomicTransaction& Transaction);
FAssetDocumentContractValidationResult RecompleteGraphInstallInverse(UBehaviorTree& Tree);
namespace UE::AssetDocumentAI::BehaviorTreeDerivedCompletion::Private
{
// Mirror mechanics never enlist; the single combined owner controls registration.
TFunction<FAssetDocumentContractValidationResult()> CaptureMirrorRollback(UBehaviorTree& Tree, UBehaviorTreeGraph& Graph,
 bool InitializeFromAsset = true, bool AllowEmptyAuthoredGraph = false);
FAssetDocumentContractValidationResult CompleteMirror(UBehaviorTree& Tree, UBehaviorTreeGraph& Graph,
 FAssetDocumentAIBehaviorTreeSemanticRuntimeMirrorResult& Out, bool InitializeFromAsset = true,
 bool AllowEmptyAuthoredGraph = false);
FAssetDocumentContractValidationResult ValidateMirror(UBehaviorTree& Tree, UBehaviorTreeGraph& Graph,
 bool AllowEmptyAuthoredGraph = false);
bool IsExactEmptyAuthoredGraph(const UBehaviorTreeGraph& Graph);
ASSETDOCUMENTAI_API FString HashEvidence(const FString& Value);
#if WITH_DEV_AUTOMATION_TESTS
enum class EValueOrCompletionFailure : uint8 { None, BeforeMirror, BeforeSecondObservation };
ASSETDOCUMENTAI_API void SetValueOrCompletionFailureForTest(EValueOrCompletionFailure Point);
ASSETDOCUMENTAI_API void SetBeforeValueOrSecondObservationForTest(TFunction<void()> Hook);
ASSETDOCUMENTAI_API void ResetNotificationTraceForTest();
ASSETDOCUMENTAI_API void SetFailAfterFirstNotificationForTest(bool Fail);
ASSETDOCUMENTAI_API TArray<FString> GetNotificationTraceForTest();
#endif
}
