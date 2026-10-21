// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// Raw reflected factory configuration kept in its own file. When an asset
// class has no factory of its own, the editor's New-asset flow uses the
// nearest ancestor factory and its ConfigureProperties class picker stores the
// chosen subclass in a class-typed factory property (UCurveFactory::CurveClass,
// USoundSourceEffectFactory::SoundEffectSourcepresetClass,
// UAudioSynesthesiaNRTFactory::AudioSynesthesiaNRTClass, ...), which
// FactoryCreateNew then instantiates. This sets that one property to the
// requested class, as the picker would. Behavior test:
// UEAssetDoc.GenericRuntime.Robustness.FactoryConfiguration.

#include "Provider/Lifecycle/AssetDocumentEditorGenericFactory.h"

#include "Engine/Blueprint.h"
#include "Factories/Factory.h"
#include "UObject/UnrealType.h"

namespace
{
bool IsDeclaredByFactorySubclass(const FProperty& Property)
{
	const UClass* Owner = Property.GetOwnerClass();
	return Owner != nullptr && Owner != UFactory::StaticClass() && Owner->IsChildOf(UFactory::StaticClass());
}

// A class-typed factory setting that selects the class to create: its meta
// class is a real asset base related to the factory's supported class and
// accepts AssetClass.
bool IsAssetClassSetting(const FClassProperty& Property, const UClass& SupportedClass, const UClass& AssetClass)
{
	const UClass* MetaClass = Property.MetaClass;
	return IsDeclaredByFactorySubclass(Property) && Property.ArrayDim == 1
		&& MetaClass != nullptr && MetaClass != UObject::StaticClass()
		&& AssetClass.IsChildOf(MetaClass)
		&& (MetaClass->IsChildOf(&SupportedClass) || SupportedClass.IsChildOf(MetaClass));
}

FString JoinFields(const TArray<FObjectPropertyBase*>& Fields)
{
	TArray<FString> Parts;
	for (const FObjectPropertyBase* Field : Fields)
	{
		Parts.Add(FString::Printf(TEXT("%s (%s)"), *Field->GetName(), *GetPathNameSafe(Field->PropertyClass)));
	}
	return FString::Join(Parts, TEXT(", "));
}
}

bool ConfigureAssetDocumentEditorGenericFactoryAssetClass(
	UFactory& Factory, UClass& AssetClass, FString& OutCode, FString& OutMessage)
{
	const UClass* SupportedClass = Factory.GetSupportedClass();
	if (SupportedClass == nullptr || SupportedClass == &AssetClass
		|| Factory.HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject))
	{
		return true;
	}
	TArray<FClassProperty*> Settings;
	for (TFieldIterator<FClassProperty> It(Factory.GetClass()); It; ++It)
	{
		if (IsAssetClassSetting(**It, *SupportedClass, AssetClass))
		{
			Settings.Add(*It);
		}
	}
	if (Settings.Num() > 1)
	{
		OutCode = AssetDocumentEditorSubclassUnsupportedCode;
		OutMessage = FString::Printf(
			TEXT("No editor factory creates %s directly, and the nearest New-menu factory %s has several class "
				"settings that could select it; the generic route does not guess. Create the asset in the editor, "
				"then Update it with a document."),
			*AssetClass.GetPathName(), *Factory.GetClass()->GetPathName());
		return false;
	}
	// No setting: the factory may honor the requested class itself (as
	// MaterialInstanceConstantFactoryNew does); the exact-class check after
	// creation refuses it otherwise.
	if (Settings.Num() == 1)
	{
		Settings[0]->SetObjectPropertyValue_InContainer(&Factory, &AssetClass);
	}
	return true;
}

FString DescribeAssetDocumentEditorGenericFactoryShortfall(
	const UFactory& Factory, const UClass& AssetClass, bool bHadContext, const UObject* Created, FString& OutCode)
{
	const UClass* SupportedClass = Factory.GetSupportedClass();
	const FString FactoryPath = Factory.GetClass()->GetPathName();
	// The factory makes Blueprints (UNamingTokensFactory: a Blueprint whose parent is its supported class), so the
	// editor never creates a plain AssetClass asset with it.
	if (const UBlueprint* Blueprint = Cast<UBlueprint>(Created))
	{
		OutCode = AssetDocumentEditorBlueprintClassUnsupportedCode;
		return FString::Printf(
			TEXT("%s creates a Blueprint (parent class %s), not a %s asset: the editor creates no plain %s asset. "
				"Create the Blueprint through the Blueprint profile (a Blueprint document with that parent class)."),
			*FactoryPath, *GetPathNameSafe(Blueprint->ParentClass.Get()), *AssetClass.GetPathName(), *AssetClass.GetName());
	}
	if (SupportedClass != nullptr && SupportedClass != &AssetClass)
	{
		OutCode = AssetDocumentEditorSubclassUnsupportedCode;
		return FString::Printf(
			TEXT("No editor factory creates %s directly; the nearest New-menu factory %s creates %s and has no class "
				"setting the generic route can set to the subclass (the editor picks it in a dialog)%s. %s cannot be "
				"created from a document; create it in the editor, then Update it with a document."),
			*AssetClass.GetPathName(), *FactoryPath, *SupportedClass->GetPathName(),
			Created != nullptr ? *FString::Printf(TEXT(", so it created %s instead"), *Created->GetClass()->GetPathName()) : TEXT(""),
			*AssetClass.GetName());
	}
	TArray<FObjectPropertyBase*> Missing;
	for (FObjectPropertyBase* Field : GetAssetDocumentEditorGenericFactoryContextFields(*Factory.GetClass()))
	{
		if (Field->GetObjectPropertyValue_InContainer(&Factory) == nullptr)
		{
			Missing.Add(Field);
		}
	}
	TArray<FString> ClassSettings;
	for (TFieldIterator<FClassProperty> It(Factory.GetClass()); It; ++It)
	{
		if (IsDeclaredByFactorySubclass(**It) && It->ArrayDim == 1
			&& It->GetObjectPropertyValue_InContainer(&Factory) == nullptr)
		{
			ClassSettings.Add(It->GetName());
		}
	}
	if (Missing.IsEmpty() && ClassSettings.IsEmpty())
	{
		OutCode = TEXT("FactoryCreate.IncompleteCreation");
		return FString::Printf(TEXT("%s did not create an exact %s (its New-asset dialog configures it in a way a "
			"document cannot supply)."), *FactoryPath, *AssetClass.GetPathName());
	}
	OutCode = AssetDocumentEditorMissingFactoryContextCode;
	FString Message = FString::Printf(TEXT("%s created no %s: it needs what its New-asset dialog asks for."),
		*FactoryPath, *AssetClass.GetPathName());
	if (!Missing.IsEmpty())
	{
		Message += FString::Printf(TEXT(" %s FactoryContext to the path of an existing asset for one of these factory "
			"fields: %s."), bHadContext ? TEXT("The given FactoryContext did not satisfy it; set") : TEXT("Set the document's"),
			*JoinFields(Missing));
	}
	if (!ClassSettings.IsEmpty())
	{
		Message += FString::Printf(TEXT(" It also needs a class chosen in that dialog (%s): set the document's "
			"FactoryContext to that class's path (/Script/Module.Class, or a Blueprint's ..._C class)."),
			*FString::Join(ClassSettings, TEXT(", ")));
	}
	return Message;
}
