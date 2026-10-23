// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "Misc/Guid.h"

// How a failed tool request's editor transaction was taken back.
enum class EAssetDocumentTransactionRollback : uint8
{
	// The transaction recorded changes; its Undo entry was applied and removed (no Redo).
	Undone,
	// The transaction recorded nothing: the engine discarded it on End and the Undo
	// history is as it was before Begin.
	NothingRecorded,
	// Another transaction was already open when Begin ran (a user interaction): the
	// changes merged into it and cannot be undone on their own.
	Nested,
	// The transaction is the newest Undo entry, but the editor refused the Undo.
	UndoFailed,
	// Begin was not called, failed, or the transaction was already finished.
	NotOpen,
	// Not taken back on purpose: the request also did work the Undo history cannot take back
	// (an asset created, renamed, deleted or saved), so undoing the rest would leave a half
	// state. The changes stay, committed as the transaction's Undo entry (if it recorded any).
	Kept,
};

ASSETDOCUMENTSERVER_API const TCHAR* LexToString(EAssetDocumentTransactionRollback Outcome);

// One editor transaction owned by a tool request (execute_python; the level tools).
//
// Begin opens it. Commit ends it: one Undo entry titled Description (an entry that
// recorded nothing is discarded by the engine). Rollback ends it first and then
// undoes it only when the newest Undo entry is this transaction:
// - UndoTransaction while the transaction is still open does nothing
//   (UTransBuffer::CanUndo is false while ActiveCount > 0) and leaves it open, so
//   every later editor edit merges into it and Undo stays disabled;
// - UTransBuffer::End pops a transaction that recorded nothing, so an Undo after
//   End without the check would undo the user's previous edit.
// The destructor commits a transaction that is still open.
class ASSETDOCUMENTSERVER_API FAssetDocumentOwnedEditorTransaction
{
public:
	FAssetDocumentOwnedEditorTransaction() = default;
	~FAssetDocumentOwnedEditorTransaction();

	FAssetDocumentOwnedEditorTransaction(const FAssetDocumentOwnedEditorTransaction&) = delete;
	FAssetDocumentOwnedEditorTransaction& operator=(const FAssetDocumentOwnedEditorTransaction&) = delete;

	// False when there is no editor transaction buffer or an Undo/Redo is being applied.
	bool Begin(const FText& Description);

	bool IsOpen() const { return bOpen; }
	// False when another transaction was already open at Begin (ours is nested in it).
	bool IsOutermost() const { return bOutermost; }
	// Invalid when nested.
	const FGuid& GetTransactionId() const { return TransactionId; }
	// True while this transaction is the editor's open transaction (an editor call that
	// resets the transaction buffer mid-request makes it false).
	bool IsStillActive() const;

	// Ends the transaction. Returns true when it left an Undo entry (it recorded changes
	// and is the newest entry).
	bool Commit();

	EAssetDocumentTransactionRollback Rollback();

	// True when the newest Undo entry of the editor is the transaction with this id.
	static bool IsNewestUndoEntry(const FGuid& TransactionId);

	// The objects this (outermost, open or committed) transaction recorded so far, i.e. what an
	// Undo of it would restore. Empty when nested or when it recorded nothing.
	TArray<UObject*> GetRecordedObjects() const;

private:
	bool bOpen = false;
	bool bOutermost = false;
	FGuid TransactionId;
};
