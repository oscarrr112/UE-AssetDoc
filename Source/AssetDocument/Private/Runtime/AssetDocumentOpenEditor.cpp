// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "Runtime/AssetDocumentOpenEditor.h"

#include "Editor.h"
#include "Framework/Commands/InputBindingManager.h"
#include "Framework/Commands/UICommandInfo.h"
#include "Framework/Commands/UICommandList.h"
#include "Misc/CoreDelegates.h"
#include "Misc/EngineVersionComparison.h"
#include "Misc/ScopeExit.h"
#include "Misc/PackageName.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Toolkits/AssetEditorToolkit.h"
#include "UObject/Package.h"
#include "UObject/ObjectKey.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UObjectGlobals.h"

namespace
{
TArray<TSharedRef<IAssetDocumentOpenEditorHandler>>& GetOpenEditorHandlers()
{
	static TArray<TSharedRef<IAssetDocumentOpenEditorHandler>> Handlers;
	return Handlers;
}

UAssetEditorSubsystem* AssetEditors()
{
	return GEditor != nullptr && !IsRunningCommandlet() ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;
}

// Packages an asset editor changed while opening (OpenEditor::HasOnlyOpenTimeChanges).
struct FOpenTimeChanges
{
	// Clean when an editor was requested to open on them, until the editor opened.
	TSet<FObjectKey> Opening;
	// Dirty only from an editor's open: nothing marked them dirty since.
	TSet<FObjectKey> Dirty;
	bool bTracking = false;
};

FOpenTimeChanges& OpenTimeChanges()
{
	static FOpenTimeChanges State;
	return State;
}

// Anything that marks the package dirty outside an editor's open is a change of its own.
void ForgetOpenTimeChanges(const UPackage* Package)
{
	FOpenTimeChanges& State = OpenTimeChanges();
	if (Package != nullptr && !State.Opening.Contains(FObjectKey(Package)))
	{
		State.Dirty.Remove(FObjectKey(Package));
	}
}

void StartOpenTimeTracking()
{
	FOpenTimeChanges& State = OpenTimeChanges();
	UAssetEditorSubsystem* Editors = GEditor != nullptr ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;
	if (State.bTracking || Editors == nullptr)
	{
		return;
	}
	State.bTracking = true;
	Editors->OnAssetEditorRequestedOpen().AddLambda([](UObject* Asset)
	{
		if (Asset != nullptr)
		{
			const UPackage* Package = Asset->GetOutermost();
			OpenTimeChanges().Opening.Remove(FObjectKey(Package));
			if (!Package->IsDirty())
			{
				OpenTimeChanges().Opening.Add(FObjectKey(Package));
			}
		}
	});
	Editors->OnAssetEditorOpened().AddLambda([](UObject* Asset)
	{
		if (Asset != nullptr)
		{
			const UPackage* Package = Asset->GetOutermost();
			FOpenTimeChanges& Changes = OpenTimeChanges();
			if (Changes.Opening.Remove(FObjectKey(Package)) > 0 && Package->IsDirty())
			{
				Changes.Dirty.Add(FObjectKey(Package));
			}
		}
	});
	UPackage::PackageMarkedDirtyEvent.AddLambda([](UPackage* Package, bool)
	{
		ForgetOpenTimeChanges(Package);
	});
	UPackage::PackageDirtyStateChangedEvent.AddLambda([](UPackage* Package)
	{
		if (Package != nullptr && !Package->IsDirty())
		{
			OpenTimeChanges().Dirty.Remove(FObjectKey(Package));
		}
	});
	FCoreUObjectDelegates::OnObjectModified.AddLambda([](UObject* Object)
	{
		ForgetOpenTimeChanges(Object != nullptr ? Object->GetPackage() : nullptr);
	});
}

FString OpenEditorObjectPath(const FString& Target)
{
	if (Target.IsEmpty() || Target.Contains(TEXT(".")))
	{
		return Target;
	}
	return Target + TEXT(".") + FPackageName::GetShortName(Target);
}
} // namespace

bool RegisterAssetDocumentOpenEditorHandler(TSharedRef<IAssetDocumentOpenEditorHandler> Handler)
{
	check(IsInGameThread());
	if (GEditor != nullptr)
	{
		StartOpenTimeTracking();
	}
	else if (!OpenTimeChanges().bTracking)
	{
		static bool bDeferred = false;
		if (!bDeferred)
		{
			bDeferred = true;
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
			FCoreDelegates::GetOnPostEngineInit().AddLambda([]() { StartOpenTimeTracking(); });
#else
			FCoreDelegates::OnPostEngineInit.AddLambda([]() { StartOpenTimeTracking(); });
#endif
		}
	}
	const FName Id = Handler->GetId();
	TArray<TSharedRef<IAssetDocumentOpenEditorHandler>>& Handlers = GetOpenEditorHandlers();
	if (Id.IsNone() || Handlers.ContainsByPredicate([Id](const TSharedRef<IAssetDocumentOpenEditorHandler>& Existing)
		{
			return Existing->GetId() == Id;
		}))
	{
		return false;
	}
	Handlers.Add(MoveTemp(Handler));
	return true;
}

