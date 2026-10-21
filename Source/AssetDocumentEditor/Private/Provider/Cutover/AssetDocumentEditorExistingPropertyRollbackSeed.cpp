// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "Provider/Cutover/AssetDocumentEditorExistingPropertyRollbackSeed.h"

#include "Provider/Property/AssetDocumentEditorPropertyAdmission.h"

namespace
{
FAssetDocumentContractValidationResult MakeExistingSeedFailure(
	const TCHAR* Code,
	const TCHAR* Message)
{
	FAssetDocumentContractValidationResult Result;
	FAssetDocumentRuntimeDiagnostic& Diagnostic =
		Result.Diagnostics.AddDefaulted_GetRef();
	Diagnostic.Path = TEXT("/ExistingPropertyRollbackSeed");
	Diagnostic.Code = Code;
	Diagnostic.Message = Message;
	return Result;
}

class FAssetDocumentEditorExistingRollbackSeedState;

class FAssetDocumentEditorExistingRollbackCapture final
	: public IAssetDocumentEditorCutoverMutationCapture
{
public:
	explicit FAssetDocumentEditorExistingRollbackCapture(
		TSharedRef<FAssetDocumentEditorExistingRollbackSeedState> InState);
	virtual ~FAssetDocumentEditorExistingRollbackCapture() override;

private:
	TSharedRef<FAssetDocumentEditorExistingRollbackSeedState> State;
};

class FAssetDocumentEditorExistingRollbackSeedState final
	: public TSharedFromThis<
		FAssetDocumentEditorExistingRollbackSeedState>
{
public:
	FAssetDocumentEditorExistingRollbackSeedState(
		FAssetDocumentEditorPropertyPreimage&& InPropertyPreimage,
		TConstArrayView<FString> InExpectedComponents)
		: PropertyPreimage(
			MakeUnique<FAssetDocumentEditorPropertyPreimage>(
				MoveTemp(InPropertyPreimage)))
	{
		AddExpectedComponents(InExpectedComponents);
	}

	FAssetDocumentEditorExistingRollbackSeedState(
		TSharedRef<IAssetDocumentEditorCutoverRollbackSeed> InCreatedSeed,
		TConstArrayView<FString> InExpectedComponents)
		: CreatedSeed(MoveTemp(InCreatedSeed))
	{
		AddExpectedComponents(InExpectedComponents);
	}

	bool Open(FStringView ComponentId, const FGuid& TransactionId)
	{
		if (!IsOpenRequestValid(ComponentId, TransactionId)
			|| !FreezeOrMatchTransaction(TransactionId))
		{
			return PoisonOpen();
		}
		OpenedComponents.Add(FString(ComponentId));
		++ActiveLeaseCount;
		return true;
	}

	void CloseLease()
	{
		if (ActiveLeaseCount <= 0 || bCaptureClosed)
		{
			bPoisoned = true;
			return;
		}
		--ActiveLeaseCount;
		if (ActiveLeaseCount == 0)
		{
			UnderlyingCapture.Reset();
			bCaptureClosed = true;
		}
	}

	FAssetDocumentContractValidationResult Rollback(
		FStringView StableFailureCode)
	{
		if (bHasCachedResult)
		{
			return CachedResult;
		}
		bConsumed = true;
		const TOptional<FAssetDocumentContractValidationResult> Failure =
			CacheRollbackReadinessFailure();
		if (Failure.IsSet())
		{
			return Failure.GetValue();
		}
		return CacheResult(RunUnderlyingRollback(StableFailureCode));
	}

private:
	void AddExpectedComponents(TConstArrayView<FString> Components)
	{
		for (const FString& Component : Components)
		{
			ExpectedComponents.Add(Component);
		}
	}

	bool IsOpenContextValid(const FGuid& TransactionId) const
	{
		return IsInGameThread() && GIsEditor && TransactionId.IsValid();
	}

	bool IsStateOpenable() const
	{
		return !bCaptureClosed && !bConsumed && !bPoisoned;
	}

	bool IsComponentOpenable(FStringView ComponentId) const
	{
		const FString Component(ComponentId);
		return ExpectedComponents.Contains(Component)
			&& !OpenedComponents.Contains(Component);
	}

	bool IsOpenRequestValid(
		FStringView ComponentId,
		const FGuid& TransactionId) const
	{
		return IsOpenContextValid(TransactionId)
			&& IsStateOpenable()
			&& IsComponentOpenable(ComponentId);
	}

	bool FreezeOrMatchTransaction(const FGuid& TransactionId)
	{
		if (!bHasTransaction)
		{
			return BeginUnderlyingCapture(TransactionId);
		}
		return FrozenTransactionId == TransactionId;
	}

	bool BeginUnderlyingCapture(const FGuid& TransactionId)
	{
		bHasTransaction = true;
		FrozenTransactionId = TransactionId;
		if (!CreatedSeed.IsValid())
		{
			return PropertyPreimage.IsValid()
				&& PropertyPreimage->IsReady();
		}
		UnderlyingCapture =
			CreatedSeed->OpenMutationCapture(TransactionId);
		return UnderlyingCapture.IsValid();
	}

	bool PoisonOpen()
	{
		bPoisoned = true;
		return false;
	}

	TOptional<FAssetDocumentContractValidationResult>
	CacheRollbackReadinessFailure()
	{
		if (bPoisoned || ActiveLeaseCount != 0)
		{
			return CacheFailure(
				TEXT("ExistingPropertyRollback.InvalidState"),
				TEXT("Rollback requires one closed, valid shared capture."));
		}
		if (OpenedComponents.Num() != ExpectedComponents.Num()
			|| !bCaptureClosed)
		{
			return CacheFailure(
				TEXT("ExistingPropertyRollback.MissingCapture"),
				TEXT("Rollback requires every expected component capture."));
		}
		return {};
	}

	FAssetDocumentContractValidationResult RunUnderlyingRollback(
		FStringView StableFailureCode)
	{
		if (PropertyPreimage.IsValid())
		{
			return PropertyPreimage->Restore();
		}
		if (CreatedSeed.IsValid())
		{
			return CreatedSeed->Rollback(StableFailureCode);
		}
		return MakeExistingSeedFailure(
			TEXT("ExistingPropertyRollback.InvalidState"),
			TEXT("The shared rollback state has no provider operation."));
	}

	FAssetDocumentContractValidationResult CacheFailure(
		const TCHAR* Code,
		const TCHAR* Message)
	{
		return CacheResult(MakeExistingSeedFailure(Code, Message));
	}

	FAssetDocumentContractValidationResult CacheResult(
		FAssetDocumentContractValidationResult Result)
	{
		CachedResult = MoveTemp(Result);
		bHasCachedResult = true;
		return CachedResult;
	}

	TUniquePtr<FAssetDocumentEditorPropertyPreimage> PropertyPreimage;
	TSharedPtr<IAssetDocumentEditorCutoverRollbackSeed> CreatedSeed;
	TUniquePtr<IAssetDocumentEditorCutoverMutationCapture> UnderlyingCapture;
	TSet<FString> ExpectedComponents;
	TSet<FString> OpenedComponents;
	FGuid FrozenTransactionId;
	FAssetDocumentContractValidationResult CachedResult;
	int32 ActiveLeaseCount = 0;
	bool bHasTransaction = false;
	bool bCaptureClosed = false;
	bool bPoisoned = false;
	bool bConsumed = false;
	bool bHasCachedResult = false;
};

FAssetDocumentEditorExistingRollbackCapture::
	FAssetDocumentEditorExistingRollbackCapture(
		TSharedRef<FAssetDocumentEditorExistingRollbackSeedState> InState)
	: State(MoveTemp(InState))
{
}

FAssetDocumentEditorExistingRollbackCapture::
	~FAssetDocumentEditorExistingRollbackCapture()
{
	State->CloseLease();
}

class FAssetDocumentEditorExistingRollbackSeed final
	: public IAssetDocumentEditorCutoverRollbackSeed
{
public:
	FAssetDocumentEditorExistingRollbackSeed(
		FString InComponentId,
		TSharedRef<FAssetDocumentEditorExistingRollbackSeedState> InState)
		: ComponentId(MoveTemp(InComponentId))
		, State(MoveTemp(InState))
	{
	}

	virtual FStringView GetRollbackComponentId() const override
	{
		return ComponentId;
	}

	virtual TUniquePtr<IAssetDocumentEditorCutoverMutationCapture>
	OpenMutationCapture(const FGuid& TransactionId) override
	{
		if (!State->Open(ComponentId, TransactionId))
		{
			return nullptr;
		}
		return MakeUnique<FAssetDocumentEditorExistingRollbackCapture>(
			State);
	}

	virtual FAssetDocumentContractValidationResult Rollback(
		FStringView StableFailureCode) override
	{
		return State->Rollback(StableFailureCode);
	}

private:
	FString ComponentId;
	TSharedRef<FAssetDocumentEditorExistingRollbackSeedState> State;
};

TArray<TSharedRef<IAssetDocumentEditorCutoverRollbackSeed>>
MakeSharedRollbackSeeds(
	TSharedRef<FAssetDocumentEditorExistingRollbackSeedState> State,
	TConstArrayView<FString> ComponentIds)
{
	TArray<TSharedRef<IAssetDocumentEditorCutoverRollbackSeed>> Result;
	for (const FString& ComponentId : ComponentIds)
	{
		Result.Add(
			MakeShared<FAssetDocumentEditorExistingRollbackSeed>(
				ComponentId, State));
	}
	return Result;
}
}

TArray<TSharedRef<IAssetDocumentEditorCutoverRollbackSeed>>
MakeAssetDocumentEditorExistingPropertyRollbackSeeds(
	FAssetDocumentEditorPropertyPreimage&& PropertyPreimage)
{
	const TArray<FString> ComponentIds = {
		TEXT("rollback.uobject"),
		TEXT("rollback.package"),
	};
	return MakeSharedRollbackSeeds(
		MakeShared<FAssetDocumentEditorExistingRollbackSeedState>(
			MoveTemp(PropertyPreimage), ComponentIds),
		ComponentIds);
}

TArray<TSharedRef<IAssetDocumentEditorCutoverRollbackSeed>>
MakeAssetDocumentEditorCombinedRollbackSeeds(
	TSharedRef<IAssetDocumentEditorCutoverRollbackSeed> CreatedSeed)
{
	const TArray<FString> ComponentIds = {
		TEXT("editor.asset.rollback_created.v1"),
		TEXT("rollback.uobject"),
		TEXT("rollback.package"),
	};
	return MakeSharedRollbackSeeds(
		MakeShared<FAssetDocumentEditorExistingRollbackSeedState>(
			MoveTemp(CreatedSeed), ComponentIds),
		ComponentIds);
}
