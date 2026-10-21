// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Provider/Property/AssetDocumentEditorPropertyTokenPayload.h"
#include "Runtime/AssetDocumentAtomicTransaction.h"
#include "Runtime/AssetDocumentPrimitiveProvider.h"

class FAssetDocumentEditorScopedPropertyValue;
class UObject;

struct FAssetDocumentEditorPropertyBatchTokenFieldView;
struct FAssetDocumentEditorPropertyPreimageCaptureResult;

class ASSETDOCUMENTEDITOR_API FAssetDocumentEditorPropertyPreimage final
{
public:
	FAssetDocumentEditorPropertyPreimage();
	~FAssetDocumentEditorPropertyPreimage();

	FAssetDocumentEditorPropertyPreimage(
		FAssetDocumentEditorPropertyPreimage&& Other);
	FAssetDocumentEditorPropertyPreimage& operator=(
		FAssetDocumentEditorPropertyPreimage&& Other);

	FAssetDocumentEditorPropertyPreimage(
		const FAssetDocumentEditorPropertyPreimage&) = delete;
	FAssetDocumentEditorPropertyPreimage& operator=(
		const FAssetDocumentEditorPropertyPreimage&) = delete;

	bool IsReady() const;
	FAssetDocumentContractValidationResult Restore();
	FAssetDocumentContractValidationResult RestoreFieldsOnly();

private:
	FAssetDocumentContractValidationResult RestoreInternal(bool bRestoreDirty);
	struct FImpl;
	TUniquePtr<FImpl> Impl;

	friend ASSETDOCUMENTEDITOR_API FAssetDocumentEditorPropertyPreimageCaptureResult
	CaptureAssetDocumentEditorAdmittedPropertyPreimage(
		UObject& ExactTarget,
		TConstArrayView<
			FAssetDocumentEditorPropertyBatchTokenFieldView> Fields);
};

struct FAssetDocumentEditorPropertyPreimageCaptureResult
{
	bool bSucceeded = false;
	FAssetDocumentEditorPropertyPreimage Preimage;
	TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
};

ASSETDOCUMENTEDITOR_API FAssetDocumentEditorPropertyPreimageCaptureResult
CaptureAssetDocumentEditorAdmittedPropertyPreimage(
	UObject& ExactTarget,
	TConstArrayView<
		FAssetDocumentEditorPropertyBatchTokenFieldView> Fields);

struct FAssetDocumentEditorPropertyEditResult
{
	bool bSucceeded = false;
	FAssetDocumentRuntimeValueDto Value;
	TArray<FAssetDocumentChangedRecord> ChangedSet;
	TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
};

class ASSETDOCUMENTEDITOR_API FAssetDocumentEditorPreparedPropertyWrite
{
public:
	FAssetDocumentEditorPreparedPropertyWrite();
	~FAssetDocumentEditorPreparedPropertyWrite();

	FAssetDocumentEditorPreparedPropertyWrite(
		FAssetDocumentEditorPreparedPropertyWrite&& Other);
	FAssetDocumentEditorPreparedPropertyWrite& operator=(
		FAssetDocumentEditorPreparedPropertyWrite&& Other);

	FAssetDocumentEditorPreparedPropertyWrite(
		const FAssetDocumentEditorPreparedPropertyWrite&) = delete;
	FAssetDocumentEditorPreparedPropertyWrite& operator=(
		const FAssetDocumentEditorPreparedPropertyWrite&) = delete;

	TConstArrayView<FAssetDocumentMutationFootprint> GetFootprint() const;

private:
	friend ASSETDOCUMENTEDITOR_API bool TryPrepareAssetDocumentEditorAdmittedPropertyValue(
		const FAssetDocumentEditorPropertyTokenView& Token,
		const FAssetDocumentRuntimeValueDto& DesiredValue,
		FAssetDocumentEditorPreparedPropertyWrite& OutPrepared,
		FAssetDocumentEditorPropertyEditResult& OutFailure,
		UObject* Owner);
	friend ASSETDOCUMENTEDITOR_API bool TryPrepareAssetDocumentEditorAdmittedPropertyWrite(
		UObject& Asset,
		const FAssetDocumentEditorPropertyTokenView& Token,
		const FAssetDocumentRuntimeValueDto& DesiredValue,
		FAssetDocumentEditorPreparedPropertyWrite& OutPrepared,
		FAssetDocumentEditorPropertyEditResult& OutFailure);
	// The public object-property API's decode (AssetDocumentEditorObjectProperties.h),
	// which checks references with its own policy before handing the value over.
	friend void AdoptAssetDocumentEditorDecodedPropertyValue(
		TUniquePtr<FAssetDocumentEditorScopedPropertyValue>&& NativeValue,
		FAssetDocumentEditorPreparedPropertyWrite& OutPrepared);
	friend ASSETDOCUMENTEDITOR_API FAssetDocumentEditorPropertyEditResult
	ApplyAssetDocumentEditorPreparedPropertyWriteWithoutEnlist(
		UObject& Asset,
		const FAssetDocumentEditorPropertyTokenView& Token,
		FAssetDocumentEditorPreparedPropertyWrite&& Prepared,
		FStringView AuthorityId);

