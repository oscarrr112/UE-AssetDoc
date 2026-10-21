// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// Open-editor apply for the Niagara assets (Runtime/AssetDocumentOpenEditor.h). Behaviour tests:
// UEAssetDoc.Niagara.OpenEditorApply.*.
//
// System, emitter asset, script: close, apply, reopen (Niagara.EditorReopened).
//   System: the system editor (FNiagaraSystemToolkit, SystemAsset mode) shows the system through its own view models
//     (emitter handles, stacks, overview, selection, documents) and keeps an edit copy of each scratch pad script,
//     which its "Apply Scratch" writes back. An apply edits the system through a view model of its own; the editor's
//     scratch pad view models keep their copies across a refresh (UNiagaraScratchPadViewModel::RefreshScriptViewModels
//     reuses a pad's view model in system mode), so a refreshed editor could still write an old pad back.
//   Emitter asset: the emitter editor (FNiagaraSystemToolkit, EmitterAsset mode) edits a copy of the emitter in a
//     transient placeholder system and writes it over the asset on its Apply (UpdateOriginalEmitter); nothing seeds
//     the copy again from the asset.
//   Script: the script editor (FNiagaraScriptToolkit) edits a duplicate of the script and writes it over the asset on
//     its Apply (UpdateOriginalNiagaraScript); its Apply button follows the copy's change id against the asset's, so
//     after an apply it would offer to write the old copy back.
//   The reopened editor builds its view models and copies from the applied asset. While the editor holds edits of its
//   own that are not applied yet (the emitter or script editor's Apply, or the system editor's Apply Scratch, is
//   enabled), closing it would ask to apply or discard them: the apply is refused before any change
//   (Niagara.OpenInEditorUnsavedEdits), naming asset_editor's ways out: apply_editor (the editor's Apply / Apply
//   Scratch), and for a system or emitter close with discard_unapplied (a forced close skips the prompts). Direct edits of a system dirty its package (DirtyExistingAssetTransactionUnsupported).
// Parameter collection: in place, refreshed. The collection editor (FNiagaraParameterCollectionToolkit) edits the
//   collection itself; its view model rebuilds its parameter list when the collection reports an external change
//   (UNiagaraParameterCollection::OnCollectionChangedDelegate -> OnCollectionChangedExternally: the instances sync with
//   the collection, then RefreshParameterViewModels), which the apply's own edits do not report. After the apply (and
//   after a rollback) the collection reports it.
// Assets that inherit from an applied emitter asset and are open in an editor follow it as they follow the emitter
// editor's Apply (AssetDocumentNiagaraEmitterAsset.cpp, update_dependents).

#include "AssetDocumentNiagaraPrimitives.h"

#include "NiagaraEmitter.h"
#include "NiagaraParameterCollection.h"
#include "NiagaraScript.h"
#include "NiagaraSystem.h"
#include "Runtime/AssetDocumentOpenEditor.h"

