// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "AssetDocumentTypes.h"

class UObject;
class UPackage;

// Applying to an asset that is open in its asset editor (open-editor apply).
//
// After such an apply the editor shows what a human would see after making the same edit in it.
// A provider registers a handler for the asset classes it owns; FAssetDocumentService::ApplyFile
// asks the handler of the target (when the target is loaded and open in an asset editor):
//   - RefreshInPlace: the apply runs as usual; afterwards (success, or failure after the
//     rollback) the handler refreshes the open editor the way the editor refreshes itself
//     after its own Undo (graph panels, trees, timelines, preview).
//   - CloseApplyReopen: the editor holds state the apply would leave stale and that cannot be
//     refreshed cleanly (e.g. a graph object the apply replaces). The editor is closed once the
//     apply is about to change the asset (BeforeChange: after validation, the no-op check,
//     preflight and, for these profiles, Validate's trial apply on a transient copy) and reopened on the same asset afterwards; the result carries Info
//     <Profile>.EditorReopened. An apply that fails before that, or changes nothing, leaves the
//     editor open and untouched. A target with unsaved changes is left open (the apply
//     refuses it with DirtyExistingAssetTransactionUnsupported before any change).
// A handler may refuse the apply before any change (CheckBeforeApply), e.g. with
// <Profile>.OpenInEditorUnsavedEdits when the editor holds pending edits of its own.
// Some editors change their asset while opening (the EQS editor recomputes the test weights its graph displays, the
// Behavior Tree editor upgrades its graph's version): the package was clean before the open and is dirty after it,
// although the user changed nothing. A handler that knows this change is editor-only graph data the apply rebuilds
// anyway (AcceptsOpenTimeChanges) lets the apply run on such a package; it is saved with the apply. As soon as
// anything else marks the package dirty (a user edit, an undo), the usual rule applies again.
// Targets without a handler keep the plain apply (no editor handling).
enum class EAssetDocumentOpenEditorStrategy : uint8
{
	RefreshInPlace,
	CloseApplyReopen,
};

class ASSETDOCUMENT_API IAssetDocumentOpenEditorHandler
{
public:
	virtual ~IAssetDocumentOpenEditorHandler() = default;
	virtual FName GetId() const = 0;
	virtual bool Supports(const UObject& Asset) const = 0;
	// The diagnostic prefix of the profile ("SoundCue", "BehaviorTree", ...).
	virtual FString GetProfileName(const UObject& Asset) const = 0;
	virtual EAssetDocumentOpenEditorStrategy GetStrategy(const UObject& Asset) const = 0;
	// Called with the editor open, before any change. A returned diagnostic refuses the apply.
	virtual TOptional<FAssetDocumentDiagnostic> CheckBeforeApply(const UObject& Asset) const { return {}; }
	// RefreshInPlace: after the apply (bSucceeded false: the asset was rolled back or left
	// unchanged), with the editor still open.
	virtual void RefreshOpenEditor(UObject& Asset, bool bSucceeded) const {}
	// True when the changes the editor makes to the asset while opening are editor-only data the apply rebuilds.
	virtual bool AcceptsOpenTimeChanges(const UObject& Asset) const { return false; }
	// True when an editor open on another asset shows this one (a sub-sequence an open Sequencer plays). The handler
	// then runs although no editor is open on the asset itself: CheckBeforeApply and RefreshOpenEditor (no reopen).
	virtual bool IsShownInOpenEditor(const UObject& Asset) const { return false; }
	// A fallback handler (for example one for every UBlueprint) is used only when no other handler supports the asset,
	// so a handler for a subclass (UControlRigBlueprint) wins whatever the module registration order.
	virtual bool IsFallback() const { return false; }

	// Pending edits (asset_editor): an editor that edits a working copy of the asset holds edits not yet written to it.
	// The editor button that writes them ("Apply", "Apply Scratch"), empty when the open editor holds none.
	virtual FString GetPendingEditsButton(const UObject& Asset) const { return FString(); }
	// Presses that button in the open editor (asset_editor apply_editor). False with OutMessage when it could not.
	virtual bool ApplyPendingEdits(UObject& Asset, FString& OutMessage) const
	{
		OutMessage = TEXT("this editor has no Apply of its own");
		return false;
	}
	// Closes the open editor without writing its pending edits: the "No" of its close prompt (asset_editor close with
	// discard_unapplied). False with OutMessage when the editor cannot close without asking.
	virtual bool CloseDiscardingPendingEdits(UObject& Asset, FString& OutMessage) const
	{
		OutMessage = TEXT("this editor cannot close without asking about its unapplied edits");
		return false;
	}
	// CloseDiscardingPendingEdits can succeed (for the refusal messages).
	virtual bool CanCloseDiscardingPendingEdits(const UObject& Asset) const { return false; }
};