bool UnregisterAssetDocumentOpenEditorHandler(FName HandlerId)
{
	check(IsInGameThread());
	return GetOpenEditorHandlers().RemoveAll([HandlerId](const TSharedRef<IAssetDocumentOpenEditorHandler>& Existing)
	{
		return Existing->GetId() == HandlerId;
	}) > 0;
}

namespace AssetDocumentOpenEditorLocal
{
// The command the editor named EditorName open on the asset has mapped and can execute now (FUICommandList::
// CanExecuteAction is true for an unmapped command, so the mapping is checked first).
bool FindEditorCommand(const UObject& Asset, FName EditorName, FName BindingContext, FName CommandName,
	TSharedPtr<FUICommandList>& OutCommands, TSharedPtr<FUICommandInfo>& OutCommand)
{
	UAssetEditorSubsystem* Editors = AssetEditors();
	const TSharedPtr<FUICommandInfo> Command = Editors != nullptr
		? FInputBindingManager::Get().FindCommandInContext(BindingContext, CommandName) : nullptr;
	if (!Command.IsValid())
	{
		return false;
	}
	for (IAssetEditorInstance* Instance : Editors->FindEditorsForAsset(const_cast<UObject*>(&Asset)))
	{
		if (Instance == nullptr || Instance->GetEditorName() != EditorName)
		{
			continue;
		}
		// (The asset editors are asset editor toolkits.)
		const TSharedRef<FUICommandList> Commands = static_cast<FAssetEditorToolkit*>(Instance)->GetToolkitCommands();
		if (Commands->IsActionMapped(Command) && Commands->CanExecuteAction(Command.ToSharedRef()))
		{
			OutCommands = Commands;
			OutCommand = Command;
			return true;
		}
	}
	return false;
}

// The handler of an asset open in an editor (or shown in one): a non-fallback handler wins over a fallback one.
TSharedPtr<IAssetDocumentOpenEditorHandler> FindHandler(const UObject& Asset, bool bOpen)
{
	TSharedPtr<IAssetDocumentOpenEditorHandler> Handler;
	for (const TSharedRef<IAssetDocumentOpenEditorHandler>& Candidate : GetOpenEditorHandlers())
	{
		if (Candidate->Supports(Asset) && (bOpen || Candidate->IsShownInOpenEditor(Asset)))
		{
			if (!Handler.IsValid() || (Handler->IsFallback() && !Candidate->IsFallback()))
			{
				Handler = Candidate;
			}
			if (!Candidate->IsFallback())
			{
				break;
			}
		}
	}
	return Handler;
}
} // namespace AssetDocumentOpenEditorLocal

