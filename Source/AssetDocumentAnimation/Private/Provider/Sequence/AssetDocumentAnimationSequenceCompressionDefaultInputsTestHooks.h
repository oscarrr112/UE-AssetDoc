// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
#pragma once
#include "Provider/Sequence/AssetDocumentAnimationSequenceCompressionDefaultInputs.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentSequenceCompressionDefaultInputsTesting
{
// Observes real getter calls; it cannot substitute a default asset or warmup.
struct FHooks
{
 bool bForceNotReady = false;
 TArray<AssetDocumentSequenceCompressionDefaultInputs::EKind> Queries;
};
extern FHooks* ActiveHooks;
}
#endif
