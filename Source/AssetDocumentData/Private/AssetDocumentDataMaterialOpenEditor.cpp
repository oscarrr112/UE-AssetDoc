// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// Open-editor apply for materials (Runtime/AssetDocumentOpenEditor.h). Behaviour tests:
// UEAssetDoc.Material.OpenEditorApply.* and UEAssetDoc.MaterialInstance.OpenEditorApply.
//
// Material, Material Function (incl. Material Layer / Layer Blend): close, apply, reopen. The Material Editor
//   (FMaterialEditor) edits a preview copy of the asset (UPreviewMaterial / a duplicated function, made once when it
//   opens) and writes that copy back over the asset on its Apply, its Save and its own Undo (PostUndo ends with
//   UpdateOriginalMaterial). No editor path seeds the copy again from the asset, so an editor left open over an apply
//   would show the old graph and write it back. The editor is closed before the apply and reopened afterwards
//   (Material.EditorReopened); the reopened editor copies the applied asset. While the copy holds edits the user has not
//   applied (the editor's Apply button is enabled), closing would ask to apply or discard them: the apply is refused
//   before any change (Material.OpenInEditorUnsavedEdits), naming asset_editor's ways out: apply_editor (the editor's
//   Apply) and close with discard_unapplied (the close prompt's "No", as a forced close answers it).
// MaterialInstance (MaterialInstanceConstant): in place, refreshed. The Material Instance Editor edits the instance
//   itself through a proxy (UMaterialEditorInstanceConstant): every edit in its Details panel is copied to the instance
//   at once (CopyToSourceInstance), which also runs on the editor's Undo / Redo. The proxy holds the parameter list,
//   override flags and values, the parent and the base property overrides as they were when it read the instance, so
//   after an apply its next edit or undo would write the old values back. After the apply (and after a rollback) the
//   proxy reads the instance again as it does when the editor opens or the parent changes (SetSourceInstance:
//   parent, physical material, base properties, RegenerateArrays), and the editor refreshes its Details panel, preview
//   and parent list (IMaterialEditor::NotifyExternalMaterialChange -> FMaterialInstanceEditor::Refresh). The editor
//   has no pending state of its own: its edits dirty the package (the usual DirtyExistingAssetTransactionUnsupported).

#include "AssetDocumentDataMaterial.h"

#include "Editor.h"
#include "IMaterialEditor.h"
#include "MaterialEditor/MaterialEditorInstanceConstant.h"
#include "Materials/Material.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Runtime/AssetDocumentOpenEditor.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Toolkits/AssetEditorToolkit.h"

