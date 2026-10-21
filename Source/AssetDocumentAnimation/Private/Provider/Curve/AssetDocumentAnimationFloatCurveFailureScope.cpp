// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
#include "AssetDocumentAnimationMutationPreparation.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "Provider/Curve/AssetDocumentAnimationFloatCurveExactState.h"
#include "Animation/AnimSequenceBase.h"
#include "Misc/PackageName.h"
#include "UObject/SoftObjectPath.h"

struct FAssetDocumentAnimationFloatCurveFailureScope::FState
{
 AssetDocumentAnimationFloatCurveExactStateTesting::FHooks Hooks;
 FString TargetPath;
 bool bInstalled = false;
 bool bTriggered = false;
};

FAssetDocumentAnimationFloatCurveFailureScope::FAssetDocumentAnimationFloatCurveFailureScope(const FString& ExactTargetPath)
 : State(MakeUnique<FState>())
{
 using namespace AssetDocumentAnimationFloatCurveExactStateTesting;
 const FSoftObjectPath Path(ExactTargetPath);
 if (!IsInGameThread() || GetHooks() || !FPackageName::IsValidObjectPath(ExactTargetPath)
  || !Path.IsValid() || !Path.GetSubPathString().IsEmpty() || Path.ToString() != ExactTargetPath) return;
 State->TargetPath = ExactTargetPath;
 State->Hooks.bFailAfterFirstPatch = true;
 State->Hooks.ShouldFailAfterFirstPatch = [Bound = State.Get()](UAnimSequenceBase& Asset)
 {
  if (Bound->bTriggered || Asset.GetPathName() != Bound->TargetPath) return false;
  Bound->bTriggered = true;
  return true;
 };
 SetHooks(&State->Hooks);
 State->bInstalled = true;
}

FAssetDocumentAnimationFloatCurveFailureScope::~FAssetDocumentAnimationFloatCurveFailureScope()
{
 using namespace AssetDocumentAnimationFloatCurveExactStateTesting;
 // Never replace a hook installed by another owner. Successful acquisition had
 // no previous hook, so null is the exact previous state to restore.
 if (State->bInstalled && GetHooks() == &State->Hooks) SetHooks(nullptr);
}
bool FAssetDocumentAnimationFloatCurveFailureScope::IsArmed() const
{
 return State->bInstalled && AssetDocumentAnimationFloatCurveExactStateTesting::GetHooks() == &State->Hooks;
}
bool FAssetDocumentAnimationFloatCurveFailureScope::WasTriggered() const
{
 return State->bTriggered;
}
#endif
