// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

class UClass;
class UDataAssetFactory;
class UFactory;

bool IsAssetDocumentEditorBehaviorTreeFactoryClassAdmitted(const UClass* AssetClass);

bool IsAssetDocumentEditorScratchClassAdmitted(const UClass* AssetClass);
bool IsAssetDocumentEditorFactoryClassAdmitted(const UClass* AssetClass);
bool IsAssetDocumentEditorDataAssetFactoryAdmitted(
	const UFactory* Factory,
	const UClass* AssetClass);
UDataAssetFactory* GetAssetDocumentEditorTransientScratchFactory(
	const UClass* AssetClass);
