// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "Provider/Curve/AssetDocumentAnimationCurveOperations.h"

#include "Animation/AnimCurveTypes.h"
#include "Animation/AnimData/CurveIdentifier.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/AnimSequenceBase.h"
#include "AssetDocumentEditorNativeState.h"
#include "Provider/AssetDocumentAnimationProviderCommon.h"

namespace AssetDocumentAnimationCurveOperations
{
const FName FloatCurvePayloadType(
	TEXT("AssetDocument.Animation.FloatCurve.v1"));

class FFloatCurvePayload final : public IAssetDocumentNativeHandlePayload
{
public:
	FFloatCurvePayload(
		UAnimSequenceBase& InOwner,
		const FFloatCurve& InCurve)
		: Owner(&InOwner)
		, Curve(&InCurve)
		, CurveName(InCurve.GetName())
	{
	}

	virtual FName GetPayloadTypeId() const override
	{
		return FloatCurvePayloadType;
	}

	virtual bool IsAlive() const override
	{
		const UAnimSequenceBase* Sequence = Owner.Get();
		if (Sequence == nullptr)
		{
			return false;
		}
		const IAnimationDataModel* Model = Sequence->GetDataModel();
		return Model != nullptr
			&& Model->FindFloatCurve(FAnimationCurveIdentifier(
				CurveName, ERawCurveTrackTypes::RCT_Float)) == Curve;
	}

private:
	TWeakObjectPtr<UAnimSequenceBase> Owner;
	const FFloatCurve* Curve = nullptr;
	FName CurveName;
};

FAssetDocumentRuntimeValueDto MakeStringValue(FString Value)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::String;
	Result.StringValue = MoveTemp(Value);
	return Result;
}

FAssetDocumentRuntimeValueDto MakeNumberValue(double Value)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::Number;
	Result.NumberValue = Value;
	return Result;
}

