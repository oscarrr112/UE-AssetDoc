// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// UEAssetDoc.Blueprint.OpenEditorApply: an Actor Blueprint open in the Blueprint editor, with a
// function graph's tab open and a component selected, takes an apply that adds a function graph, a variable, a
// component and an EventGraph node with a link, and removes a function graph and a component. The open editor then
// shows what it shows after the same edits made in it: My Blueprint lists the new members and not the removed ones,
// the Components tree and the viewport's preview actor have the new component and not the removed one, the removed
// graph's tab is closed and the details panel holds no removed object. A failed apply rolls back and the editor shows
// the rolled-back Blueprint; the editor's own edit and Undo after the apply keep the applied state; the editor closes
// cleanly.

#include "AssetDocumentOpenEditorTestSupport.h"

#include "BlueprintEditor.h"
#include "Components/ActorComponent.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "EdGraphSchema_K2_Actions.h"
#include "Engine/Blueprint.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "GameFramework/Actor.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Misc/ScopeExit.h"
#include "SKismetInspector.h"
#include "SMyBlueprint.h"
#include "SSubobjectEditor.h"
#include "ScopedTransaction.h"
#include "UObject/WeakObjectPtr.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentBlueprintOpenEditorTests
{
using namespace UE::AssetDocument::OpenEditorTests;

FString Document(const FTarget& Target, const TCHAR* Action, const FString& Body)
{
	return FString::Printf(TEXT("{\"SchemaVersion\":1,\"Target\":\"%s\",\"Class\":\"/Script/Engine.Blueprint\",\"Action\":\"%s\",")
		TEXT("\"Body\":{\"ParentClass\":\"/Script/Engine.Actor\",%s}}"), *Target.PackageName, Action, *Body);
}

const TCHAR* InitialBody = TEXT(R"json(
"Variables":[{"Name":"Health","Type":"int","DefaultValue":10}],
"Components":[{"Name":"Keep","Class":"/Script/Engine.SceneComponent"},
  {"Name":"OldMesh","Class":"/Script/Engine.StaticMeshComponent","AttachTo":"Keep"}],
"FunctionGraphs":[{"Name":"OldFn","Nodes":[],"Links":[{"From":"Entry:then","To":"Result:execute"}]}],
"UbergraphPages":[{"Name":"EventGraph","Nodes":[{"Id":"Begin","Action":"K2:BeginPlay"}],"Links":[]}])json");

// Adds Speed, NewFn, NewMesh and a Print String after Begin Play; removes OldFn and OldMesh.
const TCHAR* EditedBody = TEXT(R"json(
"Variables":[{"Name":"Health","Type":"int","DefaultValue":10},{"Name":"Speed","Type":"float","DefaultValue":2.5}],
"Components":[{"Name":"Keep","Class":"/Script/Engine.SceneComponent"},
  {"Name":"NewMesh","Class":"/Script/Engine.StaticMeshComponent","AttachTo":"Keep"}],
"FunctionGraphs":[{"Name":"NewFn","Nodes":[{"Id":"Print","Action":"K2:PrintString"}],
  "Links":[{"From":"Entry:then","To":"Print:execute"},{"From":"Print:then","To":"Result:execute"}]}],
"UbergraphPages":[{"Name":"EventGraph","Nodes":[{"Id":"Begin","Action":"K2:BeginPlay"},{"Id":"Print","Action":"K2:PrintString"}],
  "Links":[{"From":"Begin:then","To":"Print:execute"}]}])json");

// The edit a failed apply rolls back: adds FailFn and Bogus, removes NewMesh and NewFn.
const TCHAR* FailingBody = TEXT(R"json(
"Variables":[{"Name":"Health","Type":"int","DefaultValue":10},{"Name":"Speed","Type":"float","DefaultValue":2.5},
  {"Name":"Bogus","Type":"bool"}],
"Components":[{"Name":"Keep","Class":"/Script/Engine.SceneComponent"}],
"FunctionGraphs":[{"Name":"FailFn","Nodes":[],"Links":[{"From":"Entry:then","To":"Result:execute"}]}])json");

FBlueprintEditor* BlueprintEditorOf(UBlueprint* Blueprint)
{
	return static_cast<FBlueprintEditor*>(FindEditor(Blueprint));
}

// True when the editor's My Blueprint panel lists an item of that name.
bool MyBlueprintLists(FBlueprintEditor& Editor, const TCHAR* Name)
{
	const TSharedPtr<SMyBlueprint> MyBlueprint = Editor.GetMyBlueprintWidget();
	const bool bListed = MyBlueprint.IsValid() && ActionMenuLists(MyBlueprint.ToSharedRef(), FName(Name));
	if (bListed)
	{
		MyBlueprint->ClearGraphActionMenuSelection();
	}
	return bListed;
}

void CollectTreeNames(const FSubobjectEditorTreeNodePtrType& Node, TArray<FString>& Out)
{
	if (!Node.IsValid())
	{
		return;
	}
	Out.Add(Node->GetVariableName().ToString());
	for (const FSubobjectEditorTreeNodePtrType& Child : Node->GetChildren())
	{
		CollectTreeNames(Child, Out);
	}
}

// The component names the editor's Components tree shows (the scene hierarchy).
TArray<FString> ComponentsTree(FBlueprintEditor& Editor)
{
	TArray<FString> Names;
	if (const TSharedPtr<SSubobjectEditor> Tree = Editor.GetSubobjectEditor())
	{
		CollectTreeNames(Tree->GetSceneRootNode(), Names);
	}
	return Names;
}

FSubobjectEditorTreeNodePtrType FindTreeNode(const FSubobjectEditorTreeNodePtrType& Node, const FName Name)
{
	if (!Node.IsValid() || Node->GetVariableName() == Name)
	{
		return Node;
	}
	for (const FSubobjectEditorTreeNodePtrType& Child : Node->GetChildren())
	{
		if (FSubobjectEditorTreeNodePtrType Found = FindTreeNode(Child, Name))
		{
			return Found;
		}
	}
	return nullptr;
}

// The components of the editor's preview actor (the Viewport tab), or empty when it has none.
TArray<FString> PreviewComponents(FBlueprintEditor& Editor)
{
	TArray<FString> Names;
	if (const AActor* Preview = Editor.GetPreviewActor())
	{
		for (const UActorComponent* Component : Preview->GetComponents())
		{
			if (Component != nullptr)
			{
				Names.Add(Component->GetName());
			}
		}
	}
	return Names;
}

int32 DocumentTabs(FBlueprintEditor& Editor, UObject* Document)
{
	TArray<TSharedPtr<SDockTab>> Tabs;
	if (Document != nullptr)
	{
		Editor.FindOpenTabsContainingDocument(Document, Tabs);
	}
	return Tabs.Num();
}

UEdGraph* FunctionGraph(const UBlueprint* Blueprint, const TCHAR* Name)
{
	if (Blueprint == nullptr)
	{
		return nullptr;
	}
	for (UEdGraph* Graph : Blueprint->FunctionGraphs)
	{
		if (Graph != nullptr && Graph->GetName() == Name)
		{
			return Graph;
		}
	}
	return nullptr;
}

bool HasScsNode(const UBlueprint* Blueprint, const TCHAR* Name)
{
	return Blueprint != nullptr && Blueprint->SimpleConstructionScript != nullptr
		&& Blueprint->SimpleConstructionScript->FindSCSNode(FName(Name)) != nullptr;
}

// The details panel shows no object the apply removed (garbage or moved to the transient package).
void TestInspectorHoldsNoRemovedObject(FAutomationTestBase& Test, FBlueprintEditor& Editor, const FString& Stage)
{
	for (const TWeakObjectPtr<UObject>& Shown : Editor.GetInspector()->GetSelectedObjects())
	{
		const UObject* Object = Shown.Get(true);
		Test.TestTrue(*FString::Printf(TEXT("%s: the details panel shows no removed object (%s)"), *Stage, *GetPathNameSafe(Object)),
			Object == nullptr || (IsValid(Object) && Object->GetOutermost() != GetTransientPackage()));
	}
}

// What the open editor shows of the members and components.
struct FEditorView
{
	bool bNewFn = false;
	bool bOldFn = false;
	bool bFailFn = false;
	bool bSpeed = false;
	bool bBogus = false;
	TArray<FString> Tree;
	TArray<FString> Preview;
};

FEditorView ReadEditor(FBlueprintEditor& Editor)
{
	FEditorView View;
	View.bNewFn = MyBlueprintLists(Editor, TEXT("NewFn"));
	View.bOldFn = MyBlueprintLists(Editor, TEXT("OldFn"));
	View.bFailFn = MyBlueprintLists(Editor, TEXT("FailFn"));
	View.bSpeed = MyBlueprintLists(Editor, TEXT("Speed"));
	View.bBogus = MyBlueprintLists(Editor, TEXT("Bogus"));
	View.Tree = ComponentsTree(Editor);
	View.Preview = PreviewComponents(Editor);
	return View;
}
} // namespace AssetDocumentBlueprintOpenEditorTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentBlueprintOpenEditorApplyTest, "UEAssetDoc.Blueprint.OpenEditorApply",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentBlueprintOpenEditorApplyTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentBlueprintOpenEditorTests;
	const FString Folder = NewFolder();
	const FTarget Target(Folder, TEXT("BP_OpenEditor"));
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
	TWeakObjectPtr<UEdGraph> OldFn = FunctionGraph(Blueprint, TEXT("OldFn"));
	if (!TestNotNull(TEXT("the Blueprint editor"), Editor) || !TestTrue(TEXT("OldFn exists"), OldFn.IsValid()))
	{
		return false;
	}
	// The user has OldFn's tab open and OldMesh selected in the Components tree (its details shown).
	Editor->OpenGraphAndBringToFront(OldFn.Get());
	if (const TSharedPtr<SSubobjectEditor> Tree = Editor->GetSubobjectEditor())
	{
		if (const FSubobjectEditorTreeNodePtrType Node = FindTreeNode(Tree->GetSceneRootNode(), TEXT("OldMesh")))
		{
			Tree->SelectNode(Node, false);
		}
	}
	Tick();
	{
		const FEditorView Before = ReadEditor(*Editor);
		TestTrue(TEXT("before: My Blueprint lists OldFn"), Before.bOldFn);
		TestTrue(TEXT("before: the Components tree shows OldMesh"), Before.Tree.Contains(TEXT("OldMesh")));
		TestEqual(TEXT("before: OldFn has a tab"), DocumentTabs(*Editor, OldFn.Get()), 1);
		AddInfo(FString::Printf(TEXT("before: tree [%s], preview [%s]"), *FString::Join(Before.Tree, TEXT(",")), *FString::Join(Before.Preview, TEXT(","))));
	}
	ReleaseUndoHistory();

	// 1. The apply lands in the open editor.
	if (!ApplyOk(*this, Target, Document(Target, TEXT("Update"), EditedBody), TEXT("apply while open")))
	{
		return false;
	}
	Tick();
	TestEqual(TEXT("the editor is still open on the same Blueprint"), BlueprintEditorOf(Target.Get<UBlueprint>()), Editor);
	TestTrue(TEXT("asset: NewFn added"), FunctionGraph(Blueprint, TEXT("NewFn")) != nullptr);
	TestNull(TEXT("asset: OldFn removed"), FunctionGraph(Blueprint, TEXT("OldFn")));
	TestTrue(TEXT("asset: NewMesh added, OldMesh removed"), HasScsNode(Blueprint, TEXT("NewMesh")) && !HasScsNode(Blueprint, TEXT("OldMesh")));
	TestTrue(TEXT("asset: Blueprint compiles"), Blueprint->Status == BS_UpToDate || Blueprint->Status == BS_UpToDateWithWarnings);
	{
		const FEditorView After = ReadEditor(*Editor);
		AddInfo(FString::Printf(TEXT("after apply: tree [%s], preview [%s]"), *FString::Join(After.Tree, TEXT(",")), *FString::Join(After.Preview, TEXT(","))));
		TestTrue(TEXT("editor: My Blueprint lists NewFn"), After.bNewFn);
		TestTrue(TEXT("editor: My Blueprint lists Speed"), After.bSpeed);
		TestFalse(TEXT("editor: My Blueprint no longer lists OldFn"), After.bOldFn);
		TestTrue(TEXT("editor: the Components tree shows NewMesh"), After.Tree.Contains(TEXT("NewMesh")));
		TestFalse(TEXT("editor: the Components tree no longer shows OldMesh"), After.Tree.Contains(TEXT("OldMesh")));
		if (!After.Preview.IsEmpty())
		{
			TestTrue(TEXT("editor: the preview actor has NewMesh"), After.Preview.ContainsByPredicate([](const FString& Name) { return Name.StartsWith(TEXT("NewMesh")); }));
			TestFalse(TEXT("editor: the preview actor no longer has OldMesh"), After.Preview.ContainsByPredicate([](const FString& Name) { return Name.StartsWith(TEXT("OldMesh")); }));
		}
		TestEqual(TEXT("editor: the removed OldFn's tab is closed"), DocumentTabs(*Editor, OldFn.Get(true)), 0);
		TestInspectorHoldsNoRemovedObject(*this, *Editor, TEXT("after apply"));
	}
	const FString Applied = Extract(*this, Target, TEXT("after apply"));
	TestTrue(TEXT("extraction has the Print String"), Applied.Contains(TEXT("PrintString")));
	TestUndoKeepsApply(*this, Target, Applied, TEXT("after apply"));

	// 2. A failed apply rolls back while the editor is open; the editor shows the rolled-back Blueprint.
	ApplyFailingAfterMutation(*this, Target, Document(Target, TEXT("Update"), FailingBody), TEXT("failing apply while open"));
	Tick();
	Blueprint = Target.Get<UBlueprint>();
	TestEqual(TEXT("after rollback: the extraction is the applied state"), Extract(*this, Target, TEXT("after rollback")), Applied);
	TestTrue(TEXT("after rollback: the editor is still open"), Blueprint != nullptr && BlueprintEditorOf(Blueprint) == Editor);
	{
		const FEditorView RolledBack = ReadEditor(*Editor);
		AddInfo(FString::Printf(TEXT("after rollback: tree [%s], preview [%s]"), *FString::Join(RolledBack.Tree, TEXT(",")), *FString::Join(RolledBack.Preview, TEXT(","))));
		TestTrue(TEXT("after rollback: My Blueprint lists NewFn"), RolledBack.bNewFn);
		TestFalse(TEXT("after rollback: My Blueprint does not list FailFn"), RolledBack.bFailFn);
		TestFalse(TEXT("after rollback: My Blueprint does not list Bogus"), RolledBack.bBogus);
		TestTrue(TEXT("after rollback: the Components tree shows NewMesh"), RolledBack.Tree.Contains(TEXT("NewMesh")));
		if (!RolledBack.Preview.IsEmpty())
		{
			TestTrue(TEXT("after rollback: the preview actor has NewMesh"), RolledBack.Preview.ContainsByPredicate([](const FString& Name) { return Name.StartsWith(TEXT("NewMesh")); }));
		}
		TestInspectorHoldsNoRemovedObject(*this, *Editor, TEXT("after rollback"));
	}

	// 3. The editor's own edit after the apply, and its Undo.
	{
		const FScopedTransaction Transaction(NSLOCTEXT("AssetDocumentBlueprintOpenEditorTests", "AddVariable", "Add Variable"));
		Blueprint->Modify();
		FEdGraphPinType Type;
		Type.PinCategory = UEdGraphSchema_K2::PC_Boolean;
		FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("HumanVar"), Type);
	}
	Editor->RefreshEditors();
	TestTrue(TEXT("the human edit shows in My Blueprint"), MyBlueprintLists(*Editor, TEXT("HumanVar")));
	TestTrue(TEXT("the human edit undoes"), GEditor->UndoTransaction());
	Tick();
	TestFalse(TEXT("after Undo: My Blueprint no longer lists HumanVar"), MyBlueprintLists(*Editor, TEXT("HumanVar")));
	TestEqual(TEXT("after Undo: the extraction is the applied state"), Extract(*this, Target, TEXT("after undo")), Applied);

	// 4. The editor closes without prompts or crashes.
	Close(Blueprint);
	TestNull(TEXT("the editor closed"), FindEditor(Target.Get()));
	return !HasAnyErrors();
}
#endif // WITH_DEV_AUTOMATION_TESTS
