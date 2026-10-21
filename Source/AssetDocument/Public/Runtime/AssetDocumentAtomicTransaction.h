// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Runtime/AssetDocumentRuntimeTypes.h"

enum class EAssetDocumentAtomicState : uint8
{
	Live = 0,
	Mutating = 1,
	ReadyToCommit = 2,
	Committed = 3,
	RolledBack = 4,
	Poisoned = 5,
};

enum class EAssetDocumentMutationFootprintKind : uint8
{
	UObjectState = 0,
	Package = 1,
	AssetRegistry = 2,
	File = 3,
	Handle = 4,
};

struct ASSETDOCUMENT_API FAssetDocumentMutationFootprint
{
	EAssetDocumentMutationFootprintKind Kind = EAssetDocumentMutationFootprintKind::UObjectState;
	FString StableId;
	FString RollbackComponentId;
};

struct ASSETDOCUMENT_API FAssetDocumentPreflightReceipt
{
	FGuid RequestId;
	uint64 RuntimeRevision = 0;
	TArray<FAssetDocumentMutationFootprint> CompleteFootprint;
	FString ReceiptHash;
	bool bSucceeded = false;
};

struct ASSETDOCUMENT_API FAssetDocumentMutationToken
{
	FGuid RequestId;
	FGuid TransactionId;
	uint64 RuntimeRevision = 0;
};

struct ASSETDOCUMENT_API FAssetDocumentDurableCompletionReceipt
{
	FGuid RequestId;
	FGuid TransactionId;
	FString JournalId;
	FString PackageContentDigest;
	FString SidecarContentDigest;
	bool bSyncStateAdvanced = false;
};

class IAssetDocumentRollbackAction;
class IAssetDocumentPostRollbackFinalizer;

class ASSETDOCUMENT_API IAssetDocumentCancellationView
{
public:
	virtual ~IAssetDocumentCancellationView() = default;
	virtual bool IsCancellationRequested() const = 0;
};

class ASSETDOCUMENT_API IAssetDocumentAtomicTransaction
{
public:
	virtual ~IAssetDocumentAtomicTransaction() = default;
	virtual FGuid GetTransactionId() const = 0;
	virtual EAssetDocumentAtomicState GetState() const = 0;
	virtual const FAssetDocumentMutationToken& GetMutationToken() const = 0;
	virtual FAssetDocumentContractValidationResult EnlistFootprint(
		TConstArrayView<FAssetDocumentMutationFootprint> Footprint) = 0;
	virtual FAssetDocumentContractValidationResult MarkReadyToCommit() = 0;
	virtual FAssetDocumentContractValidationResult Commit(
		const FAssetDocumentDurableCompletionReceipt& Receipt) = 0;
	virtual FAssetDocumentContractValidationResult Rollback(FStringView StableFailureCode) = 0;
	virtual FAssetDocumentContractValidationResult EnlistRollbackAction(
		TSharedRef<IAssetDocumentRollbackAction> Action) = 0;
	virtual FAssetDocumentContractValidationResult EnlistPostRollbackFinalizer(
		TSharedRef<IAssetDocumentPostRollbackFinalizer> Finalizer) = 0;
};

class ASSETDOCUMENT_API IAssetDocumentAtomicTransactionFactory
{
public:
	virtual ~IAssetDocumentAtomicTransactionFactory() = default;
	virtual TUniquePtr<IAssetDocumentAtomicTransaction> BeginLiveAfterPreflight(
		const FAssetDocumentPreflightReceipt& Receipt,
		IAssetDocumentCancellationView& Cancellation) = 0;
};

