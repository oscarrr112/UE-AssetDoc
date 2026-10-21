// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// The parts of the symbolic plan (design §3.2) that model actors beyond their
// existence: component sets, the attachment graph, Outliner folders, and the planned
// property values the delete reference check reads.

#include "LevelEditTypes.h"
#include "AssetDocumentNameSuggestions.h"

#include "ActorFolder.h"
#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "EditorActorFolders.h"
#include "Engine/Brush.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/Level.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/World.h"
#include "Misc/EngineVersionComparison.h"
#include "Folder.h"
#include "GameFramework/Actor.h"
#include "Kismet2/ComponentEditorUtils.h"
#include "LevelToolResult.h"
#include "LevelWorldIndex.h"
#include "UObject/UnrealType.h"
#include "WorldPartition/DataLayer/DataLayerAsset.h"
#include "WorldPartition/DataLayer/DataLayerInstance.h"
#include "WorldPartition/DataLayer/DataLayerManager.h"

#define LOCTEXT_NAMESPACE "AssetDocumentLevelAttach"

namespace UE::AssetDocumentLevel::Private
{
namespace
{
void GatherActorComponents(AActor& Actor, bool bTemplate, TArray<FLevelPlanComponent>& Out)
{
	TInlineComponentArray<UActorComponent*> Components(&Actor);
	for (UActorComponent* Component : Components)
	{
		if (Component == nullptr || !IsValid(Component) || Component->IsVisualizationComponent())
		{
			continue;
		}
		FLevelPlanComponent& Entry = Out.AddDefaulted_GetRef();
		Entry.Name = Component->GetName();
		Entry.Class = Component->GetClass();
		Entry.Creation = Component->CreationMethod;
		const USceneComponent* Scene = Cast<USceneComponent>(Component);
		Entry.bScene = Scene != nullptr;
		Entry.bRoot = Scene != nullptr && Actor.GetRootComponent() == Scene;
		const USceneComponent* Parent = Scene != nullptr ? Scene->GetAttachParent() : nullptr;
		if (Parent != nullptr && Parent->GetOwner() == &Actor)
		{
			Entry.Parent = Parent->GetName();
			Entry.Socket = Scene->GetAttachSocketName().IsNone() ? FString() : Scene->GetAttachSocketName().ToString();
		}
		Entry.bEditable = bTemplate
			? !(Component->HasAnyFlags(RF_DefaultSubObject) && FComponentEditorUtils::GetPropertyForEditableNativeComponent(Component) == nullptr)
			: Component->CreationMethod != EComponentCreationMethod::UserConstructionScript
				&& FComponentEditorUtils::CanEditComponentInstance(Component, Parent, false);
	}
}

// The components a class creates: native default subobjects and the Blueprint
// components along the class chain (construction-script components are unknown
// until the actor exists).
void GatherClassComponents(UClass& Class, TArray<FLevelPlanComponent>& Out)
{
	AActor* Default = Class.GetDefaultObject<AActor>();
	if (Default != nullptr)
	{
		GatherActorComponents(*Default, true, Out);
	}
	const bool bNativeRoot = Default != nullptr && Default->GetRootComponent() != nullptr;
	const FString NativeRootName = bNativeRoot ? Default->GetRootComponent()->GetName() : FString();
	TArray<const USimpleConstructionScript*> Scripts;
	for (UClass* Current = &Class; Current != nullptr; Current = Current->GetSuperClass())
	{
		const UBlueprintGeneratedClass* Generated = Cast<UBlueprintGeneratedClass>(Current);
		if (Generated != nullptr && Generated->SimpleConstructionScript != nullptr)
		{
			Scripts.Insert(Generated->SimpleConstructionScript, 0);
		}
	}
	bool bHasRoot = bNativeRoot;
	for (const USimpleConstructionScript* Script : Scripts)
	{
		for (USCS_Node* Node : Script->GetAllNodes())
		{
			if (Node == nullptr || Node->ComponentTemplate == nullptr)
			{
				continue;
			}
			const FString Name = Node->GetVariableName().ToString();
			if (Out.ContainsByPredicate([&Name](const FLevelPlanComponent& Existing) { return Existing.Name == Name; }))
			{
				continue;
			}
			FLevelPlanComponent& Entry = Out.AddDefaulted_GetRef();
			Entry.Name = Name;
			Entry.Class = Node->ComponentTemplate->GetClass();
			Entry.Creation = EComponentCreationMethod::SimpleConstructionScript;
			Entry.bScene = Node->ComponentTemplate->IsA<USceneComponent>();
			if (!Entry.bScene)
			{
				continue;
			}
			if (const USCS_Node* ParentNode = Script->FindParentNode(Node))
			{
				Entry.Parent = ParentNode->GetVariableName().ToString();
			}
			else if (Node->ParentComponentOrVariableName != NAME_None)
			{
				Entry.Parent = Node->ParentComponentOrVariableName.ToString();
			}
			else if (bHasRoot)
			{
				Entry.Parent = NativeRootName.IsEmpty() ? FString() : NativeRootName;
			}
			if (!bHasRoot && Entry.Parent.IsEmpty())
			{
				Entry.bRoot = true;
				bHasRoot = true;
			}
			Entry.Socket = Node->AttachToName.IsNone() ? FString() : Node->AttachToName.ToString();
		}
	}
}

// Whether a JSON value (a planned property value) names the actor: its path, or
// "@id" for an actor the batch creates.
bool JsonNames(const TSharedPtr<FJsonValue>& Value, const FString& Path, const FString& BatchRef)
{
	if (!Value.IsValid())
	{
		return false;
	}
	FString String;
	if (Value->TryGetString(String))
	{
		return (!Path.IsEmpty() && String == Path) || (!BatchRef.IsEmpty() && String == BatchRef);
	}
	const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
	if (Value->TryGetArray(Array) && Array != nullptr)
	{
		for (const TSharedPtr<FJsonValue>& Element : *Array)
		{
			if (JsonNames(Element, Path, BatchRef))
			{
				return true;
			}
		}
		return false;
	}
	const TSharedPtr<FJsonObject>* Object = nullptr;
	if (Value->TryGetObject(Object) && Object != nullptr && Object->IsValid())
	{
		for (const auto& Member : (*Object)->Values)
		{
			if (JsonNames(Member.Value, Path, BatchRef))
			{
				return true;
			}
		}
	}
	return false;
}

// The top-level properties of Owner whose value (at any depth) references Target.
TArray<FString> FindReferencingProperties(UObject& Owner, const AActor& Target)
{
	TArray<FString> Names;
	TArray<const FProperty*> Chain;
	for (TPropertyValueIterator<FObjectPropertyBase> It(Owner.GetClass(), &Owner, EPropertyValueIteratorFlags::FullRecursion); It; ++It)
	{
		const UObject* Referenced = It.Key()->GetObjectPropertyValue(It.Value());
		// As FBlueprintEditorUtils::GetActorReferenceMap: references to the actor itself.
		if (Referenced != &Target)
		{
			continue;
		}
		Chain.Reset();
		It.GetPropertyChain(Chain);
		if (!Chain.IsEmpty() && Chain.Last() != nullptr)
		{
			Names.AddUnique(Chain.Last()->GetName());
		}
	}
	return Names;
}
}

const TCHAR* DescribeLevelComponentCreation(EComponentCreationMethod Method)
{
	switch (Method)
	{
	case EComponentCreationMethod::Native: return TEXT("Native");
	case EComponentCreationMethod::SimpleConstructionScript: return TEXT("Blueprint");
	case EComponentCreationMethod::UserConstructionScript: return TEXT("ConstructionScript");
	case EComponentCreationMethod::Instance:
	default: return TEXT("Instance");
	}
}

FString GetLevelFolderText(const AActor& Actor)
{
	const FName Folder = Actor.GetFolderPath();
	return Folder.IsNone() ? FString() : Folder.ToString();
}

TSet<FString> GatherLevelFolders(UWorld& World)
{
	TSet<FString> Folders;
	FActorFolders::Get().ForEachFolderWithRootObject(World, FFolder::GetWorldRootFolder(&World).GetRootObject(), [&Folders](const FFolder& Folder)
	{
		if (!Folder.IsNone())
		{
			Folders.Add(Folder.GetPath().ToString());
		}
		return true;
	});
	return Folders;
}

UDataLayerInstance* FindLevelDataLayerInstance(UWorld& World, const FString& Reference, FString& OutError, TArray<FString>* OutKnown)
{
	UDataLayerManager* Manager = UDataLayerManager::GetDataLayerManager(&World);
	if (Manager == nullptr)
	{
		OutError = TEXT("This world has no data layers (it is not a World Partition world).");
		return nullptr;
	}
	TArray<UDataLayerInstance*> Matches;
	TArray<FString> Known;
	Manager->ForEachDataLayerInstance([&](UDataLayerInstance* Instance)
	{
		if (Instance == nullptr)
		{
			return true;
		}
		const UDataLayerAsset* Asset = Instance->GetAsset();
		const FString AssetPath = Asset != nullptr && Asset->IsAsset() ? Asset->GetPathName() : FString();
		Known.Add(AssetPath.IsEmpty() ? Instance->GetDataLayerShortName() : AssetPath);
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
		const FName InstanceName = Instance->GetFName();
#else
		const FName InstanceName = Instance->GetDataLayerFName();
#endif
		if ((!AssetPath.IsEmpty() && AssetPath == Reference) || InstanceName.ToString() == Reference
			|| Instance->GetDataLayerShortName() == Reference || Instance->GetDataLayerFullName() == Reference)
		{
			Matches.AddUnique(Instance);
		}
		return true;
	});
	Known.Sort();
	if (OutKnown != nullptr)
	{
		*OutKnown = Known;
	}
	if (Matches.Num() == 1)
	{
		return Matches[0];
	}
	OutError = Matches.IsEmpty()
		? FString::Printf(TEXT("No data layer '%s' in this world (a data layer asset path, or the name of a data layer instance; "
			"query_level {\"what\": \"world\"} lists them)."), *Reference)
		: FString::Printf(TEXT("'%s' names %d data layers; use the data layer asset path."), *Reference, Matches.Num());
	return nullptr;
}

// ---------------------------------------------------------------------------

int32 FLevelEditPlan::FindOrAddExisting(AActor& Actor)
{
	const FString Path = GetActorPath(Actor);
	if (const int32* Found = ByPath.Find(Path))
	{
		return *Found;
	}
	FLevelPlanActor& Entry = Actors.AddDefaulted_GetRef();
	Entry.bExisting = true;
	Entry.bRegisteredOnly = true;
	Entry.Actor = &Actor;
	Entry.Path = Path;
	Entry.Class = Actor.GetClass();
	Entry.Level = Actor.GetLevel();
	const int32 Index = Actors.Num() - 1;
	ByPath.Add(Path, Index);
	return Index;
}

int32 FLevelEditPlan::FindByPath(const FString& Path) const
{
	const int32* Found = ByPath.Find(Path);
	return Found != nullptr ? *Found : INDEX_NONE;
}

bool FLevelEditPlan::IsDeletedPath(const FString& Path) const
{
	const int32* Found = ByPath.Find(Path);
	return Found != nullptr && Actors[*Found].bDeleted;
}

AActor* FLevelEditPlan::GetStandIn(int32 ActorIndex) const
{
	const FLevelPlanActor& Entry = Actors[ActorIndex];
	if (AActor* Live = Entry.bExisting ? Entry.Actor.Get() : Entry.Source.Get())
	{
		return Live;
	}
	UClass* Class = Entry.Class.Get();
	return Class != nullptr ? Class->GetDefaultObject<AActor>() : nullptr;
}

TArray<FLevelPlanComponent>& FLevelEditPlan::GetComponents(int32 ActorIndex)
{
	FLevelPlanActor& Entry = Actors[ActorIndex];
	if (!Entry.bComponentsKnown)
	{
		Entry.bComponentsKnown = true;
		if (AActor* Live = Entry.bExisting ? Entry.Actor.Get() : Entry.Source.Get())
		{
			GatherActorComponents(*Live, false, Entry.Components);
		}
		else if (UClass* Class = Entry.Class.Get())
		{
			GatherClassComponents(*Class, Entry.Components);
		}
	}
	return Entry.Components;
}

FLevelPlanComponent* FLevelEditPlan::FindComponent(int32 ActorIndex, const FString& Name)
{
	return GetComponents(ActorIndex).FindByPredicate([&Name](const FLevelPlanComponent& Component)
	{
		return Component.RemovedByOp == INDEX_NONE && Component.Name.Equals(Name, ESearchCase::CaseSensitive);
	});
}

FLevelPlanComponent* FLevelEditPlan::RequireComponent(int32 ActorIndex, const FString& Name, const FString& Pointer)
{
	if (FLevelPlanComponent* Found = FindComponent(ActorIndex, Name))
	{
		return Found;
	}
	const FLevelPlanActor& Entry = Actors[ActorIndex];
	TArray<FString> Names;
	int32 RemovedBy = INDEX_NONE;
	for (const FLevelPlanComponent& Component : GetComponents(ActorIndex))
	{
		if (Component.RemovedByOp == INDEX_NONE)
		{
			Names.Add(Component.Name);
		}
		else if (Component.Name.Equals(Name, ESearchCase::CaseSensitive))
		{
			RemovedBy = Component.RemovedByOp;
		}
	}
	Names.Sort();
	TArray<TSharedPtr<FJsonValue>> Values;
	for (const FString& Known : Names)
	{
		Values.Add(MakeShared<FJsonValueString>(Known));
	}
	TSharedRef<FJsonObject> Details = MakeShared<FJsonObject>();
	Details->SetArrayField(TEXT("components"), Values);
	const bool bPlanned = !Entry.bExisting && Entry.Source == nullptr;
	AddError(Pointer, TEXT("Level.ComponentNotFound"), RemovedBy != INDEX_NONE
		? FString::Printf(TEXT("Component '%s' of %s was removed by /ops/%d of this request."), *Name, *Entry.Display(), RemovedBy)
		: FString::Printf(TEXT("%s has no component '%s' (component names come from query_level detail \"components\"; they are object "
			"names, not display names).%s%s"), *Entry.Display(), *Name, *UE::AssetDocument::DescribeNameCandidates(Name, Names,
			TEXT("Components")), bPlanned ? TEXT(" Components a construction script creates exist only once the actor does: edit them in "
			"a later edit_level call.") : TEXT("")), Details);
	return nullptr;
}

UObject* FLevelEditPlan::GetComponentStandIn(int32 ActorIndex, const FLevelPlanComponent& Component)
{
	UClass* ComponentClass = Component.Class.Get();
	if (Component.AddedByOp != INDEX_NONE)
	{
		return ComponentClass != nullptr ? ComponentClass->GetDefaultObject() : nullptr;
	}
	const FLevelPlanActor& Entry = Actors[ActorIndex];
	if (AActor* Live = Entry.bExisting ? Entry.Actor.Get() : Entry.Source.Get())
	{
		return FindLevelActorComponent(*Live, Component.Name);
	}
	UClass* Class = Entry.Class.Get();
	return Class != nullptr ? FindLevelClassComponentTemplate(*Class, Component.Name) : nullptr;
}

int32 FLevelEditPlan::GetParent(int32 ActorIndex)
{
	if (!Actors[ActorIndex].bParentKnown)
	{
		Actors[ActorIndex].bParentKnown = true;
		AActor* Live = Actors[ActorIndex].bExisting ? Actors[ActorIndex].Actor.Get() : nullptr;
		AActor* LiveParent = Live != nullptr ? Live->GetAttachParentActor() : nullptr;
		if (LiveParent != nullptr)
		{
			const FName Socket = Live->GetAttachParentSocketName();
			const int32 ParentIndex = FindOrAddExisting(*LiveParent);
			Actors[ActorIndex].ParentIndex = ParentIndex;
			Actors[ActorIndex].ParentSocket = Socket.IsNone() ? FString() : Socket.ToString();
		}
	}
	return Actors[ActorIndex].ParentIndex;
}

bool FLevelEditPlan::PreflightAttach(int32 Child, int32 Parent, const FString& Socket, const FString& Component, const FString& Pointer)
{
	const FString ChildDisplay = Actors[Child].Display();
	const FString ParentDisplay = Actors[Parent].Display();
	auto Reject = [&](const FText& Reason)
	{
		TSharedRef<FJsonObject> Details = MakeShared<FJsonObject>();
		Details->SetStringField(TEXT("reason"), Reason.ToString());
		AddError(Pointer, TEXT("Level.AttachRejected"), FString::Printf(TEXT("%s cannot be attached to %s: %s"), *ChildDisplay,
			*ParentDisplay, *Reason.ToString()), Details);
		return false;
	};
	if (Child == Parent)
	{
		return Reject(LOCTEXT("AttachToSelf", "Cannot attach actor to self."));
	}

	AActor* ChildLive = Actors[Child].bExisting ? Actors[Child].Actor.Get() : nullptr;
	AActor* ParentLive = Actors[Parent].bExisting ? Actors[Parent].Actor.Get() : nullptr;
	if (ChildLive != nullptr && ParentLive != nullptr && GEditor != nullptr)
	{
		// Both exist: the editor's own check, except its cycle test, which reads the
		// attachments before this batch (the plan graph below has the current ones).
		FText Reason;
		if (!GEditor->CanParentActors(ParentLive, ChildLive, &Reason))
		{
			const USceneComponent* ChildRoot = ChildLive->GetRootComponent();
			const USceneComponent* ParentRoot = ParentLive->GetDefaultAttachComponent();
			if (ChildRoot == nullptr || ParentRoot == nullptr || !ParentRoot->IsAttachedTo(ChildRoot))
			{
				return Reject(Reason);
			}
		}
	}
	else
	{
		// An actor the batch creates: the same checks on the plan (class default object
		// or duplicate source standing in for it).
		AActor* ChildStandIn = GetStandIn(Child);
		AActor* ParentStandIn = GetStandIn(Parent);
		const FLevelPlanComponent* ChildRoot = GetComponents(Child).FindByPredicate([](const FLevelPlanComponent& Entry)
		{
			return Entry.bRoot && Entry.RemovedByOp == INDEX_NONE;
		});
		const FLevelPlanComponent* ParentRoot = GetComponents(Parent).FindByPredicate([](const FLevelPlanComponent& Entry)
		{
			return Entry.bRoot && Entry.RemovedByOp == INDEX_NONE;
		});
		if (ChildStandIn == nullptr || ParentStandIn == nullptr || ChildRoot == nullptr || ParentRoot == nullptr)
		{
			return Reject(LOCTEXT("AttachWithoutRoot", "Cannot attach actors without root components."));
		}
		const ABrush* ParentBrush = Cast<const ABrush>(ParentStandIn);
		const ABrush* ChildBrush = Cast<const ABrush>(ChildStandIn);
		if ((ParentBrush != nullptr && !ParentBrush->IsVolumeBrush()) || (ChildBrush != nullptr && !ChildBrush->IsVolumeBrush()))
		{
			return Reject(LOCTEXT("AttachBrush", "BSP Brushes cannot be attached"));
		}
		FText Reason;
		if (!ParentStandIn->EditorCanAttachFrom(ChildStandIn, Reason) || !ChildStandIn->EditorCanAttachTo(ParentStandIn, Reason))
		{
			return Reject(Reason.IsEmpty() ? FText::Format(LOCTEXT("AttachRefused", "{0} cannot be attached to {1}"),
				FText::FromString(ChildDisplay), FText::FromString(ParentDisplay)) : Reason);
		}
		const USceneComponent* ChildRootObject = Cast<USceneComponent>(GetComponentStandIn(Child, *ChildRoot));
		const USceneComponent* ParentRootObject = Cast<USceneComponent>(GetComponentStandIn(Parent, *ParentRoot));
		if (ChildRootObject != nullptr && ParentRootObject != nullptr && ChildRootObject->Mobility == EComponentMobility::Static
			&& ParentRootObject->Mobility != EComponentMobility::Static)
		{
			FFormatNamedArguments Arguments;
			Arguments.Add(TEXT("StaticActor"), FText::FromString(ChildDisplay));
			Arguments.Add(TEXT("DynamicActor"), FText::FromString(ParentDisplay));
			Arguments.Add(TEXT("DynamicActorMobility"), ParentRootObject->Mobility == EComponentMobility::Stationary
				? LOCTEXT("MobilityStationary", "Stationary") : LOCTEXT("MobilityMovable", "Movable"));
			return Reject(FText::Format(LOCTEXT("AttachStaticToDynamic", "Cannot attach actor with Static mobility "
				"({StaticActor}) to actor with {DynamicActorMobility} mobility ({DynamicActor})."), Arguments));
		}
		if (Actors[Child].Level != Actors[Parent].Level)
		{
			return Reject(LOCTEXT("AttachAcrossLevels", "Actors need to be in the same level!"));
		}
	}

	// Cycle, on the attachment graph after the earlier ops.
	int32 Guard = 0;
	for (int32 Current = Parent; Current != INDEX_NONE && Guard < 10000; Current = GetParent(Current), ++Guard)
	{
		if (Current == Child)
		{
			return Reject(LOCTEXT("AttachCycle", "Parent cannot become the child of their descendant"));
		}
	}

	// The parent component and socket.
	const FLevelPlanComponent* Target = nullptr;
	if (!Component.IsEmpty())
	{
		Target = RequireComponent(Parent, Component, Pointer + TEXT("/component"));
		if (Target == nullptr)
		{
			return false;
		}
		if (!Target->bScene)
		{
			AddError(Pointer + TEXT("/component"), TEXT("Level.AttachRejected"), FString::Printf(TEXT("Component '%s' of %s is not a "
				"scene component; actors attach to scene components."), *Component, *ParentDisplay));
			return false;
		}
	}
	else
	{
		Target = GetComponents(Parent).FindByPredicate([](const FLevelPlanComponent& Entry)
		{
			return Entry.bRoot && Entry.RemovedByOp == INDEX_NONE;
		});
	}
	if (!Socket.IsEmpty() && Target != nullptr && Target->AddedByOp == INDEX_NONE)
	{
		const USceneComponent* SocketOwner = Cast<USceneComponent>(GetComponentStandIn(Parent, *Target));
		if (SocketOwner != nullptr && !SocketOwner->DoesSocketExist(FName(*Socket)))
		{
			TArray<TSharedPtr<FJsonValue>> Sockets;
			for (const FName& Name : SocketOwner->GetAllSocketNames())
			{
				Sockets.Add(MakeShared<FJsonValueString>(Name.ToString()));
			}
			TSharedRef<FJsonObject> Details = MakeShared<FJsonObject>();
			Details->SetArrayField(TEXT("sockets"), Sockets);
			AddError(Pointer + TEXT("/socket"), TEXT("Level.AttachRejected"), FString::Printf(TEXT("Component '%s' of %s has no socket "
				"'%s'."), *Target->Name, *ParentDisplay, *Socket), Details);
			return false;
		}
	}

	Actors[Child].bParentKnown = true;
	Actors[Child].ParentIndex = Parent;
	Actors[Child].ParentSocket = Socket;
	return true;
}

TSet<FString>& FLevelEditPlan::GetFolders()
{
	if (!bFoldersKnown)
	{
		bFoldersKnown = true;
		Folders = GatherLevelFolders(World);
	}
	return Folders;
}

// ---------------------------------------------------------------------------
// Delete reference check against the earlier ops.

TArray<TSharedPtr<FJsonValue>> FLevelEditPlan::FindPlannedReferencers(int32 Target, const TSet<FString>& IgnoredPaths)
{
	TArray<TSharedPtr<FJsonValue>> Referencers;
	const FString TargetPath = Actors[Target].bExisting ? Actors[Target].Path : FString();
	const FString TargetRef = Actors[Target].BatchId.IsEmpty() ? FString() : TEXT("@") + Actors[Target].BatchId;
	TSet<int32> Added;
	// The newest planned value of each (actor, component, property).
	TMap<FString, const FLevelPlannedValue*> Newest;
	for (const FLevelPlannedValue& Value : PlannedValues)
	{
		Newest.Add(FString::Printf(TEXT("%d|%s|%s"), Value.ActorIndex, *Value.Component, *Value.Property), &Value);
	}
	for (const TPair<FString, const FLevelPlannedValue*>& Pair : Newest)
	{
		const FLevelPlannedValue& Value = *Pair.Value;
		const FLevelPlanActor& Entry = Actors[Value.ActorIndex];
		if (Value.ActorIndex == Target || Entry.bDeleted || Added.Contains(Value.ActorIndex)
			|| (Entry.bExisting && IgnoredPaths.Contains(Entry.Path)) || !JsonNames(Value.After, TargetPath, TargetRef))
		{
			continue;
		}
		if (!Value.Component.IsEmpty() && FindComponent(Value.ActorIndex, Value.Component) == nullptr)
		{
			continue;
		}
		Added.Add(Value.ActorIndex);
		TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("kind"), TEXT("actor"));
		Json->SetStringField(TEXT("path"), Entry.Display());
		Json->SetStringField(TEXT("set_by"), FString::Printf(TEXT("/ops/%d"), Value.OpIndex));
		Referencers.Add(MakeShared<FJsonValueObject>(Json));
	}
	// Duplicates of an actor that references the target copy the reference.
	AActor* TargetActor = Actors[Target].bExisting ? Actors[Target].Actor.Get() : nullptr;
	for (int32 Index = 0; TargetActor != nullptr && Index < Actors.Num(); ++Index)
	{
		const FLevelPlanActor& Entry = Actors[Index];
		AActor* Source = Entry.Source.Get();
		if (Entry.bExisting || Entry.bDeleted || Source == nullptr || Added.Contains(Index) || !StillReferences(Index, *Source, Target, false))
		{
			continue;
		}
		Added.Add(Index);
		TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("kind"), TEXT("actor"));
		Json->SetStringField(TEXT("path"), Entry.Display());
		Json->SetStringField(TEXT("duplicate_of"), GetActorPath(*Source));
		Referencers.Add(MakeShared<FJsonValueObject>(Json));
	}
	return Referencers;
}

