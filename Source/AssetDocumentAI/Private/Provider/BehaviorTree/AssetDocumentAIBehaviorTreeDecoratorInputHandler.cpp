// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
#include "BehaviorTree/AssetDocumentAIBehaviorTreeDecoratorInput.h"
#include "Provider/BehaviorTree/AssetDocumentAIBehaviorTreeGraphBundleInternal.h"
#include "Provider/AssetDocumentAIProviderContracts.h"

namespace
{
bool InputGuid(const FAssetDocumentRuntimeValueDto& Value, FGuid& Guid)
{
 return Value.Kind == EAssetDocumentRuntimeValueKind::String
  && FGuid::ParseExact(Value.StringValue, EGuidFormats::Digits, Guid) && Guid.IsValid()
  && Guid.ToString(EGuidFormats::Digits) == Value.StringValue;
}
FAssetDocumentPrimitiveOutcome InputFailure(const TCHAR* Code, const TCHAR* Message)
{
 return MakeAssetDocumentAIBehaviorTreeGraphFailure(Code, Message);
}
}
FAssetDocumentPrimitiveOutcome HandleAssetDocumentAIBehaviorTreeDecoratorEnsureInputCount(
 const FAssetDocumentPrimitiveInvocation& Invocation, IAssetDocumentHandleResolver& Handles,
 IAssetDocumentAtomicTransaction* ActiveTransaction)
{
 if (Invocation.Phase != EAssetDocumentRuntimeRequestPhase::Apply || !IsAssetDocumentAIBehaviorTreeScratchContext())
  return InputFailure(TEXT("AIProvider.InvalidContext"), TEXT("Decorator cardinality requires the normal Editor Apply game thread."));
 const auto& V = Invocation.ValueArguments;
 FGuid Composite, Bound, Logic;
 if (!IsAssetDocumentAICanonicalRuntimeValue(V)
  || V.ObjectKeys != TArray<FString>{TEXT("bound_graph_guid"), TEXT("composite_stable_node_id"), TEXT("exact_input_count"), TEXT("logic_kind"), TEXT("logic_stable_node_id")}
  || V.Kind != EAssetDocumentRuntimeValueKind::Object || V.Elements.Num() != 5
  || !InputGuid(V.Elements[0], Bound) || !InputGuid(V.Elements[1], Composite) || !InputGuid(V.Elements[4], Logic)
  || V.Elements[2].Kind != EAssetDocumentRuntimeValueKind::Integer
  || V.Elements[2].IntegerValue < 1 || V.Elements[2].IntegerValue > 256
  || V.Elements[3].Kind != EAssetDocumentRuntimeValueKind::String
  || (V.Elements[3].StringValue != TEXT("And") && V.Elements[3].StringValue != TEXT("Or")
   && V.Elements[3].StringValue != TEXT("Not") && V.Elements[3].StringValue != TEXT("Sink")))
  return InputFailure(TEXT("AIProvider.InvalidValueShape"), TEXT("Decorator cardinality requires its exact canonical fields and bounded input count."));
 if (!IsAssetDocumentAITransactionValid(Invocation, ActiveTransaction))
  return InputFailure(TEXT("AIProvider.InvalidTransaction"), TEXT("Decorator cardinality requires the matching Atomic transaction."));
 FAssetDocumentAIBehaviorTreeGraphLease Lease; TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
 if (Invocation.HandleArguments.Num() != 1 || Invocation.HandleArguments[0].Name != TEXT("graph")
  || !AcquireAssetDocumentAIBehaviorTreeGraphLeaseInternal(Invocation, Handles, TEXTVIEW("graph"), Lease, Diagnostics))
  return InputFailure(TEXT("AIProvider.InvalidHandle"), TEXT("An exact unconsumed scratch graph capability is required."));
 return UE::AssetDocumentAI::BehaviorTreeDecoratorInput::EnsureInputCount(Lease, Composite, Bound, Logic,
  V.Elements[3].StringValue, static_cast<int32>(V.Elements[2].IntegerValue), *ActiveTransaction);
}
