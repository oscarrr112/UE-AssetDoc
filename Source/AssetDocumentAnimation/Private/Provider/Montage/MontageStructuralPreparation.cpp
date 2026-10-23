// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
#include "Provider/Montage/MontageStructuralPreparation.h"
#include "Provider/Montage/MontageStructuralRollback.h"
#include "Provider/Montage/MontageStructuralProvider.h"
#include "Provider/AssetDocumentAnimationProviderCommon.h"
#include "AssetDocumentEditorNativeState.h"
#include "UObject/SoftObjectPath.h"

namespace AssetDocumentMontage
{
namespace MontageStructuralPreparationLocal
{
FAssetDocumentRuntimeValueDto Text(const FString& Value)
{
 FAssetDocumentRuntimeValueDto R; R.Kind = EAssetDocumentRuntimeValueKind::String; R.StringValue = Value; return R;
}
FAssetDocumentRuntimeValueDto Number(double Value)
{
 FAssetDocumentRuntimeValueDto R; R.Kind = EAssetDocumentRuntimeValueKind::Number; R.NumberValue = Value; return R;
}
FAssetDocumentRuntimeValueDto Array()
{
 FAssetDocumentRuntimeValueDto R; R.Kind = EAssetDocumentRuntimeValueKind::Array; return R;
}
FAssetDocumentRuntimeValueDto Object()
{
 FAssetDocumentRuntimeValueDto R; R.Kind = EAssetDocumentRuntimeValueKind::Object; return R;
}
void Field(FAssetDocumentRuntimeValueDto& ObjectValue, const TCHAR* Name, FAssetDocumentRuntimeValueDto Value)
{
 ObjectValue.ObjectKeys.Add(Name); ObjectValue.Elements.Add(MoveTemp(Value));
}
bool EqualValues(const FAssetDocumentRuntimeValueDto& A, const FAssetDocumentRuntimeValueDto& B)
{
 if (A.Kind != B.Kind || A.BooleanValue != B.BooleanValue || A.IntegerValue != B.IntegerValue
  || A.NumberValue != B.NumberValue || A.StringValue != B.StringValue || A.BytesValue != B.BytesValue
  || A.ObjectKeys != B.ObjectKeys || A.Elements.Num() != B.Elements.Num()) return false;
 for (int32 I = 0; I < A.Elements.Num(); ++I) if (!EqualValues(A.Elements[I], B.Elements[I])) return false;
 return true;
}
FAssetDocumentContractValidationResult ValidatePreparedInvocation(const FPreparedStructuralEdit& State,
 const FAssetDocumentMutationIntent& Expected, const FAssetDocumentPrimitiveInvocation& Invocation,
 IAssetDocumentHandleResolver& Handles)
{
 FAssetDocumentMutationToken Token;
 Token.RequestId = Invocation.RequestId; Token.TransactionId = Invocation.TransactionId; Token.RuntimeRevision = Invocation.RuntimeRevision;
 if (!IsStructuralContext() || !State.Capture || !State.Capture->IsOpenFor(Token) || State.bApplied
  || Invocation.PrimitiveId != Expected.Requirement.PrimitiveId || Invocation.Version != Expected.Requirement.Version
  || Invocation.Phase != Expected.Phase || !ValidateAssetDocumentRuntimeValue(Invocation.ValueArguments).bValid
  || !EqualValues(Invocation.ValueArguments, Expected.Intent) || !BaselineCurrent(State)
  || Invocation.HandleArguments.Num() != 1 || Invocation.HandleArguments[0].Name != TEXT("asset"))
  return Result(false, TEXT("AnimationProvider.Montage.PreparedStateDrift"));
 auto Acquired = AcquireAnimationInputLease(Handles, Invocation, TEXT("asset"), EAssetDocumentHandleKind::Asset,
  EAssetDocumentHandleAccess::Mutable);
 if (!Acquired.Lease)
 {
  auto Failure = Result(false, TEXT("AnimationProvider.InvalidHandlePayload"));
  if (!Acquired.Diagnostics.IsEmpty()) Failure.Diagnostics = MoveTemp(Acquired.Diagnostics);
  return Failure;
 }
 return WithAssetDocumentEditorAssetIdentity(*Acquired.Lease, [&](const FAssetDocumentEditorAssetIdentityView& View)
 {
  return Result(&View.GetObject() == State.Target.Get() && View.GetCanonicalObjectPath() == State.TargetPath
   && View.GetTypeRevision() == State.TargetRevision, TEXT("AnimationProvider.Montage.PreparedStateDrift"));
 });
}
}


FAssetDocumentRuntimeValueDto FreezeMontageStructuralArguments(const FStructuralIntent& Intent)
{
 using namespace MontageStructuralPreparationLocal;
 // A private canonical invocation projection, not a new persisted Body format.
 auto Value = Object();
 if (Intent.Slots.IsSet())
 {
  auto Slots = Array();
  for (const auto& InputSlot : *Intent.Slots)
  {
   auto Slot = Object(); Field(Slot, TEXT("name"), Text(InputSlot.Name.ToString()));
   auto Segments = Array();
   for (const auto& Input : InputSlot.Segments)
   {
    auto Segment = Object();
    Field(Segment, TEXT("animation"), Text(Input.Animation ? Input.Animation->GetPathName() : FString()));
    Field(Segment, TEXT("start"), Number(Input.Start)); Field(Segment, TEXT("source_start"), Number(Input.SourceStart));
    Field(Segment, TEXT("source_end"), Number(Input.SourceEnd)); Field(Segment, TEXT("rate"), Number(Input.Rate));
    Field(Segment, TEXT("loops"), Number(Input.Loops)); Field(Segment, TEXT("previous_slot"), Text(Input.PreviousSlot.ToString()));
    Field(Segment, TEXT("previous_segment"), Number(Input.PreviousSegment)); Segments.Elements.Add(MoveTemp(Segment));
   }
   Field(Slot, TEXT("segments"), MoveTemp(Segments)); Slots.Elements.Add(MoveTemp(Slot));
  }
  Field(Value, TEXT("slots"), MoveTemp(Slots));
 }
 if (Intent.Sections.IsSet())
 {
  auto Sections = Array();
  for (const auto& Input : *Intent.Sections)
  {
   auto Section = Object(); Field(Section, TEXT("name"), Text(Input.Name.ToString()));
   Field(Section, TEXT("next"), Text(Input.Next.ToString())); Field(Section, TEXT("time"), Number(Input.Time));
   Field(Section, TEXT("slot_name"), Text(Input.Slot.ToString())); Field(Section, TEXT("method"), Number(int32(Input.Method)));
   Sections.Elements.Add(MoveTemp(Section));
  }
  Field(Value, TEXT("sections"), MoveTemp(Sections));
 }
 return Value;
}
bool FStructuralCapture::IsOpenFor(const FAssetDocumentMutationToken& Token) const
{
 return bOpened && !bClosed && TransactionId.IsValid() && Token.RequestId == RequestId
  && Token.TransactionId == TransactionId && Token.RuntimeRevision == Revision;
}
bool FStructuralCapture::IsClosedForRollback() const { return bOpened && bClosed && TransactionId.IsValid(); }

FAssetDocumentContractValidationResult MakeMontageStructuralPreparedBatch(TSharedRef<FPreparedStructuralEdit> State,
 int32 StepIndex, EAssetDocumentRuntimeRequestPhase Phase,
 FAssetDocumentPreparedMutationBatch& OutBatch, FAssetDocumentMutationIntent& OutIntent)
{
 using namespace MontageStructuralPreparationLocal;
 OutBatch = FAssetDocumentPreparedMutationBatch(); OutIntent = FAssetDocumentMutationIntent();
 if (!IsStructuralContext() || State->Capture || State->bApplied || !BaselineCurrent(*State) || StepIndex < 0
  || (Phase != EAssetDocumentRuntimeRequestPhase::Apply && Phase != EAssetDocumentRuntimeRequestPhase::Repair)
  || !ValidateAssetDocumentRuntimeValue(State->FrozenArguments).bValid) return Result(false);
 FAssetDocumentMutationIntent Intent;
 Intent.StepIndex = StepIndex; Intent.Phase = Phase;
 Intent.Requirement = {State->bReplaceSlots ? TEXT("animation.montage.slot_tracks.replace.v1") : TEXT("animation.montage.sections.mutate.v1"), 1, TEXT("animation_profile")};
 Intent.InputSchemaId = Intent.Requirement.PrimitiveId + TEXT(".input.v1"); Intent.Intent = State->FrozenArguments;
 auto Capture = MakeShared<FStructuralCapture>();
 Capture->RequestId = State->RequestId; Capture->Revision = State->RuntimeRevision; Capture->Phase = Phase;
 State->Capture = Capture;
 OutBatch.OpenMutationCapture = [State, Capture](const FGuid& TransactionId, TArray<FAssetDocumentRuntimeDiagnostic>& Diagnostics)
  -> TUniquePtr<FAssetDocumentCoreMutationCaptureLease>
 {
  if (!IsStructuralContext() || !TransactionId.IsValid() || Capture->bOpened || Capture->bClosed || !BaselineCurrent(*State))
  {
   Diagnostics.Append(Result(false, TEXT("AnimationProvider.Montage.PreparedStateDrift")).Diagnostics); return {};
  }
  Capture->TransactionId = TransactionId; Capture->bOpened = true;
  return MakeUnique<FAssetDocumentCoreMutationCaptureLease>([Capture]() { Capture->bClosed = true; });
 };
 FAssetDocumentPreparedMutationContribution Contribution;
 Contribution.StepIndex = StepIndex;
 Contribution.RawFootprint = GetMontageStructuralFootprint(*State);
 // Structural completion touches retained links and the length model; one structural contribution owns this closure.
 Contribution.OwnedSurfaceIds = {State->TargetPath + TEXT("::SlotAnimTracks"), State->TargetPath + TEXT("::CompositeSections"),
  State->TargetPath + TEXT("::Notifies"), State->TargetPath + TEXT("::SequenceLength")};
 Contribution.InitialAction = MakeShared<FStructuralInverse>(State);
 Contribution.ValidateInvocation = [State, Intent](const FAssetDocumentPrimitiveInvocation& Invocation, IAssetDocumentHandleResolver& Handles)
 { return ValidatePreparedInvocation(*State, Intent, Invocation, Handles); };
 OutBatch.Contributions.Add(MoveTemp(Contribution)); OutIntent = MoveTemp(Intent);
 return Result(true);
}
}

