// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Templates/Function.h"
#include "UObject/TopLevelAssetPath.h"

class UClass;
class UFactory;
class UObject;

// Factories whose FactoryCreateNew always opens a modal settings dialog and, after its OK, makes a plain
// NewObject of the asset class with the factory's flags and fills it from the dialog's settings
// (UPhysicsAssetFactory: the "New Physics Asset" generation dialog). Such a factory cannot run unattended
// (FactoryCreate.InteractiveFactory), but the module that owns the asset type can state what the factory
// does after the dialog. The generic factory route then creates the object without running the factory:
// the target as AssetTools creates a factory-less class (IAssetTools::CreateAsset with no factory:
// NewObject with RF_Public | RF_Standalone | RF_Transactional, asset registry notification, package
// dirty), a scratch as a transient NewObject, and calls the registered initializer with the configured
// factory instance (its factory_context fields set, as the dialog-free part of the factory reads them).
// Settings the dialog would ask for are the document's to state (the owning profile applies them).
// The same registration serves a factory without a dialog whose new object lacks what a new asset of the type
// starts with: the initializer adds it (e.g. an IK Retargeter's default op stack).
//
// The initializer runs inside the non-interactive scope; returning false (with a message) fails the
// creation with FactoryCreate.DialogFactoryInitFailed and discards the new object.
using FAssetDocumentEditorDialogFactoryInit = TFunction<bool(const UFactory& Factory, UObject& NewAsset, FString& OutMessage)>;

ASSETDOCUMENTEDITOR_API void RegisterAssetDocumentEditorDialogFactory(const FTopLevelAssetPath& FactoryClassPath,
	FAssetDocumentEditorDialogFactoryInit Init);
ASSETDOCUMENTEDITOR_API void UnregisterAssetDocumentEditorDialogFactory(const FTopLevelAssetPath& FactoryClassPath);

// The initializer registered for exactly FactoryClass (not its subclasses), or null.
ASSETDOCUMENTEDITOR_API const FAssetDocumentEditorDialogFactoryInit* FindAssetDocumentEditorDialogFactory(const UClass& FactoryClass);
