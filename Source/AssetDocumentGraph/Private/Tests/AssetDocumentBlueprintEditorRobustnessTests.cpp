// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// Applies to a Blueprint open in the Blueprint editor while the editor holds view state of its own.
//   UEAssetDoc.Blueprint.OpenEditorCommentWithStaleSelection
//       A node is selected in the focused graph tab and the graph panel has no node widgets (they are rebuilt on the
//       next Slate tick after NotifyGraphChanged, which an apply's earlier steps call). The editor's "Add Comment"
//       spawner sizes a new comment box around the editor selection; with no widget its bounds are the unset
//       (FLT_MAX) rectangle. A document that adds comment boxes applies, the boxes have the document's position and
//       size (a box without Size has the size it has with the editor closed), and a document that adds a comment box
//       and then fails to compile is rolled back with the editor open.
//   UEAssetDoc.Blueprint.OpenEditorGraphTabs
//       Opening the Blueprint, a function graph's tab and switching between graph tabs leaves nothing that blocks the
//       next apply; an unsaved edit made in the editor is still refused (DirtyExistingAssetTransactionUnsupported).

#include "AssetDocumentOpenEditorTestSupport.h"

#include "BlueprintEditor.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphNode_Comment.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "K2Node_CallFunction.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/ScopeExit.h"
#include "ScopedTransaction.h"
#include "UObject/UObjectGlobals.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentBlueprintEditorRobustnessTests
{
using namespace UE::AssetDocument::OpenEditorTests;

FString Document(const FTarget& Target, const TCHAR* Action, const FString& Body)
{
	return FString::Printf(TEXT("{\"SchemaVersion\":1,\"Target\":\"%s\",\"Class\":\"/Script/Engine.Blueprint\",\"Action\":\"%s\",")
		TEXT("\"Body\":{\"ParentClass\":\"/Script/Engine.Actor\",%s}}"), *Target.PackageName, Action, *Body);
}

FBlueprintEditor* BlueprintEditorOf(UBlueprint* Blueprint)
{
	return static_cast<FBlueprintEditor*>(FindEditor(Blueprint));
}

UEdGraph* EventGraphOf(const UBlueprint* Blueprint)
{
	return Blueprint != nullptr && !Blueprint->UbergraphPages.IsEmpty() ? Blueprint->UbergraphPages[0].Get() : nullptr;
}

UEdGraph* FunctionGraph(const UBlueprint* Blueprint, const TCHAR* Name)
{
	for (UEdGraph* Graph : Blueprint != nullptr ? Blueprint->FunctionGraphs : TArray<TObjectPtr<UEdGraph>>())
	{
		if (Graph != nullptr && Graph->GetName() == Name)
		{
			return Graph;
		}
	}
	return nullptr;
}

UEdGraphNode* FindPrint(const UEdGraph* Graph)
{
	for (UEdGraphNode* Node : Graph != nullptr ? Graph->Nodes : TArray<TObjectPtr<UEdGraphNode>>())
	{
		const UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node);
		if (Call != nullptr && Call->GetFunctionName() == TEXT("PrintString"))
		{
			return Node;
		}
	}
	return nullptr;
}

const UEdGraphNode_Comment* FindComment(const UEdGraph* Graph, const TCHAR* Text)
{
	for (const UEdGraphNode* Node : Graph != nullptr ? Graph->Nodes : TArray<TObjectPtr<UEdGraphNode>>())
	{
		const UEdGraphNode_Comment* Comment = Cast<UEdGraphNode_Comment>(Node);
		if (Comment != nullptr && Comment->NodeComment == Text)
		{
			return Comment;
		}
	}
	return nullptr;
}

bool HasDiagnostic(const FAssetDocumentResult& Result, const TCHAR* Code)
{
	return Result.Diagnostics.ContainsByPredicate([Code](const FAssetDocumentDiagnostic& Diagnostic) { return Diagnostic.Code == Code; });
}

