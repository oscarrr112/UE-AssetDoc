// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "LevelToolResult.h"

// undo_editor (POST /assetdoc/editor/undo): the editor's Edit > Undo / Redo for an MCP
// client. It runs on the game thread outside any transaction (execute_python cannot undo:
// its script runs inside its own transaction). The request names the entry it expects on
// top of the Undo (or Redo) stack, by the title or id an edit_level / execute_python result
// returned; any other entry, for example the user's own edit, is refused with
// Undo.EntryMismatch naming the actual entry, and nothing is undone.
//
// Request: {"direction": "undo" (default) | "redo", "expect_title"?, "expect_id"?}; at
// least one of expect_title / expect_id.
// Payload: {direction, title, id, can_undo, can_redo, next_undo?, next_redo?}.
namespace UE::AssetDocumentLevel::Private
{
FLevelToolResult RunEditorUndo(const TSharedPtr<FJsonObject>& Request);

FLevelToolResult RunEditorUndoFromBody(const FString& RequestBody);

// The id the tools print for an Undo entry (32 hex digits; "" for an invalid guid).
FString FormatUndoEntryId(const FGuid& TransactionId);
}
