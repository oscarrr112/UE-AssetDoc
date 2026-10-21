// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Runtime/AssetDocumentRuntimeTypes.h"

class IAssetDocumentAtomicTransaction;
class IAssetDocumentVerifiedHandleLease;
class IPropertyHandle;
class IPropertyRowGenerator;
class UObject;
class UClass;

struct FAssetDocumentEditorTypedPreimageResult;
struct FAssetDocumentPrimitiveInvocation;
struct FAssetDocumentPrimitiveOutcome;

class ASSETDOCUMENTEDITOR_API FAssetDocumentEditorAssetIdentityView
{
public:
	UObject& GetObject() const;
	FStringView GetCanonicalObjectPath() const;
	uint64 GetTypeRevision() const;

	FAssetDocumentEditorAssetIdentityView(
		const FAssetDocumentEditorAssetIdentityView&) = delete;
	FAssetDocumentEditorAssetIdentityView& operator=(
		const FAssetDocumentEditorAssetIdentityView&) = delete;
	FAssetDocumentEditorAssetIdentityView(
		FAssetDocumentEditorAssetIdentityView&&) = delete;
	FAssetDocumentEditorAssetIdentityView& operator=(
		FAssetDocumentEditorAssetIdentityView&&) = delete;

private:
	FAssetDocumentEditorAssetIdentityView(
		UObject& InObject,
		FStringView InCanonicalObjectPath,
		uint64 InTypeRevision,
		const FGuid& InRequestId,
		const FGuid& InTransactionId,
		uint64 InRuntimeRevision,
		EAssetDocumentHandleAccess InAccess);

	UObject* Object = nullptr;
	FStringView CanonicalObjectPath;
	uint64 TypeRevision = 0;
	FGuid RequestId;
	FGuid TransactionId;
	uint64 RuntimeRevision = 0;
	EAssetDocumentHandleAccess Access =
		EAssetDocumentHandleAccess::ReadOnly;

	// The friend declarations must carry the same API macro as the later
	// declarations: MSVC reports C2375/C4273 on mismatched dll linkage.
	friend ASSETDOCUMENTEDITOR_API FAssetDocumentContractValidationResult
	WithAssetDocumentEditorAssetIdentity(
		const IAssetDocumentVerifiedHandleLease& Lease,
		TFunctionRef<FAssetDocumentContractValidationResult(
			const FAssetDocumentEditorAssetIdentityView&)> Consumer);

	friend ASSETDOCUMENTEDITOR_API FAssetDocumentEditorTypedPreimageResult
	CaptureAndEnlistAssetDocumentEditorObjectReferencePreimage(
		const FAssetDocumentEditorAssetIdentityView& AssetIdentity,
		UObject& ExactPropertyOwner,
		TSharedRef<IPropertyRowGenerator> PropertyRows,
		TSharedRef<IPropertyHandle> PropertyHandle,
		TConstArrayView<FString> RollbackComponentIds,
		IAssetDocumentAtomicTransaction& Transaction);
};

using FAssetDocumentEditorAssetIdentityConsumer =
	TFunctionRef<FAssetDocumentContractValidationResult(
		const FAssetDocumentEditorAssetIdentityView&)>;

ASSETDOCUMENTEDITOR_API FAssetDocumentContractValidationResult
WithAssetDocumentEditorAssetIdentity(
	const IAssetDocumentVerifiedHandleLease& Lease,
	FAssetDocumentEditorAssetIdentityConsumer Consumer);

struct ASSETDOCUMENTEDITOR_API FAssetDocumentEditorTypedPreimageResult
{
	bool bSucceeded = false;
	TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
};

ASSETDOCUMENTEDITOR_API FAssetDocumentEditorTypedPreimageResult
CaptureAndEnlistAssetDocumentEditorObjectReferencePreimage(
	const FAssetDocumentEditorAssetIdentityView& AssetIdentity,
	UObject& ExactPropertyOwner,
	TSharedRef<IPropertyRowGenerator> PropertyRows,
	TSharedRef<IPropertyHandle> PropertyHandle,
	TConstArrayView<FString> RollbackComponentIds,
	IAssetDocumentAtomicTransaction& Transaction);

