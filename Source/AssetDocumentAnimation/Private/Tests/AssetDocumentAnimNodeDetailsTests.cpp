// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// Animation Blueprint node details made in the editor and read, edited and re-applied through the document:
// - the Functions section of an anim node (On Initial Update / On Become Relevant / On Update);
// - a Property Access node's path and call site;
// - transitions sharing their rules and crossfade settings ("Promote to shared");
// - the exposed pins of a Linked Anim Graph node (Details > Inputs).

#include "AssetDocumentTolerantTestSupport.h"

#include "Animation/AnimBlueprint.h"
#include "Animation/AnimBlueprintGeneratedClass.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "AnimGraphNode_LinkedAnimGraph.h"
#include "AnimGraphNode_Root.h"
#include "AnimGraphNode_SequencePlayer.h"
#include "AnimGraphNode_StateMachine.h"
#include "AnimStateEntryNode.h"
#include "AnimStateNode.h"
#include "AnimStateTransitionNode.h"
#include "AnimationGraphSchema.h"
#include "AnimationStateMachineGraph.h"
#include "AnimationStateMachineSchema.h"
#include "AnimationTransitionGraph.h"
#include "AnimGraphNode_TransitionResult.h"
#include "BlueprintNodeBinder.h"
#include "BlueprintNodeSpawner.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphSchema.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_VariableGet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/ScopeExit.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace AssetDocumentAnimNodeDetailsTests
{
using namespace AssetDocumentTolerantTestSupport;

const TCHAR* const DetailsSkeleton = TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/TutorialTPP_Skeleton.TutorialTPP_Skeleton");
const TCHAR* const DetailsIdle = TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/Tutorial_Idle.Tutorial_Idle");
const TCHAR* const ThreadSafeUpdatePrototype = TEXT("/Script/AnimGraphRuntime.AnimExecutionContextLibrary.Prototype_ThreadSafeAnimUpdateCall");
const TCHAR* const PropertyAccessNodeClass = TEXT("/Script/PropertyAccessNode.K2Node_PropertyAccess");

// A new Animation Blueprint of the tutorial skeleton in Target's package (New Asset > Animation Blueprint).
UAnimBlueprint* NewAnimBlueprint(const FTarget& Target)
{
	USkeleton* Skeleton = LoadObject<USkeleton>(nullptr, DetailsSkeleton);
	UAnimBlueprint* Blueprint = Skeleton == nullptr ? nullptr : Cast<UAnimBlueprint>(FKismetEditorUtilities::CreateBlueprint(
		UAnimInstance::StaticClass(), CreatePackage(*Target.PackageName), FName(*Target.AssetName), BPTYPE_Normal,
		UAnimBlueprint::StaticClass(), UAnimBlueprintGeneratedClass::StaticClass()));
	if (Blueprint != nullptr)
	{
		Blueprint->TargetSkeleton = Skeleton;
		FAssetRegistryModule::AssetCreated(Blueprint);
	}
	return Blueprint;
}

UEdGraph* AnimGraphOf(UAnimBlueprint& Blueprint)
{
	const TObjectPtr<UEdGraph>* Graph = Blueprint.FunctionGraphs.FindByPredicate([](const UEdGraph* Candidate)
	{
		return Candidate != nullptr && Candidate->Schema != nullptr && Candidate->Schema->IsChildOf(UAnimationGraphSchema::StaticClass());
	});
	return Graph != nullptr ? Graph->Get() : nullptr;
}

UEdGraphNode* RootOf(const UEdGraph& Graph)
{
	for (UEdGraphNode* Node : Graph.Nodes)
	{
		if (Node != nullptr && Node->IsA<UAnimGraphNode_Root>())
		{
			return Node;
		}
	}
	return nullptr;
}

UEdGraphNode* Place(UClass* Class, UEdGraph& Graph, const FVector2D& Location)
{
	const TStrongObjectPtr<UBlueprintNodeSpawner> Spawner(UBlueprintNodeSpawner::Create(Class));
	return Spawner->Invoke(&Graph, IBlueprintNodeBinder::FBindingSet(), Location);
}

UEdGraphPin* FirstPin(UEdGraphNode* Node, EEdGraphPinDirection Direction)
{
	if (Node == nullptr)
	{
		return nullptr;
	}
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin != nullptr && Pin->Direction == Direction && !Pin->bHidden)
		{
			return Pin;
		}
	}
	return nullptr;
}

// A link dragged from From to To.
bool Link(UEdGraphPin* From, UEdGraphPin* To)
{
	return From != nullptr && To != nullptr && From->GetSchema()->TryCreateConnection(From, To);
}

bool Compiles(UBlueprint& Blueprint)
{
	FKismetEditorUtilities::CompileBlueprint(&Blueprint, EBlueprintCompileOptions::SkipGarbageCollection);
	return Blueprint.Status == BS_UpToDate || Blueprint.Status == BS_UpToDateWithWarnings;
}

void AddFloatVariable(UBlueprint& Blueprint, const TCHAR* Name)
{
	FBlueprintEditorUtils::AddMemberVariable(&Blueprint, Name, FEdGraphPinType(UEdGraphSchema_K2::PC_Real, UEdGraphSchema_K2::PC_Float,
		nullptr, EPinContainerType::None, false, FEdGraphTerminalType()));
}

