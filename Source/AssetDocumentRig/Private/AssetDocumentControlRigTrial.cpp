// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// Validate's trial target of a Control Rig (FAssetDocumentEditorTrialScope::RegisterTrialTargetMaker): Validate runs
// the profile's apply on a transient copy of the rig (or a new transient rig), so pin, node, element and compile
// errors come back at validate. The generic scratch cannot make one: it refuses Blueprints as clone sources (a copy
// needs its own generated class) and the factory makes a Blueprint Public / Standalone. The copy is the editor's
// Duplicate (StaticDuplicateObject: the Blueprint's PostDuplicate makes and compiles the copy's own generated class,
// the RigVM asset's PostDuplicate moves its function identifiers to the copy), the new rig the Control Rig factory
// with the create context's parent class (Rig / Module: ControlRig; Modular: ModularRig, which adds module Root), both
// in the transient package. The Blueprint is then transient and neither Public nor Standalone, so it dies with the
// request (its generated classes are outered to the transient package and only referenced by it).
// Both the trial's Duplicate and the duplicate operation run the Control Rig duplicate fixup (FlagModelGraphsForPostLoad).

#include "AssetDocumentControlRig.h"

#include "AssetDocumentDuplicateFixups.h"
#include "AssetDocumentEditorTrialScope.h"
#include "ControlRig.h"
#include "ControlRigBlueprintFactory.h"
#include "ControlRigBlueprintLegacy.h"
#include "Misc/FeedbackContext.h"
#include "RigVMModel/RigVMGraph.h"
#include "UObject/Package.h"
#include "UObject/UObjectHash.h"

