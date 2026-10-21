// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "Provider/BehaviorTree/AssetDocumentAIBehaviorTreePrimitives.h"

#include "AssetDocumentEditorNativeState.h"
#include "BehaviorTree/AssetDocumentAIBehaviorTreeDerivedCompletion.h"
#include "BehaviorTree/AssetDocumentAIBehaviorTreeGraphInstall.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTreeGraph.h"
#include "Provider/AssetDocumentAIProviderContracts.h"
#include "Runtime/AssetDocumentAtomicTransaction.h"
#include "UObject/SoftObjectPath.h"

namespace AssetDocumentAIBehaviorTreeValueOrKeyRefresh
{
constexpr TCHAR Component[] = TEXT("assetdocument.ai.behavior_tree.derived_completion.v1");

FAssetDocumentPrimitiveOutcome Failure(const TCHAR* Code, const TCHAR* Message,
 TConstArrayView<FAssetDocumentRuntimeDiagnostic> Nested = {})
{
 auto Out = MakeAssetDocumentAIPrimitiveFailure(EAssetDocumentPrimitiveOutcomeStatus::Failed, Code, Message);
 if (Nested.IsEmpty()) return Out;
 const TArray<FString> Admitted = {TEXT("AIProvider.AuthorityUnavailable"), TEXT("AIProvider.OperationRejected"),
  TEXT("AIProvider.BehaviorTree.ValueOrInventoryRejected"),
  TEXT("AIProvider.BehaviorTree.ValueOrRegistrationRejected"),
  TEXT("AIProvider.BehaviorTree.ValueOrBlackboardMismatch"),
  TEXT("AIProvider.BehaviorTree.ValueOrKeyTypeMismatch"),
  TEXT("AIProvider.BehaviorTree.ValueOrInvalidationFailed"),
  TEXT("AIProvider.BehaviorTree.ValueOrCompletionMismatch"),
  TEXT("AIProvider.BehaviorTree.DecoratorInventoryRejected"),
  TEXT("AIProvider.BehaviorTree.DecoratorSelectorUnresolved"),
  TEXT("AIProvider.BehaviorTree.DecoratorOperationPropertyRejected"),
  TEXT("AIProvider.BehaviorTree.DecoratorNotificationFailed"),
  TEXT("AIProvider.BehaviorTree.DecoratorCompletionMismatch")};
 if (Nested.Num() == 1 && Admitted.Contains(Nested[0].Code))
 { Out.Diagnostics[0] = Nested[0]; return Out; }
 auto& Context = Out.Diagnostics[0].Context.AddDefaulted_GetRef();
 Context.Key = TEXT("nested_diagnostics"); Context.Value.Kind = EAssetDocumentRuntimeValueKind::Array;
 for (const auto& Item : Nested)
 {
  auto& Value = Context.Value.Elements.AddDefaulted_GetRef();
  Value.Kind = EAssetDocumentRuntimeValueKind::String;
  Value.StringValue = Item.Code + TEXT(": ") + Item.Message;
 }
 return Out;
}

bool IsPath(const FAssetDocumentRuntimeValueDto& Value)
{
 if (Value.Kind != EAssetDocumentRuntimeValueKind::String || Value.StringValue.IsEmpty()) return false;
 const FSoftObjectPath Path(Value.StringValue);
 return Path.IsValid() && Path.GetSubPathString().IsEmpty() && Path.ToString() == Value.StringValue;
}

bool ParseRequest(const FAssetDocumentRuntimeValueDto& Value, bool Nullable)
{
 if (!IsAssetDocumentAICanonicalRuntimeValue(Value)
  || Value.Kind != EAssetDocumentRuntimeValueKind::Object
  || Value.ObjectKeys != TArray<FString>{TEXT("behavior_tree_path"), TEXT("completion_registration"),
   TEXT("expected_authored_inventory_digest"), TEXT("expected_blackboard_path")}
  || Value.Elements.Num() != 4 || !IsPath(Value.Elements[0])
  || (!IsPath(Value.Elements[3]) && !(Nullable && Value.Elements[3].Kind == EAssetDocumentRuntimeValueKind::Null))) return false;
 const auto& Registration = Value.Elements[1];
 const auto& Digest = Value.Elements[2];
 if (Registration.Kind != EAssetDocumentRuntimeValueKind::String
  || (Registration.StringValue != TEXT("current_graph") && Registration.StringValue != TEXT("installed_replacement"))
  || Digest.Kind != EAssetDocumentRuntimeValueKind::String || Digest.StringValue.Len() != 64) return false;
 for (TCHAR Character : Digest.StringValue)
  if (!(Character >= TEXT('0') && Character <= TEXT('9'))
   && !(Character >= TEXT('a') && Character <= TEXT('f'))) return false;
 return true;
}

FAssetDocumentRuntimeValueDto String(const FString& Value)
{
 FAssetDocumentRuntimeValueDto Out;
 Out.Kind = EAssetDocumentRuntimeValueKind::String; Out.StringValue = Value; return Out;
}

FAssetDocumentRuntimeValueDto Integer(int32 Value)
{
 FAssetDocumentRuntimeValueDto Out;
 Out.Kind = EAssetDocumentRuntimeValueKind::Integer; Out.IntegerValue = Value; return Out;
}

FAssetDocumentPrimitiveOutcome Complete(UBehaviorTree& Tree, UBehaviorTreeGraph& Graph,
 const FAssetDocumentPrimitiveInvocation& Invocation, IAssetDocumentAtomicTransaction& Transaction)
{
 const auto& Values = Invocation.ValueArguments.Elements;
 const bool Installed=Values[1].StringValue == TEXT("installed_replacement");
 if (Installed)
 {
  const auto Ownership=ValidateInstalledGraphOwnership(Tree);
  if (!Ownership.bValid) return Failure(TEXT("AIProvider.BehaviorTree.ValueOrInventoryRejected"),TEXT("Installed graph ownership is invalid."),Ownership.Diagnostics);
 }
 FAssetDocumentMutationFootprint Footprint;
 Footprint.Kind = EAssetDocumentMutationFootprintKind::UObjectState;
 Footprint.RollbackComponentId = Component;
 Footprint.StableId = FString::Printf(TEXT("asset:%s/DerivedCompletion"), *Values[0].StringValue);
 const auto Result = RunAssetDocumentBehaviorTreeDerivedCompletion(Tree, Graph,
  EAssetDocumentAIBehaviorTreeCompletionMode::ValueOrDecoratorMirror, Footprint, Transaction,
  Values[3].StringValue, Values[2].StringValue,
  Installed ? EAssetDocumentAIBehaviorTreeCompletionLifecycle::InstalledReplacement
   : EAssetDocumentAIBehaviorTreeCompletionLifecycle::CurrentGraph,
  Invocation.Version >= 2 ? UE::AssetDocumentAI::BehaviorTreeAuthoredReadback::EAuthoredGraphReadbackVersion::V2
   : UE::AssetDocumentAI::BehaviorTreeAuthoredReadback::EAuthoredGraphReadbackVersion::V1,
  Invocation.Version == 3);
 if (!Result.bSucceeded)
 {
  // OperationRejected retains the exact Atomic/typed-preimage rejection as nested evidence.
  return Failure(TEXT("AIProvider.OperationRejected"), TEXT("CurrentGraph derived completion was rejected."), Result.Diagnostics);
 }
 if (Installed)
 {
  const auto Ownership=ValidateInstalledGraphOwnership(Tree);
  if (!Ownership.bValid) return Failure(TEXT("AIProvider.BehaviorTree.ValueOrCompletionMismatch"),TEXT("Installed graph ownership changed during completion."),Ownership.Diagnostics);
 }
 FAssetDocumentRuntimeValueDto Counts;
 Counts.Kind = EAssetDocumentRuntimeValueKind::Object;
 Counts.ObjectKeys = {TEXT("Bool"), TEXT("Class"), TEXT("Enum"), TEXT("Float"), TEXT("Int32"), TEXT("Name"),
  TEXT("Object"), TEXT("Rotator"), TEXT("String"), TEXT("Struct"), TEXT("Vector")};
 int64 Total = 0;
 for (const FString& Family : Counts.ObjectKeys)
 {
  const int32* FoundCount = Result.ValueOrFamilyCounts.Find(Family);
  if (!FoundCount || *FoundCount < 0)
   return Failure(TEXT("AIProvider.BehaviorTree.ValueOrCompletionMismatch"), TEXT("Completion returned a missing or negative family count."));
  Total += *FoundCount;
  Counts.Elements.Add(Integer(*FoundCount));
 }
 if (Total != Result.ValueOrLeafCount || Result.ValueOrFamilyCounts.Num() != Counts.ObjectKeys.Num())
  return Failure(TEXT("AIProvider.BehaviorTree.ValueOrCompletionMismatch"), TEXT("Completion returned an incomplete family inventory."));
 FAssetDocumentPrimitiveOutcome Out;
 Out.Status = EAssetDocumentPrimitiveOutcomeStatus::Succeeded;
 Out.Value.Kind = EAssetDocumentRuntimeValueKind::Object;
 Out.Value.ObjectKeys = {TEXT("authored_inventory_digest"), TEXT("family_counts"),
  TEXT("leaf_count"), TEXT("resolved_mapping_digest")};
 Out.Value.Elements = {String(Result.AuthoredInventoryDigest), MoveTemp(Counts),
  Integer(Result.ValueOrLeafCount), String(Result.ResolvedMappingDigest)};
 return Out;
}
}

