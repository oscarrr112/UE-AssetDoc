// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
#pragma once
#include "Runtime/AssetDocumentPrimitiveProvider.h"

FAssetDocumentPrimitiveDescriptor MakeAnimationSkeletonSetDescriptor();
bool IsSkeletonSetAdmissionShape(const FAssetDocumentPrimitiveDescriptor& Descriptor);
FAssetDocumentPrimitiveOutcome InvokeAnimationSkeletonSet(
 const FAssetDocumentPrimitiveInvocation& Invocation,
 IAssetDocumentHandleResolver& Handles,
 IAssetDocumentAtomicTransaction* Transaction);

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentAnimationSkeletonTesting
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