namespace UE::AssetDocument::Graph::Rig::ControlRig
{
namespace
{
// A RigVM asset's PostLoad post-loads its model graphs only when they are flagged for PostLoad (on some engines
// FRigVMClient::PatchModelsOnLoad calls ConditionalPostLoad on them), and only a graph's PostLoad fills each pin's list
// of its links. An in-memory duplicate does not flag the asset's default subobjects, so the client's own graphs (the
// rig graph and the function library) are patched and refreshed with empty pin link lists: a reroute node then reads
// as unlinked and is removed with the links through it ("Cannot break link ... Reroute.Value" in the log), and a rig
// whose structure hash skips the refresh compiles without its links until it is reloaded. Flagged, the copy
// post-loads as a rig loaded from disk does. Where the client post-loads each model graph itself, a flagged graph
// post-loads once more, as the client's other graphs already do.
void FlagModelGraphsForPostLoad(UObject& Copy, const TMap<UObject*, UObject*>& CreatedObjects)
{
	for (const TPair<UObject*, UObject*>& Pair : CreatedObjects)
	{
		URigVMGraph* Graph = Cast<URigVMGraph>(Pair.Value);
		if (Graph != nullptr && !Graph->IsTemplate() && Graph->IsIn(&Copy) && !Graph->HasAnyFlags(RF_NeedPostLoad))
		{
			Graph->SetFlags(RF_NeedPostLoad | RF_NeedPostLoadSubobjects);
		}
	}
}

UObject* MakeTrialTarget(UClass& AssetClass, UObject* Source, TConstArrayView<UObject*> FactoryContexts, FString& OutMessage)
{
	if (!AssetClass.IsChildOf(UControlRigBlueprint::StaticClass()))
	{
		OutMessage = TEXT("not a Control Rig Blueprint class");
		return nullptr;
	}
	UPackage* const Transient = GetTransientPackage();
	UControlRigBlueprint* Made = nullptr;
	if (Source != nullptr)
	{
		UControlRigBlueprint* Rig = Cast<UControlRigBlueprint>(Source);
		if (Rig == nullptr)
		{
			OutMessage = FString::Printf(TEXT("%s is not a Control Rig"), *Source->GetPathName());
			return nullptr;
		}
		const FName Name = MakeUniqueObjectName(Transient, &AssetClass, *FString::Printf(TEXT("Trial_%s"), *Rig->GetName()));
		const UControlRig* Defaults = Rig->GeneratedClass != nullptr ? Cast<UControlRig>(Rig->GeneratedClass->GetDefaultObject(false)) : nullptr;
		const int32 DefaultElements = Defaults != nullptr && Defaults->GetHierarchy() != nullptr ? Defaults->GetHierarchy()->Num() : INDEX_NONE;
		FObjectDuplicationParameters Parameters = InitStaticDuplicateObjectParams(Rig, Transient, Name,
			static_cast<EObjectFlags>(RF_AllFlags & ~(RF_Public | RF_Standalone)));
		Made = Cast<UControlRigBlueprint>(DuplicateObjectWithAssetDocumentFixups(Parameters));
		// Some engines' duplicate leaves the source's class default without its hierarchy, and the instances made from
		// it (the open editor's rig, a new instance) with none: the source's hierarchy is propagated to them again.
		if (DefaultElements != INDEX_NONE && Defaults->GetHierarchy() != nullptr && Defaults->GetHierarchy()->Num() != DefaultElements)
		{
			Rig->PropagateHierarchyFromBPToInstances();
		}
	}
	else
	{
		UControlRigBlueprintFactory* Factory = NewObject<UControlRigBlueprintFactory>(Transient, NAME_None, RF_Transient);
		for (UObject* Context : FactoryContexts)
		{
			UClass* Parent = Cast<UClass>(Context);
			if (Parent == nullptr || !Parent->IsChildOf(UControlRig::StaticClass()))
			{
				OutMessage = FString::Printf(TEXT("the create context %s is not a Control Rig parent class"), *GetPathNameSafe(Context));
				return nullptr;
			}
			Factory->ParentClass = Parent;
		}
		const FName Name = MakeUniqueObjectName(Transient, &AssetClass, TEXT("Trial_ControlRig"));
		Made = Cast<UControlRigBlueprint>(Factory->FactoryCreateNew(&AssetClass, Transient, Name, RF_Transient, nullptr, GWarn));
		Factory->MarkAsGarbage();
	}
	if (Made == nullptr)
	{
		OutMessage = Source != nullptr ? TEXT("the duplicate failed") : TEXT("the Control Rig factory made nothing");
		return nullptr;
	}
	// FKismetEditorUtilities::CreateBlueprint makes the Blueprint Public | Standalone; nothing of the trial may outlive
	// the request.
	auto MakeTransient = [](UObject* Object)
	{
		Object->SetFlags(RF_Transient);
		Object->ClearFlags(RF_Public | RF_Standalone);
	};
	MakeTransient(Made);
	ForEachObjectWithOuter(Made, MakeTransient);
	for (UClass* Class : {Made->GeneratedClass.Get(), Made->SkeletonGeneratedClass.Get()})
	{
		if (Class != nullptr && Class->GetOutermost() == Transient)
		{
			Class->ClearFlags(RF_Standalone);
			ForEachObjectWithOuter(Class, [](UObject* Inner) { Inner->ClearFlags(RF_Standalone); });
			if (UObject* Default = Class->GetDefaultObject(false))
			{
				Default->ClearFlags(RF_Standalone);
			}
		}
	}
	return Made;
}
} // namespace

UControlRigBlueprint* MakeTransientCopy(UControlRigBlueprint& Source, FString& OutMessage)
{
	return Cast<UControlRigBlueprint>(MakeTrialTarget(*Source.GetClass(), &Source, {}, OutMessage));
}

void RegisterTrialTargetMaker(const FTopLevelAssetPath& ControlRigBlueprintPath)
{
	FAssetDocumentEditorTrialScope::RegisterTrialTargetMaker(ControlRigBlueprintPath, &MakeTrialTarget);
	RegisterAssetDocumentDuplicateFixup(ControlRigBlueprintPath, &FlagModelGraphsForPostLoad);
}

void UnregisterTrialTargetMaker(const FTopLevelAssetPath& ControlRigBlueprintPath)
{
	UnregisterAssetDocumentDuplicateFixup(ControlRigBlueprintPath);
	FAssetDocumentEditorTrialScope::UnregisterTrialTargetMaker(ControlRigBlueprintPath);
}
} // namespace UE::AssetDocument::Graph::Rig::ControlRig
