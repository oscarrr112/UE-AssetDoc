// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// Open-editor apply for the AI profiles (Runtime/AssetDocumentOpenEditor.h):
//   EnvQuery     the Environment Query editor edits UEnvQuery::EdGraph itself; refreshed in place as its Undo does
//                (FAIGraphEditor::PostUndo: clear the graph selection, NotifyGraphChanged).
//   StateTree    no handler: the StateTree editor's view model (UStateTreeEditingSubsystem) already refreshes its views
//                (OnAssetChanged) on the apply's own edits and on its rollback, and drops a removed state from its
//                selection (StateTree.OpenEditorApply checks this).
//   BehaviorTree an apply of the tree installs a new BTGraph (behavior_tree_production.py), while the Behavior Tree
//                editor's document tab stays bound to the graph object it opened (FBehaviorTreeEditor::RestoreBehaviorTree);
//                a later edit or graph change in that tab would rebuild the runtime tree from the old graph. The editor
//                is closed before the apply and reopened afterwards (BehaviorTree.EditorReopened).
// Both editors change the asset while opening (EnvQuery: the displayed test weights; BehaviorTree: an old graph
// version is upgraded). That is editor-only graph data the apply rebuilds, so such a package is accepted
// (IAssetDocumentOpenEditorHandler::AcceptsOpenTimeChanges) until the user changes it.

#include "AssetDocumentAIOpenEditor.h"

#include "BehaviorTree/BehaviorTree.h"
#include "EdGraph/EdGraph.h"
#include "EnvironmentQuery/EnvQuery.h"
#include "GraphEditor.h"
#include "Runtime/AssetDocumentOpenEditor.h"

namespace AssetDocumentAIOpenEditor
{
class FEnvQueryHandler final : public IAssetDocumentOpenEditorHandler
{
public:
	virtual FName GetId() const override { return TEXT("AssetDocumentAI.EnvQuery"); }
	virtual bool Supports(const UObject& Asset) const override { return Asset.IsA<UEnvQuery>(); }
	virtual FString GetProfileName(const UObject& Asset) const override { return TEXT("EnvQuery"); }
	virtual EAssetDocumentOpenEditorStrategy GetStrategy(const UObject& Asset) const override
	{
		return EAssetDocumentOpenEditorStrategy::RefreshInPlace;
	}
	// Opening recomputes the test weights the graph displays (UEnvironmentQueryGraph::Initialize -> CalculateAllWeights,
	// SetDisplayedWeight's Modify); the apply's finalize recomputes them too.
	virtual bool AcceptsOpenTimeChanges(const UObject& Asset) const override { return true; }
	virtual void RefreshOpenEditor(UObject& Asset, bool bSucceeded) const override
	{
#if WITH_EDITORONLY_DATA
		const UEdGraph* Graph = CastChecked<UEnvQuery>(&Asset)->EdGraph;
		if (const TSharedPtr<SGraphEditor> GraphEditor = Graph != nullptr ? SGraphEditor::FindGraphEditorForGraph(Graph) : nullptr)
		{
			GraphEditor->ClearSelectionSet();
			GraphEditor->NotifyGraphChanged();
		}
#endif
	}
};

class FBehaviorTreeHandler final : public IAssetDocumentOpenEditorHandler
{
public:
	virtual FName GetId() const override { return TEXT("AssetDocumentAI.BehaviorTree"); }
	virtual bool Supports(const UObject& Asset) const override { return Asset.IsA<UBehaviorTree>(); }
	virtual FString GetProfileName(const UObject& Asset) const override { return TEXT("BehaviorTree"); }
	virtual EAssetDocumentOpenEditorStrategy GetStrategy(const UObject& Asset) const override
	{
		return EAssetDocumentOpenEditorStrategy::CloseApplyReopen;
	}
	// Opening upgrades a graph saved with an older graph version (UBehaviorTreeGraph::Initialize -> UpdateVersion's
	// Modify); the apply installs a new graph.
	virtual bool AcceptsOpenTimeChanges(const UObject& Asset) const override { return true; }
};

TArray<TSharedRef<IAssetDocumentOpenEditorHandler>>& Handlers()
{
	static TArray<TSharedRef<IAssetDocumentOpenEditorHandler>> Registered;
	return Registered;
}
} // namespace AssetDocumentAIOpenEditor

void RegisterAssetDocumentAIOpenEditorHandlers()
{
	using namespace AssetDocumentAIOpenEditor;
	if (!Handlers().IsEmpty())
	{
		return;
	}
	Handlers() = {MakeShared<FEnvQueryHandler>(), MakeShared<FBehaviorTreeHandler>()};
	for (const TSharedRef<IAssetDocumentOpenEditorHandler>& Handler : Handlers())
	{
		RegisterAssetDocumentOpenEditorHandler(Handler);
	}
}

void UnregisterAssetDocumentAIOpenEditorHandlers()
{
	using namespace AssetDocumentAIOpenEditor;
	for (const TSharedRef<IAssetDocumentOpenEditorHandler>& Handler : Handlers())
	{
		UnregisterAssetDocumentOpenEditorHandler(Handler->GetId());
	}
	Handlers().Reset();
}
