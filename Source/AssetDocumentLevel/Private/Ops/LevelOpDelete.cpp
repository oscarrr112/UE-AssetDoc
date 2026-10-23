// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// delete: the Delete key (UUnrealEdEngine::DeleteActors). The reference dialogs it
// would show are computed in preflight instead (design §3.2, decision D5): a
// referenced actor is refused with Level.DeleteReferenced unless allow_referenced,
// which deletes and warns.

#include "Editor.h"
#include "Editor/UnrealEdEngine.h"
#include "Elements/Framework/TypedElementSelectionSet.h"
#include "Engine/Selection.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "LevelEditTypes.h"
#include "LevelWorldIndex.h"
#include "UnrealEdGlobals.h"

namespace UE::AssetDocumentLevel::Private
{
TSharedRef<FJsonObject> MakeLevelOpResult(const TCHAR* Kind, const FString& CreatedPath);

namespace
{
class FLevelOpDelete final : public FLevelEditOp
{
public:
	TArray<FActorRef> Targets;
	bool bAllowReferenced = false;

	virtual const TCHAR* GetKind() const override { return TEXT("delete"); }

	FString TargetPointer(int32 TargetIndex) const
	{
		return FString::Printf(TEXT("%s/actors/%d"), *Pointer, TargetIndex);
	}

	virtual void Preflight(FLevelEditPlan& Plan) const override
	{
		TArray<int32> Resolved;
		TArray<int32> ResolvedTargets;
		TArray<AActor*> Live;
		TSet<FString> Deleting;
		for (int32 TargetIndex = 0; TargetIndex < Targets.Num(); ++TargetIndex)
		{
			const int32 ActorIndex = Plan.Resolve(Targets[TargetIndex], TargetPointer(TargetIndex));
			if (ActorIndex == INDEX_NONE)
			{
				continue;
			}
			if (Resolved.Contains(ActorIndex))
			{
				Plan.AddError(TargetPointer(TargetIndex), TEXT("Level.InvalidRequest"), FString::Printf(TEXT("%s is listed twice."),
					*Plan.Actors[ActorIndex].Display()));
				continue;
			}
			Resolved.Add(ActorIndex);
			ResolvedTargets.Add(TargetIndex);
			if (AActor* Actor = Plan.Actors[ActorIndex].bExisting ? Plan.Actors[ActorIndex].Actor.Get() : nullptr)
			{
				FText Reason;
				if (GUnrealEd != nullptr && !GUnrealEd->CanDeleteActor(Actor, &Reason))
				{
					Plan.AddError(TargetPointer(TargetIndex), TEXT("Level.DeleteRefused"), FString::Printf(TEXT("The editor does not "
						"delete %s: %s"), *Plan.Actors[ActorIndex].Path, *Reason.ToString()));
					continue;
				}
				Live.Add(Actor);
				Deleting.Add(Plan.Actors[ActorIndex].Path);
			}
		}
		FText AbortReason;
		if (!Live.IsEmpty() && GUnrealEd != nullptr && GUnrealEd->ShouldAbortActorDeletion(Live, &AbortReason))
		{
			Plan.AddError(Pointer + TEXT("/actors"), TEXT("Level.DeleteRefused"), FString::Printf(TEXT("The editor refuses this delete: %s"),
				*AbortReason.ToString()));
		}
		// Actors deleted by earlier ops of the batch are no referencers either.
		for (const FLevelPlanActor& Entry : Plan.Actors)
		{
			if (Entry.bDeleted && Entry.bExisting)
			{
				Deleting.Add(Entry.Path);
			}
		}

		for (int32 Position = 0; Position < Resolved.Num(); ++Position)
		{
			const int32 ActorIndex = Resolved[Position];
			FLevelPlanActor& Entry = Plan.Actors[ActorIndex];
			const FString EntryPointer = TargetPointer(ResolvedTargets[Position]);
			{
				// What the Delete key's dialog lists now, minus referencers whose references
				// earlier ops of this request overwrite, plus the references earlier ops create
				// (spawned or duplicated actors, values set in this request).
				TArray<TSharedPtr<FJsonValue>> Referencers;
				AActor* Actor = Entry.bExisting ? Entry.Actor.Get() : nullptr;
				if (Actor != nullptr)
				{
					for (const TSharedPtr<FJsonValue>& Referencer : Plan.FindReferencers(*Actor, Deleting))
					{
						const TSharedPtr<FJsonObject> Object = Referencer->AsObject();
						const FString Path = Object->GetStringField(TEXT("path"));
						const int32 ReferencerIndex = Object->GetStringField(TEXT("kind")) != TEXT("level_blueprint") ? Plan.FindByPath(Path) : INDEX_NONE;
						AActor* LiveReferencer = ReferencerIndex != INDEX_NONE && Plan.Actors[ReferencerIndex].bExisting
							? Plan.Actors[ReferencerIndex].Actor.Get() : nullptr;
						if (LiveReferencer != nullptr && !Plan.StillReferences(ReferencerIndex, *LiveReferencer, ActorIndex))
						{
							continue;
						}
						Referencers.Add(Referencer);
					}
				}
				for (const TSharedPtr<FJsonValue>& Planned : Plan.FindPlannedReferencers(ActorIndex, Deleting))
				{
					const FString Path = Planned->AsObject()->GetStringField(TEXT("path"));
					if (!Referencers.ContainsByPredicate([&Path](const TSharedPtr<FJsonValue>& Existing)
					{
						return Existing->AsObject()->GetStringField(TEXT("path")) == Path;
					}))
					{
						Referencers.Add(Planned);
					}
				}
				if (!Referencers.IsEmpty())
				{
					TSharedRef<FJsonObject> Details = MakeShared<FJsonObject>();
					Details->SetStringField(TEXT("actor"), Entry.Display());
					Details->SetArrayField(TEXT("referencers"), Referencers);
					TArray<FString> Names;
					for (const TSharedPtr<FJsonValue>& Referencer : Referencers)
					{
						Names.Add(Referencer->AsObject()->GetStringField(TEXT("path")));
					}
					const FString Listed = FString::Join(Names, TEXT(", "));
					if (bAllowReferenced)
					{
						Plan.AddWarning(EntryPointer, TEXT("Level.DeletedActorWasReferenced"), FString::Printf(TEXT("%s was referenced by "
							"%s; those references are now broken (allow_referenced)."), *Entry.Display(), *Listed), Details);
					}
					else
					{
						Plan.AddError(EntryPointer, TEXT("Level.DeleteReferenced"), FString::Printf(TEXT("%s is referenced by %s (what the "
							"Delete key's warning dialog lists). Clear those references first (a set earlier in the same request counts), or "
							"pass \"allow_referenced\": true to delete it anyway."), *Entry.Display(), *Listed), Details);
					}
				}
			}
			Entry.bDeleted = true;
			++Plan.Deletes;
			FLevelChange& Change = Plan.Changes.AddDefaulted_GetRef();
			Change.Op = Pointer;
			Change.Kind = TEXT("Remove");
			Change.Object = TEXT("actor");
			Change.Actor = Entry.Display();
		}
	}

