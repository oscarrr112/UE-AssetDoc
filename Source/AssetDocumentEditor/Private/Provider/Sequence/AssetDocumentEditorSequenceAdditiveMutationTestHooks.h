// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentEditorSequenceAdditiveTesting
{
struct FHooks { int32 FailAfterField = INDEX_NONE; int32 FieldWrites = 0; };
void SetHooks(FHooks* Hooks);
}
#endif
