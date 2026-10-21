// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
#include "AssetDocumentEditorSequenceRetargetSourceNameMutation.h"
#include "AssetDocumentEditorNativeState.h"
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

namespace AssetDocumentEditorSequenceRetargetSourceName
{
namespace
{
constexpr const TCHAR* Primitive = TEXT("animation.sequence.retarget_source_name.set.v1");
constexpr const TCHAR* Schema = TEXT("animation.sequence.retarget_source_name.set.v1.input.v1");
FAssetDocumentContractValidationResult Result(bool Valid, const TCHAR* Code = TEXT("Editor.SequenceRetargetSourceName.Rejected"))
{
 FAssetDocumentContractValidationResult R; R.bValid = Valid;
 if (!Valid) { auto& D = R.Diagnostics.AddDefaulted_GetRef(); D.Path = TEXT("/References/RetargetSource"); D.Code = Code;
  D.Message = TEXT("One fixed native FName, verified Editor identity and owned parent capture are required."); }
 return R;
}
bool ContextValid()
{ return IsInGameThread() && GIsEditor && !IsGarbageCollecting() && !UE::IsSavingPackage(nullptr) && !FUObjectThreadContext::Get().IsRoutingPostLoad; }
bool Loaded(const UObject* O)
{ return IsValid(O) && !O->HasAnyFlags(RF_NeedLoad | RF_NeedPostLoad | RF_NeedPostLoadSubobjects)
  && !O->HasAnyInternalFlags(EInternalObjectFlags::AsyncLoadingPhase1); }
bool Canonical(const UObject* O, UClass* Class, bool bSkeletonReference = false)
{
 if (!Loaded(O) || !Loaded(O->GetOutermost()) || (bSkeletonReference ? !O->IsA(Class) : O->GetClass() != Class) || O->IsTemplate()
  || O->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject | RF_Transient) || O->GetOuter() != O->GetOutermost()) return false;
 const FSoftObjectPath P(O->GetPathName());
 return P.IsValid() && P.GetSubPathString().IsEmpty() && FPackageName::IsValidObjectPath(P.ToString()) && P.ResolveObject() == O;
}
bool ExactName(FName A, FName B) { return A.IsEqual(B, ENameCase::CaseSensitive, true); }
bool Parse(const FAssetDocumentRuntimeValueDto& V, FName& Out)
{
 if (!ValidateAssetDocumentRuntimeValue(V).bValid || V.Kind != EAssetDocumentRuntimeValueKind::Object
  || V.ObjectKeys != TArray<FString>{TEXT("retarget_source_name")} || V.Elements.Num() != 1
  || V.Elements[0].Kind != EAssetDocumentRuntimeValueKind::String) return false;
 const FString& Text = V.Elements[0].StringValue;
 // A conservative safe literal subset, not a claim about all Engine names.
 // Empty input is refused instead of silently using stock empty -> None.
 if (Text.IsEmpty() || Text.Len() >= NAME_SIZE) return false;
 for (TCHAR C : Text) if (C == 0) return false;
 Out = FName(Text.Len(), *Text, FNAME_Add); return true;
}
struct FMutation final : IAssetDocumentEditorSequenceRetargetSourceNameMutation
{
 FAssetDocumentRuntimeRequestDto Request;
 EAssetDocumentRuntimeRequestPhase Phase;
 TStrongObjectPtr<UAnimSequence> Target;
 TStrongObjectPtr<UPackage> Package;
 TStrongObjectPtr<USkeleton> Skeleton;
 TStrongObjectPtr<UClass> SkeletonClass;
 FString Path, PackagePath, SkeletonPath, SkeletonClassPath;
 uint32 Revision = 0, SkeletonRevision = 0, SkeletonClassRevision = 0;
 EClassFlags SkeletonClassFlags = CLASS_None;
 FGuid SkeletonGuid, Transaction;
 FNameProperty* Property = nullptr;
 FName Original, Desired;
 bool bOpened = false, bClosed = false, bConsumed = false, bWritten = false, bRestored = false;
 bool IdentityCurrent() const
 {
  return Canonical(Target.Get(), UAnimSequence::StaticClass()) && Target->GetPathName() == Path && Target->GetUniqueID() == Revision
   && Target->GetOutermost() == Package.Get() && Package->GetName() == PackagePath
   && Canonical(Skeleton.Get(), USkeleton::StaticClass(), true) && Skeleton->GetPathName() == SkeletonPath
   && Loaded(SkeletonClass.Get()) && Loaded(SkeletonClass->GetOutermost()) && Skeleton->GetClass() == SkeletonClass.Get()
   && SkeletonClass->GetUniqueID() == SkeletonClassRevision && SkeletonClass->GetPathName() == SkeletonClassPath
   && SkeletonClass->GetClassFlags() == SkeletonClassFlags
   && Skeleton->GetUniqueID() == SkeletonRevision && Skeleton->GetGuid() == SkeletonGuid
   && Target->GetSkeleton() == Skeleton.Get() && Target->GetSkeletonGuid() == SkeletonGuid;
 }
 bool IsOriginalCurrent() const override { return IdentityCurrent() && ExactName(Target->RetargetSource, Original); }
 bool IsDesiredCurrent() const override { return IdentityCurrent() && ExactName(Target->RetargetSource, HasChanges() ? Desired : Original); }
 bool HasChanges() const override { return Original != Desired; }
 bool DidWrite() const override { return bWritten; }
 FName GetOriginalName() const override { return Original; }
 FName GetDesiredName() const override { return HasChanges() ? Desired : Original; }
 FAssetDocumentContractValidationResult OpenCapture(const FGuid& Id) override
 {
  if (!ContextValid() || !Id.IsValid() || bOpened || bClosed || !IsOriginalCurrent()) return Result(false);
  Transaction = Id; bOpened = true; return Result(true);
 }
 void CloseCapture() override { bClosed = true; }
 FAssetDocumentHandleValidationContext HandleContext(const FAssetDocumentPrimitiveInvocation& I) const
 {
  FAssetDocumentHandleValidationContext C; C.RequestId = I.RequestId; C.TransactionId = I.TransactionId;
  C.RuntimeRevision = I.RuntimeRevision; C.ProviderId = TEXT("editor"); C.Phase = I.Phase;
  C.RequiredKind = EAssetDocumentHandleKind::Asset; C.RequiredAccess = EAssetDocumentHandleAccess::Mutable; return C;
 }
 bool MatchesView(const FAssetDocumentEditorAssetIdentityView& V) const
 { return IdentityCurrent() && &V.GetObject() == Target.Get() && V.GetTypeRevision() == Revision && V.GetCanonicalObjectPath() == Path; }
 FAssetDocumentContractValidationResult ValidateInvocation(const FAssetDocumentPrimitiveInvocation& I, IAssetDocumentHandleResolver& Handles) const override
 {
  FName Name;
  if (!ContextValid() || !bOpened || bClosed || bConsumed || !IsOriginalCurrent() || !Target->CanModify()
   || I.PrimitiveId != Primitive || I.Version != 1 || I.Phase != Phase || I.RequestId != Request.RequestId
   || I.RuntimeRevision != Request.ExpectedRuntimeRevision || I.TransactionId != Transaction
   || !Parse(I.ValueArguments, Name) || Name != Desired || I.HandleArguments.Num() != 1 || I.HandleArguments[0].Name != TEXT("sequence")) return Result(false);
  auto Lease = Handles.AcquireVerifiedLease(I.HandleArguments[0].Handle, HandleContext(I));
  if (!Lease.Lease) return Result(false);
  return WithAssetDocumentEditorAssetIdentity(*Lease.Lease, [&](const auto& V) { return Result(MatchesView(V)); });
 }
 bool Write(FName Value)
 {
  // Only this exported high-level helper writes the live fixed field. The
  // Never mode disables notifications; no extra Pre/PostEdit lifecycle is used.
  return PropertyAccessUtil::SetPropertyValue_Object(Property, Target.Get(), Property, &Value, INDEX_NONE,
   PropertyAccessUtil::EditorReadOnlyFlags, EPropertyAccessChangeNotifyMode::Never) == EPropertyAccessResultFlags::Success
   && IdentityCurrent() && ExactName(Target->RetargetSource, Value);
 }
 FAssetDocumentContractValidationResult Apply(const FAssetDocumentPrimitiveInvocation& I, IAssetDocumentHandleResolver& Handles,
  IAssetDocumentAtomicTransaction& T) override
 {
  const auto Valid = ValidateInvocation(I, Handles);
  if (!Valid.bValid || T.GetState() != EAssetDocumentAtomicState::Mutating || T.GetTransactionId() != Transaction
   || T.GetMutationToken().RequestId != I.RequestId || T.GetMutationToken().RuntimeRevision != I.RuntimeRevision
   || T.GetMutationToken().TransactionId != I.TransactionId) return Result(false);
  auto Lease = Handles.AcquireVerifiedLease(I.HandleArguments[0].Handle, HandleContext(I));
  if (!Lease.Lease) return Result(false);
  return WithAssetDocumentEditorAssetIdentity(*Lease.Lease, [&](const auto& V)
  {
   if (!ContextValid() || bConsumed || bClosed || !MatchesView(V) || !IsOriginalCurrent()) return Result(false);
   if (!HasChanges()) { bConsumed = true; return Result(true); }
   const TArray<FAssetDocumentMutationFootprint> Footprint = {
    {EAssetDocumentMutationFootprintKind::UObjectState, TEXT("Object:") + Path, TEXT("rollback.uobject")},
    {EAssetDocumentMutationFootprintKind::Package, TEXT("Package:") + PackagePath, TEXT("rollback.package")}};
   const auto Enlisted = T.EnlistFootprint(Footprint); if (!Enlisted.bValid) return Enlisted;
   // Enlistment may invoke a parent participant; revalidate before publishing.
   if (!ContextValid() || bConsumed || bClosed || !MatchesView(V) || !IsOriginalCurrent()) return Result(false);
   bWritten = bConsumed = true; return Result(Write(Desired));
  });
 }
 FAssetDocumentContractValidationResult Restore() override
 {
  if (!ContextValid() || !bOpened || !bClosed || bRestored) return Result(false);
  bRestored = true;
  return !bWritten ? Result(true) : Result(IdentityCurrent() && Target->CanModify() && Write(Original));
 }
};
}
}

