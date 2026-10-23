// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "Provider/Asset/AssetDocumentAnimationAssetOperations.h"

#include "Animation/AnimationAsset.h"
#include "Animation/AnimMetaData.h"
#include "AssetDocumentEditorNativeState.h"
#include "Provider/AssetDocumentAnimationProviderCommon.h"

namespace AssetDocumentAnimationAssetOperations
{
const FName ObservedSubobjectPayloadType(
	TEXT("AssetDocument.Animation.ObservedSubobject.v1"));

class FObservedSubobjectPayload final : public IAssetDocumentNativeHandlePayload
{
public:
	explicit FObservedSubobjectPayload(UObject& InObject)
		: Object(&InObject)
		, StableObjectId(InObject.GetPathName())
		, ClassPath(InObject.GetClass()->GetPathName())
	{
	}

	virtual FName GetPayloadTypeId() const override
	{
		return ObservedSubobjectPayloadType;
	}

	virtual bool IsAlive() const override
	{
		return Object.IsValid()
			&& Object->GetPathName() == StableObjectId
			&& Object->GetClass()->GetPathName() == ClassPath;
	}

private:
	TWeakObjectPtr<UObject> Object;
	FString StableObjectId;
	FString ClassPath;
};

FAssetDocumentRuntimeValueDto MakeStringValue(FString Value)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::String;
	Result.StringValue = MoveTemp(Value);
	return Result;
}

FAssetDocumentRuntimeValueDto MakeObjectValue(
	TArray<FString> Keys,
	TArray<FAssetDocumentRuntimeValueDto> Elements)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::Object;
	Result.ObjectKeys = MoveTemp(Keys);
	Result.Elements = MoveTemp(Elements);
	return Result;
}

FAssetDocumentRuntimeValueDto MakeArrayValue(
	TArray<FAssetDocumentRuntimeValueDto> Elements)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::Array;
	Result.Elements = MoveTemp(Elements);
	return Result;
}

FAssetDocumentRuntimeValueDto MakeIdentityRow(const UObject& Object)
{
	return MakeObjectValue(
		{TEXT("stable_id"), TEXT("class_path"), TEXT("name")},
		{MakeStringValue(Object.GetPathName()),
			MakeStringValue(Object.GetClass()->GetPathName()),
			MakeStringValue(Object.GetName())});
}

bool HasExactEmptyArguments(
	const FAssetDocumentPrimitiveInvocation& Invocation)
{
	return Invocation.ValueArguments.Kind ==
			EAssetDocumentRuntimeValueKind::Object
		&& Invocation.ValueArguments.ObjectKeys.IsEmpty()
		&& Invocation.ValueArguments.Elements.IsEmpty();
}

FAssetDocumentPrimitiveOutcome BuildEnumeration(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	FStringView ResultName,
	FStringView DomainDiagnostic,
	TConstArrayView<UObject*> Objects)
{
	TArray<FAssetDocumentRuntimeValueDto> Rows;
	TArray<FAssetDocumentOpaqueHandle> ChildHandles;
	Rows.Reserve(Objects.Num());
	ChildHandles.Reserve(Objects.Num());
	for (UObject* Object : Objects)
	{
		if (!IsValid(Object))
		{
			return MakeAnimationProviderFailure(
				DomainDiagnostic,
				TEXT("The authoritative Animation attachment array contains an invalid object."));
		}
		FAssetDocumentIssuedHandleResult Issued = Handles.Issue(
			EAssetDocumentHandleKind::Subobject,
			EAssetDocumentHandleAccess::ReadOnly,
			MakeUnique<FObservedSubobjectPayload>(*Object));
		if (!Issued.Handle.IsSet())
		{
			return MakeAnimationProviderFailure(
				TEXT("AnimationProvider.OperationRejected"),
				TEXT("The Animation attachment child handle could not be issued."));
		}
		Rows.Add(MakeIdentityRow(*Object));
		ChildHandles.Add(Issued.Handle.GetValue());
	}

	FAssetDocumentPrimitiveOutcome Outcome;
	Outcome.Status = EAssetDocumentPrimitiveOutcomeStatus::Succeeded;
	Outcome.Value = MakeObjectValue(
		{FString(ResultName)}, {MakeArrayValue(MoveTemp(Rows))});
	FAssetDocumentPrimitiveHandleResult& HandleResult =
		Outcome.HandleResults.AddDefaulted_GetRef();
	HandleResult.Name = FString(ResultName);
	HandleResult.Handles = MoveTemp(ChildHandles);
	return Outcome;
}

FAssetDocumentContractValidationResult MakeIdentityConsumerResult(
	bool bValid,
	FStringView Message)
{
	FAssetDocumentContractValidationResult Result;
	Result.bValid = bValid;
	if (!bValid)
	{
		FAssetDocumentRuntimeDiagnostic& Diagnostic =
			Result.Diagnostics.AddDefaulted_GetRef();
		Diagnostic.Path = TEXT("/Primitive/Handles/asset");
		Diagnostic.Code = TEXT("AnimationProvider.InvalidHandlePayload");
		Diagnostic.Message = FString(Message);
	}
	return Result;
}

