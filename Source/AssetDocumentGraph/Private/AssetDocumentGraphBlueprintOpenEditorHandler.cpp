// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

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
		UAssetEditorSubsystem* Editors = GEditor != nullptr ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;
		IAssetEditorInstance* Instance = Editors != nullptr ? Editors->FindEditorForAsset(&Asset, false) : nullptr;
		if (Instance == nullptr)
		{
			return;
		}
		// The editor of a Blueprint asset is an asset editor toolkit (as FBlueprintEditorUtils::GetIBlueprintEditorForObject assumes).
		FAssetEditorToolkit* Toolkit = static_cast<FAssetEditorToolkit*>(Instance);
		if (!Toolkit->IsBlueprintEditor())
		{
			return;
		}
		FBlueprintEditor* Editor = static_cast<FBlueprintEditor*>(Toolkit);
		UBlueprint& Blueprint = *CastChecked<UBlueprint>(&Asset);
		if (Editor->GetBlueprintObj() != &Blueprint)
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
} // namespace UE::AssetDocument::Graph