// Native-only M3/M4 configuration of an object just allocated and recorded by
// the Animation composite. Animation owns provenance, one parent footprint,
// publication and retirement on failure; this function neither issues handles
// nor enlists another mutation. Existing Notify objects are outside this contract.
ASSETDOCUMENTEDITOR_API FAssetDocumentPrimitiveOutcome
ConfigureAssetDocumentEditorStagedMontageNotifyProperties(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	const IAssetDocumentVerifiedHandleLease& MontageLease,
	UObject& ExactStagedNotify,
	const FAssetDocumentRuntimeValueDto& Properties,
	IAssetDocumentAtomicTransaction& Transaction);

// Native-only observation of an installed Notify on the exact verified Montage.
// Returns every admitted field that is both readable and writable, using the
// existing property reader/codec; never issues handles or changes native state.
ASSETDOCUMENTEDITOR_API FAssetDocumentPrimitiveOutcome
ReadAssetDocumentEditorMontageNotifyProperties(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	const IAssetDocumentVerifiedHandleLease& MontageLease,
	UObject& ExactLiveNotify);

// Generic admitted-property access for a subobject that a provider owns and has
// already identified (e.g. Montage metadata objects). Reads every admitted field
// that is both readable and writable; writes decode every row before any setter.
// No handles are issued and nothing is enlisted: the caller's contribution must
// already be covered (Core package snapshot).
ASSETDOCUMENTEDITOR_API bool ReadAssetDocumentEditorSubobjectProperties(
 UObject& Subobject, FAssetDocumentRuntimeValueDto& OutProperties,
 TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics);
ASSETDOCUMENTEDITOR_API bool ValidateAssetDocumentEditorSubobjectProperties(
 UClass& SubobjectClass, const FAssetDocumentRuntimeValueDto& Properties,
 TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics);
ASSETDOCUMENTEDITOR_API bool WriteAssetDocumentEditorSubobjectProperties(
 UObject& Subobject, const FAssetDocumentRuntimeValueDto& Properties, FStringView AuthorityId,
 TArray<FAssetDocumentRuntimeDiagnostic>& OutDiagnostics);

class IAssetDocumentHandleResolver;
struct FAssetDocumentHandleValidationContext;
struct FAssetDocumentEditorStagedMontageReadState;

// Holds one request-local readonly issuance. Closing also invalidates payload
// aliases and leases acquired earlier; it does not unload the staged package.
class ASSETDOCUMENTEDITOR_API FAssetDocumentEditorStagedMontageReadLease final
{
public:
 ~FAssetDocumentEditorStagedMontageReadLease();
 const FAssetDocumentOpaqueHandle& GetAssetReadHandle() const;
 const FAssetDocumentRuntimeValueDto& GetResolveValue() const;
 void Close();
 FAssetDocumentEditorStagedMontageReadLease(const FAssetDocumentEditorStagedMontageReadLease&) = delete;
 FAssetDocumentEditorStagedMontageReadLease& operator=(const FAssetDocumentEditorStagedMontageReadLease&) = delete;
private:
 FAssetDocumentEditorStagedMontageReadLease(TSharedRef<FAssetDocumentEditorStagedMontageReadState> InState,
  FAssetDocumentOpaqueHandle InHandle, FAssetDocumentRuntimeValueDto InValue);
 TSharedPtr<FAssetDocumentEditorStagedMontageReadState> State;
 FAssetDocumentOpaqueHandle Handle;
 FAssetDocumentRuntimeValueDto Value;
 friend ASSETDOCUMENTEDITOR_API TUniquePtr<FAssetDocumentEditorStagedMontageReadLease> ResolveAssetDocumentEditorStagedMontageRead(
  const FAssetDocumentRuntimeRequestDto&, const FAssetDocumentHandleValidationContext&, const FString&, const FString&,
  IAssetDocumentHandleResolver&, TArray<FAssetDocumentRuntimeDiagnostic>&);
};

// Native Verify bootstrap only: the caller supplies a readonly-issuing Editor
// session for the active verification revision and keeps this scope through verification.
// The normal readonly handoff may then expose the same payload to Animation.
ASSETDOCUMENTEDITOR_API TUniquePtr<FAssetDocumentEditorStagedMontageReadLease>
ResolveAssetDocumentEditorStagedMontageRead(
 const FAssetDocumentRuntimeRequestDto& OriginalRequest,
 const FAssetDocumentHandleValidationContext& VerifyContext,
 const FString& StagedObjectPath,
 const FString& StagedPackageFilename,
 IAssetDocumentHandleResolver& Handles,
 TArray<FAssetDocumentRuntimeDiagnostic>& Diagnostics);