using FAssetEnumerationBuilder = FAssetDocumentPrimitiveOutcome (*) (
	UObject&,
	const FAssetDocumentPrimitiveInvocation&,
	IAssetDocumentHandleResolver&);

FAssetDocumentPrimitiveOutcome InvokeAssetEnumeration(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	FAssetEnumerationBuilder Builder)
{
	FAssetDocumentVerifiedLeaseResult LeaseResult = AcquireAnimationInputLease(
		Handles, Invocation, TEXT("asset"), EAssetDocumentHandleKind::Asset,
		EAssetDocumentHandleAccess::ReadOnly);
	if (!LeaseResult.Lease)
	{
		return MakeAnimationProviderFailure(
			TEXT("AnimationProvider.InvalidHandlePayload"),
			TEXT("The Animation asset handle does not provide a verified lease."));
	}

	TOptional<FAssetDocumentPrimitiveOutcome> Enumeration;
	const FAssetDocumentContractValidationResult IdentityResult =
		WithAssetDocumentEditorAssetIdentity(
			*LeaseResult.Lease,
			[&](const FAssetDocumentEditorAssetIdentityView& Identity)
			{
				UObject& Object = Identity.GetObject();
				if (Identity.GetTypeRevision() == 0
					|| Identity.GetCanonicalObjectPath().IsEmpty()
					|| Identity.GetCanonicalObjectPath() != Object.GetPathName())
				{
					return MakeIdentityConsumerResult(false,
						TEXT("The verified Animation asset identity is inconsistent."));
				}
				Enumeration.Emplace(Builder(Object, Invocation, Handles));
				return MakeIdentityConsumerResult(true, TEXT(""));
			});
	if (!IdentityResult.bValid || !Enumeration.IsSet())
	{
		return MakeAnimationProviderFailure(
			TEXT("AnimationProvider.InvalidHandlePayload"),
			TEXT("The verified lease does not expose an Animation asset identity."));
	}
	return MoveTemp(Enumeration.GetValue());
}
} // namespace AssetDocumentAnimationAssetOperations

FAssetDocumentPrimitiveOutcome BuildAnimationMetadataEnumeration(
	UObject& Target,
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles)
{
	using namespace AssetDocumentAnimationAssetOperations;
	if (!HasExactEmptyArguments(Invocation))
	{
		return MakeAnimationProviderFailure(
			TEXT("AnimationProvider.InvalidValueShape"),
			TEXT("Metadata enumeration requires an exact empty object."));
	}
	UAnimationAsset* AnimationAsset = Cast<UAnimationAsset>(&Target);
	if (AnimationAsset == nullptr)
	{
		return MakeAnimationProviderFailure(
			TEXT("AnimationProvider.TargetClassMismatch"),
			TEXT("Metadata enumeration requires a UAnimationAsset target."));
	}
	TArray<UObject*> Objects;
	for (UObject* Metadata : AnimationAsset->GetMetaData())
	{
		Objects.Add(Metadata);
	}
	return BuildEnumeration(Invocation, Handles, TEXT("metadata"),
		TEXT("AnimationProvider.Metadata.EnumerationFailed"), Objects);
}

FAssetDocumentPrimitiveOutcome BuildAnimationAssetUserDataEnumeration(
	UObject& Target,
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles)
{
	using namespace AssetDocumentAnimationAssetOperations;
	if (!HasExactEmptyArguments(Invocation))
	{
		return MakeAnimationProviderFailure(
			TEXT("AnimationProvider.InvalidValueShape"),
			TEXT("Asset-user-data enumeration requires an exact empty object."));
	}
	UAnimationAsset* AnimationAsset = Cast<UAnimationAsset>(&Target);
	if (AnimationAsset == nullptr)
	{
		return MakeAnimationProviderFailure(
			TEXT("AnimationProvider.TargetClassMismatch"),
			TEXT("Asset-user-data enumeration requires a UAnimationAsset target."));
	}
	TArray<UObject*> Objects;
	if (const TArray<UAssetUserData*>* UserData =
		AnimationAsset->GetAssetUserDataArray())
	{
		for (UObject* Item : *UserData)
		{
			Objects.Add(Item);
		}
	}
	return BuildEnumeration(Invocation, Handles, TEXT("user_data"),
		TEXT("AnimationProvider.AssetUserData.EnumerationFailed"), Objects);
}

FAssetDocumentPrimitiveOutcome InvokeAnimationMetadataEnumerate(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	IAssetDocumentAtomicTransaction*)
{
	return AssetDocumentAnimationAssetOperations::InvokeAssetEnumeration(
		Invocation, Handles, &BuildAnimationMetadataEnumeration);
}

FAssetDocumentPrimitiveOutcome InvokeAnimationAssetUserDataEnumerate(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	IAssetDocumentAtomicTransaction*)
{
	return AssetDocumentAnimationAssetOperations::InvokeAssetEnumeration(
		Invocation, Handles, &BuildAnimationAssetUserDataEnumeration);
}
