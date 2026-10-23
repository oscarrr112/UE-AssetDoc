// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
#include "Provider/Curve/AssetDocumentAnimationFloatCurveExactState.h"

#include "Animation/AnimComposite.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimData/AnimDataModel.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Curves/RealCurve.h"
#include "UObject/StrongObjectPtr.h"

namespace AssetDocumentAnimationFloatCurveExactState
{
#if WITH_DEV_AUTOMATION_TESTS
AssetDocumentAnimationFloatCurveExactStateTesting::FHooks* Hooks = nullptr;
#endif

bool SameBits(const float& A, const float& B)
{
    return FMemory::Memcmp(&A, &B, sizeof(float)) == 0;
}

// Own the exact model/controller for the duration of synchronous notifications.
// Model.GetController() would create a new instance and is intentionally absent.
struct FIdentity
{
    TStrongObjectPtr<UAnimSequenceBase> Asset;
    TStrongObjectPtr<UObject> Model;
    TStrongObjectPtr<UObject> ControllerObject;
    IAnimationDataController* Controller = nullptr;

    explicit FIdentity(UAnimSequenceBase& InAsset) : Asset(&InAsset)
    {
        if (!IsInGameThread() || !IsValid(&InAsset)
            || (InAsset.GetClass() != UAnimMontage::StaticClass()
                && InAsset.GetClass() != UAnimComposite::StaticClass())) return;
        const auto Interface = InAsset.GetDataModelInterface();
        UObject* Object = Interface.GetObject();
        if (!IsValid(Object) || Object->GetClass() != UAnimDataModel::StaticClass()
            || Object->GetOuter() != &InAsset
            || Interface.GetInterface() != Cast<IAnimationDataModel>(Object)
            || Interface.GetInterface() != InAsset.GetDataModel()) return;
        Model.Reset(Object);
        auto& Candidate = InAsset.GetController();
        UObject* ControllerNative = Cast<UObject>(&Candidate);
        if (!IsValid(ControllerNative)
            || ControllerNative->GetClass()->GetPathName() != TEXT("/Script/AnimationDataController.AnimDataController")
            || Cast<IAnimationDataController>(ControllerNative) != &Candidate
            || Candidate.GetModelInterface().GetObject() != Object
            || Candidate.GetModelInterface().GetInterface() != Interface.GetInterface()
            || Candidate.GetModel() != Interface.GetInterface()) return;
        ControllerObject.Reset(ControllerNative);
        Controller = &Candidate;
    }

    bool Matches() const
    {
        if (!Controller || !IsValid(Asset.Get()) || !IsValid(Model.Get())
            || !IsValid(ControllerObject.Get())) return false;
        // Check the already captured controller first: Asset.GetController()
        // asserts model agreement before returning a mismatched controller.
        const auto Interface = Asset->GetDataModelInterface();
        if (Interface.GetObject() != Model.Get()
            || Controller->GetModelInterface().GetObject() != Model.Get()
            || Controller->GetModelInterface().GetInterface() != Interface.GetInterface()
            || Controller->GetModel() != Interface.GetInterface()) return false;
        const FIdentity Current(*Asset);
        return Current.Controller == Controller && Current.Model.Get() == Model.Get()
            && Current.ControllerObject.Get() == ControllerObject.Get();
    }
};

struct FBracket
{
    IAnimationDataController& Controller;
    bool bOpen = true;
    explicit FBracket(IAnimationDataController& InController) : Controller(InController)
    {
        Controller.OpenBracket(FText::FromString(TEXT("Replace complete float curves")), false);
#if WITH_DEV_AUTOMATION_TESTS
        if (Hooks) ++Hooks->OpenBrackets;
#endif
    }
    void Close()
    {
        if (!bOpen) return;
        bOpen = false;
        Controller.CloseBracket(false);
#if WITH_DEV_AUTOMATION_TESTS
        if (Hooks) ++Hooks->CloseBrackets;
#endif
    }
    ~FBracket() { Close(); }
};
}

#if WITH_DEV_AUTOMATION_TESTS
AssetDocumentAnimationFloatCurveExactStateTesting::FHooks* AssetDocumentAnimationFloatCurveExactStateTesting::GetHooks()
{
    return AssetDocumentAnimationFloatCurveExactState::Hooks;
}
void AssetDocumentAnimationFloatCurveExactStateTesting::SetHooks(FHooks* Hooks)
{
    AssetDocumentAnimationFloatCurveExactState::Hooks = Hooks;
}
#endif

bool IsExactAnimationFloatCurveTarget(UAnimSequenceBase& Asset)
{
    return AssetDocumentAnimationFloatCurveExactState::FIdentity(Asset).Matches();
}

bool MatchesAnimationFloatCurveState(UAnimSequenceBase& Asset,
    TConstArrayView<FAssetDocumentAnimationFloatCurveState> Desired)
{
    if (!IsExactAnimationFloatCurveTarget(Asset)) return false;
    TArray<FAssetDocumentAnimationFloatCurveState> Actual;
    return CaptureAnimationFloatCurveStateV2(Asset.GetDataModel()->GetFloatCurves(), Actual)
        && EqualAnimationFloatCurveStateV2(Actual, Desired);
}