// A function made by a node function picker's "Create Binding": the prototype's parameters, thread safe.
UEdGraph* AddBindingFunction(UBlueprint& Blueprint, const TCHAR* Name)
{
	UFunction* Prototype = FindObject<UFunction>(nullptr, ThreadSafeUpdatePrototype);
	UEdGraph* Graph = FBlueprintEditorUtils::CreateNewGraph(&Blueprint, Name, UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
	FBlueprintEditorUtils::AddFunctionGraph<UFunction>(&Blueprint, Graph, true, Prototype);
	TArray<UK2Node_FunctionEntry*> Entries;
	Graph->GetNodesOfClass(Entries);
	if (Prototype != nullptr && FBlueprintEditorUtils::HasFunctionBlueprintThreadSafeMetaData(Prototype) && !Entries.IsEmpty())
	{
		Entries[0]->MetaData.bThreadSafe = true;
	}
	return Graph;
}

// Every node object (one with an "Action") anywhere in the document.
void CollectNodes(const TSharedPtr<FJsonValue>& Value, TArray<TSharedPtr<FJsonObject>>& Out)
{
	if (!Value.IsValid())
	{
		return;
	}
	if (Value->Type == EJson::Array)
	{
		for (const TSharedPtr<FJsonValue>& Item : Value->AsArray())
		{
			CollectNodes(Item, Out);
		}
	}
	else if (Value->Type == EJson::Object)
	{
		const TSharedPtr<FJsonObject> Object = Value->AsObject();
		if (Object->HasTypedField<EJson::String>(TEXT("Action")))
		{
			Out.Add(Object);
		}
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Object->Values)
		{
			CollectNodes(Field.Value, Out);
		}
	}
}

// The first document node whose Action contains ActionPart (and, when given, whose From/To are the named states).
TSharedPtr<FJsonObject> FindNode(const TSharedPtr<FJsonObject>& Document, const TCHAR* ActionPart, const TCHAR* From = nullptr,
	const TCHAR* To = nullptr)
{
	TArray<TSharedPtr<FJsonObject>> Nodes;
	CollectNodes(MakeShared<FJsonValueObject>(Document), Nodes);
	for (const TSharedPtr<FJsonObject>& Node : Nodes)
	{
		if (Node != Document && Node->GetStringField(TEXT("Action")).Contains(ActionPart)
			&& (From == nullptr || Node->GetStringField(TEXT("From")) == From) && (To == nullptr || Node->GetStringField(TEXT("To")) == To))
		{
			return Node;
		}
	}
	return nullptr;
}

// The node's Properties object, made when it has none.
TSharedPtr<FJsonObject> PropertiesOf(const TSharedPtr<FJsonObject>& Node)
{
	const TSharedPtr<FJsonObject>* Found = nullptr;
	if (Node->TryGetObjectField(TEXT("Properties"), Found))
	{
		return *Found;
	}
	const TSharedPtr<FJsonObject> Made = MakeShared<FJsonObject>();
	Node->SetObjectField(TEXT("Properties"), Made);
	return Made;
}

FString StringSetting(const TSharedPtr<FJsonObject>& Node, const TCHAR* Key)
{
	FString Value;
	const TSharedPtr<FJsonObject>* Properties = nullptr;
	if (Node.IsValid() && Node->TryGetObjectField(TEXT("Properties"), Properties))
	{
		(*Properties)->TryGetStringField(Key, Value);
	}
	return Value;
}

bool HasSetting(const TSharedPtr<FJsonObject>& Node, const TCHAR* Key)
{
	const TSharedPtr<FJsonObject>* Properties = nullptr;
	return Node.IsValid() && Node->TryGetObjectField(TEXT("Properties"), Properties) && (*Properties)->HasField(Key);
}

template <typename T>
T* FirstNodeOf(const FTarget& Target)
{
	TArray<T*> Nodes;
	if (UBlueprint* Blueprint = Target.Get<UBlueprint>())
	{
		FBlueprintEditorUtils::GetAllNodesOfClass(Blueprint, Nodes);
	}
	return Nodes.IsEmpty() ? nullptr : Nodes[0];
}

UAnimStateTransitionNode* TransitionOf(const FTarget& Target, const FString& From, const FString& To)
{
	TArray<UAnimStateTransitionNode*> Transitions;
	if (UBlueprint* Blueprint = Target.Get<UBlueprint>())
	{
		FBlueprintEditorUtils::GetAllNodesOfClass(Blueprint, Transitions);
	}
	for (UAnimStateTransitionNode* Transition : Transitions)
	{
		if (Transition->GetPreviousState() != nullptr && Transition->GetNextState() != nullptr
			&& Transition->GetPreviousState()->GetStateName() == From && Transition->GetNextState()->GetStateName() == To)
		{
			return Transition;
		}
	}
	return nullptr;
}

FString MemberName(const FMemberReference& Reference)
{
	return Reference.GetMemberName().IsNone() ? FString() : Reference.GetMemberName().ToString();
}