	virtual bool Apply(FLevelEditApply& Apply) const override
	{
		UWorld* World = Apply.GetWorld();
		TArray<AActor*> Actors;
		TArray<FString> Paths;
		for (int32 TargetIndex = 0; TargetIndex < Targets.Num(); ++TargetIndex)
		{
			AActor* Actor = Apply.ResolveRef(Targets[TargetIndex], TargetPointer(TargetIndex));
			if (Actor == nullptr)
			{
				return false;
			}
			Actors.Add(Actor);
			Paths.Add(GetActorPath(*Actor));
			Apply.DeletedActorPackages.AddUnique(Actor->GetPackage()->GetName());
		}
		if (World == nullptr || GUnrealEd == nullptr || GEditor->GetSelectedActors() == nullptr)
		{
			Apply.AddError(Pointer, TEXT("Level.TransactionLost"), TEXT("The editor world is not available."));
			return false;
		}
		UTypedElementSelectionSet* Selection = GEditor->GetSelectedActors()->GetElementSelectionSet();
		if (!GUnrealEd->DeleteActors(Actors, World, Selection, /*bVerifyDeletionCanHappen*/ true, /*bWarnAboutReferences*/ false,
			/*bWarnAboutSoftReferences*/ false))
		{
			Apply.AddError(Pointer, TEXT("Level.DeleteRefused"), TEXT("The editor refused the delete (UUnrealEdEngine::DeleteActors)."));
			return false;
		}
		TArray<TSharedPtr<FJsonValue>> Deleted;
		for (const FString& Path : Paths)
		{
			Apply.ExpectGone(Path, Pointer);
			Deleted.Add(MakeShared<FJsonValueString>(Path));
			FLevelChange& Change = Apply.Changes.AddDefaulted_GetRef();
			Change.Op = Pointer;
			Change.Kind = TEXT("Remove");
			Change.Object = TEXT("actor");
			Change.Actor = Path;
			Apply.TouchedActorPaths.Remove(Path);
		}
		Apply.Deleted += Paths.Num();
		TSharedRef<FJsonObject> Result = MakeLevelOpResult(GetKind(), FString());
		Result->SetArrayField(TEXT("deleted"), Deleted);
		Apply.OpResults[Index] = Result;
		return true;
	}
};

TUniquePtr<FLevelEditOp> ParseDelete(const FJsonObject& Json, const FString& Pointer, TArray<FAssetDocumentDiagnostic>& OutDiagnostics)
{
	static const TCHAR* const Fields[] = {TEXT("actors"), TEXT("allow_referenced")};
	const int32 Before = OutDiagnostics.Num();
	CheckLevelOpFields(Json, Pointer, Fields, OutDiagnostics);
	TUniquePtr<FLevelOpDelete> Op = MakeUnique<FLevelOpDelete>();
	const TArray<TSharedPtr<FJsonValue>>* Refs = nullptr;
	if (!Json.TryGetArrayField(TEXT("actors"), Refs) || Refs == nullptr || Refs->IsEmpty())
	{
		OutDiagnostics.Add(MakeInvalidRequest(Pointer + TEXT("/actors"), TEXT("'actors' is a non-empty array of actor references.")));
	}
	else
	{
		for (int32 RefIndex = 0; RefIndex < Refs->Num(); ++RefIndex)
		{
			FActorRef Ref;
			FString Error;
			if (!ParseActorRef((*Refs)[RefIndex], Ref, Error))
			{
				OutDiagnostics.Add(MakeInvalidRequest(FString::Printf(TEXT("%s/actors/%d"), *Pointer, RefIndex), Error));
				continue;
			}
			Op->Targets.Add(MoveTemp(Ref));
		}
	}
	if (Json.HasField(TEXT("allow_referenced")) && !Json.TryGetBoolField(TEXT("allow_referenced"), Op->bAllowReferenced))
	{
		OutDiagnostics.Add(MakeInvalidRequest(Pointer + TEXT("/allow_referenced"), TEXT("'allow_referenced' must be true or false.")));
	}
	return OutDiagnostics.Num() == Before ? TUniquePtr<FLevelEditOp>(MoveTemp(Op)) : nullptr;
}

const FLevelEditOpRegistration DeleteRegistration(TEXT("delete"), &ParseDelete);
}
}