bool PublishExactFloatCurveState(UAnimSequenceBase& Asset,
    TConstArrayView<FAssetDocumentAnimationFloatCurveState> Desired)
{
    // ASSETDOCUMENT_AUTHORITY_BRIDGE_ADMISSION(ANIMATION-FLOAT-CURVE-EXACT-STATE-v1)
    // ASSETDOCUMENT_AUTHORITY_BRIDGE_CALLSITE(ANIMATION-FLOAT-CURVE-EXACT-STATE-v1|animation.curve.float.replace.v2)
    using namespace AssetDocumentAnimationFloatCurveExactState;
    static bool bPublishing = false;
    if (!IsInGameThread() || bPublishing || !IsValidAnimationFloatCurveStateV2(Desired)) return false;
    TGuardValue<bool> Guard(bPublishing, true);
    const FIdentity Identity(Asset);
    if (!Identity.Matches()) return false;
    if (MatchesAnimationFloatCurveState(Asset, Desired)) return true;
#if WITH_DEV_AUTOMATION_TESTS
    if (Hooks) ++Hooks->PublishCalls;
#endif
    auto& Controller = *Identity.Controller;
    const auto* Model = static_cast<const UAnimDataModel*>(Identity.Model.Get());
    TArray<FName> OldNames;
    for (const auto& Row : Model->GetFloatCurves()) OldNames.Add(Row.GetName());
    FBracket Bracket(Controller);
    if (!Identity.Matches()) return false;
    // The controller has no collection reorder API. Remove/re-add only the
    // float collection in requested order; transform curves remain untouched.
    for (FName Name : OldNames)
    {
        if (!Controller.RemoveCurve(FAnimationCurveIdentifier(Name, ERawCurveTrackTypes::RCT_Float), false)
            || !Identity.Matches()) return false;
    }
    for (const auto& Row : Desired)
    {
        const FAnimationCurveIdentifier Id(Row.Name, ERawCurveTrackTypes::RCT_Float);
        if (!Controller.AddCurve(Id, Row.Flags, false) || !Identity.Matches()) return false;
        if (!Controller.SetCurveKeys(Id, Row.Keys, false) || !Identity.Matches()) return false;
#if WITH_DEV_AUTOMATION_TESTS
        if (Hooks && Hooks->bFailAfterControllerKeys) return false;
#endif
        if (!Controller.SetCurveColor(Id, Row.Color, false) || !Identity.Matches()) return false;
        if (!Controller.SetCurveComment(Id, Row.Comment, false) || !Identity.Matches()) return false;

        // C0_EXACT_STOCK_FLOAT_MEMBER: the sole authorized cast, on mutable
        // stock UAnimDataModel-owned CurveData reacquired after public edits.
        const FFloatCurve* Member = Model->FindFloatCurve(Id);
        const auto& Rows = Model->GetFloatCurves();
        const FFloatCurve* Owned = Rows.FindByPredicate([&](const FFloatCurve& C) { return C.GetName() == Row.Name; });
        if (!Member || Member != Owned || Member->FloatCurve.GetNumKeys() != Row.Keys.Num()) return false;
        const auto& Keys = Member->FloatCurve.GetConstRefOfKeys();
        TArray<FKeyHandle> Handles;
        Handles.Init(FKeyHandle::Invalid(), Row.Keys.Num());
        int32 Count = 0;
        for (auto It = Member->FloatCurve.GetKeyHandleIterator(); It; ++It)
        {
            const int32 Index = Member->FloatCurve.GetIndexSafe(*It);
            if (!Handles.IsValidIndex(Index) || Handles[Index] != FKeyHandle::Invalid()
                || !SameBits(Keys[Index].Time, Row.Keys[Index].Time)) return false;
            Handles[Index] = *It;
            ++Count;
        }
        if (Count != Row.Keys.Num()) return false;
        FRichCurve& Exact = const_cast<FRichCurve&>(Member->FloatCurve);
        if (Row.bHasDefault) Exact.SetDefaultValue(Row.DefaultValue);
        else Exact.ClearDefaultValue();
        for (int32 Index = 0; Index < Handles.Num(); ++Index)
            Exact.GetKey(Handles[Index]) = Row.Keys[Index];
#if WITH_DEV_AUTOMATION_TESTS
        if (Hooks) ++Hooks->ExactPatches;
#endif
        FCurveAttributes Attributes;
        Attributes.SetPreExtrapolation(Row.PreExtrapolation);
        Attributes.SetPostExtrapolation(Row.PostExtrapolation);
        // Stock public controller emits CurveChanged without AutoSetTangents.
        if (!Controller.SetCurveAttributes(Id, Attributes, false) || !Identity.Matches()) return false;
#if WITH_DEV_AUTOMATION_TESTS
        if (Hooks && Hooks->bFailAfterFirstPatch
            && (!Hooks->ShouldFailAfterFirstPatch || Hooks->ShouldFailAfterFirstPatch(Asset))) return false;
#endif
    }
    Bracket.Close();
    return Identity.Matches() && MatchesAnimationFloatCurveState(Asset, Desired);
}
