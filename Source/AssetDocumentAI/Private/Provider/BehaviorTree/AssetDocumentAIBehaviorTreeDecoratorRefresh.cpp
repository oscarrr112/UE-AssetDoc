// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "Provider/BehaviorTree/AssetDocumentAIBehaviorTreePrimitives.h"

#include "AssetDocumentEditorNativeState.h"
#include "BehaviorTree/AssetDocumentAIBehaviorTreeDerivedCompletion.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTreeGraph.h"
#include "Provider/AssetDocumentAIProviderContracts.h"
#include "Runtime/AssetDocumentAtomicTransaction.h"
#include "UObject/SoftObjectPath.h"

namespace AssetDocumentAIBehaviorTreeDecoratorRefresh
{
constexpr TCHAR Component[] = TEXT("assetdocument.ai.behavior_tree.derived_completion.v1");

FAssetDocumentPrimitiveOutcome Failure(const TCHAR* Code, const TCHAR* Message,
 TConstArrayView<FAssetDocumentRuntimeDiagnostic> Nested = {})
{
 auto Out = MakeAssetDocumentAIPrimitiveFailure(EAssetDocumentPrimitiveOutcomeStatus::Failed, Code, Message);
 if (Nested.IsEmpty()) return Out;
 const TArray<FString> Admitted = {TEXT("AIProvider.OperationRejected"),
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

bool ParseRequest(const FAssetDocumentRuntimeValueDto& Value)
{
 if (!IsAssetDocumentAICanonicalRuntimeValue(Value)
  || Value.ObjectKeys != TArray<FString>{TEXT("behavior_tree_path"),
   TEXT("expected_authored_decorator_digest"), TEXT("expected_blackboard_path")}
  || Value.Elements.Num() != 3 || !IsPath(Value.Elements[0]) || !IsPath(Value.Elements[2])) return false;
 const auto& Digest = Value.Elements[1];
 if (Digest.Kind != EAssetDocumentRuntimeValueKind::String || Digest.StringValue.IsEmpty()) return false;
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
 FAssetDocumentMutationFootprint Footprint;
 Footprint.Kind = EAssetDocumentMutationFootprintKind::UObjectState;
 Footprint.RollbackComponentId = Component;
 Footprint.StableId = FString::Printf(TEXT("asset:%s/DerivedCompletion"), *Values[0].StringValue);
 const auto Result = RunAssetDocumentBehaviorTreeDerivedCompletion(Tree, Graph,
  EAssetDocumentAIBehaviorTreeCompletionMode::DecoratorAndMirror, Footprint, Transaction,
  Values[2].StringValue, Values[1].StringValue);
 if (!Result.bSucceeded)
 {
  // OperationRejected retains the exact Atomic/typed-preimage rejection as nested evidence.
  return Failure(TEXT("AIProvider.OperationRejected"), TEXT("CurrentGraph derived completion was rejected."), Result.Diagnostics);
 }
 FAssetDocumentPrimitiveOutcome Out;
 Out.Status = EAssetDocumentPrimitiveOutcomeStatus::Succeeded;
 Out.Value.Kind = EAssetDocumentRuntimeValueKind::Object;
 Out.Value.ObjectKeys = {TEXT("authored_decorator_digest"), TEXT("bound_decorator_count"),
  TEXT("decorator_count"), TEXT("derived_semantic_digest")};
 Out.Value.Elements = {String(Result.AuthoredDecoratorDigest), Integer(Result.BoundDecoratorCount),
  Integer(Result.DecoratorCount), String(Result.DerivedSemanticDigest)};
 auto& Change = Out.ChangedSet.AddDefaulted_GetRef();
 Change.AuthorityId = Component; Change.StableObjectId = Values[0].StringValue;
 Change.Path = Footprint.StableId; Change.ChangeKind = TEXT("Update");
 return Out;
}
}

FAssetDocumentPrimitiveOutcome HandleAssetDocumentAIBehaviorTreeDecoratorRefresh(
 const FAssetDocumentPrimitiveInvocation& Invocation,
 IAssetDocumentHandleResolver& Handles,
 IAssetDocumentAtomicTransaction* ActiveTransaction)
{
 using namespace AssetDocumentAIBehaviorTreeDecoratorRefresh;
 if (Invocation.Phase != EAssetDocumentRuntimeRequestPhase::Repair || !IsInGameThread()
  || !GIsEditor || IsRunningCommandlet())
  return Failure(TEXT("AIProvider.InvalidContext"), TEXT("Decorator refresh requires the normal Editor Repair context."));
 if (!ParseRequest(Invocation.ValueArguments))
  return Failure(TEXT("AIProvider.InvalidValueShape"), TEXT("The decorator request must contain exactly its canonical three fields."));
 if (!IsAssetDocumentAITransactionValid(Invocation, ActiveTransaction))
  return Failure(TEXT("AIProvider.InvalidTransaction"), TEXT("Decorator refresh requires the matching active Atomic transaction."));
 if (Invocation.HandleArguments.Num() != 1 || Invocation.HandleArguments[0].Name != TEXT("asset"))
  return Failure(TEXT("AIProvider.InvalidHandle"), TEXT("Decorator refresh requires exactly one mutable Asset lease."));
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
  return Failure(TEXT("AIProvider.BehaviorTree.DecoratorCompletionMismatch"), TEXT("Completed graph handles could not be invalidated."));
 return Outcome;
}
