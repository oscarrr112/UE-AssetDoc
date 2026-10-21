// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

#include "Provider/Curve/AssetDocumentAnimationCurveOperations.h"

#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/AnimSequenceBase.h"
#include "AssetDocumentEditorNativeState.h"
#include "Provider/AssetDocumentAnimationProviderCommon.h"
#include "Provider/Curve/AssetDocumentAnimationFloatCurveSnapshot.h"

namespace AssetDocumentAnimationCurveOperations
{
// These internal helpers preserve the exact v1 input and identity semantics.
bool ParseRequestedCurveNames(const FAssetDocumentPrimitiveInvocation&, TSet<FName>&);
FAssetDocumentContractValidationResult MakeIdentityConsumerResult(bool, FStringView);
}

FAssetDocumentPrimitiveOutcome BuildAnimationFloatCurveEnumerationV2(
    UObject& Target,
    const FAssetDocumentPrimitiveInvocation& Invocation)
{
    using namespace AssetDocumentAnimationCurveOperations;
    TSet<FName> RequestedNames;
    if (!ParseRequestedCurveNames(Invocation, RequestedNames))
    {
        return MakeAnimationProviderFailure(TEXT("AnimationProvider.InvalidValueShape"),
            TEXT("Float-curve enumeration requires one exact curve_names array."));
    }
    UAnimSequenceBase* Sequence = Cast<UAnimSequenceBase>(&Target);
    if (Sequence == nullptr)
    {
        return MakeAnimationProviderFailure(TEXT("AnimationProvider.TargetClassMismatch"),
            TEXT("Float-curve enumeration requires a UAnimSequenceBase target."));
    }
    const IAnimationDataModel* Model = Sequence->GetDataModel();
    if (Model == nullptr)
    {
        return MakeAnimationProviderFailure(TEXT("AnimationProvider.Curve.DataModelUnavailable"),
            TEXT("The Animation asset has no authoritative data model."));
    }
    FAssetDocumentRuntimeValueDto Curves;
    if (!CaptureAnimationFloatCurveSnapshotV2(Model->GetFloatCurves(), Curves))
    {
        return MakeAnimationProviderFailure(TEXT("AnimationProvider.ReadbackMismatch"),
            TEXT("The native float curves are outside the lossless v2 snapshot domain."));
    }
    if (!RequestedNames.IsEmpty())
    {
        Curves.Elements.RemoveAll([&](const FAssetDocumentRuntimeValueDto& Row)
        {
            return !RequestedNames.Contains(FName(*Row.Elements[0].StringValue));
        });
    }
    FAssetDocumentPrimitiveOutcome Outcome;
    Outcome.Status = EAssetDocumentPrimitiveOutcomeStatus::Succeeded;
    Outcome.Value.Kind = EAssetDocumentRuntimeValueKind::Object;
    Outcome.Value.ObjectKeys = {TEXT("curves")};
    Outcome.Value.Elements.Add(MoveTemp(Curves));
    return Outcome;
}

FAssetDocumentPrimitiveOutcome InvokeAnimationFloatCurveEnumerateV2(
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
        return MakeAnimationProviderFailure(TEXT("AnimationProvider.InvalidHandlePayload"),
            TEXT("The Animation asset handle does not provide a verified lease."));
    }
    TOptional<FAssetDocumentPrimitiveOutcome> Enumeration;
    const FAssetDocumentContractValidationResult IdentityResult =
        WithAssetDocumentEditorAssetIdentity(*LeaseResult.Lease,
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
                Enumeration.Emplace(BuildAnimationFloatCurveEnumerationV2(Object, Invocation));
                return MakeIdentityConsumerResult(true, TEXT(""));
            });
    if (!IdentityResult.bValid || !Enumeration.IsSet())
    {
        return MakeAnimationProviderFailure(TEXT("AnimationProvider.InvalidHandlePayload"),
            TEXT("The verified lease does not expose an Animation asset identity."));
    }
    return MoveTemp(Enumeration.GetValue());
}
