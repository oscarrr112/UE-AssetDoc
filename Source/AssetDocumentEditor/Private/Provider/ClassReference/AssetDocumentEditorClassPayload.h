// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "Runtime/AssetDocumentHandleRegistry.h"

class UClass;

ASSETDOCUMENTEDITOR_API TUniquePtr<IAssetDocumentNativeHandlePayload>
MakeAssetDocumentEditorClassPayload(UClass& Class);
