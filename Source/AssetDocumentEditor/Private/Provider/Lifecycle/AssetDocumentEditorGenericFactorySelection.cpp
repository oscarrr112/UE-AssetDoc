// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "Provider/Lifecycle/AssetDocumentEditorGenericFactory.h"

#include "Engine/Blueprint.h"
#include "GameFramework/Actor.h"
#include "AssetDocumentExactBlueprintClasses.h"
#include "Factories/Factory.h"
#include "UObject/Class.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"

namespace
{
constexpr EClassFlags RejectedClassFlags =
	CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists;

struct FFactoryCandidate
{
	UClass* FactoryClass = nullptr;
	int32 Distance = MAX_int32;
	bool bShownInNewMenu = false;
	FString FactoryPath;
};

int32 GetInheritanceDistance(const UClass& Child, const UClass& Ancestor)
{
	int32 Distance = 0;
	for (const UClass* Current = &Child; Current != nullptr; Current = Current->GetSuperClass())
	{
		if (Current == &Ancestor)
		{
			return Distance;
		}
		++Distance;
	}
	return INDEX_NONE;
}

bool IsBetterCandidate(const FFactoryCandidate& Left, const FFactoryCandidate& Right)
{
	if (Left.Distance != Right.Distance)
	{
		return Left.Distance < Right.Distance;
	}
	if (Left.bShownInNewMenu != Right.bShownInNewMenu)
	{
		return Left.bShownInNewMenu;
	}
	return Left.FactoryPath < Right.FactoryPath;
}

bool TryMakeCandidate(UClass& FactoryClass, const UClass& AssetClass, FFactoryCandidate& OutCandidate)
{
	if (!FactoryClass.IsChildOf(UFactory::StaticClass())
		|| FactoryClass.HasAnyClassFlags(RejectedClassFlags))
	{
		return false;
	}
	const UFactory* Factory = FactoryClass.GetDefaultObject<UFactory>();
	const UClass* Supported = Factory != nullptr ? Factory->GetSupportedClass() : nullptr;
	if (Supported == nullptr || !Factory->CanCreateNew()
		|| !EnumHasAnyFlags(Factory->GetSupportedWorkflows(), EFactoryCreateWorkflow::Default))
	{
		return false;
	}
	const int32 Distance = GetInheritanceDistance(AssetClass, *Supported);
	const bool bShown = Factory->ShouldShowInNewMenu();
	// An ancestor factory is only trusted when the New menu offers it (it then
	// receives the exact class, as the editor's class-picker flow does).
	if (Distance == INDEX_NONE || (Distance > 0 && !bShown))
	{
		return false;
	}
	OutCandidate.FactoryClass = &FactoryClass;
	OutCandidate.Distance = Distance;
	OutCandidate.bShownInNewMenu = bShown;
	OutCandidate.FactoryPath = FactoryClass.GetPathName();
	return true;
}

// A concrete factory made for exactly AssetClass that would create new assets (bCreateNew, default workflow) but
// reports CanCreateNew() false in this editor (UExternalDataLayerFactory without its experimental setting).
bool IsDisabledExactFactory(UClass& FactoryClass, const UClass& AssetClass)
{
	if (!FactoryClass.IsChildOf(UFactory::StaticClass()) || FactoryClass.HasAnyClassFlags(RejectedClassFlags))
	{
		return false;
	}
	const UFactory* Factory = FactoryClass.GetDefaultObject<UFactory>();
	// bCreateNew is protected; it is a reflected property (the default CanCreateNew() result).
	static const FBoolProperty* CreateNewProperty =
		CastField<FBoolProperty>(UFactory::StaticClass()->FindPropertyByName(TEXT("bCreateNew")));
	return Factory != nullptr && CreateNewProperty != nullptr && Factory->GetSupportedClass() == &AssetClass
		&& CreateNewProperty->GetPropertyValue_InContainer(Factory) && !Factory->CanCreateNew() && EnumHasAnyFlags(Factory->GetSupportedWorkflows(), EFactoryCreateWorkflow::Default);
}
}

const TCHAR* FAssetDocumentEditorGenericFactorySelection::GetDiagnosticCode() const
{
	switch (Rejection)
	{
	case EAssetDocumentEditorGenericFactoryRejection::None:
		return TEXT("");
	case EAssetDocumentEditorGenericFactoryRejection::BlueprintClass:
	case EAssetDocumentEditorGenericFactoryRejection::ActorClass:
		return AssetDocumentEditorBlueprintClassUnsupportedCode;
	case EAssetDocumentEditorGenericFactoryRejection::FactoryDisabled:
		return AssetDocumentEditorFactoryDisabledCode;
	case EAssetDocumentEditorGenericFactoryRejection::AbstractClass:
		return TEXT("FactoryCreate.AbstractClass");
	case EAssetDocumentEditorGenericFactoryRejection::NoFactory:
		return TEXT("FactoryCreate.NoFactory");
	default:
		return TEXT("FactoryCreate.ClassMismatch");
	}
}

