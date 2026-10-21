// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
#pragma once

#include "Animation/AnimMontage.h"
#include "Animation/AnimNotifies/AnimNotify.h"
#include "Animation/AnimNotifies/AnimNotifyState.h"

namespace AssetDocumentMontage
{
// A name alone never grants replacement authority. State events take precedence
// over event objects, matching the native mixed-timeline family classification.
inline bool IsManagedMontageNotifyEvent(const UAnimMontage& Target, const FAnimNotifyEvent& Event, bool bState)
{
 if ((Event.NotifyStateClass != nullptr) != bState) return false;
 const UObject* Object = bState ? static_cast<UObject*>(Event.NotifyStateClass.Get()) : static_cast<UObject*>(Event.Notify.Get());
 return Object && Object->GetOuter() == &Target
  && Event.NotifyName == (bState ? FName(TEXT("AssetDocument.NotifyState")) : FName(TEXT("AssetDocument.Notify")))
  && Object->GetName().StartsWith(bState ? TEXT("AssetDocumentManaged_NotifyState_") : TEXT("AssetDocumentManaged_Notify_"));
}
}
