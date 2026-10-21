// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// Open-editor apply for the Persona assets (Runtime/AssetDocumentOpenEditor.h). Persona editors edit the asset itself
// (no working copy), but most of their panels only rebuild from the asset on the editor's own edits or on Undo
// (FEditorUndoClient::PostUndo), and an apply is neither. So an apply runs in place and the open editor is refreshed
// the way its Undo refreshes it, or more:
//   AnimSequence, AnimMontage, AnimComposite, BlendSpace (Aim Offset, 1D), PoseAsset: the Animation editor
//     (FAnimationEditor, IAnimationEditor) rebuilds the asset's document tab with SetAnimationAsset on the same asset,
//     its own "open asset" path: the timeline (notify, curve, montage section / slot tracks, the play range), the
//     Montage Sections tab (OnSectionsChanged), the blend space grid, the pose list; then the preview instance is
//     restarted on the asset (UDebugSkelMeshComponent::EnablePreview -> UAnimSingleNodeInstance::SetAnimationAsset,
//     which rebuilds a montage instance's section links), keeping play / pause. SSequenceEditor::PostUndo does the
//     same two steps (SetPreviewAnimationAsset + RefreshTracks).
//   Skeleton: the skeleton tree of the Skeleton editor and of every Persona editor on an asset of that skeleton
//     rebuilds its items (ISkeletonTree::Refresh = SSkeletonTree::PostUndo's CreateFromSkeleton); the preview scene
//     reads sockets, slots and curves from the skeleton when it draws.
// The same refresh follows a failed (rolled-back) apply.

#include "AnimAssets/AssetDocumentAnimAssetOpenEditor.h"

#include "AnimPreviewInstance.h"
#include "Animation/AnimationAsset.h"
#include "Animation/DebugSkelMeshComponent.h"
#include "Animation/AnimComposite.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimSequence.h"
#include "Animation/BlendSpace.h"
#include "Animation/PoseAsset.h"
#include "Animation/Skeleton.h"
#include "Containers/Ticker.h"
#include "Engine/SkeletalMeshSocket.h"
#include "Editor.h"
#include "IAnimationEditor.h"
#include "IEditableSkeleton.h"
#include "IPersonaToolkit.h"
#include "ISkeletonTree.h"
#include "PersonaTabs.h"
#include "Runtime/AssetDocumentOpenEditor.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Toolkits/AssetEditorToolkit.h"
#include "UObject/StrongObjectPtr.h"
#include "Framework/Docking/TabManager.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/SWidget.h"

