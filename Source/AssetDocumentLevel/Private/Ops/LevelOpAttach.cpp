// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// attach / detach: the Outliner's drag onto a parent actor (UEditorEngine::ParentActors,
// after CanParentActors) and Detach (Raw/LevelDetach). Both keep the world transform
// and move the child, with its own attached children, to the folder of the topmost
// parent, as the editor does (design §2.3, §5.2).

#include "Components/SceneComponent.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "GameFramework/Actor.h"
#include "LevelEditTypes.h"
#include "AssetDocumentNameSuggestions.h"
#include "Raw/LevelDetach.h"

namespace UE::AssetDocumentLevel::Private
{
TSharedRef<FJsonObject> MakeLevelOpResult(const TCHAR* Kind, const FString& CreatedPath);

bool ApplyLevelAttach(FLevelEditApply& Apply, AActor& Child, AActor& Parent, const FString& Socket, const FString& Component,
	const FString& Pointer)
{
	const FString ChildPath = GetActorPath(Child);
	const FString ParentPath = GetActorPath(Parent);
	USceneComponent* AttachComponent = nullptr;
	if (!Component.IsEmpty())
	{
		AttachComponent = Cast<USceneComponent>(FindLevelActorComponent(Parent, Component));
		if (AttachComponent == nullptr)
		{
			Apply.AddError(Pointer + TEXT("/component"), TEXT("Level.ComponentNotFound"), FString::Printf(TEXT("%s has no scene component "
				"'%s'.%s"), *ParentPath, *Component, *UE::AssetDocument::DescribeNameCandidates(Component, ListLevelActorComponentNames(Parent),
				TEXT("Components"))));
			return false;
		}
	}
	FText Reason;
	if (GEditor == nullptr || !GEditor->CanParentActors(&Parent, &Child, &Reason))
	{
		TSharedRef<FJsonObject> Details = MakeShared<FJsonObject>();
		Details->SetStringField(TEXT("reason"), Reason.ToString());
		Apply.AddError(Pointer, TEXT("Level.AttachRejected"), FString::Printf(TEXT("%s cannot be attached to %s: %s"), *ChildPath,
			*ParentPath, *Reason.ToString()), Details);
		return false;
	}
	USceneComponent* SocketOwner = AttachComponent != nullptr ? AttachComponent : Parent.GetDefaultAttachComponent();
	if (!Socket.IsEmpty() && (SocketOwner == nullptr || !SocketOwner->DoesSocketExist(FName(*Socket))))
	{
		Apply.AddError(Pointer + TEXT("/socket"), TEXT("Level.AttachRejected"), FString::Printf(TEXT("'%s' is not a socket of %s."), *Socket,
			SocketOwner != nullptr ? *SocketOwner->GetName() : *ParentPath));
		return false;
	}
	const AActor* OldParent = Child.GetAttachParentActor();
	const FString Before = OldParent != nullptr ? GetActorPath(*OldParent) : FString();
	if (!Apply.SettleBeforeReparent(Child, Pointer))
	{
		return false;
	}
	GEditor->ParentActors(&Parent, &Child, Socket.IsEmpty() ? FName(NAME_None) : FName(*Socket), AttachComponent);
	AActor* ChildAfter = Apply.ResolvePath(ChildPath);
	if (ChildAfter == nullptr || ChildAfter->GetAttachParentActor() == nullptr
		|| GetActorPath(*ChildAfter->GetAttachParentActor()) != ParentPath)
	{
		Apply.AddError(Pointer, TEXT("Level.AttachRejected"), FString::Printf(TEXT("The editor did not attach %s to %s."), *ChildPath,
			*ParentPath));
		return false;
	}
	Apply.Touch(*ChildAfter);
	++Apply.Attachments;
	FLevelChange& Change = Apply.AddChange(Pointer, TEXT("Replace"), TEXT("attachment"), ChildPath);
	if (Before.IsEmpty())
	{
		Change.Before = MakeShared<FJsonValueNull>();
	}
	else
	{
		Change.Before = MakeShared<FJsonValueString>(Before);
	}
	Change.After = MakeShared<FJsonValueString>(ParentPath);
	if (!Socket.IsEmpty() || !Component.IsEmpty())
	{
		Change.Extra = MakeShared<FJsonObject>();
		if (!Socket.IsEmpty())
		{
			Change.Extra->SetStringField(TEXT("socket"), Socket);
		}
		if (!Component.IsEmpty())
		{
			Change.Extra->SetStringField(TEXT("component"), Component);
		}
	}
	Apply.ExpectAttachment(ChildPath, ParentPath, Socket, Component, Pointer);
	return true;
}

namespace
{
class FLevelOpAttach final : public FLevelEditOp
{
public:
	FActorRef Target;
	FActorRef Parent;
	FString Socket;
	FString Component;

