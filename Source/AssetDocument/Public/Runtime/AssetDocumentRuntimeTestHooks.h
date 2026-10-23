// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "Runtime/AssetDocumentRuntimeTypes.h"

// Fault injection for tests that drive the production runtime (a Service apply) and need it to
// fail at one point of a mutation. Each call arms exactly one failure, consumed by the next
// request that reaches that point; Reset() disarms both. Game thread only.
struct ASSETDOCUMENT_API FAssetDocumentRuntimeTestHooks
{
	// The next mutation request that enters Phase (Repair: after Apply mutated the target;
	// Persist: after Repair; Verify: after the durable install) fails there with
	// RequestCoordinator.TestInjectedFailure.
	static void FailNextRequestAtPhase(EAssetDocumentRuntimeRequestPhase Phase);
	// The next persistence that records Event (StageSidecar, PrepareJournal, InstallPackageHeader,
	// InstallPackageCompanions, InstallSidecar, AdvanceSync, MarkDurableComplete) fails the file
	// write that follows it; Event Cleanup fails its journal cleanup.
	static void FailNextPersistenceAtEvent(const FString& Event);
	static void Reset();

	// Called by the runtime.
	static bool ConsumeRequestPhaseFailure(EAssetDocumentRuntimeRequestPhase Phase);
	static void ObservePersistenceEvent(FStringView Event);
};
