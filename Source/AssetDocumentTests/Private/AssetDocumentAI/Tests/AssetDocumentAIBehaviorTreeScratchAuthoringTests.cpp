// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
#if WITH_DEV_AUTOMATION_TESTS
#include "AssetDocumentAIPrimitiveProvider.h"
#include "Provider/AssetDocumentAIProviderContracts.h"
#include "AssetDocumentAIBehaviorTreeProviderTestSupport.h"
#include "Tests/Fixtures/AssetDocumentAITestFixture.h"
#include "Tests/Fixtures/AssetDocumentAIScratchAuthoringTestTypes.h"
#include "Provider/BehaviorTree/AssetDocumentAIBehaviorTreeScratchAuthoring.h"
#include "Provider/BehaviorTree/AssetDocumentAIBehaviorTreeAuthoredReadbackInternal.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/Tasks/BTTask_MoveTo.h"
#include "BehaviorTree/Tasks/BTTask_Wait.h"
#include "BehaviorTreeGraph.h"
#include "BehaviorTreeGraphNode.h"
#include "BehaviorTreeGraphNode_Root.h"
#include "BehaviorTreeGraphNode_CompositeDecorator.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphSchema.h"
#include "EdGraphNode_Comment.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "UObject/Package.h"

namespace AssetDocumentAIBehaviorTreeScratchAuthoringTests
{
using namespace AssetDocumentAIBehaviorTreeProviderTestSupport;
using namespace UE::AssetDocumentAI::BehaviorTreeAuthoredReadback;
using FValue = FAssetDocumentRuntimeValueDto;
FValue Id(int32 Number) { return StringValue(FGuid(0, 0, 0, Number).ToString(EGuidFormats::Digits)); }
FValue Editor(int32 X, int32 Y)
{
 return ObjectValue({TEXT("comment_bubble_pinned"), TEXT("comment_bubble_visible"), TEXT("node_comment"), TEXT("position")},
  {BooleanValue(true), BooleanValue(true), StringValue(TEXT("authored note")), ObjectValue({TEXT("x"), TEXT("y")}, {IntegerValue(X), IntegerValue(Y)})});
}
FValue BoundNode(int32 Number, const TCHAR* Kind, int32 Inputs)
{
 return ObjectValue({TEXT("authored_properties"), TEXT("editor"), TEXT("input_count"), TEXT("kind"), TEXT("node_class_path_or_null"), TEXT("stable_node_id")},
  {ObjectValue({}, {}), Editor(Number * 10, Number * 20), IntegerValue(Inputs), StringValue(Kind),
   FString(Kind) == TEXT("test") ? StringValue(TEXT("/Script/AIModule.BTDecorator_ForceSuccess")) : NullValue(), Id(Number)});
}
FValue Link(int32 From, int32 To, int32 Input)
{
 return ObjectValue({TEXT("from_stable_node_id"), TEXT("to_input_index"), TEXT("to_stable_node_id")}, {Id(From), IntegerValue(Input), Id(To)});
}
FValue Comment(int32 Number)
{
 return ObjectValue({TEXT("color"), TEXT("color_comment_bubble"), TEXT("comment_bubble_pinned"), TEXT("comment_bubble_visible"), TEXT("comment_bubble_visible_in_details"),
  TEXT("comment_depth"), TEXT("font_size"), TEXT("move_mode"), TEXT("node_details"), TEXT("position"), TEXT("size"), TEXT("stable_node_id"), TEXT("text")},
  {ObjectValue({TEXT("a"), TEXT("b"), TEXT("g"), TEXT("r")}, {NumberValue(1), NumberValue(0.25), NumberValue(0.5), NumberValue(0.75)}),
   BooleanValue(true), BooleanValue(true), BooleanValue(true), BooleanValue(true), IntegerValue(3), IntegerValue(18), StringValue(TEXT("GroupMovement")),
   StringValue(TEXT("details")), ObjectValue({TEXT("x"), TEXT("y")}, {IntegerValue(-50), IntegerValue(60)}),
   ObjectValue({TEXT("height"), TEXT("width")}, {IntegerValue(150), IntegerValue(300)}), Id(Number), StringValue(TEXT("whole authored comment"))});
}
FValue Tree(bool bComposite, bool bNested = false, bool bEmptyKey = false)
{
 FValue Task = ObjectValue({TEXT("authored_properties"), TEXT("children"), TEXT("decorators"), TEXT("editor"), TEXT("node_class_path"), TEXT("services"), TEXT("stable_node_id")},
  {ObjectValue({TEXT("WaitTime")}, {ObjectValue({TEXT("DefaultValue"), TEXT("Key")}, {NumberValue(7.5), StringValue(TEXT("None"))})}), ArrayValue({}), ArrayValue({}), Editor(30, 200), StringValue(TEXT("/Script/AIModule.BTTask_Wait")), ArrayValue({}), Id(2)});
 if (bNested)
 {
  Task.Elements[Task.ObjectKeys.IndexOfByKey(TEXT("node_class_path"))] = StringValue(UAssetDocumentAIScratchNestedTask::StaticClass()->GetPathName());
  Task.Elements[Task.ObjectKeys.IndexOfByKey(TEXT("authored_properties"))] = ObjectValue({TEXT("Dynamic"), TEXT("Nested"), TEXT("Numbers"), TEXT("Rows"), TEXT("StringRows")},
   {ObjectValue({TEXT("Properties"), TEXT("Struct")}, {ObjectValue({TEXT("X"), TEXT("Y"), TEXT("Z")}, {NumberValue(1.5), NumberValue(2.5), NumberValue(3.5)}), StringValue(TEXT("/Script/CoreUObject.Vector"))}),
    ObjectValue({TEXT("Class"), TEXT("Properties")}, {StringValue(UAssetDocumentAIInstallOwnedChild::StaticClass()->GetPathName()), ObjectValue({TEXT("AuthoredValue")}, {IntegerValue(109)})}),
    ArrayValue({IntegerValue(0), IntegerValue(7)}), ObjectValue({TEXT("None"), TEXT("other")}, {IntegerValue(41), IntegerValue(73)}),
    bEmptyKey ? ArrayValue({ObjectValue({TEXT("key"), TEXT("value")}, {StringValue(TEXT("")), IntegerValue(7)}),
     ObjectValue({TEXT("key"), TEXT("value")}, {StringValue(TEXT("other")), IntegerValue(19)})})
     : ObjectValue({TEXT("other")}, {IntegerValue(19)})});
 }
 FValue Decorators = ArrayValue({});
 if (bComposite)
 {
  FValue Bound = ObjectValue({TEXT("comments"), TEXT("graph_guid"), TEXT("links"), TEXT("nodes")},
   {ArrayValue({Comment(30)}), Id(10), ArrayValue({Link(14, 12, 0), Link(15, 12, 1), Link(16, 12, 2), Link(12, 11, 0)}),
    ArrayValue({BoundNode(11, TEXT("sink"), 1), BoundNode(12, TEXT("and"), 3), BoundNode(14, TEXT("test"), 0), BoundNode(15, TEXT("test"), 0), BoundNode(16, TEXT("test"), 0)})});
  Decorators = ArrayValue({ObjectValue({TEXT("authored_properties"), TEXT("bound_graph"), TEXT("editor"), TEXT("kind"), TEXT("node_class_path"), TEXT("stable_node_id")},
   {ObjectValue({TEXT("bShowOperations"), TEXT("CompositeName")}, {BooleanValue(true), StringValue(TEXT("Exact gate"))}), MoveTemp(Bound), Editor(0, 100), StringValue(TEXT("composite")), NullValue(), Id(3)})});
 }
 FValue Root = ObjectValue({TEXT("authored_properties"), TEXT("children"), TEXT("decorators"), TEXT("editor"), TEXT("node_class_path"), TEXT("services"), TEXT("stable_node_id")},
  {ObjectValue({}, {}), ArrayValue({Task}), MoveTemp(Decorators), Editor(0, 100), StringValue(TEXT("/Script/AIModule.BTComposite_Sequence")), ArrayValue({}), Id(1)});
 return ObjectValue({TEXT("comments"), TEXT("graph_guid"), TEXT("graph_state"), TEXT("root")}, {ArrayValue({Comment(40)}), Id(100), StringValue(TEXT("present")), MoveTemp(Root)});
}

struct FScenario
{
 TStrongObjectPtr<UPackage> Package;
 TStrongObjectPtr<UBehaviorTree> Target;
 FResolveExistingHandleResolver Resolver;
 TUniquePtr<FAssetDocumentRollbackActionComposition> Composition;
 TUniquePtr<IAssetDocumentAtomicTransaction> Kernel;
 FAssetDocumentOpaqueHandle Graph;
 TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
 ~FScenario()
 {
  if (Kernel && (Kernel->GetState() == EAssetDocumentAtomicState::Live || Kernel->GetState() == EAssetDocumentAtomicState::Mutating || Kernel->GetState() == EAssetDocumentAtomicState::ReadyToCommit)) Kernel->Rollback(TEXTVIEW("Test.ScratchCleanup"));
  if (Target.IsValid()) Target->ClearFlags(RF_Standalone);
  if (Package.IsValid()) Package->SetDirtyFlag(false);
 }
 FAssetDocumentPrimitiveInvocation Invocation(const TCHAR* Primitive, FValue Value)
 {
  FAssetDocumentPrimitiveInvocation I;
  const auto& Token = Kernel->GetMutationToken();
  I.PrimitiveId = Primitive; I.Version = 1; I.RequestId = Token.RequestId;
  I.TransactionId = Token.TransactionId; I.RuntimeRevision = Token.RuntimeRevision;
  I.Phase = EAssetDocumentRuntimeRequestPhase::Apply; I.ValueArguments = MoveTemp(Value);
  return I;
 }
 bool Start(FAutomationTestBase& Test)
 {
  Package.Reset(CreatePackage(*(TEXT("/Temp/AssetDocumentAI/ScratchAuthor_") + FGuid::NewGuid().ToString(EGuidFormats::Digits))));
  Target.Reset(NewObject<UBehaviorTree>(Package.Get(), TEXT("Tree"), RF_Public | RF_Standalone));
  Composition = MakeUnique<FAssetDocumentRollbackActionComposition>(Resolver, TArray<TSharedRef<IAssetDocumentRollbackAction>>{},
   [&](EAssetDocumentRuntimeRequestPhase, const FAssetDocumentRuntimeDiagnostic& D) { Diagnostics.Add(D); });
  FAssetDocumentPreflightReceipt Receipt;
  Receipt.RequestId = FGuid::NewGuid(); Receipt.RuntimeRevision = 1; Receipt.bSucceeded = true; Receipt.ReceiptHash = TEXT("sha256:scratch-authoring-test");
  Receipt.CompleteFootprint = {{EAssetDocumentMutationFootprintKind::UObjectState,
   TEXT("scratch:ai.behavior_tree:request:") + Receipt.RequestId.ToString(EGuidFormats::Digits), TEXT("assetdocument.ai.behavior_tree.scratch_bundle.v1")}};
  static FNoCancellation Cancellation;
  Kernel = Composition->BeginLiveAfterPreflight(Receipt, Cancellation);
  if (!Kernel) return Test.TestTrue(TEXT("real Atomic begins"), false);
  Resolver.SetMutationToken(Kernel->GetMutationToken()); Resolver.SetIssuingProviderId(TEXT("ai"));
  const auto Created = CreateAssetDocumentAIPrimitiveProvider()->Invoke(Invocation(TEXT("ai.behavior_tree.graph.create.v1"),
   ObjectValue({TEXT("behavior_tree_path"), TEXT("graph_name")}, {StringValue(Target->GetPathName()), StringValue(TEXT("AuthoredGraph"))})), Resolver, Kernel.Get());
  if (!Test.TestEqual(TEXT("actual graph.create"), Created.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded)) return false;
  for (const auto& Result : Created.HandleResults) if (Result.Name == TEXT("graph") && Result.Handles.Num() == 1) { Graph = Result.Handles[0]; return true; }
  return false;
 }
 FAssetDocumentPrimitiveOutcome Author(FValue Value)
 {
  auto I = Invocation(TEXT("ai.behavior_tree.graph.author_scratch.v1"), ObjectValue({TEXT("tree")}, {MoveTemp(Value)}));
  I.HandleArguments = {{TEXT("graph"), Graph}};
  return CreateAssetDocumentAIPrimitiveProvider()->Invoke(I, Resolver, Kernel.Get());
 }
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAIScratchAuthoringTest,
 "UEAssetDoc.AIProvider.BehaviorTree.ScratchAuthoring.StockActionsTypedIdentity",
 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentAIScratchAuthoringTest::RunTest(const FString&)
{
 using namespace AssetDocumentAIBehaviorTreeScratchAuthoringTests;
 FAssetDocumentAITestFixture Authority;
 if (!Authority.BootstrapBehaviorTreeEditorCache(*this) || !Authority.CloseBehaviorTreeEditorToolkit(*this)) return false;
 for (int32 Case = 0; Case < 6; ++Case)
 {
  AddInfo(FString::Printf(TEXT("ScratchAuthoring StockActions case=%d (0=simple, 1=composite, 2=nested, 3=generated-guid, 4=empty-string-map, 5=disabled-MoveTo-defaults)"), Case));
  const bool bComposite = Case == 1;
  const bool bNested = Case == 2 || Case == 4;
  const bool bEmptyKey = Case == 4;
  const bool bMoveToDefaults = Case == 5;
  FScenario S;
  if (!S.Start(*this)) return false;
  const bool bTargetPackageDirty = S.Package->IsDirty();
  FGuid StockGraphGuid;
  if (Case == 3)
  {
   auto I = S.Invocation(TEXT("ai.behavior_tree.graph.author_scratch.v1"), ObjectValue({}, {}));
   I.HandleArguments = {{TEXT("graph"), S.Graph}};
   FAssetDocumentAIBehaviorTreeGraphLease Native;
   TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
   if (!AcquireAssetDocumentAIBehaviorTreeGraphLeaseInternal(I, S.Resolver, TEXTVIEW("graph"), Native, Diagnostics)) return false;
   StockGraphGuid = Native.Graph->GraphGuid;
  }
  FValue Desired = Tree(bComposite, bNested, bEmptyKey);
  FValue MoveToDefaults;
  TArray<uint8> MoveToDefaultBytes;
  TStrongObjectPtr<UBlackboardData> UnrelatedBlackboard;
  if (bMoveToDefaults)
  {
   if (!TestTrue(TEXT("actual MoveTo typed class defaults are readable"), BuildAuthoredProperties(
    *GetMutableDefault<UBTTask_MoveTo>(), MoveToDefaults, nullptr, EAuthoredGraphReadbackVersion::V2)
    && EncodeAuthoredCanonicalValue(MoveToDefaults, MoveToDefaultBytes))) return false;
   TestFalse(TEXT("stock MoveTo observe driver defaults to false"), GetDefault<UBTTask_MoveTo>()->bObserveBlackboardValue);
   TestNull(TEXT("non-editable observe driver remains outside authored defaults"), FindAssetDocumentAIBehaviorTreeField(MoveToDefaults, TEXT("bObserveBlackboardValue")));
   if (!TestNotNull(TEXT("disabled tolerance remains part of complete authored defaults"), FindAssetDocumentAIBehaviorTreeField(MoveToDefaults, TEXT("ObservedBlackboardValueTolerance")))) return false;
   auto I = S.Invocation(TEXT("ai.behavior_tree.graph.author_scratch.v1"), ObjectValue({}, {}));
   I.HandleArguments = {{TEXT("graph"), S.Graph}};
   FAssetDocumentAIBehaviorTreeGraphLease Native;
   TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
   if (!AcquireAssetDocumentAIBehaviorTreeGraphLeaseInternal(I, S.Resolver, TEXTVIEW("graph"), Native, Diagnostics)) return false;
   Native.Graph->GetSchema()->CreateDefaultNodesForGraph(*Native.Graph);
   auto* StockRoot = Native.Graph->Nodes.Num() == 1 ? Cast<UBehaviorTreeGraphNode_Root>(Native.Graph->Nodes[0]) : nullptr;
   if (!TestNotNull(TEXT("scratch has its real stock default Root"), StockRoot)) return false;
   UnrelatedBlackboard.Reset(NewObject<UBlackboardData>(GetTransientPackage(), NAME_None, RF_Transient));
   StockRoot->BlackboardAsset = UnrelatedBlackboard.Get();
   StockRoot->UpdateBlackboard();
   TestTrue(TEXT("stock Root setup reproduces an unrelated loaded Blackboard choice"), Native.State->Tree->BlackboardAsset == UnrelatedBlackboard.Get());
   auto& DesiredRoot = Desired.Elements[Desired.ObjectKeys.IndexOfByKey(TEXT("root"))];
   auto& TaskDto = DesiredRoot.Elements[DesiredRoot.ObjectKeys.IndexOfByKey(TEXT("children"))].Elements[0];
   TaskDto.Elements[TaskDto.ObjectKeys.IndexOfByKey(TEXT("node_class_path"))] = StringValue(UBTTask_MoveTo::StaticClass()->GetPathName());
   TaskDto.Elements[TaskDto.ObjectKeys.IndexOfByKey(TEXT("authored_properties"))] = MoveToDefaults;
  }
  if (Case == 3)
  {
   Desired.Elements[Desired.ObjectKeys.IndexOfByKey(TEXT("graph_guid"))] = NullValue();
   FValue& CommentDto = Desired.Elements[Desired.ObjectKeys.IndexOfByKey(TEXT("comments"))].Elements[0];
   CommentDto.Elements[CommentDto.ObjectKeys.IndexOfByKey(TEXT("comment_bubble_visible_in_details"))] = BooleanValue(false);
  }
  const auto Result = S.Author(Desired);
  for (const auto& D : Result.Diagnostics) AddInfo(D.Code + TEXT(": ") + D.Message);
  if (!TestEqual(TEXT("whole lowered tree via public stock actions"), Result.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded)) return false;
  const auto* GraphGuid = FindAssetDocumentAIBehaviorTreeField(Result.Value, TEXT("graph_guid"));
  if (Case == 3)
  {
   FGuid Generated;
   TestTrue(TEXT("null top graph GUID returns a valid stock identity"), GraphGuid
    && FGuid::Parse(GraphGuid->StringValue, Generated) && Generated.IsValid() && Generated == StockGraphGuid);
  }
  else TestTrue(TEXT("exact authored GraphGuid retained"), GraphGuid && GraphGuid->StringValue == Id(100).StringValue);
  const auto* Root = FindAssetDocumentAIBehaviorTreeField(Result.Value, TEXT("root"));
  const auto* Comments = FindAssetDocumentAIBehaviorTreeField(Result.Value, TEXT("comments"));
  if (!Comments || Comments->Elements.Num() != 1) return false;
  const FValue& CommentDto = Comments->Elements[0];
  TestTrue(TEXT("comment color condition and independent bubble values survive stock notifications"),
   FindAssetDocumentAIBehaviorTreeField(CommentDto, TEXT("color_comment_bubble"))->BooleanValue
   && FindAssetDocumentAIBehaviorTreeField(CommentDto, TEXT("comment_bubble_pinned"))->BooleanValue
   && FindAssetDocumentAIBehaviorTreeField(CommentDto, TEXT("comment_bubble_visible"))->BooleanValue
   && FindAssetDocumentAIBehaviorTreeField(CommentDto, TEXT("comment_bubble_visible_in_details"))->BooleanValue == (Case != 3));
  if (!TestNotNull(TEXT("full root readback"), Root)) return false;
  TestEqual(TEXT("exact root NodeGuid"), FindAssetDocumentAIBehaviorTreeField(*Root, TEXT("stable_node_id"))->StringValue, Id(1).StringValue);
  const auto* Children = FindAssetDocumentAIBehaviorTreeField(*Root, TEXT("children"));
  if (!Children || Children->Elements.Num() != 1) return false;
  const auto* Props = FindAssetDocumentAIBehaviorTreeField(Children->Elements[0], TEXT("authored_properties"));
  if (bNested)
  {
   const auto* Nested = Props ? FindAssetDocumentAIBehaviorTreeField(*Props, TEXT("Nested")) : nullptr;
   const auto* NestedProperties = Nested ? FindAssetDocumentAIBehaviorTreeField(*Nested, TEXT("Properties")) : nullptr;
   const auto* Authored = NestedProperties ? FindAssetDocumentAIBehaviorTreeField(*NestedProperties, TEXT("AuthoredValue")) : nullptr;
   TestTrue(TEXT("stock edit-inline created missing owned instance with authored properties"), Authored && Authored->IntegerValue == 109);
   const auto* Dynamic = Props ? FindAssetDocumentAIBehaviorTreeField(*Props, TEXT("Dynamic")) : nullptr;
   const auto* DynamicProperties = Dynamic ? FindAssetDocumentAIBehaviorTreeField(*Dynamic, TEXT("Properties")) : nullptr;
   const auto* X = DynamicProperties ? FindAssetDocumentAIBehaviorTreeField(*DynamicProperties, TEXT("X")) : nullptr;
   TestTrue(TEXT("stock property helper initializes instanced struct and typed vector payload"), X && X->NumberValue == 1.5);
   const auto* Numbers = Props ? FindAssetDocumentAIBehaviorTreeField(*Props, TEXT("Numbers")) : nullptr;
   const auto* Rows = Props ? FindAssetDocumentAIBehaviorTreeField(*Props, TEXT("Rows")) : nullptr;
   TestTrue(TEXT("set preserves zero element while adding another key"), Numbers && Numbers->Elements.Num() == 2
    && Numbers->Elements[0].IntegerValue == 0 && Numbers->Elements[1].IntegerValue == 7);
   TestTrue(TEXT("map preserves default None name key while adding another key"), Rows
    && FindAssetDocumentAIBehaviorTreeField(*Rows, TEXT("None")) && FindAssetDocumentAIBehaviorTreeField(*Rows, TEXT("None"))->IntegerValue == 41);
   auto I = S.Invocation(TEXT("ai.behavior_tree.graph.author_scratch.v1"), ObjectValue({}, {}));
   I.HandleArguments = {{TEXT("graph"), S.Graph}};
   FAssetDocumentAIBehaviorTreeGraphLease Native;
   TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
   if (!AcquireAssetDocumentAIBehaviorTreeGraphLeaseInternal(I, S.Resolver, TEXTVIEW("graph"), Native, Diagnostics)) return false;
   UAssetDocumentAIScratchNestedTask* Task = nullptr;
   for (UEdGraphNode* Node : Native.Graph->Nodes) if (auto* BT = Cast<UBehaviorTreeGraphNode>(Node))
    if (auto* Candidate = Cast<UAssetDocumentAIScratchNestedTask>(BT->NodeInstance)) Task = Candidate;
   TestTrue(TEXT("new nested object has exact native owner and authored value"), Task && Task->Nested
    && Task->Nested->GetOuter() == Task && Task->Nested->AuthoredValue == 109);
   if (bEmptyKey)
   {
   const int32* EmptyValue = Task ? Task->StringRows.Find(FString()) : nullptr;
   const int32* OtherValue = Task ? Task->StringRows.Find(TEXT("other")) : nullptr;
   TestTrue(TEXT("native FString map hash lookup preserves empty and nonempty keys"),
    Task && Task->StringRows.Num() == 2 && EmptyValue && *EmptyValue == 7 && OtherValue && *OtherValue == 19);
   const auto* StringRows = Props ? FindAssetDocumentAIBehaviorTreeField(*Props, TEXT("StringRows")) : nullptr;
   TestTrue(TEXT("V2 empty-key map uses sorted explicit entries"), StringRows && StringRows->Kind == EAssetDocumentRuntimeValueKind::Array
    && StringRows->Elements.Num() == 2
    && FindAssetDocumentAIBehaviorTreeField(StringRows->Elements[0], TEXT("key"))->StringValue.IsEmpty()
    && FindAssetDocumentAIBehaviorTreeField(StringRows->Elements[0], TEXT("value"))->IntegerValue == 7);
   }
   TestTrue(TEXT("class default nested reference was not modified"), !GetDefault<UAssetDocumentAIScratchNestedTask>()->Nested);
   FValue V1;
   if (bEmptyKey)
   {
    if (!TestTrue(TEXT("V1 internal producer retains its original object representation"),
     BuildAuthoredProperties(*Task, V1, nullptr, EAuthoredGraphReadbackVersion::V1))) return false;
    const auto* OldRows = FindAssetDocumentAIBehaviorTreeField(V1, TEXT("StringRows"));
    const auto* OldEmpty = OldRows ? FindAssetDocumentAIBehaviorTreeField(*OldRows, TEXT("")) : nullptr;
    TestTrue(TEXT("V1 internal object preserves the empty FString key and its value"),
     OldRows && OldRows->Kind == EAssetDocumentRuntimeValueKind::Object
     && OldRows->ObjectKeys.Num() == 2 && OldEmpty && OldEmpty->IntegerValue == 7);
    TestFalse(TEXT("V1 internal empty-key object remains outside the public AI canonical value boundary"),
     IsAssetDocumentAICanonicalRuntimeValue(V1));
   }
   else
   {
   if (!Task || !BuildAuthoredProperties(*Task, V1, nullptr, EAuthoredGraphReadbackVersion::V1)) return false;
   const auto* OldNested = FindAssetDocumentAIBehaviorTreeField(V1, TEXT("Nested"));
   TestTrue(TEXT("readback V1 keeps its original reference representation"), OldNested
    && FindAssetDocumentAIBehaviorTreeField(*OldNested, TEXT("Kind"))
    && FindAssetDocumentAIBehaviorTreeField(*OldNested, TEXT("Kind"))->StringValue == TEXT("AssetRef"));
   }
  }
  else if (bMoveToDefaults)
  {
   TArray<uint8> AuthoredBytes;
   TestTrue(TEXT("complete MoveTo defaults survive typed scratch authoring"), Props
    && EncodeAuthoredCanonicalValue(*Props, AuthoredBytes) && AuthoredBytes == MoveToDefaultBytes);
   auto I = S.Invocation(TEXT("ai.behavior_tree.graph.author_scratch.v1"), ObjectValue({}, {}));
   I.HandleArguments = {{TEXT("graph"), S.Graph}};
   FAssetDocumentAIBehaviorTreeGraphLease Native;
   TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
   if (!AcquireAssetDocumentAIBehaviorTreeGraphLeaseInternal(I, S.Resolver, TEXTVIEW("graph"), Native, Diagnostics)) return false;
   TestNull(TEXT("whole authoring clears the stock Root's unrelated Blackboard before node creation"), Native.State->Tree->BlackboardAsset.Get());
   for (UEdGraphNode* Node : Native.Graph->Nodes) if (auto* StockRoot = Cast<UBehaviorTreeGraphNode_Root>(Node))
    TestNull(TEXT("scratch Root and Tree retain the same neutral Blackboard"), StockRoot->BlackboardAsset.Get());
   UBTTask_MoveTo* Task = nullptr;
   for (UEdGraphNode* Node : Native.Graph->Nodes) if (auto* BT = Cast<UBehaviorTreeGraphNode>(Node))
    if (auto* Candidate = Cast<UBTTask_MoveTo>(BT->NodeInstance)) Task = Candidate;
   if (!TestNotNull(TEXT("stock MoveTo instance belongs to the actual scratch graph"), Task)) return false;
   TestTrue(TEXT("MoveTo writes target the scratch tree rather than the live asset"), Native.State.IsValid()
    && Task->GetOuter() == Native.State->Tree.Get() && Task->GetOuter() != S.Target.Get());
   const auto* Tolerance = FindAssetDocumentAIBehaviorTreeField(MoveToDefaults, TEXT("ObservedBlackboardValueTolerance"));
   FString Error;
   TestTrue(TEXT("unchanged disabled tolerance succeeds without a setter"), UE::AssetDocumentAI::BehaviorTreeScratchAuthoring::WriteProperties(
    *Task, ObjectValue({TEXT("ObservedBlackboardValueTolerance")}, {*Tolerance}), Error));
   FValue ChangedTolerance = *Tolerance;
   const int32 DefaultIndex = ChangedTolerance.ObjectKeys.IndexOfByKey(TEXT("DefaultValue"));
   if (!TestTrue(TEXT("disabled tolerance has its actual float default"), DefaultIndex != INDEX_NONE
    && ChangedTolerance.Elements[DefaultIndex].Kind == EAssetDocumentRuntimeValueKind::Number)) return false;
   ChangedTolerance.Elements[DefaultIndex].NumberValue = ChangedTolerance.Elements[DefaultIndex].NumberValue == 1.0 ? 2.0 : 1.0;
   TestFalse(TEXT("changed disabled tolerance remains rejected by stock editability"), UE::AssetDocumentAI::BehaviorTreeScratchAuthoring::WriteProperties(
    *Task, ObjectValue({TEXT("ObservedBlackboardValueTolerance")}, {ChangedTolerance}), Error));
   TestTrue(TEXT("changed disabled failure names the exact typed field"), Error.Contains(TEXT("ObservedBlackboardValueTolerance: disabled authored field differs")));
   TestFalse(TEXT("observe driver cannot be authored to enable the condition"), UE::AssetDocumentAI::BehaviorTreeScratchAuthoring::WriteProperties(
    *Task, ObjectValue({TEXT("bObserveBlackboardValue")}, {BooleanValue(true)}), Error));
   TestFalse(TEXT("disabled-field handling leaves the native observe driver false"), Task->bObserveBlackboardValue);
   FValue After;
   TArray<uint8> AfterBytes;
   TestTrue(TEXT("disabled no-op and rejected changes preserve every scratch authored value"), BuildAuthoredProperties(
    *Task, After, nullptr, EAuthoredGraphReadbackVersion::V2) && EncodeAuthoredCanonicalValue(After, AfterBytes) && AfterBytes == AuthoredBytes);
   AfterBytes.Reset();
   TestTrue(TEXT("MoveTo class defaults remain unchanged after scratch authoring"), BuildAuthoredProperties(
    *GetMutableDefault<UBTTask_MoveTo>(), After, nullptr, EAuthoredGraphReadbackVersion::V2)
    && EncodeAuthoredCanonicalValue(After, AfterBytes) && AfterBytes == MoveToDefaultBytes);
   TestEqual(TEXT("disabled-field scratch handling preserves the live package dirty state"), S.Package->IsDirty(), bTargetPackageDirty);
  }
  else
  {
   const auto* Wait = Props ? FindAssetDocumentAIBehaviorTreeField(*Props, TEXT("WaitTime")) : nullptr;
   const auto* Default = Wait ? FindAssetDocumentAIBehaviorTreeField(*Wait, TEXT("DefaultValue")) : nullptr;
   TestTrue(TEXT("typed WaitTime literal survived full readback"), Default && Default->NumberValue == 7.5);
  }
  if (bComposite)
  {
   const auto* Decorators = FindAssetDocumentAIBehaviorTreeField(*Root, TEXT("decorators"));
   if (!Decorators || Decorators->Elements.Num() != 1) return false;
   const auto* Bound = FindAssetDocumentAIBehaviorTreeField(Decorators->Elements[0], TEXT("bound_graph"));
   TestTrue(TEXT("bound graph full nodes/links/comments preserved"), Bound
    && FindAssetDocumentAIBehaviorTreeField(*Bound, TEXT("nodes"))->Elements.Num() == 5
    && FindAssetDocumentAIBehaviorTreeField(*Bound, TEXT("links"))->Elements.Num() == 4
    && FindAssetDocumentAIBehaviorTreeField(*Bound, TEXT("comments"))->Elements.Num() == 1);
  }
  TestTrue(TEXT("only scratch aggregate row ready"), S.Kernel->MarkReadyToCommit().bValid);
  TestTrue(TEXT("real inverse releases complete scratch"), S.Kernel->Rollback(TEXTVIEW("Test.ScratchAuthoringRollback")).bValid);
  TestEqual(TEXT("real kernel invalidates capability"), S.Resolver.GetInvalidationCount(), 1);
 }
 return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAIScratchAuthoringFailureTest,
 "UEAssetDoc.AIProvider.BehaviorTree.ScratchAuthoring.FailureConsumesCapability",
 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentAIScratchAuthoringFailureTest::RunTest(const FString&)
{
 using namespace AssetDocumentAIBehaviorTreeScratchAuthoringTests;
 FAssetDocumentAITestFixture Authority;
 if (!Authority.BootstrapBehaviorTreeEditorCache(*this) || !Authority.CloseBehaviorTreeEditorToolkit(*this)) return false;
 FScenario S;
 if (!S.Start(*this)) return false;
 auto Desired = Tree(false);
 auto& Root = Desired.Elements[Desired.ObjectKeys.IndexOfByKey(TEXT("root"))];
 auto& Properties = Root.Elements[Root.ObjectKeys.IndexOfByKey(TEXT("authored_properties"))];
 Properties = ObjectValue({TEXT("UnknownAuthoredField")}, {IntegerValue(1)});
 const auto Rejected = S.Author(MoveTemp(Desired));
 TestEqual(TEXT("unsupported typed property failure closes"), Rejected.Status, EAssetDocumentPrimitiveOutcomeStatus::Failed);
 TestTrue(TEXT("partial authored payload never returned"), Rejected.ChangedSet.IsEmpty() && Rejected.HandleResults.IsEmpty());
 const auto Retry = S.Author(Tree(false));
 TestEqual(TEXT("partial scratch capability cannot be reused"), Retry.Status, EAssetDocumentPrimitiveOutcomeStatus::Failed);
 TestTrue(TEXT("failed build still has complete scratch inverse"), S.Kernel->Rollback(TEXTVIEW("Test.ScratchFailureRollback")).bValid);
 return !HasAnyErrors();
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDocumentAIScratchContainersTest,
 "UEAssetDoc.AIProvider.BehaviorTree.ScratchAuthoring.FixedArrayAndStructSetLookup",
 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetDocumentAIScratchContainersTest::RunTest(const FString&)
{
 using namespace AssetDocumentAIBehaviorTreeScratchAuthoringTests;
 FAssetDocumentAITestFixture Authority;
 if (!Authority.BootstrapBehaviorTreeEditorCache(*this) || !Authority.CloseBehaviorTreeEditorToolkit(*this)) return false;
 for (const bool bWrongLength : {false, true})
 {
  FScenario S;
  if (!S.Start(*this)) return false;
  auto Desired = Tree(false, true);
  auto& Root = Desired.Elements[Desired.ObjectKeys.IndexOfByKey(TEXT("root"))];
  auto& TaskValue = Root.Elements[Root.ObjectKeys.IndexOfByKey(TEXT("children"))].Elements[0];
  auto Vector = [](double X, double Y, double Z)
  { return ObjectValue({TEXT("X"), TEXT("Y"), TEXT("Z")}, {NumberValue(X), NumberValue(Y), NumberValue(Z)}); };
  TaskValue.Elements[TaskValue.ObjectKeys.IndexOfByKey(TEXT("authored_properties"))] = ObjectValue(
   {TEXT("FixedValues"), TEXT("Vectors")},
   {bWrongLength ? ArrayValue({IntegerValue(4), IntegerValue(5)}) : ArrayValue({IntegerValue(4), IntegerValue(5), IntegerValue(6)}),
    ArrayValue({Vector(0, 0, 0), Vector(1, 2, 3), Vector(4, 5, 6)})});
  const auto Result = S.Author(MoveTemp(Desired));
  for (const auto& D : Result.Diagnostics) AddInfo(D.Code + TEXT(": ") + D.Message);
  if (bWrongLength)
  {
   TestEqual(TEXT("fixed array size mismatch rejects without a resize attempt"), Result.Status, EAssetDocumentPrimitiveOutcomeStatus::Failed);
  }
  else
  {
   if (!TestEqual(TEXT("fixed array and whole set-element writes succeed"), Result.Status, EAssetDocumentPrimitiveOutcomeStatus::Succeeded)) return false;
   auto I = S.Invocation(TEXT("ai.behavior_tree.graph.author_scratch.v1"), ObjectValue({}, {}));
   I.HandleArguments = {{TEXT("graph"), S.Graph}};
   FAssetDocumentAIBehaviorTreeGraphLease Native;
   TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
   if (!AcquireAssetDocumentAIBehaviorTreeGraphLeaseInternal(I, S.Resolver, TEXTVIEW("graph"), Native, Diagnostics)) return false;
   UAssetDocumentAIScratchNestedTask* Task = nullptr;
   for (UEdGraphNode* Node : Native.Graph->Nodes) if (auto* BT = Cast<UBehaviorTreeGraphNode>(Node))
    if (auto* Candidate = Cast<UAssetDocumentAIScratchNestedTask>(BT->NodeInstance)) Task = Candidate;
   if (!TestNotNull(TEXT("native authored task"), Task)) return false;
   TestTrue(TEXT("fixed array writes exact existing elements"), Task->FixedValues[0] == 4 && Task->FixedValues[1] == 5 && Task->FixedValues[2] == 6);
   TestEqual(TEXT("set contains exactly three values"), Task->Vectors.Num(), 3);
   for (const auto& Expected : {FVector::ZeroVector, FVector(1, 2, 3), FVector(4, 5, 6)})
   {
    TestTrue(TEXT("native TSet Contains uses correct hash"), Task->Vectors.Contains(Expected));
    const auto* Found = Task->Vectors.Find(Expected);
    TestTrue(TEXT("native TSet Find finds the complete authored vector"), Found && *Found == Expected);
   }
  }
  TestTrue(TEXT("real scratch rollback succeeds after either container result"), S.Kernel->Rollback(TEXTVIEW("Test.ScratchContainers")).bValid);
 }
 return !HasAnyErrors();
}
#endif
