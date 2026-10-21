// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
#pragma once
#include "Runtime/AssetDocumentPrimitiveProvider.h"
#include "Runtime/AssetDocumentMutationPreparation.h"

FAssetDocumentPrimitiveDescriptor MakeAnimationPreviewMeshSetDescriptor();
bool IsPreviewMeshSetAdmissionShape(const FAssetDocumentPrimitiveDescriptor& Descriptor);
FAssetDocumentPrimitiveOutcome InvokeAnimationPreviewMeshSet(
 const FAssetDocumentPrimitiveInvocation& Invocation,
 IAssetDocumentHandleResolver& Handles,
 IAssetDocumentAtomicTransaction* Transaction);

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentAnimationPreviewMeshTesting
{
struct FHooks
{
 int32 ForwardSetterCalls = 0;
 int32 InverseSetterCalls = 0;
 TFunction<void()> AfterSetter;
 bool bFailForwardReadback = false;
 bool bFailRollbackReadback = false;
};
void SetHooks(FHooks* Hooks);
}
#endif

FAssetDocumentContractValidationResult PrepareAnimationPreviewMeshMutationBatch(
 const FAssetDocumentRuntimeRequestDto& Request, TConstArrayView<FAssetDocumentMutationIntent> Intents,
 FAssetDocumentPreparedMutationBatch& OutBatch);

FAssetDocumentContractValidationResult ValidatePreparedAnimationPreviewMeshInvocation(
 const FAssetDocumentPrimitiveInvocation& Invocation, IAssetDocumentHandleResolver& Handles);

// Records only an impending public setter after successful footprint enlistment.
FAssetDocumentContractValidationResult MarkPreparedAnimationPreviewMeshWrite(
 const FAssetDocumentPrimitiveInvocation& Invocation, IAssetDocumentHandleResolver& Handles);
