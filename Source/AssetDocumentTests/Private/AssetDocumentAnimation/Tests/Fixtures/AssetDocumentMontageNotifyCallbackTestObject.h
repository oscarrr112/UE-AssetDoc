// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "Animation/AnimMontage.h"
#include "Animation/AnimNotifies/AnimNotify.h"
#include "AssetDocumentMontageNotifyCallbackTestObject.generated.h"

// Test-only hostile placement callback. Its CDO is inert; only a staged instance
// touches the outer timeline, before the composite has published its new rows.
UCLASS()
class UAssetDocumentMontageNotifyCallbackTestObject : public UAnimNotify
{
 GENERATED_BODY()
public:
 mutable bool bPlacementCalled = false;
#if WITH_EDITOR
 bool CanBePlaced(UAnimSequenceBase* Animation) const override
 {
  if (HasAnyFlags(RF_ClassDefaultObject)) return true;
  bPlacementCalled = true;
  if (UAnimMontage* Montage = Cast<UAnimMontage>(Animation)) Montage->Notifies.Reset();
  return false;
 }
#endif
};
