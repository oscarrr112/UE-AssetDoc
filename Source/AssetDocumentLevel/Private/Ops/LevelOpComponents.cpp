// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// add_component / remove_component / reparent_component: the Details panel's
// component tree for a placed actor (instance components), through
// USubobjectDataSubsystem as the panel does (design §2.3, §5.2).

#include "ComponentAssetBroker.h"
#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/Blueprint.h"
#include "GameFramework/Actor.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/ComponentEditorUtils.h"
#include "Kismet2/Kismet2NameValidators.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "LevelEditTypes.h"
#include "AssetDocumentNameSuggestions.h"
#include "LevelToolResult.h"
#include "SubobjectData.h"
#include "SubobjectDataSubsystem.h"
#include "UObject/Package.h"

namespace UE::AssetDocumentLevel::Private
{
UObject* LoadLevelSpawnSource(const FString& Path, bool bClass);
TSharedRef<FJsonObject> MakeLevelOpResult(const TCHAR* Kind, const FString& CreatedPath);

namespace
{
FString ComponentParentName(const UActorComponent& Component)
{
	const USceneComponent* Scene = Cast<USceneComponent>(&Component);
	const USceneComponent* Parent = Scene != nullptr ? Scene->GetAttachParent() : nullptr;
	return Parent != nullptr && Parent->GetOwner() == Component.GetOwner() ? Parent->GetName() : FString();
}

FString ComponentSocketName(const UActorComponent& Component)
{
	const USceneComponent* Scene = Cast<USceneComponent>(&Component);
	return Scene != nullptr && !Scene->GetAttachSocketName().IsNone() ? Scene->GetAttachSocketName().ToString() : FString();
}

// The subobject handle of the actor (the root of the Details component tree) and of
// one of its components.
bool GatherHandles(AActor& Actor, const UActorComponent* Component, FSubobjectDataHandle& OutActor, FSubobjectDataHandle& OutComponent)
{
	USubobjectDataSubsystem* Subsystem = USubobjectDataSubsystem::Get();
	if (Subsystem == nullptr)
	{
		return false;
	}
	TArray<FSubobjectDataHandle> Handles;
	Subsystem->GatherSubobjectData(&Actor, Handles);
	if (Handles.IsEmpty() || !Handles[0].IsValid())
	{
		return false;
	}
	OutActor = Handles[0];
	if (Component == nullptr)
	{
		return true;
	}
	OutComponent = Subsystem->FindHandleForObject(OutActor, Component);
	// A component a Blueprint's construction script made can be listed by its SCS node
	// (the Details tree's own lookup maps the node to the instance).
	for (int32 Index = 0; !OutComponent.IsValid() && Index < Handles.Num(); ++Index)
	{
		const FSubobjectData* Data = Handles[Index].GetData();
		if (Data != nullptr && Data->IsComponent() && (Data->GetObject() == Component || Data->FindComponentInstanceInActor(&Actor) == Component))
		{
			OutComponent = Handles[Index];
		}
	}
	if (!OutComponent.IsValid())
	{
		TArray<FString> Listed;
		for (const FSubobjectDataHandle& Handle : Handles)
		{
			const FSubobjectData* Data = Handle.GetData();
			Listed.Add(Data != nullptr && Data->GetObject() != nullptr ? Data->GetObject()->GetPathName() : FString(TEXT("<none>")));
		}
		UE_LOG(LogTemp, Verbose, TEXT("edit_level: %s is not in the Details component tree of %s: [%s]"), *Component->GetPathName(),
			*Actor.GetPathName(), *FString::Join(Listed, TEXT(", ")));
	}
	return OutComponent.IsValid();
}

TSharedRef<FJsonObject> CreationDetails(EComponentCreationMethod Creation)
{
	TSharedRef<FJsonObject> Details = MakeShared<FJsonObject>();
	Details->SetStringField(TEXT("creation"), DescribeLevelComponentCreation(Creation));
	return Details;
}

// Whether Name can be the new component's object name: the Details panel's rename
// rules (USubobjectDataSubsystem::IsValidRename) plus the Blueprint name validator the
// subsystem applies to instances of Blueprint actors.
bool CheckNewComponentName(FLevelEditPlan& Plan, int32 ActorIndex, UClass& Class, const FString& Name, const FString& Pointer)
{
	const FString Display = Plan.Actors[ActorIndex].Display();
	FString Sanitized = FBlueprintEditorUtils::ReplaceInvalidBlueprintNameCharactersInline(Name);
	if (Name.Len() >= NAME_SIZE || Sanitized != Name || !FName::IsValidXName(Name, INVALID_OBJECTNAME_CHARACTERS)
		|| !FComponentEditorUtils::IsValidVariableNameString(Class.GetDefaultObject<UActorComponent>(), Name))
	{
		Plan.AddError(Pointer, TEXT("Level.InvalidRequest"), FString::Printf(TEXT("'%s' is not a valid component name (letters, digits "
			"and '_'; not DefaultSceneRoot; not the <Class>_<number> form the engine generates)."), *Name));
		return false;
	}
	const bool bTaken = Plan.GetComponents(ActorIndex).ContainsByPredicate([&Name](const FLevelPlanComponent& Component)
	{
		return Component.Name.Equals(Name, ESearchCase::IgnoreCase);
	});
	AActor* StandIn = Plan.GetStandIn(ActorIndex);
	UClass* ActorClass = Plan.Actors[ActorIndex].Class.Get();
	const UBlueprint* Blueprint = ActorClass != nullptr ? Cast<UBlueprint>(ActorClass->ClassGeneratedBy) : nullptr;
	const bool bObjectTaken = Plan.Actors[ActorIndex].bExisting && StandIn != nullptr
		&& !FComponentEditorUtils::IsComponentNameAvailable(Name, StandIn);
	const bool bBlueprintTaken = Blueprint != nullptr && FKismetNameValidator(Blueprint).IsValid(Name) != EValidatorResult::Ok;
	if (bTaken || bObjectTaken || bBlueprintTaken)
	{
		Plan.AddError(Pointer, TEXT("Level.ComponentNameTaken"), FString::Printf(TEXT("%s already uses the name '%s' (%s); pick another "
			"component name."), *Display, *Name, bBlueprintTaken && !bTaken ? TEXT("a variable, function or component of its Blueprint")
			: TEXT("a component or subobject, possibly one removed earlier in this request")));
		return false;
	}
	return true;
}

// The parent of a scene component: a scene component of the actor that exists at this
// point of the batch.
const FLevelPlanComponent* RequireSceneParent(FLevelEditPlan& Plan, int32 ActorIndex, const FString& Parent, const FString& Pointer)
{
	const FLevelPlanComponent* Found = Plan.RequireComponent(ActorIndex, Parent, Pointer);
	if (Found != nullptr && !Found->bScene)
	{
		Plan.AddError(Pointer, TEXT("Level.InvalidRequest"), FString::Printf(TEXT("Component '%s' of %s is not a scene component, so "
			"nothing can be attached to it."), *Parent, *Plan.Actors[ActorIndex].Display()));
		return nullptr;
	}
	return Found;
}

bool CheckSocket(FLevelEditPlan& Plan, int32 ActorIndex, const FLevelPlanComponent& Parent, const FString& Socket, const FString& Pointer)
{
	if (Socket.IsEmpty() || Parent.AddedByOp != INDEX_NONE)
	{
		return true;
	}
	const USceneComponent* Owner = Cast<USceneComponent>(Plan.GetComponentStandIn(ActorIndex, Parent));
	if (Owner == nullptr || Owner->DoesSocketExist(FName(*Socket)))
	{
		return true;
	}
	TArray<TSharedPtr<FJsonValue>> Sockets;
	for (const FName& Name : Owner->GetAllSocketNames())
	{
		Sockets.Add(MakeShared<FJsonValueString>(Name.ToString()));
	}
	TSharedRef<FJsonObject> Details = MakeShared<FJsonObject>();
	Details->SetArrayField(TEXT("sockets"), Sockets);
	Plan.AddError(Pointer, TEXT("Level.AttachRejected"), FString::Printf(TEXT("Component '%s' of %s has no socket '%s'."), *Parent.Name,
		*Plan.Actors[ActorIndex].Display(), *Socket), Details);
	return false;
}

// Applies a parent socket to a scene component, as AttachToComponent with the
// relative transform kept (the Details panel has no socket row for instance
// components; this is what the Blueprint editor's socket picker stores).
bool ApplySocket(FLevelEditApply& Apply, USceneComponent& Component, const FString& Socket, const FString& Pointer)
{
	USceneComponent* Parent = Component.GetAttachParent();
	if (Parent == nullptr || !Parent->DoesSocketExist(FName(*Socket)))
	{
		Apply.AddError(Pointer, TEXT("Level.AttachRejected"), FString::Printf(TEXT("'%s' is not a socket of the parent component %s."),
			*Socket, Parent != nullptr ? *Parent->GetName() : TEXT("(none)")));
		return false;
	}
	Component.Modify();
	return Component.AttachToComponent(Parent, FAttachmentTransformRules::KeepRelativeTransform, FName(*Socket));
}

UClass* LoadComponentClass(const FString& Path)
{
	return Cast<UClass>(LoadLevelSpawnSource(Path, true));
}

// ---------------------------------------------------------------------------

class FLevelOpAddComponent final : public FLevelEditOp
{
public:
	FActorRef Target;
	FString ClassPath;
	FString Name;
	FString Parent;
	FString Socket;
	FString AssetPath;
	FLevelActorFieldEdits Edits;