// The node is the only one selected in the editor's focused graph tab, and that panel has no node widgets: the editor's
// selection bounds are the unset rectangle (FKismetEditorUtilities::GetBoundsForSelectedNodes reports false and leaves
// FLT_MAX in it).
bool SelectWithoutWidgets(FAutomationTestBase& Test, FBlueprintEditor& Editor, UBlueprint& Blueprint, UEdGraph& Graph, UEdGraphNode& Node,
	const FString& Stage)
{
	const TSharedPtr<SGraphEditor> GraphEditor = Editor.OpenGraphAndBringToFront(&Graph);
	if (!Test.TestTrue(*FString::Printf(TEXT("%s: the graph's tab is open"), *Stage), GraphEditor.IsValid()))
	{
		return false;
	}
	Test.TestEqual(*FString::Printf(TEXT("%s: the graph's tab is focused"), *Stage), Editor.GetFocusedGraph(), &Graph);
	GraphEditor->ClearSelectionSet();
	GraphEditor->SetNodeSelection(&Node, true);
	Graph.NotifyGraphChanged();
	FSlateRect Bounds(0.f, 0.f, 0.f, 0.f);
	const bool bValid = FKismetEditorUtilities::GetBoundsForSelectedNodes(&Blueprint, Bounds, 50.f);
	return Test.TestEqual(*FString::Printf(TEXT("%s: one node selected"), *Stage), FKismetEditorUtilities::GetNumberOfSelectedNodes(&Blueprint), 1)
		&& Test.TestFalse(*FString::Printf(TEXT("%s: the selection has no node widget"), *Stage), bValid)
		&& Test.TestTrue(*FString::Printf(TEXT("%s: the selection bounds are unset (FLT_MAX)"), *Stage), Bounds.Left == MAX_FLT && Bounds.Top == MAX_FLT);
}

const TCHAR* InitialBody = TEXT(R"json(
"UbergraphPages":[{"Name":"EventGraph","Nodes":[{"Id":"Begin","Action":"K2:BeginPlay","Position":{"X":0,"Y":0}},
  {"Id":"Print","Action":"K2:PrintString","Position":{"X":320,"Y":0}}],
  "Links":[{"From":"Begin:then","To":"Print:execute"}]}])json");

const TCHAR* CommentsBody = TEXT(R"json(
"UbergraphPages":[{"Name":"EventGraph","Nodes":[{"Id":"Begin","Action":"K2:BeginPlay","Position":{"X":0,"Y":0}},
  {"Id":"Print","Action":"K2:PrintString","Position":{"X":320,"Y":0}},
  {"Id":"Box","Action":"K2:Comment","Comment":"Sized","Position":{"X":-64,"Y":-96},"Size":{"Width":640,"Height":320}},
  {"Id":"Plain","Action":"K2:Comment","Comment":"Unsized","Position":{"X":0,"Y":400}}],
  "Links":[{"From":"Begin:then","To":"Print:execute"}]}])json");

// Adds a comment box and a For Each Loop whose Array type is undetermined: the compile fails after the box was placed.
const TCHAR* FailingBody = TEXT(R"json(
"UbergraphPages":[{"Name":"EventGraph","Nodes":[{"Id":"Begin","Action":"K2:BeginPlay","Position":{"X":0,"Y":0}},
  {"Id":"Print","Action":"K2:PrintString","Position":{"X":320,"Y":0}},
  {"Id":"Box","Action":"K2:Comment","Comment":"Sized","Position":{"X":-64,"Y":-96},"Size":{"Width":640,"Height":320}},
  {"Id":"Plain","Action":"K2:Comment","Comment":"Unsized","Position":{"X":0,"Y":400}},
  {"Id":"Failing","Action":"K2:Comment","Comment":"Rolled back","Position":{"X":0,"Y":800},"Size":{"Width":300,"Height":200}},
  {"Id":"Loop","Action":"K2:ForEachLoop","Position":{"X":640,"Y":0}}],
  "Links":[{"From":"Begin:then","To":"Print:execute"},{"From":"Print:then","To":"Loop:Exec"}]}])json");

const TCHAR* TabsBody = TEXT(R"json(
"Variables":[{"Name":"Health","Type":"int","DefaultValue":10}],
"FunctionGraphs":[{"Name":"Heal","Nodes":[{"Id":"Print","Action":"K2:PrintString","Position":{"X":240,"Y":0}},
    {"Id":"Note","Action":"K2:Comment","Comment":"Heal note","Position":{"X":-40,"Y":-80},"Size":{"Width":600,"Height":240}}],
  "Links":[{"From":"Entry:then","To":"Print:execute"},{"From":"Print:then","To":"Result:execute"}]}],
"UbergraphPages":[{"Name":"EventGraph","Nodes":[{"Id":"Begin","Action":"K2:BeginPlay","Position":{"X":0,"Y":0}},
  {"Id":"Print","Action":"K2:PrintString","Position":{"X":320,"Y":0}},
  {"Id":"Box","Action":"K2:Comment","Comment":"Event note","Position":{"X":-64,"Y":-96},"Size":{"Width":640,"Height":320}}],
  "Links":[{"From":"Begin:then","To":"Print:execute"}]}])json");

