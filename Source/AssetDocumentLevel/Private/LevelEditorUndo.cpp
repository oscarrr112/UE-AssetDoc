// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "LevelEditorUndo.h"

#include "AssetDocumentEditorNonInteractiveScope.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "Editor/Transactor.h"
#include "Framework/Application/SlateApplication.h"
#include "LevelEditTypes.h"
#include "LevelQuery.h"
#include "Misc/ITransaction.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace UE::AssetDocumentLevel::Private
{
namespace
{
struct FUndoRequest
{
	bool bRedo = false;
	FString ExpectTitle;
	FString ExpectId;
};

bool ParseUndoRequest(const FJsonObject& Json, FUndoRequest& Out, TArray<FAssetDocumentDiagnostic>& Diagnostics)
{
	for (const auto& Field : Json.Values)
	{
		if (Field.Key != TEXT("direction") && Field.Key != TEXT("expect_title") && Field.Key != TEXT("expect_id"))
		{
			Diagnostics.Add(MakeInvalidRequest(TEXT("/") + FString(*Field.Key), FString::Printf(TEXT("Unknown field '%s' (fields: direction, "
				"expect_title, expect_id)."), *Field.Key)));
		}
		else if (Field.Value.IsValid() && Field.Value->Type != EJson::String)
		{
			Diagnostics.Add(MakeInvalidRequest(TEXT("/") + FString(*Field.Key), FString::Printf(TEXT("'%s' must be a string."), *Field.Key)));
		}
	}
	if (!Diagnostics.IsEmpty())
	{
		return false;
	}
	FString Direction = TEXT("undo");
	Json.TryGetStringField(TEXT("direction"), Direction);
	if (Direction != TEXT("undo") && Direction != TEXT("redo"))
	{
		Diagnostics.Add(MakeInvalidRequest(TEXT("/direction"), FString::Printf(TEXT("'direction' is \"undo\" or \"redo\", not '%s'."),
			*Direction)));
		return false;
	}
	Out.bRedo = Direction == TEXT("redo");
	Json.TryGetStringField(TEXT("expect_title"), Out.ExpectTitle);
	Json.TryGetStringField(TEXT("expect_id"), Out.ExpectId);
	if (Out.ExpectTitle.IsEmpty() && Out.ExpectId.IsEmpty())
	{
		Diagnostics.Add(MakeInvalidRequest(TEXT("/expect_title"), TEXT("Name the entry to undo or redo: 'expect_title' (the "
			"transaction title an edit_level / execute_python result returned) and/or 'expect_id' (its id). The tool never undoes "
			"an entry it was not told about, so the user's own edits are safe.")));
		return false;
	}
	FGuid Parsed;
	if (!Out.ExpectId.IsEmpty() && (!FGuid::Parse(Out.ExpectId, Parsed) || !Parsed.IsValid()))
	{
		Diagnostics.Add(MakeInvalidRequest(TEXT("/expect_id"), FString::Printf(TEXT("'%s' is not an Undo entry id (32 hex digits, as "
			"edit_level's transaction.id returns it)."), *Out.ExpectId)));
		return false;
	}
	return true;
}

TSharedRef<FJsonObject> EntryJson(const FTransactionContext& Context)
{
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("title"), Context.Title.ToString());
	Json->SetStringField(TEXT("id"), FormatUndoEntryId(Context.TransactionId));
	return Json;
}

// The stack state after the step: whether more entries remain and which.
void AddStackState(UTransactor& Trans, FJsonObject& Payload)
{
	const bool bCanUndo = Trans.CanUndo();
	const bool bCanRedo = Trans.CanRedo();
	Payload.SetBoolField(TEXT("can_undo"), bCanUndo);
	Payload.SetBoolField(TEXT("can_redo"), bCanRedo);
	if (bCanUndo)
	{
		Payload.SetObjectField(TEXT("next_undo"), EntryJson(Trans.GetUndoContext(true)));
	}
	if (bCanRedo)
	{
		Payload.SetObjectField(TEXT("next_redo"), EntryJson(Trans.GetRedoContext()));
	}
}
}

FString FormatUndoEntryId(const FGuid& TransactionId)
{
	return TransactionId.IsValid() ? TransactionId.ToString(EGuidFormats::Digits) : FString();
}

FLevelToolResult RunEditorUndoFromBody(const FString& RequestBody)
{
	TSharedPtr<FJsonObject> Json;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(RequestBody);
	if (!FJsonSerializer::Deserialize(Reader, Json) || !Json.IsValid())
	{
		return FLevelToolResult::Failure({MakeInvalidRequest(TEXT(""), TEXT("The request body must be a JSON object "
			"({\"direction\"?, \"expect_title\"?, \"expect_id\"?})."))});
	}
	return RunEditorUndo(Json);
}