	virtual const TCHAR* GetKind() const override { return TEXT("add_component"); }

	virtual void Preflight(FLevelEditPlan& Plan) const override
	{
		const int32 ActorIndex = Plan.Resolve(Target, Pointer + TEXT("/actor"));
		if (ActorIndex == INDEX_NONE)
		{
			return;
		}
		UClass* Class = LoadComponentClass(ClassPath);
		if (Class == nullptr || !Class->IsChildOf<UActorComponent>() || Class->HasAnyClassFlags(CLASS_Deprecated | CLASS_NewerVersionExists)
			|| (Class->ClassWithin != nullptr && Class->ClassWithin != UObject::StaticClass())
			|| !FKismetEditorUtilities::IsClassABlueprintSpawnableComponent(Class))
		{
			Plan.AddError(Pointer + TEXT("/class"), TEXT("Level.ComponentClassInvalid"), Class == nullptr
				? FString::Printf(TEXT("No component class '%s' (a native class is \"/Script/Engine.PointLightComponent\"; a Blueprint "
					"component is \"/Game/Path/BP_Name\")."), *ClassPath)
				: FString::Printf(TEXT("%s is not a component class the Add Component menu offers (a concrete, Blueprint-spawnable "
					"UActorComponent)."), *Class->GetPathName()));
			return;
		}
		const bool bScene = Class->IsChildOf<USceneComponent>();
		if (!bScene && (!Parent.IsEmpty() || !Socket.IsEmpty()))
		{
			Plan.AddError(Pointer + (Parent.IsEmpty() ? TEXT("/socket") : TEXT("/parent")), TEXT("Level.InvalidRequest"), FString::Printf(
				TEXT("%s is not a scene component: it has no parent or socket."), *Class->GetName()));
			return;
		}
		if (!CheckNewComponentName(Plan, ActorIndex, *Class, Name, Pointer + TEXT("/name")))
		{
			return;
		}
		FString ParentName;
		bool bRoot = false;
		if (bScene)
		{
			const FLevelPlanComponent* ParentEntry = nullptr;
			if (!Parent.IsEmpty())
			{
				ParentEntry = RequireSceneParent(Plan, ActorIndex, Parent, Pointer + TEXT("/parent"));
				if (ParentEntry == nullptr)
				{
					return;
				}
			}
			else
			{
				ParentEntry = Plan.GetComponents(ActorIndex).FindByPredicate([](const FLevelPlanComponent& Entry)
				{
					return Entry.bRoot && Entry.RemovedByOp == INDEX_NONE;
				});
				bRoot = ParentEntry == nullptr;
			}
			if (ParentEntry != nullptr)
			{
				ParentName = ParentEntry->Name;
				if (!CheckSocket(Plan, ActorIndex, *ParentEntry, Socket, Pointer + TEXT("/socket")))
				{
					return;
				}
			}
			else if (!Socket.IsEmpty())
			{
				Plan.AddError(Pointer + TEXT("/socket"), TEXT("Level.InvalidRequest"), FString::Printf(TEXT("%s has no root component, so "
					"the new component becomes the root and has no parent socket."), *Plan.Actors[ActorIndex].Display()));
				return;
			}
		}
		if (!AssetPath.IsEmpty())
		{
			UObject* Asset = LoadLevelSpawnSource(AssetPath, false);
			if (Asset == nullptr)
			{
				Plan.AddError(Pointer + TEXT("/asset"), TEXT("Level.AssetNotFound"), FString::Printf(TEXT("No asset '%s'."), *AssetPath));
				return;
			}
			bool bSupported = false;
			TArray<FString> Supported;
			for (UClass* AssetClass : FComponentAssetBrokerage::GetSupportedAssets(Class))
			{
				bSupported |= AssetClass != nullptr && Asset->IsA(AssetClass);
				if (AssetClass != nullptr)
				{
					Supported.Add(AssetClass->GetName());
				}
			}
			if (!bSupported)
			{
				Plan.AddError(Pointer + TEXT("/asset"), TEXT("Level.InvalidRequest"), Supported.IsEmpty()
					? FString::Printf(TEXT("%s takes no asset; set its properties instead."), *Class->GetName())
					: FString::Printf(TEXT("%s takes a %s asset, not a %s."), *Class->GetName(), *FString::Join(Supported, TEXT(" or ")),
						*Asset->GetClass()->GetName()));
				return;
			}
		}

		FLevelPlanComponent& Entry = Plan.GetComponents(ActorIndex).AddDefaulted_GetRef();
		Entry.Name = Name;
		Entry.Class = Class;
		Entry.Parent = ParentName;
		Entry.Socket = Socket;
		Entry.Creation = EComponentCreationMethod::Instance;
		Entry.bScene = bScene;
		Entry.bRoot = bRoot;
		Entry.bEditable = true;
		Entry.AddedByOp = Index;

		FLevelChange& Change = Plan.Changes.AddDefaulted_GetRef();
		Change.Op = Pointer;
		Change.Kind = TEXT("Add");
		Change.Object = TEXT("component");
		Change.Actor = Plan.Actors[ActorIndex].Display();
		Change.Component = Name;
		Change.Extra = MakeShared<FJsonObject>();
		Change.Extra->SetStringField(TEXT("class"), Class->GetPathName());
		if (!ParentName.IsEmpty())
		{
			Change.Extra->SetStringField(TEXT("parent"), ParentName);
		}
		if (!Socket.IsEmpty())
		{
			Change.Extra->SetStringField(TEXT("socket"), Socket);
		}
		Plan.PreflightFields(ActorIndex, Edits, Pointer);
	}

