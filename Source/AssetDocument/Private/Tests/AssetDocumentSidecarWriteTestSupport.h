// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "CoreMinimal.h"
#include "AssetDocumentTypes.h"
#if WITH_DEV_AUTOMATION_TESTS
// Test-only composition of the exact Service sidecar-write scope.
ASSETDOCUMENT_API FAssetDocumentResult WithAssetDocumentSidecarWriteForTest(
 const FString& SidecarPath, TFunctionRef<FAssetDocumentResult()> Callback);
#endif