FAssetDocumentContractValidationResult PrepareAnimationMontageStructuralMutationBatch(
 const FAssetDocumentRuntimeRequestDto& Request, TConstArrayView<FAssetDocumentMutationIntent> Intents,
 FAssetDocumentPreparedMutationBatch& OutBatch)
{
 using namespace AssetDocumentMontage;
 OutBatch = FAssetDocumentPreparedMutationBatch();
 if (!IsStructuralContext() || Request.Operation != EAssetDocumentRuntimeOperation::Apply
  || !Request.RequestId.IsValid() || Request.ExpectedRuntimeRevision == 0 || Intents.Num() != 1
  || Request.RequestedAssetClassPath != TEXT("/Script/Engine.AnimMontage")) return Result(false);
 const FSoftObjectPath Path(Request.TargetAssetPath);
 if (!Path.IsValid() || !Path.GetSubPathString().IsEmpty() || Path.ToString() != Request.TargetAssetPath
  || !FPackageName::IsValidObjectPath(Request.TargetAssetPath)) return Result(false);
 auto* Target = Cast<UAnimMontage>(Path.ResolveObject());
 if (!IsValid(Target) || Target->GetClass() != UAnimMontage::StaticClass() || Target->GetPathName() != Request.TargetAssetPath)
  return Result(false, TEXT("AnimationProvider.Montage.DependencyUnavailable"));
 const auto& Input = Intents[0];
 if (Input.StepIndex < 0 || Input.Requirement.Version != 1 || Input.Requirement.RequiredByRouteId != TEXT("animation_profile")
  || (Input.Requirement.PrimitiveId != SlotsPrimitive && Input.Requirement.PrimitiveId != SectionsPrimitive)
  || Input.InputSchemaId != Input.Requirement.PrimitiveId + TEXT(".input.v1")
  || (Input.Phase != EAssetDocumentRuntimeRequestPhase::Apply && Input.Phase != EAssetDocumentRuntimeRequestPhase::Repair)) return Result(false);
 FStructuralIntent Parsed;
 auto Valid = ParseMontageStructuralArguments(Input.Intent, Input.Requirement.PrimitiveId, Parsed);
 if (!Valid.bValid) return Valid;
 TSharedPtr<FPreparedStructuralEdit> State;
 Valid = PrepareMontageStructuralEdit(*Target, Parsed, Request.RequestId, Request.ExpectedRuntimeRevision, State);
 if (!Valid.bValid) return Valid;
 // Preserve the exact admitted DTO numbers; native float conversion is deterministic but is not a DTO rewrite.
 State->FrozenArguments = Input.Intent;
 FAssetDocumentMutationIntent Frozen;
 return MakeMontageStructuralPreparedBatch(State.ToSharedRef(), Input.StepIndex, Input.Phase, OutBatch, Frozen);
}
