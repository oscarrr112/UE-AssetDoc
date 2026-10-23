// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "Runtime/AssetDocumentAtomicTransaction.h"

class IAssetDocumentVerifiedHandleLease;
class IAssetDocumentHandleResolver;
class UObject;
class UClass;
class FAssetDocumentEditorSequenceOwnedFragmentClassPlan;

enum class EAssetDocumentEditorSequenceFragmentKind : uint8
{
	Metadata,
	AssetUserData,
};

// Native operation identity, not a public primitive or fragment/Body schema.
struct FAssetDocumentEditorSequenceFragmentContext
{
	FGuid OperationId;
	FAssetDocumentMutationToken Mutation;
	EAssetDocumentRuntimeRequestPhase Phase = EAssetDocumentRuntimeRequestPhase::Apply;
	EAssetDocumentEditorSequenceFragmentKind Kind = EAssetDocumentEditorSequenceFragmentKind::Metadata;
};

struct FAssetDocumentEditorSequenceFragmentReadResult
{
	bool bSucceeded = false;
	FAssetDocumentRuntimeValueDto Properties;
	TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
};

// Only reads admitted requested fields of one exact currently attached child.
ASSETDOCUMENTEDITOR_API FAssetDocumentEditorSequenceFragmentReadResult
ReadAssetDocumentEditorSequenceOwnedFragmentProperties(
	const FAssetDocumentEditorSequenceFragmentContext& Context,
	const IAssetDocumentVerifiedHandleLease& ParentLease,
	IAssetDocumentHandleResolver& Handles,
	UObject& ExactChild,
	TConstArrayView<FString> FieldNames);

// Request-local typed field participant. No attachment or completion authority.
class ASSETDOCUMENTEDITOR_API FAssetDocumentEditorSequenceOwnedFragmentProperties final
{
public:
	~FAssetDocumentEditorSequenceOwnedFragmentProperties();
	FAssetDocumentEditorSequenceOwnedFragmentProperties(const FAssetDocumentEditorSequenceOwnedFragmentProperties&) = delete;
	FAssetDocumentEditorSequenceOwnedFragmentProperties& operator=(const FAssetDocumentEditorSequenceOwnedFragmentProperties&) = delete;

	// The sole lifecycle owner must enlist its parent footprint/inverse first.
	// Both directions are raw Never-notify writes; the owner completes all effects.
	FAssetDocumentContractValidationResult ApplyRaw(
		const FAssetDocumentEditorSequenceFragmentContext& Context,
		const IAssetDocumentVerifiedHandleLease& ParentLease,
		IAssetDocumentHandleResolver& Handles,
		IAssetDocumentAtomicTransaction& Transaction);
	// Uses the retained native preimage after Core revokes forward handles. The
	// lifecycle owner must restore this exact attachment before invoking inverse.
	// Consumes an unapplied participant without any setters; cannot revive Apply.
	FAssetDocumentContractValidationResult RestoreRaw(
		const FAssetDocumentEditorSequenceFragmentContext& Context,
		IAssetDocumentAtomicTransaction& Transaction);

private:
	friend class FAssetDocumentEditorSequenceOwnedFragmentClassPlan;
	struct FImpl;
	explicit FAssetDocumentEditorSequenceOwnedFragmentProperties(TUniquePtr<FImpl> InImpl);
	TUniquePtr<FImpl> Impl;
	friend ASSETDOCUMENTEDITOR_API TUniquePtr<FAssetDocumentEditorSequenceOwnedFragmentProperties>
	PrepareAssetDocumentEditorSequenceOwnedFragmentProperties(
		const FAssetDocumentEditorSequenceFragmentContext&,
		const IAssetDocumentVerifiedHandleLease&, IAssetDocumentHandleResolver&, UObject&,
		const FAssetDocumentRuntimeValueDto&, IAssetDocumentAtomicTransaction&,
		TArray<FAssetDocumentRuntimeDiagnostic>&);
};