	virtual const TCHAR* GetKind() const override { return TEXT("attach"); }

	virtual void Preflight(FLevelEditPlan& Plan) const override
	{
		const int32 Child = Plan.Resolve(Target, Pointer + TEXT("/actor"));
		const int32 ParentIndex = Plan.Resolve(Parent, Pointer + TEXT("/parent"));
		if (Child == INDEX_NONE || ParentIndex == INDEX_NONE)
		{
			return;
		}
		const int32 OldParent = Plan.GetParent(Child);
		if (!Plan.PreflightAttach(Child, ParentIndex, Socket, Component, Pointer))
		{
			return;
		}
		FLevelChange& Change = Plan.Changes.AddDefaulted_GetRef();
		Change.Op = Pointer;
		Change.Kind = TEXT("Replace");
		Change.Object = TEXT("attachment");
		Change.Actor = Plan.Actors[Child].Display();
		if (OldParent != INDEX_NONE)
		{
			Change.Before = MakeShared<FJsonValueString>(Plan.Actors[OldParent].Display());
		}
		else
		{
			Change.Before = MakeShared<FJsonValueNull>();
		}
		Change.After = MakeShared<FJsonValueString>(Plan.Actors[ParentIndex].Display());
	}

	virtual bool Apply(FLevelEditApply& Apply) const override
	{
		AActor* Child = Apply.ResolveRef(Target, Pointer + TEXT("/actor"));
		AActor* ParentActor = Child != nullptr ? Apply.ResolveRef(Parent, Pointer + TEXT("/parent")) : nullptr;
		if (Child == nullptr || ParentActor == nullptr)
		{
			return false;
		}
		const FString Path = GetActorPath(*Child);
		TSharedRef<FJsonObject> Result = MakeLevelOpResult(GetKind(), FString());
		Result->SetStringField(TEXT("actor"), Path);
		Apply.OpResults[Index] = Result;
		return ApplyLevelAttach(Apply, *Child, *ParentActor, Socket, Component, Pointer);
	}
};

class FLevelOpDetach final : public FLevelEditOp
{
public:
	FActorRef Target;

	virtual const TCHAR* GetKind() const override { return TEXT("detach"); }

	virtual void Preflight(FLevelEditPlan& Plan) const override
	{
		const int32 Child = Plan.Resolve(Target, Pointer + TEXT("/actor"));
		if (Child == INDEX_NONE)
		{
			return;
		}
		const int32 OldParent = Plan.GetParent(Child);
		Plan.Actors[Child].ParentIndex = INDEX_NONE;
		Plan.Actors[Child].ParentSocket.Reset();
		if (OldParent == INDEX_NONE)
		{
			// As the editor's Detach: an actor that is not attached is left alone.
			return;
		}
		FLevelChange& Change = Plan.Changes.AddDefaulted_GetRef();
		Change.Op = Pointer;
		Change.Kind = TEXT("Replace");
		Change.Object = TEXT("attachment");
		Change.Actor = Plan.Actors[Child].Display();
		Change.Before = MakeShared<FJsonValueString>(Plan.Actors[OldParent].Display());
		Change.After = MakeShared<FJsonValueNull>();
	}

