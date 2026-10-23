// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
#include "AssetDocumentStagedBehaviorTreeReadBinding.h"
#include "AssetDocumentEditorStagedBehaviorTreeRead.h"
#include "Runtime/AssetDocumentHandleRegistry.h"

namespace
{
class FBTStagedReadHandles final : public IAssetDocumentHandleResolver
{
public:
 explicit FBTStagedReadHandles(IAssetDocumentHandleResolver& InInner) : Inner(InInner) {}
 FAssetDocumentIssuedHandleResult Issue(EAssetDocumentHandleKind Kind, EAssetDocumentHandleAccess Access,
  TUniquePtr<IAssetDocumentNativeHandlePayload>&& Payload) override
 {
  return Kind == EAssetDocumentHandleKind::Asset && Access == EAssetDocumentHandleAccess::ReadOnly
   ? Inner.Issue(Kind, Access, MoveTemp(Payload)) : FAssetDocumentIssuedHandleResult{};
 }
 FAssetDocumentVerifiedLeaseResult AcquireVerifiedLease(const FAssetDocumentOpaqueHandle& Handle,
  const FAssetDocumentHandleValidationContext& Context) override
 {
  return Context.RequiredKind == EAssetDocumentHandleKind::Asset && Context.RequiredAccess == EAssetDocumentHandleAccess::ReadOnly
   ? Inner.AcquireVerifiedLease(Handle, Context) : FAssetDocumentVerifiedLeaseResult{};
 }
 FAssetDocumentHandleInvalidationResult Invalidate(const FAssetDocumentHandleInvalidation& Event) override
 { return Inner.Invalidate(Event); }
private:
 IAssetDocumentHandleResolver& Inner;
};
FAssetDocumentPrimitiveOutcome BTStagedResolveFailure()
{
 FAssetDocumentPrimitiveOutcome Outcome;
 auto& D = Outcome.Diagnostics.AddDefaulted_GetRef();
 D.Path = TEXT("/GenericRuntime/FreshVerification"); D.Code = TEXT("GenericRuntime.BehaviorTreeStagedResolveMismatch");
 D.Message = TEXT("Staged resolution requires the bound request's exact readonly Verify target and revision.");
 return Outcome;
}
}

FAssetDocumentStagedBehaviorTreeReadBinding::FAssetDocumentStagedBehaviorTreeReadBinding(TSharedRef<IAssetDocumentPrimitiveProvider> InInner)
 : Inner(MoveTemp(InInner)) {}
