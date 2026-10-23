// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentGraphComponentTemplates.h"

#include "Components/ActorComponent.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/InheritableComponentHandler.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Kismet2/KismetReinstanceUtilities.h"
#include "UObject/Package.h"
#include "UObject/UObjectHash.h"

namespace UE::AssetDocument::Graph
{
namespace
{
struct FStaleTemplate
{
	TWeakObjectPtr<UBlueprint> Blueprint;
	FString Component;
	FString Template;
	FString StaleClass;
	UClass* Stale = nullptr;
	UClass* Current = nullptr;
	bool bTemplateValid = false;
};

// The class a stale template must become: the class its SCS node / override record declares or,
// failing that, the current class of the Blueprint that generated the stale class.
UClass* FindCurrentClass(UClass* Stale, UClass* Declared)
{
	if (Declared != nullptr && !IsOutdatedComponentClass(Declared) && Declared->IsChildOf<UActorComponent>())
	{
		return Declared;
	}
	const UBlueprint* Generator = Stale != nullptr ? UBlueprint::GetBlueprintFromClass(Stale) : nullptr;
	UClass* Generated = Generator != nullptr ? Generator->GeneratedClass.Get() : nullptr;
	return Generated != nullptr && !IsOutdatedComponentClass(Generated) && Generated->IsChildOf<UActorComponent>() ? Generated : nullptr;
}

bool IsScanned(const UBlueprint& Blueprint)
{
	// Staged copies (fresh verification) and transient scratch Blueprints are not the user's.
	const UPackage* Package = Blueprint.GetOutermost();
	return IsValid(&Blueprint) && Package != nullptr && Package != GetTransientPackage()
		&& !Package->GetName().StartsWith(TEXT("/Temp/")) && !Package->HasAnyPackageFlags(PKG_CompiledIn);
}

void AddIfStale(UBlueprint& Blueprint, const FString& Component, UActorComponent* Template, UClass* Declared, TArray<FStaleTemplate>& Out)
{
	if (Template == nullptr)
	{
		return;
	}
	UClass* Class = Template->GetClass();
	const bool bValid = IsValid(Template);
	if (bValid && !IsOutdatedComponentClass(Class))
	{
		return;
	}
	FStaleTemplate& Entry = Out.AddDefaulted_GetRef();
	Entry.Blueprint = &Blueprint;
	Entry.Component = Component;
	Entry.Template = Template->GetPathName();
	Entry.StaleClass = GetPathNameSafe(Class);
	Entry.Stale = Class;
	Entry.Current = FindCurrentClass(Class, Declared);
	Entry.bTemplateValid = bValid;
}

TArray<FStaleTemplate> FindStaleTemplates()
{
	TArray<FStaleTemplate> Stale;
	TArray<UObject*> Blueprints;
	GetObjectsOfClass(UBlueprint::StaticClass(), Blueprints, true, RF_ClassDefaultObject, EInternalObjectFlags::Garbage);
	for (UObject* Object : Blueprints)
	{
		UBlueprint* Blueprint = CastChecked<UBlueprint>(Object);
		if (!IsScanned(*Blueprint))
		{
			continue;
		}
		if (const USimpleConstructionScript* Script = Blueprint->SimpleConstructionScript)
		{
			for (USCS_Node* Node : Script->GetAllNodes())
			{
				if (Node != nullptr)
				{
					AddIfStale(*Blueprint, Node->GetVariableName().ToString(), Node->ComponentTemplate, Node->ComponentClass, Stale);
				}
			}
		}
		if (UInheritableComponentHandler* Handler = Blueprint->GetInheritableComponentHandler(false))
		{
			for (auto Record = Handler->CreateRecordIterator(); Record; ++Record)
			{
				AddIfStale(*Blueprint, Record->ComponentKey.GetSCSVariableName().ToString(), Record->ComponentTemplate, Record->ComponentClass, Stale);
			}
		}
		if (const UBlueprintGeneratedClass* Generated = Cast<UBlueprintGeneratedClass>(Blueprint->GeneratedClass))
		{
			for (UActorComponent* Template : Generated->ComponentTemplates)
			{
				AddIfStale(*Blueprint, Template != nullptr ? Template->GetName() : FString(), Template, nullptr, Stale);
			}
		}
	}
	return Stale;
}

FAssetDocumentRuntimeDiagnostic TemplateMakeDiagnostic(const TCHAR* Code, const FString& Message)
{
	FAssetDocumentRuntimeDiagnostic Diagnostic;
	Diagnostic.Path = TEXT("/ComponentTemplates");
	Diagnostic.Code = Code;
	Diagnostic.Severity = EAssetDocumentDiagnosticSeverity::Warning;
	Diagnostic.Message = Message;
	return Diagnostic;
}

FString Describe(const FStaleTemplate& Entry)
{
	return FString::Printf(TEXT("component '%s' of %s (template %s, class %s)"), *Entry.Component,
		*GetPathNameSafe(Entry.Blueprint.Get()), *Entry.Template, *Entry.StaleClass);
}
} // namespace

bool IsOutdatedComponentClass(const UClass* Class)
{
	if (Class == nullptr)
	{
		return false;
	}
	if (!IsValid(Class) || Class->HasAnyClassFlags(CLASS_NewerVersionExists))
	{
		return true;
	}
	const FString Name = Class->GetName();
	return Name.StartsWith(TEXT("REINST_")) || Name.StartsWith(TEXT("TRASHCLASS_")) || Name.StartsWith(TEXT("HOTRELOADED_"));
}

void CheckComponentTemplates(TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics)
{
	check(IsInGameThread());
	const TArray<FStaleTemplate> Found = FindStaleTemplates();
	if (Found.IsEmpty())
	{
		return;
	}
	TMap<UClass*, UClass*> OldToNew;
	TMap<UPackage*, bool> DirtyFlags;
	for (const FStaleTemplate& Entry : Found)
	{
		if (Entry.bTemplateValid && Entry.Current != nullptr && Entry.Stale != Entry.Current)
		{
			OldToNew.Add(Entry.Stale, Entry.Current);
			if (UBlueprint* Blueprint = Entry.Blueprint.Get())
			{
				UPackage* Package = Blueprint->GetOutermost();
				DirtyFlags.Add(Package, Package->IsDirty());
			}
		}
	}
	if (!OldToNew.IsEmpty())
	{
		// What the editor's reinstancing does for every instance of a recompiled class: a new
		// template of the current class, the old values copied over, references repointed.
		FReplaceInstancesOfClassParameters Parameters;
		FBlueprintCompileReinstancer::BatchReplaceInstancesOfClass(OldToNew, Parameters);
		for (const TPair<UPackage*, bool>& Entry : DirtyFlags)
		{
			Entry.Key->SetDirtyFlag(Entry.Value);
		}
	}
	const TArray<FStaleTemplate> Remaining = FindStaleTemplates();
	for (const FStaleTemplate& Entry : Found)
	{
		const bool bStill = Remaining.ContainsByPredicate([&Entry](const FStaleTemplate& Other)
		{
			return Other.Blueprint == Entry.Blueprint && Other.Component == Entry.Component;
		});
		if (bStill)
		{
			OutDiagnostics.Add(TemplateMakeDiagnostic(TEXT("Graph.DependentTemplateStale"), FString::Printf(
				TEXT("The %s is an instance of a replaced class and could not be reinstanced: instances of this "
				"Blueprint get an outdated component. Restart the editor (the saved asset on disk is unaffected) "
				"before playing or saving it."), *Describe(Entry))));
		}
		else
		{
			OutDiagnostics.Add(TemplateMakeDiagnostic(TEXT("Graph.DependentTemplateRepaired"), FString::Printf(
				TEXT("The %s was an instance of a replaced class; it was reinstanced to %s as the editor's compile does."),
				*Describe(Entry), *GetPathNameSafe(Entry.Current))));
		}
	}
}
} // namespace UE::AssetDocument::Graph
