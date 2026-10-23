// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "BehaviorTree/AssetDocumentAIBlackboardOpenEditorHandler.h"

#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTreeEditor.h"
#include "Editor.h"
#include "Runtime/AssetDocumentOpenEditor.h"
#include "Subsystems/AssetEditorSubsystem.h"

namespace UE::AssetDocument::AI::BlackboardOpenEditorHandling
{
const FName HandlerId(TEXT("AssetDocumentAI.BlackboardData"));
const FName BehaviorTreeEditorName(TEXT("Behavior Tree"));

class FBlackboardOpenEditorHandler final : public IAssetDocumentOpenEditorHandler
{
public:
	virtual FName GetId() const override
	{
		return HandlerId;
	}

	virtual bool Supports(const UObject& Asset) const override
	{
		return Asset.IsA<UBlackboardData>();
	}

	virtual FString GetProfileName(const UObject& Asset) const override
	{
		return TEXT("BlackboardData");
	}

	virtual EAssetDocumentOpenEditorStrategy GetStrategy(const UObject& Asset) const override
	{
		return EAssetDocumentOpenEditorStrategy::RefreshInPlace;
	}

	virtual void RefreshOpenEditor(UObject& Asset, bool bSucceeded) const override
	{
		UAssetEditorSubsystem* Editors = GEditor != nullptr ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;
		if (Editors == nullptr)
		{
			return;
		}
		// Every open Behavior Tree editor (one per Behavior Tree or Blackboard asset it was opened on) showing this Blackboard.
		TArray<IAssetEditorInstance*> Refreshed;
		for (UObject* Edited : Editors->GetAllEditedAssets())
		{
			IAssetEditorInstance* Instance = Edited != nullptr ? Editors->FindEditorForAsset(Edited, false) : nullptr;
			if (Instance == nullptr || Refreshed.Contains(Instance) || Instance->GetEditorName() != BehaviorTreeEditorName)
			{
				continue;
			}
			FBehaviorTreeEditor* Editor = static_cast<FBehaviorTreeEditor*>(Instance);
			if (Editor->GetBlackboardData() == &Asset)
			{
				Refreshed.Add(Instance);
				Editor->PostUndo(true);
			}
		}
	}
};
} // namespace UE::AssetDocument::AI::BlackboardOpenEditorHandling

void RegisterAssetDocumentBlackboardOpenEditorHandler()
{
	RegisterAssetDocumentOpenEditorHandler(MakeShared<UE::AssetDocument::AI::BlackboardOpenEditorHandling::FBlackboardOpenEditorHandler>());
}

void UnregisterAssetDocumentBlackboardOpenEditorHandler()
{
	UnregisterAssetDocumentOpenEditorHandler(UE::AssetDocument::AI::BlackboardOpenEditorHandling::HandlerId);
}