	virtual bool Apply(FLevelEditApply& Apply) const override
	{
		AActor* Actor = Apply.ResolveRef(Target, Pointer + TEXT("/actor"));
		if (Actor == nullptr)
		{
			return false;
		}
		const FString Path = GetActorPath(*Actor);
		UClass* Class = LoadComponentClass(ClassPath);
		UObject* Asset = AssetPath.IsEmpty() ? nullptr : LoadLevelSpawnSource(AssetPath, false);
		UActorComponent* ParentComponent = Parent.IsEmpty() ? nullptr : FindLevelActorComponent(*Actor, Parent);
		USubobjectDataSubsystem* Subsystem = USubobjectDataSubsystem::Get();
		FSubobjectDataHandle ActorHandle;
		FSubobjectDataHandle ParentHandle;
		const TCHAR* Missing = Class == nullptr ? TEXT("the component class") : Subsystem == nullptr ? TEXT("the component tree")
			: (!Parent.IsEmpty() && ParentComponent == nullptr) ? TEXT("the parent component")
			: !GatherHandles(*Actor, ParentComponent, ActorHandle, ParentHandle) ? TEXT("the parent in the Details component tree")
			: nullptr;
		if (Missing != nullptr)
		{
			Apply.AddError(Pointer, TEXT("Level.ComponentNotFound"), FString::Printf(TEXT("%s of %s is no longer available."), Missing,
				*Path));
			return false;
		}
		FAddNewSubobjectParams Params;
		Params.ParentHandle = ParentComponent != nullptr ? ParentHandle : ActorHandle;
		Params.NewClass = Class;
		Params.AssetOverride = Asset;
		Params.BlueprintContext = nullptr;
		FText FailReason;
		const FSubobjectDataHandle NewHandle = Subsystem->AddNewSubobject(Params, FailReason);
		const FSubobjectData* NewData = NewHandle.GetData();
		UActorComponent* Created = NewData != nullptr ? const_cast<UActorComponent*>(NewData->GetComponentTemplate()) : nullptr;
		if (Created == nullptr)
		{
			Apply.AddError(Pointer, TEXT("Level.ComponentClassInvalid"), FString::Printf(TEXT("The editor did not add a %s to %s%s"),
				*Class->GetName(), *Path, FailReason.IsEmpty() ? TEXT(".") : *(TEXT(": ") + FailReason.ToString())));
			return false;
		}
		if (!Created->GetName().Equals(Name, ESearchCase::CaseSensitive))
		{
			Subsystem->RenameSubobject(NewHandle, FText::FromString(Name));
		}
		Actor = Apply.ResolvePath(Path);
		Created = Actor != nullptr ? FindLevelActorComponent(*Actor, Name) : nullptr;
		if (Created == nullptr)
		{
			Apply.AddError(Pointer + TEXT("/name"), TEXT("Level.ComponentNameTaken"), FString::Printf(TEXT("The editor did not give the "
				"new component the name '%s' (the name is in use on %s)."), *Name, *Path));
			return false;
		}
		if (!Socket.IsEmpty())
		{
			USceneComponent* Scene = Cast<USceneComponent>(Created);
			if (Scene == nullptr || !ApplySocket(Apply, *Scene, Socket, Pointer + TEXT("/socket")))
			{
				return false;
			}
		}
		if (Asset != nullptr && FComponentAssetBrokerage::GetAssetFromComponent(Created) != Asset)
		{
			Apply.AddError(Pointer + TEXT("/asset"), TEXT("Level.VerificationMismatch"), FString::Printf(TEXT("The new component does not "
				"use %s after the add."), *AssetPath));
			return false;
		}
		Apply.Touch(*Actor);
		++Apply.ComponentsChanged;
		const FString CreatedParent = ComponentParentName(*Created);
		FLevelChange& Change = Apply.AddChange(Pointer, TEXT("Add"), TEXT("component"), Path);
		Change.Component = Name;
		Change.Extra = MakeShared<FJsonObject>();
		Change.Extra->SetStringField(TEXT("class"), Class->GetPathName());
		if (!CreatedParent.IsEmpty())
		{
			Change.Extra->SetStringField(TEXT("parent"), CreatedParent);
		}
		if (!Socket.IsEmpty())
		{
			Change.Extra->SetStringField(TEXT("socket"), Socket);
		}
		Apply.ExpectComponent(Path, Name, Class, CreatedParent, Socket, Pointer);
		TSharedRef<FJsonObject> Result = MakeLevelOpResult(GetKind(), FString());
		Result->SetStringField(TEXT("actor"), Path);
		Result->SetStringField(TEXT("component"), Name);
		Apply.OpResults[Index] = Result;
		return Apply.ApplyFields(Path, Edits, Pointer);
	}
};

class FLevelOpRemoveComponent final : public FLevelEditOp
{
public:
	FActorRef Target;
	FString Name;

