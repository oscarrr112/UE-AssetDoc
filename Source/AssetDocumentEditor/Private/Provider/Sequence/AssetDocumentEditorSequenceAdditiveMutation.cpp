// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
#include "AssetDocumentEditorSequenceAdditiveMutation.h"
#include "AssetDocumentEditorNativeState.h"
#include "Provider/AssetDocumentEditorTypedValueCodec.h"
#include "Provider/Sequence/AssetDocumentEditorSequenceAdditiveMutationTestHooks.h"
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "Misc/PackageName.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/PropertyAccessUtil.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectThreadContext.h"
#include "UObject/UnrealType.h"

namespace AssetDocumentEditorSequenceAdditive
{
namespace
{
#if WITH_DEV_AUTOMATION_TESTS
AssetDocumentEditorSequenceAdditiveTesting::FHooks* TestHooks = nullptr;
#endif
constexpr const TCHAR* Primitive = TEXT("animation.sequence.complete_edit.v2");
FAssetDocumentContractValidationResult Result(bool Valid, const TCHAR* Code=TEXT("Editor.SequenceAdditive.Rejected"))
{
 FAssetDocumentContractValidationResult R; R.bValid = Valid;
 if (!Valid) { auto& D = R.Diagnostics.AddDefaulted_GetRef(); D.Path = TEXT("/Additive"); D.Code = Code; D.Message = TEXT("Exact frozen four-field atomic values, verified identities and parent capture are required."); }
 return R;
}
bool ContextValid()
{ return IsInGameThread() && GIsEditor && !IsGarbageCollecting() && !UE::IsSavingPackage(nullptr) && !FUObjectThreadContext::Get().IsRoutingPostLoad; }
bool Canonical(const UAnimSequence* S)
{
 if (!IsValid(S) || S->IsTemplate() || S->GetClass() != UAnimSequence::StaticClass()
  || S->GetOuter() != S->GetOutermost() || S->HasAnyFlags(RF_NeedLoad | RF_NeedPostLoad)) return false;
 const FSoftObjectPath P(S->GetPathName());
 return P.IsValid() && P.GetSubPathString().IsEmpty() && FPackageName::IsValidObjectPath(P.ToString()) && P.ResolveObject() == S;
}
bool CanonicalSkeleton(const USkeleton* S)
{
 if (!IsValid(S) || S->IsTemplate() || S->GetClass()!=USkeleton::StaticClass()
  || S->GetOuter()!=S->GetOutermost() || S->HasAnyFlags(RF_NeedLoad|RF_NeedPostLoad)) return false;
 const FSoftObjectPath P(S->GetPathName());
 return P.IsValid() && P.GetSubPathString().IsEmpty() && FPackageName::IsValidObjectPath(P.ToString()) && P.ResolveObject()==S;
}
struct FMutation final : IAssetDocumentEditorSequenceAdditiveMutation
{
 FAssetDocumentRuntimeRequestDto Request;
 EAssetDocumentRuntimeRequestPhase Phase;
 TStrongObjectPtr<UAnimSequence> Target, OriginalReference, DesiredReference;
 TStrongObjectPtr<UPackage> Package;
 TStrongObjectPtr<USkeleton> Skeleton;
 FString TargetPath, PackagePath, OriginalReferencePath, DesiredReferencePath, SkeletonPath;
 uint32 TargetRevision = 0, OriginalReferenceRevision = 0, DesiredReferenceRevision = 0;
 uint32 SkeletonRevision=0;
 FGuid SkeletonGuid;
 FGuid Transaction;
 FByteProperty* Type = nullptr; FByteProperty* Base = nullptr; FIntProperty* Frame = nullptr; FObjectPropertyBase* Reference = nullptr;
 TArray<FProperty*> Properties;
 TArray<TUniquePtr<FAssetDocumentEditorScopedPropertyValue>> Old, Desired;
 bool bSuppliedReference = false, bOpened = false, bClosed = false, bWritten = false, bConsumed = false, bRestored = false;
 bool IdentityCurrent() const
 {
  auto SameReference = [&](UAnimSequence* S, const FString& Path, uint32 Revision)
  { return !S ? Path.IsEmpty() : Canonical(S) && S->GetPathName() == Path && S->GetUniqueID() == Revision
    && S->GetSkeleton()==Skeleton.Get() && S->GetSkeletonGuid()==SkeletonGuid; };
  return Canonical(Target.Get()) && Target->GetPathName() == TargetPath && Target->GetUniqueID() == TargetRevision
   && Target->GetOutermost() == Package.Get() && Package->GetName() == PackagePath
   && CanonicalSkeleton(Skeleton.Get()) && Skeleton->GetPathName()==SkeletonPath && Skeleton->GetUniqueID()==SkeletonRevision
   && Skeleton->GetGuid()==SkeletonGuid && Target->GetSkeleton()==Skeleton.Get() && Target->GetSkeletonGuid()==SkeletonGuid
   && SameReference(OriginalReference.Get(), OriginalReferencePath, OriginalReferenceRevision)
   && SameReference(DesiredReference.Get(), DesiredReferencePath, DesiredReferenceRevision);
 }
 bool Matches(const TArray<TUniquePtr<FAssetDocumentEditorScopedPropertyValue>>& Values) const
 {
  return IdentityCurrent() && Type->GetPropertyValue(Values[0]->GetMemory()) == Target->AdditiveAnimType.GetValue()
   && Base->GetPropertyValue(Values[1]->GetMemory()) == Target->RefPoseType.GetValue()
   && Frame->GetPropertyValue(Values[2]->GetMemory()) == Target->RefFrameIndex
   && Reference->GetObjectPropertyValue(Values[3]->GetMemory()) == Target->RefPoseSeq.Get();
 }
 bool IsOriginalCurrent() const override { return Matches(Old); }
 bool IsDesiredCurrent() const override { return Matches(Desired); }
 bool HasChanges() const override
 {
  for (int32 I = 0; I < 4; ++I) if (!Properties[I]->Identical(Old[I]->GetMemory(), Desired[I]->GetMemory())) return true;
  return false;
 }
 bool DidWrite() const override { return bWritten; }
 FAssetDocumentContractValidationResult OpenCapture(const FGuid& Id) override
 {
  if (!ContextValid() || !Id.IsValid() || bOpened || bClosed || !IsOriginalCurrent()) return Result(false);
  Transaction = Id; bOpened = true; return Result(true);
 }
 void CloseCapture() override { bClosed = true; }
 FAssetDocumentHandleValidationContext HandleContext(const FAssetDocumentPrimitiveInvocation& I,EAssetDocumentHandleAccess Access) const
 {
  FAssetDocumentHandleValidationContext C; C.RequestId=I.RequestId; C.TransactionId=I.TransactionId;
  // These capabilities were issued by the real Editor asset resolver. Their
  // immutable owner metadata is Editor even when Animation consumes the lease.
  C.RuntimeRevision=I.RuntimeRevision; C.ProviderId=TEXT("editor"); C.Phase=I.Phase;
  C.RequiredKind=EAssetDocumentHandleKind::Asset; C.RequiredAccess=Access; return C;
 }
 bool MatchesView(const FAssetDocumentEditorAssetIdentityView& V,UAnimSequence* Expected) const
 {
  return IdentityCurrent() && &V.GetObject()==Expected && V.GetTypeRevision()==Expected->GetUniqueID()
   && V.GetCanonicalObjectPath()==Expected->GetPathName() && Canonical(Expected)
   && Expected->GetSkeleton()==Skeleton.Get() && Expected->GetSkeletonGuid()==SkeletonGuid;
 }
 FAssetDocumentContractValidationResult ValidateInvocation(const FAssetDocumentPrimitiveInvocation& I, IAssetDocumentHandleResolver& Handles) const override
 {
  if (!ContextValid() || !bOpened || bClosed || bConsumed || !IsOriginalCurrent() || !Target->CanModify()
   || I.PrimitiveId != Primitive || I.Version != 2 || I.Phase != Phase || I.RequestId != Request.RequestId
   || I.RuntimeRevision != Request.ExpectedRuntimeRevision || I.TransactionId != Transaction
   || I.HandleArguments.Num() != (bSuppliedReference && DesiredReference.IsValid() ? 2 : 1)) return Result(false);
  if (!ValidateAssetDocumentRuntimeValue(I.ValueArguments).bValid || I.ValueArguments.Kind != EAssetDocumentRuntimeValueKind::Object
   || I.ValueArguments.ObjectKeys != TArray<FString>{TEXT("completion_kind")} || I.ValueArguments.Elements.Num() != 1
   || I.ValueArguments.Elements[0].Kind != EAssetDocumentRuntimeValueKind::String
   || I.ValueArguments.Elements[0].StringValue != TEXT("additive_root")) return Result(false);
  const auto* TargetArg = I.HandleArguments.FindByPredicate([](const auto& A){ return A.Name == TEXT("sequence"); });
  const auto* RefArg = I.HandleArguments.FindByPredicate([](const auto& A){ return A.Name == TEXT("ref_pose_seq"); });
  if (!TargetArg || ((bSuppliedReference && DesiredReference.IsValid()) != (RefArg != nullptr))) return Result(false);
  auto Acquire = [&](const FAssetDocumentOpaqueHandle& H, EAssetDocumentHandleAccess Access, UAnimSequence* Expected)
  {
   auto L = Handles.AcquireVerifiedLease(H, HandleContext(I,Access)); if (!L.Lease) return Result(false);
   return WithAssetDocumentEditorAssetIdentity(*L.Lease, [&](const FAssetDocumentEditorAssetIdentityView& V)
   { return Result(MatchesView(V,Expected)); });
  };
  const auto T = Acquire(TargetArg->Handle, EAssetDocumentHandleAccess::Mutable, Target.Get());
  if (!T.bValid) return T;
  return RefArg ? Acquire(RefArg->Handle, EAssetDocumentHandleAccess::ReadOnly, DesiredReference.Get()) : Result(true);
 }
 bool Write(const TArray<TUniquePtr<FAssetDocumentEditorScopedPropertyValue>>& Values, bool bInverse)
 {
  // One parent owns all four values before this first public lifecycle/write.
  Target->PreEditChange(Type);
  for (int32 I = 0; I < 4; ++I)
  {
   if (PropertyAccessUtil::SetPropertyValue_Object(Properties[I], Target.Get(), Properties[I], Values[I]->GetMemory(),
    INDEX_NONE, PropertyAccessUtil::EditorReadOnlyFlags, EPropertyAccessChangeNotifyMode::Never) != EPropertyAccessResultFlags::Success) return false;
#if WITH_DEV_AUTOMATION_TESTS
   if (!bInverse && TestHooks) { ++TestHooks->FieldWrites; if (TestHooks->FailAfterField == I) return false; }
#endif
  }
  (void)bInverse;
  return Matches(Values);
 }
 FAssetDocumentContractValidationResult Apply(const FAssetDocumentPrimitiveInvocation& I, IAssetDocumentHandleResolver& Handles, IAssetDocumentAtomicTransaction& T) override
 {
  const auto V = ValidateInvocation(I, Handles);
  if (!V.bValid || T.GetState() != EAssetDocumentAtomicState::Mutating || T.GetTransactionId() != Transaction
   || T.GetMutationToken().RequestId != I.RequestId || T.GetMutationToken().RuntimeRevision != I.RuntimeRevision
   || T.GetMutationToken().TransactionId != I.TransactionId) return Result(false);
  const auto* TargetArg=I.HandleArguments.FindByPredicate([](const auto& A){return A.Name==TEXT("sequence");});
  const auto* RefArg=I.HandleArguments.FindByPredicate([](const auto& A){return A.Name==TEXT("ref_pose_seq");});
  auto WriteInTargetLease=[&]()
  {
   auto Lease=Handles.AcquireVerifiedLease(TargetArg->Handle,HandleContext(I,EAssetDocumentHandleAccess::Mutable));
   if (!Lease.Lease) return Result(false);
   return WithAssetDocumentEditorAssetIdentity(*Lease.Lease,[&](const FAssetDocumentEditorAssetIdentityView& View)
   {
    if (!ContextValid() || !bOpened || bClosed || bConsumed || !IsOriginalCurrent() || !MatchesView(View,Target.Get())) return Result(false);
    if (!HasChanges()) { bConsumed=true; return Result(true); }
    const TArray<FAssetDocumentMutationFootprint> Footprint={
     {EAssetDocumentMutationFootprintKind::UObjectState,TEXT("Object:")+TargetPath,TEXT("rollback.uobject")},
     {EAssetDocumentMutationFootprintKind::Package,TEXT("Package:")+PackagePath,TEXT("rollback.package")}};
    auto Enlisted=T.EnlistFootprint(Footprint); if (!Enlisted.bValid) return Enlisted;
    bWritten=bConsumed=true;
    return Result(Write(Desired,false));
   });
  };
  // Every forward setter executes while the actual mutable identity callback
  // is live. An explicitly supplied reference retains its verified readonly
  // callback/lease across that group; omission never synthesizes one.
  if (!RefArg) return WriteInTargetLease();
  auto ReferenceLease=Handles.AcquireVerifiedLease(RefArg->Handle,HandleContext(I,EAssetDocumentHandleAccess::ReadOnly));
  if (!ReferenceLease.Lease) return Result(false);
  return WithAssetDocumentEditorAssetIdentity(*ReferenceLease.Lease,[&](const FAssetDocumentEditorAssetIdentityView& View)
  { return MatchesView(View,DesiredReference.Get()) ? WriteInTargetLease() : Result(false); });
 }
 FAssetDocumentContractValidationResult Restore() override
 {
  if (!ContextValid() || !bOpened || !bClosed || bRestored) return Result(false);
  bRestored = true;
  return !bWritten ? Result(true) : Result(IdentityCurrent() && Target->CanModify() && Write(Old, true));
 }
};
}
}

