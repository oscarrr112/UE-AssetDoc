// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// Editor-made Behavior Trees with content an author leaves behind in the editor: a branch whose wire to its parent was
// broken (it stays in the graph, out of the runtime tree), siblings dragged onto the same position, a composite
// decorator's graph with an added input left open, a comment, and a node not wired to the Sink, a node property with
// no document form, and nodes whose class no longer loads. Each tree is first made from a document, then changed as
// the editor changes it, and saved. Each reads (with warnings where the document has a rule for it), its extract
// re-applies as a no-op, an edit elsewhere applies and keeps it, and new input of the same kind is refused at its
// document path.

#include "AssetDocumentTolerantTestSupport.h"

#include "AIGraphNode.h"
#include "AIGraphTypes.h"
#include "BehaviorTree/BTCompositeNode.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BehaviorTree/Services/BTService_DefaultFocus.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTreeDecoratorGraphNode_Logic.h"
#include "BehaviorTreeGraph.h"
#include "BehaviorTreeGraphNode.h"
#include "BehaviorTreeGraphNode_Service.h"
#include "BehaviorTreeGraphNode_CompositeDecorator.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphSchema.h"
#include "EdGraphNode_Comment.h"
#include "Misc/ScopeExit.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace AssetDocumentTolerantBehaviorTreeTestsLocal
{
namespace Tolerant = AssetDocumentTolerantTestSupport;

// A persistent node identity of the fixtures: 32 hex digits, as FGuid(0, 0, 0, Value) prints them.
FString NodeId(uint32 Value)
{
	return FGuid(0, 0, 0, Value).ToString(EGuidFormats::Digits);
}

// The tree made from a document, as the AssetDocument tools make it, then saved.
bool CreateTree(FAutomationTestBase& Test, const Tolerant::FTarget& Target, const FString& Tree)
{
	const FAssetDocumentResult Created = Tolerant::ApplyText(Test, Target, FString::Printf(TEXT(R"({"SchemaVersion": 1, "Target": "%s",
		"Class": "/Script/AIModule.BehaviorTree", "Action": "CreateOrUpdate", "Definitions": {}, "Properties": {},
		"Body": {"BlackboardAsset": null, "Tree": %s}})"), *Target.PackageName, *Tree), TEXT("Create"));
	return Test.TestTrue(TEXT("tree created from its document"), Created.IsSuccess() && Target.Get<UBehaviorTree>() != nullptr);
}

UBehaviorTreeGraph* TreeGraph(const Tolerant::FTarget& Target)
{
	UBehaviorTree* Tree = Target.Get<UBehaviorTree>();
	return Tree != nullptr ? Cast<UBehaviorTreeGraph>(Tree->BTGraph) : nullptr;
}

// The main node, decorator or service of the tree's graph with persistent identity Id, or null.
UBehaviorTreeGraphNode* GraphNode(const Tolerant::FTarget& Target, const FString& Id)
{
	UBehaviorTreeGraph* Graph = TreeGraph(Target);
	for (int32 Index = 0; Graph != nullptr && Index < Graph->Nodes.Num(); ++Index)
	{
		UBehaviorTreeGraphNode* Main = Cast<UBehaviorTreeGraphNode>(Graph->Nodes[Index]);
		if (Main == nullptr)
		{
			continue;
		}
		if (Main->NodeGuid.ToString(EGuidFormats::Digits) == Id)
		{
			return Main;
		}
		for (UAIGraphNode* Sub : Main->SubNodes)
		{
			if (Sub != nullptr && Sub->NodeGuid.ToString(EGuidFormats::Digits) == Id)
			{
				return Cast<UBehaviorTreeGraphNode>(Sub);
			}
		}
	}
	return nullptr;
}

UEdGraphPin* NodePin(UEdGraphNode* Node, EEdGraphPinDirection Direction)
{
	for (int32 Index = 0; Node != nullptr && Index < Node->Pins.Num(); ++Index)
	{
		if (Node->Pins[Index] != nullptr && Node->Pins[Index]->Direction == Direction)
		{
			return Node->Pins[Index];
		}
	}
	return nullptr;
}

// The editor's change is done: the tree is rebuilt from its graph and saved.
bool FinishEdit(FAutomationTestBase& Test, const Tolerant::FTarget& Target)
{
	UBehaviorTreeGraph* Graph = TreeGraph(Target);
	if (!Test.TestNotNull(TEXT("tree graph"), Graph))
	{
		return false;
	}
	Graph->UpdateAsset();
	return Test.TestTrue(TEXT("saved after the editor change"), Target.Save());
}

// Sets the NodeName of the node object at Pointer (its Properties are made when the extract omitted them).
void SetNodeName(const TSharedPtr<FJsonObject>& Document, const FString& Pointer, const FString& Name)
{
	const TSharedPtr<FJsonValue> Node = Tolerant::At(Document, Pointer);
	if (!Node.IsValid() || Node->Type != EJson::Object)
	{
		return;
	}
	const TSharedPtr<FJsonObject>* Properties = nullptr;
	if (!Node->AsObject()->TryGetObjectField(TEXT("Properties"), Properties))
	{
		Node->AsObject()->SetObjectField(TEXT("Properties"), MakeShared<FJsonObject>());
	}
	Node->AsObject()->GetObjectField(TEXT("Properties"))->SetStringField(TEXT("NodeName"), Name);
}

int32 RuntimeChildCount(const Tolerant::FTarget& Target)
{
	const UBehaviorTree* Tree = Target.Get<UBehaviorTree>();
	return Tree != nullptr && Tree->RootNode != nullptr ? Tree->RootNode->Children.Num() : INDEX_NONE;
}

// The test folder's package path, where the lost classes were.
FString LostClassFolder()
{
	return FString(TEXT("/Game/")) + Tolerant::FolderName;
}

// The class of Node no longer loads: the node keeps class data naming the Blueprint class Name in the test folder, and
// its instance is gone (as after a load that could not find the class).
bool LoseNodeClass(FAutomationTestBase& Test, UAIGraphNode* Node, const FString& Name)
{
	if (!Test.TestTrue(*FString::Printf(TEXT("node for %s has its instance"), *Name), Node != nullptr && Node->NodeInstance != nullptr))
	{
		return false;
	}
	Node->Modify();
	UObject* Instance = Node->NodeInstance;
	Node->NodeInstance = nullptr;
	Instance->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional);
	Instance->MarkAsGarbage();
	Node->ClassData = FGraphNodeClassData(Name, LostClassFolder() / Name, Name + TEXT("_C"), nullptr);
	return true;
}

// The node with persistent identity Id has no instance and its class data names the class Name of the test folder.
bool KeepsLostClass(const Tolerant::FTarget& Target, const FString& Id, const FString& Name)
{
	const UBehaviorTreeGraphNode* Node = GraphNode(Target, Id);
	return Node != nullptr && Node->NodeInstance == nullptr && Node->ClassData.GetClassName() == Name + TEXT("_C")
		&& Node->ClassData.GetPackageName() == LostClassFolder() / Name;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentTolerantBehaviorTreeDetachedTest, "UEAssetDoc.Tolerant.BehaviorTree.Detached",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentTolerantBehaviorTreeDetachedTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentTolerantTestSupport;
	using namespace AssetDocumentTolerantBehaviorTreeTestsLocal;
	const FTarget Target(TEXT("BT_Detached"));
	AddExpectedMessage(TEXT("package was marked as deleted in editor"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1, false);
	ON_SCOPE_EXIT { Target.Cleanup(); };

	// A Sequence with a Wait and a Selector that owns a Wait.
	if (!CreateTree(*this, Target, FString::Printf(TEXT(R"({"Root": {"Id": "%s", "Class": "/Script/AIModule.BTComposite_Sequence", "Children": [
		{"Id": "%s", "Class": "/Script/AIModule.BTTask_Wait", "Editor": {"Position": {"X": 0, "Y": 200}}},
		{"Id": "%s", "Class": "/Script/AIModule.BTComposite_Selector", "Editor": {"Position": {"X": 400, "Y": 200}}, "Children": [
			{"Id": "%s", "Class": "/Script/AIModule.BTTask_Wait", "Editor": {"Position": {"X": 400, "Y": 400}}}]}]}})"),
		*NodeId(1), *NodeId(2), *NodeId(3), *NodeId(4))))
	{
		return false;
	}
	// The wire from the Sequence to the Selector is broken (Alt+click on the pin): the Selector and its Wait stay.
	UEdGraphPin* SelectorInput = NodePin(GraphNode(Target, NodeId(3)), EGPD_Input);
	UBehaviorTreeGraph* Graph = TreeGraph(Target);
	if (!TestTrue(TEXT("the Selector is wired"), SelectorInput != nullptr && Graph != nullptr && SelectorInput->LinkedTo.Num() == 1))
	{
		return false;
	}
	Graph->GetSchema()->BreakPinLinks(*SelectorInput, true);
	if (!FinishEdit(*this, Target))
	{
		return false;
	}
	TestEqual(TEXT("the runtime tree keeps one child"), RuntimeChildCount(Target), 1);

	const TSharedPtr<FJsonObject> Extracted = TestTolerantRoundTrip(*this, Target, FExpected(), TEXT("Detached branch"));
	if (!Extracted.IsValid())
	{
		return false;
	}
	TestEqual(TEXT("the branch reads as Tree.Detached"), StringAt(Extracted, TEXT("/Body/Tree/Detached/0/Id")), NodeId(3));
	TestEqual(TEXT("with its child"), StringAt(Extracted, TEXT("/Body/Tree/Detached/0/Children/0/Id")), NodeId(4));
	TestFalse(TEXT("Root no longer lists it"), At(Extracted, TEXT("/Body/Tree/Root/Children/1")).IsValid());

	if (TestTrue(TEXT("another node changes"), ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		SetNodeName(Document, TEXT("/Body/Tree/Root/Children/0"), TEXT("Edited wait"));
	}, TEXT("Edit")).IsSuccess()))
	{
		UBehaviorTreeGraphNode* Selector = GraphNode(Target, NodeId(3));
		UEdGraphPin* Output = NodePin(Selector, EGPD_Output);
		TestTrue(TEXT("the branch is kept unwired"), Selector != nullptr && NodePin(Selector, EGPD_Input) != nullptr
			&& NodePin(Selector, EGPD_Input)->LinkedTo.IsEmpty() && Selector->NodePosX == 400 && Selector->NodePosY == 200);
		TestTrue(TEXT("with its child wired"), Output != nullptr && Output->LinkedTo.Num() == 1
			&& Output->LinkedTo[0]->GetOwningNode() == GraphNode(Target, NodeId(4)));
		TestEqual(TEXT("the runtime tree still has one child"), RuntimeChildCount(Target), 1);
		const FAssetDocumentResult After = ExtractResult(Target);
		TestTrue(TEXT("the branch still reads as Tree.Detached"), After.IsSuccess()
			&& StringAt(After.Payload, TEXT("/Body/Tree/Detached/0/Id")) == NodeId(3));
	}

	// A document that rewrites only Root (Detached omitted) keeps the branch.
	if (TestTrue(TEXT("only Root is written"), ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		const TSharedPtr<FJsonValue> Tree = At(Document, TEXT("/Body/Tree"));
		if (Tree.IsValid() && Tree->Type == EJson::Object)
		{
			Tree->AsObject()->RemoveField(TEXT("Detached"));
		}
		SetNodeName(Document, TEXT("/Body/Tree/Root"), TEXT("Rewritten root"));
	}, TEXT("Root only")).IsSuccess()))
	{
		UBehaviorTreeGraphNode* Selector = GraphNode(Target, NodeId(3));
		TestTrue(TEXT("the omitted branch is kept unwired"), Selector != nullptr && NodePin(Selector, EGPD_Input) != nullptr
			&& NodePin(Selector, EGPD_Input)->LinkedTo.IsEmpty() && GraphNode(Target, NodeId(4)) != nullptr);
		const FAssetDocumentResult After = ExtractResult(Target);
		TestTrue(TEXT("the omitted branch still reads as Tree.Detached"), After.IsSuccess()
			&& StringAt(After.Payload, TEXT("/Body/Tree/Detached/0/Id")) == NodeId(3)
			&& StringAt(After.Payload, TEXT("/Body/Tree/Detached/0/Children/0/Id")) == NodeId(4));
	}

	// A decorator class cannot head a main branch, detached or not.
	TestRefused(*this, ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		SetStringAt(Document, TEXT("/Body/Tree/Detached/0/Class"), TEXT("/Script/AIModule.BTDecorator_Cooldown"));
	}, TEXT("Decorator as a branch")), TEXT("InvalidBehaviorTreeNodeClass"), TEXT("/Body/Tree/Detached/0/Class"), TEXT("Decorator as a branch"));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentTolerantBehaviorTreeSamePositionTest, "UEAssetDoc.Tolerant.BehaviorTree.SamePosition",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentTolerantBehaviorTreeSamePositionTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentTolerantTestSupport;
	using namespace AssetDocumentTolerantBehaviorTreeTestsLocal;
	const FTarget Target(TEXT("BT_SamePosition"));
	AddExpectedMessage(TEXT("package was marked as deleted in editor"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1, false);
	ON_SCOPE_EXIT { Target.Cleanup(); };

	if (!CreateTree(*this, Target, FString::Printf(TEXT(R"({"Root": {"Id": "%s", "Class": "/Script/AIModule.BTComposite_Sequence", "Children": [
		{"Id": "%s", "Class": "/Script/AIModule.BTTask_Wait", "Editor": {"Position": {"X": 100, "Y": 200}}},
		{"Id": "%s", "Class": "/Script/AIModule.BTTask_Wait", "Editor": {"Position": {"X": 400, "Y": 200}}}]}})"),
		*NodeId(1), *NodeId(2), *NodeId(3))))
	{
		return false;
	}
	// The second Wait is dragged onto the first one.
	UBehaviorTreeGraphNode* Second = GraphNode(Target, NodeId(3));
	if (!TestNotNull(TEXT("second Wait"), Second))
	{
		return false;
	}
	Second->Modify();
	Second->NodePosX = 100;
	if (!FinishEdit(*this, Target))
	{
		return false;
	}

	// The editor's rebuild sorts children by position and does not keep the order of children at the same position:
	// the order it stored is the one to read and keep.
	const UBehaviorTree* Stored = Target.Get<UBehaviorTree>();
	const UBehaviorTreeGraphNode* StoredFirst = GraphNode(Target, NodeId(2));
	if (!TestTrue(TEXT("the stored tree has both Waits"), Stored != nullptr && Stored->RootNode != nullptr
		&& Stored->RootNode->Children.Num() == 2 && StoredFirst != nullptr))
	{
		return false;
	}
	const bool bSecondFirst = static_cast<const UObject*>(Stored->RootNode->Children[0].ChildTask.Get()) != StoredFirst->NodeInstance.Get();
	const FString FirstId = bSecondFirst ? NodeId(3) : NodeId(2);
	const FString SecondId = bSecondFirst ? NodeId(2) : NodeId(3);

	const TSharedPtr<FJsonObject> Extracted = TestTolerantRoundTrip(*this, Target, FExpected(), TEXT("Same position"));
	if (!Extracted.IsValid())
	{
		return false;
	}
	TestEqual(TEXT("the stored first Wait reads first"), StringAt(Extracted, TEXT("/Body/Tree/Root/Children/0/Id")), FirstId);
	TestEqual(TEXT("the stored second Wait reads second"), StringAt(Extracted, TEXT("/Body/Tree/Root/Children/1/Id")), SecondId);

	if (TestTrue(TEXT("the Sequence changes"), ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		SetNodeName(Document, TEXT("/Body/Tree/Root"), TEXT("Edited sequence"));
	}, TEXT("Edit")).IsSuccess()))
	{
		const UBehaviorTreeGraphNode* First = GraphNode(Target, FirstId);
		const UBehaviorTreeGraphNode* Kept = GraphNode(Target, SecondId);
		TestTrue(TEXT("both Waits keep the shared position"), First != nullptr && Kept != nullptr
			&& First->NodePosX == 100 && Kept->NodePosX == 100 && First->NodePosY == Kept->NodePosY);
		const UBehaviorTree* Tree = Target.Get<UBehaviorTree>();
		TestTrue(TEXT("the runtime order is kept"), Tree != nullptr && Tree->RootNode != nullptr && Tree->RootNode->Children.Num() == 2
			&& First != nullptr && Kept != nullptr
			&& static_cast<const UObject*>(Tree->RootNode->Children[0].ChildTask.Get()) == First->NodeInstance.Get()
			&& static_cast<const UObject*>(Tree->RootNode->Children[1].ChildTask.Get()) == Kept->NodeInstance.Get());
		const FAssetDocumentResult After = ExtractResult(Target);
		TestTrue(TEXT("the order still reads"), After.IsSuccess()
			&& StringAt(After.Payload, TEXT("/Body/Tree/Root/Children/1/Id")) == SecondId);
	}

	TestRefused(*this, ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		const TSharedPtr<FJsonValue> Position = At(Document, TEXT("/Body/Tree/Root/Children/1/Editor/Position"));
		if (Position.IsValid() && Position->Type == EJson::Object)
		{
			Position->AsObject()->SetNumberField(TEXT("X"), 50);
		}
	}, TEXT("Out of order")), TEXT("BehaviorTreeLayoutOrderConflict"), TEXT("/Body/Tree/Root/Children/1/Editor/Position/X"), TEXT("Out of order"));

	// A document with two children at the same position, beside a child with a decorator, is built in its order.
	const FTarget Made(TEXT("BT_SamePositionMade"));
	ON_SCOPE_EXIT { Made.Cleanup(); };
	if (!CreateTree(*this, Made, FString::Printf(TEXT(R"({"Root": {"Id": "%s", "Class": "/Script/AIModule.BTComposite_Sequence", "Children": [
		{"Id": "%s", "Class": "/Script/AIModule.BTTask_Wait", "Editor": {"Position": {"X": 100, "Y": 200}}},
		{"Id": "%s", "Class": "/Script/AIModule.BTTask_Wait", "Editor": {"Position": {"X": 100, "Y": 200}}},
		{"Id": "%s", "Class": "/Script/AIModule.BTTask_Wait", "Editor": {"Position": {"X": 400, "Y": 200}},
			"Decorators": [{"Id": "%s", "Class": "/Script/AIModule.BTDecorator_Cooldown"}]}]}})"),
		*NodeId(1), *NodeId(5), *NodeId(6), *NodeId(7), *NodeId(8))))
	{
		return false;
	}
	const auto RuntimeOrderIs = [&Made](std::initializer_list<uint32> Ids)
	{
		const UBehaviorTree* Tree = Made.Get<UBehaviorTree>();
		if (Tree == nullptr || Tree->RootNode == nullptr || Tree->RootNode->Children.Num() != static_cast<int32>(Ids.size()))
		{
			return false;
		}
		int32 Index = 0;
		for (const uint32 Id : Ids)
		{
			const UBehaviorTreeGraphNode* Node = GraphNode(Made, NodeId(Id));
			if (Node == nullptr || static_cast<const UObject*>(Tree->RootNode->Children[Index++].ChildTask.Get()) != Node->NodeInstance.Get())
			{
				return false;
			}
		}
		return true;
	};
	TestTrue(TEXT("the made tree runs its children in document order"), RuntimeOrderIs({5, 6, 7}));
	const FAssetDocumentResult MadeRead = ExtractResult(Made);
	TestTrue(TEXT("the made tree reads in document order"), MadeRead.IsSuccess()
		&& StringAt(MadeRead.Payload, TEXT("/Body/Tree/Root/Children/0/Id")) == NodeId(5)
		&& StringAt(MadeRead.Payload, TEXT("/Body/Tree/Root/Children/1/Id")) == NodeId(6)
		&& StringAt(MadeRead.Payload, TEXT("/Body/Tree/Root/Children/2/Id")) == NodeId(7));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentTolerantBehaviorTreeBoundGraphTest, "UEAssetDoc.Tolerant.BehaviorTree.BoundGraph",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentTolerantBehaviorTreeBoundGraphTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentTolerantTestSupport;
	using namespace AssetDocumentTolerantBehaviorTreeTestsLocal;
	const FTarget Target(TEXT("BT_BoundGraph"));
	AddExpectedMessage(TEXT("package was marked as deleted in editor"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1, false);
	ON_SCOPE_EXIT { Target.Cleanup(); };

	// A Wait under a composite decorator whose graph is Sink <- And <- (Cooldown, Cooldown).
	if (!CreateTree(*this, Target, FString::Printf(TEXT(R"({"Root": {"Id": "%s", "Class": "/Script/AIModule.BTComposite_Sequence", "Children": [
		{"Id": "%s", "Class": "/Script/AIModule.BTTask_Wait", "Editor": {"Position": {"X": 0, "Y": 200}}, "Decorators": [
			{"Id": "%s", "Kind": "Composite", "Properties": {"CompositeName": "Gate", "bShowOperations": true}, "BoundGraph": {
				"GraphGuid": "%s", "Nodes": [
					{"Id": "%s", "Kind": "Sink"}, {"Id": "%s", "Kind": "And"},
					{"Id": "%s", "Kind": "Test", "Class": "/Script/AIModule.BTDecorator_Cooldown"},
					{"Id": "%s", "Kind": "Test", "Class": "/Script/AIModule.BTDecorator_Cooldown"}],
				"Links": [{"From": "%s", "To": "%s", "ToInput": 0}, {"From": "%s", "To": "%s", "ToInput": 0},
					{"From": "%s", "To": "%s", "ToInput": 1}]}}]}]}})"),
		*NodeId(1), *NodeId(2), *NodeId(5), *NodeId(100), *NodeId(6), *NodeId(7), *NodeId(8), *NodeId(9),
		*NodeId(7), *NodeId(6), *NodeId(8), *NodeId(7), *NodeId(9), *NodeId(7))))
	{
		return false;
	}
	UBehaviorTreeGraphNode_CompositeDecorator* Composite = Cast<UBehaviorTreeGraphNode_CompositeDecorator>(GraphNode(Target, NodeId(5)));
	UEdGraph* Bound = Composite != nullptr ? Composite->BoundGraph.Get() : nullptr;
	UBehaviorTreeDecoratorGraphNode_Logic* And = nullptr;
	for (int32 Index = 0; Bound != nullptr && Index < Bound->Nodes.Num(); ++Index)
	{
		if (Bound->Nodes[Index] != nullptr && Bound->Nodes[Index]->NodeGuid.ToString(EGuidFormats::Digits) == NodeId(7))
		{
			And = Cast<UBehaviorTreeDecoratorGraphNode_Logic>(Bound->Nodes[Index]);
		}
	}
	if (!TestTrue(TEXT("decorator graph made"), Bound != nullptr && And != nullptr))
	{
		return false;
	}
	// The And's Add Input, left unwired; a Not placed but not wired; a comment.
	Bound->Modify();
	And->Modify();
	And->CreatePin(EGPD_Input, TEXT("Transition"), TEXT("In"));
	FGraphNodeCreator<UBehaviorTreeDecoratorGraphNode_Logic> NotCreator(*Bound);
	UBehaviorTreeDecoratorGraphNode_Logic* Not = NotCreator.CreateNode(false);
	Not->LogicMode = EDecoratorLogicMode::Not;
	NotCreator.Finalize();
	Not->NodeGuid = FGuid(0, 0, 0, 10);
	Not->NodePosX = 300;
	Not->NodePosY = 120;
	FGraphNodeCreator<UEdGraphNode_Comment> CommentCreator(*Bound);
	UEdGraphNode_Comment* Comment = CommentCreator.CreateNode(false);
	CommentCreator.Finalize();
	Comment->NodeGuid = FGuid(0, 0, 0, 11);
	Comment->NodeComment = TEXT("Unfinished gate");
	Bound->NotifyGraphChanged();
	Composite->OnInnerGraphChanged();
	if (!FinishEdit(*this, Target))
	{
		return false;
	}

	// Nodes read in identity order: Sink, And, Cooldown, Cooldown, Not.
	const FString Nodes = TEXT("/Body/Tree/Root/Children/0/Decorators/0/BoundGraph/Nodes/");
	FExpected Expected;
	Expected.Warnings = {{TEXT("InvalidBehaviorTreeDecoratorArity"), Nodes + TEXT("1")},
		{TEXT("InvalidBehaviorTreeDecoratorArity"), Nodes + TEXT("4")},
		{TEXT("UnreachableBehaviorTreeDecoratorNode"), Nodes + TEXT("4")}};
	const TSharedPtr<FJsonObject> Extracted = TestTolerantRoundTrip(*this, Target, Expected, TEXT("Decorator graph"));
	if (!Extracted.IsValid())
	{
		return false;
	}
	const TSharedPtr<FJsonValue> InputCount = At(Extracted, Nodes + TEXT("1/InputCount"));
	TestTrue(TEXT("the And reads its three inputs"), InputCount.IsValid() && InputCount->AsNumber() == 3);
	TestEqual(TEXT("the Not reads"), StringAt(Extracted, Nodes + TEXT("4/Kind")), FString(TEXT("Not")));
	TestEqual(TEXT("the comment reads"), StringAt(Extracted, TEXT("/Body/Tree/Root/Children/0/Decorators/0/BoundGraph/Comments/0/Text")),
		FString(TEXT("Unfinished gate")));

	if (TestTrue(TEXT("the Sequence changes"), ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		SetNodeName(Document, TEXT("/Body/Tree/Root"), TEXT("Edited sequence"));
	}, TEXT("Edit")).IsSuccess()))
	{
		const UBehaviorTreeGraphNode_CompositeDecorator* After = Cast<UBehaviorTreeGraphNode_CompositeDecorator>(GraphNode(Target, NodeId(5)));
		int32 Comments = 0, Logic = 0;
		for (int32 Index = 0; After != nullptr && After->BoundGraph != nullptr && Index < After->BoundGraph->Nodes.Num(); ++Index)
		{
			Comments += Cast<UEdGraphNode_Comment>(After->BoundGraph->Nodes[Index]) != nullptr ? 1 : 0;
			Logic += Cast<UBehaviorTreeDecoratorGraphNode_Logic>(After->BoundGraph->Nodes[Index]) != nullptr ? 1 : 0;
		}
		TestEqual(TEXT("the comment is kept"), Comments, 1);
		TestEqual(TEXT("Sink, And and Not are kept"), Logic, 3);
		TestStillShows(*this, Target, TEXT("InvalidBehaviorTreeDecoratorArity"), Nodes + TEXT("1"), TEXT("After edit"));
		TestStillShows(*this, Target, TEXT("UnreachableBehaviorTreeDecoratorNode"), Nodes + TEXT("4"), TEXT("After edit"));
	}

	TestRefused(*this, ApplyEdited(*this, Target, [&Nodes](const TSharedPtr<FJsonObject>& Document)
	{
		const TSharedPtr<FJsonValue> Node = At(Document, Nodes + TEXT("1"));
		if (Node.IsValid() && Node->Type == EJson::Object)
		{
			Node->AsObject()->SetNumberField(TEXT("InputCount"), 4);
		}
	}, TEXT("Another open input")), TEXT("InvalidBehaviorTreeDecoratorArity"), Nodes + TEXT("1"), TEXT("Another open input"));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentTolerantBehaviorTreeUnsupportedPropertyTest,
	"UEAssetDoc.Tolerant.BehaviorTree.UnsupportedProperty", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentTolerantBehaviorTreeUnsupportedPropertyTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentTolerantTestSupport;
	using namespace AssetDocumentTolerantBehaviorTreeTestsLocal;
	const FTarget Target(TEXT("BT_UnsupportedProperty"));
	AddExpectedMessage(TEXT("package was marked as deleted in editor"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1, false);
	ON_SCOPE_EXIT { Target.Cleanup(); };
	// A task whose map has integer keys: a value with no document form.
	const TCHAR* const TaskClass = TEXT("/Script/AssetDocumentTests.AssetDocumentUnsupportedExtractTaskTestNode");
	if (!TestNotNull(TEXT("task class with an integer-keyed map"), LoadClass<UBTTaskNode>(nullptr, TaskClass)))
	{
		return false;
	}
	if (!CreateTree(*this, Target, FString::Printf(TEXT(R"({"Root": {"Id": "%s", "Class": "/Script/AIModule.BTComposite_Sequence", "Children": [
		{"Id": "%s", "Class": "/Script/AIModule.BTTask_Wait", "Editor": {"Position": {"X": 0, "Y": 200}}},
		{"Id": "%s", "Class": "%s", "Editor": {"Position": {"X": 400, "Y": 200}}}]}})"),
		*NodeId(1), *NodeId(2), *NodeId(3), TaskClass)))
	{
		return false;
	}
	// The map gets an entry in the details panel.
	UBehaviorTreeGraphNode* Task = GraphNode(Target, NodeId(3));
	UObject* Instance = Task != nullptr ? Task->NodeInstance.Get() : nullptr;
	const FProperty* Map = Instance != nullptr ? FindFProperty<FMapProperty>(Instance->GetClass(), TEXT("UnsupportedMap")) : nullptr;
	if (!TestTrue(TEXT("task instance with its map"), Map != nullptr))
	{
		return false;
	}
	Instance->Modify();
	Map->ImportText_InContainer(TEXT("((1, 2.500000))"), Instance, Instance, PPF_None);
	FString Stored;
	Map->ExportText_InContainer(0, Stored, Instance, nullptr, nullptr, PPF_None);
	if (!TestFalse(TEXT("the map holds an entry"), Stored.IsEmpty() || Stored == TEXT("()")) || !FinishEdit(*this, Target))
	{
		return false;
	}

	const FString Marker = TEXT("/Body/Tree/Root/Children/1/Properties/UnsupportedMap");
	FExpected Expected;
	Expected.Warnings = {{TEXT("BehaviorTree.Unrepresented"), Marker}};
	Expected.Markers = {Marker};
	const TSharedPtr<FJsonObject> Extracted = TestTolerantRoundTrip(*this, Target, Expected, TEXT("Integer-keyed map"));
	if (!Extracted.IsValid())
	{
		return false;
	}

	if (TestTrue(TEXT("another node changes"), ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		SetNodeName(Document, TEXT("/Body/Tree/Root/Children/0"), TEXT("Edited wait"));
	}, TEXT("Edit")).IsSuccess()))
	{
		const UBehaviorTreeGraphNode* Rebuilt = GraphNode(Target, NodeId(3));
		const UObject* Kept = Rebuilt != nullptr ? Rebuilt->NodeInstance.Get() : nullptr;
		FString After;
		if (Kept != nullptr && Kept->GetClass() == Map->GetOwnerClass())
		{
			Map->ExportText_InContainer(0, After, Kept, nullptr, nullptr, PPF_None);
		}
		TestEqual(TEXT("the rebuilt task keeps the map's entry"), After, Stored);
		TestStillShows(*this, Target, TEXT("BehaviorTree.Unrepresented"), Marker, TEXT("After edit"));
	}

	// The marker is the asset's: its text cannot be edited, nor can a value be written in its place.
	TestRefused(*this, ApplyEdited(*this, Target, [&Marker](const TSharedPtr<FJsonObject>& Document)
	{
		const TSharedPtr<FJsonValue> Value = At(Document, Marker);
		if (Value.IsValid() && Value->Type == EJson::Object)
		{
			Value->AsObject()->SetStringField(TEXT("Text"), TEXT("((1, 9.000000))"));
		}
	}, TEXT("Edited marker")), TEXT("BehaviorTree.UnrepresentedValue"), Marker, TEXT("Edited marker"));
	TestRefused(*this, ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		const TSharedPtr<FJsonValue> Node = At(Document, TEXT("/Body/Tree/Root/Children/1"));
		if (Node.IsValid() && Node->Type == EJson::Object)
		{
			const TSharedPtr<FJsonObject> Value = MakeShared<FJsonObject>();
			Value->SetNumberField(TEXT("1"), 9.0);
			Node->AsObject()->GetObjectField(TEXT("Properties"))->SetObjectField(TEXT("UnsupportedMap"), Value);
		}
	}, TEXT("Value for the map")), TEXT("BehaviorTree.UnrepresentedValue"), Marker, TEXT("Value for the map"));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentTolerantBehaviorTreeMissingClassTest,
	"UEAssetDoc.Tolerant.BehaviorTree.MissingClass", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentTolerantBehaviorTreeMissingClassTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentTolerantTestSupport;
	using namespace AssetDocumentTolerantBehaviorTreeTestsLocal;
	const FTarget Target(TEXT("BT_MissingClass"));
	AddExpectedMessage(TEXT("package was marked as deleted in editor"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1, false);
	// The engine reports each node whose class does not load when the graph is checked (UAIGraph::UpdateErrorMessages),
	// and a rebuilt node tries to load its class's package once.
	AddExpectedErrorPlain(TEXT("class missing. Referenced by"), EAutomationExpectedErrorFlags::Contains, 0);
	for (const TCHAR* Lost : {TEXT("BTT_Gone"), TEXT("BTD_Gone"), TEXT("BTS_Gone")})
	{
		AddExpectedMessage(LostClassFolder() / Lost, ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1, false);
	}
	ON_SCOPE_EXIT { Target.Cleanup(); };

	// A Sequence with a Wait (a decorator on it) and a second Wait.
	if (!CreateTree(*this, Target, FString::Printf(TEXT(R"({"Root": {"Id": "%s", "Class": "/Script/AIModule.BTComposite_Sequence", "Children": [
		{"Id": "%s", "Class": "/Script/AIModule.BTTask_Wait", "Editor": {"Position": {"X": 0, "Y": 200}},
			"Decorators": [{"Id": "%s", "Class": "/Script/AIModule.BTDecorator_Cooldown"}]},
		{"Id": "%s", "Class": "/Script/AIModule.BTTask_Wait", "Editor": {"Position": {"X": 400, "Y": 200}}}]}})"),
		*NodeId(1), *NodeId(2), *NodeId(4), *NodeId(3))))
	{
		return false;
	}
	// A service added on the first Wait in the editor, its Blackboard key not picked yet.
	UBehaviorTreeGraphNode* FirstWait = GraphNode(Target, NodeId(2));
	if (!TestNotNull(TEXT("first Wait"), FirstWait))
	{
		return false;
	}
	UBehaviorTreeGraphNode_Service* Service = NewObject<UBehaviorTreeGraphNode_Service>(TreeGraph(Target));
	Service->ClassData = FGraphNodeClassData(UBTService_DefaultFocus::StaticClass(), FString());
	FirstWait->AddSubNode(Service, TreeGraph(Target));
	Service->NodeGuid = FGuid(0, 0, 0, 5);
	// The classes of the second Wait, the decorator and the service are gone: their nodes keep their class data.
	if (!LoseNodeClass(*this, GraphNode(Target, NodeId(3)), TEXT("BTT_Gone"))
		|| !LoseNodeClass(*this, GraphNode(Target, NodeId(4)), TEXT("BTD_Gone"))
		|| !LoseNodeClass(*this, GraphNode(Target, NodeId(5)), TEXT("BTS_Gone")) || !FinishEdit(*this, Target))
	{
		return false;
	}
	TestEqual(TEXT("the runtime tree leaves the task out"), RuntimeChildCount(Target), 1);

	const FString TaskMarker = TEXT("/Body/Tree/Root/Children/1");
	const FString DecoratorMarker = TEXT("/Body/Tree/Root/Children/0/Decorators/0");
	const FString ServiceMarker = TEXT("/Body/Tree/Root/Children/0/Services/0");
	FExpected Expected;
	Expected.Warnings = {{TEXT("BehaviorTree.Unrepresented"), TaskMarker}, {TEXT("BehaviorTree.Unrepresented"), DecoratorMarker},
		{TEXT("BehaviorTree.Unrepresented"), ServiceMarker}};
	Expected.Markers = {TaskMarker, DecoratorMarker, ServiceMarker};
	const TSharedPtr<FJsonObject> Extracted = TestTolerantRoundTrip(*this, Target, Expected, TEXT("Missing classes"));
	if (!Extracted.IsValid())
	{
		return false;
	}
	TestEqual(TEXT("the task reads as its class"), StringAt(Extracted, TaskMarker + TEXT("/Class")),
		LostClassFolder() / TEXT("BTT_Gone.BTT_Gone_C"));
	TestFalse(TEXT("without properties"), At(Extracted, TaskMarker + TEXT("/Properties")).IsValid());

	if (TestTrue(TEXT("another node changes"), ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		SetNodeName(Document, TEXT("/Body/Tree/Root/Children/0"), TEXT("Edited wait"));
	}, TEXT("Edit")).IsSuccess()))
	{
		const UBehaviorTreeGraphNode* Task = GraphNode(Target, NodeId(3));
		TestTrue(TEXT("the task is kept with its class data"), KeepsLostClass(Target, NodeId(3), TEXT("BTT_Gone"))
			&& Task->NodePosX == 400 && Task->NodePosY == 200);
		TestTrue(TEXT("the decorator is kept with its class data"), KeepsLostClass(Target, NodeId(4), TEXT("BTD_Gone")));
		TestTrue(TEXT("the service is kept with its class data"), KeepsLostClass(Target, NodeId(5), TEXT("BTS_Gone")));
		TestEqual(TEXT("the runtime tree still leaves the task out"), RuntimeChildCount(Target), 1);
		TestStillShows(*this, Target, TEXT("BehaviorTree.Unrepresented"), TaskMarker, TEXT("After edit"));
		TestStillShows(*this, Target, TEXT("BehaviorTree.Unrepresented"), ServiceMarker, TEXT("After edit"));
	}

	// A marked node is the asset's: its class cannot be changed, nor properties given to it, nor a new one added.
	TestRefused(*this, ApplyEdited(*this, Target, [&TaskMarker](const TSharedPtr<FJsonObject>& Document)
	{
		SetStringAt(Document, TaskMarker + TEXT("/Class"), LostClassFolder() / TEXT("BTT_Other.BTT_Other_C"));
	}, TEXT("Edited class")), TEXT("BehaviorTree.UnrepresentedValue"), TaskMarker, TEXT("Edited class"));
	TestRefused(*this, ApplyEdited(*this, Target, [&ServiceMarker](const TSharedPtr<FJsonObject>& Document)
	{
		const TSharedPtr<FJsonValue> Service = At(Document, ServiceMarker);
		if (Service.IsValid() && Service->Type == EJson::Object)
		{
			Service->AsObject()->SetObjectField(TEXT("Properties"), MakeShared<FJsonObject>());
		}
	}, TEXT("Properties for the service")), TEXT("BehaviorTree.UnrepresentedValue"), ServiceMarker + TEXT("/Properties"),
		TEXT("Properties for the service"));
	TestRefused(*this, ApplyEdited(*this, Target, [&TaskMarker](const TSharedPtr<FJsonObject>& Document)
	{
		const TSharedPtr<FJsonValue> Task = At(Document, TaskMarker);
		const TSharedPtr<FJsonValue> Children = At(Document, TEXT("/Body/Tree/Root/Children"));
		if (Task.IsValid() && Task->Type == EJson::Object && Children.IsValid() && Children->Type == EJson::Array)
		{
			const TSharedPtr<FJsonObject> Copy = MakeShared<FJsonObject>(*Task->AsObject());
			Copy->SetStringField(TEXT("Id"), NodeId(6));
			Copy->RemoveField(TEXT("Editor"));
			TArray<TSharedPtr<FJsonValue>> Items = Children->AsArray();
			Items.Add(MakeShared<FJsonValueObject>(Copy));
			Document->GetObjectField(TEXT("Body"))->GetObjectField(TEXT("Tree"))->GetObjectField(TEXT("Root"))
				->SetArrayField(TEXT("Children"), Items);
		}
	}, TEXT("New marked node")), TEXT("BehaviorTree.UnrepresentedValue"), TEXT("/Body/Tree/Root/Children/2"), TEXT("New marked node"));
	return !HasAnyErrors();
}

#endif
