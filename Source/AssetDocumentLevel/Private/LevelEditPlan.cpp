// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// edit_level preflight (design §3.2): every op is validated against a symbolic plan
// of the world after the earlier ops; nothing in the world changes. Property values
// go through the AssetDocument admission and codec (decode only), with the level
// reference policy.

#include "LevelEditTypes.h"
#include "AssetDocumentNameSuggestions.h"

#include "AssetToolsModule.h"
#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Editor/GroupActor.h"
#include "Editor/UnrealEdEngine.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/Level.h"
#include "Engine/LevelScriptActor.h"
#include "Engine/LevelScriptBlueprint.h"
#include "Engine/LevelStreaming.h"
#include "Engine/LODActor.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "IAssetTools.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/ComponentEditorUtils.h"
#include "LevelToolResult.h"
#include "LevelUtils.h"
#include "LevelWorldIndex.h"
#include "ActorEditorUtils.h"
#include "UnrealEdGlobals.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"

namespace UE::AssetDocumentLevel::Private
{
namespace ObjectProperties = UE::AssetDocument::EditorObjectProperties;

namespace
{
TSharedRef<FJsonObject> NamesDetails(const TCHAR* Field, TArray<FString> Names)
{
	Names.Sort();
	TArray<TSharedPtr<FJsonValue>> Values;
	for (const FString& Name : Names)
	{
		Values.Add(MakeShared<FJsonValueString>(Name));
	}
	TSharedRef<FJsonObject> Details = MakeShared<FJsonObject>();
	Details->SetArrayField(Field, Values);
	return Details;
}

TSharedPtr<FJsonValue> ReadValueJson(UObject& Object, const FString& Property)
{
	FLevelValue Value;
	return ObjectProperties::Read(Object, Property, Value) ? LevelValueToJson(Value) : TSharedPtr<FJsonValue>();
}

// Float-normalised authored value (batch ids stay "@id" in a plan).
FLevelValue NormalizedAuthored(UObject& Owner, const FString& Property, const FLevelValue& Authored)
{
	FLevelValue Expected;
	TArray<FAssetDocumentRuntimeDiagnostic> Ignored;
	return ObjectProperties::PrepareExpected(Owner, Property, Authored, ObjectProperties::FReferencePolicy(), Expected, Ignored)
		? Expected : Authored;
}
}

// ---------------------------------------------------------------------------
// Shared helpers

UActorComponent* FindLevelActorComponent(AActor& Actor, const FString& Name)
{
	TInlineComponentArray<UActorComponent*> Components(&Actor);
	for (UActorComponent* Component : Components)
	{
		if (Component != nullptr && IsValid(Component) && !Component->IsVisualizationComponent()
			&& Component->GetName().Equals(Name, ESearchCase::CaseSensitive))
		{
			return Component;
		}
	}
	return nullptr;
}

TArray<FString> ListLevelActorComponentNames(AActor& Actor)
{
	TArray<FString> Names;
	TInlineComponentArray<UActorComponent*> Components(&Actor);
	for (UActorComponent* Component : Components)
	{
		if (Component != nullptr && IsValid(Component) && !Component->IsVisualizationComponent())
		{
			Names.Add(Component->GetName());
		}
	}
	return Names;
}

UActorComponent* FindLevelClassComponentTemplate(UClass& Class, const FString& Name)
{
	if (AActor* Default = Class.GetDefaultObject<AActor>())
	{
		if (UActorComponent* Native = FindLevelActorComponent(*Default, Name))
		{
			return Native;
		}
	}
	for (UClass* Current = &Class; Current != nullptr; Current = Current->GetSuperClass())
	{
		const UBlueprintGeneratedClass* Generated = Cast<UBlueprintGeneratedClass>(Current);
		const USimpleConstructionScript* Script = Generated != nullptr ? Generated->SimpleConstructionScript.Get() : nullptr;
		if (Script == nullptr)
		{
			continue;
		}
		for (const USCS_Node* Node : Script->GetAllNodes())
		{
			if (Node != nullptr && Node->ComponentTemplate != nullptr
				&& Node->GetVariableName().ToString().Equals(Name, ESearchCase::CaseSensitive))
			{
				return Node->ComponentTemplate;
			}
		}
	}
	return nullptr;
}

TArray<FString> ListLevelClassComponentNames(UClass& Class)
{
	TArray<FString> Names;
	if (AActor* Default = Class.GetDefaultObject<AActor>())
	{
		Names = ListLevelActorComponentNames(*Default);
	}
	for (UClass* Current = &Class; Current != nullptr; Current = Current->GetSuperClass())
	{
		const UBlueprintGeneratedClass* Generated = Cast<UBlueprintGeneratedClass>(Current);
		const USimpleConstructionScript* Script = Generated != nullptr ? Generated->SimpleConstructionScript.Get() : nullptr;
		if (Script == nullptr)
		{
			continue;
		}
		for (const USCS_Node* Node : Script->GetAllNodes())
		{
			if (Node != nullptr && Node->ComponentTemplate != nullptr)
			{
				Names.AddUnique(Node->GetVariableName().ToString());
			}
		}
	}
	return Names;
}

FAssetDocumentDiagnostic ProjectLevelPropertyDiagnostic(const FAssetDocumentRuntimeDiagnostic& Diagnostic, const FString& Prefix,
	const FString& PropertyName)
{
	FAssetDocumentDiagnostic Projected = ProjectAssetDocumentRuntimeDiagnostic(Diagnostic);
	const FString Head = TEXT("/") + PropertyName;
	FString Rest;
	if (Diagnostic.Path.StartsWith(Head, ESearchCase::CaseSensitive))
	{
		Rest = Diagnostic.Path.RightChop(Head.Len());
		if (!Rest.IsEmpty() && !Rest.StartsWith(TEXT("/")))
		{
			Rest.Reset();
		}
	}
	Projected.Path = Prefix + Rest;
	if (Projected.Severity.IsEmpty())
	{
		Projected.Severity = TEXT("error");
	}
	return Projected;
}

ObjectProperties::FReferencePolicy MakeLevelReferencePolicy(const ULevel* Level, TFunction<UObject*(FStringView)> ResolveBatchId)
{
	ObjectProperties::FReferencePolicy Policy;
	Policy.SameLevel = Level;
	Policy.ResolveBatchId = MoveTemp(ResolveBatchId);
	return Policy;
}

FString GetLevelPackageOf(const AActor& Actor)
{
	const ULevel* Level = Actor.GetLevel();
	return Level != nullptr ? GetLevelPackageName(*Level) : FString();
}

// ---------------------------------------------------------------------------
// Plan

FString FLevelPlanActor::Display() const
{
	if (bExisting)
	{
		return Path;
	}
	return BatchId.IsEmpty() ? FString::Printf(TEXT("(created by /ops/%d)"), CreatedByOp) : TEXT("@") + BatchId;
}

FLevelEditPlan::FLevelEditPlan(UWorld& InWorld)
	: World(InWorld)
{
}

void FLevelEditPlan::AddError(const FString& Path, const FString& Code, const FString& Message, TSharedPtr<FJsonObject> Details)
{
	Diagnostics.Add(MakeLevelDiagnostic(Path, Code, Message, MoveTemp(Details)));
}

void FLevelEditPlan::AddWarning(const FString& Path, const FString& Code, const FString& Message, TSharedPtr<FJsonObject> Details)
{
	FAssetDocumentDiagnostic Diagnostic = MakeLevelDiagnostic(Path, Code, Message, MoveTemp(Details));
	Diagnostic.Severity = TEXT("warning");
	Warnings.Add(MoveTemp(Diagnostic));
}

int32 FLevelEditPlan::Resolve(const FActorRef& Ref, const FString& Pointer)
{
	if (Ref.Kind == EActorRefKind::BatchId)
	{
		const int32* Found = ByBatchId.Find(Ref.Value);
		if (Found == nullptr)
		{
			AddError(Pointer, TEXT("Level.UnknownId"), FString::Printf(TEXT("'@%s' names no actor created by an earlier op of this "
				"request (an op's \"id\" defines it; ops run in order)."), *Ref.Value));
			return INDEX_NONE;
		}
		if (Actors[*Found].bDeleted)
		{
			AddError(Pointer, TEXT("Level.ActorDeletedEarlierInBatch"), FString::Printf(
				TEXT("'@%s' was deleted by an earlier op of this request."), *Ref.Value));
			return INDEX_NONE;
		}
		return *Found;
	}
	const FActorRefResolution Resolution = ResolveActorRef(World, Ref);
	if (!Resolution.bResolved)
	{
		AddError(Pointer, Resolution.Code, Resolution.Message,
			Resolution.Candidates.IsEmpty() ? nullptr : NamesDetails(TEXT("candidates"), Resolution.Candidates).ToSharedPtr());
		return INDEX_NONE;
	}
	RefPaths.Add(Pointer, Resolution.Path);
	const int32* Registered = ByPath.Find(Resolution.Path);
	if (Registered != nullptr)
	{
		if (Actors[*Registered].bDeleted)
		{
			AddError(Pointer, TEXT("Level.ActorDeletedEarlierInBatch"), FString::Printf(
				TEXT("'%s' was deleted by an earlier op of this request."), *Resolution.Path));
			return INDEX_NONE;
		}
		if (!Actors[*Registered].bRegisteredOnly)
		{
			return *Registered;
		}
	}
	AActor* Actor = Resolution.Actor.Get();
	if (!Resolution.bLoaded || Actor == nullptr)
	{
		AddError(Pointer, TEXT("Level.ActorNotLoaded"), FString::Printf(TEXT("'%s' is a World Partition actor that is not loaded. "
			"edit_level never loads actors: load its region or pin it in the editor first, then retry."), *Resolution.Path));
		return INDEX_NONE;
	}
	if (Actor->IsInLevelInstance() && !Actor->IsInEditLevelInstance())
	{
		AddError(Pointer, TEXT("Level.ActorInLevelInstance"), FString::Printf(TEXT("'%s' belongs to a Level Instance; its actors "
			"are edited in the instance's own level (Level Instance > Edit), not from the outer world."), *Resolution.Path));
		return INDEX_NONE;
	}
	if (FLevelUtils::IsLevelLocked(Actor->GetLevel()))
	{
		AddError(Pointer, TEXT("Level.LevelLocked"), FString::Printf(TEXT("'%s' is in level %s, which is locked in the Levels "
			"panel; unlock it first."), *Resolution.Path, *GetLevelPackageOf(*Actor)));
		return INDEX_NONE;
	}
	if (Registered != nullptr)
	{
		// Known only as a node of the attachment graph so far.
		Actors[*Registered].bRegisteredOnly = false;
		return *Registered;
	}
	FLevelPlanActor& Entry = Actors.AddDefaulted_GetRef();
	Entry.bExisting = true;
	Entry.Actor = Actor;
	Entry.Path = Resolution.Path;
	Entry.Class = Actor->GetClass();
	Entry.Level = Actor->GetLevel();
	const int32 Index = Actors.Num() - 1;
	ByPath.Add(Resolution.Path, Index);
	return Index;
}

int32 FLevelEditPlan::AddPlanned(int32 OpIndex, const FString& InBatchId, UClass& Class, ULevel& Level, AActor* Source)
{
	FLevelPlanActor& Entry = Actors.AddDefaulted_GetRef();
	Entry.BatchId = InBatchId;
	Entry.CreatedByOp = OpIndex;
	Entry.Class = &Class;
	Entry.Level = &Level;
	Entry.Source = Source;
	const int32 Index = Actors.Num() - 1;
	if (!InBatchId.IsEmpty())
	{
		ByBatchId.Add(InBatchId, Index);
	}
	return Index;
}

ULevel* FLevelEditPlan::ResolveTargetLevel(const FString& Package, const FString& Pointer)
{
	ULevel* Level = nullptr;
	if (Package.IsEmpty())
	{
		Level = World.GetCurrentLevel() != nullptr ? World.GetCurrentLevel() : World.PersistentLevel.Get();
	}
	else
	{
		for (ULevel* Loaded : GetLoadedLevels(World))
		{
			if (GetLevelPackageName(*Loaded) == Package)
			{
				Level = Loaded;
			}
		}
		if (Level == nullptr)
		{
			bool bKnown = false;
			for (const ULevelStreaming* Streaming : World.GetStreamingLevels())
			{
				bKnown |= Streaming != nullptr && Streaming->GetWorldAssetPackageName() == Package;
			}
			AddError(Pointer, bKnown ? TEXT("Level.LevelNotLoaded") : TEXT("Level.LevelNotFound"), bKnown
				? FString::Printf(TEXT("Level %s is a sublevel of this world but is not loaded; load it in the Levels panel first."), *Package)
				: FString::Printf(TEXT("No level %s in the editor world (query_level {\"what\": \"world\"} lists the levels)."), *Package));
			return nullptr;
		}
	}
	if (Level != nullptr && FLevelUtils::IsLevelLocked(Level))
	{
		AddError(Pointer, TEXT("Level.LevelLocked"), FString::Printf(TEXT("Level %s is locked in the Levels panel; unlock it first."),
			*GetLevelPackageName(*Level)));
		return nullptr;
	}
	return Level;
}

TFunction<UObject*(FStringView)> FLevelEditPlan::MakeBatchResolver() const
{
	// While an actor does not exist yet its class default object (or the source of a
	// duplicate) stands in for it: type checks run, its path is not stored anywhere.
	return [this](FStringView Id) -> UObject*
	{
		const int32* Found = ByBatchId.Find(FString(Id));
		if (Found == nullptr || Actors[*Found].bDeleted)
		{
			return nullptr;
		}
		const FLevelPlanActor& Entry = Actors[*Found];
		if (AActor* Source = Entry.Source.Get())
		{
			return Source;
		}
		UClass* Class = Entry.Class.Get();
		return Class != nullptr ? Class->GetDefaultObject() : nullptr;
	};
}

UObject* FLevelEditPlan::GetValueOwner(int32 ActorIndex, const FString& Component, const FString& Pointer)
{
	if (!Component.IsEmpty())
	{
		// Components earlier ops added (their class default object stands in) or removed.
		const bool bKnown = GetComponents(ActorIndex).ContainsByPredicate([&Component](const FLevelPlanComponent& Entry)
		{
			return Entry.Name.Equals(Component, ESearchCase::CaseSensitive) && (Entry.AddedByOp != INDEX_NONE || Entry.RemovedByOp != INDEX_NONE);
		});
		if (bKnown)
		{
			const FLevelPlanComponent* Planned = RequireComponent(ActorIndex, Component, Pointer);
			return Planned != nullptr ? GetComponentStandIn(ActorIndex, *Planned) : nullptr;
		}
	}
	const FLevelPlanActor& Entry = Actors[ActorIndex];
	AActor* Live = Entry.bExisting ? Entry.Actor.Get() : Entry.Source.Get();
	if (Live != nullptr)
	{
		if (Component.IsEmpty())
		{
			return Live;
		}
		UActorComponent* Found = FindLevelActorComponent(*Live, Component);
		if (Found == nullptr)
		{
			const TArray<FString> Names = ListLevelActorComponentNames(*Live);
			AddError(Pointer, TEXT("Level.ComponentNotFound"), FString::Printf(TEXT("%s has no component '%s' (component names come "
				"from query_level detail \"components\"; they are object names, not display names).%s"), *Entry.Display(), *Component,
				*UE::AssetDocument::DescribeNameCandidates(Component, Names, TEXT("Components"))), NamesDetails(TEXT("components"), Names));
			return nullptr;
		}
		const USceneComponent* Scene = Cast<USceneComponent>(Found);
		const USceneComponent* Parent = Scene != nullptr ? Scene->GetAttachParent() : nullptr;
		if (Found->CreationMethod == EComponentCreationMethod::UserConstructionScript
			|| !FComponentEditorUtils::CanEditComponentInstance(Found, Parent, false))
		{
			TSharedRef<FJsonObject> Details = MakeShared<FJsonObject>();
			Details->SetStringField(TEXT("creation"), DescribeLevelComponentCreation(Found->CreationMethod));
			AddError(Pointer, TEXT("Level.ComponentNotEditable"), Found->CreationMethod == EComponentCreationMethod::UserConstructionScript
				? FString::Printf(TEXT("Component '%s' of %s is made by the construction script; it is rebuilt on every change, so "
					"edits to it cannot be kept. Edit the Blueprint (its construction script or variables) instead."), *Component, *Entry.Display())
				: FString::Printf(TEXT("Component '%s' of %s is not editable on this actor (the Details panel shows it read-only)."),
					*Component, *Entry.Display()), Details);
			return nullptr;
		}
		return Found;
	}
	UClass* Class = Entry.Class.Get();
	if (Class == nullptr)
	{
		return nullptr;
	}
	if (Component.IsEmpty())
	{
		return Class->GetDefaultObject();
	}
	UActorComponent* Template = FindLevelClassComponentTemplate(*Class, Component);
	if (Template == nullptr)
	{
		const TArray<FString> Names = ListLevelClassComponentNames(*Class);
		AddError(Pointer, TEXT("Level.ComponentNotFound"), FString::Printf(TEXT("%s (%s) has no component '%s' (native and Blueprint "
			"components are named as query_level lists them; components a construction script creates exist only once the actor "
			"does: edit them in a later edit_level call).%s"), *Entry.Display(), *Class->GetPathName(), *Component,
			*UE::AssetDocument::DescribeNameCandidates(Component, Names, TEXT("Components"))), NamesDetails(TEXT("components"), Names));
		return nullptr;
	}
	if (Template->HasAnyFlags(RF_DefaultSubObject) && Template->GetOuter() == Class->GetDefaultObject()
		&& FComponentEditorUtils::GetPropertyForEditableNativeComponent(Template) == nullptr)
	{
		AddError(Pointer, TEXT("Level.ComponentNotEditable"), FString::Printf(TEXT("Component '%s' of %s is not editable on a "
			"placed actor (the Details panel shows it read-only)."), *Component, *Class->GetPathName()));
		return nullptr;
	}
	return Template;
}

void FLevelEditPlan::PreflightFields(int32 ActorIndex, const FLevelActorFieldEdits& Edits, const FString& OpPointer)
{
	const FString Display = Actors[ActorIndex].Display();
	const bool bExisting = Actors[ActorIndex].bExisting;
	AActor* Live = bExisting ? Actors[ActorIndex].Actor.Get() : Actors[ActorIndex].Source.Get();
	ObjectProperties::FReferencePolicy Policy = MakeLevelReferencePolicy(Actors[ActorIndex].Level.Get(), MakeBatchResolver());
	// "@id" of an actor the batch creates: its class default object stands in for its
	// value; the cross-level check uses the level it will be in.
	Policy.ResolveBatchLevel = [this](FStringView Id) -> const ULevel*
	{
		const int32* Found = ByBatchId.Find(FString(Id));
		return Found != nullptr ? Actors[*Found].Level.Get() : nullptr;
	};

	auto AddChange = [this, &Display](const FString& Op, const TCHAR* Object, const FString& Component, const FString& Property,
		TSharedPtr<FJsonValue> Before, TSharedPtr<FJsonValue> After)
	{
		FLevelChange& Change = Changes.AddDefaulted_GetRef();
		Change.Op = Op;
		Change.Kind = TEXT("Replace");
		Change.Object = Object;
		Change.Actor = Display;
		Change.Component = Component;
		Change.Property = Property;
		Change.Before = MoveTemp(Before);
		Change.After = MoveTemp(After);
	};

	if (Edits.Transform.IsSet())
	{
		const FLevelTransformEdit& Transform = Edits.Transform.GetValue();
		USceneComponent* Root = Live != nullptr ? Live->GetRootComponent() : nullptr;
		if (bExisting && Root == nullptr)
		{
			AddError(Transform.Pointer, TEXT("Level.InvalidRequest"), FString::Printf(TEXT("%s has no root component, so it has no "
				"transform."), *Display));
		}
		for (int32 Member = 0; Member < 3; ++Member)
		{
			if (!Transform.Members[Member].IsSet())
			{
				continue;
			}
			const ELevelTransformMember Kind = static_cast<ELevelTransformMember>(Member);
			FLevelValue Before;
			const bool bHasBefore = Root != nullptr && ObjectProperties::Read(*Root, FLevelTransformEdit::MemberProperty(Kind), Before);
			AddChange(Transform.Pointer + TEXT("/") + FLevelTransformEdit::MemberField(Kind), TEXT("transform"), FString(),
				FLevelTransformEdit::MemberField(Kind), bHasBefore ? LevelValueToJson(Before) : nullptr,
				LevelValueToJson(bHasBefore && !Transform.bWorld ? MergeAuthoredValue(Before, Transform.Members[Member].GetValue())
					: Transform.Members[Member].GetValue()));
		}
	}

	if (Edits.Label.IsSet())
	{
		FText Error;
		if (!FActorEditorUtils::ValidateActorName(FText::FromString(Edits.Label.GetValue()), Error))
		{
			AddError(Edits.LabelPointer, TEXT("Level.InvalidRequest"), FString::Printf(TEXT("'%s' is not a valid actor label: %s"),
				*Edits.Label.GetValue(), *Error.ToString()));
		}
		else if (bExisting && Live != nullptr && !Live->IsActorLabelEditable())
		{
			AddError(Edits.LabelPointer, TEXT("Level.InvalidRequest"), FString::Printf(TEXT("The label of %s cannot be edited."), *Display));
		}
		AddChange(Edits.LabelPointer, TEXT("label"), FString(), FString(),
			Live != nullptr ? MakeShared<FJsonValueString>(Live->GetActorLabel(false)).ToSharedPtr() : nullptr,
			MakeShared<FJsonValueString>(Edits.Label.GetValue()));
	}

	if (Edits.Folder.IsSet())
	{
		// SetFolderPath_Recursively creates the folder (and its parents).
		for (FString Folder = Edits.Folder.GetValue(); !Folder.IsEmpty(); )
		{
			GetFolders().Add(Folder);
			int32 Slash = INDEX_NONE;
			Folder = Folder.FindLastChar(TEXT('/'), Slash) ? Folder.Left(Slash) : FString();
		}
		const FName Folder = Live != nullptr ? Live->GetFolderPath() : NAME_None;
		AddChange(Edits.FolderPointer, TEXT("folder"), FString(), FString(),
			Live != nullptr ? MakeShared<FJsonValueString>(Folder.IsNone() ? FString() : Folder.ToString()).ToSharedPtr() : nullptr,
			MakeShared<FJsonValueString>(Edits.Folder.GetValue()));
	}

	auto PreflightProperty = [&](UObject& Owner, const FString& Name, const FLevelValue& Value, const FString& Pointer,
		const TCHAR* Object, const FString& Component, const FString& ChangeOp, const FString& ChangeProperty)
	{
		ObjectProperties::FPreparedWrite Prepared;
		TArray<FAssetDocumentRuntimeDiagnostic> Problems;
		if (!ObjectProperties::Decode(Owner, Name, Value, Policy, Prepared, Problems))
		{
			for (const FAssetDocumentRuntimeDiagnostic& Problem : Problems)
			{
				Diagnostics.Add(ProjectLevelPropertyDiagnostic(Problem, Pointer, Name));
			}
			if (Problems.IsEmpty())
			{
				AddError(Pointer, TEXT("PropertyWrite.DecodeFailed"), FString::Printf(TEXT("The value for '%s' is not valid."), *Name));
			}
			return;
		}
		const TSharedPtr<FJsonValue> Before = ReadValueJson(Owner, Name);
		FLevelValue BeforeValue;
		ObjectProperties::Read(Owner, Name, BeforeValue);
		const FLevelValue After = MergeAuthoredValue(BeforeValue, NormalizedAuthored(Owner, Name, Value));
		AddChange(ChangeOp, Object, Component, ChangeProperty, Before, LevelValueToJson(After));
		FLevelPlannedValue& Planned = PlannedValues.AddDefaulted_GetRef();
		Planned.ActorIndex = ActorIndex;
		Planned.Component = Component;
		Planned.Property = Name;
		Planned.After = LevelValueToJson(After);
		Planned.OpIndex = CurrentOp;
	};

	if (Edits.Tags.IsSet())
	{
		if (UObject* Owner = GetValueOwner(ActorIndex, FString(), Edits.TagsPointer))
		{
			PreflightProperty(*Owner, TEXT("Tags"), Edits.Tags.GetValue(), Edits.TagsPointer, TEXT("tags"), FString(),
				Edits.TagsPointer, FString());
		}
	}
	for (const FLevelPropertyEdit& Edit : Edits.Properties)
	{
		if (UObject* Owner = GetValueOwner(ActorIndex, FString(), Edit.Pointer))
		{
			PreflightProperty(*Owner, Edit.Name, Edit.Value, Edit.Pointer, TEXT("property"), FString(), Edit.Pointer, Edit.Name);
		}
	}
	for (const FLevelComponentEdit& Component : Edits.Components)
	{
		UObject* Owner = GetValueOwner(ActorIndex, Component.Name, Component.Pointer);
		if (Owner == nullptr)
		{
			continue;
		}
		for (const FLevelPropertyEdit& Edit : Component.Properties)
		{
			PreflightProperty(*Owner, Edit.Name, Edit.Value, Edit.Pointer, TEXT("property"), Component.Name, Edit.Pointer, Edit.Name);
		}
	}
	PreflightLayers(ActorIndex, Edits);
}

TArray<TSharedPtr<FJsonValue>> FLevelEditPlan::FindReferencers(AActor& Actor, const TSet<FString>& IgnoredPaths)
{
	TArray<TSharedPtr<FJsonValue>> Referencers;
	TArray<UClass*> IgnoredClasses;
	if (GUnrealEd != nullptr)
	{
		for (const TObjectPtr<UClass>& Class : GUnrealEd->ClassesToIgnoreDeleteReferenceWarning)
		{
			if (Class != nullptr)
			{
				IgnoredClasses.AddUnique(Class.Get());
			}
		}
	}
	IgnoredClasses.AddUnique(ALevelScriptActor::StaticClass());
	IgnoredClasses.AddUnique(AGroupActor::StaticClass());
	if (!bReferenceMapBuilt)
	{
		bReferenceMapBuilt = true;
		TMap<AActor*, TArray<AActor*>> Map;
		FBlueprintEditorUtils::GetActorReferenceMap(&World, IgnoredClasses, Map);
		for (const TPair<AActor*, TArray<AActor*>>& Pair : Map)
		{
			TArray<TWeakObjectPtr<AActor>>& Entry = ReferenceMap.Add(Pair.Key);
			for (AActor* Referencer : Pair.Value)
			{
				Entry.Add(Referencer);
			}
		}
	}
	auto Add = [&Referencers](const TCHAR* Kind, const FString& Path, const FString& Label)
	{
		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("kind"), Kind);
		Entry->SetStringField(TEXT("path"), Path);
		if (!Label.IsEmpty())
		{
			Entry->SetStringField(TEXT("label"), Label);
		}
		Referencers.Add(MakeShared<FJsonValueObject>(Entry));
	};