TOptional<FAssetDocumentPrimitiveOutcome> RejectAssetDocumentAIBehaviorTreeValueOrKeyInvocation(
 const FAssetDocumentPrimitiveInvocation& Invocation,
 IAssetDocumentAtomicTransaction* ActiveTransaction)
{
 using namespace AssetDocumentAIBehaviorTreeValueOrKeyRefresh;
 if (!((Invocation.PrimitiveId == TEXT("ai.behavior_tree.value_or_key.refresh.v1") && Invocation.Version == 1)
  || (Invocation.PrimitiveId == TEXT("ai.behavior_tree.value_or_key.refresh.v2") && Invocation.Version == 2)
  || (Invocation.PrimitiveId == TEXT("ai.behavior_tree.value_or_key.refresh.v3") && Invocation.Version == 3)))
  return Failure(TEXT("AIProvider.InvalidContext"), TEXT("ValueOr refresh requires its exact versioned entry."));
 if (Invocation.Phase != EAssetDocumentRuntimeRequestPhase::Repair || !IsInGameThread()
  || !GIsEditor || IsRunningCommandlet())
  return Failure(TEXT("AIProvider.InvalidContext"), TEXT("ValueOr refresh requires the normal Editor Repair context."));
 if (!ParseRequest(Invocation.ValueArguments, Invocation.Version == 3))
  return Failure(TEXT("AIProvider.InvalidValueShape"), TEXT("The ValueOr request must contain exactly its canonical four fields."));
 if (!IsAssetDocumentAITransactionValid(Invocation, ActiveTransaction))
  return Failure(TEXT("AIProvider.InvalidTransaction"), TEXT("ValueOr refresh requires the matching active Atomic transaction."));
 if (Invocation.HandleArguments.Num() != 1 || Invocation.HandleArguments[0].Name != TEXT("asset"))
  return Failure(TEXT("AIProvider.InvalidHandle"), TEXT("ValueOr refresh requires exactly one mutable Asset lease."));
 return {};
}

