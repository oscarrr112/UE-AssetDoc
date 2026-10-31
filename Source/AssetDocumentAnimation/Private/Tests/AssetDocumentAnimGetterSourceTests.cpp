// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// An editor-made Animation Blueprint whose state machine leaves the state "Move" (an Idle and a Walk sequence player
// blended) for "Rest" when Time Remaining (ratio) of the Idle player is less than that of the Walk player: two
// getters of one function that differ only in their source. Extract keys each by its source, the extract and the
// older key without the source re-apply as no-ops, a getter added by its key reads the named player, and a key
// without the source that the transition could place for either player is refused as ambiguous. A getter kept by its
// NodeGuid moves to another source the graph has, and is refused when its source is one the graph does not have.

#include "AssetDocumentTolerantTestSupport.h"

#include "Animation/AnimBlueprint.h"
#include "Animation/AnimBlueprintGeneratedClass.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "AnimGraphNode_Root.h"
#include "AnimGraphNode_SequencePlayer.h"
#include "AnimGraphNode_StateMachine.h"
#include "AnimGraphNode_StateResult.h"
#include "AnimGraphNode_TransitionResult.h"
#include "AnimGraphNode_TwoWayBlend.h"
#include "AnimStateEntryNode.h"
#include "AnimStateNode.h"
#include "AnimStateTransitionNode.h"
#include "AnimationGraphSchema.h"
#include "AnimationStateGraph.h"
#include "AnimationStateMachineGraph.h"
#include "AnimationStateMachineSchema.h"
#include "AnimationTransitionGraph.h"
#include "BlueprintActionDatabase.h"
#include "BlueprintFunctionNodeSpawner.h"
#include "BlueprintNodeBinder.h"
#include "BlueprintNodeSpawner.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphSchema.h"
#include "K2Node_AnimGetter.h"
#include "Kismet/KismetMathLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/ScopeExit.h"
#include "UObject/StrongObjectPtr.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace AssetDocumentAnimGetterSourceTests
{
const TCHAR* const GetterSkeleton = TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/TutorialTPP_Skeleton.TutorialTPP_Skeleton");
const TCHAR* const GetterIdle = TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/Tutorial_Idle.Tutorial_Idle");
const TCHAR* const GetterWalk = TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/Tutorial_Walk_Fwd.Tutorial_Walk_Fwd");
const TCHAR* const TimeLeftKey = TEXT("K2:AnimGetter:/Script/Engine.AnimInstance:GetInstanceAssetPlayerTimeFromEndFraction");
const TCHAR* const TimeKey = TEXT("K2:AnimGetter:/Script/Engine.AnimInstance:GetInstanceAssetPlayerTimeFraction");

UEdGraphPin* AnimGetterPin(UEdGraphNode* Node, EEdGraphPinDirection Direction, const TCHAR* Name = nullptr)
{
	if (Node == nullptr)
	{
		return nullptr;
	}
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin != nullptr && Pin->Direction == Direction && !Pin->bHidden && (Name == nullptr || Pin->PinName == FName(Name)))
		{
			return Pin;
		}
	}
	return nullptr;
}

bool AnimGetterLink(UEdGraphPin* From, UEdGraphPin* To)
{
	return From != nullptr && To != nullptr && From->GetOwningNode()->GetGraph()->GetSchema()->TryCreateConnection(From, To);
}

UAnimGraphNode_Base* AnimGetterPlayer(UEdGraph* Graph, UAnimationAsset* Asset, const FVector2D& Location)
{
	const TStrongObjectPtr<UBlueprintNodeSpawner> Spawner(UBlueprintNodeSpawner::Create(UAnimGraphNode_SequencePlayer::StaticClass()));
	UAnimGraphNode_SequencePlayer* Player = Cast<UAnimGraphNode_SequencePlayer>(Spawner->Invoke(Graph, IBlueprintNodeBinder::FBindingSet(), Location));
	if (Player != nullptr)
	{
		Player->SetAnimationAsset(Asset);
	}
	return Player;
}

