// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
#if WITH_DEV_AUTOMATION_TESTS
#include "AssetDocumentAIPrimitiveProvider.h"
#include "AssetDocumentAIBehaviorTreeProviderTestSupport.h"
#include "AssetDocumentEditorPrimitiveProvider.h"
#include "AssetDocumentRollbackAction.h"
#include "BehaviorTree/AssetDocumentAIBehaviorTreeDerivedCompletion.h"
#include "BehaviorTree/AssetDocumentAIBehaviorTreeSemanticRuntimeMirrorRollback.h"
#include "BehaviorTree/AssetDocumentAIBehaviorTreeSemanticRuntimeMirrorRollbackPrivate.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BehaviorTreeComponent.h"
#include "BehaviorTree/BlackboardComponent.h"
#include "Editor.h"
#include "Engine/World.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Bool.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Int.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_String.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Enum.h"
#include "BehaviorTree/Decorators/BTDecorator_Blackboard.h"
#include "BehaviorTree/Composites/BTComposite_Sequence.h"
#include "BehaviorTree/Tasks/BTTask_Wait.h"
#include "BehaviorTreeGraph.h"
#include "BehaviorTreeGraphNode_Root.h"
#include "BehaviorTreeGraphNode_Composite.h"
#include "BehaviorTreeGraphNode_Task.h"
#include "BehaviorTreeGraphNode_Decorator.h"
#include "BehaviorTreeGraphNode_CompositeDecorator.h"
#include "BehaviorTreeDecoratorGraph.h"
#include "BehaviorTreeDecoratorGraphNode_Decorator.h"
#include "BehaviorTreeDecoratorGraphNode_Logic.h"
#include "EdGraphSchema_BehaviorTree.h"
#include "AIGraphSchema.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Runtime/AssetDocumentAtomicTransaction.h"
#include "Runtime/AssetDocumentHandleRegistry.h"
#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"
namespace AssetDocumentAIBehaviorTreeDecoratorRefreshTests
{
using namespace AssetDocumentAIBehaviorTreeProviderTestSupport;

// Reflection below configures test fixtures and observes derived discriminators;
// it is never used to implement production refresh or rollback cache restoration.
template <typename T> T& Field(UObject& Object, const TCHAR* Name)
{
	FProperty* Property = Object.GetClass()->FindPropertyByName(Name);
	check(Property);
	return *Property->ContainerPtrToValuePtr<T>(&Object);
}
UEdGraphPin* Pin(UEdGraphNode& Node, EEdGraphPinDirection Direction)
{
	for (UEdGraphPin* Item : Node.Pins) if (Item && Item->Direction == Direction) return Item;
	return nullptr;
}
struct FFixture
{
	TStrongObjectPtr<UPackage> Package;
	TStrongObjectPtr<UBehaviorTree> Tree;
	UBehaviorTreeGraph* Graph = nullptr;
	UBlackboardData* Blackboard = nullptr;
	TArray<UBTDecorator_Blackboard*> Decorators;
	TArray<UEdGraphNode*> Wrappers;
	TArray<UBehaviorTreeGraphNode_CompositeDecorator*> Composites;
	~FFixture()
	{
		if (Tree.IsValid()) Tree->ClearFlags(RF_Standalone);
		if (Blackboard) Blackboard->ClearFlags(RF_Standalone);
		if (Package.IsValid()) Package->SetDirtyFlag(false);
	}
	bool Create(FAutomationTestBase& Test, FString PackagePath = {})
	{
		if (PackagePath.IsEmpty()) PackagePath = TEXT("/Temp/AssetDocumentAI/DecoratorRefresh_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
		Package.Reset(CreatePackage(*PackagePath));
		Tree.Reset(NewObject<UBehaviorTree>(Package.Get(), TEXT("Tree"), RF_Public | RF_Standalone));
		Blackboard = NewObject<UBlackboardData>(Package.Get(), TEXT("Blackboard"), RF_Public | RF_Standalone);
		Blackboard->UpdatePersistentKey<UBlackboardKeyType_Bool>(TEXT("Basic"));
		Blackboard->UpdatePersistentKey<UBlackboardKeyType_Int>(TEXT("Arithmetic"));
		Blackboard->UpdatePersistentKey<UBlackboardKeyType_String>(TEXT("Text"));
		UBlackboardKeyType_Enum* EnumKey = Blackboard->UpdatePersistentKey<UBlackboardKeyType_Enum>(TEXT("Enum"));
		EnumKey->EnumType = StaticEnum<EBTBlackboardRestart::Type>();
		Tree->BlackboardAsset = Blackboard;
		Graph = Cast<UBehaviorTreeGraph>(FBlueprintEditorUtils::CreateNewGraph(Tree.Get(), TEXT("BehaviorTreeGraph"), UBehaviorTreeGraph::StaticClass(), UEdGraphSchema_BehaviorTree::StaticClass()));
		Tree->BTGraph = Graph;
		Graph->GetSchema()->CreateDefaultNodesForGraph(*Graph);
		Graph->OnCreated(); Graph->Initialize();
		UBehaviorTreeGraphNode_Root* Root = Cast<UBehaviorTreeGraphNode_Root>(Graph->Nodes[0]);
		if (!Test.TestNotNull(TEXT("stock root exists"), Root)) return false;
		Root->BlackboardAsset = Blackboard; Root->UpdateBlackboard();
		FAISchemaAction_NewNode CompositeAction;
		CompositeAction.NodeTemplate = NewObject<UBehaviorTreeGraphNode_Composite>();
		CompositeAction.NodeTemplate->NodeInstance = NewObject<UBTComposite_Sequence>(CompositeAction.NodeTemplate);
		UEdGraphNode* Composite = CompositeAction.PerformAction(Graph, Pin(*Root, EGPD_Output), FVector2f(0, 100), false);
		if (!Composite) return false;
		FAISchemaAction_NewNode TaskAction;
		TaskAction.NodeTemplate = NewObject<UBehaviorTreeGraphNode_Task>();
		TaskAction.NodeTemplate->NodeInstance = NewObject<UBTTask_Wait>(TaskAction.NodeTemplate);
		UBehaviorTreeGraphNode_Task* Task = Cast<UBehaviorTreeGraphNode_Task>(TaskAction.PerformAction(Graph, Pin(*Composite, EGPD_Output), FVector2f(0, 200), false));
		if (!Task) return false;
		Graph->LockUpdates();
		for (const bool Bound : {false, true})
		{
			for (const TCHAR* Key : {TEXT("Basic"), TEXT("Arithmetic"), TEXT("Text"), TEXT("Enum")})
			{
				UBTDecorator_Blackboard* Decorator = nullptr;
				UEdGraphNode* Wrapper = nullptr;
				FAISchemaAction_NewSubNode Action; Action.ParentNode = Task;
				if (!Bound)
				{
					Action.NodeTemplate = NewObject<UBehaviorTreeGraphNode_Decorator>();
					Action.NodeTemplate->NodeInstance = NewObject<UBTDecorator_Blackboard>(Action.NodeTemplate);
					Action.PerformAction(Graph, nullptr, FVector2f::ZeroVector, false);
					auto* Added = Cast<UBehaviorTreeGraphNode_Decorator>(Task->Decorators.Last());
					Wrapper = Added; Decorator = Cast<UBTDecorator_Blackboard>(Added->NodeInstance);
				}
				else
				{
					Action.NodeTemplate = NewObject<UBehaviorTreeGraphNode_CompositeDecorator>();
					Action.PerformAction(Graph, nullptr, FVector2f::ZeroVector, false);
					auto* Added = Cast<UBehaviorTreeGraphNode_CompositeDecorator>(Task->Decorators.Last());
					Composites.Add(Added); Added->bShowOperations = true;
					UBehaviorTreeDecoratorGraph* BoundGraph = CastChecked<UBehaviorTreeDecoratorGraph>(Added->BoundGraph);
					FGraphNodeCreator<UBehaviorTreeDecoratorGraphNode_Decorator> Creator(*BoundGraph);
					auto* Leaf = Creator.CreateNode(false);
					Leaf->ClassData = FGraphNodeClassData(UBTDecorator_Blackboard::StaticClass(), FString()); Creator.Finalize();
					Wrapper = Leaf; Decorator = Cast<UBTDecorator_Blackboard>(Leaf->NodeInstance);
					UEdGraphNode* Sink = BoundGraph->Nodes[0];
					if (!BoundGraph->GetSchema()->TryCreateConnection(Pin(*Leaf, EGPD_Output), Pin(*Sink, EGPD_Input))) return false;
				}
				if (!Decorator) return false;
				auto& Selector = Field<FBlackboardKeySelector>(*Decorator, TEXT("BlackboardKey"));
				Selector.SelectedKeyName = Key; Selector.ResolveSelectedKey(*Blackboard);
				Field<uint8>(*Decorator, TEXT("BasicOperation")) = EBasicKeyOperation::NotSet;
				Field<uint8>(*Decorator, TEXT("ArithmeticOperation")) = FCString::Strcmp(Key, TEXT("Enum")) == 0 ? uint8(EArithmeticKeyOperation::NotEqual) : uint8(EArithmeticKeyOperation::Greater);
				Field<uint8>(*Decorator, TEXT("FlowAbortMode")) = Bound && (FCString::Strcmp(Key, TEXT("Basic")) == 0 || FCString::Strcmp(Key, TEXT("Text")) == 0) ? uint8(EBTFlowAbortMode::Self) : uint8(EBTFlowAbortMode::None);
				Field<uint8>(*Decorator, TEXT("TextOperation")) = ETextKeyOperation::Contain;
				Field<int32>(*Decorator, TEXT("IntValue")) = 42;
				Field<FString>(*Decorator, TEXT("StringValue")) = FCString::Strcmp(Key, TEXT("Enum")) == 0 ? TEXT("ResultChange") : TEXT("needle");
				Decorators.Add(Decorator); Wrappers.Add(Wrapper);
			}
		}
		Graph->UnlockUpdates();
		// Deliberately stale Basic-to-Arithmetic/Text and enum state after the
		// stock fixture build. Successful refresh must discriminate every row.
		for (UBTDecorator_Blackboard* D : Decorators)
		{
			Field<uint8>(*D, TEXT("OperationType")) = 0;
			Field<FString>(*D, TEXT("CachedDescription")) = TEXT("STALE_BASIC");
			if (Field<FBlackboardKeySelector>(*D, TEXT("BlackboardKey")).SelectedKeyName == TEXT("Enum"))
				Field<int32>(*D, TEXT("IntValue")) = 77;
		}
		for (int32 Index = 0; Index < Composites.Num(); ++Index)
		{
			Field<FString>(*Composites[Index], TEXT("CachedDescription")) = TEXT("STALE_COMPOSITE");
			Composites[Index]->bCanAbortFlow = Index % 2 != 0;
		}
		Package->SetDirtyFlag(false);
		return true;
	}
};
FAssetDocumentRuntimeValueDto String(FString Value)
{
	FAssetDocumentRuntimeValueDto Result; Result.Kind = EAssetDocumentRuntimeValueKind::String; Result.StringValue = MoveTemp(Value); return Result;
}
FAssetDocumentMutationFootprint Footprint(const UBehaviorTree& Tree)
{
	return {EAssetDocumentMutationFootprintKind::UObjectState, TEXT("asset:") + Tree.GetPathName() + TEXT("/DerivedCompletion"), TEXT("assetdocument.ai.behavior_tree.derived_completion.v1")};
}
struct FScenario
{
	FFixture Fixture;
	FResolveExistingHandleResolver Resolver;
	TArray<FAssetDocumentRuntimeDiagnostic> KernelDiagnostics;
	TUniquePtr<FAssetDocumentRollbackActionComposition> Composition;
	TUniquePtr<IAssetDocumentAtomicTransaction> Transaction;
	FAssetDocumentOpaqueHandle Handle;
	TArray<FString> RollbackOrder;
	bool SetUp(FAutomationTestBase& Test, FString PackagePath = {}, TFunction<void()> Restore = {}, bool BothModes = false)
	{
		if (!Fixture.Create(Test, PackagePath)) return false;
		TArray<TSharedRef<IAssetDocumentRollbackAction>> Actions;
		Actions.Add(MakeShared<FOrderedAuthoredAction>(TEXT("test.decorator.authored"), RollbackOrder, TEXT("authored"), MoveTemp(Restore)));
		Composition = MakeUnique<FAssetDocumentRollbackActionComposition>(Resolver, MoveTemp(Actions), [this](EAssetDocumentRuntimeRequestPhase, const FAssetDocumentRuntimeDiagnostic& D) { KernelDiagnostics.Add(D); });
		FAssetDocumentPreflightReceipt Receipt; Receipt.RequestId = FGuid::NewGuid(); Receipt.RuntimeRevision = 1;
		Receipt.ReceiptHash = TEXT("sha256:decorator-provider-local-exact"); Receipt.bSucceeded = true;
		Receipt.CompleteFootprint = {{EAssetDocumentMutationFootprintKind::UObjectState, TEXT("asset:") + Fixture.Tree->GetPathName() + TEXT("/TestAuthored"), TEXT("test.decorator.authored")}, Footprint(*Fixture.Tree)};
		if (BothModes) Receipt.CompleteFootprint.Add({EAssetDocumentMutationFootprintKind::UObjectState, TEXT("asset:") + Fixture.Tree->GetPathName() + TEXT("/RuntimeMirror"), TEXT("assetdocument.ai.behavior_tree.runtime_mirror.v1")});
		static FNoCancellation Cancellation;
		Transaction = Composition->BeginLiveAfterPreflight(Receipt, Cancellation);
		if (!Test.TestNotNull(TEXT("real Atomic transaction exists"), Transaction.Get())) return false;
		if (!Test.TestTrue(TEXT("authored action enlists exact receipt row"), Transaction->EnlistFootprint(MakeArrayView(Receipt.CompleteFootprint.GetData(), 1)).bValid)) return false;
		Resolver.SetMutationToken(Transaction->GetMutationToken());
		auto Lease = ResolveExistingMutableAssetLease(Test, *Fixture.Tree, *Transaction, Resolver);
		if (!Lease) return false;
		auto Alias = Resolver.RegisterProviderScopedAlias(Lease->GetHandle(), TEXTVIEW("ai"), EAssetDocumentHandleAccess::Mutable);
		if (!Alias.IsSet()) return false; Handle = Alias.GetValue(); return true;
	}
	bool Reissue(FAutomationTestBase& Test)
	{
		auto Lease = ResolveExistingMutableAssetLease(Test, *Fixture.Tree, *Transaction, Resolver);
		if (!Lease) return false;
		auto Alias = Resolver.RegisterProviderScopedAlias(Lease->GetHandle(), TEXTVIEW("ai"), EAssetDocumentHandleAccess::Mutable);
		if (!Alias.IsSet()) return false;
		Handle = Alias.GetValue(); return true;
	}
	FAssetDocumentPrimitiveInvocation Invocation(const FString& Digest)
	{
		FAssetDocumentPrimitiveInvocation I; I.PrimitiveId = TEXT("ai.behavior_tree.decorator.refresh.v1"); I.Version = 1;
		const auto& Token = Transaction->GetMutationToken(); I.RequestId = Token.RequestId; I.TransactionId = Token.TransactionId; I.RuntimeRevision = Token.RuntimeRevision;
		I.Phase = EAssetDocumentRuntimeRequestPhase::Repair; I.HandleArguments = {{TEXT("asset"), Handle}};
		I.ValueArguments.Kind = EAssetDocumentRuntimeValueKind::Object;
		I.ValueArguments.ObjectKeys = {TEXT("behavior_tree_path"), TEXT("expected_authored_decorator_digest"), TEXT("expected_blackboard_path")};
		I.ValueArguments.Elements = {String(Fixture.Tree->GetPathName()), String(Digest), String(Fixture.Blackboard->GetPathName())};
		return I;
	}
	FAssetDocumentPrimitiveOutcome Invoke(IAssetDocumentAtomicTransaction* Via = nullptr, FString Digest = {})
	{
		if (Digest.IsEmpty()) Digest = CaptureAssetDocumentBehaviorTreeDecoratorEvidence(*Fixture.Tree, *Fixture.Graph).AuthoredDecoratorDigest;
		return CreateAssetDocumentAIPrimitiveProvider()->Invoke(Invocation(Digest), Resolver, Via ? Via : Transaction.Get());
	}
};
bool Succeeded(FAutomationTestBase& Test, const FAssetDocumentPrimitiveOutcome& Outcome)
{
	for (const auto& D : Outcome.Diagnostics) Test.AddInfo(D.Code + TEXT(": ") + D.Message);
	return Test.TestEqual(TEXT("real decorator provider succeeds"), Outcome.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
}
bool CheckDerived(FAutomationTestBase& Test, const FFixture& F)
{
	bool Passed = true;
	for (int32 Index = 0; Index < F.Decorators.Num(); ++Index)
	{
		UBTDecorator_Blackboard& D = *F.Decorators[Index];
		const FName Key = Field<FBlackboardKeySelector>(D, TEXT("BlackboardKey")).SelectedKeyName;
		const uint8 Expected = Key == TEXT("Basic") ? uint8(EBasicKeyOperation::NotSet) : Key == TEXT("Text") ? uint8(ETextKeyOperation::Contain) : Key == TEXT("Enum") ? uint8(EArithmeticKeyOperation::NotEqual) : uint8(EArithmeticKeyOperation::Greater);
		Passed &= Test.TestEqual(TEXT("active operation projects into runtime discriminator"), Field<uint8>(D, TEXT("OperationType")), Expected);
		Passed &= Test.TestFalse(TEXT("native description is recomputed"), D.GetStaticDescription().Contains(TEXT("STALE_BASIC")));
		if (Key == TEXT("Enum")) Passed &= Test.TestEqual(TEXT("authored enum StringValue replaces stale runtime IntValue=77"), Field<int32>(D, TEXT("IntValue")), 1);
		if (Key == TEXT("Text")) Passed &= Test.TestTrue(TEXT("text operand reaches description"), D.GetStaticDescription().Contains(TEXT("needle")));
		if (Key == TEXT("Arithmetic")) Passed &= Test.TestTrue(TEXT("arithmetic operand reaches description"), D.GetStaticDescription().Contains(TEXT("42")));
	}
	return Passed;
}
// Use the same public Editor-world component setup as the existing Blackboard
// consumer tests. No actor, running tree, node instance seam or raw key memory.
bool CheckNativePredicatesAndComposites(FAutomationTestBase& Test, const FFixture& F)
{
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!Test.TestNotNull(TEXT("native predicate witness has an Editor world"), World)) return false;
	TStrongObjectPtr<UBlackboardComponent> Blackboard(NewObject<UBlackboardComponent>(World));
	TStrongObjectPtr<UBehaviorTreeComponent> Behavior(NewObject<UBehaviorTreeComponent>(World));
	if (!Test.TestTrue(TEXT("real predicate Blackboard initializes through public API"), Blackboard->InitializeBlackboard(*F.Blackboard))) return false;
	Behavior->CacheBlackboardComponent(Blackboard.Get());
	bool Passed = true;
	for (UBTDecorator_Blackboard* Decorator : F.Decorators)
	{
		const FName Key = Field<FBlackboardKeySelector>(*Decorator, TEXT("BlackboardKey")).SelectedKeyName;
		for (bool Expected : {true, false})
		{
			if (Key == TEXT("Basic")) Blackboard->SetValueAsBool(Key, !Expected);
			else if (Key == TEXT("Arithmetic")) Blackboard->SetValueAsInt(Key, Expected ? 43 : 42);
			else if (Key == TEXT("Text")) Blackboard->SetValueAsString(Key, Expected ? TEXT("hay needle stack") : TEXT("haystack"));
			else if (Key == TEXT("Enum")) Blackboard->SetValueAsEnum(Key, Expected ? 0 : 1);
			else { Test.AddError(TEXT("unexpected predicate fixture key")); return false; }
			Passed &= Test.TestEqual(*FString::Printf(TEXT("%s native predicate evaluates %s"), *Key.ToString(), Expected ? TEXT("true") : TEXT("false")),
				Decorator->WrappedCanExecute(*Behavior, nullptr), Expected);
		}
	}
	Passed &= Test.TestEqual(TEXT("all four bound wrappers participate in completion checks"), F.Composites.Num(), 4);
	int32 CanAbortCount = 0;
	for (UBehaviorTreeGraphNode_CompositeDecorator* Composite : F.Composites)
	{
		TArray<UBTDecorator*> Nodes; TArray<FBTDecoratorLogic> Operations;
		Composite->CollectDecoratorData(Nodes, Operations);
		if (!Test.TestEqual(TEXT("each bound fixture has one predicate leaf"), Nodes.Num(), 1)) return false;
		const FName Key = Field<FBlackboardKeySelector>(*Nodes[0], TEXT("BlackboardKey")).SelectedKeyName;
		const bool ExpectedAbort = Key == TEXT("Basic") || Key == TEXT("Text");
		Passed &= Test.TestEqual(TEXT("bound abort capability follows authored leaf policy"), Nodes[0]->GetFlowAbortMode() != EBTFlowAbortMode::None, ExpectedAbort);
		Passed &= Test.TestEqual(TEXT("composite abort cache is recomputed in both directions"), bool(Composite->bCanAbortFlow), ExpectedAbort);
		CanAbortCount += Composite->bCanAbortFlow ? 1 : 0;
		const FString Description = Composite->GetDescription().ToString();
		Passed &= Test.TestFalse(TEXT("composite cached description replaces stale sentinel"), Description.Contains(TEXT("STALE_COMPOSITE")));
		Passed &= Test.TestTrue(TEXT("composite description includes its recompleted native predicate"), Description.Contains(Nodes[0]->GetStaticDescription()));
	}
	Passed &= Test.TestEqual(TEXT("two wrappers allow abort and two forbid it"), CanAbortCount, 2);
	return Passed;
}
bool CheckTrace(FAutomationTestBase& Test)
{
	const auto Trace = UE::AssetDocumentAI::BehaviorTreeDerivedCompletion::Private::GetNotificationTraceForTest();
	int32 Enums = 0; bool Passed = true;
	for (int32 Index = 0; Index < Trace.Num(); ++Index)
	{
		if (!Trace[Index].EndsWith(TEXT("StringValue"))) continue;
		++Enums;
		Passed &= Test.TestTrue(TEXT("enum operand notification precedes same-locator arithmetic operation"), Trace.IsValidIndex(Index + 1)
			&& Trace[Index + 1] == Trace[Index].LeftChop(11) + TEXT("ArithmeticOperation"));
	}
	Passed &= Test.TestEqual(TEXT("direct and bound enum operand notifications are observed"), Enums, 2);
	return Passed;
}
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAIBehaviorTreeDecoratorRefreshTest,
	"UEAssetDoc.AIProvider.BehaviorTree.DecoratorRefresh.ForwardAndRollback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentAIBehaviorTreeDecoratorRefreshTest::RunTest(const FString&)
{
	using namespace AssetDocumentAIBehaviorTreeDecoratorRefreshTests;
	FScenario S; int32 TraceAtAuthoredRollback = -1;
	if (!S.SetUp(*this, {}, [&]()
	{
		TraceAtAuthoredRollback = UE::AssetDocumentAI::BehaviorTreeDerivedCompletion::Private::GetNotificationTraceForTest().Num();
		for (UBTDecorator_Blackboard* D : S.Fixture.Decorators)
			Field<uint8>(*D, TEXT("ArithmeticOperation")) = Field<FBlackboardKeySelector>(*D, TEXT("BlackboardKey")).SelectedKeyName == TEXT("Enum")
				? uint8(EArithmeticKeyOperation::NotEqual) : uint8(EArithmeticKeyOperation::Greater);
	})) return false;
	const auto Before = CaptureAssetDocumentBehaviorTreeDecoratorEvidence(*S.Fixture.Tree, *S.Fixture.Graph);
	if (!TestTrue(TEXT("exact live fixture inventory is captured"), Before.bSucceeded)) return false;
	TArray<FGuid> Guids; for (UEdGraphNode* Wrapper : S.Fixture.Wrappers) Guids.Add(Wrapper->NodeGuid);
	TArray<UClass*> Classes; for (const auto* Decorator : S.Fixture.Decorators) Classes.Add(Decorator->GetClass());
	UE::AssetDocumentAI::BehaviorTreeDerivedCompletion::Private::ResetNotificationTraceForTest();
	FInstrumentedTransaction Proxy(*S.Transaction);
	bool Passed = Succeeded(*this, S.Invoke(&Proxy));
	Passed &= TestEqual(TEXT("selected descriptor enlists one finalizer before its one footprint"), Proxy.Calls, TArray<FString>{TEXT("finalizer"), TEXT("footprint")});
	Passed &= CheckDerived(*this, S.Fixture); Passed &= CheckTrace(*this);
	Passed &= CheckNativePredicatesAndComposites(*this, S.Fixture);
	const auto After = CaptureAssetDocumentBehaviorTreeDecoratorEvidence(*S.Fixture.Tree, *S.Fixture.Graph);
	Passed &= TestEqual(TEXT("all eight direct/bound decorators are completed"), After.DecoratorCount, 8);
	Passed &= TestEqual(TEXT("four bound decorators are completed"), After.BoundDecoratorCount, 4);
	Passed &= TestEqual(TEXT("authored inventory survives forward derived completion"), After.AuthoredDecoratorDigest, Before.AuthoredDecoratorDigest);
	for (UBTDecorator_Blackboard* D : S.Fixture.Decorators) Field<uint8>(*D, TEXT("ArithmeticOperation")) = EArithmeticKeyOperation::Less;
	for (auto* Composite : S.Fixture.Composites)
	{
		Field<FString>(*Composite, TEXT("CachedDescription")) = TEXT("STALE_COMPOSITE");
		Composite->bCanAbortFlow = !Composite->bCanAbortFlow;
	}
	UE::AssetDocumentAI::BehaviorTreeDerivedCompletion::Private::ResetNotificationTraceForTest();
	Passed &= TestTrue(TEXT("real Atomic rollback recompletes restored live graph"), S.Transaction->Rollback(TEXTVIEW("Test.DecoratorRollback")).bValid);
	Passed &= CheckDerived(*this, S.Fixture); Passed &= CheckTrace(*this);
	Passed &= CheckNativePredicatesAndComposites(*this, S.Fixture);
	Passed &= TestEqual(TEXT("ordinary authored restoration runs before terminal notifications"), TraceAtAuthoredRollback, 0);
	Passed &= TestEqual(TEXT("ordinary authored rollback executed"), S.RollbackOrder, TArray<FString>{TEXT("authored")});
	Passed &= TestFalse(TEXT("dirty ownership restored at terminal completion"), S.Fixture.Package->IsDirty());
	for (int32 Index = 0; Index < Guids.Num(); ++Index)
	{
		Passed &= TestEqual(TEXT("wrapper persistent NodeGuid preserved"), S.Fixture.Wrappers[Index]->NodeGuid, Guids[Index]);
		Passed &= TestEqual(TEXT("decorator class at persistent locator preserved"), S.Fixture.Decorators[Index]->GetClass(), Classes[Index]);
	}
	return Passed;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAIBehaviorTreeDecoratorRefreshEnlistFailureTest,
	"UEAssetDoc.AIProvider.BehaviorTree.DecoratorRefresh.EnlistFailureZeroChange",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentAIBehaviorTreeDecoratorRefreshEnlistFailureTest::RunTest(const FString&)
{
	using namespace AssetDocumentAIBehaviorTreeDecoratorRefreshTests;
	bool Passed = true;
	for (bool FailFinalizer : {true, false})
	{
		FScenario S; if (!S.SetUp(*this)) return false;
		FInstrumentedTransaction Proxy(*S.Transaction); Proxy.bFailFinalizer = FailFinalizer; Proxy.bFailFootprint = !FailFinalizer;
		UE::AssetDocumentAI::BehaviorTreeDerivedCompletion::Private::ResetNotificationTraceForTest();
		const auto Outcome = S.Invoke(&Proxy);
		Passed &= TestNotEqual(TEXT("rejected enlistment cannot report success"), Outcome.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
		Passed &= TestEqual(TEXT("one finalizer attempt"), Proxy.FinalizerCalls, 1);
		Passed &= TestEqual(TEXT("footprint only after accepted finalizer"), Proxy.FootprintCalls, FailFinalizer ? 0 : 1);
		Passed &= TestTrue(TEXT("no lifecycle notification before full coverage"), UE::AssetDocumentAI::BehaviorTreeDerivedCompletion::Private::GetNotificationTraceForTest().IsEmpty());
		Passed &= TestTrue(TEXT("failed enlistment returns no changed set"), Outcome.ChangedSet.IsEmpty());
		Passed &= TestEqual(TEXT("stale enum discriminator remains untouched"), Field<int32>(*S.Fixture.Decorators[3], TEXT("IntValue")), 77);
		Passed &= TestFalse(TEXT("failed enlistment preserves clean dirty state"), S.Fixture.Package->IsDirty());
		S.Transaction->Rollback(TEXTVIEW("Test.EnlistFailureCleanup"));
	}
	return Passed;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAIBehaviorTreeDecoratorRefreshRejectionTest,
	"UEAssetDoc.AIProvider.BehaviorTree.DecoratorRefresh.IdentityAndDigestZeroChange",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentAIBehaviorTreeDecoratorRefreshRejectionTest::RunTest(const FString&)
{
	using namespace AssetDocumentAIBehaviorTreeDecoratorRefreshTests;
	bool Passed = true;
	for (const TCHAR* Case : {TEXT("digest"), TEXT("blackboard"), TEXT("graph"), TEXT("selector")})
	{
		FScenario S; if (!S.SetUp(*this)) return false;
		const auto Evidence = CaptureAssetDocumentBehaviorTreeDecoratorEvidence(*S.Fixture.Tree, *S.Fixture.Graph);
		auto I = S.Invocation(Evidence.AuthoredDecoratorDigest);
		UEdGraph* OriginalGraph = S.Fixture.Tree->BTGraph;
		if (FCString::Strcmp(Case, TEXT("digest")) == 0) I.ValueArguments.Elements[1] = String(FString::ChrN(64, TEXT('0')));
		if (FCString::Strcmp(Case, TEXT("blackboard")) == 0) I.ValueArguments.Elements[2] = String(TEXT("/Game/WrongBlackboard.WrongBlackboard"));
		if (FCString::Strcmp(Case, TEXT("graph")) == 0) S.Fixture.Tree->BTGraph = NewObject<UBehaviorTreeGraph>(GetTransientPackage());
		if (FCString::Strcmp(Case, TEXT("selector")) == 0) Field<FBlackboardKeySelector>(*S.Fixture.Decorators[0], TEXT("BlackboardKey")).SelectedKeyType = nullptr;
		FInstrumentedTransaction Proxy(*S.Transaction);
		const auto Outcome = CreateAssetDocumentAIPrimitiveProvider()->Invoke(I, S.Resolver, &Proxy);
		Passed &= TestNotEqual(Case, Outcome.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
		const FString ExpectedCode = FCString::Strcmp(Case, TEXT("digest")) == 0 ? TEXT("AIProvider.BehaviorTree.DecoratorInventoryRejected")
			: FCString::Strcmp(Case, TEXT("graph")) == 0 ? TEXT("AIProvider.InvalidHandle") : TEXT("AIProvider.BehaviorTree.DecoratorSelectorUnresolved");
		Passed &= TestTrue(TEXT("each rejection reaches its exact intended boundary"), Outcome.Diagnostics.ContainsByPredicate([&](const auto& D) { return D.Code == ExpectedCode; }));
		Passed &= TestEqual(TEXT("preflight mismatch enlists no finalizer"), Proxy.FinalizerCalls, 0);
		Passed &= TestEqual(TEXT("preflight mismatch enlists no footprint"), Proxy.FootprintCalls, 0);
		Passed &= TestEqual(TEXT("preflight mismatch never projects enum value"), Field<int32>(*S.Fixture.Decorators[3], TEXT("IntValue")), 77);
		Passed &= TestTrue(TEXT("rejected inventory returns empty changed set"), Outcome.ChangedSet.IsEmpty());
		S.Fixture.Tree->BTGraph = OriginalGraph;
		S.Transaction->Rollback(TEXTVIEW("Test.IdentityFailureCleanup"));
	}
	return Passed;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAIBehaviorTreeDecoratorRefreshPartialFailureTest,
	"UEAssetDoc.AIProvider.BehaviorTree.DecoratorRefresh.PartialNotificationRollback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentAIBehaviorTreeDecoratorRefreshPartialFailureTest::RunTest(const FString&)
{
	using namespace AssetDocumentAIBehaviorTreeDecoratorRefreshTests;
	FScenario S; if (!S.SetUp(*this)) return false;
	const auto Before = CaptureAssetDocumentBehaviorTreeDecoratorEvidence(*S.Fixture.Tree, *S.Fixture.Graph);
	UE::AssetDocumentAI::BehaviorTreeDerivedCompletion::Private::ResetNotificationTraceForTest();
	UE::AssetDocumentAI::BehaviorTreeDerivedCompletion::Private::SetFailAfterFirstNotificationForTest(true);
	const auto Outcome = S.Invoke();
	UE::AssetDocumentAI::BehaviorTreeDerivedCompletion::Private::SetFailAfterFirstNotificationForTest(false);
	bool Passed = TestNotEqual(TEXT("a partially completed notification sequence cannot succeed"), Outcome.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
	Passed &= TestTrue(TEXT("partial completion retains exact notification failure"), Outcome.Diagnostics.ContainsByPredicate([](const auto& D) { return D.Code == TEXT("AIProvider.BehaviorTree.DecoratorNotificationFailed"); }));
	Passed &= TestEqual(TEXT("injected forward failure happens after exactly one completed notification"), UE::AssetDocumentAI::BehaviorTreeDerivedCompletion::Private::GetNotificationTraceForTest().Num(), 1);
	int32 UnchangedDescriptions = 0;
	for (UBTDecorator_Blackboard* D : S.Fixture.Decorators) if (D->GetStaticDescription().Contains(TEXT("STALE_BASIC"))) ++UnchangedDescriptions;
	Passed &= TestTrue(TEXT("forward failure leaves a partially recomputed fixture"), UnchangedDescriptions > 0 && UnchangedDescriptions < S.Fixture.Decorators.Num());
	UE::AssetDocumentAI::BehaviorTreeDerivedCompletion::Private::ResetNotificationTraceForTest();
	Passed &= TestTrue(TEXT("real Atomic rollback completes after the one-shot forward failure"), S.Transaction->Rollback(TEXTVIEW("Test.PartialNotificationRollback")).bValid);
	Passed &= TestEqual(TEXT("partial forward failure is recoverable without poison"), S.Transaction->GetState(), EAssetDocumentAtomicState::RolledBack);
	Passed &= CheckDerived(*this, S.Fixture); Passed &= CheckTrace(*this);
	Passed &= TestEqual(TEXT("rollback preserves authored inventory after partial notifications"), CaptureAssetDocumentBehaviorTreeDecoratorEvidence(*S.Fixture.Tree, *S.Fixture.Graph).AuthoredDecoratorDigest, Before.AuthoredDecoratorDigest);
	Passed &= TestFalse(TEXT("partial forward failure restores original dirty ownership"), S.Fixture.Package->IsDirty());
	return Passed;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAIBehaviorTreeDecoratorRefreshFinalizerFailureTest,
	"UEAssetDoc.AIProvider.BehaviorTree.DecoratorRefresh.RollbackFailurePoisonsWithoutRetry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentAIBehaviorTreeDecoratorRefreshFinalizerFailureTest::RunTest(const FString&)
{
	using namespace AssetDocumentAIBehaviorTreeDecoratorRefreshTests;
	FScenario S; if (!S.SetUp(*this)) return false;
	if (!Succeeded(*this, S.Invoke())) return false;
	UE::AssetDocumentAI::BehaviorTreeDerivedCompletion::Private::ResetNotificationTraceForTest();
	UE::AssetDocumentAI::BehaviorTreeSemanticRuntimeMirrorRollback::Private::SetForceSemanticReadbackFailureForTest(true);
	const auto R = S.Transaction->Rollback(TEXTVIEW("Test.DecoratorInjectedRollbackFailure"));
	UE::AssetDocumentAI::BehaviorTreeSemanticRuntimeMirrorRollback::Private::SetForceSemanticReadbackFailureForTest(false);
	bool Passed = TestFalse(TEXT("post-rollback completion failure is not success"), R.bValid);
	Passed &= TestEqual(TEXT("real Atomic enters Poisoned"), S.Transaction->GetState(), EAssetDocumentAtomicState::Poisoned);
	Passed &= TestTrue(TEXT("kernel exact finalizer failure is preserved"), S.KernelDiagnostics.ContainsByPredicate([](const auto& D) { return D.Code == TEXT("Atomic.Poisoned.PostRollbackFinalizerFailed"); }));
	Passed &= CheckTrace(*this);
	Passed &= TestFalse(TEXT("dirty restoration is terminal even after failed verification"), S.Fixture.Package->IsDirty());
	return Passed;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAIBehaviorTreeDecoratorRefreshSecondFinalizerTest,
	"UEAssetDoc.AIProvider.BehaviorTree.DecoratorRefresh.SecondFinalizerRejected",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentAIBehaviorTreeDecoratorRefreshSecondFinalizerTest::RunTest(const FString&)
{
	using namespace AssetDocumentAIBehaviorTreeDecoratorRefreshTests;
	FScenario S; if (!S.SetUp(*this)) return false;
	if (!Succeeded(*this, S.Invoke())) return false;
	UE::AssetDocumentAI::BehaviorTreeDerivedCompletion::Private::ResetNotificationTraceForTest();
	FInstrumentedTransaction Proxy(*S.Transaction);
	if (!S.Reissue(*this)) return false;
	const auto Again = S.Invoke(&Proxy);
	bool Passed = TestNotEqual(TEXT("a second terminal registration fails"), Again.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded);
	Passed &= TestEqual(TEXT("repeated handler attempts one closed finalizer"), Proxy.FinalizerCalls, 1);
	Passed &= TestEqual(TEXT("rejected second finalizer never enlists a second footprint"), Proxy.FootprintCalls, 0);
	Passed &= TestTrue(TEXT("second finalizer rejection does not replay notifications"), UE::AssetDocumentAI::BehaviorTreeDerivedCompletion::Private::GetNotificationTraceForTest().IsEmpty());
	S.Transaction->Rollback(TEXTVIEW("Test.SecondFinalizerCleanup"));
	return Passed;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAIBehaviorTreeDecoratorRefreshModeExclusionTest,
	"UEAssetDoc.AIProvider.BehaviorTree.DecoratorRefresh.MutuallyExclusiveModes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentAIBehaviorTreeDecoratorRefreshModeExclusionTest::RunTest(const FString&)
{
	using namespace AssetDocumentAIBehaviorTreeDecoratorRefreshTests;
	bool Passed = true;
	for (bool DecoratorFirst : {true, false})
	{
		FScenario S; if (!S.SetUp(*this, {}, {}, true)) return false;
		auto Lease = ResolveExistingMutableAssetLease(*this, *S.Fixture.Tree, *S.Transaction, S.Resolver);
		if (!Lease) return false;
		const FAssetDocumentMutationFootprint MirrorRow{EAssetDocumentMutationFootprintKind::UObjectState,
			TEXT("asset:") + S.Fixture.Tree->GetPathName() + TEXT("/RuntimeMirror"), TEXT("assetdocument.ai.behavior_tree.runtime_mirror.v1")};
		auto Mirror = [&](IAssetDocumentAtomicTransaction& Transaction)
		{
			return RebuildAssetDocumentBehaviorTreeRuntimeMirror(*Lease, *S.Fixture.Graph,
				UBehaviorTreeGraph::KeepRebuildCounter, MirrorRow, Transaction);
		};
		if (DecoratorFirst) { if (!Succeeded(*this, S.Invoke())) return false; }
		else if (!TestTrue(TEXT("standalone MirrorOnly first mode still succeeds"), Mirror(*S.Transaction).bSucceeded)) return false;
		UE::AssetDocumentAI::BehaviorTreeDerivedCompletion::Private::ResetNotificationTraceForTest();
		FInstrumentedTransaction Proxy(*S.Transaction);
		if (DecoratorFirst) { Lease = ResolveExistingMutableAssetLease(*this, *S.Fixture.Tree, *S.Transaction, S.Resolver); if (!Lease) return false; Passed &= TestFalse(TEXT("MirrorOnly cannot register after DecoratorAndMirror"), Mirror(Proxy).bSucceeded); }
		else { if (!S.Reissue(*this)) return false; Passed &= TestNotEqual(TEXT("DecoratorAndMirror cannot register after MirrorOnly"), S.Invoke(&Proxy).Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded); }
		Passed &= TestEqual(TEXT("second mode attempts one finalizer"), Proxy.FinalizerCalls, 1);
		Passed &= TestEqual(TEXT("second mode never enlists its terminal row"), Proxy.FootprintCalls, 0);
		Passed &= TestTrue(TEXT("second mode never begins decorator notifications"), UE::AssetDocumentAI::BehaviorTreeDerivedCompletion::Private::GetNotificationTraceForTest().IsEmpty());
		S.Transaction->Rollback(TEXTVIEW("Test.ModeExclusionCleanup"));
	}
	return Passed;
}
namespace AssetDocumentAIBehaviorTreeValueOrKeyRefreshTests
{
FString ProducerDigest(const FString& Path, const FAssetDocumentRuntimeValueDto& Value);
}
namespace AssetDocumentAIBehaviorTreeDecoratorRefreshTests
{
FAssetDocumentPrimitiveOutcome PublicValueOrReadback(FScenario& S)
{
 auto I=S.Invocation(TEXT("unused"));I.PrimitiveId=TEXT("ai.behavior_tree.authored_graph.readback.v1");
 I.ValueArguments.ObjectKeys={TEXT("behavior_tree_path")};I.ValueArguments.Elements={String(S.Fixture.Tree->GetPathName())};
 const auto ReadOnly=S.Resolver.RegisterProviderScopedAlias(S.Handle,TEXTVIEW("ai"),EAssetDocumentHandleAccess::ReadOnly);
 if(!ReadOnly.IsSet())return {};
 I.HandleArguments={{TEXT("asset"),ReadOnly.GetValue()}};
 return CreateAssetDocumentAIPrimitiveProvider()->Invoke(I,S.Resolver,S.Transaction.Get());
}
FAssetDocumentPrimitiveInvocation TopValueOrInvocation(FScenario& S,const FAssetDocumentRuntimeValueDto& D2)
{
 auto I=S.Invocation(TEXT("unused"));I.PrimitiveId=TEXT("ai.behavior_tree.value_or_key.refresh.v1");
 I.ValueArguments.ObjectKeys={TEXT("behavior_tree_path"),TEXT("completion_registration"),TEXT("expected_authored_inventory_digest"),TEXT("expected_blackboard_path")};
 I.ValueArguments.Elements={String(S.Fixture.Tree->GetPathName()),String(TEXT("current_graph")),String(AssetDocumentAIBehaviorTreeValueOrKeyRefreshTests::ProducerDigest(S.Fixture.Tree->GetPathName(),D2)),String(S.Fixture.Blackboard->GetPathName())};return I;
}
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAIValueOrDecoratorEnumTest,
 "UEAssetDoc.AIProvider.BehaviorTree.ValueOrKeyRefresh.StaleEnumProjectionAndOrdinaryOperandGuard",
 EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FAssetDocumentAIValueOrDecoratorEnumTest::RunTest(const FString&)
{
 using namespace AssetDocumentAIBehaviorTreeDecoratorRefreshTests;
 using namespace UE::AssetDocumentAI::BehaviorTreeDerivedCompletion::Private;
 for(bool CorruptOrdinary:{false,true})
 {
  FScenario S;if(!S.SetUp(*this,{},[&S](){for(auto* D:S.Fixture.Decorators)if(Field<FBlackboardKeySelector>(*D,TEXT("BlackboardKey")).SelectedKeyName==TEXT("Arithmetic"))Field<int32>(*D,TEXT("IntValue"))=42;}))return false;
  const auto Before=PublicValueOrReadback(S);if(!TestEqual(TEXT("public D2 admits stale enum fixture"),Before.Status,EAssetDocumentPrimitiveOutcomeStatus::Succeeded))return false;
  if(CorruptOrdinary)SetBeforeValueOrSecondObservationForTest([&S](){Field<int32>(*S.Fixture.Decorators[1],TEXT("IntValue"))=43;});
  const auto R=CreateAssetDocumentAIPrimitiveProvider()->Invoke(TopValueOrInvocation(S,Before.Value),S.Resolver,S.Transaction.Get());
  SetBeforeValueOrSecondObservationForTest({});
  if(CorruptOrdinary)
  {
   TestTrue(TEXT("ordinary integer operand mutation remains forbidden"),R.Diagnostics.ContainsByPredicate([](const auto& D){return D.Code==TEXT("AIProvider.BehaviorTree.ValueOrCompletionMismatch");}));
   TestTrue(TEXT("mismatch has empty results"),R.Value.Kind==EAssetDocumentRuntimeValueKind::Object&&R.Value.ObjectKeys.IsEmpty()&&R.Value.Elements.IsEmpty()&&R.HandleResults.IsEmpty()&&R.ChangedSet.IsEmpty());
  }
  else
  {
   if(!Succeeded(*this,R))return false;
   if(!CheckDerived(*this,S.Fixture)||!CheckNativePredicatesAndComposites(*this,S.Fixture)||!S.Reissue(*this))return false;
   const auto After=PublicValueOrReadback(S);if(!TestEqual(TEXT("D2 observes completed enum projection"),After.Status,EAssetDocumentPrimitiveOutcomeStatus::Succeeded))return false;
   const int32 DigestIndex=R.Value.ObjectKeys.IndexOfByKey(TEXT("authored_inventory_digest"));if(!TestTrue(TEXT("top-mode returns resulting wire digest"),DigestIndex!=INDEX_NONE))return false;
   TestEqual(TEXT("returned wire digest matches actual completed D2 Value"),R.Value.Elements[DigestIndex].StringValue,AssetDocumentAIBehaviorTreeValueOrKeyRefreshTests::ProducerDigest(S.Fixture.Tree->GetPathName(),After.Value));
  }
  TestTrue(TEXT("same combined finalizer restores all decorator semantics"),S.Transaction->Rollback(TEXTVIEW("Test.TopModeDecoratorRollback")).bValid);
  if(!CheckDerived(*this,S.Fixture)||!CheckNativePredicatesAndComposites(*this,S.Fixture))return false;
 }
 return !HasAnyErrors();
}
#endif // WITH_DEV_AUTOMATION_TESTS