namespace AssetDocumentAnimAssetOpenEditor
{
const FName AnimationEditorName(TEXT("AnimationEditor"));
const FName SkeletonTreeWidgetType(TEXT("SSkeletonTree"));

UAssetEditorSubsystem* Editors()
{
	return GEditor != nullptr ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;
}

// The Animation editor open on Asset (Persona's FAnimationEditor), or null.
IAnimationEditor* FindAnimationEditor(UObject& Asset)
{
	UAssetEditorSubsystem* Subsystem = Editors();
	IAssetEditorInstance* Instance = Subsystem != nullptr ? Subsystem->FindEditorForAsset(&Asset, false) : nullptr;
	if (Instance == nullptr || Instance->GetEditorName() != AnimationEditorName)
	{
		return nullptr;
	}
	return static_cast<IAnimationEditor*>(static_cast<FAssetEditorToolkit*>(Instance));
}

// The skeleton tree (SSkeletonTree, an ISkeletonTree) in the content of a tab (a workflow tab wraps its body in a
// padding border), or null.
TSharedPtr<ISkeletonTree> FindSkeletonTreeWidget(const TSharedRef<SWidget>& Widget, int32 Depth)
{
	if (Widget->GetType() == SkeletonTreeWidgetType)
	{
		return StaticCastSharedRef<ISkeletonTree>(Widget);
	}
	FChildren* Children = Depth > 0 ? Widget->GetChildren() : nullptr;
	for (int32 Index = 0; Children != nullptr && Index < Children->Num(); ++Index)
	{
		if (TSharedPtr<ISkeletonTree> Found = FindSkeletonTreeWidget(Children->GetChildAt(Index), Depth - 1))
		{
			return Found;
		}
	}
	return nullptr;
}

// Rebuilds the skeleton tree of an open Persona editor from its skeleton, as its Undo does (SSkeletonTree::PostUndo:
// CreateFromSkeleton, then the tree view refresh).
void RefreshSkeletonTree(IAssetEditorInstance& Instance)
{
	const TSharedPtr<FTabManager> TabManager = Instance.GetAssociatedTabManager();
	const TSharedPtr<SDockTab> Tab = TabManager.IsValid() ? TabManager->FindExistingLiveTab(FTabId(FPersonaTabs::SkeletonTreeViewID)) : nullptr;
	if (const TSharedPtr<ISkeletonTree> Tree = Tab.IsValid() ? FindSkeletonTreeWidget(Tab->GetContent(), 4) : nullptr)
	{
		// Some trees keep the items of the build before for one more build (they restore each row's expansion from
		// them): the second build lets go of the rows built before the apply, which may list sockets it removed.
		Tree->Refresh();
		Tree->Refresh();
		Tree->GetEditableSkeleton()->RefreshBoneTree();
	}
}

// Rebuilds the Animation editor's document tab and preview from the asset (see the file comment).
void RefreshAnimationEditor(IAnimationEditor& Editor, UAnimationAsset& Asset)
{
	Editor.SetAnimationAsset(&Asset);
	// The preview scene keeps a preview of the same asset as it is (FAnimationEditorPreviewScene::
	// SetPreviewAnimationAsset returns early), so the preview instance is restarted on the asset as opening it does.
	UDebugSkelMeshComponent* Mesh = Editor.GetPersonaToolkit()->GetPreviewMeshComponent();
	UAnimPreviewInstance* Preview = Mesh != nullptr ? Mesh->PreviewInstance.Get() : nullptr;
	if (Preview != nullptr && Preview->GetCurrentAsset() == &Asset)
	{
		const bool bPlaying = Preview->IsPlaying();
		Mesh->EnablePreview(true, &Asset);
		Preview->SetPlaying(bPlaying);
	}
}

FString ProfileNameOf(const UObject& Asset)
{
	if (Asset.IsA<UAnimMontage>()) { return TEXT("AnimMontage"); }
	if (Asset.IsA<UAnimComposite>()) { return TEXT("AnimComposite"); }
	if (Asset.IsA<UAnimSequence>()) { return TEXT("AnimSequence"); }
	if (Asset.IsA<UBlendSpace>()) { return TEXT("BlendSpace"); }
	if (Asset.IsA<UPoseAsset>()) { return TEXT("PoseAsset"); }
	return TEXT("Skeleton");
}

class FHandler final : public IAssetDocumentOpenEditorHandler
{
public:
	virtual FName GetId() const override { return TEXT("AssetDocumentAnimation.Persona"); }
	virtual bool Supports(const UObject& Asset) const override
	{
		return Asset.IsA<UAnimMontage>() || Asset.IsA<UAnimSequence>() || Asset.IsA<UAnimComposite>()
			|| Asset.IsA<UBlendSpace>() || Asset.IsA<UPoseAsset>() || Asset.IsA<USkeleton>();
	}
	virtual FString GetProfileName(const UObject& Asset) const override { return ProfileNameOf(Asset); }
	virtual EAssetDocumentOpenEditorStrategy GetStrategy(const UObject& Asset) const override
	{
		return EAssetDocumentOpenEditorStrategy::RefreshInPlace;
	}
	virtual TOptional<FAssetDocumentDiagnostic> CheckBeforeApply(const UObject& Asset) const override
	{
		// The skeleton tree keeps raw pointers to the sockets it lists until its items go (they unbind from them then):
		// the sockets an apply removes stay alive until the tree view has dropped the rows it generated before the
		// refresh after the apply, which it does when it next paints.
		ListedSockets.Reset();
		if (const USkeleton* Skeleton = Cast<USkeleton>(&Asset))
		{
			for (USkeletalMeshSocket* Socket : Skeleton->Sockets)
			{
				if (Socket != nullptr)
				{
					ListedSockets.Emplace(Socket);
				}
			}
		}
		return {};
	}
	virtual void RefreshOpenEditor(UObject& Asset, bool bSucceeded) const override
	{
		if (USkeleton* Skeleton = Cast<USkeleton>(&Asset))
		{
			RefreshSkeletonEditors(*Skeleton);
		}
		else if (IAnimationEditor* Editor = FindAnimationEditor(Asset))
		{
			RefreshAnimationEditor(*Editor, *CastChecked<UAnimationAsset>(&Asset));
		}
		if (!ListedSockets.IsEmpty())
		{
			// Released two frames on: the tree view has painted (and dropped its old rows) by then.
			FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
				[Kept = MoveTemp(ListedSockets), Until = GFrameCounter + 2](float) { return GFrameCounter < Until; }));
			ListedSockets.Reset();
		}
	}

private:
	mutable TArray<TStrongObjectPtr<USkeletalMeshSocket>> ListedSockets;

	static void RefreshSkeletonEditors(USkeleton& Skeleton)
	{
		UAssetEditorSubsystem* Subsystem = Editors();
		if (Subsystem == nullptr)
		{
			return;
		}
		TSet<IAssetEditorInstance*> Refreshed;
		for (UObject* Edited : Subsystem->GetAllEditedAssets())
		{
			const UAnimationAsset* Animation = Cast<UAnimationAsset>(Edited);
			if (Edited != &Skeleton && (Animation == nullptr || Animation->GetSkeleton() != &Skeleton))
			{
				continue;
			}
			for (IAssetEditorInstance* Instance : Subsystem->FindEditorsForAsset(Edited))
			{
				if (Instance != nullptr && !Refreshed.Contains(Instance))
				{
					Refreshed.Add(Instance);
					RefreshSkeletonTree(*Instance);
				}
			}
		}
	}
};

TSharedPtr<IAssetDocumentOpenEditorHandler>& Handler()
{
	static TSharedPtr<IAssetDocumentOpenEditorHandler> Registered;
	return Registered;
}
} // namespace AssetDocumentAnimAssetOpenEditor

void RegisterAssetDocumentAnimAssetOpenEditorHandlers()
{
	using namespace AssetDocumentAnimAssetOpenEditor;
	if (!Handler().IsValid())
	{
		Handler() = MakeShared<FHandler>();
		RegisterAssetDocumentOpenEditorHandler(Handler().ToSharedRef());
	}
}

void UnregisterAssetDocumentAnimAssetOpenEditorHandlers()
{
	using namespace AssetDocumentAnimAssetOpenEditor;
	if (Handler().IsValid())
	{
		UnregisterAssetDocumentOpenEditorHandler(Handler()->GetId());
		Handler().Reset();
	}
}
