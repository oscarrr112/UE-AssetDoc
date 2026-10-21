// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#pragma once

#include "AssetDocumentEditorNativeState.h"
#include "AssetDocumentRollbackAction.h"
#include "Runtime/AssetDocumentAtomicTransaction.h"
#include "Runtime/AssetDocumentHandleRegistry.h"

namespace UE::AssetDocument::AI::TypedPreimage
{
inline FAssetDocumentRuntimeDiagnostic MakeDiagnostic(
	const TCHAR* Path,
	const TCHAR* Code,
	const TCHAR* Message)
{
	FAssetDocumentRuntimeDiagnostic Diagnostic;
	Diagnostic.Path = Path;
	Diagnostic.Code = Code;
	Diagnostic.Message = Message;
	return Diagnostic;
}

inline FAssetDocumentContractValidationResult MakeValidationFailure(
	const TCHAR* Path,
	const TCHAR* Code,
	const TCHAR* Message)
{
	FAssetDocumentContractValidationResult Result;
	Result.Diagnostics.Add(MakeDiagnostic(Path, Code, Message));
	return Result;
}

inline FAssetDocumentContractValidationResult MakeValidationSuccess()
{
	FAssetDocumentContractValidationResult Result;
	Result.bValid = true;
	return Result;
}

inline FAssetDocumentEditorTypedPreimageResult MakePreimageFailure(
	const TCHAR* Path,
	const TCHAR* Code,
	const TCHAR* Message)
{
	FAssetDocumentEditorTypedPreimageResult Result;
	Result.Diagnostics.Add(MakeDiagnostic(Path, Code, Message));
	return Result;
}

inline FAssetDocumentEditorTypedPreimageResult MakePreimageSuccess()
{
	FAssetDocumentEditorTypedPreimageResult Result;
	Result.bSucceeded = true;
	return Result;
}

inline bool IsEditorGameThread()
{
	return GIsEditor && IsInGameThread();
}

inline bool IsAdmittedObject(const UObject* Object)
{
	return IsValid(Object)
		&& !Object->IsTemplate()
		&& !Object->HasAnyFlags(
			RF_ClassDefaultObject
			| RF_ArchetypeObject
			| RF_BeginDestroyed
			| RF_FinishDestroyed);
}

inline FAssetDocumentEditorTypedPreimageResult ValidateCommonCapture(
	const TCHAR* Path,
	const IAssetDocumentVerifiedHandleLease& Lease,
	const FAssetDocumentEditorAssetIdentityView& AssetIdentity,
	IAssetDocumentAtomicTransaction& Transaction,
	TConstArrayView<FString> RollbackComponentIds)
{
	if (!IsEditorGameThread())
	{
		return MakePreimageFailure(
			Path,
			TEXT("AITypedPreimage.InvalidExecutionContext"),
			TEXT("AI typed preimage capture requires the Editor game thread."));
	}
	const EAssetDocumentAtomicState State = Transaction.GetState();
	if ((State != EAssetDocumentAtomicState::Live
			&& State != EAssetDocumentAtomicState::Mutating)
		|| !Transaction.GetTransactionId().IsValid())
	{
		return MakePreimageFailure(
			Path,
			TEXT("AITypedPreimage.TransactionStateInvalid"),
			TEXT("AI typed preimage capture requires a live active transaction."));
	}
	const FAssetDocumentOpaqueHandle& Handle = Lease.GetHandle();
	const FAssetDocumentMutationToken& Token = Transaction.GetMutationToken();
	if (Handle.Kind != EAssetDocumentHandleKind::Asset
		|| Handle.Access != EAssetDocumentHandleAccess::Mutable
		|| Handle.RequestId != Token.RequestId
		|| Handle.TransactionId != Token.TransactionId
		|| Handle.TransactionId != Transaction.GetTransactionId()
		|| Handle.RuntimeRevision != Token.RuntimeRevision)
	{
		return MakePreimageFailure(
			Path,
			TEXT("AITypedPreimage.TransactionBindingMismatch"),
			TEXT("The verified mutable Asset lease is not bound to the active transaction token."));
	}
	UObject& Asset = AssetIdentity.GetObject();
	if (!IsAdmittedObject(&Asset)
		|| Asset.GetPathName() != AssetIdentity.GetCanonicalObjectPath()
		|| static_cast<uint64>(Asset.GetUniqueID())
			!= AssetIdentity.GetTypeRevision())
	{
		return MakePreimageFailure(
			Path,
			TEXT("AITypedPreimage.AssetIdentityStale"),
			TEXT("The verified Asset identity changed before typed capture."));
	}
	if (RollbackComponentIds.IsEmpty())
	{
		return MakePreimageFailure(
			Path,
			TEXT("AITypedPreimage.RollbackComponentInvalid"),
			TEXT("Typed preimage capture requires rollback component IDs."));
	}
	TSet<FString> UniqueIds;
	for (const FString& ComponentId : RollbackComponentIds)
	{
		if (ComponentId.IsEmpty() || UniqueIds.Contains(ComponentId))
		{
			return MakePreimageFailure(
				Path,
				TEXT("AITypedPreimage.RollbackComponentInvalid"),
				TEXT("Rollback component IDs must be non-empty and unique."));
		}
		UniqueIds.Add(ComponentId);
	}
	return MakePreimageSuccess();
}

class FAliasRollbackState
{
public:
	virtual ~FAliasRollbackState() = default;