FAssetDocumentContractValidationResult PrepareAssetDocumentEditorSequenceRetargetSourceNameMutation(
 const FAssetDocumentRuntimeRequestDto& Request, const FAssetDocumentMutationIntent& Intent,
 TSharedPtr<IAssetDocumentEditorSequenceRetargetSourceNameMutation>& OutMutation)
{
 using namespace AssetDocumentEditorSequenceRetargetSourceName;
 OutMutation.Reset(); FName Desired;
 if (!ContextValid() || !Request.RequestId.IsValid() || !Request.ExpectedRuntimeRevision
  || Request.Operation != EAssetDocumentRuntimeOperation::Apply || Request.RequestedAssetClassPath != TEXT("/Script/Engine.AnimSequence")
  || Intent.Requirement.PrimitiveId != Primitive || Intent.Requirement.Version != 1 || Intent.StepIndex < 0
  || Intent.Requirement.RequiredByRouteId != TEXT("animation_profile") || Intent.InputSchemaId != Schema
  || (Intent.Phase != EAssetDocumentRuntimeRequestPhase::Apply && Intent.Phase != EAssetDocumentRuntimeRequestPhase::Repair)
  || !Parse(Intent.Intent, Desired)) return Result(false, TEXT("Editor.SequenceRetargetSourceName.IntentShape"));
 auto* Target = Cast<UAnimSequence>(FSoftObjectPath(Request.TargetAssetPath).ResolveObject());
 if (!Canonical(Target, UAnimSequence::StaticClass()) || Target->GetPathName() != Request.TargetAssetPath || !Target->CanModify()
  || !Canonical(Target->GetSkeleton(), USkeleton::StaticClass(), true) || Target->GetSkeletonGuid() != Target->GetSkeleton()->GetGuid()) return Result(false);
 auto* Property = FindFProperty<FNameProperty>(UAnimSequence::StaticClass(), GET_MEMBER_NAME_CHECKED(UAnimSequence, RetargetSource));
 if (!Property || Property->GetOwnerClass() != UAnimSequence::StaticClass() || Property->ArrayDim != 1
  || PropertyAccessUtil::CanSetPropertyValue(Property, PropertyAccessUtil::EditorReadOnlyFlags, true) != EPropertyAccessResultFlags::Success) return Result(false);
 auto S = MakeShared<FMutation>(); S->Request = Request; S->Phase = Intent.Phase; S->Target.Reset(Target);
 S->Package.Reset(Target->GetOutermost()); S->Skeleton.Reset(Target->GetSkeleton()); S->Path = Target->GetPathName();
 S->PackagePath = S->Package->GetName(); S->Revision = Target->GetUniqueID(); S->SkeletonPath = S->Skeleton->GetPathName();
 S->SkeletonRevision = S->Skeleton->GetUniqueID(); S->SkeletonGuid = S->Skeleton->GetGuid(); S->Property = Property;
 S->SkeletonClass.Reset(S->Skeleton->GetClass()); S->SkeletonClassPath = S->SkeletonClass->GetPathName();
 S->SkeletonClassRevision = S->SkeletonClass->GetUniqueID(); S->SkeletonClassFlags = S->SkeletonClass->GetClassFlags();
 S->Original = Target->RetargetSource; S->Desired = Desired;
 if (!S->IsOriginalCurrent()) return Result(false);
 OutMutation = S; return Result(true);
}