	virtual const TCHAR* GetKind() const override { return TEXT("remove_component"); }

	virtual void Preflight(FLevelEditPlan& Plan) const override
	{
		const int32 ActorIndex = Plan.Resolve(Target, Pointer + TEXT("/actor"));
		FLevelPlanComponent* Found = ActorIndex != INDEX_NONE ? Plan.RequireComponent(ActorIndex, Name, Pointer + TEXT("/component")) : nullptr;
		if (Found == nullptr)
		{
			return;
		}
		if (Found->Creation != EComponentCreationMethod::Instance)
		{
			Plan.AddError(Pointer + TEXT("/component"), TEXT("Level.ComponentNotRemovable"), FString::Printf(TEXT("Component '%s' of %s "
				"comes from its class (%s); only components added to this actor (instance components) can be removed, as in the Details "
				"panel. Change the class or Blueprint instead."), *Name, *Plan.Actors[ActorIndex].Display(),
				DescribeLevelComponentCreation(Found->Creation)), CreationDetails(Found->Creation));
			return;
		}
		// DeleteComponents promotes the children of a removed scene component.
		const FString OldParent = Found->Parent;
		const bool bRoot = Found->bRoot;
		Found->RemovedByOp = Index;
		for (FLevelPlanComponent& Other : Plan.GetComponents(ActorIndex))
		{
			if (Other.RemovedByOp == INDEX_NONE && Other.Parent == Name)
			{
				Other.Parent = OldParent;
				if (bRoot && OldParent.IsEmpty())
				{
					Other.bRoot = true;
					break;
				}
			}
		}
		FLevelChange& Change = Plan.Changes.AddDefaulted_GetRef();
		Change.Op = Pointer;
		Change.Kind = TEXT("Remove");
		Change.Object = TEXT("component");
		Change.Actor = Plan.Actors[ActorIndex].Display();
		Change.Component = Name;
	}

