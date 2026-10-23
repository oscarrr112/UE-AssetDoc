// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"

class UBTNode;

namespace UE::AssetDocumentAI::BehaviorTreeSemanticRuntimeMirrorRollback::Private
{
ASSETDOCUMENTAI_API void SetForceSemanticReadbackFailureForTest(bool bForceFailure);

#if WITH_DEV_AUTOMATION_TESTS
/** Appends extra blackboard evidence lines for node classes the stock evidence does not describe (empty: none). */
ASSETDOCUMENTAI_API void SetBlackboardEvidenceExtensionForTest(
	TFunction<void(FString& Lines, const FString& RolePath, const UBTNode& Node)> Extension);
#endif
}
