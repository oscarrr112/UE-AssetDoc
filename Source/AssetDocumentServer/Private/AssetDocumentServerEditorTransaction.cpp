// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentServerEditorTransaction.h"

#include "Editor.h"
#include "Editor/Transactor.h"
#include "Misc/ITransaction.h"

const TCHAR* LexToString(EAssetDocumentTransactionRollback Outcome)
{
	switch (Outcome)
	{
	case EAssetDocumentTransactionRollback::Undone: return TEXT("Undone");
	case EAssetDocumentTransactionRollback::NothingRecorded: return TEXT("NothingRecorded");
	case EAssetDocumentTransactionRollback::Nested: return TEXT("Nested");
	case EAssetDocumentTransactionRollback::UndoFailed: return TEXT("UndoFailed");
	case EAssetDocumentTransactionRollback::Kept: return TEXT("Kept");
	case EAssetDocumentTransactionRollback::NotOpen:
	default: return TEXT("NotOpen");
	}
}

FAssetDocumentOwnedEditorTransaction::~FAssetDocumentOwnedEditorTransaction()
{
	if (bOpen)
	{
		Commit();
	}
}

bool FAssetDocumentOwnedEditorTransaction::Begin(const FText& Description)
{
	if (bOpen || GEditor == nullptr || GEditor->Trans == nullptr || GIsTransacting)
	{
		return false;
	}
	bOutermost = !GEditor->IsTransactionActive();
	GEditor->BeginTransaction(Description);
	if (!GEditor->IsTransactionActive())
	{
		// The editor refused the transaction (e.g. while loading packages).
		return false;
	}
	bOpen = true;
	TransactionId = bOutermost && GUndo != nullptr ? GUndo->GetContext().TransactionId : FGuid();
	return true;
}

bool FAssetDocumentOwnedEditorTransaction::IsStillActive() const
{
	return bOpen
		&& GEditor != nullptr
		&& GEditor->IsTransactionActive()
		&& GUndo != nullptr
		&& (!bOutermost || GUndo->GetContext().TransactionId == TransactionId);
}

bool FAssetDocumentOwnedEditorTransaction::IsNewestUndoEntry(const FGuid& Id)
{
	if (!Id.IsValid() || GEditor == nullptr || GEditor->Trans == nullptr || GEditor->IsTransactionActive())
	{
		return false;
	}
	// GetUndoContext(false) names the entry Undo would apply next, i.e. the newest
	// undoable one (entries above it are Redo entries).
	return GEditor->Trans->GetUndoContext(false).TransactionId == Id;
}

bool FAssetDocumentOwnedEditorTransaction::Commit()
{
	if (!bOpen)
	{
		return false;
	}
	bOpen = false;
	if (GEditor == nullptr)
	{
		return false;
	}
	GEditor->EndTransaction();
	return bOutermost && IsNewestUndoEntry(TransactionId);
}

EAssetDocumentTransactionRollback FAssetDocumentOwnedEditorTransaction::Rollback()
{
	if (!bOpen || GEditor == nullptr)
	{
		bOpen = false;
		return EAssetDocumentTransactionRollback::NotOpen;
	}
	bOpen = false;
	GEditor->EndTransaction();
	if (!bOutermost)
	{
		return EAssetDocumentTransactionRollback::Nested;
	}
	if (!IsNewestUndoEntry(TransactionId))
	{
		// Recorded nothing: End discarded it. The newest entry is the user's own.
		return EAssetDocumentTransactionRollback::NothingRecorded;
	}
	return GEditor->UndoTransaction(/*bCanRedo*/ false)
		? EAssetDocumentTransactionRollback::Undone
		: EAssetDocumentTransactionRollback::UndoFailed;
}

TArray<UObject*> FAssetDocumentOwnedEditorTransaction::GetRecordedObjects() const
{
	TArray<UObject*> Objects;
	if (!bOutermost || !TransactionId.IsValid() || GEditor == nullptr || GEditor->Trans == nullptr)
	{
		return Objects;
	}
	for (int32 Index = GEditor->Trans->GetQueueLength() - 1; Index >= 0; --Index)
	{
		const FTransaction* Transaction = GEditor->Trans->GetTransaction(Index);
		if (Transaction != nullptr && Transaction->GetContext().TransactionId == TransactionId)
		{
			Transaction->GetTransactionObjects(Objects);
			break;
		}
	}
	return Objects;
}