	TUniquePtr<FAssetDocumentEditorScopedPropertyValue> NativeValue;
	TArray<FAssetDocumentMutationFootprint> Footprint;
};

// Defined with the object-property API (AssetDocumentEditorObjectProperties.cpp).
void AdoptAssetDocumentEditorDecodedPropertyValue(
	TUniquePtr<FAssetDocumentEditorScopedPropertyValue>&& NativeValue,
	FAssetDocumentEditorPreparedPropertyWrite& OutPrepared);

// Value-only leaf shared by top-level writes and the native staged Notify caller.
// No target admission, identity, footprint or transaction is manufactured here.
// With Owner the value is an edit of Owner's current value (omitted struct
// members / instanced Properties keep theirs; see FAssetDocumentEditorDecodeBase).
ASSETDOCUMENTEDITOR_API bool TryPrepareAssetDocumentEditorAdmittedPropertyValue(
	const FAssetDocumentEditorPropertyTokenView& Token,
	const FAssetDocumentRuntimeValueDto& DesiredValue,
	FAssetDocumentEditorPreparedPropertyWrite& OutPrepared,
	FAssetDocumentEditorPropertyEditResult& OutFailure,
	UObject* Owner);

inline bool TryPrepareAssetDocumentEditorAdmittedPropertyValue(
	const FAssetDocumentEditorPropertyTokenView& Token,
	const FAssetDocumentRuntimeValueDto& DesiredValue,
	FAssetDocumentEditorPreparedPropertyWrite& OutPrepared,
	FAssetDocumentEditorPropertyEditResult& OutFailure)
{
	return TryPrepareAssetDocumentEditorAdmittedPropertyValue(Token, DesiredValue, OutPrepared, OutFailure, nullptr);
}

ASSETDOCUMENTEDITOR_API bool TryPrepareAssetDocumentEditorAdmittedPropertyWrite(
	UObject& Asset,
	const FAssetDocumentEditorPropertyTokenView& Token,
	const FAssetDocumentRuntimeValueDto& DesiredValue,
	FAssetDocumentEditorPreparedPropertyWrite& OutPrepared,
	FAssetDocumentEditorPropertyEditResult& OutFailure);

ASSETDOCUMENTEDITOR_API FAssetDocumentEditorPropertyEditResult
ApplyAssetDocumentEditorPreparedPropertyWriteWithoutEnlist(
	UObject& Asset,
	const FAssetDocumentEditorPropertyTokenView& Token,
	FAssetDocumentEditorPreparedPropertyWrite&& Prepared,
	FStringView AuthorityId);

// AsAuthored (optional): an authored value the read is shaped like. Struct
// members and instanced Properties the authored value omits are left out of
// the result (they keep their current value on Apply), so Diff and Verify can
// compare a partial value exactly.
ASSETDOCUMENTEDITOR_API FAssetDocumentEditorPropertyEditResult
ReadAssetDocumentEditorAdmittedProperty(
	UObject& Asset,
	const FAssetDocumentEditorPropertyTokenView& Token,
	const FAssetDocumentRuntimeValueDto* AsAuthored = nullptr);

// The document read of the generic property primitive: {"value": V} like
// ReadAssetDocumentEditorAdmittedProperty, or {"editor_derived": true} when the
// property currently holds an editor-derived reference (an object inside the
// asset through a non-instanced reference, see
// FindAssetDocumentEditorDerivedReference). Documents omit such a property; a
// write may replace the object but not name one inside the asset
// (Property.EditorDerivedReference).
FAssetDocumentEditorPropertyEditResult
ReadAssetDocumentEditorAdmittedDocumentProperty(
	UObject& Asset,
	const FAssetDocumentEditorPropertyTokenView& Token,
	const FAssetDocumentRuntimeValueDto* AsAuthored = nullptr);

// {"value": V}: the read of an extract's diff_only, V sparse over the value Defaults holds (the class default object):
// struct members and array elements' members equal to it are left out (EditorPropertyValueCodec::PruneToChanged).
FAssetDocumentEditorPropertyEditResult
ReadAssetDocumentEditorAdmittedPropertyChanged(
	UObject& Asset,
	const FAssetDocumentEditorPropertyTokenView& Token,
	UObject& Defaults);

FAssetDocumentEditorPropertyEditResult
WriteAssetDocumentEditorAdmittedProperty(
	UObject& Asset,
	const FAssetDocumentEditorPropertyTokenView& Token,
	const FAssetDocumentRuntimeValueDto& DesiredValue,
	IAssetDocumentAtomicTransaction& Transaction);

ASSETDOCUMENTEDITOR_API FAssetDocumentEditorPropertyEditResult
PreflightAssetDocumentEditorAdmittedProperty(
	UObject& Scratch,
	const FAssetDocumentEditorPropertyTokenView& Token,
	const FAssetDocumentRuntimeValueDto& DesiredValue);