// Registration is game-thread only. Returns false for an empty or duplicate id.
ASSETDOCUMENT_API bool RegisterAssetDocumentOpenEditorHandler(TSharedRef<IAssetDocumentOpenEditorHandler> Handler);
ASSETDOCUMENT_API bool UnregisterAssetDocumentOpenEditorHandler(FName HandlerId);

namespace UE::AssetDocument::OpenEditor
{
// True when an asset editor is open on the asset.
ASSETDOCUMENT_API bool IsOpen(const UObject& Asset);

// True when an asset editor named EditorName (IAssetEditorInstance::GetEditorName) is open on the asset and has mapped
// the command (its binding context and name, e.g. "MaterialEditor" / "Apply") in its toolkit commands, and the command
// can execute now. An editor that edits a working copy of the asset enables its Apply only while the copy holds edits
// not yet written to the asset: the editor's own pending edits (<Profile>.OpenInEditorUnsavedEdits).
ASSETDOCUMENT_API bool CanExecuteEditorCommand(const UObject& Asset, FName EditorName, FName BindingContext, FName CommandName);

// Pending edits of the asset's open editor, through the handler of the asset (asset_editor close / apply_editor):
// the button that writes them (empty: none, or no handler knows), the editor's Apply pressed, the editor closed without
// them. False with OutMessage when the editor cannot.
ASSETDOCUMENT_API FString GetPendingEditsButton(const UObject& Asset);
ASSETDOCUMENT_API bool ApplyPendingEdits(UObject& Asset, FString& OutMessage);
ASSETDOCUMENT_API bool CloseDiscardingPendingEdits(UObject& Asset, FString& OutMessage);
// What the AI can do about them, in the MCP tools' words: one sentence for the refusals
// (<Profile>.OpenInEditorUnsavedEdits, Assets.EditorNotClosed). bCanDiscard: the editor closes without them.
ASSETDOCUMENT_API FString DescribePendingEditsWayOut(bool bCanDiscard);
ASSETDOCUMENT_API FString DescribePendingEditsWayOut(const UObject& Asset);

// For handlers: runs the command CanExecuteEditorCommand finds (true when it ran), and closes the editors named
// EditorName open on the asset as a forced delete closes them (EAssetEditorCloseReason::AssetForceDeleted: an editor
// that would ask whether to apply its unapplied edits closes without them; true when none of them is left open).
ASSETDOCUMENT_API bool ExecuteEditorCommand(const UObject& Asset, FName EditorName, FName BindingContext, FName CommandName);
ASSETDOCUMENT_API bool CloseEditorDiscardingPendingEdits(const UObject& Asset, FName EditorName);

// True when the package is dirty only because an asset editor opened on it (it was clean before the open and
// nothing marked it dirty since). Tracked from the first handler registration on.
ASSETDOCUMENT_API bool HasOnlyOpenTimeChanges(const UPackage& Package);

// Called by the apply once the document passed validation, the no-op check and preflight, right before it changes the
// asset: the innermost FScopedApply closes a CloseApplyReopen editor now (CloseBeforeChange). Nothing without one.
ASSETDOCUMENT_API void BeforeChange();
// True while the innermost FScopedApply holds an open CloseApplyReopen editor that BeforeChange would close. The apply
// then runs Validate's trial first, so a document error found only while editing leaves the editor open.
ASSETDOCUMENT_API bool IsClosePending();

// The open-editor handling of one apply (used by FAssetDocumentService::ApplyFile).
class ASSETDOCUMENT_API FScopedApply
{
public:
	// Target: the document's Target (package or object path).
	explicit FScopedApply(const FString& Target);
	~FScopedApply();
	FScopedApply(const FScopedApply&) = delete;
	FScopedApply& operator=(const FScopedApply&) = delete;

	// Set when the handler refused the apply before any change.
	const TOptional<FAssetDocumentResult>& GetRefusal() const { return Refusal; }
	// The apply is about to change the asset (BeforeChange): a CloseApplyReopen editor is closed (reopened by Finish).
	void CloseBeforeChange();
	bool IsClosePending() const { return bClosePending; }
	// After the apply: refreshes or reopens the editor and adds diagnostics to Result.
	void Finish(FAssetDocumentResult& Result);

private:
	FString ObjectPath;
	TSharedPtr<IAssetDocumentOpenEditorHandler> Handler;
	FString ProfileName;
	// A CloseApplyReopen editor that is open: closed by CloseBeforeChange, left open when the apply never gets there.
	bool bClosePending = false;
	FScopedApply* PreviousActive = nullptr;
	bool bReopen = false;
	// The package was dirty only from the editor's open; its dirty flag is restored when the apply saves nothing.
	bool bRestoreOpenTimeDirty = false;
	bool bFinished = false;
	TOptional<FAssetDocumentResult> Refusal;
};
}