FLevelToolResult RunEditorUndo(const TSharedPtr<FJsonObject>& RequestJson)
{
	check(IsInGameThread());
	TArray<FAssetDocumentDiagnostic> Diagnostics;
	if (!CheckLevelToolAdmission(Diagnostics))
	{
		return FLevelToolResult::Failure(MoveTemp(Diagnostics));
	}
	FUndoRequest Request;
	if (!RequestJson.IsValid() || !ParseUndoRequest(*RequestJson, Request, Diagnostics))
	{
		if (Diagnostics.IsEmpty())
		{
			Diagnostics.Add(MakeInvalidRequest(TEXT(""), TEXT("The request must be a JSON object.")));
		}
		return FLevelToolResult::Failure(MoveTemp(Diagnostics));
	}
	UTransactor* Trans = GEditor->Trans;
	const TCHAR* Verb = Request.bRedo ? TEXT("redo") : TEXT("undo");
	if (Trans == nullptr || GIsTransacting || GEditor->IsTransactionActive()
		|| (FSlateApplication::IsInitialized() && FSlateApplication::Get().GetActiveModalWindow().IsValid()))
	{
		const FString Reason = GEditor->IsTransactionActive()
			? FString::Printf(TEXT("'%s' is in progress (for example a gizmo drag)"), *GEditor->GetTransactionName().ToString())
			: FString(TEXT("an Undo / Redo or a modal dialog is in progress"));
		return FLevelToolResult::Failure({MakeLevelDiagnostic(TEXT(""), TEXT("Level.EditorBusy"), FString::Printf(TEXT("The editor "
			"cannot %s now: %s. Nothing was changed; retry shortly."), Verb, *Reason))});
	}

	// The entry this step would apply: the newest Undo entry, or the oldest undone one for Redo.
	FText Unavailable;
	const bool bPossible = Request.bRedo ? Trans->CanRedo(&Unavailable) : Trans->CanUndo(&Unavailable);
	if (!bPossible)
	{
		TSharedRef<FJsonObject> Details = MakeShared<FJsonObject>();
		Details->SetStringField(TEXT("reason"), Unavailable.ToString());
		return FLevelToolResult::Failure({MakeLevelDiagnostic(TEXT("/direction"), Request.bRedo ? TEXT("Undo.NothingToRedo")
			: TEXT("Undo.NothingToUndo"), FString::Printf(TEXT("There is nothing to %s %s; nothing was changed."), Verb,
				*Unavailable.ToString()), Details)});
	}
	const FTransactionContext Entry = Request.bRedo ? Trans->GetRedoContext() : Trans->GetUndoContext(true);
	const FString Title = Entry.Title.ToString();
	const FString Id = FormatUndoEntryId(Entry.TransactionId);
	FGuid ExpectedId;
	FGuid::Parse(Request.ExpectId, ExpectedId);
	const bool bTitleMatches = Request.ExpectTitle.IsEmpty() || Request.ExpectTitle == Title;
	const bool bIdMatches = Request.ExpectId.IsEmpty() || ExpectedId == Entry.TransactionId;
	if (!bTitleMatches || !bIdMatches)
	{
		TSharedRef<FJsonObject> Details = MakeShared<FJsonObject>();
		Details->SetStringField(TEXT("actual_title"), Title);
		Details->SetStringField(TEXT("actual_id"), Id);
		const FString Expected = !Request.ExpectTitle.IsEmpty() ? FString::Printf(TEXT("'%s'"), *Request.ExpectTitle)
			: FString::Printf(TEXT("id %s"), *Request.ExpectId);
		return FLevelToolResult::Failure({MakeLevelDiagnostic(Request.ExpectTitle.IsEmpty() ? TEXT("/expect_id") : TEXT("/expect_title"),
			TEXT("Undo.EntryMismatch"), FString::Printf(TEXT("The %s entry is '%s' (id %s), not %s; nothing was %s. It may be the "
				"user's own edit or a later one: %s only the entry a result of yours named, after checking the level."),
				Request.bRedo ? TEXT("next Redo") : TEXT("newest Undo"), *Title, *Id, *Expected, Request.bRedo ? TEXT("redone")
				: TEXT("undone"), Verb), Details)});
	}

	bool bDone = false;
	{
		FAssetDocumentEditorNonInteractiveScope NonInteractive;
		bDone = Request.bRedo ? GEditor->RedoTransaction() : GEditor->UndoTransaction();
	}
	if (!bDone)
	{
		return FLevelToolResult::Failure({MakeLevelDiagnostic(TEXT(""), TEXT("Undo.Failed"), FString::Printf(TEXT("The editor refused "
			"to %s '%s' (it is saving or collecting garbage); nothing was changed. Retry shortly."), Verb, *Title))});
	}
	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("direction"), Verb);
	Payload->SetStringField(TEXT("title"), Title);
	Payload->SetStringField(TEXT("id"), Id);
	AddStackState(*Trans, *Payload);
	return FLevelToolResult::Success(FString::Printf(TEXT("%s '%s'."), Request.bRedo ? TEXT("Redid") : TEXT("Undid"), *Title), Payload);
}
}