	virtual bool Apply(FLevelEditApply& Apply) const override
	{
		AActor* Actor = Apply.ResolveRef(Target, Pointer + TEXT("/actor"));
		if (Actor == nullptr)
		{
			return false;
		}
		const FString Path = GetActorPath(*Actor);
		UActorComponent* Component = FindLevelActorComponent(*Actor, Name);
		FSubobjectDataHandle ActorHandle;
		FSubobjectDataHandle Handle;
		if (Component == nullptr || !GatherHandles(*Actor, Component, ActorHandle, Handle))
		{
			Apply.AddError(Pointer + TEXT("/component"), TEXT("Level.ComponentNotFound"), FString::Printf(TEXT("%s has no component '%s'.%s"),
				*Path, *Name, *UE::AssetDocument::DescribeNameCandidates(Name, ListLevelActorComponentNames(*Actor), TEXT("Components"))));
			return false;
		}
		const FSubobjectData* Data = Handle.GetData();
		if (Component->CreationMethod != EComponentCreationMethod::Instance || Data == nullptr || !Data->CanDelete())
		{
			Apply.AddError(Pointer + TEXT("/component"), TEXT("Level.ComponentNotRemovable"), FString::Printf(TEXT("The editor does not "
				"remove component '%s' of %s."), *Name, *Path), CreationDetails(Component->CreationMethod));
			return false;
		}
		const FString OldParent = ComponentParentName(*Component);
		if (USubobjectDataSubsystem::Get()->DeleteSubobject(ActorHandle, Handle) < 1)
		{
			Apply.AddError(Pointer, TEXT("Level.ComponentNotRemovable"), FString::Printf(TEXT("The editor did not remove component '%s' of "
				"%s."), *Name, *Path));
			return false;
		}
		if (AActor* After = Apply.ResolvePath(Path))
		{
			Apply.Touch(*After);
		}
		++Apply.ComponentsChanged;
		Apply.ForgetComponent(Path, Name, OldParent);
		Apply.ExpectComponentGone(Path, Name, Pointer);
		FLevelChange& Change = Apply.AddChange(Pointer, TEXT("Remove"), TEXT("component"), Path);
		Change.Component = Name;
		TSharedRef<FJsonObject> Result = MakeLevelOpResult(GetKind(), FString());
		Result->SetStringField(TEXT("actor"), Path);
		Apply.OpResults[Index] = Result;
		return true;
	}
};

class FLevelOpReparentComponent final : public FLevelEditOp
{
public:
	FActorRef Target;
	FString Name;
	FString Parent;
	FString Socket;