	virtual bool Apply(FLevelEditApply& Apply) const override
	{
		AActor* Child = Apply.ResolveRef(Target, Pointer + TEXT("/actor"));
		if (Child == nullptr)
		{
			return false;
		}
		const FString Path = GetActorPath(*Child);
		TSharedRef<FJsonObject> Result = MakeLevelOpResult(GetKind(), FString());
		Result->SetStringField(TEXT("actor"), Path);
		Apply.OpResults[Index] = Result;
		const AActor* OldParent = Child->GetAttachParentActor();
		if (OldParent == nullptr)
		{
			Apply.ExpectAttachment(Path, FString(), FString(), FString(), Pointer);
			return true;
		}
		const FString Before = GetActorPath(*OldParent);
		if (!Apply.SettleBeforeReparent(*Child, Pointer))
		{
			return false;
		}
		DetachLevelActor(*Child);
		if (AActor* After = Apply.ResolvePath(Path))
		{
			Apply.Touch(*After);
		}
		++Apply.Attachments;
		FLevelChange& Change = Apply.AddChange(Pointer, TEXT("Replace"), TEXT("attachment"), Path);
		Change.Before = MakeShared<FJsonValueString>(Before);
		Change.After = MakeShared<FJsonValueNull>();
		Apply.ExpectAttachment(Path, FString(), FString(), FString(), Pointer);
		return true;
	}
};

TUniquePtr<FLevelEditOp> ParseAttach(const FJsonObject& Json, const FString& Pointer, TArray<FAssetDocumentDiagnostic>& OutDiagnostics)
{
	static const TCHAR* const Fields[] = {TEXT("actor"), TEXT("parent"), TEXT("socket"), TEXT("component")};
	const int32 Before = OutDiagnostics.Num();
	CheckLevelOpFields(Json, Pointer, Fields, OutDiagnostics);
	TUniquePtr<FLevelOpAttach> Op = MakeUnique<FLevelOpAttach>();
	ParseLevelActorRefField(Json, TEXT("actor"), Pointer, true, Op->Target, OutDiagnostics);
	ParseLevelActorRefField(Json, TEXT("parent"), Pointer, true, Op->Parent, OutDiagnostics);
	for (const TCHAR* Field : {TEXT("socket"), TEXT("component")})
	{
		FString& Out = FCString::Strcmp(Field, TEXT("socket")) == 0 ? Op->Socket : Op->Component;
		if (Json.HasField(Field) && (!Json.TryGetStringField(Field, Out) || Out.IsEmpty()))
		{
			OutDiagnostics.Add(MakeInvalidRequest(Pointer + TEXT("/") + Field, FString::Printf(TEXT("'%s' is a non-empty string."), Field)));
		}
	}
	return OutDiagnostics.Num() == Before ? TUniquePtr<FLevelEditOp>(MoveTemp(Op)) : nullptr;
}

TUniquePtr<FLevelEditOp> ParseDetach(const FJsonObject& Json, const FString& Pointer, TArray<FAssetDocumentDiagnostic>& OutDiagnostics)
{
	static const TCHAR* const Fields[] = {TEXT("actor")};
	const int32 Before = OutDiagnostics.Num();
	CheckLevelOpFields(Json, Pointer, Fields, OutDiagnostics);
	TUniquePtr<FLevelOpDetach> Op = MakeUnique<FLevelOpDetach>();
	ParseLevelActorRefField(Json, TEXT("actor"), Pointer, true, Op->Target, OutDiagnostics);
	return OutDiagnostics.Num() == Before ? TUniquePtr<FLevelEditOp>(MoveTemp(Op)) : nullptr;
}

const FLevelEditOpRegistration AttachRegistration(TEXT("attach"), &ParseAttach);
const FLevelEditOpRegistration DetachRegistration(TEXT("detach"), &ParseDetach);
}
}
