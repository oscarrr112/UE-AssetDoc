// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// Factories whose New-asset dialog is only a class picker that keeps the chosen class in factory state reflection
// does not expose, and whose FactoryCreateNew then makes a plain NewObject of that class:
//   UInterchangePipelineBaseFactory ("Interchange Pipeline", InterchangePipelineFactories.cpp): ConfigureProperties
//   opens "Pick a Pipeline Class" (children of UInterchangePipelineBase that are not abstract, deprecated,
//   HideDropDown, Hidden or Transient) and stores the choice in the private PipelineClass; FactoryCreateNew is
//   NewObject<UInterchangePipelineBase>(InParent, PipelineClass, Name, Flags | RF_Transactional).
// The document's Class is the class the picker would choose, so the generic factory route creates it as the
// dialog's OK would. They are registered as dialog factories (AssetDocumentEditorDialogFactory.h): the target is
// made as AssetTools makes a factory-less class (NewObject with RF_Public | RF_Standalone | RF_Transactional, the
// same object and flags the factory makes), a scratch as a transient NewObject, and the initializer refuses a class
// the picker does not list. Behavior test: UEAssetDoc.GenericRuntime.Robustness.FactoryConfiguration.

#include "Provider/Lifecycle/AssetDocumentEditorGenericFactory.h"

#include "AssetDocumentEditorDialogFactory.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"

namespace
{
struct FClassPickerFactory
{
	const TCHAR* FactoryClassPath;
	const TCHAR* BaseClassPath;
	const TCHAR* DialogTitle;
	EClassFlags DisallowedClassFlags;
};

const FClassPickerFactory ClassPickerFactories[] = {
	{TEXT("/Script/InterchangeEditorPipelines.InterchangePipelineBaseFactory"),
		TEXT("/Script/InterchangeCore.InterchangePipelineBase"), TEXT("Pick a Pipeline Class"),
		CLASS_Abstract | CLASS_Deprecated | CLASS_HideDropDown | CLASS_Hidden | CLASS_Transient},
};

bool InitPickedClass(const FClassPickerFactory& Picker, const UFactory& Factory, UObject& NewAsset, FString& OutMessage)
{
	(void)Factory;
	const UClass* Base = FindObject<UClass>(nullptr, Picker.BaseClassPath);
	const UClass* Picked = NewAsset.GetClass();
	if (Base == nullptr || !Picked->IsChildOf(Base) || Picked->HasAnyClassFlags(Picker.DisallowedClassFlags))
	{
		OutMessage = FString::Printf(
			TEXT("%s is not offered by the factory's \"%s\" class picker (it lists the concrete subclasses of %s that "
				"are not abstract, deprecated, hidden, HideDropDown or transient), so the editor cannot create it as a new asset."),
			*Picked->GetPathName(), Picker.DialogTitle, Picker.BaseClassPath);
		return false;
	}
	return true;
}
}

void RegisterAssetDocumentEditorClassPickerFactories()
{
	for (const FClassPickerFactory& Picker : ClassPickerFactories)
	{
		RegisterAssetDocumentEditorDialogFactory(FTopLevelAssetPath(Picker.FactoryClassPath),
			[&Picker](const UFactory& Factory, UObject& NewAsset, FString& OutMessage)
			{
				return InitPickedClass(Picker, Factory, NewAsset, OutMessage);
			});
	}
}

void UnregisterAssetDocumentEditorClassPickerFactories()
{
	for (const FClassPickerFactory& Picker : ClassPickerFactories)
	{
		UnregisterAssetDocumentEditorDialogFactory(FTopLevelAssetPath(Picker.FactoryClassPath));
	}
}