	virtual const TCHAR* GetKind() const override { return TEXT("reparent_component"); }

	virtual void Preflight(FLevelEditPlan& Plan) const override
	{
		const int32 ActorIndex = Plan.Resolve(Target, Pointer + TEXT("/actor"));
		FLevelPlanComponent* Found = ActorIndex != INDEX_NONE ? Plan.RequireComponent(ActorIndex, Name, Pointer + TEXT("/component")) : nullptr;
		if (Found == nullptr)
		{
			return;
		}
		const FString Display = Plan.Actors[ActorIndex].Display();
		if (!Found->bScene || Found->bRoot || Found->Creation != EComponentCreationMethod::Instance)
		{
			Plan.AddError(Pointer + TEXT("/component"), TEXT("Level.ComponentNotEditable"), !Found->bScene
				? FString::Printf(TEXT("Component '%s' of %s is not a scene component; it has no parent."), *Name, *Display)
				: Found->bRoot ? FString::Printf(TEXT("Component '%s' is the root of %s; the root is not reparented."), *Name, *Display)
				: FString::Printf(TEXT("Component '%s' of %s comes from its class (%s); only instance components are reparented (as in "
					"the Details panel)."), *Name, *Display, DescribeLevelComponentCreation(Found->Creation)), CreationDetails(Found->Creation));
			return;
		}
		const FLevelPlanComponent* NewParent = RequireSceneParent(Plan, ActorIndex, Parent, Pointer + TEXT("/parent"));
		if (NewParent == nullptr)
		{
			return;
		}
		// A component cannot become the child of itself or of its own descendant.
		FString Current = Parent;
		for (int32 Guard = 0; !Current.IsEmpty() && Guard < 1000; ++Guard)
		{
			if (Current == Name)
			{
				Plan.AddError(Pointer + TEXT("/parent"), TEXT("Level.AttachRejected"), FString::Printf(TEXT("'%s' is '%s' or attached below "
					"it; a component cannot be attached to itself or its own child."), *Parent, *Name));
				return;
			}
			const FLevelPlanComponent* Step = Plan.FindComponent(ActorIndex, Current);
			Current = Step != nullptr ? Step->Parent : FString();
		}
		if (!CheckSocket(Plan, ActorIndex, *NewParent, Socket, Pointer + TEXT("/socket")))
		{
			return;
		}
		FLevelChange& Change = Plan.Changes.AddDefaulted_GetRef();
		Change.Op = Pointer;
		Change.Kind = TEXT("Replace");
		Change.Object = TEXT("component");
		Change.Actor = Display;
		Change.Component = Name;
		Change.Property = TEXT("parent");
		Change.Before = MakeShared<FJsonValueString>(Found->Parent);
		Change.After = MakeShared<FJsonValueString>(Parent);
		// Found may move when RequireComponent grows the array; look it up again.
		if (FLevelPlanComponent* Moved = Plan.FindComponent(ActorIndex, Name))
		{
			Moved->Parent = Parent;
			Moved->Socket = Socket;
		}
	}

