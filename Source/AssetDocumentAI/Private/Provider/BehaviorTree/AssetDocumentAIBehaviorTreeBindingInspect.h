// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
#pragma once

#include "Runtime/AssetDocumentHandleRegistry.h"
#include "Runtime/AssetDocumentPrimitiveProvider.h"

// AI-private, evidence-only leaf. The caller owns binding diagnostics and policy.
TOptional<FAssetDocumentPrimitiveOutcome> RejectAssetDocumentAIBehaviorTreeBindingInspectInvocation(
 const FAssetDocumentPrimitiveInvocation&, IAssetDocumentAtomicTransaction*);
FAssetDocumentPrimitiveOutcome HandleAssetDocumentAIBehaviorTreeBindingInspect(
 const FAssetDocumentPrimitiveInvocation&, IAssetDocumentHandleResolver&, IAssetDocumentAtomicTransaction*);
