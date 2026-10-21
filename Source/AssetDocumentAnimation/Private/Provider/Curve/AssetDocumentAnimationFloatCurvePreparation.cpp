// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
#include "Provider/Curve/AssetDocumentAnimationFloatCurveMutationState.h"

namespace AssetDocumentAnimationFloatCurveMutation
{
namespace
{
struct FCapture
{
 FGuid Transaction;
 bool bOpened = false, bClosed = false, bForwardWritten = false;
};
struct FBinding
{
 TSharedPtr<FPrepared> State;
 TSharedPtr<FCapture> Capture;
 FAssetDocumentRuntimeRequestDto Request;
 FAssetDocumentMutationIntent Intent;
};
// Only the currently open, non-reentrant game-thread family capture is visible.
// No request cache, handle registry, or alternative transaction owner exists here.
const FBinding* ActiveCapture = nullptr;
FAssetDocumentContractValidationResult Validate(const FBinding& Binding,
 const FAssetDocumentPrimitiveInvocation& I, IAssetDocumentHandleResolver& Handles)
{
 const auto& C = *Binding.Capture; auto& S = *Binding.State;
 if (!ContextValid() || !C.bOpened || C.bClosed || C.bForwardWritten || I.TransactionId != C.Transaction
  || I.RequestId != Binding.Request.RequestId || I.RuntimeRevision != Binding.Request.ExpectedRuntimeRevision
  || I.Phase != Binding.Intent.Phase)
  return Result(false, TEXT("AnimationProvider.OperationRejected"));
 TArray<FCurveState> Desired;
 if (!ValidateAssetDocumentPrimitiveInvocation(I, MakeAnimationFloatCurveReplaceDescriptor()).bValid
  || !ExactHandles(I) || !Parse(I.ValueArguments, Desired) || !EqualAnimationFloatCurveStateV2(Desired, S.Desired))
  return Result(false, TEXT("AnimationProvider.InvalidValueShape"));
 TUniquePtr<IAssetDocumentVerifiedHandleLease> Lease; UObject* Native = nullptr;
 UClass* Class = S.ClassPath == TEXT("/Script/Engine.AnimMontage") ? UAnimMontage::StaticClass() : UAnimComposite::StaticClass();
 auto Valid = Acquire(I, Handles, TEXT("asset"), Class, EAssetDocumentHandleAccess::Mutable, Lease, Native);
 if (!Valid.bValid) return Valid;
 if (Native->GetPathName() != S.TargetPath || Native->GetOutermost()->GetName() != S.PackagePath
  || FSoftObjectPath(S.TargetPath).ResolveObject() != Native) return Result(false);
 if (S.bCreate && !S.Target.IsValid())
 {
  // Bind the real factory result once; Core's factory action owns create rollback.
  if (!S.Bind(*static_cast<UAnimSequenceBase*>(Native))) return Result(false);
 }
 return Result(Native == S.Target.Get() && S.BaselineCurrent());
}
class FFloatCurveInverse final : public IAssetDocumentRollbackAction
{
public:
 explicit FFloatCurveInverse(TSharedRef<FBinding> InBinding) : Binding(MoveTemp(InBinding)) {}
 FStringView GetRollbackComponentId() const override { return RollbackId; }
 FAssetDocumentContractValidationResult Rollback(FStringView) override
 {
  if (bConsumed) return Result(false, TEXT("AnimationProvider.Curve.RollbackFailed"));
  bConsumed = true; auto& C = *Binding->Capture; auto& S = *Binding->State;
  if (!ContextValid() || !C.bOpened || !C.bClosed) return Result(false, TEXT("AnimationProvider.Curve.RollbackFailed"));
  if (!C.bForwardWritten) return Result(true);
  if (!S.IdentityCurrent()) return Result(false, TEXT("AnimationProvider.Curve.RollbackFailed"));
  const bool Restored = PublishExactFloatCurveState(*S.Target, S.Old);
  return Result(Restored && S.RetainedCurrent() && MatchesAnimationFloatCurveState(*S.Target, S.Old), TEXT("AnimationProvider.Curve.RollbackFailed"));
 }
private:
 TSharedRef<FBinding> Binding;
 bool bConsumed = false;
};
}
FAssetDocumentContractValidationResult AcquirePrepared(const FAssetDocumentPrimitiveInvocation& I,
 IAssetDocumentHandleResolver& Handles, TSharedPtr<FPrepared>& OutState)
{
 OutState.Reset();
 if (!ContextValid() || !ActiveCapture) return Result(false, TEXT("AnimationProvider.OperationRejected"));
 auto Valid = Validate(*ActiveCapture, I, Handles);
 if (Valid.bValid) OutState = ActiveCapture->State;
 return Valid;
}
FAssetDocumentContractValidationResult MarkWrite(const FAssetDocumentPrimitiveInvocation& I,
 IAssetDocumentHandleResolver& Handles, TSharedPtr<FPrepared>& OutState)
{
 auto Valid = AcquirePrepared(I, Handles, OutState);
 if (Valid.bValid) ActiveCapture->Capture->bForwardWritten = true;
 return Valid;
}
}
FAssetDocumentContractValidationResult PrepareAnimationFloatCurveMutationBatch(
 const FAssetDocumentRuntimeRequestDto& Request, TConstArrayView<FAssetDocumentMutationIntent> Intents,
 FAssetDocumentPreparedMutationBatch& OutBatch)
{
 using namespace AssetDocumentAnimationFloatCurveMutation;
 OutBatch = FAssetDocumentPreparedMutationBatch(); static bool bPreparing = false;
 if (!ContextValid() || bPreparing || !Request.RequestId.IsValid() || Request.ExpectedRuntimeRevision == 0
  || Request.Operation != EAssetDocumentRuntimeOperation::Apply || !CanonicalPath(Request.TargetAssetPath)
  || (Request.RequestedAssetClassPath != TEXT("/Script/Engine.AnimMontage") && Request.RequestedAssetClassPath != TEXT("/Script/Engine.AnimComposite")))
  return Result(false, TEXT("AnimationProvider.OperationRejected"));
 TGuardValue<bool> Guard(bPreparing, true);
 if (Intents.Num() > 1) return Result(false, TEXT("AnimationProvider.InvalidValueShape"));
 if (Intents.IsEmpty()) return Result(true);
 const auto& Intent = Intents[0];
 if (Intent.StepIndex < 0 || Intent.Requirement.PrimitiveId != Primitive || Intent.Requirement.Version != 2
  || Intent.Requirement.RequiredByRouteId != TEXT("animation_profile") || Intent.InputSchemaId != Schema
  || (Intent.Phase != EAssetDocumentRuntimeRequestPhase::Apply && Intent.Phase != EAssetDocumentRuntimeRequestPhase::Repair))
  return Result(false, TEXT("AnimationProvider.InvalidValueShape"));
 auto State = MakeShared<FPrepared>();
 if (!Parse(Intent.Intent, State->Desired)) return Result(false, TEXT("AnimationProvider.InvalidValueShape"));
 State->TargetPath = Request.TargetAssetPath; State->ClassPath = Request.RequestedAssetClassPath;
 State->PackagePath = FSoftObjectPath(State->TargetPath).GetLongPackageName();
 UObject* Target = FSoftObjectPath(State->TargetPath).ResolveObject(); State->bCreate = Target == nullptr;
 if (!State->bCreate && (!TargetClass(Target) || Target->GetClass()->GetPathName() != State->ClassPath
  || !State->Bind(*static_cast<UAnimSequenceBase*>(Target)))) return Result(false, TEXT("AnimationProvider.Curve.UnsupportedPreimage"));
 if (!State->BaselineCurrent()) return Result(false);
 auto Binding = MakeShared<FBinding>(); Binding->State = State; Binding->Capture = MakeShared<FCapture>();
 Binding->Request = Request; Binding->Intent = Intent;
 FAssetDocumentPreparedMutationBatch Prepared;
 Prepared.OpenMutationCapture = [Binding](const FGuid& Transaction, TArray<FAssetDocumentRuntimeDiagnostic>& Diagnostics)
  -> TUniquePtr<FAssetDocumentCoreMutationCaptureLease>
 {
  auto& C = *Binding->Capture;
  if (!ContextValid() || !Transaction.IsValid() || ActiveCapture || C.bOpened || C.bClosed || !Binding->State->BaselineCurrent())
  { Diagnostics.Append(Result(false, TEXT("AnimationProvider.OperationRejected")).Diagnostics); return nullptr; }
  C.Transaction = Transaction; C.bOpened = true; ActiveCapture = &Binding.Get();
  return MakeUnique<FAssetDocumentCoreMutationCaptureLease>([Binding]()
  { if (ActiveCapture == &Binding.Get()) ActiveCapture = nullptr; Binding->Capture->bClosed = true; });
 };
 auto& Contribution = Prepared.Contributions.AddDefaulted_GetRef(); Contribution.StepIndex = Intent.StepIndex;
 Contribution.OwnedSurfaceIds = {State->TargetPath + TEXT("::FloatCurves")};
 Contribution.RawFootprint = Footprint(State->TargetPath, State->PackagePath);
 if (!State->bCreate) Contribution.InitialAction = MakeShared<FFloatCurveInverse>(Binding);
 Contribution.ValidateInvocation = [Binding](const auto& I, auto& Handles) { return Validate(*Binding, I, Handles); };
 OutBatch = MoveTemp(Prepared); return Result(true);
}
