// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// Raw construction kept in its own file: AssetTools creates its New/Import
// factories with NewObject<UFactory>(GetTransientPackage(), FactoryClass) so a
// configured instance never mutates the factory CDO. Behavior test:
// UEAssetDoc.GenericProvider.GenericFactory.TransientInstance.

#include "Provider/Lifecycle/AssetDocumentEditorGenericFactory.h"

#include "Factories/Factory.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

TStrongObjectPtr<UFactory> NewAssetDocumentEditorGenericFactoryInstance(
	UClass& FactoryClass)
{
	if (!FactoryClass.IsChildOf(UFactory::StaticClass())
		|| FactoryClass.HasAnyClassFlags(CLASS_Abstract))
	{
		return TStrongObjectPtr<UFactory>();
	}
	return TStrongObjectPtr<UFactory>(NewObject<UFactory>(
		GetTransientPackage(), &FactoryClass, NAME_None, RF_Transient));
}
