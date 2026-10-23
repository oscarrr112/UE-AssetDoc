// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "TypedPreimage/AssetDocumentAISelectorTypedPreimage.h"
#include "TypedPreimage/AssetDocumentAISelectorTypedPreimageInternal.h"

FAssetDocumentEditorTypedPreimageResult
CaptureAndEnlistAssetDocumentAIBehaviorTreeSelectorPreimage(
	const IAssetDocumentVerifiedHandleLease& AssetLease,
	UObject& ExactPropertyOwner,
	TSharedRef<IPropertyRowGenerator> PropertyRows,
	TSharedRef<IPropertyHandle> SelectorPropertyHandle,
	TConstArrayView<FString> RollbackComponentIds,
	IAssetDocumentAtomicTransaction& Transaction)
{
	using namespace UE::AssetDocument::AI::TypedPreimage::Selector;
	FAssetDocumentEditorTypedPreimageResult Result;
	const FAssetDocumentContractValidationResult IdentityResult =
		WithAssetDocumentEditorAssetIdentity(
			AssetLease,
			[&](const FAssetDocumentEditorAssetIdentityView& Identity)
			{
				Result = ValidateCommonCapture(
					SelectorPath,
					AssetLease,
					Identity,
					Transaction,
					RollbackComponentIds);
				if (!Result.bSucceeded)
				{
					FAssetDocumentContractValidationResult Failure;
					Failure.Diagnostics = Result.Diagnostics;
					return Failure;
				}
				TSharedPtr<FAliasRollbackState> State;
				Result = CaptureSelectorState(Identity, ExactPropertyOwner,
					MoveTemp(PropertyRows), SelectorPropertyHandle, State);
				if (!Result.bSucceeded)
				{
					FAssetDocumentContractValidationResult Failure;
					Failure.Diagnostics = Result.Diagnostics;
					return Failure;
				}
				Result = EnlistAliasActions(
					SelectorPath, RollbackComponentIds, State.ToSharedRef(), Transaction);
				if (Result.bSucceeded)
				{
					return MakeValidationSuccess();
				}
				FAssetDocumentContractValidationResult Failure;
				Failure.Diagnostics = Result.Diagnostics;
				return Failure;
			});
	if (!IdentityResult.bValid && Result.Diagnostics.IsEmpty())
	{
		Result = IdentityFailure(IdentityResult);
	}
	return Result;
}