	void Arm()
	{
		bArmed = true;
	}

	FAssetDocumentContractValidationResult Restore()
	{
		if (!bArmed)
		{
			return MakeValidationSuccess();
		}
		if (!bConsumed)
		{
			bConsumed = true;
			CachedResult = RestoreOnce();
		}
		return CachedResult;
	}

protected:
	virtual FAssetDocumentContractValidationResult RestoreOnce() = 0;

private:
	bool bArmed = false;
	bool bConsumed = false;
	FAssetDocumentContractValidationResult CachedResult;
};

class FAliasRollbackAction final : public IAssetDocumentRollbackAction
{
public:
	FAliasRollbackAction(
		FString InComponentId,
		TSharedRef<FAliasRollbackState> InState)
		: ComponentId(MoveTemp(InComponentId))
		, State(MoveTemp(InState))
	{
	}

	virtual FStringView GetRollbackComponentId() const override
	{
		return ComponentId;
	}

	virtual FAssetDocumentContractValidationResult Rollback(
		FStringView StableFailureCode) override
	{
		(void)StableFailureCode;
		return State->Restore();
	}

private:
	FString ComponentId;
	TSharedRef<FAliasRollbackState> State;
};

inline FAssetDocumentEditorTypedPreimageResult EnlistAliasActions(
	const TCHAR* Path,
	TConstArrayView<FString> RollbackComponentIds,
	const TSharedRef<FAliasRollbackState>& State,
	IAssetDocumentAtomicTransaction& Transaction)
{
	for (const FString& ComponentId : RollbackComponentIds)
	{
		const FAssetDocumentContractValidationResult Enlisted =
			Transaction.EnlistRollbackAction(
				MakeShared<FAliasRollbackAction>(ComponentId, State));
		if (!Enlisted.bValid)
		{
			return MakePreimageFailure(
				Path,
				TEXT("AITypedPreimage.RollbackActionEnlistmentFailed"),
				TEXT("The active transaction rejected an AI typed rollback action."));
		}
	}
	State->Arm();
	return MakePreimageSuccess();
}

inline FAssetDocumentEditorTypedPreimageResult IdentityFailure(
	const FAssetDocumentContractValidationResult& Validation)
{
	FAssetDocumentEditorTypedPreimageResult Result;
	Result.Diagnostics = Validation.Diagnostics;
	return Result;
}

namespace Blackboard
{
ASSETDOCUMENTAI_API bool AreAuthoredTextsIdenticalForRollback(
	const FText& Left,
	const FText& Right);
}
} // namespace UE::AssetDocument::AI::TypedPreimage