FAssetDocumentPrimitiveOutcome HandleAssetDocumentAIBehaviorTreeValueOrKeyRefresh(
 const FAssetDocumentPrimitiveInvocation& Invocation,
 IAssetDocumentHandleResolver& Handles,
 IAssetDocumentAtomicTransaction* ActiveTransaction)
{
 using namespace AssetDocumentAIBehaviorTreeValueOrKeyRefresh;
 if (auto Rejected = RejectAssetDocumentAIBehaviorTreeValueOrKeyInvocation(Invocation, ActiveTransaction))
  return MoveTemp(Rejected.GetValue());
 auto Lease = AcquireAssetDocumentAIMutableAssetLease(Handles, Invocation);
 if (!Lease.Lease)
  return Failure(TEXT("AIProvider.InvalidHandle"), TEXT("The mutable Asset lease is invalid."));
 FAssetDocumentPrimitiveOutcome Outcome;
 const auto Identity = WithAssetDocumentEditorAssetIdentity(*Lease.Lease,
  [&](const FAssetDocumentEditorAssetIdentityView& View)
  {
   auto* Tree = Cast<UBehaviorTree>(&View.GetObject());
   auto* Graph = Tree ? Cast<UBehaviorTreeGraph>(Tree->BTGraph) : nullptr;
   if (!Tree || Tree->GetClass() != UBehaviorTree::StaticClass()
    || View.GetCanonicalObjectPath() != Invocation.ValueArguments.Elements[0].StringValue
    || Tree->GetPathName() != View.GetCanonicalObjectPath()
    || static_cast<uint64>(Tree->GetUniqueID()) != View.GetTypeRevision()
    || !Graph || Graph->GetClass() != UBehaviorTreeGraph::StaticClass()
    || Graph->GetOuter() != Tree || !Graph->GraphGuid.IsValid() || Graph->IsLocked())
   {
    Outcome = Failure(TEXT("AIProvider.InvalidHandle"), TEXT("The Asset must own its exact unlocked live BehaviorTree graph."));
    return FAssetDocumentContractValidationResult{};
   }
   Outcome = Complete(*Tree, *Graph, Invocation, *ActiveTransaction);
   FAssetDocumentContractValidationResult Valid;
   Valid.bValid = Outcome.Status == EAssetDocumentPrimitiveOutcomeStatus::Succeeded;
   return Valid;
  });
 if (!Identity.bValid)
  return Outcome.Diagnostics.IsEmpty() ? Failure(TEXT("AIProvider.InvalidHandle"), TEXT("The Asset identity is no longer live."), Identity.Diagnostics) : Outcome;
 FAssetDocumentHandleInvalidation Event;
 Event.Reason = EAssetDocumentHandleInvalidationReason::Rebuild;
 Event.RequestId = Invocation.RequestId; Event.TransactionId = Invocation.TransactionId;
 Event.RuntimeRevision = Invocation.RuntimeRevision; Event.ProviderId = TEXT("ai");
 if (Handles.Invalidate(Event).InvalidatedHandleCount == 0)
  return Failure(TEXT("AIProvider.BehaviorTree.ValueOrCompletionMismatch"), TEXT("Completed graph handles could not be invalidated."));
 return Outcome;
}
