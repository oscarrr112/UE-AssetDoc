// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "Provider/Property/AssetDocumentEditorPropertyBatchTokenPrimitive.h"

#include "Provider/Property/AssetDocumentEditorPropertyAdmission.h"
#include "Provider/Property/AssetDocumentEditorPropertyHandlers.h"
#include "Runtime/AssetDocumentHandleRegistry.h"

namespace
{
FAssetDocumentPrimitiveOutcome MakePropertyBatchFailure(
	const TCHAR* Code,
	const TCHAR* Message)
{
	FAssetDocumentPrimitiveOutcome Outcome;
	Outcome.Status = EAssetDocumentPrimitiveOutcomeStatus::Failed;
	Outcome.Value.Kind = EAssetDocumentRuntimeValueKind::Object;
	FAssetDocumentRuntimeDiagnostic& Diagnostic =
		Outcome.Diagnostics.AddDefaulted_GetRef();
	Diagnostic.Path = TEXT("/Primitive");
	Diagnostic.Code = Code;
	Diagnostic.Message = Message;
	return Outcome;
}

bool HasExactEmptyPropertyBatchValue(
	const FAssetDocumentRuntimeValueDto& Value)
{
	return Value.Kind == EAssetDocumentRuntimeValueKind::Object
		&& Value.ObjectKeys.IsEmpty()
		&& Value.Elements.IsEmpty();
}

const FAssetDocumentPrimitiveHandleArgument* FindExactPropertyBatchAsset(
	const FAssetDocumentPrimitiveInvocation& Invocation)
{
	return Invocation.HandleArguments.Num() == 1
		&& Invocation.HandleArguments[0].Name == TEXT("asset")
		? &Invocation.HandleArguments[0]
		: nullptr;
}

FAssetDocumentPrimitiveOutcome PublishPropertyBatchAdmission(
	FAssetDocumentEditorPropertyAdmissionResult&& Admission)
{
	if (!Admission.bSucceeded || !Admission.PropertyBatchHandle.IsSet())
	{
		FAssetDocumentPrimitiveOutcome Failure = MakePropertyBatchFailure(
			TEXT("PropertyAdmission.TokenIssueFailed"),
			TEXT("The admitted property batch could not be built."));
		if (!Admission.Diagnostics.IsEmpty())
		{
			Failure.Diagnostics = MoveTemp(Admission.Diagnostics);
		}
		return Failure;
	}
	FAssetDocumentPrimitiveOutcome Outcome;
	Outcome.Status = EAssetDocumentPrimitiveOutcomeStatus::Succeeded;
	Outcome.Value = MoveTemp(Admission.Value);
	FAssetDocumentPrimitiveHandleResult& Properties =
		Outcome.HandleResults.AddDefaulted_GetRef();
	Properties.Name = TEXT("properties");
	Properties.Handles = MoveTemp(Admission.Handles);
	FAssetDocumentPrimitiveHandleResult& PropertyBatch =
		Outcome.HandleResults.AddDefaulted_GetRef();
	PropertyBatch.Name = TEXT("property_batch");
	PropertyBatch.Handles.Add(Admission.PropertyBatchHandle.GetValue());
	return Outcome;
}
} // namespace

FAssetDocumentPrimitiveOutcome HandleAssetDocumentEditorPropertyEnumerateAdmittedBatch(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	IAssetDocumentAtomicTransaction* ActiveTransaction)
{
	(void)ActiveTransaction;
	FAssetDocumentRuntimeValueDto Remaining;
	TArray<FString> DeclaredNames;
	if (!TrySplitAssetDocumentEditorDeclaredPropertyArguments(
			Invocation.ValueArguments, Remaining, DeclaredNames)
		|| !HasExactEmptyPropertyBatchValue(Remaining))
	{
		return MakePropertyBatchFailure(
			TEXT("PrimitiveInvocation.InvalidValueShape"),
			TEXT("Property batch enumeration accepts only optional admit_properties names."));
	}
	const FAssetDocumentPrimitiveHandleArgument* Asset =
		FindExactPropertyBatchAsset(Invocation);
	if (Asset == nullptr)
	{
		return MakePropertyBatchFailure(
			TEXT("PrimitiveInvocation.InvalidHandleArguments"),
			TEXT("Property batch enumeration requires one exact asset handle."));
	}
	TUniquePtr<IAssetDocumentVerifiedHandleLease> AssetLease;
	UObject* ExactTarget = nullptr;
	FAssetDocumentPrimitiveOutcome Failure;
	if (!TryAcquireAssetDocumentEditorPropertyBatchTarget(
		Invocation,
		Asset->Handle,
		Handles,
		AssetLease,
		ExactTarget,
		Failure))
	{
		return Failure;
	}
	return PublishPropertyBatchAdmission(
		AdmitAssetDocumentEditorPropertyBatch(*ExactTarget, Handles, DeclaredNames));
}