	virtual bool Apply(FLevelEditApply& Apply) const override
	{
		AActor* Actor = Apply.ResolveRef(Target, Pointer + TEXT("/actor"));
		if (Actor == nullptr)
		{
			return false;
		}
		const FString Path = GetActorPath(*Actor);
		USceneComponent* Component = Cast<USceneComponent>(FindLevelActorComponent(*Actor, Name));
		USceneComponent* NewParent = Cast<USceneComponent>(FindLevelActorComponent(*Actor, Parent));
		FSubobjectDataHandle ActorHandle;
		FSubobjectDataHandle Handle;
		FSubobjectDataHandle ParentHandle;
		if (Component == nullptr || NewParent == nullptr || !GatherHandles(*Actor, Component, ActorHandle, Handle)
			|| !GatherHandles(*Actor, NewParent, ActorHandle, ParentHandle))
		{
			Apply.AddError(Pointer, TEXT("Level.ComponentNotFound"), FString::Printf(TEXT("Component '%s' or '%s' of %s is no longer "
				"available."), *Name, *Parent, *Path));
			return false;
		}
		const FSubobjectData* Data = Handle.GetData();
		if (Data == nullptr || !Data->CanReparent())
		{
			Apply.AddError(Pointer + TEXT("/component"), TEXT("Level.ComponentNotEditable"), FString::Printf(TEXT("The editor does not "
				"reparent component '%s' of %s."), *Name, *Path), CreationDetails(Component->CreationMethod));
			return false;
		}
		const FString Before = ComponentParentName(*Component);
		// The component keeps its world transform: transform values written to it
		// earlier in this batch are checked now (they were relative to the old parent).
		if (!Apply.SettleComponentTransform(Path, Name))
		{
			return false;
		}
		FReparentSubobjectParams Params;
		Params.NewParentHandle = ParentHandle;
		if (!USubobjectDataSubsystem::Get()->ReparentSubobject(Params, Handle))
		{
			Apply.AddError(Pointer, TEXT("Level.AttachRejected"), FString::Printf(TEXT("The editor did not reparent '%s' to '%s'."), *Name,
				*Parent));
			return false;
		}
		Actor = Apply.ResolvePath(Path);
		Component = Actor != nullptr ? Cast<USceneComponent>(FindLevelActorComponent(*Actor, Name)) : nullptr;
		if (Component == nullptr)
		{
			Apply.AddError(Pointer, TEXT("Level.VerificationMismatch"), FString::Printf(TEXT("Component '%s' of %s is gone after the "
				"reparent."), *Name, *Path));
			return false;
		}
		if (!Socket.IsEmpty() && !ApplySocket(Apply, *Component, Socket, Pointer + TEXT("/socket")))
		{
			return false;
		}
		Apply.Touch(*Actor);
		++Apply.ComponentsChanged;
		FLevelChange& Change = Apply.AddChange(Pointer, TEXT("Replace"), TEXT("component"), Path);
		Change.Component = Name;
		Change.Property = TEXT("parent");
		Change.Before = MakeShared<FJsonValueString>(Before);
		Change.After = MakeShared<FJsonValueString>(Parent);
		Apply.ExpectComponent(Path, Name, Component->GetClass(), Parent, Socket, Pointer);
		TSharedRef<FJsonObject> Result = MakeLevelOpResult(GetKind(), FString());
		Result->SetStringField(TEXT("actor"), Path);
		Apply.OpResults[Index] = Result;
		return true;
	}
};

bool ReadRequiredString(const FJsonObject& Json, const TCHAR* Field, const FString& Pointer, const TCHAR* Meaning, FString& Out,
	TArray<FAssetDocumentDiagnostic>& OutDiagnostics)
{
	if (!Json.TryGetStringField(Field, Out) || Out.IsEmpty())
	{
		OutDiagnostics.Add(MakeInvalidRequest(Pointer + TEXT("/") + Field, FString::Printf(TEXT("'%s' (%s) is a required non-empty "
			"string."), Field, Meaning)));
		return false;
	}
	return true;
}

bool ReadOptionalString(const FJsonObject& Json, const TCHAR* Field, const FString& Pointer, FString& Out,
	TArray<FAssetDocumentDiagnostic>& OutDiagnostics)
{
	if (Json.HasField(Field) && (!Json.TryGetStringField(Field, Out) || Out.IsEmpty()))
	{
		OutDiagnostics.Add(MakeInvalidRequest(Pointer + TEXT("/") + Field, FString::Printf(TEXT("'%s' is a non-empty string."), Field)));
		return false;
	}
	return true;
}

TUniquePtr<FLevelEditOp> ParseAddComponent(const FJsonObject& Json, const FString& Pointer, TArray<FAssetDocumentDiagnostic>& OutDiagnostics)
{
	static const TCHAR* const Fields[] = {TEXT("actor"), TEXT("class"), TEXT("name"), TEXT("parent"), TEXT("socket"), TEXT("asset"),
		TEXT("properties")};
	const int32 Before = OutDiagnostics.Num();
	CheckLevelOpFields(Json, Pointer, Fields, OutDiagnostics);
	TUniquePtr<FLevelOpAddComponent> Op = MakeUnique<FLevelOpAddComponent>();
	ParseLevelActorRefField(Json, TEXT("actor"), Pointer, true, Op->Target, OutDiagnostics);
	ReadRequiredString(Json, TEXT("class"), Pointer, TEXT("the component class path"), Op->ClassPath, OutDiagnostics);
	ReadRequiredString(Json, TEXT("name"), Pointer, TEXT("the new component's name"), Op->Name, OutDiagnostics);
	ReadOptionalString(Json, TEXT("parent"), Pointer, Op->Parent, OutDiagnostics);
	ReadOptionalString(Json, TEXT("socket"), Pointer, Op->Socket, OutDiagnostics);
	ReadOptionalString(Json, TEXT("asset"), Pointer, Op->AssetPath, OutDiagnostics);
	if (Json.HasField(TEXT("properties")) && !Op->Name.IsEmpty())
	{
		// The new component's values: the field edits' components form.
		TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
		Body->SetField(TEXT("properties"), Json.TryGetField(TEXT("properties")));
		TSharedRef<FJsonObject> Components = MakeShared<FJsonObject>();
		Components->SetObjectField(Op->Name, Body);
		FJsonObject Wrapper;
		Wrapper.SetObjectField(TEXT("components"), Components);
		TArray<FAssetDocumentDiagnostic> Nested;
		ParseLevelActorFieldEdits(Wrapper, Pointer, Op->Edits, Nested);
		const FString Prefix = Pointer + TEXT("/components/") + EscapePointerSegment(Op->Name);
		for (FAssetDocumentDiagnostic& Diagnostic : Nested)
		{
			Diagnostic.Path = Pointer + Diagnostic.Path.RightChop(Prefix.Len());
			OutDiagnostics.Add(MoveTemp(Diagnostic));
		}
		for (FLevelComponentEdit& Component : Op->Edits.Components)
		{
			Component.Pointer = Pointer;
			for (FLevelPropertyEdit& Edit : Component.Properties)
			{
				Edit.Pointer = Pointer + Edit.Pointer.RightChop(Prefix.Len());
			}
		}
	}
	return OutDiagnostics.Num() == Before ? TUniquePtr<FLevelEditOp>(MoveTemp(Op)) : nullptr;
}

TUniquePtr<FLevelEditOp> ParseRemoveComponent(const FJsonObject& Json, const FString& Pointer,
	TArray<FAssetDocumentDiagnostic>& OutDiagnostics)
{
	static const TCHAR* const Fields[] = {TEXT("actor"), TEXT("component")};
	const int32 Before = OutDiagnostics.Num();
	CheckLevelOpFields(Json, Pointer, Fields, OutDiagnostics);
	TUniquePtr<FLevelOpRemoveComponent> Op = MakeUnique<FLevelOpRemoveComponent>();
	ParseLevelActorRefField(Json, TEXT("actor"), Pointer, true, Op->Target, OutDiagnostics);
	ReadRequiredString(Json, TEXT("component"), Pointer, TEXT("the component name"), Op->Name, OutDiagnostics);
	return OutDiagnostics.Num() == Before ? TUniquePtr<FLevelEditOp>(MoveTemp(Op)) : nullptr;
}

TUniquePtr<FLevelEditOp> ParseReparentComponent(const FJsonObject& Json, const FString& Pointer,
	TArray<FAssetDocumentDiagnostic>& OutDiagnostics)
{
	static const TCHAR* const Fields[] = {TEXT("actor"), TEXT("component"), TEXT("parent"), TEXT("socket")};
	const int32 Before = OutDiagnostics.Num();
	CheckLevelOpFields(Json, Pointer, Fields, OutDiagnostics);
	TUniquePtr<FLevelOpReparentComponent> Op = MakeUnique<FLevelOpReparentComponent>();
	ParseLevelActorRefField(Json, TEXT("actor"), Pointer, true, Op->Target, OutDiagnostics);
	ReadRequiredString(Json, TEXT("component"), Pointer, TEXT("the component name"), Op->Name, OutDiagnostics);
	ReadRequiredString(Json, TEXT("parent"), Pointer, TEXT("the new parent component's name"), Op->Parent, OutDiagnostics);
	ReadOptionalString(Json, TEXT("socket"), Pointer, Op->Socket, OutDiagnostics);
	return OutDiagnostics.Num() == Before ? TUniquePtr<FLevelEditOp>(MoveTemp(Op)) : nullptr;
}

const FLevelEditOpRegistration AddComponentRegistration(TEXT("add_component"), &ParseAddComponent);
const FLevelEditOpRegistration RemoveComponentRegistration(TEXT("remove_component"), &ParseRemoveComponent);
const FLevelEditOpRegistration ReparentComponentRegistration(TEXT("reparent_component"), &ParseReparentComponent);
}
}