// The palette action of the getter Function that reads Source (UK2Node_AnimGetter registers one per source).
UBlueprintNodeSpawner* AnimGetterAction(UAnimBlueprint* Blueprint, const UAnimGraphNode_Base* Source, FName Function)
{
	FBlueprintActionDatabase::Get().RefreshAssetActions(Blueprint);
	const FBlueprintActionDatabase::FActionList* Actions = FBlueprintActionDatabase::Get().GetAllActions().Find(FObjectKey(Blueprint));
	if (Actions == nullptr)
	{
		return nullptr;
	}
	// Outered to a graph of the Blueprint (not placed in it), so the getter's function reference finds the Blueprint's
	// class as a placed node does.
	UEdGraph* Graph = !Blueprint->FunctionGraphs.IsEmpty() ? Blueprint->FunctionGraphs[0].Get() : nullptr;
	if (Graph == nullptr)
	{
		return nullptr;
	}
	const TStrongObjectPtr<UK2Node_AnimGetter> Scratch(NewObject<UK2Node_AnimGetter>(Graph, NAME_None, RF_Transient));
	ON_SCOPE_EXIT
	{
		Scratch->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional | REN_DoNotDirty);
	};
	for (UBlueprintNodeSpawner* Spawner : *Actions)
	{
		if (Spawner == nullptr || Spawner->NodeClass != UK2Node_AnimGetter::StaticClass() || !Spawner->CustomizeNodeDelegate.IsBound())
		{
			continue;
		}
		Scratch->SourceNode = nullptr;
		Scratch->SourceStateNode = nullptr;
		Spawner->CustomizeNodeDelegate.Execute(Scratch.Get(), true);
		if (Scratch->SourceNode == Source && Scratch->FunctionReference.GetMemberName() == Function)
		{
			return Spawner;
		}
	}
	return nullptr;
}

// Every node object (one with an "Action") anywhere in the document.
void AnimGetterNodes(const TSharedPtr<FJsonValue>& Value, TArray<TSharedPtr<FJsonObject>>& Out)
{
	if (!Value.IsValid())
	{
		return;
	}
	if (Value->Type == EJson::Array)
	{
		for (const TSharedPtr<FJsonValue>& Item : Value->AsArray())
		{
			AnimGetterNodes(Item, Out);
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
			AnimGetterNodes(Field.Value, Out);
		}
	}
}

// The actions of the document's getters, sorted and joined.
FString AnimGetterActions(const TSharedPtr<FJsonObject>& Document)
{
	TArray<TSharedPtr<FJsonObject>> Nodes;
	AnimGetterNodes(MakeShared<FJsonValueObject>(Document), Nodes);
	TArray<FString> Out;
	for (const TSharedPtr<FJsonObject>& Node : Nodes)
	{
		const FString Action = Node->GetStringField(TEXT("Action"));
		if (Action.StartsWith(TEXT("K2:AnimGetter:")))
		{
			Out.Add(Action);
		}
	}
	Out.Sort();
	return FString::Join(Out, TEXT(", "));
}

// The graph object (with "Nodes") holding a node whose action starts with Prefix, or null.
TSharedPtr<FJsonObject> AnimGetterGraph(const TSharedPtr<FJsonValue>& Value, const FString& Prefix)
{
	if (!Value.IsValid())
	{
		return nullptr;
	}
	if (Value->Type == EJson::Array)
	{
		for (const TSharedPtr<FJsonValue>& Item : Value->AsArray())
		{
			if (TSharedPtr<FJsonObject> Found = AnimGetterGraph(Item, Prefix))
			{
				return Found;
			}
		}
		return nullptr;
	}
	if (Value->Type != EJson::Object)
	{
		return nullptr;
	}
	const TSharedPtr<FJsonObject> Object = Value->AsObject();
	const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
	if (Object->TryGetArrayField(TEXT("Nodes"), Nodes))
	{
		for (const TSharedPtr<FJsonValue>& Node : *Nodes)
		{
			FString Action;
			if (Node->Type == EJson::Object && Node->AsObject()->TryGetStringField(TEXT("Action"), Action) && Action.StartsWith(Prefix))
			{
				return Object;
			}
		}
	}
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Object->Values)
	{
		if (TSharedPtr<FJsonObject> Found = AnimGetterGraph(Field.Value, Prefix))
		{
			return Found;
		}
	}
	return nullptr;
}

void AnimGetterAddNode(const TSharedPtr<FJsonObject>& Document, const TCHAR* Id, const FString& Action)
{
	if (const TSharedPtr<FJsonObject> Graph = AnimGetterGraph(MakeShared<FJsonValueObject>(Document), TimeLeftKey))
	{
		TArray<TSharedPtr<FJsonValue>> Nodes = Graph->GetArrayField(TEXT("Nodes"));
		const TSharedPtr<FJsonObject> Node = MakeShared<FJsonObject>();
		Node->SetStringField(TEXT("Id"), Id);
		Node->SetStringField(TEXT("Action"), Action);
		Nodes.Add(MakeShared<FJsonValueObject>(Node));
		Graph->SetArrayField(TEXT("Nodes"), Nodes);
	}
}