FAssetDocumentStagedBehaviorTreeReadBinding::~FAssetDocumentStagedBehaviorTreeReadBinding() { Close(); }
FString FAssetDocumentStagedBehaviorTreeReadBinding::GetProviderId() const { return Inner->GetProviderId(); }
TConstArrayView<FAssetDocumentPrimitiveDescriptor> FAssetDocumentStagedBehaviorTreeReadBinding::DescribePrimitives() const { return Inner->DescribePrimitives(); }
FAssetDocumentProviderAvailability FAssetDocumentStagedBehaviorTreeReadBinding::InspectAvailability() const { return Inner->InspectAvailability(); }
bool FAssetDocumentStagedBehaviorTreeReadBinding::Bind(const FAssetDocumentRuntimeRequestDto& OriginalRequest,
 const FString& ObjectPath, const FString& PackageFilename, uint64 FreshRevision)
{
 if (Request.IsSet() || !OriginalRequest.RequestId.IsValid() || OriginalRequest.Operation != EAssetDocumentRuntimeOperation::Apply
  || OriginalRequest.RequestedAssetClassPath != TEXT("/Script/AIModule.BehaviorTree")
  || FreshRevision == 0 || ObjectPath.IsEmpty() || ObjectPath == OriginalRequest.TargetAssetPath || PackageFilename.IsEmpty()) return false;
 Request = OriginalRequest; StagedObjectPath = ObjectPath; StagedPackageFilename = PackageFilename; Revision = FreshRevision;
 return true;
}
void FAssetDocumentStagedBehaviorTreeReadBinding::Close()
{
 if (Request.IsSet()) { ClosedRequestId = Request->RequestId; ClosedTarget = Request->TargetAssetPath; ClosedRevision = Revision; }
 ReadLease.Reset(); Request.Reset(); StagedObjectPath.Reset(); StagedPackageFilename.Reset(); Revision = 0;
}
FString FAssetDocumentStagedBehaviorTreeReadBinding::GetStagedObjectPath() const { return StagedObjectPath; }
bool FAssetDocumentStagedBehaviorTreeReadBinding::IsBound() const { return Request.IsSet(); }
bool FAssetDocumentStagedBehaviorTreeReadBinding::TryGetStagedObjectPath(const FGuid& RequestId, uint64 FreshRevision,
 const FString& OriginalTarget, FString& OutPath) const
{
 OutPath.Reset();
 if (!Request.IsSet() || !ReadLease || !ReadLease->IsCurrent() || Request->RequestId != RequestId
  || Revision != FreshRevision || Request->TargetAssetPath != OriginalTarget) return false;
 OutPath = StagedObjectPath; return true;
}
FAssetDocumentPrimitiveOutcome FAssetDocumentStagedBehaviorTreeReadBinding::Invoke(
 const FAssetDocumentPrimitiveInvocation& Invocation, IAssetDocumentHandleResolver& Handles,
 IAssetDocumentAtomicTransaction* Transaction)
{
 if (Invocation.PrimitiveId != TEXT("editor.asset.resolve_existing.v1"))
  return Inner->Invoke(Invocation, Handles, Transaction);
 const auto& Args = Invocation.ValueArguments;
 const int32 PathIndex = Args.ObjectKeys.IndexOfByKey(TEXT("path"));
 if (!Args.Elements.IsValidIndex(PathIndex) || Args.Elements[PathIndex].Kind != EAssetDocumentRuntimeValueKind::String)
  return Request.IsSet() ? BTStagedResolveFailure() : Inner->Invoke(Invocation, Handles, Transaction);
 const FString& Path = Args.Elements[PathIndex].StringValue;
 if (ClosedRequestId.IsValid() && ClosedRequestId == Invocation.RequestId
  && ClosedRevision == Invocation.RuntimeRevision && ClosedTarget == Path) return BTStagedResolveFailure();
 if (!Request.IsSet() || Path != Request->TargetAssetPath) return Inner->Invoke(Invocation, Handles, Transaction);
 const int32 ClassIndex = Args.ObjectKeys.IndexOfByKey(TEXT("expected_class"));
 if (ReadLease || Transaction || Invocation.Version != 1 || Args.Kind != EAssetDocumentRuntimeValueKind::Object
  || Invocation.RequestId != Request->RequestId || Invocation.RuntimeRevision != Revision
  || Invocation.Phase != EAssetDocumentRuntimeRequestPhase::Verify || !Invocation.HandleArguments.IsEmpty()
  || Args.ObjectKeys.Num() != 2 || Args.Elements.Num() != 2 || !Args.Elements.IsValidIndex(ClassIndex)
  || Args.Elements[ClassIndex].Kind != EAssetDocumentRuntimeValueKind::String
  || Args.Elements[ClassIndex].StringValue != TEXT("/Script/AIModule.BehaviorTree")) return BTStagedResolveFailure();
 FAssetDocumentHandleValidationContext Context;
 Context.RequestId = Invocation.RequestId; Context.TransactionId = Invocation.TransactionId;
 Context.RuntimeRevision = Invocation.RuntimeRevision; Context.ProviderId = TEXT("editor");
 Context.Phase = EAssetDocumentRuntimeRequestPhase::Verify;
 Context.RequiredKind = EAssetDocumentHandleKind::Asset; Context.RequiredAccess = EAssetDocumentHandleAccess::ReadOnly;
 FBTStagedReadHandles ReadHandles(Handles); FAssetDocumentPrimitiveOutcome Outcome;
 ReadLease = ResolveAssetDocumentEditorStagedBehaviorTreeRead(Request.GetValue(), Context,
  StagedObjectPath, StagedPackageFilename, Revision, ReadHandles, Outcome.Diagnostics);
 if (!ReadLease) return Outcome;
 Outcome.Status = EAssetDocumentPrimitiveOutcomeStatus::Succeeded; Outcome.Value = ReadLease->GetResolveValue();
 Outcome.HandleResults.Add({TEXT("asset_read"), {ReadLease->GetAssetReadHandle()}});
 Outcome.HandleResults.Add({TEXT("asset_mutable"), {}}); Outcome.HandleResults.Add({TEXT("package"), {}});
 return Outcome;
}