FString FAssetDocumentEditorGenericFactorySelection::GetDiagnosticMessage(
	const UClass* AssetClass) const
{
	const FString ClassPath = AssetClass != nullptr ? AssetClass->GetPathName() : FString(TEXT("<none>"));
	switch (Rejection)
	{
	case EAssetDocumentEditorGenericFactoryRejection::None:
		return FString();
	case EAssetDocumentEditorGenericFactoryRejection::BlueprintClass:
		return FString::Printf(TEXT("%s is a Blueprint asset class; create it through its Blueprint profile, not the generic factory lifecycle."), *ClassPath);
	case EAssetDocumentEditorGenericFactoryRejection::ActorClass:
		return FString::Printf(TEXT("%s is an Actor class. Actors are not standalone assets: the editor's New-asset factory for it creates a Blueprint whose parent class is %s. Create that Blueprint through the Blueprint profile (a Blueprint document with this parent class), not the generic factory lifecycle."), *ClassPath, *ClassPath);
	case EAssetDocumentEditorGenericFactoryRejection::FactoryDisabled:
		return FString::Printf(TEXT("The editor factory that creates %s (%s) is switched off in this editor (it reports CanCreateNew() false, for example because an editor or experimental setting disables it), so the Content Browser does not offer it and the generic route does not create it through an ancestor factory either. Enable that setting, or create the asset in the editor and Update it with a document."), *ClassPath, *DisabledFactoryPath);
	case EAssetDocumentEditorGenericFactoryRejection::AbstractClass:
		return FString::Printf(TEXT("%s is abstract or deprecated; choose a concrete asset class."), *ClassPath);
	case EAssetDocumentEditorGenericFactoryRejection::NoFactory:
		return FString::Printf(TEXT("No editor factory can create new %s assets (no UFactory supports it with CanCreateNew). Such assets can only be updated, not created."), *ClassPath);
	default:
		return TEXT("The asset class is not a valid canonical class.");
	}
}

FAssetDocumentEditorGenericFactorySelection
SelectAssetDocumentEditorGenericFactory(const UClass* AssetClass)
{
	FAssetDocumentEditorGenericFactorySelection Result;
	if (AssetClass == nullptr || !IsValid(AssetClass))
	{
		return Result;
	}
	// Blueprint-derived classes are created through their Blueprint profile, except the classes an exact profile
	// registered (AssetDocumentExactBlueprintClasses.h), whose editor factory is the Create route.
	if (AssetClass->IsChildOf(UBlueprint::StaticClass()) && !IsAssetDocumentExactBlueprintClass(AssetClass))
	{
		Result.Rejection = EAssetDocumentEditorGenericFactoryRejection::BlueprintClass;
		return Result;
	}
	if (AssetClass->HasAnyClassFlags(RejectedClassFlags))
	{
		Result.Rejection = EAssetDocumentEditorGenericFactoryRejection::AbstractClass;
		return Result;
	}
	if (AssetClass->IsChildOf(AActor::StaticClass()))
	{
		Result.Rejection = EAssetDocumentEditorGenericFactoryRejection::ActorClass;
		return Result;
	}
	// A Blueprint class here is one an exact profile serves: its own factory stays its Create route where an editor
	// setting switches that factory off in favour of another asset type for new rigs or graphs.
	const bool bExactBlueprintProfile = AssetClass->IsChildOf(UBlueprint::StaticClass());
	TOptional<FFactoryCandidate> Best;
	UClass* DisabledExactFactory = nullptr;
	for (TObjectIterator<UClass> It; It; ++It)
	{
		FFactoryCandidate Candidate;
		if (TryMakeCandidate(**It, *AssetClass, Candidate)
			&& (!Best.IsSet() || IsBetterCandidate(Candidate, Best.GetValue())))
		{
			Best = MoveTemp(Candidate);
		}
		else if (IsDisabledExactFactory(**It, *AssetClass))
		{
			if (bExactBlueprintProfile)
			{
				FFactoryCandidate Exact;
				Exact.FactoryClass = *It;
				Exact.Distance = 0;
				Exact.FactoryPath = It->GetPathName();
				if (!Best.IsSet() || IsBetterCandidate(Exact, Best.GetValue()))
				{
					Best = MoveTemp(Exact);
				}
			}
			else if (DisabledExactFactory == nullptr)
			{
				DisabledExactFactory = *It;
			}
		}
	}
	// The class's own factory is switched off: the editor offers no way to create it, and an ancestor factory
	// would only create its base class.
	if (DisabledExactFactory != nullptr && (!Best.IsSet() || Best->Distance > 0))
	{
		Result.Rejection = EAssetDocumentEditorGenericFactoryRejection::FactoryDisabled;
		Result.DisabledFactoryPath = DisabledExactFactory->GetPathName();
		return Result;
	}
	if (!Best.IsSet())
	{
		Result.Rejection = EAssetDocumentEditorGenericFactoryRejection::NoFactory;
		return Result;
	}
	Result.FactoryClass = Best->FactoryClass;
	Result.bExactSupportedClass = Best->Distance == 0;
	Result.Rejection = EAssetDocumentEditorGenericFactoryRejection::None;
	return Result;
}

bool IsAssetDocumentEditorGenericFactoryClassAdmitted(const UClass* AssetClass)
{
	return SelectAssetDocumentEditorGenericFactory(AssetClass).IsSelected();
}