	if (const TArray<TWeakObjectPtr<AActor>>* Hard = ReferenceMap.Find(&Actor))
	{
		for (const TWeakObjectPtr<AActor>& Weak : *Hard)
		{
			AActor* Referencer = Weak.Get();
			if (Referencer == nullptr || Referencer == &Actor || Referencer->IsA<ALODActor>()
				|| IgnoredPaths.Contains(GetActorPath(*Referencer)))
			{
				continue;
			}
			const UChildActorComponent* ParentComponent = Referencer->GetParentComponent();
			if (ParentComponent != nullptr && ParentComponent->GetOwner() == &Actor)
			{
				continue;
			}
			Add(TEXT("actor"), GetActorPath(*Referencer), Referencer->GetActorLabel(false));
		}
	}

	ULevelScriptBlueprint* LevelScript = Actor.GetLevel() != nullptr ? Actor.GetLevel()->GetLevelScriptBlueprint(true) : nullptr;
	TArray<UK2Node*> Nodes;
	if (LevelScript != nullptr && FBlueprintEditorUtils::FindReferencesToActorFromLevelScript(LevelScript, &Actor, Nodes) && !Nodes.IsEmpty())
	{
		Add(TEXT("level_blueprint"), LevelScript->GetPathName(), FString());
	}

	TMap<FSoftObjectPath, TArray<UObject*>> Soft;
	FAssetToolsModule& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
	AssetTools.Get().FindSoftReferencesToObjects({FSoftObjectPath(&Actor)}, Soft);
	if (const TArray<UObject*>* Objects = Soft.Find(FSoftObjectPath(&Actor)))
	{
		for (UObject* Object : *Objects)
		{
			if (Object == nullptr || (Object->IsInOuter(&Actor) && Object->GetPackage() == Actor.GetPackage()))
			{
				continue;
			}
			const AActor* ReferencingActor = Cast<AActor>(Object);
			if (ReferencingActor != nullptr && IgnoredPaths.Contains(GetActorPath(*ReferencingActor)))
			{
				continue;
			}
			bool bIgnored = false;
			for (const UClass* Class : IgnoredClasses)
			{
				bIgnored |= Object->IsA(Class);
			}
			if (!bIgnored)
			{
				Add(TEXT("soft"), ReferencingActor != nullptr ? GetActorPath(*ReferencingActor) : Object->GetPathName(),
					ReferencingActor != nullptr ? ReferencingActor->GetActorLabel(false) : FString());
			}
		}
	}
	return Referencers;
}
}