bool FLevelEditPlan::StillReferences(int32 Referencer, AActor& LiveReferencer, int32 Target, bool bIfNoneFound)
{
	AActor* TargetActor = Actors[Target].bExisting ? Actors[Target].Actor.Get() : nullptr;
	if (TargetActor == nullptr)
	{
		return bIfNoneFound;
	}
	const FString TargetPath = Actors[Target].Path;
	const FString TargetRef = Actors[Target].BatchId.IsEmpty() ? FString() : TEXT("@") + Actors[Target].BatchId;
	auto Overwritten = [&](const FString& Component, const FString& Property)
	{
		const FLevelPlannedValue* Last = nullptr;
		for (const FLevelPlannedValue& Value : PlannedValues)
		{
			if (Value.ActorIndex == Referencer && Value.Component == Component && Value.Property == Property)
			{
				Last = &Value;
			}
		}
		return Last != nullptr && !JsonNames(Last->After, TargetPath, TargetRef);
	};
	// A reference held anywhere but in a property the batch can overwrite (or one this
	// scan does not see) keeps the actor a referencer.
	bool bFound = false;
	for (const FString& Property : FindReferencingProperties(LiveReferencer, *TargetActor))
	{
		bFound = true;
		if (!Overwritten(FString(), Property))
		{
			return true;
		}
	}
	TInlineComponentArray<UActorComponent*> Components(&LiveReferencer);
	for (UActorComponent* Component : Components)
	{
		if (Component == nullptr || !IsValid(Component))
		{
			continue;
		}
		const FString Name = Component->GetName();
		const TArray<FString> Properties = FindReferencingProperties(*Component, *TargetActor);
		if (Properties.IsEmpty())
		{
			continue;
		}
		bFound = true;
		if (FindComponent(Referencer, Name) == nullptr)
		{
			continue;
		}
		for (const FString& Property : Properties)
		{
			if (!Overwritten(Name, Property))
			{
				return true;
			}
		}
	}
	return bFound ? false : bIfNoneFound;
}
}

#undef LOCTEXT_NAMESPACE
