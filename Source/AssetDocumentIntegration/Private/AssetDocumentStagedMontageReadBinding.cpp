// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
#include "AssetDocumentStagedMontageReadBinding.h"
#include "AssetDocumentEditorNativeState.h"
#include "Runtime/AssetDocumentHandleRegistry.h"

namespace
{
// Narrow the normal resolver invocation's issuance to this seam's sole result.
class FStagedReadHandles final : public IAssetDocumentHandleResolver
{
public:
	explicit FStagedReadHandles(IAssetDocumentHandleResolver& InInner) : Inner(InInner) {}
	FAssetDocumentIssuedHandleResult Issue(EAssetDocumentHandleKind Kind, EAssetDocumentHandleAccess Access,
		TUniquePtr<IAssetDocumentNativeHandlePayload>&& Payload) override
	{
		if (Kind != EAssetDocumentHandleKind::Asset || Access != EAssetDocumentHandleAccess::ReadOnly) return {};
		return Inner.Issue(Kind, Access, MoveTemp(Payload));
	}
	FAssetDocumentVerifiedLeaseResult AcquireVerifiedLease(const FAssetDocumentOpaqueHandle& Handle,
		const FAssetDocumentHandleValidationContext& Context) override
	{
		if (Context.RequiredKind != EAssetDocumentHandleKind::Asset
			|| Context.RequiredAccess != EAssetDocumentHandleAccess::ReadOnly) return {};
		return Inner.AcquireVerifiedLease(Handle, Context);
	}
	FAssetDocumentHandleInvalidationResult Invalidate(const FAssetDocumentHandleInvalidation& Event) override
	{ return Inner.Invalidate(Event); }
private:
	IAssetDocumentHandleResolver& Inner;
};
}

FAssetDocumentStagedMontageReadBinding::FAssetDocumentStagedMontageReadBinding(TSharedRef<IAssetDocumentPrimitiveProvider> InInner)
	: Inner(MoveTemp(InInner)) {}
FAssetDocumentStagedMontageReadBinding::~FAssetDocumentStagedMontageReadBinding() { Close(); }
FString FAssetDocumentStagedMontageReadBinding::GetProviderId() const { return Inner->GetProviderId(); }
TConstArrayView<FAssetDocumentPrimitiveDescriptor> FAssetDocumentStagedMontageReadBinding::DescribePrimitives() const { return Inner->DescribePrimitives(); }
FAssetDocumentProviderAvailability FAssetDocumentStagedMontageReadBinding::InspectAvailability() const { return Inner->InspectAvailability(); }
bool FAssetDocumentStagedMontageReadBinding::Bind(const FAssetDocumentRuntimeRequestDto& OriginalRequest,
	const FString& ObjectPath, const FString& PackageFilename, uint64 FreshRevision)
{
	if (Request.IsSet() || FreshRevision == 0 || ObjectPath.IsEmpty() || PackageFilename.IsEmpty()) return false;
	Request = OriginalRequest; StagedObjectPath = ObjectPath; StagedPackageFilename = PackageFilename; Revision = FreshRevision;
	return true;
}
void FAssetDocumentStagedMontageReadBinding::Close()
{
	ReadLease.Reset(); Request.Reset(); StagedObjectPath.Reset(); StagedPackageFilename.Reset(); Revision = 0;
}
FAssetDocumentPrimitiveOutcome FAssetDocumentStagedMontageReadBinding::Invoke(
	const FAssetDocumentPrimitiveInvocation& Invocation, IAssetDocumentHandleResolver& Handles,
	IAssetDocumentAtomicTransaction* Transaction)
{
	if (!Request.IsSet() || Invocation.PrimitiveId != TEXT("editor.asset.resolve_existing.v1")
		|| Invocation.Version != 1 || Invocation.ValueArguments.Kind != EAssetDocumentRuntimeValueKind::Object)
		return Inner->Invoke(Invocation, Handles, Transaction);
	const auto& Args = Invocation.ValueArguments;
	const int32 PathIndex = Args.ObjectKeys.IndexOfByKey(TEXT("path"));
	if (!Args.Elements.IsValidIndex(PathIndex) || Args.Elements[PathIndex].Kind != EAssetDocumentRuntimeValueKind::String
		|| Args.Elements[PathIndex].StringValue != Request->TargetAssetPath)
		return Inner->Invoke(Invocation, Handles, Transaction);
	FAssetDocumentPrimitiveOutcome Outcome;
	const int32 ClassIndex = Args.ObjectKeys.IndexOfByKey(TEXT("expected_class"));
	if (ReadLease || Transaction || Invocation.RequestId != Request->RequestId
		|| Invocation.RuntimeRevision != Revision || Invocation.Phase != EAssetDocumentRuntimeRequestPhase::Verify
		|| Args.ObjectKeys.Num() != 2 || Args.Elements.Num() != 2 || !Args.Elements.IsValidIndex(ClassIndex)
		|| Args.Elements[ClassIndex].Kind != EAssetDocumentRuntimeValueKind::String
		|| Args.Elements[ClassIndex].StringValue != TEXT("/Script/Engine.AnimMontage"))
	{
		auto& D = Outcome.Diagnostics.AddDefaulted_GetRef();
		D.Path = TEXT("/GenericRuntime/FreshVerification"); D.Code = TEXT("GenericRuntime.AnimationStagedResolveMismatch");
		D.Message = TEXT("Staged resolution requires this request's exact readonly Verify target once.");
		return Outcome;
	}
	FAssetDocumentHandleValidationContext Context;
	Context.RequestId = Invocation.RequestId; Context.TransactionId = Invocation.TransactionId;
	Context.RuntimeRevision = Invocation.RuntimeRevision; Context.ProviderId = TEXT("editor");
	Context.Phase = EAssetDocumentRuntimeRequestPhase::Verify;
	Context.RequiredKind = EAssetDocumentHandleKind::Asset; Context.RequiredAccess = EAssetDocumentHandleAccess::ReadOnly;
	FStagedReadHandles ReadHandles(Handles);
	ReadLease = ResolveAssetDocumentEditorStagedMontageRead(Request.GetValue(), Context,
		StagedObjectPath, StagedPackageFilename, ReadHandles, Outcome.Diagnostics);
	if (!ReadLease) return Outcome;
	Outcome.Status = EAssetDocumentPrimitiveOutcomeStatus::Succeeded;
	Outcome.Value = ReadLease->GetResolveValue();
	Outcome.HandleResults.Add({TEXT("asset_read"), {ReadLease->GetAssetReadHandle()}});
	Outcome.HandleResults.Add({TEXT("asset_mutable"), {}});
	Outcome.HandleResults.Add({TEXT("package"), {}});
	return Outcome;
}