namespace AssetDocumentDataMaterialOpenEditorLocal
{
const FName MaterialEditorName(TEXT("MaterialEditor"));
const FName MaterialInstanceEditorName(TEXT("MaterialInstanceEditor"));
const FName MaterialEditorCommands(TEXT("MaterialEditor"));
const FName ApplyCommand(TEXT("Apply"));

class FMaterialHandler final : public IAssetDocumentOpenEditorHandler
{
public:
	virtual FName GetId() const override { return TEXT("AssetDocumentData.Material"); }
	virtual bool Supports(const UObject& Asset) const override { return Asset.IsA<UMaterial>() || Asset.IsA<UMaterialFunction>(); }
	virtual FString GetProfileName(const UObject& Asset) const override { return TEXT("Material"); }
	virtual EAssetDocumentOpenEditorStrategy GetStrategy(const UObject& Asset) const override
	{
		return EAssetDocumentOpenEditorStrategy::CloseApplyReopen;
	}
	virtual TOptional<FAssetDocumentDiagnostic> CheckBeforeApply(const UObject& Asset) const override
	{
		// FMaterialEditor::OnApplyEnabled: the preview copy holds edits not yet applied to the asset (bMaterialDirty).
		if (!UE::AssetDocument::OpenEditor::CanExecuteEditorCommand(Asset, MaterialEditorName, MaterialEditorCommands, ApplyCommand))
		{
			return {};
		}
		FAssetDocumentDiagnostic Diagnostic;
		Diagnostic.Path = TEXT("/");
		Diagnostic.Code = TEXT("Material.OpenInEditorUnsavedEdits");
		Diagnostic.Severity = TEXT("error");
		Diagnostic.Message = FString::Printf(TEXT("%s is open in the Material Editor with changes that are not applied to the asset yet "
			"(its Apply button is enabled). %s Then apply the document."),
			*Asset.GetName(), *UE::AssetDocument::OpenEditor::DescribePendingEditsWayOut(true));
		return Diagnostic;
	}
	// asset_editor apply_editor / close with discard_unapplied: the editor's Apply (FMaterialEditor::OnApply:
	// UpdateOriginalMaterial), and its close prompt's "No" (OnRequestClose drops bMaterialDirty for a forced close).
	virtual FString GetPendingEditsButton(const UObject& Asset) const override
	{
		return UE::AssetDocument::OpenEditor::CanExecuteEditorCommand(Asset, MaterialEditorName, MaterialEditorCommands, ApplyCommand)
			? FString(TEXT("Apply")) : FString();
	}
	virtual bool ApplyPendingEdits(UObject& Asset, FString& OutMessage) const override
	{
		if (!UE::AssetDocument::OpenEditor::ExecuteEditorCommand(Asset, MaterialEditorName, MaterialEditorCommands, ApplyCommand))
		{
			OutMessage = TEXT("the Material Editor's Apply could not run");
			return false;
		}
		return true;
	}
	virtual bool CloseDiscardingPendingEdits(UObject& Asset, FString& OutMessage) const override
	{
		if (!UE::AssetDocument::OpenEditor::CloseEditorDiscardingPendingEdits(Asset, MaterialEditorName))
		{
			OutMessage = TEXT("the Material Editor stayed open");
			return false;
		}
		return true;
	}
	virtual bool CanCloseDiscardingPendingEdits(const UObject& Asset) const override { return true; }
};

class FMaterialInstanceHandler final : public IAssetDocumentOpenEditorHandler
{
public:
	virtual FName GetId() const override { return TEXT("AssetDocumentData.MaterialInstance"); }
	// The MaterialInstance profile's class (subclasses such as landscape instances use the generic profile).
	virtual bool Supports(const UObject& Asset) const override { return Asset.GetClass() == UMaterialInstanceConstant::StaticClass(); }
	virtual FString GetProfileName(const UObject& Asset) const override { return TEXT("MaterialInstance"); }
	virtual EAssetDocumentOpenEditorStrategy GetStrategy(const UObject& Asset) const override
	{
		return EAssetDocumentOpenEditorStrategy::RefreshInPlace;
	}
	virtual void RefreshOpenEditor(UObject& Asset, bool bSucceeded) const override
	{
		UMaterialInstanceConstant& Instance = *CastChecked<UMaterialInstanceConstant>(&Asset);
		UAssetEditorSubsystem* Editors = GEditor != nullptr ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;
		if (Editors == nullptr)
		{
			return;
		}
		for (IAssetEditorInstance* Editor : Editors->FindEditorsForAsset(&Instance))
		{
			if (Editor == nullptr || Editor->GetEditorName() != MaterialInstanceEditorName)
			{
				continue;
			}
			// FMaterialInstanceEditor edits {the instance, its proxy} (InitMaterialInstanceEditor's ObjectsToEdit).
			FAssetEditorToolkit* Toolkit = static_cast<FAssetEditorToolkit*>(Editor);
			if (const TArray<UObject*>* EditedObjects = Toolkit->GetObjectsCurrentlyBeingEdited())
			{
				for (UObject* Edited : *EditedObjects)
				{
					UMaterialEditorInstanceConstant* Proxy = Cast<UMaterialEditorInstanceConstant>(Edited);
					if (Proxy != nullptr && Proxy->SourceInstance == &Instance && !Proxy->bIsFunctionPreviewMaterial)
					{
						Proxy->SetSourceInstance(&Instance);
					}
				}
			}
			static_cast<IMaterialEditor*>(Toolkit)->NotifyExternalMaterialChange();
		}
	}
};

TArray<TSharedRef<IAssetDocumentOpenEditorHandler>>& Registered()
{
	static TArray<TSharedRef<IAssetDocumentOpenEditorHandler>> Handlers;
	return Handlers;
}
} // namespace AssetDocumentDataMaterialOpenEditorLocal

namespace UE::AssetDocument::Graph::Data::Material
{
void RegisterOpenEditorHandlers()
{
	using namespace AssetDocumentDataMaterialOpenEditorLocal;
	if (!Registered().IsEmpty())
	{
		return;
	}
	Registered() = {MakeShared<FMaterialHandler>(), MakeShared<FMaterialInstanceHandler>()};
	for (const TSharedRef<IAssetDocumentOpenEditorHandler>& Handler : Registered())
	{
		RegisterAssetDocumentOpenEditorHandler(Handler);
	}
}

void UnregisterOpenEditorHandlers()
{
	using namespace AssetDocumentDataMaterialOpenEditorLocal;
	for (const TSharedRef<IAssetDocumentOpenEditorHandler>& Handler : Registered())
	{
		UnregisterAssetDocumentOpenEditorHandler(Handler->GetId());
	}
	Registered().Reset();
}
} // namespace UE::AssetDocument::Graph::Data::Material

