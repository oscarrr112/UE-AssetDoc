// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// Validate's trial target of a new Blueprint (FAssetDocumentEditorTrialScope::RegisterTrialTargetMaker, exactly
// /Script/Engine.Blueprint, /Script/UMGEditor.WidgetBlueprint and /Script/Engine.AnimBlueprint): a transient Blueprint
// of the asset's class and the document's parent class (the create context's one factory_context, a class), made as
// Content Browser > Blueprint Class / User Widget / Animation Blueprint makes one (FKismetEditorUtilities::
// CreateBlueprint with the generated class its factory passes) but in the transient package; an Animation Blueprint is
// made a template (no skeleton: only its K2 graphs are checked). The profiles only read it: Validate of a new
// Blueprint asks graph.link.check.v1 about the document's node keys, pins and links in that Blueprint's palette, as
// it does in the existing target (an existing Blueprint is checked in place, never copied). Only the Blueprint is
// marked transient: its graphs stay ordinary, as the link check reads the action database through the Blueprint's
// own top-level graphs. Neither it nor anything in it is Public / Standalone, so it dies with the request.

#include "AssetDocumentBlueprintTrialTarget.h"

#include "AssetDocumentEditorTrialScope.h"
#include "Animation/AnimBlueprint.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "UObject/Package.h"
#include "UObject/UObjectHash.h"

namespace UE::AssetDocument::Graph
{
namespace
{
UObject* MakeBlueprintTrialTarget(UClass& AssetClass, UObject* Source, TConstArrayView<UObject*> FactoryContexts, FString& OutMessage)
{
	// The generated class the asset class's factory passes (an Animation Blueprint's factory passes the base one; the
	// compiler makes its anim class).
	const UBlueprint* const Kind = AssetClass.IsChildOf(UBlueprint::StaticClass()) ? AssetClass.GetDefaultObject<UBlueprint>() : nullptr;
	UClass* const GeneratedClass = Kind == nullptr ? nullptr
		: AssetClass.IsChildOf(UAnimBlueprint::StaticClass()) ? UBlueprintGeneratedClass::StaticClass() : Kind->GetBlueprintClass();
	if (GeneratedClass == nullptr)
	{
		OutMessage = FString::Printf(TEXT("%s is not a Blueprint class"), *AssetClass.GetPathName());
		return nullptr;
	}
	if (Source != nullptr)
	{
		OutMessage = TEXT("an existing Blueprint is checked in place, not copied");
		return nullptr;
	}
	UClass* const Parent = FactoryContexts.Num() == 1 ? Cast<UClass>(FactoryContexts[0]) : nullptr;
	if (Parent == nullptr || !FKismetEditorUtilities::CanCreateBlueprintOfClass(Parent))
	{
		OutMessage = FString::Printf(TEXT("the create context %s is not a class a Blueprint can derive from"),
			FactoryContexts.Num() == 1 ? *GetPathNameSafe(FactoryContexts[0]) : TEXT("(none)"));
		return nullptr;
	}
	UPackage* const Transient = GetTransientPackage();
	const FName Name = MakeUniqueObjectName(Transient, &AssetClass, *(TEXT("Trial_") + AssetClass.GetName()));
	UBlueprint* const Made = FKismetEditorUtilities::CreateBlueprint(Parent, Transient, Name, BPTYPE_Normal,
		&AssetClass, GeneratedClass);
	if (Made == nullptr)
	{
		OutMessage = TEXT("FKismetEditorUtilities::CreateBlueprint made nothing");
		return nullptr;
	}
	if (UAnimBlueprint* const Anim = Cast<UAnimBlueprint>(Made))
	{
		Anim->bIsTemplate = true;
	}
	Made->SetFlags(RF_Transient);
	auto DropStandalone = [](UObject* Object) { Object->ClearFlags(RF_Public | RF_Standalone); };
	DropStandalone(Made);
	ForEachObjectWithOuter(Made, DropStandalone);
	for (UClass* Class : {Made->GeneratedClass.Get(), Made->SkeletonGeneratedClass.Get()})
	{
		if (Class != nullptr && Class->GetOutermost() == Transient)
		{
			DropStandalone(Class);
			ForEachObjectWithOuter(Class, DropStandalone);
			if (UObject* Default = Class->GetDefaultObject(false))
			{
				DropStandalone(Default);
			}
		}
	}
	return Made;
}

} // namespace

namespace AssetDocumentBlueprintTrialTargetLocal
{
// The Blueprint asset classes whose new assets get a trial Blueprint: a Blueprint Class, a Widget Blueprint (UMGEditor,
// found by path when the trial is made) and an Animation Blueprint.
TArray<FTopLevelAssetPath> TrialBlueprintClassPaths()
{
	return {FTopLevelAssetPath(TEXT("/Script/Engine"), TEXT("Blueprint")), FTopLevelAssetPath(TEXT("/Script/UMGEditor"), TEXT("WidgetBlueprint")),
		FTopLevelAssetPath(TEXT("/Script/Engine"), TEXT("AnimBlueprint"))};
}
} // namespace AssetDocumentBlueprintTrialTargetLocal

void RegisterBlueprintTrialTargetMaker()
{
	using namespace AssetDocumentBlueprintTrialTargetLocal;
	for (const FTopLevelAssetPath& Path : TrialBlueprintClassPaths())
	{
		FAssetDocumentEditorTrialScope::RegisterTrialTargetMaker(Path, &MakeBlueprintTrialTarget);
	}
}

void UnregisterBlueprintTrialTargetMaker()
{
	using namespace AssetDocumentBlueprintTrialTargetLocal;
	for (const FTopLevelAssetPath& Path : TrialBlueprintClassPaths())
	{
		FAssetDocumentEditorTrialScope::UnregisterTrialTargetMaker(Path);
	}
}
} // namespace UE::AssetDocument::Graph
