// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "CoreMinimal.h"
#include "Runtime/AssetDocumentRuntimeTypes.h"

enum class EAssetDocumentHandleInvalidationReason : uint8
{
	Commit = 0,
	Rollback = 1,
	Compile = 2,
	Rebuild = 3,
	Reinstance = 4,
	GarbageCollection = 5,
	PackageUnload = 6,
	PackageReload = 7,
	RuntimeRevisionSwap = 8,
	ProviderUnload = 9,
};

struct ASSETDOCUMENT_API FAssetDocumentHandleValidationContext
{
	FGuid RequestId;
	FGuid TransactionId;
	uint64 RuntimeRevision = 0;
	FString ProviderId;
	EAssetDocumentRuntimeRequestPhase Phase = EAssetDocumentRuntimeRequestPhase::Validate;
	EAssetDocumentHandleKind RequiredKind = EAssetDocumentHandleKind::Object;
	EAssetDocumentHandleAccess RequiredAccess = EAssetDocumentHandleAccess::ReadOnly;
};

struct ASSETDOCUMENT_API FAssetDocumentHandleInvalidation
{
	EAssetDocumentHandleInvalidationReason Reason = EAssetDocumentHandleInvalidationReason::Commit;
	FGuid RequestId;
	FGuid TransactionId;
	uint64 RuntimeRevision = 0;
	FString ProviderId;
};

struct ASSETDOCUMENT_API FAssetDocumentHandleInvalidationResult
{
	uint32 InvalidatedHandleCount = 0;
};

class ASSETDOCUMENT_API IAssetDocumentNativeHandlePayload
{
public:
	virtual ~IAssetDocumentNativeHandlePayload() = default;
	virtual FName GetPayloadTypeId() const = 0;
	virtual bool IsAlive() const = 0;
};

class ASSETDOCUMENT_API IAssetDocumentVerifiedHandleLease
{
public:
	virtual ~IAssetDocumentVerifiedHandleLease() = default;
	virtual const FAssetDocumentOpaqueHandle& GetHandle() const = 0;
	virtual IAssetDocumentNativeHandlePayload& GetPayloadChecked() const = 0;
};

struct ASSETDOCUMENT_API FAssetDocumentIssuedHandleResult
{
	FAssetDocumentIssuedHandleResult() = default;
	FAssetDocumentIssuedHandleResult(const FAssetDocumentIssuedHandleResult&) = delete;
	FAssetDocumentIssuedHandleResult& operator=(const FAssetDocumentIssuedHandleResult&) = delete;
	FAssetDocumentIssuedHandleResult(FAssetDocumentIssuedHandleResult&&) = default;
	FAssetDocumentIssuedHandleResult& operator=(FAssetDocumentIssuedHandleResult&&) = default;

	TOptional<FAssetDocumentOpaqueHandle> Handle;
	TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
};

struct ASSETDOCUMENT_API FAssetDocumentVerifiedLeaseResult
{
	TUniquePtr<IAssetDocumentVerifiedHandleLease> Lease;
	TArray<FAssetDocumentRuntimeDiagnostic> Diagnostics;
};

class ASSETDOCUMENT_API IAssetDocumentHandleResolver
{
public:
	virtual ~IAssetDocumentHandleResolver() = default;
	virtual FAssetDocumentIssuedHandleResult Issue(
		EAssetDocumentHandleKind Kind,
		EAssetDocumentHandleAccess Access,
		TUniquePtr<IAssetDocumentNativeHandlePayload>&& Payload) = 0;
	virtual FAssetDocumentVerifiedLeaseResult AcquireVerifiedLease(
		const FAssetDocumentOpaqueHandle& Handle,
		const FAssetDocumentHandleValidationContext& Context) = 0;
	virtual FAssetDocumentHandleInvalidationResult Invalidate(
		const FAssetDocumentHandleInvalidation& Event) = 0;
};

namespace UE::AssetDocument::HandleRegistry::Private
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
		Diagnostic.Path = TEXT("/Handle");
		Diagnostic.Code = Code;
		Diagnostic.Message = Message;
	}
	return Result;
}

inline bool HasMatchingRequestIdentity(
	const FAssetDocumentOpaqueHandle& Handle,
	const FAssetDocumentHandleValidationContext& Context)
{
	return Handle.RequestId.IsValid()
		&& Context.RequestId.IsValid()
		&& Handle.RequestId == Context.RequestId;
}

inline bool HasMatchingTransactionIdentity(
	const FAssetDocumentOpaqueHandle& Handle,
	const FAssetDocumentHandleValidationContext& Context)
{
	return Handle.TransactionId.IsValid()
		&& Context.TransactionId.IsValid()
		&& Handle.TransactionId == Context.TransactionId;
}

inline bool HasMatchingRuntimeRevision(
	const FAssetDocumentOpaqueHandle& Handle,
	const FAssetDocumentHandleValidationContext& Context)
{
	return Handle.RuntimeRevision > 0
		&& Context.RuntimeRevision > 0
		&& Handle.RuntimeRevision == Context.RuntimeRevision;
}

inline bool HasMatchingProvider(
	const FAssetDocumentOpaqueHandle& Handle,
	const FAssetDocumentHandleValidationContext& Context)
{
	return !Handle.ProviderId.IsEmpty()
		&& !Context.ProviderId.IsEmpty()
		&& Handle.ProviderId == Context.ProviderId;
}