namespace
{
class FBTStagedAIReadProvider final : public IAssetDocumentPrimitiveProvider
{
public:
 FBTStagedAIReadProvider(TSharedRef<IAssetDocumentPrimitiveProvider> InInner,
  TSharedRef<FAssetDocumentStagedBehaviorTreeReadBinding> InBinding) : Inner(MoveTemp(InInner)), Binding(MoveTemp(InBinding)) {}
 FString GetProviderId() const override { return Inner->GetProviderId(); }
 TConstArrayView<FAssetDocumentPrimitiveDescriptor> DescribePrimitives() const override { return Inner->DescribePrimitives(); }
 FAssetDocumentProviderAvailability InspectAvailability() const override { return Inner->InspectAvailability(); }
 FAssetDocumentPrimitiveOutcome Invoke(const FAssetDocumentPrimitiveInvocation& Invocation,
  IAssetDocumentHandleResolver& Handles, IAssetDocumentAtomicTransaction* Transaction) override
 {
  if (!Binding->IsBound()) return Inner->Invoke(Invocation, Handles, Transaction);
  if (Invocation.PrimitiveId == TEXT("ai.behavior_tree.binding.inspect.v1"))
  {
   const auto& Value = Invocation.ValueArguments;
   const int32 SourceIndex = Value.ObjectKeys.IndexOfByKey(TEXT("source"));
   if (Transaction || Invocation.Version != 1 || Invocation.Phase != EAssetDocumentRuntimeRequestPhase::Verify
    || Value.Kind != EAssetDocumentRuntimeValueKind::Object || !Value.Elements.IsValidIndex(SourceIndex)) return BTStagedResolveFailure();
   const auto& Source = Value.Elements[SourceIndex];
   const int32 KindIndex = Source.ObjectKeys.IndexOfByKey(TEXT("kind"));
   if (Source.Kind != EAssetDocumentRuntimeValueKind::Object || !Source.Elements.IsValidIndex(KindIndex)
    || Source.Elements[KindIndex].Kind != EAssetDocumentRuntimeValueKind::String) return BTStagedResolveFailure();
   if (Source.Elements[KindIndex].StringValue == TEXT("class_defaults")) return Inner->Invoke(Invocation, Handles, Transaction);
   if (Source.ObjectKeys != TArray<FString>{TEXT("behavior_tree_path"), TEXT("kind"), TEXT("stable_node_id")}
    || Source.Elements.Num() != 3 || Source.Elements[KindIndex].StringValue != TEXT("live_node")
    || Source.Elements[0].Kind != EAssetDocumentRuntimeValueKind::String
    || Invocation.HandleArguments.Num() != 1 || Invocation.HandleArguments[0].Name != TEXT("asset")
    || Invocation.HandleArguments[0].Handle.Kind != EAssetDocumentHandleKind::Asset
    || Invocation.HandleArguments[0].Handle.Access != EAssetDocumentHandleAccess::ReadOnly) return BTStagedResolveFailure();
   FString StagedPath;
   if (!Binding->TryGetStagedObjectPath(Invocation.RequestId, Invocation.RuntimeRevision, Source.Elements[0].StringValue, StagedPath))
    return BTStagedResolveFailure();
   FAssetDocumentPrimitiveInvocation Actual = Invocation;
   Actual.ValueArguments.Elements[SourceIndex].Elements[0].StringValue = StagedPath;
   return Inner->Invoke(Actual, Handles, Transaction);
  }
  const bool TargetRead = Invocation.PrimitiveId == TEXT("ai.behavior_tree.authored_graph.readback.v1")
   || Invocation.PrimitiveId == TEXT("ai.behavior_tree.authored_graph.readback.v2")
   || Invocation.PrimitiveId == TEXT("ai.behavior_tree.blackboard.readback.v1")
   || Invocation.PrimitiveId == TEXT("ai.behavior_tree.selector.inventory.v1");
  if (!TargetRead)
  {
   for (const auto& Descriptor : Inner->DescribePrimitives())
    if (Descriptor.PrimitiveId == Invocation.PrimitiveId && Descriptor.Access != EAssetDocumentPrimitiveAccess::ReadOnly)
     return BTStagedResolveFailure();
   return Inner->Invoke(Invocation, Handles, Transaction);
  }
  const auto& Value = Invocation.ValueArguments;
  if (Transaction || Invocation.Phase != EAssetDocumentRuntimeRequestPhase::Verify
   || Invocation.Version != (Invocation.PrimitiveId == TEXT("ai.behavior_tree.authored_graph.readback.v2") ? 2u : 1u)
   || Value.Kind != EAssetDocumentRuntimeValueKind::Object
   || Value.ObjectKeys != TArray<FString>{TEXT("behavior_tree_path")} || Value.Elements.Num() != 1
   || Value.Elements[0].Kind != EAssetDocumentRuntimeValueKind::String
   || Invocation.HandleArguments.Num() != 1 || Invocation.HandleArguments[0].Name != TEXT("asset")
   || Invocation.HandleArguments[0].Handle.Kind != EAssetDocumentHandleKind::Asset
   || Invocation.HandleArguments[0].Handle.Access != EAssetDocumentHandleAccess::ReadOnly) return BTStagedResolveFailure();
  FString StagedPath;
  if (!Binding->TryGetStagedObjectPath(Invocation.RequestId, Invocation.RuntimeRevision, Value.Elements[0].StringValue, StagedPath))
   return BTStagedResolveFailure();
  FAssetDocumentPrimitiveInvocation Actual = Invocation;
  Actual.ValueArguments.Elements[0].StringValue = StagedPath;
  // No lease or identity is synthesized. AI observes the same readonly payload
  // and checks it against the actual staged path through its original handler.
  return Inner->Invoke(Actual, Handles, Transaction);
 }
private:
 TSharedRef<IAssetDocumentPrimitiveProvider> Inner;
 TSharedRef<FAssetDocumentStagedBehaviorTreeReadBinding> Binding;
};
}
TSharedRef<IAssetDocumentPrimitiveProvider> WrapAssetDocumentBehaviorTreeStagedAIReadProvider(
 TSharedRef<IAssetDocumentPrimitiveProvider> Inner, TSharedRef<FAssetDocumentStagedBehaviorTreeReadBinding> Binding)
{
 return MakeShared<FBTStagedAIReadProvider>(MoveTemp(Inner), MoveTemp(Binding));
}