const TCHAR* TabsEditedBody = TEXT(R"json(
"Variables":[{"Name":"Health","Type":"int","DefaultValue":25}])json");

// Records what changes a package while the watch lives: the objects modified (OnObjectModified) and whether the
// package was marked dirty.
class FPackageChangeWatch
{
public:
	explicit FPackageChangeWatch(const UPackage* InPackage)
		: Package(InPackage)
	{
		ModifiedHandle = FCoreUObjectDelegates::OnObjectModified.AddRaw(this, &FPackageChangeWatch::HandleModified);
		DirtyHandle = UPackage::PackageMarkedDirtyEvent.AddRaw(this, &FPackageChangeWatch::HandleDirty);
	}

	~FPackageChangeWatch()
	{
		FCoreUObjectDelegates::OnObjectModified.Remove(ModifiedHandle);
		UPackage::PackageMarkedDirtyEvent.Remove(DirtyHandle);
	}

	FPackageChangeWatch(const FPackageChangeWatch&) = delete;
	FPackageChangeWatch& operator=(const FPackageChangeWatch&) = delete;

	TArray<FString> Modified;
	int32 DirtyMarks = 0;

private:
	void HandleModified(UObject* Object)
	{
		if (Object != nullptr && Object->GetPackage() == Package)
		{
			Modified.Add(FString::Printf(TEXT("%s (%s, %s)"), *Object->GetPathName(), *Object->GetClass()->GetName(),
				Object->HasAnyFlags(RF_Transactional) ? TEXT("transactional") : TEXT("not transactional")));
		}
	}

	void HandleDirty(UPackage* Dirtied, bool)
	{
		if (Dirtied == Package)
		{
			++DirtyMarks;
		}
	}

	const UPackage* Package = nullptr;
	FDelegateHandle ModifiedHandle;
	FDelegateHandle DirtyHandle;
};

