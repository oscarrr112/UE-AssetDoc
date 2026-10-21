// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "LevelEditTransaction.h"

#include "AssetDocumentEditorNonInteractiveScope.h"
#include "AssetDocumentServerEditorTransaction.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "EditorActorFolders.h"
#include "EngineUtils.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "FileHelpers.h"
#include "Folder.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/Actor.h"
#include "LevelEditDigest.h"
#include "LevelEditTypes.h"
#include "LevelEditorUndo.h"
#include "LevelQuery.h"
#include "LevelWorldIndex.h"
#include "NavigationSystem.h"
#include "ActorFolder.h"
#include "Editor/Transactor.h"
#include "Misc/ITransaction.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Subsystems/EditorActorSubsystem.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"

namespace UE::AssetDocumentLevel::Private
{
namespace
{
TOptional<int32>& InjectedFailure()
{
	static TOptional<int32> AfterOp;
	return AfterOp;
}

TSharedRef<FJsonObject> FailurePayload(const TCHAR* Phase, bool bRolledBack)
{
	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetBoolField(TEXT("rolled_back"), bRolledBack);
	Payload->SetStringField(TEXT("phase"), Phase);
	return Payload;
}

FLevelToolResult PreflightFailure(TArray<FAssetDocumentDiagnostic> Diagnostics)
{
	return FLevelToolResult::Failure(MoveTemp(Diagnostics), FailurePayload(TEXT("preflight"), false));
}

TArray<TSharedPtr<FJsonValue>> ChangesJson(const TArray<FLevelChange>& Changes)
{
	TArray<TSharedPtr<FJsonValue>> Values;
	for (const FLevelChange& Change : Changes)
	{
		Values.Add(MakeShared<FJsonValueObject>(Change.ToJson()));
	}
	return Values;
}

TArray<TSharedPtr<FJsonValue>> StringArrayJson(const TArray<FString>& Strings)
{
	TArray<TSharedPtr<FJsonValue>> Values;
	for (const FString& String : Strings)
	{
		Values.Add(MakeShared<FJsonValueString>(String));
	}
	return Values;
}

bool IsEditorBusy(FString& OutReason)
{
	if (GIsTransacting)
	{
		OutReason = TEXT("an Undo / Redo is being applied");
		return true;
	}
	if (GEditor->IsTransactionActive())
	{
		OutReason = FString::Printf(TEXT("an editor operation is in progress ('%s', for example a gizmo drag)"),
			*GEditor->GetTransactionName().ToString());
		return true;
	}
	if (FSlateApplication::IsInitialized() && FSlateApplication::Get().GetActiveModalWindow().IsValid())
	{
		OutReason = TEXT("a modal dialog is open");
		return true;
	}
	return false;
}

FString Plural(int32 Count, const TCHAR* Noun)
{
	return FString::Printf(TEXT("%d %s%s"), Count, Noun, Count == 1 ? TEXT("") : TEXT("s"));
}

FString DescribeCounts(const FLevelEditApply& Apply)
{
	const int32 Spawned = Apply.Spawned;
	const int32 Deleted = Apply.Deleted;
	const int32 Duplicated = Apply.Duplicated;
	const int32 ValuesSet = Apply.ValuesSet;
	const TCHAR* Verb = TEXT("");
	TArray<FString> Parts;
	if (Spawned > 0)
	{
		Parts.Add(FString::Printf(TEXT("%s %sspawned"), *Plural(Spawned, TEXT("actor")), Verb));
	}
	if (Duplicated > 0)
	{
		Parts.Add(FString::Printf(TEXT("%d %sduplicated"), Duplicated, Verb));
	}
	if (Deleted > 0)
	{
		Parts.Add(FString::Printf(TEXT("%d %sdeleted"), Deleted, Verb));
	}
	if (ValuesSet > 0)
	{
		Parts.Add(FString::Printf(TEXT("%s %sset"), *Plural(ValuesSet, TEXT("value")), Verb));
	}
	if (Apply.ComponentsChanged > 0)
	{
		Parts.Add(FString::Printf(TEXT("%s added, removed or reparented"), *Plural(Apply.ComponentsChanged, TEXT("component"))));
	}
	if (Apply.Attachments > 0)
	{
		Parts.Add(FString::Printf(TEXT("%d attached or detached"), Apply.Attachments));
	}
	if (Apply.FolderOps > 0)
	{
		Parts.Add(FString::Printf(TEXT("%s changed"), *Plural(Apply.FolderOps, TEXT("folder"))));
	}
	return Parts.IsEmpty() ? FString(TEXT("no change")) : FString::Join(Parts, TEXT(", "));
}

// The packages a level edit dirties: each touched actor's package (the map or sublevel;
// its external package with one file per actor), the packages of deleted actors, and
// the actor folder packages folder ops changed, and the level packages that became
// dirty during the batch (the old parent of a detached actor, ...).
TArray<FString> CollectDirtyPackages(const FLevelEditApply& Apply, const TSet<FString>& DirtyBefore, const TSet<FString>& DirtyAfter)
{
	TArray<FString> Packages;
	for (const FString& Package : DirtyAfter)
	{
		if (!DirtyBefore.Contains(Package))
		{
			Packages.AddUnique(Package);
		}
	}
	for (const FString& Path : Apply.TouchedActorPaths)
	{
		if (AActor* Actor = Apply.ResolvePath(Path))
		{
			Packages.AddUnique(Actor->GetPackage()->GetName());
			// The actor's folder and its parents, when they are objects with their own
			// unsaved package (World Partition): saving the actor without them would lose
			// its folder on reload, so they are part of what "save" writes.
			// The folders are found by path: the actor's folder path, then each parent path.
			const ULevel* Level = Actor->GetLevel();
			FString FolderPath = Level != nullptr && Level->IsUsingActorFolders() ? Actor->GetFolderPath().ToString() : FString();
			while (!FolderPath.IsEmpty())
			{
				const UActorFolder* Folder = Level->GetActorFolder(FName(*FolderPath));
				const UPackage* Package = Folder != nullptr ? Folder->GetExternalPackage() : nullptr;
				if (Package != nullptr && Package->IsDirty())
				{
					Packages.AddUnique(Package->GetName());
				}
				int32 Slash = INDEX_NONE;
				if (!FolderPath.FindLastChar(TEXT('/'), Slash))
				{
					break;
				}
				FolderPath.LeftInline(Slash);
			}
		}
	}
	for (const FString& Package : Apply.DeletedActorPackages)
	{
		Packages.AddUnique(Package);
	}
	for (const FString& Name : Apply.FolderPackages)
	{
		const UPackage* Package = FindObject<UPackage>(nullptr, *Name);
		if (Package != nullptr && Package->IsDirty())
		{
			Packages.AddUnique(Name);
		}
	}
	Packages.Sort();
	return Packages;
}
}

FScopedLevelEditFailureInjection::FScopedLevelEditFailureInjection(int32 AfterOp)
	: Previous(InjectedFailure())
{
	InjectedFailure() = AfterOp;
}

FScopedLevelEditFailureInjection::~FScopedLevelEditFailureInjection()
{
	InjectedFailure() = Previous;
}

FLevelToolResult RunLevelEditFromBody(const FString& RequestBody)
{
	TSharedPtr<FJsonObject> Json;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(RequestBody);
	if (!FJsonSerializer::Deserialize(Reader, Json) || !Json.IsValid())
	{
		return PreflightFailure({MakeInvalidRequest(TEXT(""), TEXT("The request body must be a JSON object "
			"({\"ops\": [...], \"description\"?, \"dry_run\"?, \"save\"?, \"after\"?}).")) });
	}
	return RunLevelEdit(Json);
}

FLevelToolResult RunLevelEdit(const TSharedPtr<FJsonObject>& RequestJson)
{
	check(IsInGameThread());

	// 1. Admission.
	TArray<FAssetDocumentDiagnostic> Diagnostics;
	if (!CheckLevelToolAdmission(Diagnostics))
	{
		return PreflightFailure(MoveTemp(Diagnostics));
	}
	FString BusyReason;
	if (IsEditorBusy(BusyReason))
	{
		return PreflightFailure({MakeLevelDiagnostic(TEXT(""), TEXT("Level.EditorBusy"), FString::Printf(TEXT("The editor is busy: %s. "
			"Nothing was changed; retry shortly."), *BusyReason))});
	}

	// 2. Parse.
	FLevelEditRequest Request;
	if (!RequestJson.IsValid() || !ParseLevelEditRequest(*RequestJson, Request, Diagnostics))
	{
		if (Diagnostics.IsEmpty())
		{
			Diagnostics.Add(MakeInvalidRequest(TEXT(""), TEXT("The request must be a JSON object.")));
		}
		return PreflightFailure(MoveTemp(Diagnostics));
	}
	UWorld& World = *GEditor->GetEditorWorldContext().World();

	// 3. Preflight on the symbolic plan. The world does not change during preflight, so
	// reference lookups share one gathering of the World Partition descriptors.
	TUniquePtr<FScopedUnloadedActorsCache> DescriptorCache = MakeUnique<FScopedUnloadedActorsCache>(World);
	FLevelEditPlan Plan(World);
	bool bFolderOps = false;
	for (const TUniquePtr<FLevelEditOp>& Op : Request.Ops)
	{
		Plan.CurrentOp = Op->Index;
		Op->Preflight(Plan);
		bFolderOps |= FString(Op->GetKind()).EndsWith(TEXT("_folder"));
	}
	Plan.CurrentOp = INDEX_NONE;
	auto CheckHelperRefs = [&Plan](const TArray<FActorRef>& Refs, const TCHAR* Field)
	{
		for (int32 RefIndex = 0; RefIndex < Refs.Num(); ++RefIndex)
		{
			Plan.Resolve(Refs[RefIndex], FString::Printf(TEXT("/after/%s/%d"), Field, RefIndex));
		}
	};
	CheckHelperRefs(Request.Select, TEXT("select"));
	CheckHelperRefs(Request.Focus, TEXT("focus"));
	DescriptorCache.Reset();
	if (!Plan.Diagnostics.IsEmpty())
	{
		TArray<FAssetDocumentDiagnostic> All = MoveTemp(Plan.Diagnostics);
		All.Append(Plan.Warnings);
		return PreflightFailure(MoveTemp(All));
	}

	const FString Title = Request.Description.IsEmpty()
		? FString::Printf(TEXT("AI level edit (%s)"), *Plural(Request.Ops.Num(), TEXT("op")))
		: Request.Description;

	if (Request.bDryRun)
	{
		TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
		Payload->SetBoolField(TEXT("dry_run"), true);
		Payload->SetStringField(TEXT("description"), Title);
		Payload->SetArrayField(TEXT("changes"), ChangesJson(Plan.Changes));
		Payload->SetNumberField(TEXT("would_spawn"), Plan.Spawns);
		Payload->SetNumberField(TEXT("would_duplicate"), Plan.Duplicates);
		Payload->SetNumberField(TEXT("would_delete"), Plan.Deletes);
		FLevelToolResult Result = FLevelToolResult::Success(FString::Printf(TEXT("Dry run: the batch is valid and would make %d "
			"changes (%d spawned, %d duplicated, %d deleted); nothing was changed. Construction scripts, PostEditChange rules and "
			"placement adjustments run only on apply, where the verify step checks the stored values."), Plan.Changes.Num(),
			Plan.Spawns, Plan.Duplicates, Plan.Deletes), Payload);
		Result.Diagnostics = MoveTemp(Plan.Warnings);
		return Result;
	}

	// 4. Pre-image digest and the dirty state of the packages involved.
	FLevelEditDigestScope Scope;
	Scope.bFolders = bFolderOps;
	const TSet<FString> DirtyLevelPackagesBefore = GatherDirtyLevelPackages(World);
	TMap<FString, bool> DirtyBefore;
	for (const FLevelPlanActor& Entry : Plan.Actors)
	{
		if (const ULevel* Level = Entry.Level.Get())
		{
			Scope.LevelPackages.AddUnique(GetLevelPackageName(*Level));
			DirtyBefore.Add(Level->GetOutermost()->GetName(), Level->GetOutermost()->IsDirty());
		}
		if (AActor* Actor = Entry.bExisting ? Entry.Actor.Get() : nullptr)
		{
			Scope.ActorPaths.AddUnique(Entry.Path);
			DirtyBefore.Add(Actor->GetPackage()->GetName(), Actor->GetPackage()->IsDirty());
		}
	}
	for (const FLevelChange& Change : Plan.Changes)
	{
		if (Change.Object == TEXT("property") && Scope.ActorPaths.Contains(Change.Actor))
		{
			Scope.Values.Add({Change.Actor, Change.Component, Change.Property});
		}
	}
	const FLevelEditDigest PreImage = CaptureLevelEditDigest(World, Scope);

	// 5-6. Apply and verify inside one transaction.
	FLevelEditApply Apply;
	Apply.OpResults.SetNum(Request.Ops.Num());
	Apply.RefPaths = Plan.RefPaths;
	const TCHAR* FailedPhase = nullptr;
	bool bLost = false;
	FAssetDocumentOwnedEditorTransaction Transaction;
	{
		FAssetDocumentEditorNonInteractiveScope NonInteractive;
		if (!Transaction.Begin(FText::FromString(Title)) || !Transaction.IsOutermost())
		{
			Transaction.Rollback();
			return PreflightFailure({MakeLevelDiagnostic(TEXT(""), TEXT("Level.EditorBusy"), TEXT("The editor could not open the "
				"batch transaction (another transaction or an Undo is in progress). Nothing was changed; retry shortly."))});
		}
		const TOptional<int32> InjectAfter = InjectedFailure();
		auto Inject = [&Apply, &FailedPhase](const FString& Path)
		{
			Apply.AddError(Path, TEXT("Level.TestInjectedFailure"), TEXT("Failure injected by a test after this op."));
			FailedPhase = TEXT("apply");
		};
		if (InjectAfter.IsSet() && InjectAfter.GetValue() < 0)
		{
			Inject(TEXT("/ops/0"));
		}
		for (int32 OpIndex = 0; FailedPhase == nullptr && OpIndex < Request.Ops.Num(); ++OpIndex)
		{
			const FLevelEditOp& Op = *Request.Ops[OpIndex];
			if (!Op.Apply(Apply))
			{
				FailedPhase = TEXT("apply");
				if (Apply.Diagnostics.IsEmpty())
				{
					Apply.AddError(Op.Pointer, TEXT("Level.SpawnFailed"), TEXT("The op failed."));
				}
			}
			else if (!Transaction.IsStillActive())
			{
				Apply.AddError(Op.Pointer, TEXT("Level.TransactionLost"), TEXT("An editor step of this op ended or reset the batch "
					"transaction (Undo history); the batch cannot be rolled back as one step. Check the level, and Undo manually if "
					"needed."));
				FailedPhase = TEXT("apply");
				bLost = true;
			}
			else if (InjectAfter.IsSet() && InjectAfter.GetValue() == OpIndex)
			{
				Inject(Op.Pointer);
			}
		}
		if (FailedPhase == nullptr && NonInteractive.WasInteractionRefused())
		{
			Apply.AddError(TEXT(""), TEXT("Level.EditorDialogRefused"), TEXT("An editor step of the batch tried to open a dialog, which "
				"edit_level refuses (it never waits for a person); the result of that step is unknown, so the batch is rolled back."));
			FailedPhase = TEXT("apply");
		}
		if (FailedPhase == nullptr && !Apply.Verify())
		{
			FailedPhase = TEXT("verify");
		}
	}

	// 8. Failure: end, Undo only when the newest entry is ours, check the digest.
	if (FailedPhase != nullptr)
	{
		const EAssetDocumentTransactionRollback Outcome = Transaction.Rollback();
		const bool bReverted = !bLost && (Outcome == EAssetDocumentTransactionRollback::Undone
			|| Outcome == EAssetDocumentTransactionRollback::NothingRecorded);
		TArray<FString> Differences;
		if (bReverted)
		{
			Differences = CompareLevelEditDigests(PreImage, CaptureLevelEditDigest(World, Scope));
		}
		TArray<FAssetDocumentDiagnostic> All = MoveTemp(Apply.Diagnostics);
		TSharedRef<FJsonObject> Payload = FailurePayload(FailedPhase, bReverted && Differences.IsEmpty());
		Payload->SetStringField(TEXT("rollback"), LexToString(Outcome));
		if (bReverted)
		{
			Payload->SetStringField(TEXT("rollback_check"), Differences.IsEmpty() ? TEXT("equal") : TEXT("different"));
		}
		Payload->SetArrayField(TEXT("changes"), ChangesJson(Apply.Changes));
		if (!Differences.IsEmpty())
		{
			TSharedRef<FJsonObject> Details = MakeShared<FJsonObject>();
			Details->SetArrayField(TEXT("differences"), StringArrayJson(Differences));
			All.Add(MakeLevelDiagnostic(TEXT(""), TEXT("Level.RollbackIncomplete"), TEXT("The batch was undone, but the level differs from "
				"before it (details.differences). Nothing more was done automatically: check the listed actors."), Details));
		}
		else if (!bReverted && !bLost)
		{
			All.Add(MakeLevelDiagnostic(TEXT(""), TEXT("Level.RollbackIncomplete"), FString::Printf(TEXT("The batch could not be undone "
				"(%s); its partial changes stay in the level and in the Undo history."), LexToString(Outcome))));
		}
		return FLevelToolResult::Failure(MoveTemp(All), Payload);
	}

	// 7. Commit: one Undo entry.
	bool bUndoable = Transaction.Commit();

	// 9. Post-commit helpers (not part of the transaction). Unloading actors (unpin) can
	// make the engine reset the whole Undo history, as the Outliner's Unpin does
	// (ILoaderAdapter::PostLoadedStateChanged); the batch's entry is then gone too.
	const int32 QueueBeforeHelpers = GEditor->Trans != nullptr ? GEditor->Trans->GetQueueLength() : 0;
	bool bRanHelpers = false;
	for (const TUniquePtr<FLevelEditOp>& Op : Request.Ops)
	{
		if (Op->IsPostCommitHelper())
		{
			Op->PostCommit(Apply);
			bRanHelpers = true;
		}
	}
	if (bRanHelpers && GEditor->Trans != nullptr && GEditor->Trans->GetQueueLength() < QueueBeforeHelpers)
	{
		bUndoable = false;
		Apply.AddWarning(TEXT(""), TEXT("Level.UndoHistoryCleared"), TEXT("Unloading the unpinned actors made the editor clear its "
			"whole Undo history, as the Outliner's Unpin does (this batch's entry included); the edits stay applied."));
	}
	auto ResolveHelperActors = [&Apply, &World](const TArray<FActorRef>& Refs, const TCHAR* Field)
	{
		TArray<AActor*> Actors;
		for (int32 RefIndex = 0; RefIndex < Refs.Num(); ++RefIndex)
		{
			const FActorRef Ref = Apply.PinRef(Refs[RefIndex], FString::Printf(TEXT("/after/%s/%d"), Field, RefIndex));
			const FActorRefResolution Resolution = ResolveActorRef(World, Ref, [&Apply](FStringView Id)
			{
				return Cast<AActor>(Apply.ResolveBatchObject(Id));
			});
			if (AActor* Actor = Resolution.Actor.Get())
			{
				Actors.AddUnique(Actor);
			}
		}
		return Actors;
	};
	UEditorActorSubsystem* ActorSubsystem = GEditor->GetEditorSubsystem<UEditorActorSubsystem>();
	TArray<TSharedPtr<FJsonValue>> Selection;
	if (Request.bHasSelect && ActorSubsystem != nullptr)
	{
		ActorSubsystem->SetSelectedLevelActors(ResolveHelperActors(Request.Select, TEXT("select")));
		for (AActor* Selected : ActorSubsystem->GetSelectedLevelActors())
		{
			Selection.Add(MakeShared<FJsonValueString>(GetActorPath(*Selected)));
		}
	}
	if (!Request.Focus.IsEmpty())
	{
		const TArray<AActor*> FocusActors = ResolveHelperActors(Request.Focus, TEXT("focus"));
		if (!FocusActors.IsEmpty())
		{
			GEditor->MoveViewportCamerasToActor(FocusActors, /*bActiveViewportOnly*/ true);
		}
	}

	// The navigation actors the engine adds for what the batch placed (the RecastNavMesh a first Nav Mesh Bounds Volume
	// brings) are made on the navigation system's next tick: it runs now, so they are among the packages the batch
	// dirtied (and saved with them).
	if (UNavigationSystemV1* Navigation = FNavigationSystem::GetCurrent<UNavigationSystemV1>(&World);
		Navigation != nullptr && !Navigation->PendingNavBoundsUpdates.IsEmpty())
	{
		Navigation->Tick(0.0f);
	}

	// Response.
	const TArray<FString> DirtyPackages = CollectDirtyPackages(Apply, DirtyLevelPackagesBefore, GatherDirtyLevelPackages(World));
	TArray<FString> AlreadyDirty;
	for (const FString& Package : DirtyPackages)
	{
		const bool* bWasDirty = DirtyBefore.Find(Package);
		if ((bWasDirty != nullptr && *bWasDirty) || DirtyLevelPackagesBefore.Contains(Package))
		{
			AlreadyDirty.Add(Package);
		}
	}
	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	TSharedRef<FJsonObject> TransactionJson = MakeShared<FJsonObject>();
	TransactionJson->SetStringField(TEXT("title"), Title);
	TransactionJson->SetBoolField(TEXT("undoable"), bUndoable);
	// What undo_editor takes to undo exactly this batch (never another entry).
	if (bUndoable)
	{
		TransactionJson->SetStringField(TEXT("id"), FormatUndoEntryId(Transaction.GetTransactionId()));
	}
	Payload->SetObjectField(TEXT("transaction"), TransactionJson);
	TSharedRef<FJsonObject> ActorsJson = MakeShared<FJsonObject>();
	for (const TPair<FString, FString>& Entry : Apply.BatchPaths)
	{
		if (AActor* Actor = Apply.ResolvePath(Entry.Value))
		{
			ActorsJson->SetObjectField(Entry.Key, MakeActorRefJson(*Actor));
		}
	}
	Payload->SetObjectField(TEXT("actors"), ActorsJson);
	TArray<TSharedPtr<FJsonValue>> OpsJson;
	for (const TSharedPtr<FJsonObject>& OpResult : Apply.OpResults)
	{
		TSharedRef<FJsonObject> Out = OpResult.IsValid() ? OpResult.ToSharedRef() : MakeShared<FJsonObject>();
		FString Created;
		if (Out->TryGetStringField(TEXT("created"), Created))
		{
			if (AActor* Actor = Apply.ResolvePath(Created))
			{
				Out->SetObjectField(TEXT("created"), MakeActorRefJson(*Actor));
			}
		}
		OpsJson.Add(MakeShared<FJsonValueObject>(Out));
	}
	Payload->SetArrayField(TEXT("ops"), OpsJson);
	Payload->SetArrayField(TEXT("changes"), ChangesJson(Apply.Changes));
	Payload->SetArrayField(TEXT("dirty_packages"), StringArrayJson(DirtyPackages));
	Payload->SetArrayField(TEXT("already_dirty"), StringArrayJson(AlreadyDirty));
	if (Request.bHasSelect)
	{
		Payload->SetArrayField(TEXT("selection"), Selection);
	}
	if (!Apply.Pinned.IsEmpty())
	{
		Payload->SetArrayField(TEXT("pinned"), Apply.Pinned);
	}
	if (!Apply.Unpinned.IsEmpty())
	{
		Payload->SetArrayField(TEXT("unpinned"), Apply.Unpinned);
	}

	FString Message = Apply.Changes.IsEmpty() ? FString(TEXT("Level edit changed nothing (no Undo entry)."))
		: FString::Printf(TEXT("Level edit applied: %s (undo: '%s')."), *DescribeCounts(Apply), *Title);
	if (!Apply.Pinned.IsEmpty() || !Apply.Unpinned.IsEmpty())
	{
		Message += FString::Printf(TEXT(" %d pinned, %d unpinned (not in Undo)."), Apply.Pinned.Num(), Apply.Unpinned.Num());
	}
	TArray<FAssetDocumentDiagnostic> ResultDiagnostics = MoveTemp(Plan.Warnings);
	ResultDiagnostics.Append(Apply.Warnings);
	// An empty folder of a level without actor folder objects is not saved with anything (as in the Outliner).
	TArray<FString> SessionOnly;
	for (const FString& Folder : Apply.SessionOnlyFolders)
	{
		bool bHasActor = false;
		for (TActorIterator<AActor> It(&World); It && !bHasActor; ++It)
		{
			const FString ActorFolder = It->GetFolderPath().ToString();
			bHasActor = ActorFolder == Folder || ActorFolder.StartsWith(Folder + TEXT("/"));
		}
		if (!bHasActor && FActorFolders::Get().ContainsFolder(World, FFolder(FFolder::GetWorldRootFolder(&World).GetRootObject(), FName(*Folder))))
		{
			SessionOnly.Add(Folder);
		}
	}
	if (!SessionOnly.IsEmpty())
	{
		TSharedRef<FJsonObject> Details = MakeShared<FJsonObject>();
		Details->SetArrayField(TEXT("folders"), StringArrayJson(SessionOnly));
		FAssetDocumentDiagnostic Warning = MakeLevelDiagnostic(TEXT(""), TEXT("Level.FolderNotSaved"), FString::Printf(TEXT("Folder(s) %s hold no actor. "
			"This level has no actor folder objects (no World Partition): its folders are the folder paths of its actors, so the "
			"editor keeps an empty folder for this session only and no save keeps it (the Outliner does the same). Put an actor "
			"in it (set {\"folder\"}, spawn \"folder\") to keep it."), *FString::Join(SessionOnly, TEXT(", "))), Details);
		Warning.Severity = TEXT("warning");
		ResultDiagnostics.Add(MoveTemp(Warning));
	}

	// 10. Save (only when asked; exactly the packages the batch dirtied).
	TArray<FString> Saved;
	int32 HttpStatus = 200;
	if (Request.bSave && !DirtyPackages.IsEmpty())
	{
		TArray<UPackage*> ToSave;
		for (const FString& Name : DirtyPackages)
		{
			if (UPackage* Package = FindPackage(nullptr, *Name))
			{
				ToSave.Add(Package);
			}
		}
		TArray<UPackage*> Failed;
		{
			FAssetDocumentEditorNonInteractiveScope NonInteractive;
			FEditorFileUtils::PromptForCheckoutAndSave(ToSave, /*bCheckDirty*/ false, /*bPromptToSave*/ false, &Failed,
				/*bAlreadyCheckedOut*/ false, /*bCanBeDeclined*/ false);
		}
		for (UPackage* Package : ToSave)
		{
			if (!Failed.Contains(Package) && !Package->IsDirty())
			{
				Saved.Add(Package->GetName());
			}
		}
		if (Saved.Num() != ToSave.Num())
		{
			TArray<FString> NotSaved;
			for (UPackage* Package : ToSave)
			{
				if (!Saved.Contains(Package->GetName()))
				{
					NotSaved.Add(Package->GetName());
				}
			}
			TSharedRef<FJsonObject> Details = MakeShared<FJsonObject>();
			Details->SetArrayField(TEXT("not_saved"), StringArrayJson(NotSaved));
			ResultDiagnostics.Add(MakeLevelDiagnostic(TEXT("/save"), TEXT("Level.SaveFailed"), FString::Printf(TEXT("The edit is applied and "
				"in the Undo history, but %d of %d packages were not saved (details.not_saved); save them from the editor."),
				NotSaved.Num(), ToSave.Num()), Details));
			HttpStatus = 207;
			Message += TEXT(" Saving failed for some packages.");
		}
		else
		{
			Message += FString::Printf(TEXT(" Saved %s."), *Plural(Saved.Num(), TEXT("package")));
		}
	}
	else if (Request.bSave)
	{
		Message += TEXT(" Nothing to save: the batch dirtied no package.");
	}
	Payload->SetArrayField(TEXT("saved_packages"), StringArrayJson(Saved));

	FLevelToolResult Result = FLevelToolResult::Success(Message, Payload);
	// A failed save is a partial failure (AssetDocument's 207): the edit stays applied.
	Result.HttpStatus = HttpStatus;
	Result.bSuccess = HttpStatus == 200;
	Result.Diagnostics = MoveTemp(ResultDiagnostics);
	return Result;
}
}
