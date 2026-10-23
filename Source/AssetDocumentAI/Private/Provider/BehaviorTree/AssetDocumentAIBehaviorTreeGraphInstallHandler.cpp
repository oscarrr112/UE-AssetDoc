// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
#include "Provider/BehaviorTree/AssetDocumentAIBehaviorTreePrimitives.h"
#include "Provider/BehaviorTree/AssetDocumentAIBehaviorTreeGraphBundleInternal.h"
#include "BehaviorTree/AssetDocumentAIBehaviorTreeGraphInstall.h"
#include "Provider/AssetDocumentAIProviderContracts.h"
#include "AssetDocumentEditorNativeState.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTreeGraph.h"

namespace AssetDocumentAIBehaviorTreeGraphInstallHandler
{
FAssetDocumentPrimitiveOutcome Failure(const TCHAR* Code, const TCHAR* Message)
{
 return MakeAssetDocumentAIPrimitiveFailure(EAssetDocumentPrimitiveOutcomeStatus::Failed, Code, Message);
}
bool Parse(const FAssetDocumentRuntimeValueDto& V)
{
 if (!IsAssetDocumentAICanonicalRuntimeValue(V) || V.Kind != EAssetDocumentRuntimeValueKind::Object
  || V.ObjectKeys != TArray<FString>{TEXT("behavior_tree_path"), TEXT("expected_current_graph_guid_or_null"), TEXT("rollback_completion_mode")}
  || V.Elements.Num() != 3 || !IsAssetDocumentAIBehaviorTreeCanonicalObjectPath(&V.Elements[0])
  || V.Elements[2].Kind != EAssetDocumentRuntimeValueKind::String
  || V.Elements[2].StringValue != TEXT("ValueOrDecoratorMirror")) return false;
 if (V.Elements[1].Kind == EAssetDocumentRuntimeValueKind::Null) return true;
 FGuid Guid;
 return V.Elements[1].Kind == EAssetDocumentRuntimeValueKind::String
  && FGuid::ParseExact(V.Elements[1].StringValue, EGuidFormats::Digits, Guid) && Guid.IsValid()
  && Guid.ToString(EGuidFormats::Digits) == V.Elements[1].StringValue;
}
}

TOptional<FAssetDocumentPrimitiveOutcome> RejectAssetDocumentAIBehaviorTreeGraphInstallInvocation(
 const FAssetDocumentPrimitiveInvocation& Invocation, IAssetDocumentAtomicTransaction* ActiveTransaction)
{
 using namespace AssetDocumentAIBehaviorTreeGraphInstallHandler;
 if (Invocation.Phase != EAssetDocumentRuntimeRequestPhase::Apply || !IsAssetDocumentAIBehaviorTreeScratchContext())
  return Failure(TEXT("AIProvider.InvalidContext"), TEXT("Graph installation requires the normal Editor Apply game thread."));
 if (!Parse(Invocation.ValueArguments))
  return Failure(TEXT("AIProvider.InvalidValueShape"), TEXT("Graph installation requires its exact canonical three fields."));
 if (!IsAssetDocumentAITransactionValid(Invocation, ActiveTransaction))
  return Failure(TEXT("AIProvider.InvalidTransaction"), TEXT("Graph installation requires the matching active Atomic transaction."));
 if (Invocation.HandleArguments.Num() != 2)
  return Failure(TEXT("AIProvider.InvalidHandle"), TEXT("Graph installation requires mutable asset and scratch graph handles."));
 bool Asset = false, Graph = false;
 for (const auto& Argument : Invocation.HandleArguments)
 {
  if (Argument.Name == TEXT("asset") && !Asset) Asset = true;
  else if (Argument.Name == TEXT("graph") && !Graph) Graph = true;
  else return Failure(TEXT("AIProvider.InvalidHandle"), TEXT("Graph installation handle names must be unique and exact."));
 }
 if (!Asset || !Graph) return Failure(TEXT("AIProvider.InvalidHandle"), TEXT("Graph installation is missing a required handle."));
 return {};
}

FAssetDocumentPrimitiveOutcome HandleAssetDocumentAIBehaviorTreeGraphInstall(
 const FAssetDocumentPrimitiveInvocation& Invocation, IAssetDocumentHandleResolver& Handles,
 IAssetDocumentAtomicTransaction* ActiveTransaction)
{
 using namespace AssetDocumentAIBehaviorTreeGraphInstallHandler;
 if (auto Rejected = RejectAssetDocumentAIBehaviorTreeGraphInstallInvocation(Invocation, ActiveTransaction)) return MoveTemp(Rejected.GetValue());
 auto AssetLease = AcquireAssetDocumentAIMutableAssetLease(Handles, Invocation);
 FAssetDocumentAIBehaviorTreeGraphLease GraphLease;
 TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
 if (!AssetLease.Lease || !AcquireAssetDocumentAIBehaviorTreeGraphLeaseInternal(Invocation, Handles, TEXTVIEW("graph"), GraphLease, Diagnostics))
  return Failure(TEXT("AIProvider.InvalidHandle"), TEXT("The target or unconsumed scratch graph lease is invalid."));
 FAssetDocumentPrimitiveOutcome Outcome;
 const auto Identity = WithAssetDocumentEditorAssetIdentity(*AssetLease.Lease,
  [&](const FAssetDocumentEditorAssetIdentityView& View)
  {
   auto* Tree = Cast<UBehaviorTree>(&View.GetObject());
   const FString& Path = Invocation.ValueArguments.Elements[0].StringValue;
   if (!Tree || Tree->GetClass() != UBehaviorTree::StaticClass() || Tree->GetPathName() != Path
    || View.GetCanonicalObjectPath() != Path || static_cast<uint64>(Tree->GetUniqueID()) != View.GetTypeRevision()
    || GraphLease.LiveBehaviorTreePath != Path || Tree == GraphLease.State->Tree.Get())
   { Outcome = Failure(TEXT("AIProvider.InvalidHandle"), TEXT("The provided leases do not bind this exact target and scratch bundle.")); return FAssetDocumentContractValidationResult{}; }
   auto* Old = Cast<UBehaviorTreeGraph>(Tree->BTGraph);
   const auto& Expected = Invocation.ValueArguments.Elements[1];
   if ((Tree->BTGraph && (!Old || Old->GetClass() != UBehaviorTreeGraph::StaticClass() || Old->GetOuter() != Tree || Old->IsLocked()))
    || (Old && (Expected.Kind != EAssetDocumentRuntimeValueKind::String || Old->GraphGuid.ToString(EGuidFormats::Digits) != Expected.StringValue))
    || (!Old && Expected.Kind != EAssetDocumentRuntimeValueKind::Null) || GraphLease.Graph->IsLocked())
   { Outcome = Failure(TEXT("AIProvider.BehaviorTree.GraphInstallPreimageMismatch"), TEXT("The expected old graph does not match the exact live target.")); return FAssetDocumentContractValidationResult{}; }
   Outcome = UE::AssetDocumentAI::BehaviorTreeGraphInstall::InstallExactGraph(*Tree, GraphLease.State.ToSharedRef(), Handles, *ActiveTransaction);
   FAssetDocumentContractValidationResult R; R.bValid = Outcome.Status == EAssetDocumentPrimitiveOutcomeStatus::Succeeded; return R;
  });
 if (!Identity.bValid && Outcome.Diagnostics.IsEmpty()) return Failure(TEXT("AIProvider.InvalidHandle"), TEXT("The target asset identity is no longer live."));
 return Outcome;
}