namespace UE::AssetDocument::AtomicTransaction::Private
{
inline FAssetDocumentContractValidationResult MakeValidationResult(
	bool bValid,
	const TCHAR* Code,
	const TCHAR* Message)
{
	FAssetDocumentContractValidationResult Result;
	Result.bValid = bValid;
	if (!bValid)
	{
		FAssetDocumentRuntimeDiagnostic& Diagnostic = Result.Diagnostics.AddDefaulted_GetRef();
		Diagnostic.Path = TEXT("/PreflightReceipt");
		Diagnostic.Code = Code;
		Diagnostic.Message = Message;
	}
	return Result;
}

inline bool IsValidFootprintKind(EAssetDocumentMutationFootprintKind Kind)
{
	return static_cast<uint8>(Kind) <= static_cast<uint8>(EAssetDocumentMutationFootprintKind::Handle);
}

inline bool IsValidAtomicState(EAssetDocumentAtomicState State)
{
	return static_cast<uint8>(State) <= static_cast<uint8>(EAssetDocumentAtomicState::Poisoned);
}

inline FAssetDocumentContractValidationResult ValidateReceiptIdentity(
	const FAssetDocumentPreflightReceipt& Receipt)
{
	if (!Receipt.bSucceeded)
	{
		return MakeValidationResult(false, TEXT("PreflightReceipt.NotSucceeded"), TEXT("Preflight must succeed before a live transaction can begin."));
	}
	if (!Receipt.RequestId.IsValid())
	{
		return MakeValidationResult(false, TEXT("PreflightReceipt.InvalidRequestId"), TEXT("Preflight receipt requires a valid request identity."));
	}
	if (Receipt.RuntimeRevision == 0)
	{
		return MakeValidationResult(false, TEXT("PreflightReceipt.InvalidRuntimeRevision"), TEXT("Preflight receipt requires a non-zero runtime revision."));
	}
	return MakeValidationResult(true, TEXT(""), TEXT(""));
}

inline FAssetDocumentContractValidationResult ValidateReceiptShape(
	const FAssetDocumentPreflightReceipt& Receipt)
{
	if (Receipt.ReceiptHash.IsEmpty())
	{
		return MakeValidationResult(false, TEXT("PreflightReceipt.EmptyReceiptHash"), TEXT("Preflight receipt requires a stable receipt hash."));
	}
	if (Receipt.CompleteFootprint.IsEmpty())
	{
		return MakeValidationResult(false, TEXT("PreflightReceipt.EmptyFootprint"), TEXT("Preflight receipt requires a complete mutation footprint."));
	}
	return MakeValidationResult(true, TEXT(""), TEXT(""));
}

inline FAssetDocumentContractValidationResult ValidateFootprintItem(
	const FAssetDocumentMutationFootprint& Item)
{
	if (!IsValidFootprintKind(Item.Kind))
	{
		return MakeValidationResult(false, TEXT("PreflightReceipt.InvalidFootprintKind"), TEXT("Mutation footprint kind is outside the frozen enum."));
	}
	if (Item.StableId.IsEmpty())
	{
		return MakeValidationResult(false, TEXT("PreflightReceipt.EmptyStableId"), TEXT("Every mutation footprint item requires a stable identity."));
	}
	if (Item.RollbackComponentId.IsEmpty())
	{
		return MakeValidationResult(
			false,
			TEXT("PreflightReceipt.EmptyRollbackComponentId"),
			TEXT("Every mutation footprint item requires a rollback component identity."));
	}
	return MakeValidationResult(true, TEXT(""), TEXT(""));
}

inline FAssetDocumentContractValidationResult ValidateCompleteFootprint(
	TConstArrayView<FAssetDocumentMutationFootprint> Footprint)
{
	TSet<FString> StableIds;
	for (const FAssetDocumentMutationFootprint& Item : Footprint)
	{
		const FAssetDocumentContractValidationResult ItemResult = ValidateFootprintItem(Item);
		if (!ItemResult.bValid)
		{
			return ItemResult;
		}
		if (StableIds.Contains(Item.StableId))
		{
			return MakeValidationResult(false, TEXT("PreflightReceipt.DuplicateStableId"), TEXT("Mutation footprint stable identities must be unique."));
		}
		StableIds.Add(Item.StableId);
	}
	return MakeValidationResult(true, TEXT(""), TEXT(""));
}
} // namespace UE::AssetDocument::AtomicTransaction::Private

inline bool IsAssetDocumentAtomicTransitionAllowed(
	EAssetDocumentAtomicState From,
	EAssetDocumentAtomicState To)
{
	using namespace UE::AssetDocument::AtomicTransaction::Private;
	if (!IsValidAtomicState(From) || !IsValidAtomicState(To))
	{
		return false;
	}
	static constexpr bool Allowed[6][6] = {
		{false, true, false, false, true, true},
		{false, false, true, false, true, true},
		{false, false, false, true, true, true},
		{false, false, false, false, false, false},
		{false, false, false, false, false, false},
		{false, false, false, false, false, false},
	};
	return Allowed[static_cast<uint8>(From)][static_cast<uint8>(To)];
}

inline FAssetDocumentContractValidationResult ValidateAssetDocumentPreflightReceipt(
	const FAssetDocumentPreflightReceipt& Receipt)
{
	using namespace UE::AssetDocument::AtomicTransaction::Private;
	const FAssetDocumentContractValidationResult Identity = ValidateReceiptIdentity(Receipt);
	if (!Identity.bValid)
	{
		return Identity;
	}
	const FAssetDocumentContractValidationResult Shape = ValidateReceiptShape(Receipt);
	if (!Shape.bValid)
	{
		return Shape;
	}
	return ValidateCompleteFootprint(Receipt.CompleteFootprint);
}
