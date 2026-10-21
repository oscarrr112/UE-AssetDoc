// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Templates/Function.h"
#include "UObject/TopLevelAssetPath.h"

class UClass;
class UFactory;
class UObject;

// Factories whose New-asset dialog hands the context asset over in a form the reflected-field rule cannot match by
// class (for example a document template that names the asset, plus the Content Browser selection). The module that
// owns the asset type registers how the dialog configures such a factory for its context objects; the generic factory
// route then calls it instead of the field rule (ConfigureAssetDocumentEditorGenericFactoryContexts).
//
// The configurator returns nullptr on success, otherwise a FactoryCreate.* diagnostic code
// (FactoryCreate.ContextUnsupported when the dialog would not accept the objects).
using FAssetDocumentEditorFactoryContextConfigurator = TFunction<const TCHAR*(UFactory& Factory, TConstArrayView<UObject*> Contexts)>;

ASSETDOCUMENTEDITOR_API void RegisterAssetDocumentEditorFactoryContextConfigurator(const FTopLevelAssetPath& FactoryClassPath,
	FAssetDocumentEditorFactoryContextConfigurator Configure);
ASSETDOCUMENTEDITOR_API void UnregisterAssetDocumentEditorFactoryContextConfigurator(const FTopLevelAssetPath& FactoryClassPath);

// The configurator registered for exactly FactoryClass (not its subclasses), or null.
ASSETDOCUMENTEDITOR_API const FAssetDocumentEditorFactoryContextConfigurator* FindAssetDocumentEditorFactoryContextConfigurator(
	const UClass& FactoryClass);
