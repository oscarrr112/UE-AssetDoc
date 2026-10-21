// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Misc/Guid.h"

// Editor undo history (the UE transaction buffer) around an AssetDocument apply.
//
// An apply changes assets outside the editor's transaction system (Core
// package snapshot rollback, staged save), yet stock editor actions it uses
// record their own partial transactions (e.g. Blueprint "Break Pin Links",
// Sequencer edits), and the human's earlier transactions only hold pre-apply
// states of some objects. Undoing either kind after the apply mixes pre- and
// post-apply state (e.g. pins linked to nodes the apply removed). Behaviour at
// the end of an apply (FinishApply):
//   - the apply's own transactions are removed (an apply is never an Undo entry);
//   - after an apply that changed the asset, or that recorded transactions and
//     was rolled back, the entries that record or reference an object of the
//     applied package are removed too (the editor's UPackageTools::ReloadPackages
//     / ObjectTools::DeleteSingleObject reset the whole buffer for such a change);
//   - so is a kept entry that would restore an object across a change a removed
//     entry made (an older Undo entry, or a newer Redo entry, recording an object
//     the removed entry changed outside the applied package; for the apply's own
//     entries only objects of packages the apply dirtied count);
//   - every other entry (level edits, edits of other assets) is kept, Undo and
//     Redo alike, in order;
//   - a rejected or no-op apply leaves the history untouched;
//   - when entries cannot be removed one by one (a transactor that is not the
//     editor's UTransBuffer), the whole history is cleared instead.
// The apply result says when the history was trimmed (Undo.HistoryTrimmed) or
// cleared (Undo.HistoryCleared); removing only the apply's own entries is not
// reported (the history the user had is unchanged).

// Identifies the state of the transaction buffer; any recorded or removed
// transaction changes it.
struct FAssetDocumentUndoHistoryMark
{
	int32 QueueLength = 0;
	int32 UndoCount = 0;
	FGuid NewestTransactionId;
	// Every entry's id, oldest first.
	TArray<FGuid> TransactionIds;
	// The packages with unsaved changes (not part of the comparison).
	TSet<FName> DirtyPackages;

	bool operator==(const FAssetDocumentUndoHistoryMark& Other) const
	{
		return QueueLength == Other.QueueLength && UndoCount == Other.UndoCount
			&& NewestTransactionId == Other.NewestTransactionId && TransactionIds == Other.TransactionIds;
	}
};

// What FinishApply did to the history.
struct FAssetDocumentUndoHistoryOutcome
{
	// Entries the apply itself recorded, removed.
	int32 RemovedApplyEntries = 0;
	// Earlier entries removed because they recorded or referenced the applied package, or
	// would restore an object across a change such an entry made.
	int32 RemovedOtherEntries = 0;
	// Entries kept (Undo and Redo).
	int32 KeptEntries = 0;
	// The titles of the removed earlier entries, newest first (at most 5).
	TArray<FString> RemovedTitles;
	// The whole history was cleared (selective removal was not possible).
	bool bCleared = false;
	int32 ClearedEntries = 0;

	bool ChangedUserHistory() const { return bCleared || RemovedOtherEntries > 0; }
};

namespace UE::AssetDocument::UndoHistory
{
ASSETDOCUMENT_API FAssetDocumentUndoHistoryMark Capture();

// True when the transaction buffer records or references an object of the package.
ASSETDOCUMENT_API bool ReferencesPackage(const FString& PackageName);

// Applies the behaviour above at the end of an apply.
ASSETDOCUMENT_API FAssetDocumentUndoHistoryOutcome FinishApply(
	const FAssetDocumentUndoHistoryMark& BeforeApply,
	const FString& PackageName,
	bool bAppliedChanges);
}
