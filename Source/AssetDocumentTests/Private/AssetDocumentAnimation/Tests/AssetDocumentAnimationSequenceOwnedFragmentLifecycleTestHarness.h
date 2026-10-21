// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
#pragma once
#include "AssetDocumentEditorPrimitiveProvider.h"
#include "Animation/AnimSequence.h"
#include "Misc/AutomationTest.h"
#include "Runtime/AssetDocumentHandleRegistry.h"
#include "Runtime/AssetDocumentAtomicTransaction.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentSequenceOwnedFragmentOrderTests
{
// Test-owned single request registry. The immutable native Asset payload comes
// from the real Editor resolve handler; no copied payload/class/field codec.
class FResolver final : public IAssetDocumentHandleResolver
{
	struct FEntry { FAssetDocumentOpaqueHandle Handle; TUniquePtr<IAssetDocumentNativeHandlePayload> Payload; };
	class FLease final : public IAssetDocumentVerifiedHandleLease
	{
		FAssetDocumentOpaqueHandle Handle;
		IAssetDocumentNativeHandlePayload& Payload;
	public:
		FLease(FAssetDocumentOpaqueHandle H, IAssetDocumentNativeHandlePayload& P) : Handle(MoveTemp(H)), Payload(P) {}
		const FAssetDocumentOpaqueHandle& GetHandle() const override { return Handle; }
		IAssetDocumentNativeHandlePayload& GetPayloadChecked() const override { return Payload; }
	};
	TArray<FEntry> Entries;
	FString Issuer = TEXT("editor");
public:
	FAssetDocumentMutationToken Token;
	int32 RollbackInvalidations = 0;
	FAssetDocumentIssuedHandleResult Issue(EAssetDocumentHandleKind Kind, EAssetDocumentHandleAccess Access,
		TUniquePtr<IAssetDocumentNativeHandlePayload>&& Payload) override
	{
		FAssetDocumentIssuedHandleResult R; if (!Payload || !Payload->IsAlive()) return R;
		auto& E = Entries.AddDefaulted_GetRef(); E.Handle.RequestId = Token.RequestId; E.Handle.TransactionId = Token.TransactionId;
		E.Handle.RuntimeRevision = Token.RuntimeRevision; E.Handle.ProviderId = Issuer; E.Handle.Kind = Kind; E.Handle.Access = Access;
		E.Handle.Slot = Entries.Num(); E.Handle.Generation = 1; E.Payload = MoveTemp(Payload); R.Handle = E.Handle; return R;
	}
	FAssetDocumentVerifiedLeaseResult AcquireVerifiedLease(const FAssetDocumentOpaqueHandle& H,
		const FAssetDocumentHandleValidationContext& V) override
	{
		FAssetDocumentVerifiedLeaseResult R; auto Validation = ValidateAssetDocumentHandleMetadata(H, V);
		if (!Validation.bValid) { R.Diagnostics = MoveTemp(Validation.Diagnostics); return R; }
		const int32 Index = int32(H.Slot) - 1;
		if (!Entries.IsValidIndex(Index)) return R;
		auto& E = Entries[Index];
		if (!E.Payload || !E.Payload->IsAlive() || E.Handle.Generation != H.Generation || E.Handle.ProviderId != H.ProviderId
			|| E.Handle.Kind != H.Kind || E.Handle.Access != H.Access || E.Handle.RequestId != H.RequestId
			|| E.Handle.TransactionId != H.TransactionId || E.Handle.RuntimeRevision != H.RuntimeRevision) return R;
		R.Lease = MakeUnique<FLease>(H, *E.Payload); return R;
	}
	FAssetDocumentHandleInvalidationResult Invalidate(const FAssetDocumentHandleInvalidation& Event) override
	{
		FAssetDocumentHandleInvalidationResult R;
		if (Event.RequestId != Token.RequestId || Event.TransactionId != Token.TransactionId || Event.RuntimeRevision != Token.RuntimeRevision) return R;
		for (auto& E : Entries) if (E.Payload && (Event.ProviderId.IsEmpty() || E.Handle.ProviderId == Event.ProviderId))
		{ E.Payload.Reset(); ++E.Handle.Generation; ++R.InvalidatedHandleCount; }
		if (Event.Reason == EAssetDocumentHandleInvalidationReason::Rollback) ++RollbackInvalidations;
		return R;
	}
	TOptional<FAssetDocumentOpaqueHandle> ResolveParent(UAnimSequence& Parent, IAssetDocumentAtomicTransaction& Transaction)
	{
		FAssetDocumentPrimitiveInvocation I; I.PrimitiveId = TEXT("editor.asset.resolve_existing.v1"); I.Version = 1;
		I.RequestId = Token.RequestId; I.TransactionId = Token.TransactionId; I.RuntimeRevision = Token.RuntimeRevision;
		I.Phase = EAssetDocumentRuntimeRequestPhase::Apply; I.ValueArguments.Kind = EAssetDocumentRuntimeValueKind::Object;
		I.ValueArguments.ObjectKeys = {TEXT("path"), TEXT("expected_class")};
		for (const FString& S : {Parent.GetPathName(), Parent.GetClass()->GetPathName()})
		{ auto& V = I.ValueArguments.Elements.AddDefaulted_GetRef(); V.Kind = EAssetDocumentRuntimeValueKind::String; V.StringValue = S; }
		Issuer = TEXT("editor");
		const auto Outcome = CreateAssetDocumentEditorPrimitiveProvider()->Invoke(I, *this, &Transaction);
		if (Outcome.Status != EAssetDocumentPrimitiveOutcomeStatus::Succeeded) return {};
		for (const auto& Group : Outcome.HandleResults) if (Group.Name == TEXT("asset_mutable") && Group.Handles.Num() == 1)
		{
			// Only fixture registry issuance is adapted. Move the actual Editor
			// Asset payload into its Animation slot, consuming its old issuance.
			const int32 Index = int32(Group.Handles[0].Slot) - 1;
			if (!Entries.IsValidIndex(Index) || !Entries[Index].Payload) return {};
			auto Payload = MoveTemp(Entries[Index].Payload); ++Entries[Index].Handle.Generation;
			Issuer = TEXT("animation"); auto Issued = Issue(EAssetDocumentHandleKind::Asset, EAssetDocumentHandleAccess::Mutable, MoveTemp(Payload));
			return MoveTemp(Issued.Handle);
		}
		return {};
	}
};
class FCancellation final : public IAssetDocumentCancellationView
{
public:
	bool IsCancellationRequested() const override { return false; }
};
}
#endif