void LogChanges(FAutomationTestBase& Test, const FPackageChangeWatch& Watch, const UPackage* Package, const FString& Stage)
{
	Test.AddInfo(FString::Printf(TEXT("%s: package %s, %d dirty mark(s), modified: [%s]"), *Stage,
		Package != nullptr && Package->IsDirty() ? TEXT("dirty") : TEXT("clean"), Watch.DirtyMarks, *FString::Join(Watch.Modified, TEXT("; "))));
}
} // namespace AssetDocumentBlueprintEditorRobustnessTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentBlueprintOpenEditorCommentWithStaleSelectionTest,
	"UEAssetDoc.Blueprint.OpenEditorCommentWithStaleSelection", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentBlueprintOpenEditorCommentWithStaleSelectionTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentBlueprintEditorRobustnessTests;
	const FString Folder = NewFolder();
	const FTarget Target(Folder, TEXT("BP_StaleSelection"));
	ON_SCOPE_EXIT { Cleanup({Target}); };
	if (!ApplyOk(*this, Target, Document(Target, TEXT("Create"), InitialBody), TEXT("create")))
	{
		return false;
	}
	UBlueprint* Blueprint = Target.Get<UBlueprint>();
	if (!TestNotNull(TEXT("Blueprint exists"), Blueprint) || Open(*this, Blueprint) == nullptr)
	{
		return false;
	}
	FBlueprintEditor* Editor = BlueprintEditorOf(Blueprint);
	UEdGraph* EventGraph = EventGraphOf(Blueprint);
	UEdGraphNode* Print = FindPrint(EventGraph);
	if (!TestNotNull(TEXT("the Blueprint editor"), Editor) || !TestNotNull(TEXT("the Print String node"), Print)
		|| !SelectWithoutWidgets(*this, *Editor, *Blueprint, *EventGraph, *Print, TEXT("before the comment apply")))
	{
		return false;
	}
	ReleaseUndoHistory();

	// 1. Comment boxes added while the selection has no widget: the boxes have the document's bounds.
	if (!ApplyOk(*this, Target, Document(Target, TEXT("Update"), CommentsBody), TEXT("comment boxes while open")))
	{
		return false;
	}
	Blueprint = Target.Get<UBlueprint>();
	EventGraph = EventGraphOf(Blueprint);
	const UEdGraphNode_Comment* Sized = FindComment(EventGraph, TEXT("Sized"));
	const UEdGraphNode_Comment* Unsized = FindComment(EventGraph, TEXT("Unsized"));
	const UEdGraphNode_Comment* Fresh = GetDefault<UEdGraphNode_Comment>();
	if (TestNotNull(TEXT("the sized box"), Sized) && TestNotNull(TEXT("the unsized box"), Unsized))
	{
		AddInfo(FString::Printf(TEXT("sized box (%d, %d) %dx%d, unsized box (%d, %d) %dx%d"), Sized->NodePosX, Sized->NodePosY, Sized->NodeWidth,
			Sized->NodeHeight, Unsized->NodePosX, Unsized->NodePosY, Unsized->NodeWidth, Unsized->NodeHeight));
		TestTrue(TEXT("the sized box has the document's position and size"),
			Sized->NodePosX == -64 && Sized->NodePosY == -96 && Sized->NodeWidth == 640 && Sized->NodeHeight == 320);
		TestTrue(TEXT("the unsized box has the document's position and a new box's size"), Unsized->NodePosX == 0 && Unsized->NodePosY == 400
			&& Unsized->NodeWidth == Fresh->NodeWidth && Unsized->NodeHeight == Fresh->NodeHeight);
	}
	const FString Applied = Extract(*this, Target, TEXT("after the comment apply"));
	TestEqual(TEXT("the editor is still open on the Blueprint"), BlueprintEditorOf(Blueprint), Editor);

	// 2. A failing apply that places a comment box while the selection has no widget is rolled back.
	Print = FindPrint(EventGraph);
	if (TestNotNull(TEXT("the Print String node after the apply"), Print)
		&& SelectWithoutWidgets(*this, *Editor, *Blueprint, *EventGraph, *Print, TEXT("before the failing apply")))
	{
		const FAssetDocumentResult Failed = Apply(*this, Target, Document(Target, TEXT("Update"), FailingBody), TEXT("failing apply while open"));
		TestFalse(TEXT("the failing apply fails"), Failed.IsSuccess());
		TestTrue(TEXT("the failure is the compiler's (Graph.FinalizeFailed)"), HasDiagnostic(Failed, TEXT("Graph.FinalizeFailed")));
		TickEditorUi();
		Blueprint = Target.Get<UBlueprint>();
		TestEqual(TEXT("after the rollback: the extraction is the applied state"), Extract(*this, Target, TEXT("after the rollback")), Applied);
		TestNull(TEXT("after the rollback: no rolled-back comment box"), FindComment(EventGraphOf(Blueprint), TEXT("Rolled back")));
		TestTrue(TEXT("after the rollback: the editor is still open"), Blueprint != nullptr && BlueprintEditorOf(Blueprint) == Editor);
	}

	Close(Blueprint);
	TestNull(TEXT("the editor closed"), FindEditor(Target.Get()));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentBlueprintOpenEditorGraphTabsTest, "UEAssetDoc.Blueprint.OpenEditorGraphTabs",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentBlueprintOpenEditorGraphTabsTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentBlueprintEditorRobustnessTests;
	const FString Folder = NewFolder();
	const FTarget Target(Folder, TEXT("BP_GraphTabs"));
	ON_SCOPE_EXIT { Cleanup({Target}); };
	if (!ApplyOk(*this, Target, Document(Target, TEXT("Create"), TabsBody), TEXT("create")))
	{
		return false;
	}
	UBlueprint* Blueprint = Target.Get<UBlueprint>();
	UPackage* Package = Target.Package();
	if (!TestNotNull(TEXT("Blueprint exists"), Blueprint) || !TestFalse(TEXT("the created package is saved"), Package->IsDirty()))
	{
		return false;
	}
	FPackageChangeWatch Watch(Package);
	if (Open(*this, Blueprint) == nullptr)
	{
		return false;
	}
	TickEditorUi();
	LogChanges(*this, Watch, Package, TEXT("opened"));
	FBlueprintEditor* Editor = BlueprintEditorOf(Blueprint);
	UEdGraph* Heal = FunctionGraph(Blueprint, TEXT("Heal"));
	UEdGraph* EventGraph = EventGraphOf(Blueprint);
	if (!TestNotNull(TEXT("the Blueprint editor"), Editor) || !TestNotNull(TEXT("the Heal graph"), Heal) || !TestNotNull(TEXT("the EventGraph"), EventGraph))
	{
		return false;
	}
	// The user opens Heal's tab, goes back to the EventGraph tab and again to Heal's.
	for (UEdGraph* Graph : {Heal, EventGraph, Heal})
	{
		Editor->OpenGraphAndBringToFront(Graph);
		TickEditorUi();
		LogChanges(*this, Watch, Package, FString::Printf(TEXT("tab %s"), *Graph->GetName()));
	}
	AddInfo(FString::Printf(TEXT("widgets: EventGraph %s, Heal %s"), PanelShowsEveryNode(*EventGraph) ? TEXT("shown") : TEXT("not built"),
		PanelShowsEveryNode(*Heal) ? TEXT("shown") : TEXT("not built")));
	// The user clicks each node in turn (the Details panel shows it), then clicks empty graph space.
	for (UEdGraph* Graph : {EventGraph, Heal})
	{
		const TSharedPtr<SGraphEditor> GraphEditor = Editor->OpenGraphAndBringToFront(Graph);
		if (!TestTrue(FString::Printf(TEXT("%s: the tab is open"), *Graph->GetName()), GraphEditor.IsValid()))
		{
			return false;
		}
		const TArray<TObjectPtr<UEdGraphNode>> Nodes = Graph->Nodes;
		for (UEdGraphNode* Node : Nodes)
		{
			GraphEditor->ClearSelectionSet();
			GraphEditor->SetNodeSelection(Node, true);
			TickEditorUi();
		}
		GraphEditor->ClearSelectionSet();
		TickEditorUi();
		LogChanges(*this, Watch, Package, FString::Printf(TEXT("nodes selected in %s"), *Graph->GetName()));
	}
	TestFalse(TEXT("opening, switching tabs and selecting leave the package saved"), Package->IsDirty());

	// The next apply runs.
	if (!ApplyOk(*this, Target, Document(Target, TEXT("Update"), TabsEditedBody), TEXT("apply after the tabs")))
	{
		return false;
	}
	Blueprint = Target.Get<UBlueprint>();
	TestFalse(TEXT("the applied package is saved"), Target.Package()->IsDirty());

	// An unsaved edit made in the editor (on a later editor frame) is still refused.
	FCoreUObjectDelegates::ObjectsModifiedThisFrame.Empty();
	{
		const FScopedTransaction Transaction(NSLOCTEXT("AssetDocumentBlueprintEditorRobustnessTests", "AddVariable", "Add Variable"));
		Blueprint->Modify();
		FEdGraphPinType Type;
		Type.PinCategory = UEdGraphSchema_K2::PC_Boolean;
		FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("HumanVar"), Type);
	}
	const FAssetDocumentResult Refused = Apply(*this, Target, Document(Target, TEXT("Update"), TabsBody), TEXT("apply over an unsaved edit"));
	TestFalse(TEXT("an unsaved edit is refused"), Refused.IsSuccess());
	TestTrue(TEXT("the refusal is DirtyExistingAssetTransactionUnsupported"), HasDiagnostic(Refused, TEXT("DirtyExistingAssetTransactionUnsupported")));
	const FAssetDocumentDiagnostic* Refusal = Refused.Diagnostics.FindByPredicate([](const FAssetDocumentDiagnostic& Diagnostic)
	{
		return Diagnostic.Code == TEXT("DirtyExistingAssetTransactionUnsupported");
	});
	if (Refusal != nullptr)
	{
		AddInfo(FString::Printf(TEXT("refusal: %s"), *Refusal->Message));
		TestTrue(TEXT("the refusal names the first modified object, transactional, in a transaction"),
			Refusal->Message.Contains(FString::Printf(TEXT("The first unsaved change: %s (Blueprint), transactional, inside an editor transaction"),
				*Target.Get<UBlueprint>()->GetPathName())));
	}
	TestTrue(TEXT("the unsaved edit is kept"), FBlueprintEditorUtils::FindNewVariableIndex(Target.Get<UBlueprint>(), TEXT("HumanVar")) != INDEX_NONE);

	Close(Target.Get());
	return !HasAnyErrors();
}
#endif // WITH_DEV_AUTOMATION_TESTS
