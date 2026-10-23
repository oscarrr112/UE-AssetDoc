// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "AssetDocumentEditorSequenceOwnedFragmentProperties.h"
#include "AssetDocumentRollbackAction.h"

class UAnimSequence;
class UClass;

namespace AssetDocumentSequenceOwnedFragmentLifecycle
{
// Native operation-local inputs, never a Body schema or child handle. Python
// owns reconciliation and supplies the explicit complete final attachment order.
struct FOrderedFragment
{
	UObject* Existing = nullptr;
	UClass* CreateClass = nullptr;
	TOptional<FString> Name;
	FAssetDocumentRuntimeValueDto Properties;
};

struct FReplacement
{
	EAssetDocumentEditorSequenceFragmentKind Kind = EAssetDocumentEditorSequenceFragmentKind::Metadata;
	TArray<UObject*> Managed;
	TArray<FOrderedFragment> Order;
};

// Readonly feasibility, not mutation authority. Verifies exact membership,
// preservation of unmanaged objects/order and the stock selectors' class rules.
ASSETDOCUMENTANIMATION_API FAssetDocumentContractValidationResult ValidateAttachmentOrder(
	const UAnimSequence& Parent, const FReplacement& Replacement);

// The lifecycle composite owns this strong native preimage. It restores parent
// membership before field inverse. The captured dirty flag is a completion
// invariant: the stock raw field/pointer chain does not change package dirtiness.
// No public primitive/catalog admission follows from constructing this state.
class ASSETDOCUMENTANIMATION_API FParentPreimage final
{
public:
	~FParentPreimage();
	FParentPreimage(const FParentPreimage&) = delete;
	FParentPreimage& operator=(const FParentPreimage&) = delete;

	static TUniquePtr<FParentPreimage> Capture(
		const FAssetDocumentEditorSequenceFragmentContext& Context,
		const IAssetDocumentVerifiedHandleLease& ParentLease,
		IAssetDocumentHandleResolver& Handles, const FReplacement& Replacement,
		IAssetDocumentAtomicTransaction& Transaction,
		TArray<FAssetDocumentRuntimeDiagnostic>& Diagnostics);

	FAssetDocumentContractValidationResult ValidateBaseline(
		const FAssetDocumentEditorSequenceFragmentContext& Context,
		const IAssetDocumentVerifiedHandleLease& ParentLease,
		IAssetDocumentHandleResolver& Handles,
		IAssetDocumentAtomicTransaction& Transaction) const;
	FAssetDocumentContractValidationResult ValidateIdentity(
		const FAssetDocumentEditorSequenceFragmentContext& Context,
		const IAssetDocumentVerifiedHandleLease& ParentLease,
		IAssetDocumentHandleResolver& Handles,
		IAssetDocumentAtomicTransaction& Transaction) const;

	bool OriginalAttachmentsCurrent() const;
	bool OtherAttachmentsCurrent() const;
	UAnimSequence& GetParent() const;
	const FReplacement& GetReplacement() const;
	TArray<UObject*> GetOriginalOrder() const;
	bool WasPackageDirty() const;

	// Called by the sole parent rollback action, after handle revocation. The
	// owner must already have enlisted the full parent/package preimage before
	// any forward helper; this leaf cannot register actions or regain a lease.
	FAssetDocumentContractValidationResult RestoreAttachments(
		const FAssetDocumentEditorSequenceFragmentContext& Context,
		IAssetDocumentAtomicTransaction& Transaction,
		TConstArrayView<UObject*> ExactOwnedCurrentOrder);

private:
	struct FImpl;
	explicit FParentPreimage(TUniquePtr<FImpl> InImpl);
	TUniquePtr<FImpl> Impl;
};

// Existing-child update/delete participant. Creation is a separate unresolved
// effect-ownership edge; this operation never calls a class constructor. The
// same existing pure Prepare captures all fields before the parent's enlist.
class ASSETDOCUMENTANIMATION_API FExistingReplacement final : public IAssetDocumentRollbackAction,
	public TSharedFromThis<FExistingReplacement>
{
public:
	~FExistingReplacement();
	static TSharedPtr<FExistingReplacement> Prepare(
		const FAssetDocumentEditorSequenceFragmentContext& Context,
		const IAssetDocumentVerifiedHandleLease& ParentLease,
		IAssetDocumentHandleResolver& Handles, const FReplacement& Replacement,
		IAssetDocumentAtomicTransaction& Transaction,
		TArray<FAssetDocumentRuntimeDiagnostic>& Diagnostics);
	TArray<FAssetDocumentMutationFootprint> GetFootprint() const;
	FStringView GetRollbackComponentId() const override;
	FAssetDocumentContractValidationResult Apply(
		const FAssetDocumentEditorSequenceFragmentContext& Context,
		const IAssetDocumentVerifiedHandleLease& ParentLease,
		IAssetDocumentHandleResolver& Handles,
		IAssetDocumentAtomicTransaction& Transaction);
	FAssetDocumentContractValidationResult Rollback(FStringView StableFailureCode) override;
private:
	struct FImpl;
	explicit FExistingReplacement(TUniquePtr<FImpl> InImpl);
	TUniquePtr<FImpl> Impl;
};
}