// Whether the Linked Anim Graph node shows the input pin of the target class's variable Name.
bool ShowsPin(const UAnimGraphNode_LinkedAnimGraph* Node, const TCHAR* Name)
{
	// CustomPinProperties is protected: read through its property, as the Details panel does.
	const FArrayProperty* Property = FindFProperty<FArrayProperty>(UAnimGraphNode_CustomProperty::StaticClass(), TEXT("CustomPinProperties"));
	const TArray<FOptionalPinFromProperty>* Pins = Node != nullptr && Property != nullptr
		? Property->ContainerPtrToValuePtr<TArray<FOptionalPinFromProperty>>(Node) : nullptr;
	const FOptionalPinFromProperty* Pin = Pins == nullptr ? nullptr : Pins->FindByPredicate(
		[Name](const FOptionalPinFromProperty& Candidate) { return Candidate.PropertyName == Name; });
	return Pin != nullptr && Pin->bShowPin;
}
} // namespace AssetDocumentAnimNodeDetailsTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAnimNodeFunctionsTest, "UEAssetDoc.Fidelity.AnimBlueprint.NodeFunctions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAnimNodeFunctionsTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentTolerantTestSupport;
	using namespace AssetDocumentAnimNodeDetailsTests;
	const FTarget Target(TEXT("ABP_NodeFunctions"));
	AddExpectedMessage(TEXT("package was marked as deleted in editor"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1, false);
	ON_SCOPE_EXIT { Target.Cleanup(); };

	// A Sequence Player into the Output Pose whose On Update calls OnPlayerUpdate, a function made by "Create Binding";
	// OnPlayerRelevant is another such function and PlainFunction an ordinary one.
	UAnimBlueprint* Blueprint = NewAnimBlueprint(Target);
	UAnimSequence* Idle = LoadObject<UAnimSequence>(nullptr, DetailsIdle);
	UEdGraph* AnimGraph = Blueprint != nullptr ? AnimGraphOf(*Blueprint) : nullptr;
	if (!TestNotNull(TEXT("animation blueprint"), AnimGraph) || !TestNotNull(TEXT("idle"), Idle))
	{
		return false;
	}
	AddBindingFunction(*Blueprint, TEXT("OnPlayerUpdate"));
	AddBindingFunction(*Blueprint, TEXT("OnPlayerRelevant"));
	UEdGraph* Plain = FBlueprintEditorUtils::CreateNewGraph(Blueprint, TEXT("PlainFunction"), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
	FBlueprintEditorUtils::AddFunctionGraph<UFunction>(Blueprint, Plain, true, nullptr);
	UAnimGraphNode_SequencePlayer* Player = Cast<UAnimGraphNode_SequencePlayer>(Place(UAnimGraphNode_SequencePlayer::StaticClass(), *AnimGraph, FVector2D(-300.0, 0.0)));
	if (!TestNotNull(TEXT("sequence player"), Player))
	{
		return false;
	}
	Player->SetAnimationAsset(Idle);
	TestTrue(TEXT("player into the Output Pose"), Link(FirstPin(Player, EGPD_Output), FirstPin(RootOf(*AnimGraph), EGPD_Input)));
	TestTrue(TEXT("fixture compiles"), Compiles(*Blueprint));
	UFunction* Update = Blueprint->SkeletonGeneratedClass->FindFunctionByName(TEXT("OnPlayerUpdate"));
	if (!TestNotNull(TEXT("OnPlayerUpdate compiled"), Update))
	{
		return false;
	}
	Player->UpdateFunction.SetFromField<UFunction>(Update, true);
	TestTrue(TEXT("bound fixture compiles"), Compiles(*Blueprint));
	if (!TestTrue(TEXT("saved"), Target.Save()))
	{
		return false;
	}

	// Extract names the bound function; the extract re-applies as a no-op.
	const TSharedPtr<FJsonObject> Extracted = TestTolerantRoundTrip(*this, Target, FExpected(), TEXT("Node functions"));
	if (!Extracted.IsValid())
	{
		return false;
	}
	const TSharedPtr<FJsonObject> PlayerDoc = FindNode(Extracted, TEXT("Anim:SequencePlayer"));
	TestEqual(TEXT("On Update reads back"), StringSetting(PlayerDoc, TEXT("Functions.OnUpdate")), FString(TEXT("OnPlayerUpdate")));
	TestFalse(TEXT("an unbound function is omitted"), HasSetting(PlayerDoc, TEXT("Functions.OnInitialUpdate")));

	// Bind On Become Relevant; On Update, left out, keeps its function.
	const FAssetDocumentResult Bound = ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		const TSharedPtr<FJsonObject> Properties = PropertiesOf(FindNode(Document, TEXT("Anim:SequencePlayer")));
		Properties->SetStringField(TEXT("Functions.OnBecomeRelevant"), TEXT("OnPlayerRelevant"));
		Properties->RemoveField(TEXT("Functions.OnUpdate"));
	}, TEXT("Bind On Become Relevant"));
	if (TestTrue(TEXT("binding applies"), Bound.IsSuccess()))
	{
		const UAnimGraphNode_SequencePlayer* Live = FirstNodeOf<UAnimGraphNode_SequencePlayer>(Target);
		TestEqual(TEXT("On Become Relevant is bound"), Live != nullptr ? MemberName(Live->BecomeRelevantFunction) : FString(), FString(TEXT("OnPlayerRelevant")));
		TestEqual(TEXT("On Update, left out, is kept"), Live != nullptr ? MemberName(Live->UpdateFunction) : FString(), FString(TEXT("OnPlayerUpdate")));
	}

	// "" unbinds.
	const FAssetDocumentResult Unbound = ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		PropertiesOf(FindNode(Document, TEXT("Anim:SequencePlayer")))->SetStringField(TEXT("Functions.OnUpdate"), TEXT(""));
	}, TEXT("Unbind On Update"));
	if (TestTrue(TEXT("unbinding applies"), Unbound.IsSuccess()))
	{
		const UAnimGraphNode_SequencePlayer* Live = FirstNodeOf<UAnimGraphNode_SequencePlayer>(Target);
		TestEqual(TEXT("On Update is unbound"), Live != nullptr ? MemberName(Live->UpdateFunction) : FString(TEXT("?")), FString());
	}

	// A function the picker does not offer (not thread safe, or no such function) is refused.
	const FAssetDocumentResult NotThreadSafe = ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		PropertiesOf(FindNode(Document, TEXT("Anim:SequencePlayer")))->SetStringField(TEXT("Functions.OnInitialUpdate"), TEXT("PlainFunction"));
	}, TEXT("Bind a function that is not thread safe"));
	TestRefused(*this, NotThreadSafe, TEXT("Graph.InvalidArgument"), FString(), TEXT("Function not thread safe"));
	const FAssetDocumentResult Missing = ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		PropertiesOf(FindNode(Document, TEXT("Anim:SequencePlayer")))->SetStringField(TEXT("Functions.OnInitialUpdate"), TEXT("NoSuchFunction"));
	}, TEXT("Bind a function that does not exist"));
	TestRefused(*this, Missing, TEXT("Graph.InvalidArgument"), FString(), TEXT("No such function"));
	TestTolerantRoundTrip(*this, Target, FExpected(), TEXT("Node functions after edits"));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAnimPropertyAccessNodeTest, "UEAssetDoc.Fidelity.AnimBlueprint.PropertyAccessNode",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAnimPropertyAccessNodeTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentTolerantTestSupport;
	using namespace AssetDocumentAnimNodeDetailsTests;
	const FTarget Target(TEXT("ABP_PropertyAccessNode"));
	AddExpectedMessage(TEXT("package was marked as deleted in editor"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1, false);
	ON_SCOPE_EXIT { Target.Cleanup(); };

	// A Property Access node in the AnimGraph reading Speed, its call site set to Thread Safe.
	UAnimBlueprint* Blueprint = NewAnimBlueprint(Target);
	UEdGraph* AnimGraph = Blueprint != nullptr ? AnimGraphOf(*Blueprint) : nullptr;
	UClass* AccessClass = FindObject<UClass>(nullptr, PropertyAccessNodeClass);
	if (!TestNotNull(TEXT("animation blueprint"), AnimGraph) || !TestNotNull(TEXT("Property Access node class"), AccessClass))
	{
		return false;
	}
	AddFloatVariable(*Blueprint, TEXT("Speed"));
	AddFloatVariable(*Blueprint, TEXT("Height"));
	TestTrue(TEXT("variables compile"), Compiles(*Blueprint));
	UEdGraphNode* Access = Place(AccessClass, *AnimGraph, FVector2D(-400.0, 300.0));
	FArrayProperty* PathProperty = FindFProperty<FArrayProperty>(AccessClass, TEXT("Path"));
	FNameProperty* ContextProperty = FindFProperty<FNameProperty>(AccessClass, TEXT("ContextId"));
	if (!TestNotNull(TEXT("Property Access node"), Access) || !TestNotNull(TEXT("Path"), PathProperty) || !TestNotNull(TEXT("ContextId"), ContextProperty))
	{
		return false;
	}
	// The path picker (SetPath) and the Call Site menu (SetContextId).
	*PathProperty->ContainerPtrToValuePtr<TArray<FString>>(Access) = {TEXT("Speed")};
	*ContextProperty->ContainerPtrToValuePtr<FName>(Access) = TEXT("UnBatched_ThreadSafe");
	Access->ReconstructNode();
	TestTrue(TEXT("fixture compiles"), Compiles(*Blueprint));
	if (!TestTrue(TEXT("saved"), Target.Save()))
	{
		return false;
	}

	const TSharedPtr<FJsonObject> Extracted = TestTolerantRoundTrip(*this, Target, FExpected(), TEXT("Property Access node"));
	if (!Extracted.IsValid())
	{
		return false;
	}
	const TSharedPtr<FJsonObject> AccessDoc = FindNode(Extracted, TEXT("PropertyAccess"));
	TestEqual(TEXT("Path reads back"), StringSetting(AccessDoc, TEXT("Path")), FString(TEXT("Speed")));
	TestEqual(TEXT("ContextId reads back"), StringSetting(AccessDoc, TEXT("ContextId")), FString(TEXT("UnBatched_ThreadSafe")));

	// Another path, call site back to Automatic.
	const FAssetDocumentResult Edited = ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		const TSharedPtr<FJsonObject> Properties = PropertiesOf(FindNode(Document, TEXT("PropertyAccess")));
		Properties->SetStringField(TEXT("Path"), TEXT("Height"));
		Properties->SetStringField(TEXT("ContextId"), TEXT(""));
	}, TEXT("Read Height automatically"));
	if (TestTrue(TEXT("path and call site apply"), Edited.IsSuccess()))
	{
		UEdGraphNode* Live = nullptr;
		TArray<UEdGraphNode*> Nodes;
		FBlueprintEditorUtils::GetAllNodesOfClass(Target.Get<UBlueprint>(), Nodes);
		for (UEdGraphNode* Node : Nodes)
		{
			Live = Live == nullptr && Node->GetClass() == AccessClass ? Node : Live;
		}
		const TArray<FString>* Path = Live != nullptr ? PathProperty->ContainerPtrToValuePtr<TArray<FString>>(Live) : nullptr;
		TestTrue(TEXT("the node reads Height"), Path != nullptr && *Path == TArray<FString>{TEXT("Height")});
		TestTrue(TEXT("the call site is Automatic"), Live != nullptr && ContextProperty->GetPropertyValue_InContainer(Live).IsNone());
	}

	// A path that does not resolve and an unknown call site are refused.
	const FAssetDocumentResult BadPath = ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		PropertiesOf(FindNode(Document, TEXT("PropertyAccess")))->SetStringField(TEXT("Path"), TEXT("NoSuchMember"));
	}, TEXT("Path that does not resolve"));
	TestRefused(*this, BadPath, TEXT("Graph.InvalidArgument"), FString(), TEXT("Unresolved path"));
	const FAssetDocumentResult BadSite = ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		PropertiesOf(FindNode(Document, TEXT("PropertyAccess")))->SetStringField(TEXT("ContextId"), TEXT("Nowhere"));
	}, TEXT("Unknown call site"));
	TestRefused(*this, BadSite, TEXT("Graph.InvalidArgument"), FString(), TEXT("Unknown call site"));
	TestTolerantRoundTrip(*this, Target, FExpected(), TEXT("Property Access node after edits"));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAnimSharedTransitionRulesTest, "UEAssetDoc.Fidelity.AnimBlueprint.SharedTransitionRules",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAnimSharedTransitionRulesTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentTolerantTestSupport;
	using namespace AssetDocumentAnimNodeDetailsTests;
	const FTarget Target(TEXT("ABP_SharedRules"));
	AddExpectedMessage(TEXT("package was marked as deleted in editor"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1, false);
	ON_SCOPE_EXIT { Target.Cleanup(); };

	// A state machine Idle -> Walk -> Run -> Idle; Idle -> Walk's rule is promoted to the shared rule "Moving", which
	// Walk -> Run uses; Idle -> Walk's crossfade settings are promoted to "Smooth".
	UAnimBlueprint* Blueprint = NewAnimBlueprint(Target);
	UEdGraph* AnimGraph = Blueprint != nullptr ? AnimGraphOf(*Blueprint) : nullptr;
	if (!TestNotNull(TEXT("animation blueprint"), AnimGraph))
	{
		return false;
	}
	UAnimGraphNode_StateMachine* Machine = Cast<UAnimGraphNode_StateMachine>(Place(UAnimGraphNode_StateMachine::StaticClass(), *AnimGraph, FVector2D(-300.0, 0.0)));
	UAnimationStateMachineGraph* MachineGraph = Machine != nullptr ? Machine->EditorStateMachineGraph.Get() : nullptr;
	if (!TestNotNull(TEXT("state machine"), MachineGraph))
	{
		return false;
	}
	TestTrue(TEXT("state machine into the Output Pose"), Link(FirstPin(Machine, EGPD_Output), FirstPin(RootOf(*AnimGraph), EGPD_Input)));
	TArray<UAnimStateNode*> States;
	for (const TCHAR* Name : {TEXT("Idle"), TEXT("Walk"), TEXT("Run")})
	{
		UAnimStateNode* State = FEdGraphSchemaAction_NewStateNode::SpawnNodeFromTemplate<UAnimStateNode>(MachineGraph,
			NewObject<UAnimStateNode>(), FVector2f(300.0f + 400.0f * States.Num(), 0.0f), false);
		if (!TestNotNull(TEXT("state"), State))
		{
			return false;
		}
		static_cast<UEdGraphNode*>(State)->OnRenameNode(Name);
		States.Add(State);
	}
	const UAnimStateNodeBase* Idle = States[0];
	const UAnimStateNodeBase* Walk = States[1];
	const UAnimStateNodeBase* Run = States[2];
	TestTrue(TEXT("entry to Idle"), MachineGraph->EntryNode != nullptr && Link(MachineGraph->EntryNode->GetOutputPin(), Idle->GetInputPin()));
	TestTrue(TEXT("Idle -> Walk"), Link(Idle->GetOutputPin(), Walk->GetInputPin()));
	TestTrue(TEXT("Walk -> Run"), Link(Walk->GetOutputPin(), Run->GetInputPin()));
	TestTrue(TEXT("Run -> Idle"), Link(Run->GetOutputPin(), Idle->GetInputPin()));
	UAnimStateTransitionNode* IdleWalk = TransitionOf(Target, TEXT("Idle"), TEXT("Walk"));
	UAnimStateTransitionNode* WalkRun = TransitionOf(Target, TEXT("Walk"), TEXT("Run"));
	if (!TestNotNull(TEXT("Idle -> Walk"), IdleWalk) || !TestNotNull(TEXT("Walk -> Run"), WalkRun))
	{
		return false;
	}
	// Idle -> Walk's rule reads bMoving (a variable dragged into the rule graph, linked to Can Enter Transition).
	FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("bMoving"), FEdGraphPinType(UEdGraphSchema_K2::PC_Boolean, NAME_None,
		nullptr, EPinContainerType::None, false, FEdGraphTerminalType()));
	UAnimationTransitionGraph* RuleGraph = Cast<UAnimationTransitionGraph>(IdleWalk->BoundGraph);
	if (!TestNotNull(TEXT("Idle -> Walk rule graph"), RuleGraph) || !TestNotNull(TEXT("rule result"), RuleGraph->MyResultNode.Get()))
	{
		return false;
	}
	UK2Node_VariableGet* Moving = NewObject<UK2Node_VariableGet>(RuleGraph);
	Moving->VariableReference.SetSelfMember(TEXT("bMoving"));
	Moving->CreateNewGuid();
	RuleGraph->AddNode(Moving, false, false);
	Moving->PostPlacedNewNode();
	Moving->AllocateDefaultPins();
	TestTrue(TEXT("bMoving into Can Enter Transition"),
		Link(Moving->FindPin(TEXT("bMoving")), RuleGraph->MyResultNode->FindPin(TEXT("bCanEnterTransition"))));
	// The transition's context menu: Promote To Shared, Use Shared.
	IdleWalk->MakeRulesShareable(TEXT("Moving"));
	WalkRun->UseSharedRules(IdleWalk);
	IdleWalk->MakeCrossfadeShareable(TEXT("Smooth"));
	// A second state machine Rest -> Look, with a rule of its own.
	UAnimGraphNode_StateMachine* Upper = Cast<UAnimGraphNode_StateMachine>(Place(UAnimGraphNode_StateMachine::StaticClass(), *AnimGraph, FVector2D(-300.0, 300.0)));
	UAnimationStateMachineGraph* UpperGraph = Upper != nullptr ? Upper->EditorStateMachineGraph.Get() : nullptr;
	if (!TestNotNull(TEXT("second state machine"), UpperGraph))
	{
		return false;
	}
	TArray<UAnimStateNode*> UpperStates;
	for (const TCHAR* Name : {TEXT("Rest"), TEXT("Look")})
	{
		UAnimStateNode* State = FEdGraphSchemaAction_NewStateNode::SpawnNodeFromTemplate<UAnimStateNode>(UpperGraph,
			NewObject<UAnimStateNode>(), FVector2f(300.0f + 400.0f * UpperStates.Num(), 0.0f), false);
		if (!TestNotNull(TEXT("state"), State))
		{
			return false;
		}
		static_cast<UEdGraphNode*>(State)->OnRenameNode(Name);
		UpperStates.Add(State);
	}
	TestTrue(TEXT("entry to Rest"), UpperGraph->EntryNode != nullptr && Link(UpperGraph->EntryNode->GetOutputPin(), UpperStates[0]->GetInputPin()));
	TestTrue(TEXT("Rest -> Look"), Link(UpperStates[0]->GetOutputPin(), UpperStates[1]->GetInputPin()));
	TestTrue(TEXT("fixture compiles"), Compiles(*Blueprint));
	if (!TestTrue(TEXT("saved"), Target.Save()))
	{
		return false;
	}

	const TSharedPtr<FJsonObject> Extracted = TestTolerantRoundTrip(*this, Target, FExpected(), TEXT("Shared rules"));
	if (!Extracted.IsValid())
	{
		return false;
	}
	const TSharedPtr<FJsonObject> IdleWalkDoc = FindNode(Extracted, TEXT("SM:Transition"), TEXT("Idle"), TEXT("Walk"));
	const TSharedPtr<FJsonObject> WalkRunDoc = FindNode(Extracted, TEXT("SM:Transition"), TEXT("Walk"), TEXT("Run"));
	const TSharedPtr<FJsonObject> RunIdleDoc = FindNode(Extracted, TEXT("SM:Transition"), TEXT("Run"), TEXT("Idle"));
	TestEqual(TEXT("the promoted transition names its shared rule"), StringSetting(IdleWalkDoc, TEXT("SharedRules")), FString(TEXT("Moving")));
	TestEqual(TEXT("the transition using it names it"), StringSetting(WalkRunDoc, TEXT("SharedRules")), FString(TEXT("Moving")));
	TestEqual(TEXT("the shared crossfade is named"), StringSetting(IdleWalkDoc, TEXT("SharedCrossfade")), FString(TEXT("Smooth")));
	TestFalse(TEXT("a transition with its own rule names none"), HasSetting(RunIdleDoc, TEXT("SharedRules")));
	TestTrue(TEXT("the promoted transition writes the shared rule graph"), IdleWalkDoc.IsValid() && IdleWalkDoc->HasField(TEXT("Graph")));
	TestTrue(TEXT("the transition using it writes no graph"), WalkRunDoc.IsValid() && !WalkRunDoc->HasField(TEXT("Graph")));
	if (HasAnyErrors())
	{
		AddInfo(FString::Printf(TEXT("Idle -> Walk: %s"), IdleWalkDoc.IsValid() ? *Serialize(IdleWalkDoc) : TEXT("<none>")));
		AddInfo(FString::Printf(TEXT("Walk -> Run: %s"), WalkRunDoc.IsValid() ? *Serialize(WalkRunDoc) : TEXT("<none>")));
	}

	// Run -> Idle uses the shared rule and crossfade.
	const FAssetDocumentResult Joined = ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		const TSharedPtr<FJsonObject> Transition = FindNode(Document, TEXT("SM:Transition"), TEXT("Run"), TEXT("Idle"));
		Transition->RemoveField(TEXT("Graph"));
		const TSharedPtr<FJsonObject> Properties = PropertiesOf(Transition);
		Properties->SetStringField(TEXT("SharedRules"), TEXT("Moving"));
		Properties->SetStringField(TEXT("SharedCrossfade"), TEXT("Smooth"));
	}, TEXT("Use the shared rule"));
	if (TestTrue(TEXT("using the shared rule applies"), Joined.IsSuccess()))
	{
		const UAnimStateTransitionNode* Owner = TransitionOf(Target, TEXT("Idle"), TEXT("Walk"));
		const UAnimStateTransitionNode* Joiner = TransitionOf(Target, TEXT("Run"), TEXT("Idle"));
		TestTrue(TEXT("Run -> Idle uses the shared rule graph"), Owner != nullptr && Joiner != nullptr && Joiner->bSharedRules
			&& Joiner->SharedRulesName == TEXT("Moving") && Joiner->BoundGraph == Owner->BoundGraph);
		TestTrue(TEXT("Run -> Idle uses the shared crossfade"), Joiner != nullptr && Joiner->bSharedCrossfade
			&& Joiner->SharedCrossfadeName == TEXT("Smooth"));
	}

	// Walk -> Run stops sharing: it gets its own copy of the rule.
	const FAssetDocumentResult Unshared = ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		PropertiesOf(FindNode(Document, TEXT("SM:Transition"), TEXT("Walk"), TEXT("Run")))->SetStringField(TEXT("SharedRules"), TEXT(""));
	}, TEXT("Unshare Walk -> Run"));
	if (TestTrue(TEXT("unsharing applies"), Unshared.IsSuccess()))
	{
		const UAnimStateTransitionNode* Owner = TransitionOf(Target, TEXT("Idle"), TEXT("Walk"));
		const UAnimStateTransitionNode* Left = TransitionOf(Target, TEXT("Walk"), TEXT("Run"));
		TestTrue(TEXT("Walk -> Run has its own rule graph"), Owner != nullptr && Left != nullptr && !Left->bSharedRules
			&& Left->BoundGraph != nullptr && Left->BoundGraph != Owner->BoundGraph);
	}
	// Walk -> Run, with blend settings of its own, uses the shared rule again: only SharedRules names the group, so it
	// keeps its blend settings (the editor's Use Shared Rules would also take the group's shared crossfade).
	const FAssetDocumentResult OwnBlend = ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		PropertiesOf(FindNode(Document, TEXT("SM:Transition"), TEXT("Walk"), TEXT("Run")))->SetNumberField(TEXT("CrossfadeDuration"), 0.75);
	}, TEXT("Walk -> Run blend time"));
	TestTrue(TEXT("Walk -> Run blend time applies"), OwnBlend.IsSuccess());
	const FAssetDocumentResult RulesOnly = ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		const TSharedPtr<FJsonObject> Transition = FindNode(Document, TEXT("SM:Transition"), TEXT("Walk"), TEXT("Run"));
		Transition->RemoveField(TEXT("Graph"));
		PropertiesOf(Transition)->SetStringField(TEXT("SharedRules"), TEXT("Moving"));
	}, TEXT("Walk -> Run uses the shared rule only"));
	if (TestTrue(TEXT("using the shared rule only applies"), RulesOnly.IsSuccess()))
	{
		const UAnimStateTransitionNode* Owner = TransitionOf(Target, TEXT("Idle"), TEXT("Walk"));
		const UAnimStateTransitionNode* Rejoined = TransitionOf(Target, TEXT("Walk"), TEXT("Run"));
		TestTrue(TEXT("Walk -> Run uses the shared rule graph"), Owner != nullptr && Rejoined != nullptr && Rejoined->bSharedRules
			&& Rejoined->BoundGraph == Owner->BoundGraph);
		TestTrue(TEXT("Walk -> Run keeps its own blend settings"), Rejoined != nullptr && !Rejoined->bSharedCrossfade
			&& Rejoined->SharedCrossfadeName.IsEmpty() && FMath::IsNearlyEqual(Rejoined->CrossfadeDuration, 0.75f));
		const TSharedPtr<FJsonObject> Again = ExtractResult(Target).Payload;
		if (TestTrue(TEXT("extract after joining"), Again.IsValid()))
		{
			TestFalse(TEXT("Walk -> Run names no shared crossfade"),
				HasSetting(FindNode(Again, TEXT("SM:Transition"), TEXT("Walk"), TEXT("Run")), TEXT("SharedCrossfade")));
			TestAppliesUnchanged(*this, Target, Again, TEXT("Shared rule only, re-applied"));
		}
	}

	// A shared rule belongs to one state machine: Rest -> Look cannot use "Moving", with or without a Graph of its own.
	for (const bool bKeepGraph : {true, false})
	{
		const FAssetDocumentResult Across = ApplyEdited(*this, Target, [bKeepGraph](const TSharedPtr<FJsonObject>& Document)
		{
			const TSharedPtr<FJsonObject> Transition = FindNode(Document, TEXT("SM:Transition"), TEXT("Rest"), TEXT("Look"));
			if (!bKeepGraph)
			{
				Transition->RemoveField(TEXT("Graph"));
			}
			PropertiesOf(Transition)->SetStringField(TEXT("SharedRules"), TEXT("Moving"));
		}, bKeepGraph ? TEXT("Another state machine holds the rule too") : TEXT("Another state machine uses the rule"));
		TestRefused(*this, Across, TEXT("Graph.SharedRuleAcrossStateMachines"), FString(), bKeepGraph ? TEXT("Rule held twice") : TEXT("Rule used across"));
		const UAnimStateTransitionNode* Kept = TransitionOf(Target, TEXT("Rest"), TEXT("Look"));
		TestTrue(TEXT("Rest -> Look keeps its own rule"), Kept != nullptr && !Kept->bSharedRules);
	}
	TestTolerantRoundTrip(*this, Target, FExpected(), TEXT("Shared rules after edits"));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAnimLinkedGraphPinsTest, "UEAssetDoc.Fidelity.AnimBlueprint.LinkedGraphPins",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAnimLinkedGraphPinsTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentTolerantTestSupport;
	using namespace AssetDocumentAnimNodeDetailsTests;
	const FTarget Linked(TEXT("ABP_LinkedPinsTarget"));
	const FTarget Target(TEXT("ABP_LinkedPins"));
	AddExpectedMessage(TEXT("package was marked as deleted in editor"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1, false);
	ON_SCOPE_EXIT
	{
		Target.Cleanup();
		Linked.Cleanup();
	};

	// The linked Animation Blueprint: Speed is instance editable (its pin shows by default), Weight is not (hidden by
	// default).
	UAnimBlueprint* LinkedBlueprint = NewAnimBlueprint(Linked);
	if (!TestNotNull(TEXT("linked animation blueprint"), LinkedBlueprint))
	{
		return false;
	}
	AddFloatVariable(*LinkedBlueprint, TEXT("Speed"));
	AddFloatVariable(*LinkedBlueprint, TEXT("Weight"));
	FBlueprintEditorUtils::SetBlueprintOnlyEditableFlag(LinkedBlueprint, TEXT("Speed"), false);
	TestTrue(TEXT("linked blueprint compiles"), Compiles(*LinkedBlueprint));
	if (!TestTrue(TEXT("linked blueprint saved"), Linked.Save()))
	{
		return false;
	}

	// A Linked Anim Graph node of it into the Output Pose.
	UAnimBlueprint* Blueprint = NewAnimBlueprint(Target);
	UEdGraph* AnimGraph = Blueprint != nullptr ? AnimGraphOf(*Blueprint) : nullptr;
	if (!TestNotNull(TEXT("animation blueprint"), AnimGraph))
	{
		return false;
	}
	UAnimGraphNode_LinkedAnimGraph* Node = Cast<UAnimGraphNode_LinkedAnimGraph>(Place(UAnimGraphNode_LinkedAnimGraph::StaticClass(), *AnimGraph, FVector2D(-300.0, 0.0)));
	if (!TestNotNull(TEXT("Linked Anim Graph node"), Node))
	{
		return false;
	}
	// Details > Instance Class.
	Node->Node.InstanceClass = LinkedBlueprint->GeneratedClass.Get();
	Node->ReconstructNode();
	TestTrue(TEXT("node into the Output Pose"), Link(FirstPin(Node, EGPD_Output), FirstPin(RootOf(*AnimGraph), EGPD_Input)));
	TestTrue(TEXT("Speed shows by default"), ShowsPin(Node, TEXT("Speed")));
	TestFalse(TEXT("Weight is hidden by default"), ShowsPin(Node, TEXT("Weight")));
	TestTrue(TEXT("fixture compiles"), Compiles(*Blueprint));
	if (!TestTrue(TEXT("saved"), Target.Save()))
	{
		return false;
	}

	const TSharedPtr<FJsonObject> Extracted = TestTolerantRoundTrip(*this, Target, FExpected(), TEXT("Linked graph pins"));
	if (!Extracted.IsValid())
	{
		return false;
	}
	const TSharedPtr<FJsonObject> NodeDoc = FindNode(Extracted, TEXT("Anim:LinkedAnimGraph"));
	TestTrue(TEXT("pins at their default are not written"), NodeDoc.IsValid() && !HasSetting(NodeDoc, TEXT("ShowPin.Speed"))
		&& !HasSetting(NodeDoc, TEXT("ShowPin.Weight")));

	// Expose Weight, hide Speed.
	const FAssetDocumentResult Toggled = ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		const TSharedPtr<FJsonObject> Properties = PropertiesOf(FindNode(Document, TEXT("Anim:LinkedAnimGraph")));
		Properties->SetBoolField(TEXT("ShowPin.Weight"), true);
		Properties->SetBoolField(TEXT("ShowPin.Speed"), false);
	}, TEXT("Expose Weight, hide Speed"));
	if (TestTrue(TEXT("pin toggles apply"), Toggled.IsSuccess()))
	{
		const UAnimGraphNode_LinkedAnimGraph* Live = FirstNodeOf<UAnimGraphNode_LinkedAnimGraph>(Target);
		TestTrue(TEXT("Weight shows"), ShowsPin(Live, TEXT("Weight")));
		TestFalse(TEXT("Speed is hidden"), ShowsPin(Live, TEXT("Speed")));
		const FAssetDocumentResult Reread = ExtractResult(Target);
		const TSharedPtr<FJsonObject> Read = Reread.IsSuccess() ? FindNode(Reread.Payload, TEXT("Anim:LinkedAnimGraph")) : nullptr;
		TestTrue(TEXT("both toggles read back"), HasSetting(Read, TEXT("ShowPin.Weight")) && HasSetting(Read, TEXT("ShowPin.Speed")));
	}

	// Left out, the toggles are kept.
	const FAssetDocumentResult Omitted = ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		const TSharedPtr<FJsonObject> Properties = PropertiesOf(FindNode(Document, TEXT("Anim:LinkedAnimGraph")));
		Properties->RemoveField(TEXT("ShowPin.Weight"));
		Properties->RemoveField(TEXT("ShowPin.Speed"));
	}, TEXT("Toggles left out"));
	if (TestTrue(TEXT("a document without the toggles applies"), Omitted.IsSuccess()))
	{
		const UAnimGraphNode_LinkedAnimGraph* Live = FirstNodeOf<UAnimGraphNode_LinkedAnimGraph>(Target);
		TestTrue(TEXT("Weight still shows"), ShowsPin(Live, TEXT("Weight")));
		TestFalse(TEXT("Speed is still hidden"), ShowsPin(Live, TEXT("Speed")));
	}

	const FAssetDocumentResult Unknown = ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		PropertiesOf(FindNode(Document, TEXT("Anim:LinkedAnimGraph")))->SetBoolField(TEXT("ShowPin.NoSuchVariable"), true);
	}, TEXT("Unknown pin"));
	TestRefused(*this, Unknown, TEXT("Graph.InvalidArgument"), FString(), TEXT("Unknown pin"));
	TestTolerantRoundTrip(*this, Target, FExpected(), TEXT("Linked graph pins after edits"));
	return !HasAnyErrors();
}

#endif