namespace AssetDocumentNiagaraOpenEditorLocal
{
const FName SystemEditorName(TEXT("Niagara"));
const FName ScriptEditorName(TEXT("NiagaraScriptEditor"));
const FName NiagaraCommands(TEXT("NiagaraEditor"));
const FName ApplyCommand(TEXT("Apply"));
const FName ApplyScratchCommand(TEXT("ApplyScratchPadChanges"));

FAssetDocumentDiagnostic UnsavedEdits(const UObject& Asset, const TCHAR* Editor, const TCHAR* Button)
{
	FAssetDocumentDiagnostic Diagnostic;
	Diagnostic.Path = TEXT("/");
	Diagnostic.Code = TEXT("Niagara.OpenInEditorUnsavedEdits");
	Diagnostic.Severity = TEXT("error");
	// The script editor has no close without its prompt (FNiagaraScriptToolkit::OnRequestClose ignores the reason).
	Diagnostic.Message = FString::Printf(TEXT("%s is open in the %s with changes that are not applied to the asset yet (its %s "
		"button is enabled). %s Then apply the document."),
		*Asset.GetName(), Editor, Button, *UE::AssetDocument::OpenEditor::DescribePendingEditsWayOut(!Asset.IsA<UNiagaraScript>()));
	return Diagnostic;
}

// The pending-edits button of the asset's open Niagara editor (empty: none).
FString PendingButton(const UObject& Asset)
{
	using UE::AssetDocument::OpenEditor::CanExecuteEditorCommand;
	if (Asset.IsA<UNiagaraScript>())
	{
		return CanExecuteEditorCommand(Asset, ScriptEditorName, NiagaraCommands, ApplyCommand) ? FString(TEXT("Apply")) : FString();
	}
	if (Asset.IsA<UNiagaraSystem>() || Asset.IsA<UNiagaraEmitter>())
	{
		// FNiagaraSystemToolkit::OnApplyEnabled (emitter editor: its copy changed) / OnApplyScratchPadChangesEnabled.
		if (CanExecuteEditorCommand(Asset, SystemEditorName, NiagaraCommands, ApplyCommand))
		{
			return TEXT("Apply");
		}
		if (CanExecuteEditorCommand(Asset, SystemEditorName, NiagaraCommands, ApplyScratchCommand))
		{
			return TEXT("Apply Scratch");
		}
	}
	return FString();
}

class FHandler final : public IAssetDocumentOpenEditorHandler
{
public:
	virtual FName GetId() const override { return TEXT("AssetDocumentNiagara.Assets"); }
	virtual bool Supports(const UObject& Asset) const override
	{
		return Asset.IsA<UNiagaraSystem>() || Asset.IsA<UNiagaraEmitter>() || Asset.IsA<UNiagaraScript>()
			|| Asset.IsA<UNiagaraParameterCollection>();
	}
	virtual FString GetProfileName(const UObject& Asset) const override { return TEXT("Niagara"); }
	virtual EAssetDocumentOpenEditorStrategy GetStrategy(const UObject& Asset) const override
	{
		return Asset.IsA<UNiagaraParameterCollection>() ? EAssetDocumentOpenEditorStrategy::RefreshInPlace
			: EAssetDocumentOpenEditorStrategy::CloseApplyReopen;
	}
	virtual TOptional<FAssetDocumentDiagnostic> CheckBeforeApply(const UObject& Asset) const override
	{
		const FString Button = PendingButton(Asset);
		if (Button.IsEmpty())
		{
			return {};
		}
		const TCHAR* Editor = Asset.IsA<UNiagaraScript>() ? TEXT("Niagara script editor")
			: Asset.IsA<UNiagaraSystem>() ? TEXT("Niagara system editor") : TEXT("Niagara emitter editor");
		return UnsavedEdits(Asset, Editor, *Button);
	}
	// asset_editor apply_editor / close with discard_unapplied. The system / emitter editor writes its pending edits with
	// Apply (emitter editor: UpdateOriginalEmitter) and Apply Scratch (the scratch pad scripts), and drops them on a
	// forced close (FNiagaraSystemToolkit::OnRequestClose skips both prompts); the script editor writes them with Apply
	// (UpdateOriginalNiagaraScript) and has no close without its prompt.
	virtual FString GetPendingEditsButton(const UObject& Asset) const override { return PendingButton(Asset); }
	virtual bool ApplyPendingEdits(UObject& Asset, FString& OutMessage) const override
	{
		using UE::AssetDocument::OpenEditor::CanExecuteEditorCommand;
		using UE::AssetDocument::OpenEditor::ExecuteEditorCommand;
		const FName EditorName = Asset.IsA<UNiagaraScript>() ? ScriptEditorName : SystemEditorName;
		bool bRan = false;
		for (const FName Command : {ApplyScratchCommand, ApplyCommand})
		{
			if (CanExecuteEditorCommand(Asset, EditorName, NiagaraCommands, Command))
			{
				if (!ExecuteEditorCommand(Asset, EditorName, NiagaraCommands, Command))
				{
					OutMessage = FString::Printf(TEXT("the editor's %s could not run"), Command == ApplyCommand ? TEXT("Apply") : TEXT("Apply Scratch"));
					return false;
				}
				bRan = true;
			}
		}
		if (!bRan)
		{
			OutMessage = TEXT("its editor holds no unapplied edits");
		}
		return bRan;
	}
	virtual bool CloseDiscardingPendingEdits(UObject& Asset, FString& OutMessage) const override
	{
		if (Asset.IsA<UNiagaraScript>())
		{
			OutMessage = TEXT("the Niagara script editor cannot close without asking about its unapplied edits");
			return false;
		}
		if (!UE::AssetDocument::OpenEditor::CloseEditorDiscardingPendingEdits(Asset, SystemEditorName))
		{
			OutMessage = TEXT("the Niagara editor stayed open");
			return false;
		}
		return true;
	}
	virtual bool CanCloseDiscardingPendingEdits(const UObject& Asset) const override
	{
		return Asset.IsA<UNiagaraSystem>() || Asset.IsA<UNiagaraEmitter>();
	}
	virtual void RefreshOpenEditor(UObject& Asset, bool bSucceeded) const override
	{
#if WITH_EDITORONLY_DATA
		if (UNiagaraParameterCollection* Collection = Cast<UNiagaraParameterCollection>(&Asset))
		{
			Collection->OnCollectionChangedDelegate.Broadcast();
		}
#endif
	}
};

TSharedPtr<IAssetDocumentOpenEditorHandler>& Registered()
{
	static TSharedPtr<IAssetDocumentOpenEditorHandler> Handler;
	return Handler;
}
} // namespace AssetDocumentNiagaraOpenEditorLocal

namespace UE::AssetDocument::Niagara
{
void RegisterNiagaraOpenEditorHandler()
{
	using namespace AssetDocumentNiagaraOpenEditorLocal;
	if (!Registered().IsValid())
	{
		Registered() = MakeShared<FHandler>();
		RegisterAssetDocumentOpenEditorHandler(Registered().ToSharedRef());
	}
}

void UnregisterNiagaraOpenEditorHandler()
{
	using namespace AssetDocumentNiagaraOpenEditorLocal;
	if (Registered().IsValid())
	{
		UnregisterAssetDocumentOpenEditorHandler(Registered()->GetId());
		Registered().Reset();
	}
}
} // namespace UE::AssetDocument::Niagara
