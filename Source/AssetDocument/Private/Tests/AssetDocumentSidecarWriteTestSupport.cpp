// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
#include "Tests/AssetDocumentSidecarWriteTestSupport.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "AssetDocumentEditorSync.h"
FAssetDocumentResult WithAssetDocumentSidecarWriteForTest(
 const FString& SidecarPath, TFunctionRef<FAssetDocumentResult()> Callback)
{
 FAssetDocumentEditorSync::FScopedSidecarWrite Guard(SidecarPath);
 return Callback();
}
#endif
