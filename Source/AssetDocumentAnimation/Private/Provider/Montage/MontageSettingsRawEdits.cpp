// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
#include "Provider/Montage/MontageSettingsRawEdits.h"
#include "Animation/AnimMetaData.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/TimeStretchCurve.h"
#include "UObject/UnrealType.h"

namespace AssetDocumentMontageSettingsRaw
{
namespace
{
FFloatProperty* TimeStretchField(const TCHAR* Name)
{
 return FindFProperty<FFloatProperty>(FTimeStretchCurve::StaticStruct(), Name);
}
}

bool GetTimeStretchConfig(const UAnimMontage& Montage, float& OutSamplingRate, float& OutPrecision)
{
 const FFloatProperty* Rate = TimeStretchField(TEXT("SamplingRate"));
 const FFloatProperty* Precision = TimeStretchField(TEXT("CurveValueMinPrecision"));
 if (!Rate || !Precision) return false;
 OutSamplingRate = Rate->GetPropertyValue_InContainer(&Montage.TimeStretchCurve);
 OutPrecision = Precision->GetPropertyValue_InContainer(&Montage.TimeStretchCurve);
 return true;
}

bool SetTimeStretchConfig(UAnimMontage& Montage, const float* SamplingRate, const float* Precision)
{
 FFloatProperty* RateField = TimeStretchField(TEXT("SamplingRate"));
 FFloatProperty* PrecisionField = TimeStretchField(TEXT("CurveValueMinPrecision"));
 if (!RateField || !PrecisionField) return false;
 if (SamplingRate) RateField->SetPropertyValue_InContainer(&Montage.TimeStretchCurve, *SamplingRate);
 if (Precision) PrecisionField->SetPropertyValue_InContainer(&Montage.TimeStretchCurve, *Precision);
 return true;
}

void BakeTimeStretchCurve(UAnimMontage& Montage)
{
 Montage.TimeStretchCurve.Reset();
 const IAnimationDataModel* Model = Montage.GetDataModel();
 const FFloatCurve* Curve = Model
  ? Model->FindFloatCurve(FAnimationCurveIdentifier(Montage.TimeStretchCurveName, ERawCurveTrackTypes::RCT_Float))
  : nullptr;
 if (Curve) Montage.TimeStretchCurve.BakeFromFloatCurve(*Curve, Montage.GetPlayLength());
}

void RefreshDerivedSyncState(UAnimMontage& Montage)
{
 FProperty* SyncGroup = FindFProperty<FProperty>(UAnimMontage::StaticClass(), GET_MEMBER_NAME_CHECKED(UAnimMontage, SyncGroup));
 FPropertyChangedEvent Event(SyncGroup, EPropertyChangeType::ValueSet);
 static_cast<UObject&>(Montage).PostEditChangeProperty(Event);
}

UAnimMetaData* NewInstancedMetaData(UAnimMontage& Montage, UClass& Class)
{
 return NewObject<UAnimMetaData>(&Montage, &Class, NAME_None, RF_Transactional);
}
}
