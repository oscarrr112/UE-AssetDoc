// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
#include "Provider/BehaviorTree/AssetDocumentAIBehaviorTreePrimitives.h"
#include "Provider/AssetDocumentAIProviderContracts.h"
#include "AssetDocumentEditorNativeState.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BlackboardData.h"
#include "Runtime/AssetDocumentAtomicTransaction.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"

FAssetDocumentPrimitiveOutcome HandleAssetDocumentAIBehaviorTreeBlackboardReadback(
 const FAssetDocumentPrimitiveInvocation& Invocation, IAssetDocumentHandleResolver& Handles,
 IAssetDocumentAtomicTransaction* ActiveTransaction)
{
 auto Fail=[](const TCHAR* Code,const TCHAR* Message)
 { return MakeAssetDocumentAIPrimitiveFailure(EAssetDocumentPrimitiveOutcomeStatus::Failed,Code,Message); };
 const bool Mutation=Invocation.Phase==EAssetDocumentRuntimeRequestPhase::Apply
  || Invocation.Phase==EAssetDocumentRuntimeRequestPhase::Repair;
 if (!IsInGameThread() || !GIsEditor || IsRunningCommandlet()
  || (!Mutation && Invocation.Phase!=EAssetDocumentRuntimeRequestPhase::Validate
   && Invocation.Phase!=EAssetDocumentRuntimeRequestPhase::Preflight
   && Invocation.Phase!=EAssetDocumentRuntimeRequestPhase::Verify))
  return Fail(TEXT("AIProvider.InvalidContext"),TEXT("Blackboard readback requires its admitted Editor phase."));
 const auto& Value=Invocation.ValueArguments;
 if (Invocation.PrimitiveId!=TEXT("ai.behavior_tree.blackboard.readback.v1") || Invocation.Version!=1
  || !IsAssetDocumentAICanonicalRuntimeValue(Value) || Value.Kind!=EAssetDocumentRuntimeValueKind::Object
  || Value.ObjectKeys!=TArray<FString>{TEXT("behavior_tree_path")} || Value.Elements.Num()!=1
  || Value.Elements[0].Kind!=EAssetDocumentRuntimeValueKind::String || Value.Elements[0].StringValue.IsEmpty())
  return Fail(TEXT("AIProvider.InvalidValueShape"),TEXT("Blackboard readback requires exactly behavior_tree_path."));
 const FString& Path=Value.Elements[0].StringValue;
 const FSoftObjectPath Requested(Path);
 if (!Requested.IsValid() || !Requested.GetSubPathString().IsEmpty() || Requested.ToString()!=Path)
  return Fail(TEXT("AIProvider.InvalidValueShape"),TEXT("The BehaviorTree path is not canonical."));
 if ((Mutation && !IsAssetDocumentAITransactionValid(Invocation,ActiveTransaction)) || (!Mutation && ActiveTransaction))
  return Fail(TEXT("AIProvider.InvalidTransaction"),TEXT("The readback transaction must match its phase."));
 if (Invocation.HandleArguments.Num()!=1 || Invocation.HandleArguments[0].Name!=TEXT("asset"))
  return Fail(TEXT("AIProvider.InvalidHandle"),TEXT("Blackboard readback requires one read-only Asset lease."));
 FAssetDocumentHandleValidationContext Context;
 Context.RequestId=Invocation.RequestId; Context.TransactionId=Invocation.TransactionId;
 Context.RuntimeRevision=Invocation.RuntimeRevision; Context.ProviderId=TEXT("ai"); Context.Phase=Invocation.Phase;
 Context.RequiredKind=EAssetDocumentHandleKind::Asset; Context.RequiredAccess=EAssetDocumentHandleAccess::ReadOnly;
 auto Lease=Handles.AcquireVerifiedLease(Invocation.HandleArguments[0].Handle,Context);
 if (!Lease.Lease) return Fail(TEXT("AIProvider.InvalidHandle"),TEXT("The AI read-only Asset lease is invalid."));
 FAssetDocumentPrimitiveOutcome Outcome;
 const auto Identity=WithAssetDocumentEditorAssetIdentity(*Lease.Lease,[&](const FAssetDocumentEditorAssetIdentityView& View)
 {
  FAssetDocumentContractValidationResult Result;
  const auto* Tree=Cast<UBehaviorTree>(&View.GetObject());
  if (!Tree || Tree->GetClass()!=UBehaviorTree::StaticClass() || View.GetCanonicalObjectPath()!=Path
   || Tree->GetPathName()!=Path || Tree->GetOuter()!=Tree->GetPackage())
  { Outcome=Fail(TEXT("AIProvider.InvalidHandle"),TEXT("The Asset lease does not bind the exact requested BehaviorTree.")); return Result; }
  const UBlackboardData* Blackboard=Tree->GetBlackboardAsset();
  FAssetDocumentRuntimeValueDto Header; Header.Kind=EAssetDocumentRuntimeValueKind::Null;
  if (Blackboard)
  {
   const FSoftObjectPath BlackboardPath(Blackboard);
   if (!IsValid(Blackboard) || Blackboard->GetOuter()!=Blackboard->GetPackage() || !BlackboardPath.IsValid()
    || !BlackboardPath.GetSubPathString().IsEmpty() || BlackboardPath.ToString()!=Blackboard->GetPathName())
   { Outcome=Fail(TEXT("AIProvider.BehaviorTree.BlackboardMismatch"),TEXT("The current Blackboard is not an exact canonical asset.")); return Result; }
   Header.Kind=EAssetDocumentRuntimeValueKind::String; Header.StringValue=BlackboardPath.ToString();
  }
  Outcome.Value.Kind=EAssetDocumentRuntimeValueKind::Object;
  Outcome.Value.ObjectKeys={TEXT("blackboard_path_or_null")}; Outcome.Value.Elements={MoveTemp(Header)};
  Outcome.Status=EAssetDocumentPrimitiveOutcomeStatus::Succeeded; Result.bValid=true; return Result;
 });
 return Identity.bValid || !Outcome.Diagnostics.IsEmpty()?Outcome:
  Fail(TEXT("AIProvider.InvalidHandle"),TEXT("The Asset lease could not enter Editor identity."));
}
