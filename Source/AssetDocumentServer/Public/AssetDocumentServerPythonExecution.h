// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "AssetDocumentServerEditorTransaction.h"

struct FAssetDocumentServerPythonLogEntry
{
	// "info", "warning" or "error"
	FString Type;
	FString Message;
};

struct FAssetDocumentServerPythonExecutionResult
{
	bool bSucceeded = false;
	// Set when the script could not run at all (no Python plugin, editor busy).
	FString Error;
	FString CommandResult;
	TArray<FAssetDocumentServerPythonLogEntry> Logs;
	// Success: the script's changes are one Undo entry titled with the description
	// (false when it changed nothing undoable).
	bool bUndoEntryAdded = false;
	// With bUndoEntryAdded: the entry's id (undo_editor's expect_id; its title is the description).
	FGuid UndoEntryId;
	// Failure: how the script's changes were taken back.
	EAssetDocumentTransactionRollback Rollback = EAssetDocumentTransactionRollback::NotOpen;
	// Work the script did that the Undo history cannot take back ("renamed <old> to <new>",
	// "created <asset>", "deleted <asset>", "saved <package>", "opened map <file>"). When a
	// failed script did any, nothing is undone (Rollback Kept).
	TArray<FString> NotUndoable;
	// Rollback Undone: the objects the Undo restored (at most 50, by path) and how many there were.
	TArray<FString> RolledBackObjects;
	int32 RolledBackObjectCount = 0;
};

// POST /assetdoc/execute (MCP execute_python), game thread only: runs Code as a
// file-scope Python script inside one editor transaction. A script that succeeds is
// one Undo entry; a script that fails is rolled back with
// FAssetDocumentOwnedEditorTransaction::Rollback (its recorded changes are undone,
// the user's previous Undo entry is never touched, and no transaction stays open),
// unless it also did work the Undo history cannot take back (NotUndoable): then
// everything it did is kept (Rollback Kept), as undoing only part would leave a half state.
// Code is a script file only when the whole input is one .py path (with optional arguments);
// any other code runs as code, even when it mentions a .py file name.
ASSETDOCUMENTSERVER_API FAssetDocumentServerPythonExecutionResult ExecuteAssetDocumentServerPython(
	const FString& Code,
	const FString& Description);