#if WITH_DEV_AUTOMATION_TESTS
void AssetDocumentEditorSequenceAdditiveTesting::SetHooks(FHooks* Hooks)
{ AssetDocumentEditorSequenceAdditive::TestHooks = Hooks; }
#endif

FAssetDocumentContractValidationResult PrepareAssetDocumentEditorSequenceAdditiveMutation(
 const FAssetDocumentRuntimeRequestDto& Request, const FAssetDocumentMutationIntent& Intent,
 TSharedPtr<IAssetDocumentEditorSequenceAdditiveMutation>& OutMutation)
{
 using namespace AssetDocumentEditorSequenceAdditive;
 OutMutation.Reset();
 if (!ContextValid() || !Request.RequestId.IsValid() || !Request.ExpectedRuntimeRevision
  || Request.Operation != EAssetDocumentRuntimeOperation::Apply || Request.RequestedAssetClassPath != TEXT("/Script/Engine.AnimSequence")
  || Intent.Requirement.PrimitiveId != Primitive || Intent.Requirement.Version != 2 || Intent.StepIndex < 0
  || Intent.Requirement.RequiredByRouteId != TEXT("animation_profile") || Intent.InputSchemaId != TEXT("animation.sequence.complete_edit.input.v2")
  || (Intent.Phase != EAssetDocumentRuntimeRequestPhase::Apply && Intent.Phase != EAssetDocumentRuntimeRequestPhase::Repair)
  || !ValidateAssetDocumentRuntimeValue(Intent.Intent).bValid || Intent.Intent.Kind != EAssetDocumentRuntimeValueKind::Object
  || Intent.Intent.ObjectKeys.Num() != 2) return Result(false,TEXT("Editor.SequenceAdditive.IntentShape"));
 const int32 K = Intent.Intent.ObjectKeys.IndexOfByKey(TEXT("completion_kind")), A = Intent.Intent.ObjectKeys.IndexOfByKey(TEXT("additive"));
 if (!Intent.Intent.Elements.IsValidIndex(K) || !Intent.Intent.Elements.IsValidIndex(A)
  || Intent.Intent.Elements[K].Kind != EAssetDocumentRuntimeValueKind::String || Intent.Intent.Elements[K].StringValue != TEXT("additive_root")
  || Intent.Intent.Elements[A].Kind != EAssetDocumentRuntimeValueKind::Object) return Result(false,TEXT("Editor.SequenceAdditive.CompletionShape"));
 auto* Target = Cast<UAnimSequence>(FSoftObjectPath(Request.TargetAssetPath).ResolveObject());
 if (!Canonical(Target) || Target->GetPathName() != Request.TargetAssetPath || !Target->CanModify()
  || !CanonicalSkeleton(Target->GetSkeleton()) || Target->GetSkeletonGuid()!=Target->GetSkeleton()->GetGuid()) return Result(false,TEXT("Editor.SequenceAdditive.TargetIdentity"));
 auto S = MakeShared<FMutation>(); S->Request = Request; S->Phase = Intent.Phase;
 S->Target.Reset(Target); S->Package.Reset(Target->GetOutermost()); S->TargetPath = Target->GetPathName();
 S->PackagePath = S->Package->GetName(); S->TargetRevision = Target->GetUniqueID();
 S->Skeleton.Reset(Target->GetSkeleton()); S->SkeletonPath=S->Skeleton->GetPathName();
 S->SkeletonGuid=S->Skeleton->GetGuid(); S->SkeletonRevision=S->Skeleton->GetUniqueID();
 S->Type = FindFProperty<FByteProperty>(UAnimSequence::StaticClass(), GET_MEMBER_NAME_CHECKED(UAnimSequence, AdditiveAnimType));
 S->Base = FindFProperty<FByteProperty>(UAnimSequence::StaticClass(), GET_MEMBER_NAME_CHECKED(UAnimSequence, RefPoseType));
 S->Frame = FindFProperty<FIntProperty>(UAnimSequence::StaticClass(), GET_MEMBER_NAME_CHECKED(UAnimSequence, RefFrameIndex));
 S->Reference = FindFProperty<FObjectPropertyBase>(UAnimSequence::StaticClass(), GET_MEMBER_NAME_CHECKED(UAnimSequence, RefPoseSeq));
 if (!S->Type || !S->Base || !S->Frame || !S->Reference || S->Type->Enum != StaticEnum<EAdditiveAnimationType>()
  || S->Base->Enum != StaticEnum<EAdditiveBasePoseType>() || S->Reference->PropertyClass != UAnimSequence::StaticClass()) return Result(false,TEXT("Editor.SequenceAdditive.PropertyIdentity"));
 S->Properties = {S->Type, S->Base, S->Frame, S->Reference};
 for (auto* P : S->Properties)
  if (P->GetOwnerClass() != UAnimSequence::StaticClass() || P->ArrayDim != 1
   || PropertyAccessUtil::CanSetPropertyValue(P, PropertyAccessUtil::EditorReadOnlyFlags, false) != EPropertyAccessResultFlags::Success) return Result(false,TEXT("Editor.SequenceAdditive.PropertyAccess"));
 for (auto* P : S->Properties) S->Old.Add(MakeUnique<FAssetDocumentEditorScopedPropertyValue>(*P));
 S->Type->SetPropertyValue(S->Old[0]->GetMemory(), Target->AdditiveAnimType.GetValue());
 S->Base->SetPropertyValue(S->Old[1]->GetMemory(), Target->RefPoseType.GetValue());
 S->Frame->SetPropertyValue(S->Old[2]->GetMemory(), Target->RefFrameIndex);
 S->Reference->SetObjectPropertyValue(S->Old[3]->GetMemory(), Target->RefPoseSeq.Get());
 S->OriginalReference.Reset(Target->RefPoseSeq.Get()); S->DesiredReference.Reset(Target->RefPoseSeq.Get());
 if (S->OriginalReference.IsValid()) { S->OriginalReferencePath = S->OriginalReference->GetPathName(); S->OriginalReferenceRevision = S->OriginalReference->GetUniqueID(); }
 for (int32 I = 0; I < 4; ++I)
 { auto V = MakeUnique<FAssetDocumentEditorScopedPropertyValue>(*S->Properties[I]); S->Properties[I]->CopyCompleteValue(V->GetMemory(), S->Old[I]->GetMemory()); S->Desired.Add(MoveTemp(V)); }
 const auto& Fields = Intent.Intent.Elements[A];
 const TArray<FString> Names = {TEXT("AdditiveAnimType"), TEXT("RefPoseType"), TEXT("RefFrameIndex"), TEXT("RefPoseSeq")};
 for (int32 I = 0; I < Fields.ObjectKeys.Num(); ++I)
 {
  const int32 Field = Names.IndexOfByKey(Fields.ObjectKeys[I]); if (Field == INDEX_NONE) return Result(false,TEXT("Editor.SequenceAdditive.UnknownField"));
  const auto& Value = Fields.Elements[I];
  if (Field < 3)
  {
   TUniquePtr<FAssetDocumentEditorScopedPropertyValue> Decoded;
   const auto Decoding=DecodeAssetDocumentEditorTypedValue(*S->Properties[Field], Value, Decoded);
   if (!Decoding.bSucceeded) return Result(false,Field==0 ? TEXT("Editor.SequenceAdditive.TypeDecode")
    : Field==1 ? TEXT("Editor.SequenceAdditive.BaseDecode") : TEXT("Editor.SequenceAdditive.FrameDecode"));
   S->Desired[Field] = MoveTemp(Decoded);
  }
  else
  {
   S->bSuppliedReference = true; UAnimSequence* Desired = nullptr;
   if (Value.Kind != EAssetDocumentRuntimeValueKind::Null)
   {
    if (Value.Kind != EAssetDocumentRuntimeValueKind::String) return Result(false,TEXT("Editor.SequenceAdditive.ReferenceKind"));
    const FSoftObjectPath P(Value.StringValue);
    Desired = Cast<UAnimSequence>(P.ResolveObject());
    if (!Canonical(Desired) || P.GetSubPathString().Len() || Value.StringValue != Desired->GetPathName()) return Result(false,TEXT("Editor.SequenceAdditive.ReferenceIdentity"));
   }
   S->DesiredReference.Reset(Desired); S->Reference->SetObjectPropertyValue(S->Desired[3]->GetMemory(), Desired);
  }
 }
 if (S->DesiredReference.IsValid()) { S->DesiredReferencePath = S->DesiredReference->GetPathName(); S->DesiredReferenceRevision = S->DesiredReference->GetUniqueID(); }
 if (!S->IsOriginalCurrent()) return Result(false,TEXT("Editor.SequenceAdditive.FrozenIdentity"));
 OutMutation = S; return Result(true);
}