inline FAssetDocumentContractValidationResult ValidateIdentity(
	const FAssetDocumentOpaqueHandle& Handle,
	const FAssetDocumentHandleValidationContext& Context)
{
	if (!HasMatchingRequestIdentity(Handle, Context))
	{
		return MakeValidationResult(false, TEXT("HandleMetadata.RequestMismatch"), TEXT("Handle request identity does not match the validation context."));
	}
	if (!HasMatchingTransactionIdentity(Handle, Context))
	{
		return MakeValidationResult(false, TEXT("HandleMetadata.TransactionMismatch"), TEXT("Handle transaction identity does not match the validation context."));
	}
	if (!HasMatchingRuntimeRevision(Handle, Context))
	{
		return MakeValidationResult(false, TEXT("HandleMetadata.RuntimeRevisionMismatch"), TEXT("Handle runtime revision does not match the validation context."));
	}
	if (!HasMatchingProvider(Handle, Context))
	{
		return MakeValidationResult(false, TEXT("HandleMetadata.ProviderMismatch"), TEXT("Handle provider does not match the validation context."));
	}
	return MakeValidationResult(true, TEXT(""), TEXT(""));
}

inline bool IsValidPhase(EAssetDocumentRuntimeRequestPhase Phase)
{
	return static_cast<uint8>(Phase) <= static_cast<uint8>(EAssetDocumentRuntimeRequestPhase::Failed);
}

inline bool IsValidKind(EAssetDocumentHandleKind Kind)
{
	return static_cast<uint8>(Kind) <= static_cast<uint8>(EAssetDocumentHandleKind::Scratch);
}

inline bool IsValidAccess(EAssetDocumentHandleAccess Access)
{
	return static_cast<uint8>(Access) <= static_cast<uint8>(EAssetDocumentHandleAccess::Mutable);
}

inline bool AreHandleKindsValid(
	const FAssetDocumentOpaqueHandle& Handle,
	const FAssetDocumentHandleValidationContext& Context)
{
	return IsValidKind(Handle.Kind) && IsValidKind(Context.RequiredKind);
}

inline bool AreHandleAccessValuesValid(
	const FAssetDocumentOpaqueHandle& Handle,
	const FAssetDocumentHandleValidationContext& Context)
{
	return IsValidAccess(Handle.Access) && IsValidAccess(Context.RequiredAccess);
}

inline FAssetDocumentContractValidationResult ValidateEnums(
	const FAssetDocumentOpaqueHandle& Handle,
	const FAssetDocumentHandleValidationContext& Context)
{
	if (!IsValidPhase(Context.Phase))
	{
		return MakeValidationResult(false, TEXT("HandleMetadata.InvalidPhase"), TEXT("Handle validation phase is outside the frozen request-phase enum."));
	}
	if (!AreHandleKindsValid(Handle, Context))
	{
		return MakeValidationResult(false, TEXT("HandleMetadata.InvalidKind"), TEXT("Handle kind metadata is outside the frozen handle-kind enum."));
	}
	if (!AreHandleAccessValuesValid(Handle, Context))
	{
		return MakeValidationResult(false, TEXT("HandleMetadata.InvalidAccess"), TEXT("Handle access metadata is outside the frozen access enum."));
	}
	return MakeValidationResult(true, TEXT(""), TEXT(""));
}

inline FAssetDocumentContractValidationResult ValidateRequirements(
	const FAssetDocumentOpaqueHandle& Handle,
	const FAssetDocumentHandleValidationContext& Context)
{
	if (Handle.Kind != Context.RequiredKind)
	{
		return MakeValidationResult(false, TEXT("HandleMetadata.KindMismatch"), TEXT("Handle kind does not match the required kind."));
	}
	if (Handle.Access != Context.RequiredAccess)
	{
		return MakeValidationResult(false, TEXT("HandleMetadata.AccessMismatch"), TEXT("Handle access does not match the required access."));
	}
	return MakeValidationResult(true, TEXT(""), TEXT(""));
}

inline FAssetDocumentContractValidationResult ValidateStorage(const FAssetDocumentOpaqueHandle& Handle)
{
	if (Handle.Slot == 0)
	{
		return MakeValidationResult(false, TEXT("HandleMetadata.InvalidSlot"), TEXT("Handle slot must be non-zero."));
	}
	if (Handle.Generation == 0)
	{
		return MakeValidationResult(false, TEXT("HandleMetadata.InvalidGeneration"), TEXT("Handle generation must be non-zero."));
	}
	return MakeValidationResult(true, TEXT(""), TEXT(""));
}
} // namespace UE::AssetDocument::HandleRegistry::Private

inline FAssetDocumentContractValidationResult ValidateAssetDocumentHandleMetadata(
	const FAssetDocumentOpaqueHandle& Handle,
	const FAssetDocumentHandleValidationContext& Context)
{
	using namespace UE::AssetDocument::HandleRegistry::Private;
	const FAssetDocumentContractValidationResult Identity = ValidateIdentity(Handle, Context);
	if (!Identity.bValid)
	{
		return Identity;
	}
	const FAssetDocumentContractValidationResult Enums = ValidateEnums(Handle, Context);
	if (!Enums.bValid)
	{
		return Enums;
	}
	const FAssetDocumentContractValidationResult Requirements = ValidateRequirements(Handle, Context);
	if (!Requirements.bValid)
	{
		return Requirements;
	}
	return ValidateStorage(Handle);
}
