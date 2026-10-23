// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// pin / unpin: the World Outliner's Pin and Unpin of World Partition actors
// (UWorldPartition::PinActors / UnpinActors). Post-commit helpers (design §3.1 step 9,
// §3.7): they change what is loaded, not the level, so they are outside the batch
// transaction, never rolled back, and run only after the batch committed. An actor a
// pin loads becomes editable from the next edit_level call on.

#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "LevelEditTypes.h"
#include "UObject/Package.h"
#include "WorldPartition/WorldPartition.h"

namespace UE::AssetDocumentLevel::Private
{
TSharedRef<FJsonObject> MakeLevelOpResult(const TCHAR* Kind, const FString& CreatedPath);

namespace
{
class FLevelOpPin final : public FLevelEditOp
{
public:
	bool bPin = true;
	TArray<FActorRef> Targets;

	virtual const TCHAR* GetKind() const override { return bPin ? TEXT("pin") : TEXT("unpin"); }
	virtual bool IsPostCommitHelper() const override { return true; }

	FString TargetPointer(int32 TargetIndex) const
	{
		return FString::Printf(TEXT("%s/actors/%d"), *Pointer, TargetIndex);
	}

	virtual void Preflight(FLevelEditPlan& Plan) const override
	{
		if (Plan.World.GetWorldPartition() == nullptr)
		{
			Plan.AddError(Pointer, TEXT("Level.InvalidRequest"), FString::Printf(TEXT("%s works on World Partition worlds only; every actor "
				"of this world is loaded with its level."), GetKind()));
			return;
		}
		for (int32 TargetIndex = 0; TargetIndex < Targets.Num(); ++TargetIndex)
		{
			const FActorRef& Ref = Targets[TargetIndex];
			if (Ref.Kind == EActorRefKind::BatchId)
			{
				Plan.AddError(TargetPointer(TargetIndex), TEXT("Level.InvalidRequest"), FString::Printf(TEXT("%s takes actors that exist "
					"before the request (path, guid or label), not \"@%s\"."), GetKind(), *Ref.Value));
				continue;
			}
			const FActorRefResolution Resolution = ResolveActorRef(Plan.World, Ref);
			if (!Resolution.bResolved)
			{
				Plan.AddError(TargetPointer(TargetIndex), Resolution.Code, Resolution.Message);
				continue;
			}
			Plan.RefPaths.Add(TargetPointer(TargetIndex), Resolution.Path);
			if (!Resolution.Guid.IsValid())
			{
				Plan.AddError(TargetPointer(TargetIndex), TEXT("Level.InvalidRequest"), FString::Printf(TEXT("%s is not a World Partition "
					"actor (it has no actor descriptor), so it is always loaded."), *Resolution.Path));
				continue;
			}
			const AActor* Actor = Resolution.Actor.Get();
			if (!bPin && Actor != nullptr && Actor->GetPackage() != nullptr && Actor->GetPackage()->IsDirty())
			{
				Plan.AddError(TargetPointer(TargetIndex), TEXT("Level.InvalidRequest"), FString::Printf(TEXT("%s has unsaved changes; "
					"unpinning could unload it and lose them. Save it first (edit_level \"save\": true, or in the editor)."), *Resolution.Path));
			}
		}
	}

	virtual bool Apply(FLevelEditApply& Apply) const override
	{
		// Nothing inside the transaction; see PostCommit.
		Apply.OpResults[Index] = MakeLevelOpResult(GetKind(), FString());
		return true;
	}

	virtual void PostCommit(FLevelEditApply& Apply) const override
	{
		UWorld* World = Apply.GetWorld();
		UWorldPartition* WorldPartition = World != nullptr ? World->GetWorldPartition() : nullptr;
		if (WorldPartition == nullptr)
		{
			return;
		}
		TArray<FGuid> Guids;
		TArray<FString> Paths;
		for (int32 TargetIndex = 0; TargetIndex < Targets.Num(); ++TargetIndex)
		{
			const FActorRefResolution Resolution = ResolveActorRef(*World, Apply.PinRef(Targets[TargetIndex], TargetPointer(TargetIndex)));
			const AActor* Actor = Resolution.Actor.Get();
			if (!Resolution.bResolved || !Resolution.Guid.IsValid())
			{
				continue;
			}
			if (!bPin && Actor != nullptr && Actor->GetPackage() != nullptr && Actor->GetPackage()->IsDirty())
			{
				Apply.AddWarning(TargetPointer(TargetIndex), TEXT("Level.InvalidRequest"), FString::Printf(TEXT("%s was not unpinned: it "
					"has unsaved changes (from this request)."), *Resolution.Path));
				continue;
			}
			Guids.AddUnique(Resolution.Guid);
			Paths.AddUnique(Resolution.Path);
		}
		if (Guids.IsEmpty())
		{
			return;
		}
		if (bPin)
		{
			WorldPartition->PinActors(Guids);
		}
		else
		{
			WorldPartition->UnpinActors(Guids);
		}
		TArray<TSharedPtr<FJsonValue>> Done;
		for (int32 Position = 0; Position < Guids.Num(); ++Position)
		{
			if (WorldPartition->IsActorPinned(Guids[Position]) != bPin)
			{
				Apply.AddWarning(Pointer, TEXT("Level.VerificationMismatch"), FString::Printf(TEXT("%s is %s pinned after %s."), *Paths[Position],
					bPin ? TEXT("not") : TEXT("still"), GetKind()));
				continue;
			}
			Done.Add(MakeShared<FJsonValueString>(Paths[Position]));
			(bPin ? Apply.Pinned : Apply.Unpinned).Add(MakeShared<FJsonValueString>(Paths[Position]));
		}
		if (Apply.OpResults.IsValidIndex(Index) && Apply.OpResults[Index].IsValid())
		{
			Apply.OpResults[Index]->SetArrayField(bPin ? TEXT("pinned") : TEXT("unpinned"), Done);
		}
	}
};

TUniquePtr<FLevelEditOp> ParsePin(const FJsonObject& Json, const FString& Pointer, bool bPin, TArray<FAssetDocumentDiagnostic>& OutDiagnostics)
{
	static const TCHAR* const Fields[] = {TEXT("actors")};
	const int32 Before = OutDiagnostics.Num();
	CheckLevelOpFields(Json, Pointer, Fields, OutDiagnostics);
	TUniquePtr<FLevelOpPin> Op = MakeUnique<FLevelOpPin>();
	Op->bPin = bPin;
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
	return OutDiagnostics.Num() == Before ? TUniquePtr<FLevelEditOp>(MoveTemp(Op)) : nullptr;
}

const FLevelEditOpRegistration PinRegistration(TEXT("pin"), [](const FJsonObject& Json, const FString& Pointer,
	TArray<FAssetDocumentDiagnostic>& OutDiagnostics) { return ParsePin(Json, Pointer, true, OutDiagnostics); });
const FLevelEditOpRegistration UnpinRegistration(TEXT("unpin"), [](const FJsonObject& Json, const FString& Pointer,
	TArray<FAssetDocumentDiagnostic>& OutDiagnostics) { return ParsePin(Json, Pointer, false, OutDiagnostics); });
}
}