// Captures complete touched-field native values and decodes every desired value
// before any setter or enlistment, in the same Live or Mutating transaction.
// Apply remains Mutating-only. Buffers strongly retain their object references.
ASSETDOCUMENTEDITOR_API TUniquePtr<FAssetDocumentEditorSequenceOwnedFragmentProperties>
PrepareAssetDocumentEditorSequenceOwnedFragmentProperties(
	const FAssetDocumentEditorSequenceFragmentContext& Context,
	const IAssetDocumentVerifiedHandleLease& ParentLease,
	IAssetDocumentHandleResolver& Handles,
	UObject& ExactChild,
	const FAssetDocumentRuntimeValueDto& Properties,
	IAssetDocumentAtomicTransaction& Transaction,
	TArray<FAssetDocumentRuntimeDiagnostic>& Diagnostics);

// Class-only desired-value plan for the sole lifecycle owner's create path.
// No class loading/CDO mutation, authored allocation, attachment or enlistment.
// Prepare/Validate accept the same Live or Mutating Core transaction in Apply.
class ASSETDOCUMENTEDITOR_API FAssetDocumentEditorSequenceOwnedFragmentClassPlan final
{
public:
	~FAssetDocumentEditorSequenceOwnedFragmentClassPlan();
	FAssetDocumentEditorSequenceOwnedFragmentClassPlan(const FAssetDocumentEditorSequenceOwnedFragmentClassPlan&) = delete;
	FAssetDocumentEditorSequenceOwnedFragmentClassPlan& operator=(const FAssetDocumentEditorSequenceOwnedFragmentClassPlan&) = delete;

	FAssetDocumentContractValidationResult ValidateBeforeCreate(
		const FAssetDocumentEditorSequenceFragmentContext& Context,
		const IAssetDocumentVerifiedHandleLease& ParentLease,
		IAssetDocumentHandleResolver& Handles,
		const IAssetDocumentAtomicTransaction& Transaction) const;

	// Consumes the attempt even on rejection. The lifecycle owner has enlisted
	// its inverse/footprint and created/attached this exact child (Mutating).
	// Captures its native preimage and transfers already decoded values, never
	// re-decodes. Class admission is rechecked against actual instance access.
	// Captures the supplied child's actual name/path/identity; this is NOT proof
	// of creation provenance and defines no requested-name or rename policy.
	TUniquePtr<FAssetDocumentEditorSequenceOwnedFragmentProperties> BindOnce(
		const FAssetDocumentEditorSequenceFragmentContext& Context,
		const IAssetDocumentVerifiedHandleLease& ParentLease,
		IAssetDocumentHandleResolver& Handles,
		UObject& ExactChild,
		IAssetDocumentAtomicTransaction& Transaction,
		TArray<FAssetDocumentRuntimeDiagnostic>& Diagnostics);

private:
	struct FImpl;
	explicit FAssetDocumentEditorSequenceOwnedFragmentClassPlan(TUniquePtr<FImpl> InImpl);
	TUniquePtr<FImpl> Impl;
	friend ASSETDOCUMENTEDITOR_API TUniquePtr<FAssetDocumentEditorSequenceOwnedFragmentClassPlan>
	PrepareAssetDocumentEditorSequenceOwnedFragmentClassPlan(
		const FAssetDocumentEditorSequenceFragmentContext&,
		const IAssetDocumentVerifiedHandleLease&, IAssetDocumentHandleResolver&, UClass&,
		const FAssetDocumentRuntimeValueDto&, IAssetDocumentAtomicTransaction&,
		TArray<FAssetDocumentRuntimeDiagnostic>&);
};

// Decode-all of the exact already-loaded Metadata/UserData class before the
// lifecycle owner allocates or attaches anything. No child/provenance is issued.
ASSETDOCUMENTEDITOR_API TUniquePtr<FAssetDocumentEditorSequenceOwnedFragmentClassPlan>
PrepareAssetDocumentEditorSequenceOwnedFragmentClassPlan(
	const FAssetDocumentEditorSequenceFragmentContext& Context,
	const IAssetDocumentVerifiedHandleLease& ParentLease,
	IAssetDocumentHandleResolver& Handles,
	UClass& ExactClass,
	const FAssetDocumentRuntimeValueDto& Properties,
	IAssetDocumentAtomicTransaction& Transaction,
	TArray<FAssetDocumentRuntimeDiagnostic>& Diagnostics);