FAssetDocumentRuntimeValueDto MakeIntegerValue(int64 Value)
{
	FAssetDocumentRuntimeValueDto Result;
	Result.Kind = EAssetDocumentRuntimeValueKind::Integer;
	Result.IntegerValue = Value;
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

FAssetDocumentRuntimeValueDto MakeCurveKeyValue(const FRichCurveKey& Key)
{
	return MakeObjectValue(
		{TEXT("time"), TEXT("value"), TEXT("arrive_tangent"),
			TEXT("leave_tangent"), TEXT("interp_mode"), TEXT("tangent_mode"),
			TEXT("tangent_weight_mode")},
		{MakeNumberValue(Key.Time), MakeNumberValue(Key.Value),
			MakeNumberValue(Key.ArriveTangent), MakeNumberValue(Key.LeaveTangent),
			MakeIntegerValue(Key.InterpMode), MakeIntegerValue(Key.TangentMode),
			MakeIntegerValue(Key.TangentWeightMode)});
}

FAssetDocumentRuntimeValueDto MakeCurveValue(const FFloatCurve& Curve)
{
	TArray<FAssetDocumentRuntimeValueDto> Keys;
	const TArray<FRichCurveKey>& AuthoritativeKeys =
		Curve.FloatCurve.GetConstRefOfKeys();
	Keys.Reserve(AuthoritativeKeys.Num());
	for (const FRichCurveKey& Key : AuthoritativeKeys)
	{
		Keys.Add(MakeCurveKeyValue(Key));
	}
	return MakeObjectValue(
		{TEXT("name"), TEXT("flags"), TEXT("keys")},
		{MakeStringValue(Curve.GetName().ToString()),
			MakeIntegerValue(Curve.GetCurveTypeFlags()),
			MakeArrayValue(MoveTemp(Keys))});
}

bool ParseRequestedCurveNames(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	TSet<FName>& OutNames)
{
	const FAssetDocumentRuntimeValueDto& Arguments = Invocation.ValueArguments;
	if (Arguments.Kind != EAssetDocumentRuntimeValueKind::Object
		|| Arguments.ObjectKeys != TArray<FString>{TEXT("curve_names")}
		|| Arguments.Elements.Num() != 1
		|| Arguments.Elements[0].Kind != EAssetDocumentRuntimeValueKind::Array)
	{
		return false;
	}
	for (const FAssetDocumentRuntimeValueDto& NameValue :
		Arguments.Elements[0].Elements)
	{
		if (NameValue.Kind != EAssetDocumentRuntimeValueKind::String
			|| NameValue.StringValue.IsEmpty())
		{
			return false;
		}
		const FName Name(*NameValue.StringValue);
		if (OutNames.Contains(Name))
		{
			return false;
		}
		OutNames.Add(Name);
	}
	return true;
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
} // namespace AssetDocumentAnimationCurveOperations

FAssetDocumentPrimitiveOutcome BuildAnimationFloatCurveEnumeration(
	UObject& Target,
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles)
{
	using namespace AssetDocumentAnimationCurveOperations;
	TSet<FName> RequestedNames;
	if (!ParseRequestedCurveNames(Invocation, RequestedNames))
	{
		return MakeAnimationProviderFailure(
			TEXT("AnimationProvider.InvalidValueShape"),
			TEXT("Float-curve enumeration requires one exact curve_names array."));
	}
	UAnimSequenceBase* Sequence = Cast<UAnimSequenceBase>(&Target);
	if (Sequence == nullptr)
	{
		return MakeAnimationProviderFailure(
			TEXT("AnimationProvider.TargetClassMismatch"),
			TEXT("Float-curve enumeration requires a UAnimSequenceBase target."));
	}
	const IAnimationDataModel* Model = Sequence->GetDataModel();
	if (Model == nullptr)
	{
		return MakeAnimationProviderFailure(
			TEXT("AnimationProvider.Curve.DataModelUnavailable"),
			TEXT("The Animation asset has no authoritative data model."));
	}

	TArray<FAssetDocumentRuntimeValueDto> CurveValues;
	TArray<FAssetDocumentOpaqueHandle> CurveHandles;
	for (const FFloatCurve& Curve : Model->GetFloatCurves())
	{
		if (!RequestedNames.IsEmpty()
			&& !RequestedNames.Contains(Curve.GetName()))
		{
			continue;
		}
		FAssetDocumentIssuedHandleResult Issued = Handles.Issue(
			EAssetDocumentHandleKind::Curve,
			EAssetDocumentHandleAccess::ReadOnly,
			MakeUnique<FFloatCurvePayload>(*Sequence, Curve));
		if (!Issued.Handle.IsSet())
		{
			return MakeAnimationProviderFailure(
				TEXT("AnimationProvider.OperationRejected"),
				TEXT("The Animation float-curve child handle could not be issued."));
		}
		CurveValues.Add(MakeCurveValue(Curve));
		CurveHandles.Add(Issued.Handle.GetValue());
	}

	FAssetDocumentPrimitiveOutcome Outcome;
	Outcome.Status = EAssetDocumentPrimitiveOutcomeStatus::Succeeded;
	Outcome.Value = MakeObjectValue(
		{TEXT("curves")}, {MakeArrayValue(MoveTemp(CurveValues))});
	FAssetDocumentPrimitiveHandleResult& HandleResult =
		Outcome.HandleResults.AddDefaulted_GetRef();
	HandleResult.Name = TEXT("curves");
	HandleResult.Handles = MoveTemp(CurveHandles);
	return Outcome;
}

FAssetDocumentPrimitiveOutcome InvokeAnimationFloatCurveEnumerate(
	const FAssetDocumentPrimitiveInvocation& Invocation,
	IAssetDocumentHandleResolver& Handles,
	IAssetDocumentAtomicTransaction*)
{
	using namespace AssetDocumentAnimationCurveOperations;
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
				Enumeration.Emplace(BuildAnimationFloatCurveEnumeration(
					Object, Invocation, Handles));
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