// Sets the Action of the document's getter node whose Action is From.
void AnimGetterSetAction(const TSharedPtr<FJsonObject>& Document, const FString& From, const FString& To)
{
	TArray<TSharedPtr<FJsonObject>> Nodes;
	AnimGetterNodes(MakeShared<FJsonValueObject>(Document), Nodes);
	for (const TSharedPtr<FJsonObject>& Node : Nodes)
	{
		if (Node->GetStringField(TEXT("Action")) == From)
		{
			Node->SetStringField(TEXT("Action"), To);
		}
	}
}

// The asset each getter of Function in the Blueprint reads, sorted by name and joined.
FString AnimGetterSources(const AssetDocumentTolerantTestSupport::FTarget& Target, FName Function)
{
	TArray<FString> Out;
	UAnimBlueprint* Blueprint = Target.Get<UAnimBlueprint>();
	if (Blueprint == nullptr)
	{
		return FString();
	}
	TArray<UK2Node_AnimGetter*> Getters;
	FBlueprintEditorUtils::GetAllNodesOfClass(Blueprint, Getters);
	for (const UK2Node_AnimGetter* Getter : Getters)
	{
		if (Getter->FunctionReference.GetMemberName() == Function)
		{
			const UAnimationAsset* Asset = Getter->SourceNode != nullptr ? Getter->SourceNode->GetAnimationAsset() : nullptr;
			Out.Add(Asset != nullptr ? Asset->GetName() : FString(TEXT("<none>")));
		}
	}
	Out.Sort();
	return FString::Join(Out, TEXT(", "));
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAnimGetterSourceTest, "UEAssetDoc.Codec.AnimBlueprint.AnimGetterSource",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetDocumentAnimGetterSourceTest::RunTest(const FString& Parameters)
{
	using namespace AssetDocumentTolerantTestSupport;
	using namespace AssetDocumentAnimGetterSourceTests;
	const FTarget Target(TEXT("ABP_GetterSource"));
	AddExpectedMessage(TEXT("package was marked as deleted in editor"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1, false);
	ON_SCOPE_EXIT { Target.Cleanup(); };

	USkeleton* Skeleton = LoadObject<USkeleton>(nullptr, GetterSkeleton);
	UAnimSequence* Idle = LoadObject<UAnimSequence>(nullptr, GetterIdle);
	UAnimSequence* Walk = LoadObject<UAnimSequence>(nullptr, GetterWalk);
	if (!TestNotNull(TEXT("skeleton"), Skeleton) || !TestNotNull(TEXT("idle"), Idle) || !TestNotNull(TEXT("walk"), Walk))
	{
		return false;
	}
	UAnimBlueprint* Blueprint = Cast<UAnimBlueprint>(FKismetEditorUtilities::CreateBlueprint(UAnimInstance::StaticClass(),
		CreatePackage(*Target.PackageName), FName(*Target.AssetName), BPTYPE_Normal, UAnimBlueprint::StaticClass(),
		UAnimBlueprintGeneratedClass::StaticClass()));
	if (!TestNotNull(TEXT("animation blueprint created"), Blueprint))
	{
		return false;
	}
	Blueprint->TargetSkeleton = Skeleton;
	FAssetRegistryModule::AssetCreated(Blueprint);
	const TObjectPtr<UEdGraph>* AnimGraph = Blueprint->FunctionGraphs.FindByPredicate([](const UEdGraph* Graph)
	{
		return Graph != nullptr && Graph->Schema != nullptr && Graph->Schema->IsChildOf(UAnimationGraphSchema::StaticClass());
	});
	if (!TestNotNull(TEXT("AnimGraph"), AnimGraph))
	{
		return false;
	}
	UEdGraphNode* Root = nullptr;
	for (UEdGraphNode* Node : (*AnimGraph)->Nodes)
	{
		Root = Root != nullptr ? Root : Cast<UAnimGraphNode_Root>(Node);
	}

	// A state machine into the Output Pose: Entry -> Move -> Rest.
	const TStrongObjectPtr<UBlueprintNodeSpawner> MachineSpawner(UBlueprintNodeSpawner::Create(UAnimGraphNode_StateMachine::StaticClass()));
	UAnimGraphNode_StateMachine* Machine = Cast<UAnimGraphNode_StateMachine>(
		MachineSpawner->Invoke(AnimGraph->Get(), IBlueprintNodeBinder::FBindingSet(), FVector2D(-300.0, 0.0)));
	UAnimationStateMachineGraph* MachineGraph = Machine != nullptr ? Machine->EditorStateMachineGraph.Get() : nullptr;
	if (!TestNotNull(TEXT("state machine placed"), MachineGraph) || !TestNotNull(TEXT("Output Pose"), Root))
	{
		return false;
	}
	TestTrue(TEXT("state machine linked to the Output Pose"),
		AnimGetterLink(AnimGetterPin(Machine, EGPD_Output), AnimGetterPin(Root, EGPD_Input)));
	UAnimStateNode* Move = FEdGraphSchemaAction_NewStateNode::SpawnNodeFromTemplate<UAnimStateNode>(MachineGraph,
		NewObject<UAnimStateNode>(), FVector2f(300.0f, 0.0f), false);
	UAnimStateNode* Rest = FEdGraphSchemaAction_NewStateNode::SpawnNodeFromTemplate<UAnimStateNode>(MachineGraph,
		NewObject<UAnimStateNode>(), FVector2f(700.0f, 0.0f), false);
	if (!TestNotNull(TEXT("state Move"), Move) || !TestNotNull(TEXT("state Rest"), Rest))
	{
		return false;
	}
	static_cast<UEdGraphNode*>(Move)->OnRenameNode(TEXT("Move"));
	static_cast<UEdGraphNode*>(Rest)->OnRenameNode(TEXT("Rest"));
	const UAnimStateNodeBase* MoveState = Move;
	const UAnimStateNodeBase* RestState = Rest;
	TestTrue(TEXT("entry linked to Move"), MachineGraph->EntryNode != nullptr
		&& AnimGetterLink(MachineGraph->EntryNode->GetOutputPin(), MoveState->GetInputPin()));
	TestTrue(TEXT("transition Move -> Rest"), AnimGetterLink(MoveState->GetOutputPin(), RestState->GetInputPin()));
	UAnimStateTransitionNode* Transition = nullptr;
	for (UEdGraphNode* Node : MachineGraph->Nodes)
	{
		UAnimStateTransitionNode* Candidate = Cast<UAnimStateTransitionNode>(Node);
		Transition = Transition == nullptr && Candidate != nullptr && Candidate->GetPreviousState() == Move ? Candidate : Transition;
	}
	UAnimationStateGraph* MoveGraph = Cast<UAnimationStateGraph>(Move->BoundGraph);
	UAnimationTransitionGraph* RuleGraph = Transition != nullptr ? Cast<UAnimationTransitionGraph>(Transition->BoundGraph) : nullptr;
	if (!TestNotNull(TEXT("Move's graph"), MoveGraph) || !TestNotNull(TEXT("transition rule graph"), RuleGraph))
	{
		return false;
	}

	// Move: Idle and Walk sequence players blended into the state's result.
	UAnimGraphNode_Base* IdlePlayer = AnimGetterPlayer(MoveGraph, Idle, FVector2D(-600.0, 0.0));
	UAnimGraphNode_Base* WalkPlayer = AnimGetterPlayer(MoveGraph, Walk, FVector2D(-600.0, 300.0));
	const TStrongObjectPtr<UBlueprintNodeSpawner> BlendSpawner(UBlueprintNodeSpawner::Create(UAnimGraphNode_TwoWayBlend::StaticClass()));
	UEdGraphNode* Blend = BlendSpawner->Invoke(MoveGraph, IBlueprintNodeBinder::FBindingSet(), FVector2D(-300.0, 0.0));
	if (!TestNotNull(TEXT("Idle player"), IdlePlayer) || !TestNotNull(TEXT("Walk player"), WalkPlayer) || !TestNotNull(TEXT("blend"), Blend))
	{
		return false;
	}
	TestTrue(TEXT("Idle into the blend"), AnimGetterLink(AnimGetterPin(IdlePlayer, EGPD_Output), AnimGetterPin(Blend, EGPD_Input, TEXT("A"))));
	TestTrue(TEXT("Walk into the blend"), AnimGetterLink(AnimGetterPin(WalkPlayer, EGPD_Output), AnimGetterPin(Blend, EGPD_Input, TEXT("B"))));
	TestTrue(TEXT("blend into Move's result"),
		AnimGetterLink(AnimGetterPin(Blend, EGPD_Output), AnimGetterPin(MoveGraph->MyResultNode, EGPD_Input)));

	// The rule: Time Remaining (ratio) (Tutorial_Idle) < Time Remaining (ratio) (Tutorial_Walk_Fwd), each getter placed
	// from the palette action of its player.
	const FName TimeLeft = GET_FUNCTION_NAME_CHECKED(UAnimInstance, GetInstanceAssetPlayerTimeFromEndFraction);
	UBlueprintNodeSpawner* IdleAction = AnimGetterAction(Blueprint, IdlePlayer, TimeLeft);
	UBlueprintNodeSpawner* WalkAction = AnimGetterAction(Blueprint, WalkPlayer, TimeLeft);
	if (!TestNotNull(TEXT("Time Remaining (ratio) of the Idle player"), IdleAction)
		|| !TestNotNull(TEXT("Time Remaining (ratio) of the Walk player"), WalkAction))
	{
		return false;
	}
	UEdGraphNode* IdleLeft = IdleAction->Invoke(RuleGraph, IBlueprintNodeBinder::FBindingSet(), FVector2D(-500.0, 0.0));
	UEdGraphNode* WalkLeft = WalkAction->Invoke(RuleGraph, IBlueprintNodeBinder::FBindingSet(), FVector2D(-500.0, 200.0));
	const UFunction* LessFunction = UKismetMathLibrary::StaticClass()->FindFunctionByName(GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, Less_DoubleDouble));
	const TStrongObjectPtr<UBlueprintFunctionNodeSpawner> LessSpawner(UBlueprintFunctionNodeSpawner::Create(LessFunction));
	UEdGraphNode* Less = LessSpawner->Invoke(RuleGraph, IBlueprintNodeBinder::FBindingSet(), FVector2D(-200.0, 0.0));
	if (!TestNotNull(TEXT("Idle getter placed"), IdleLeft) || !TestNotNull(TEXT("Walk getter placed"), WalkLeft) || !TestNotNull(TEXT("Less"), Less))
	{
		return false;
	}
	TestTrue(TEXT("Idle getter into A"), AnimGetterLink(AnimGetterPin(IdleLeft, EGPD_Output), AnimGetterPin(Less, EGPD_Input, TEXT("A"))));
	TestTrue(TEXT("Walk getter into B"), AnimGetterLink(AnimGetterPin(WalkLeft, EGPD_Output), AnimGetterPin(Less, EGPD_Input, TEXT("B"))));
	TestTrue(TEXT("Less into the rule's result"),
		AnimGetterLink(AnimGetterPin(Less, EGPD_Output), AnimGetterPin(RuleGraph->MyResultNode, EGPD_Input)));
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	TestTrue(TEXT("fixture compiles"), Blueprint->Status == BS_UpToDate || Blueprint->Status == BS_UpToDateWithWarnings);
	if (!TestTrue(TEXT("saved"), Target.Save()))
	{
		return false;
	}

	// Extract keys each getter by its player's asset; the extract re-applies as a no-op.
	const TSharedPtr<FJsonObject> Extracted = TestTolerantRoundTrip(*this, Target, FExpected(), TEXT("Getters of two players"));
	if (!Extracted.IsValid())
	{
		return false;
	}
	const FString IdleKey = FString(TimeLeftKey) + TEXT("@Tutorial_Idle");
	const FString WalkKey = FString(TimeLeftKey) + TEXT("@Tutorial_Walk_Fwd");
	TestEqual(TEXT("each getter is keyed by its source"), AnimGetterActions(Extracted), IdleKey + TEXT(", ") + WalkKey);

	// The older key without the source still names each getter kept by its NodeGuid: no change.
	{
		const TSharedPtr<FJsonObject> Older = Parse(Serialize(Extracted));
		Older->SetStringField(TEXT("Action"), TEXT("Update"));
		TArray<TSharedPtr<FJsonObject>> Nodes;
		AnimGetterNodes(MakeShared<FJsonValueObject>(Older), Nodes);
		for (const TSharedPtr<FJsonObject>& Node : Nodes)
		{
			if (Node->GetStringField(TEXT("Action")).StartsWith(TimeLeftKey))
			{
				Node->SetStringField(TEXT("Action"), TimeLeftKey);
			}
		}
		const FDateTime Stamp = IFileManager::Get().GetTimeStamp(*Target.PackageFile);
		const FAssetDocumentResult Applied = ApplyText(*this, Target, Serialize(Older), TEXT("Key without the source"));
		TestTrue(TEXT("without the source: applies"), Applied.IsSuccess());
		TestFalse(TEXT("without the source: saves nothing"), Applied.bSavedAsset);
		TestEqual(TEXT("without the source: package file untouched"), IFileManager::Get().GetTimeStamp(*Target.PackageFile), Stamp);
	}

	// A getter added by its key reads the player the key names; the two getters keep theirs.
	const FAssetDocumentResult Added = ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		AnimGetterAddNode(Document, TEXT("WalkTime"), FString(TimeKey) + TEXT("@Tutorial_Walk_Fwd"));
	}, TEXT("Add a getter of the Walk player"));
	if (TestTrue(TEXT("the getter applies"), Added.IsSuccess()))
	{
		const FName TimeFraction = GET_FUNCTION_NAME_CHECKED(UAnimInstance, GetInstanceAssetPlayerTimeFraction);
		TestEqual(TEXT("the new getter reads the Walk player"), AnimGetterSources(Target, TimeFraction), FString(TEXT("Tutorial_Walk_Fwd")));
		TestEqual(TEXT("the rule's getters keep their players"), AnimGetterSources(Target, TimeLeft),
			FString(TEXT("Tutorial_Idle, Tutorial_Walk_Fwd")));
		const FAssetDocumentResult Reread = ExtractResult(Target);
		TestEqual(TEXT("the getters read back by source"), Reread.IsSuccess() ? AnimGetterActions(Reread.Payload) : FString(),
			FString(TimeKey) + TEXT("@Tutorial_Walk_Fwd, ") + IdleKey + TEXT(", ") + WalkKey);
	}

	// A new getter without the source, which the transition offers for both players, names neither.
	const FAssetDocumentResult Ambiguous = ApplyEdited(*this, Target, [](const TSharedPtr<FJsonObject>& Document)
	{
		AnimGetterAddNode(Document, TEXT("EitherTime"), TimeKey);
	}, TEXT("Add a getter without the source"));
	TestRefused(*this, Ambiguous, TEXT("Graph.ActionAmbiguous"), FString(), TEXT("Getter without the source"));

	// The Idle getter (kept by its NodeGuid) written with a source the graph does not have, as a document written
	// before the source was replaced reads: refused, naming both sources; the getters keep their players.
	const FAssetDocumentResult Stale = ApplyEdited(*this, Target, [&IdleKey](const TSharedPtr<FJsonObject>& Document)
	{
		AnimGetterSetAction(Document, IdleKey, FString(TimeLeftKey) + TEXT("@Tutorial_Run_Fwd"));
	}, TEXT("Getter of a source the graph does not have"));
	TestRefused(*this, Stale, TEXT("Graph.GetterSourceMissing"), FString(), TEXT("Stale getter source"));
	const FAssetDocumentDiagnostic* StaleDiagnostic = Stale.Diagnostics.FindByPredicate([](const FAssetDocumentDiagnostic& Diagnostic)
	{
		return Diagnostic.Code == TEXT("Graph.GetterSourceMissing");
	});
	TestTrue(TEXT("the refusal names the stale and the current source"), StaleDiagnostic != nullptr
		&& StaleDiagnostic->Message.Contains(TEXT("'Tutorial_Run_Fwd'")) && StaleDiagnostic->Message.Contains(TEXT("'Tutorial_Idle'")));
	TestEqual(TEXT("after the refusal the getters keep their players"), AnimGetterSources(Target, TimeLeft),
		FString(TEXT("Tutorial_Idle, Tutorial_Walk_Fwd")));

	// The same getter moved to another source the graph has: the getter of that source replaces it.
	const FAssetDocumentResult Switched = ApplyEdited(*this, Target, [&IdleKey, &WalkKey](const TSharedPtr<FJsonObject>& Document)
	{
		AnimGetterSetAction(Document, IdleKey, WalkKey);
	}, TEXT("Getter moved to the Walk player"));
	if (TestTrue(TEXT("the source switch applies"), Switched.IsSuccess()))
	{
		TestEqual(TEXT("both rule getters read the Walk player"), AnimGetterSources(Target, TimeLeft),
			FString(TEXT("Tutorial_Walk_Fwd, Tutorial_Walk_Fwd")));
	}
	return !HasAnyErrors();
}

#endif
