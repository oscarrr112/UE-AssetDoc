// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentGraphBlueprintOpenEditorHandler.h"

#include "BlueprintEditor.h"
#include "Editor.h"
#include "EdGraph/EdGraph.h"
#include "Engine/Blueprint.h"
#include "GraphEditor.h"
#include "Runtime/AssetDocumentOpenEditor.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Toolkits/AssetEditorToolkit.h"
#include "Widgets/Docking/SDockTab.h"
#include "WorkflowOrientedApp/WorkflowTabManager.h"

namespace UE::AssetDocument::Graph
{
namespace BlueprintOpenEditorHandling
{
// The graphs shown by the graph editors under Widget (a graph document tab's content).
void CollectShownGraphs(const TSharedRef<SWidget>& Widget, TArray<UEdGraph*>& OutGraphs)
{
	static const FName GraphEditorType(TEXT("SGraphEditor"));
	if (Widget->GetType() == GraphEditorType)
	{
		if (UEdGraph* Graph = StaticCastSharedRef<SGraphEditor>(Widget)->GetCurrentGraph())
		{
			OutGraphs.AddUnique(Graph);
		}
		return;
	}
	FChildren* Children = Widget->GetChildren();
	for (int32 Index = 0; Children != nullptr && Index < Children->Num(); ++Index)
	{
		CollectShownGraphs(Children->GetChildAt(Index), OutGraphs);
	}
}

// The Blueprint editor open on Blueprint, or null.
FBlueprintEditor* FindBlueprintEditor(const UBlueprint& Blueprint)
{
	UAssetEditorSubsystem* Editors = GEditor != nullptr ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;
	IAssetEditorInstance* Instance = Editors != nullptr ? Editors->FindEditorForAsset(const_cast<UBlueprint*>(&Blueprint), false) : nullptr;
	if (Instance == nullptr)
	{
		return nullptr;
	}
	// The editor of a Blueprint asset is an asset editor toolkit (as FBlueprintEditorUtils::GetIBlueprintEditorForObject assumes).
	FAssetEditorToolkit* Toolkit = static_cast<FAssetEditorToolkit*>(Instance);
	if (!Toolkit->IsBlueprintEditor())
	{
		return nullptr;
	}
	FBlueprintEditor* Editor = static_cast<FBlueprintEditor*>(Toolkit);
	return Editor->GetBlueprintObj() == &Blueprint ? Editor : nullptr;
}

class FBlueprintOpenEditorHandler final : public IAssetDocumentOpenEditorHandler
{
public:
	virtual FName GetId() const override
	{
		return TEXT("AssetDocumentGraph.Blueprint");
	}

	virtual FString GetProfileName(const UObject& Asset) const override
	{
		return Asset.GetClass()->GetName();
	}

	virtual EAssetDocumentOpenEditorStrategy GetStrategy(const UObject& Asset) const override
	{
		return EAssetDocumentOpenEditorStrategy::RefreshInPlace;
	}

	virtual bool Supports(const UObject& Asset) const override
	{
		return Asset.IsA<UBlueprint>();
	}

	// Every UBlueprint: Blueprint subclasses with their own editor handler (Control Rig) take precedence.
	virtual bool IsFallback() const override
	{
		return true;
	}

	virtual void RefreshOpenEditor(UObject& Asset, bool bSucceeded) const override
	{
		UBlueprint& Blueprint = *CastChecked<UBlueprint>(&Asset);
		FBlueprintEditor* Editor = FindBlueprintEditor(Blueprint);
		if (Editor == nullptr)
		{
			return;
		}

		if (Editor->DocumentManager.IsValid())
		{
			TArray<UEdGraph*> CurrentGraphs;
			Blueprint.GetAllGraphs(CurrentGraphs);
			TArray<UEdGraph*> Shown;
			for (const TSharedPtr<SDockTab>& Tab : Editor->DocumentManager->GetAllDocumentTabs())
			{
				if (Tab.IsValid())
				{
					CollectShownGraphs(Tab->GetContent(), Shown);
				}
			}
			for (UEdGraph* Graph : Shown)
			{
				if (!CurrentGraphs.Contains(Graph))
				{
					Editor->CloseDocumentTab(Graph);
				}
			}
		}
		Editor->RefreshEditors();
	}
};
} // namespace BlueprintOpenEditorHandling

TSharedRef<IAssetDocumentOpenEditorHandler> MakeBlueprintOpenEditorHandler()
{
	return MakeShared<BlueprintOpenEditorHandling::FBlueprintOpenEditorHandler>();
}

void ClearOpenEditorGraphSelection(const UBlueprint& Blueprint)
{
	if (FBlueprintEditor* Editor = BlueprintOpenEditorHandling::FindBlueprintEditor(Blueprint))
	{
		Editor->ClearSelectionStateFor(FBlueprintEditor::SelectionState_Graph);
	}
}
} // namespace UE::AssetDocument::Graph
