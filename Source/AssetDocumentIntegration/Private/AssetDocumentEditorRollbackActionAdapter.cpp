// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "AssetDocumentEditorRollbackActionAdapter.h"

namespace
{
FAssetDocumentContractValidationResult MakeAlreadyConsumedResult()
{
	FAssetDocumentContractValidationResult Result;
	Result.bValid = false;
	FAssetDocumentRuntimeDiagnostic& Diagnostic =
		Result.Diagnostics.AddDefaulted_GetRef();
	Diagnostic.Path = TEXT("/EditorRollbackActionAdapter");
	Diagnostic.Code = TEXT("EditorRollbackActionAdapter.AlreadyConsumed");
	Diagnostic.Message =
		TEXT("Editor rollback action has already been consumed.");
	return Result;
}

class FAssetDocumentEditorRollbackActionAdapter final
	: public IAssetDocumentRollbackAction
{
public:
	explicit FAssetDocumentEditorRollbackActionAdapter(
		TSharedRef<IAssetDocumentEditorCutoverRollbackSeed> InSeed,
		bool bInPackageSnapshotRestores)
		: Seed(MoveTemp(InSeed))
		, ComponentId(Seed->GetRollbackComponentId())
		, bPackageSnapshotRestores(bInPackageSnapshotRestores)
	{
	}

	virtual FStringView GetRollbackComponentId() const override
	{
		return ComponentId;
	}

	virtual FAssetDocumentContractValidationResult Rollback(
		FStringView StableFailureCode) override
	{
		if (bConsumed)
		{
			return MakeAlreadyConsumedResult();
		}
		bConsumed = true;
		if (bPackageSnapshotRestores)
		{
			FAssetDocumentContractValidationResult Covered;
			Covered.bValid = true;
			return Covered;
		}
		return Seed->Rollback(StableFailureCode);
	}

private:
	TSharedRef<IAssetDocumentEditorCutoverRollbackSeed> Seed;
	FString ComponentId;
	bool bPackageSnapshotRestores = false;
	bool bConsumed = false;
};
}

TArray<TSharedRef<IAssetDocumentRollbackAction>>
AdaptAssetDocumentEditorRollbackSeeds(
	TConstArrayView<
		TSharedRef<IAssetDocumentEditorCutoverRollbackSeed>> Seeds,
	bool bPackageSnapshotRestores)
{
	TArray<TSharedRef<IAssetDocumentRollbackAction>> Actions;
	Actions.Reserve(Seeds.Num());
	for (const TSharedRef<IAssetDocumentEditorCutoverRollbackSeed>& Seed :
		Seeds)
	{
		Actions.Add(
			MakeShared<FAssetDocumentEditorRollbackActionAdapter>(
				Seed, bPackageSnapshotRestores));
	}
	return Actions;
}

bool IsAssetDocumentGenericRuntimeRollbackEligibleFailure(
	FStringView StableFailureCode)
{
	return !StableFailureCode.IsEmpty()
		&& StableFailureCode != TEXTVIEW("GenericRuntime.NotImplemented");
}
