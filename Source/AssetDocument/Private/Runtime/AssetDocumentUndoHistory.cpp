// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "Runtime/AssetDocumentUndoHistory.h"

#include "Editor.h"
#include "Editor/TransBuffer.h"
#include "Editor/Transactor.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectIterator.h"

namespace UE::AssetDocument::UndoHistory
{
namespace
{
UTransactor* GetTransactor()
{
	return GEditor ? ToRawPtr(GEditor->Trans) : nullptr;
}

bool IsInPackage(const UObject* Object, const FName PackageName)
{
	const UPackage* Package = Object ? Object->GetPackage() : nullptr;
	return Package != nullptr && Package->GetFName() == PackageName;
}

// The objects one transaction records or references (what it keeps reachable).
TArray<UObject*> ReferencedObjects(FTransaction& Transaction)
{
	TArray<UObject*> Referenced;
	FReferenceFinder Finder(Referenced);
	Transaction.AddReferencedObjects(Finder);
	return Referenced;
}

const FText& ResetReason()
{
	static const FText Reason = NSLOCTEXT("AssetDocument", "UndoHistoryResetByApply", "AssetDocument changed the asset");
	return Reason;
}

FAssetDocumentUndoHistoryOutcome ClearAll(UTransactor& Transactor)
{
	FAssetDocumentUndoHistoryOutcome Outcome;
	Outcome.bCleared = true;
	Outcome.ClearedEntries = Transactor.GetQueueLength();
	GEditor->ResetTransaction(ResetReason());
	return Outcome;
}
}

namespace
{
FAssetDocumentUndoHistoryMark CaptureMark(bool bWithDirtyPackages)
{
	FAssetDocumentUndoHistoryMark Mark;
	if (UTransactor* Transactor = GetTransactor())
	{
		Mark.QueueLength = Transactor->GetQueueLength();
		Mark.UndoCount = Transactor->GetUndoCount();
		Mark.TransactionIds.Reserve(Mark.QueueLength);
		// Only needed to tell the apply's own changes from earlier ones (FinishApply's closure).
		for (TObjectIterator<UPackage> It; bWithDirtyPackages && Mark.QueueLength > 0 && It; ++It)
		{
			if (It->IsDirty())
			{
				Mark.DirtyPackages.Add(It->GetFName());
			}
		}
		for (int32 Index = 0; Index < Mark.QueueLength; ++Index)
		{
			const FTransaction* Transaction = Transactor->GetTransaction(Index);
			Mark.TransactionIds.Add(Transaction != nullptr ? Transaction->GetId() : FGuid());
		}
		if (const FTransaction* Newest = Mark.QueueLength > 0 ? Transactor->GetTransaction(Mark.QueueLength - 1) : nullptr)
		{
			Mark.NewestTransactionId = Newest->GetContext().TransactionId;
		}
	}
	return Mark;
}
}

FAssetDocumentUndoHistoryMark Capture()
{
	return CaptureMark(true);
}

bool ReferencesPackage(const FString& PackageName)
{
	UTransactor* Transactor = GetTransactor();
	if (Transactor == nullptr || PackageName.IsEmpty() || Transactor->GetQueueLength() == 0)
	{
		return false;
	}
	// The objects the buffer keeps reachable: every recorded object and every
	// object referenced by a recorded state.
	TArray<UObject*> Referenced;
	FReferenceFinder Finder(Referenced);
	Transactor->CallAddReferencedObjects(Finder);
	const FName Name(*PackageName);
	return Referenced.ContainsByPredicate([Name](const UObject* Object) { return IsInPackage(Object, Name); });
}

FAssetDocumentUndoHistoryOutcome FinishApply(
	const FAssetDocumentUndoHistoryMark& BeforeApply,
	const FString& PackageName,
	bool bAppliedChanges)
{
	FAssetDocumentUndoHistoryOutcome Outcome;
	UTransactor* Transactor = GetTransactor();
	// Never change history from inside an open transaction or an undo/redo.
	if (Transactor == nullptr || Transactor->GetQueueLength() == 0 || Transactor->IsActive() || GIsTransacting)
	{
		return Outcome;
	}
	const bool bApplyRecorded = !(CaptureMark(false) == BeforeApply);
	if (!bApplyRecorded && !bAppliedChanges)
	{
		return Outcome;
	}
	UTransBuffer* Buffer = Cast<UTransBuffer>(Transactor);
	if (Buffer == nullptr)
	{
		// Entries can only be removed one by one from the editor's own buffer.
		return bApplyRecorded || ReferencesPackage(PackageName) ? ClearAll(*Transactor) : Outcome;
	}

	// Seeds: the apply's own entries, and the entries that record or reference an object of the package.
	const TSet<FGuid> Before(BeforeApply.TransactionIds);
	const FName Package(*PackageName);
	const int32 Num = Buffer->UndoBuffer.Num();
	const int32 FirstRedo = Num - Buffer->UndoCount;
	TArray<bool> Remove;
	TArray<bool> Own;
	TArray<TSet<const UObject*>> Recorded;
	Remove.Init(false, Num);
	Own.Init(false, Num);
	Recorded.SetNum(Num);
	for (int32 Index = 0; Index < Num; ++Index)
	{
		FTransaction& Transaction = Buffer->UndoBuffer[Index].Get();
		Own[Index] = !Before.Contains(Transaction.GetId());
		Remove[Index] = Own[Index] || (!PackageName.IsEmpty() && ReferencedObjects(Transaction).ContainsByPredicate(
			[Package](const UObject* Object) { return IsInPackage(Object, Package); }));
		TArray<UObject*> Objects;
		Transaction.GetTransactionObjects(Objects);
		Recorded[Index].Append(Objects);
	}
	// Closure. A transaction restores whole recorded states of its objects, so a kept entry that
	// would restore an object across a change a removed entry made is removed too: an older Undo
	// entry (or a newer Redo entry) recording an object a removed entry changed (e.g. a level batch
	// that spawned a lamp recorded the level's actor list a removed batch changed again). Objects of
	// the applied package need no closure (every entry touching them is removed). The apply's own
	// entries count only for objects whose package the apply dirtied (its editor steps also record
	// transient work, e.g. a preview actor spawned and destroyed in the editor world).
	TArray<TSet<const UObject*>> Changed;
	Changed.SetNum(Num);
	auto SeedChanged = [&](int32 Index)
	{
		for (const UObject* Object : Recorded[Index])
		{
			const UPackage* ObjectPackage = Object != nullptr ? Object->GetPackage() : nullptr;
			if (ObjectPackage == nullptr || ObjectPackage->GetFName() == Package)
			{
				continue;
			}
			if (Own[Index] && !(ObjectPackage->IsDirty() && !BeforeApply.DirtyPackages.Contains(ObjectPackage->GetFName())))
			{
				continue;
			}
			Changed[Index].Add(Object);
		}
	};
	for (int32 Index = 0; Index < Num; ++Index)
	{
		if (Remove[Index])
		{
			SeedChanged(Index);
		}
	}
	for (bool bGrew = true; bGrew;)
	{
		bGrew = false;
		for (int32 Removed = 0; Removed < Num; ++Removed)
		{
			if (!Remove[Removed] || Changed[Removed].IsEmpty())
			{
				continue;
			}
			const bool bRedo = Removed >= FirstRedo;
			for (int32 Index = bRedo ? Removed + 1 : 0; Index < (bRedo ? Num : Removed); ++Index)
			{
				if (Remove[Index])
				{
					continue;
				}
				for (const UObject* Object : Recorded[Index])
				{
					if (Changed[Removed].Contains(Object))
					{
						Remove[Index] = true;
						SeedChanged(Index);
						bGrew = true;
						break;
					}
				}
			}
		}
	}
	if (!Remove.Contains(true))
	{
		return Outcome;
	}

	// 2. Remove, newest first, keeping the Redo count and the undo barriers on the kept entries.
	int32 UndoCount = Buffer->UndoCount;
	TArray<int32> Barriers = Buffer->UndoBarrierStack;
	for (int32 Index = Num - 1; Index >= 0; --Index)
	{
		if (!Remove[Index])
		{
			++Outcome.KeptEntries;
			continue;
		}
		if (Own[Index])
		{
			++Outcome.RemovedApplyEntries;
		}
		else
		{
			++Outcome.RemovedOtherEntries;
			if (Outcome.RemovedTitles.Num() < 5)
			{
				Outcome.RemovedTitles.Add(Buffer->UndoBuffer[Index]->GetTitle().ToString());
			}
		}
		if (Index >= FirstRedo)
		{
			--UndoCount;
		}
		for (int32& Barrier : Barriers)
		{
			Barrier -= Index < Barrier ? 1 : 0;
		}
		Buffer->UndoBuffer.RemoveAt(Index);
	}
	Buffer->UndoCount = UndoCount;
	Buffer->UndoBarrierStack = Barriers;
	if (Buffer->UndoBuffer.IsEmpty())
	{
		Buffer->ResetReason = ResetReason();
	}
	Buffer->CheckState();
	// The buffer has no public "changed" broadcast; an empty transaction that is cancelled
	// restores the buffer exactly and broadcasts OnUndoBufferChanged (Undo History panel, menus).
	Buffer->Begin(TEXT("AssetDocument"), ResetReason());
	Buffer->Cancel(0);
	return Outcome;
}
}