namespace UE::AssetDocument::OpenEditor
{
namespace OpenEditorLocal
{
// The innermost FScopedApply of the apply running now (BeforeChange).
FScopedApply*& ActiveScope()
{
	static FScopedApply* Active = nullptr;
	return Active;
}
}

void BeforeChange()
{
	using namespace OpenEditorLocal;
	if (FScopedApply* Active = ActiveScope())
	{
		Active->CloseBeforeChange();
	}
}

bool IsClosePending()
{
	using namespace OpenEditorLocal;
	const FScopedApply* Active = ActiveScope();
	return Active != nullptr && Active->IsClosePending();
}

bool IsOpen(const UObject& Asset)
{
	UAssetEditorSubsystem* Editors = AssetEditors();
	return Editors != nullptr && Editors->FindEditorForAsset(const_cast<UObject*>(&Asset), false) != nullptr;
}

bool CanExecuteEditorCommand(const UObject& Asset, FName EditorName, FName BindingContext, FName CommandName)
{
	using namespace AssetDocumentOpenEditorLocal;
	TSharedPtr<FUICommandList> Commands;
	TSharedPtr<FUICommandInfo> Command;
	return FindEditorCommand(Asset, EditorName, BindingContext, CommandName, Commands, Command);
}

bool ExecuteEditorCommand(const UObject& Asset, FName EditorName, FName BindingContext, FName CommandName)
{
	using namespace AssetDocumentOpenEditorLocal;
	TSharedPtr<FUICommandList> Commands;
	TSharedPtr<FUICommandInfo> Command;
	return FindEditorCommand(Asset, EditorName, BindingContext, CommandName, Commands, Command)
		&& Commands->ExecuteAction(Command.ToSharedRef());
}

bool CloseEditorDiscardingPendingEdits(const UObject& Asset, FName EditorName)
{
	UAssetEditorSubsystem* Editors = AssetEditors();
	if (Editors == nullptr)
	{
		return false;
	}
	for (IAssetEditorInstance* Instance : Editors->FindEditorsForAsset(const_cast<UObject*>(&Asset)))
	{
		if (Instance != nullptr && Instance->GetEditorName() == EditorName)
		{
			Instance->CloseWindow(EAssetEditorCloseReason::AssetForceDeleted);
		}
	}
	for (IAssetEditorInstance* Instance : Editors->FindEditorsForAsset(const_cast<UObject*>(&Asset)))
	{
		if (Instance != nullptr && Instance->GetEditorName() == EditorName)
		{
			return false;
		}
	}
	return true;
}

FString GetPendingEditsButton(const UObject& Asset)
{
	const TSharedPtr<IAssetDocumentOpenEditorHandler> Handler = IsOpen(Asset) ? AssetDocumentOpenEditorLocal::FindHandler(Asset, true) : nullptr;
	return Handler.IsValid() ? Handler->GetPendingEditsButton(Asset) : FString();
}

bool ApplyPendingEdits(UObject& Asset, FString& OutMessage)
{
	const TSharedPtr<IAssetDocumentOpenEditorHandler> Handler = IsOpen(Asset) ? AssetDocumentOpenEditorLocal::FindHandler(Asset, true) : nullptr;
	if (!Handler.IsValid() || Handler->GetPendingEditsButton(Asset).IsEmpty())
	{
		OutMessage = TEXT("its editor holds no unapplied edits");
		return false;
	}
	return Handler->ApplyPendingEdits(Asset, OutMessage);
}

bool CloseDiscardingPendingEdits(UObject& Asset, FString& OutMessage)
{
	const TSharedPtr<IAssetDocumentOpenEditorHandler> Handler = IsOpen(Asset) ? AssetDocumentOpenEditorLocal::FindHandler(Asset, true) : nullptr;
	if (!Handler.IsValid())
	{
		OutMessage = TEXT("no editor with unapplied edits of its own is open on it");
		return false;
	}
	return Handler->CloseDiscardingPendingEdits(Asset, OutMessage);
}

FString DescribePendingEditsWayOut(bool bCanDiscard)
{
	return bCanDiscard
		? FString(TEXT("To keep them: asset_editor apply_editor (the editor's own Apply), then save_assets and extract the asset "
			"again. To drop them: asset_editor close with discard_unapplied: true."))
		: FString(TEXT("To keep them: asset_editor apply_editor (the editor's own Apply), then save_assets and extract the asset "
			"again. To drop them (this editor cannot close without asking): asset_editor apply_editor, asset_editor close, "
			"then reload_assets with force: true."));
}

FString DescribePendingEditsWayOut(const UObject& Asset)
{
	const TSharedPtr<IAssetDocumentOpenEditorHandler> Handler = AssetDocumentOpenEditorLocal::FindHandler(Asset, true);
	return DescribePendingEditsWayOut(Handler.IsValid() && Handler->CanCloseDiscardingPendingEdits(Asset));
}

bool HasOnlyOpenTimeChanges(const UPackage& Package)
{
	return Package.IsDirty() && OpenTimeChanges().Dirty.Contains(FObjectKey(&Package));
}

FScopedApply::FScopedApply(const FString& Target)
	: ObjectPath(OpenEditorObjectPath(Target))
{
	{
		using namespace OpenEditorLocal;
		PreviousActive = ActiveScope();
		ActiveScope() = this;
	}
	UAssetEditorSubsystem* Editors = AssetEditors();
	UObject* Asset = Editors != nullptr && !ObjectPath.IsEmpty() ? FSoftObjectPath(ObjectPath).ResolveObject() : nullptr;
	if (Asset == nullptr)
	{
		return;
	}
	const bool bOpen = IsOpen(*Asset);
	Handler = AssetDocumentOpenEditorLocal::FindHandler(*Asset, bOpen);
	if (!Handler.IsValid())
	{
		return;
	}
	ProfileName = Handler->GetProfileName(*Asset);
	if (TOptional<FAssetDocumentDiagnostic> Refused = Handler->CheckBeforeApply(*Asset))
	{
		FAssetDocumentResult Result = FAssetDocumentResult::FailureFromDiagnostics({Refused.GetValue()}, Refused->Message);
		Result.Target = FPackageName::ObjectPathToPackageName(ObjectPath);
		Result.AssetPath = ObjectPath;
		Refusal = MoveTemp(Result);
		Handler.Reset();
		return;
	}
	// Changes the editor made while opening, which the apply rebuilds: the package counts as saved for the apply (its
	// save writes them with the apply's changes); a failed apply leaves it dirty again.
	UPackage* Package = Asset->GetOutermost();
	if (Handler->AcceptsOpenTimeChanges(*Asset) && HasOnlyOpenTimeChanges(*Package))
	{
		bRestoreOpenTimeDirty = true;
		Package->SetDirtyFlag(false);
	}
	// A target with unsaved changes stays open: the apply refuses it before any change. Otherwise the editor is closed
	// once the apply is about to change the asset (CloseBeforeChange): an apply that fails validation or preflight, or
	// changes nothing, leaves it open and untouched.
	if (bOpen && Handler->GetStrategy(*Asset) == EAssetDocumentOpenEditorStrategy::CloseApplyReopen && !Asset->GetOutermost()->IsDirty())
	{
		bClosePending = true;
	}
}

FScopedApply::~FScopedApply()
{
	if (!bFinished && (bReopen || Handler.IsValid()))
	{
		FAssetDocumentResult Ignored;
		Finish(Ignored);
	}
	using namespace OpenEditorLocal;
	if (ActiveScope() == this)
	{
		ActiveScope() = PreviousActive;
	}
}

void FScopedApply::CloseBeforeChange()
{
	if (!bClosePending)
	{
		return;
	}
	bClosePending = false;
	UObject* Asset = FSoftObjectPath(ObjectPath).ResolveObject();
	UAssetEditorSubsystem* Editors = AssetEditors();
	if (Asset != nullptr && Editors != nullptr && IsOpen(*Asset))
	{
		bReopen = true;
		Editors->CloseAllEditorsForAsset(Asset);
	}
}

void FScopedApply::Finish(FAssetDocumentResult& Result)
{
	if (bFinished || !Handler.IsValid())
	{
		return;
	}
	bFinished = true;
	UObject* Asset = FSoftObjectPath(ObjectPath).ResolveObject();
	UAssetEditorSubsystem* Editors = AssetEditors();
	if (Asset == nullptr || Editors == nullptr)
	{
		return;
	}
	ON_SCOPE_EXIT
	{
		// A failed (rolled back) or unchanged (no-op) apply leaves the package as saved: the editor's open-time changes
		// are unsaved again, as before the apply. An apply that changed the package without saving it leaves the
		// package dirty with changes of its own, which no longer count as open-time changes.
		UPackage* Package = Asset->GetOutermost();
		if (bRestoreOpenTimeDirty && !Package->IsDirty() && (!Result.IsSuccess() || !Result.bSavedAsset))
		{
			Package->SetDirtyFlag(true);
			OpenTimeChanges().Dirty.Add(FObjectKey(Package));
		}
	};
	if (bClosePending)
	{
		// The apply never got to change the asset (refused, failed before any change, or a no-op): the editor stayed
		// open and needs nothing. Defensive: an apply that saved changes without announcing them (BeforeChange) gets
		// the editor closed and reopened now, rather than a stale editor.
		if (!(Result.IsSuccess() && Result.bSavedAsset))
		{
			bClosePending = false;
			return;
		}
		CloseBeforeChange();
	}
	if (!bReopen)
	{
		if (IsOpen(*Asset) || Handler->IsShownInOpenEditor(*Asset))
		{
			Handler->RefreshOpenEditor(*Asset, Result.IsSuccess());
		}
		return;
	}
	const bool bOpened = !IsOpen(*Asset) && Editors->OpenEditorForAsset(Asset);
	FAssetDocumentDiagnostic Diagnostic;
	Diagnostic.Path = TEXT("/Target");
	Diagnostic.Code = ProfileName + TEXT(".EditorReopened");
	Diagnostic.Severity = TEXT("info");
	Diagnostic.Message = bOpened
		? FString::Printf(TEXT("The %s editor was closed for this apply and reopened on the %s asset."), *ProfileName,
			Result.IsSuccess() ? TEXT("applied") : TEXT("unchanged"))
		: FString::Printf(TEXT("The %s editor was closed for this apply and could not be reopened; open the asset again."), *ProfileName);
	Result.Diagnostics.Add(MoveTemp(Diagnostic));
}
} // namespace UE::AssetDocument::OpenEditor
