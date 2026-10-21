// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
#include "AssetDocumentEditorSequenceCompressionMutation.h"
#include "AssetDocumentEditorSequenceCompressionMutationTestHooks.h"
#include "AssetDocumentEditorNativeState.h"
#include "Provider/AssetDocumentEditorTypedValueCodec.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimBoneCompressionSettings.h"
#include "Animation/AnimCurveCompressionSettings.h"
#include "Animation/VariableFrameStrippingSettings.h"
#include "Animation/Skeleton.h"
#include "Misc/PackageName.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/PropertyAccessUtil.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectThreadContext.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AssetDocumentEditorSequenceCompressionTesting
{
static FHooks* ActiveHooks = nullptr;
void SetHooks(FHooks* Hooks) { ActiveHooks = Hooks; }
}
#endif

namespace AssetDocumentEditorSequenceCompression
{
namespace
{
constexpr const TCHAR* Primitive = TEXT("animation.sequence.compression.settings.set.v1");
constexpr const TCHAR* Schema = TEXT("animation.sequence.compression.settings.set.v1.input.v1");
constexpr int32 FieldCount = 5;
constexpr const TCHAR* Fields[FieldCount] = {
 TEXT("CompressionErrorThresholdScale"), TEXT("BoneCompressionSettings"), TEXT("CurveCompressionSettings"),
 TEXT("VariableFrameStrippingSettings"), TEXT("bDoNotOverrideCompression")};
constexpr const TCHAR* Roles[3] = {
 TEXT("bone_compression_settings"), TEXT("curve_compression_settings"), TEXT("frame_stripping_settings")};
bool Exact(const FString& A, const FString& B) { return A.Equals(B, ESearchCase::CaseSensitive); }
bool ExactFloat(float A, float B) { return FMemory::Memcmp(&A, &B, sizeof(float)) == 0; }
FAssetDocumentContractValidationResult Result(bool Valid, const TCHAR* Code = TEXT("Editor.SequenceCompression.Rejected"))
{
 FAssetDocumentContractValidationResult R; R.bValid = Valid;
 if (!Valid) { auto& D = R.Diagnostics.AddDefaulted_GetRef(); D.Path = TEXT("/Settings/Compression"); D.Code = Code;
  D.Message = TEXT("The fixed compression tuple, frozen literal projection and verified Editor identities are required."); }
 return R;
}
bool ContextValid()
{ return IsInGameThread() && GIsEditor && !IsGarbageCollecting() && !UE::IsSavingPackage(nullptr) && !FUObjectThreadContext::Get().IsRoutingPostLoad; }
bool Loaded(const UObject* O)
{ return IsValid(O) && !O->HasAnyFlags(RF_NeedLoad | RF_NeedPostLoad | RF_NeedPostLoadSubobjects)
  && !O->HasAnyInternalFlags(EInternalObjectFlags::AsyncLoadingPhase1); }
bool CanonicalPath(const FString& Text)
{
 if (Text.IsEmpty()) return false;
 for (TCHAR C : Text) if (C == 0) return false;
 const FSoftObjectPath P(Text);
 return P.IsValid() && P.GetSubPathString().IsEmpty() && FPackageName::IsValidObjectPath(Text) && Exact(P.ToString(), Text);
}
bool Canonical(const UObject* O, UClass* Class)
{
 return Loaded(O) && Loaded(O->GetOutermost()) && O->GetClass() == Class && !O->IsTemplate()
  && !O->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject | RF_Transient)
  && O->GetOuter() == O->GetOutermost() && CanonicalPath(O->GetPathName()) && FSoftObjectPath(O->GetPathName()).ResolveObject() == O;
}
UClass* SettingsClass(int32 I)
{
 if (I == 0) return UAnimBoneCompressionSettings::StaticClass();
 if (I == 1) return UAnimCurveCompressionSettings::StaticClass();
 return UVariableFrameStrippingSettings::StaticClass();
}
bool SettingsValid(const UObject* O, int32 I)
{
 if (!Canonical(O, SettingsClass(I))) return false;
 TArray<UObject*> Owned;
 FReferenceFinder(Owned, const_cast<UObject*>(O), false, true, true, true).FindReferences(const_cast<UObject*>(O));
 for (const UObject* C : Owned) if (!Loaded(C)) return false;
 if (I == 0) return CastChecked<UAnimBoneCompressionSettings>(O)->AreSettingsValid();
 if (I == 1) return CastChecked<UAnimCurveCompressionSettings>(O)->AreSettingsValid();
 // Stock variable settings have no AreSettingsValid API. Their loaded typed
 // identity is checked here; native completion decides whether they are usable.
 return true;
}
struct FProjection
{
 bool Present[FieldCount] = {};
 double ScaleLiteral = 0.; float Scale = 0.f;
 FString Paths[3]; bool Keep = false;
 bool Matches(const FProjection& Other) const
 {
  for (int32 I = 0; I < FieldCount; ++I) if (Present[I] != Other.Present[I]) return false;
  if (Present[0] && FMemory::Memcmp(&ScaleLiteral, &Other.ScaleLiteral, sizeof(double)) != 0) return false;
  for (int32 I = 0; I < 3; ++I) if (Present[I + 1] && !Exact(Paths[I], Other.Paths[I])) return false;
  return !Present[4] || Keep == Other.Keep;
 }
};
bool Parse(const FAssetDocumentRuntimeValueDto& V, FProjection& Out)
{
 if (!ValidateAssetDocumentRuntimeValue(V).bValid || V.Kind != EAssetDocumentRuntimeValueKind::Object
  || V.ObjectKeys.Num() != 1 || !Exact(V.ObjectKeys[0], TEXT("fields")) || V.Elements.Num() != 1) return false;
 const auto& F = V.Elements[0];
 if (F.Kind != EAssetDocumentRuntimeValueKind::Object || F.ObjectKeys.IsEmpty() || F.ObjectKeys.Num() > FieldCount
  || F.Elements.Num() != F.ObjectKeys.Num()) return false;
 FProjection P;
 for (int32 N = 0; N < F.ObjectKeys.Num(); ++N)
 {
  int32 Index = INDEX_NONE;
  for (int32 I = 0; I < FieldCount; ++I) if (Exact(F.ObjectKeys[N], Fields[I])) { Index = I; break; }
  if (Index == INDEX_NONE || P.Present[Index]) return false;
  P.Present[Index] = true; const auto& Value = F.Elements[N];
  if (Index == 0)
  {
   if (Value.Kind != EAssetDocumentRuntimeValueKind::Number || !FMath::IsFinite(Value.NumberValue)) return false;
   P.ScaleLiteral = Value.NumberValue; P.Scale = static_cast<float>(Value.NumberValue);
   if (!FMath::IsFinite(P.Scale) || (Value.NumberValue != 0. && P.Scale == 0.f)) return false;
  }
  else if (Index == 4)
  { if (Value.Kind != EAssetDocumentRuntimeValueKind::Boolean) return false; P.Keep = Value.BooleanValue; }
  else
  { if (Value.Kind != EAssetDocumentRuntimeValueKind::String || !CanonicalPath(Value.StringValue)) return false; P.Paths[Index - 1] = Value.StringValue; }
 }
 Out = MoveTemp(P); return true;
}
struct FTuple
{
 float Scale = 0.f; UObject* Settings[3] = {}; bool Keep = false;
 explicit FTuple(const UAnimSequence& S) : Scale(S.CompressionErrorThresholdScale), Keep(S.bDoNotOverrideCompression)
 { Settings[0] = S.BoneCompressionSettings.Get(); Settings[1] = S.CurveCompressionSettings.Get(); Settings[2] = S.VariableFrameStrippingSettings.Get(); }
 bool Matches(const UAnimSequence& S) const
 { return ExactFloat(Scale, S.CompressionErrorThresholdScale) && Settings[0] == S.BoneCompressionSettings.Get()
   && Settings[1] == S.CurveCompressionSettings.Get() && Settings[2] == S.VariableFrameStrippingSettings.Get() && Keep == bool(S.bDoNotOverrideCompression); }
 bool Same(const FTuple& Other) const
 { return ExactFloat(Scale, Other.Scale) && Settings[0] == Other.Settings[0] && Settings[1] == Other.Settings[1]
   && Settings[2] == Other.Settings[2] && Keep == Other.Keep; }
};
struct FIdentity
{
 TStrongObjectPtr<UObject> Object; TStrongObjectPtr<UPackage> Package;
 FString Path, PackagePath; uint32 Revision = 0; bool Dirty = false;
 explicit FIdentity(UObject* O) : Object(O), Package(O->GetOutermost()), Path(O->GetPathName()),
  PackagePath(O->GetOutermost()->GetName()), Revision(O->GetUniqueID()), Dirty(O->GetOutermost()->IsDirty()) {}
 bool Current(UClass* Class, bool ReadOnly) const
 { return Canonical(Object.Get(), Class) && Object->GetUniqueID() == Revision && Exact(Object->GetPathName(), Path)
   && Object->GetOutermost() == Package.Get() && Exact(Package->GetName(), PackagePath) && (!ReadOnly || Package->IsDirty() == Dirty); }
 bool MatchesView(const FAssetDocumentEditorAssetIdentityView& V) const
 { return &V.GetObject() == Object.Get() && V.GetTypeRevision() == Revision && Exact(FString(V.GetCanonicalObjectPath()), Path); }
};
struct FMutation final : IAssetDocumentEditorSequenceCompressionMutation
{
 FAssetDocumentRuntimeRequestDto Request; EAssetDocumentRuntimeRequestPhase Phase;
 FProjection Projection; FIdentity Target, Skeleton;
 FGuid SkeletonGuid, Transaction;
 FTuple Original, Desired;
 TUniquePtr<FIdentity> OriginalSettings[3], DesiredSettings[3];
 FProperty* Properties[FieldCount] = {};
 TArray<TUniquePtr<FAssetDocumentEditorScopedPropertyValue>> OldValues, DesiredValues;
 bool bOpened = false, bClosed = false, bConsumed = false, bWritten = false, bRestored = false;
 FMutation(const FAssetDocumentRuntimeRequestDto& R, EAssetDocumentRuntimeRequestPhase P, const FProjection& V, UAnimSequence& S)
  : Request(R), Phase(P), Projection(V), Target(&S), Skeleton(S.GetSkeleton()), SkeletonGuid(S.GetSkeleton()->GetGuid()), Original(S), Desired(S) {}
 UAnimSequence* Sequence() const { return CastChecked<UAnimSequence>(Target.Object.Get()); }
 bool IdentityCurrent() const
 {
  if (!Target.Current(UAnimSequence::StaticClass(), false) || !Skeleton.Current(USkeleton::StaticClass(), true)
   || Sequence()->GetSkeleton() != Skeleton.Object.Get() || Sequence()->GetSkeletonGuid() != SkeletonGuid
   || CastChecked<USkeleton>(Skeleton.Object.Get())->GetGuid() != SkeletonGuid) return false;
  for (int32 I = 0; I < 3; ++I)
   if (!OriginalSettings[I] || !DesiredSettings[I] || !OriginalSettings[I]->Current(SettingsClass(I), true)
    || !DesiredSettings[I]->Current(SettingsClass(I), true) || !SettingsValid(OriginalSettings[I]->Object.Get(), I)
    || !SettingsValid(DesiredSettings[I]->Object.Get(), I)) return false;
  return true;
 }
 bool PropertiesCurrent() const
 {
  for (int32 I = 0; I < FieldCount; ++I)
   if (!Properties[I] || Properties[I]->GetOwnerClass() != UAnimSequence::StaticClass() || Properties[I]->ArrayDim != 1
    || PropertyAccessUtil::CanSetPropertyValue(Properties[I], PropertyAccessUtil::EditorReadOnlyFlags, false) != EPropertyAccessResultFlags::Success) return false;
  return true;
 }
 bool IsOriginalCurrent() const override { return IdentityCurrent() && Original.Matches(*Sequence()); }
 bool IsDesiredCurrent() const override { return IdentityCurrent() && Desired.Matches(*Sequence()); }
 bool HasChanges() const override { return !Original.Same(Desired); }
 bool DidWrite() const override { return bWritten; }
 FAssetDocumentContractValidationResult OpenCapture(const FGuid& Id) override
 {
  if (!ContextValid() || !Id.IsValid() || bOpened || bClosed || !IsOriginalCurrent() || !PropertiesCurrent()) return Result(false);
  Transaction = Id; bOpened = true; return Result(true);
 }
 void CloseCapture() override { bClosed = true; }
 const FAssetDocumentPrimitiveHandleArgument* FindHandle(const FAssetDocumentPrimitiveInvocation& I, const TCHAR* Role) const
 { for (const auto& A : I.HandleArguments) if (Exact(A.Name, Role)) return &A; return nullptr; }
 bool InvocationShape(const FAssetDocumentPrimitiveInvocation& I) const
 {
  FProjection Runtime;
  if (!ContextValid() || !bOpened || bClosed || !Exact(I.PrimitiveId, Primitive) || I.Version != 1 || I.Phase != Phase
   || I.RequestId != Request.RequestId || I.RuntimeRevision != Request.ExpectedRuntimeRevision || I.TransactionId != Transaction
   || !Parse(I.ValueArguments, Runtime) || !Projection.Matches(Runtime)) return false;
  int32 Required = 1;
  for (int32 N = 0; N < 3; ++N) if (Projection.Present[N + 1]) ++Required;
  if (I.HandleArguments.Num() != Required || !FindHandle(I, TEXT("sequence"))) return false;
  for (int32 N = 0; N < 3; ++N) if (Projection.Present[N + 1] && !FindHandle(I, Roles[N])) return false;
  // With an exact count, duplicate roles cannot substitute for a required role.
  return true;
 }
 FAssetDocumentHandleValidationContext HandleContext(const FAssetDocumentPrimitiveInvocation& I, EAssetDocumentHandleAccess Access) const
 {
  FAssetDocumentHandleValidationContext C; C.RequestId = I.RequestId; C.TransactionId = I.TransactionId;
  C.RuntimeRevision = I.RuntimeRevision; C.ProviderId = TEXT("editor"); C.Phase = I.Phase;
  C.RequiredKind = EAssetDocumentHandleKind::Asset; C.RequiredAccess = Access; return C;
 }
 FAssetDocumentContractValidationResult WithSettings(int32 Index, const FAssetDocumentPrimitiveInvocation& I,
  IAssetDocumentHandleResolver& Handles, TFunctionRef<FAssetDocumentContractValidationResult()> Body) const
 {
  if (Index == 3) return Body();
  if (!Projection.Present[Index + 1]) return WithSettings(Index + 1, I, Handles, Body);
  const auto* A = FindHandle(I, Roles[Index]);
  if (!Exact(A->Handle.ProviderId, TEXT("editor"))) return Result(false);
  auto Lease = Handles.AcquireVerifiedLease(A->Handle, HandleContext(I, EAssetDocumentHandleAccess::ReadOnly));
  if (!Lease.Lease) return Result(false);
  return WithAssetDocumentEditorAssetIdentity(*Lease.Lease, [&](const auto& V)
  {
   if (!ContextValid() || !IdentityCurrent() || !DesiredSettings[Index]->MatchesView(V)) return Result(false);
   const auto R = WithSettings(Index + 1, I, Handles, Body);
   return ContextValid() && IdentityCurrent() && DesiredSettings[Index]->MatchesView(V) ? R : Result(false);
  });
 }
 FAssetDocumentContractValidationResult WithVerifiedAssets(const FAssetDocumentPrimitiveInvocation& I,
  IAssetDocumentHandleResolver& Handles, TFunctionRef<FAssetDocumentContractValidationResult()> Body) const override
 {
  if (!InvocationShape(I) || !IdentityCurrent() || !PropertiesCurrent() || !Sequence()->CanModify()) return Result(false);
  const auto* A = FindHandle(I, TEXT("sequence"));
  if (!Exact(A->Handle.ProviderId, TEXT("editor"))) return Result(false);
  auto Lease = Handles.AcquireVerifiedLease(A->Handle, HandleContext(I, EAssetDocumentHandleAccess::Mutable));
  if (!Lease.Lease) return Result(false);
  return WithAssetDocumentEditorAssetIdentity(*Lease.Lease, [&](const auto& V)
  {
   if (!ContextValid() || !IdentityCurrent() || !Target.MatchesView(V)) return Result(false);
   const auto R = WithSettings(0, I, Handles, Body);
   return ContextValid() && IdentityCurrent() && Target.MatchesView(V) ? R : Result(false);
  });
 }
 FAssetDocumentContractValidationResult ValidateInvocation(const FAssetDocumentPrimitiveInvocation& I,
  IAssetDocumentHandleResolver& Handles) const override
 {
  if (bConsumed || !IsOriginalCurrent()) return Result(false);
  return WithVerifiedAssets(I, Handles, [&] { return Result(true); });
 }
 bool MakeValues(const FTuple& Tuple, TArray<TUniquePtr<FAssetDocumentEditorScopedPropertyValue>>& Values)
 {
  for (int32 I = 0; I < FieldCount; ++I)
  {
   auto Value = MakeUnique<FAssetDocumentEditorScopedPropertyValue>(*Properties[I]);
   if (!Value->IsValid()) return false;
   // These typed writes are confined to temporary property-layout storage.
   // In particular the boolean uses the native bitfield mask, not &bool.
   if (I == 0) CastFieldChecked<FFloatProperty>(Properties[I])->SetPropertyValue(Value->GetMemory(), Tuple.Scale);
   else if (I == 4) CastFieldChecked<FBoolProperty>(Properties[I])->SetPropertyValue(Value->GetMemory(), Tuple.Keep);
   else CastFieldChecked<FObjectPropertyBase>(Properties[I])->SetObjectPropertyValue(Value->GetMemory(), Tuple.Settings[I - 1]);
   Values.Add(MoveTemp(Value));
  }
  return true;
 }
 bool Write(const TArray<TUniquePtr<FAssetDocumentEditorScopedPropertyValue>>& Values, const FTuple& Tuple, bool Inverse)
 {
  for (int32 I = 0; I < FieldCount; ++I)
  {
   if (!ContextValid() || !IdentityCurrent() || !PropertiesCurrent()) return false;
   const auto R = PropertyAccessUtil::SetPropertyValue_Object(Properties[I], Sequence(), Properties[I], Values[I]->GetMemory(),
    INDEX_NONE, PropertyAccessUtil::EditorReadOnlyFlags, EPropertyAccessChangeNotifyMode::Never);
   if (R != EPropertyAccessResultFlags::Success) return false;
#if WITH_DEV_AUTOMATION_TESTS
   if (!Inverse && AssetDocumentEditorSequenceCompressionTesting::ActiveHooks)
   {
    auto& H = *AssetDocumentEditorSequenceCompressionTesting::ActiveHooks; ++H.FieldWrites;
    if (H.FailAfterField == I) return false;
   }
#endif
  }
  return IdentityCurrent() && Tuple.Matches(*Sequence());
 }
 FAssetDocumentContractValidationResult Apply(const FAssetDocumentPrimitiveInvocation& I, IAssetDocumentHandleResolver& Handles,
  IAssetDocumentAtomicTransaction& T) override
 {
  const auto Valid = ValidateInvocation(I, Handles);
  if (!Valid.bValid || T.GetState() != EAssetDocumentAtomicState::Mutating || T.GetTransactionId() != Transaction
   || T.GetMutationToken().RequestId != I.RequestId || T.GetMutationToken().RuntimeRevision != I.RuntimeRevision
   || T.GetMutationToken().TransactionId != I.TransactionId) return Result(false);
  return WithVerifiedAssets(I, Handles, [&]
  {
   if (bConsumed || bClosed || !IsOriginalCurrent()) return Result(false);
   if (!HasChanges()) { bConsumed = true; return Result(true); }
   const TArray<FAssetDocumentMutationFootprint> Footprint = {
    {EAssetDocumentMutationFootprintKind::UObjectState, TEXT("Object:") + Target.Path, TEXT("rollback.uobject")},
    {EAssetDocumentMutationFootprintKind::Package, TEXT("Package:") + Target.PackagePath, TEXT("rollback.package")}};
   const auto Enlisted = T.EnlistFootprint(Footprint); if (!Enlisted.bValid) return Enlisted;
   // Parent enlistment can run code. Reacquire every actual identity after it
   // before the first setter, while the outer identity callbacks stay held.
   const auto AfterEnlist = ValidateInvocation(I, Handles);
   if (!AfterEnlist.bValid) return AfterEnlist;
   bWritten = bConsumed = true; return Result(Write(DesiredValues, Desired, false));
  });
 }
 FAssetDocumentContractValidationResult Restore() override
 {
  if (!ContextValid() || !bOpened || !bClosed || bRestored) return Result(false);
  bRestored = true;
  return !bWritten ? Result(true) : Result(IdentityCurrent() && PropertiesCurrent() && Sequence()->CanModify() && Write(OldValues, Original, true));
 }
};
}
}

FAssetDocumentContractValidationResult PrepareAssetDocumentEditorSequenceCompressionMutation(
 const FAssetDocumentRuntimeRequestDto& Request, const FAssetDocumentMutationIntent& Intent,
 TSharedPtr<IAssetDocumentEditorSequenceCompressionMutation>& OutMutation)
{
 using namespace AssetDocumentEditorSequenceCompression;
 OutMutation.Reset(); FProjection Projection;
 if (!ContextValid() || !Request.RequestId.IsValid() || !Request.ExpectedRuntimeRevision
  || Request.Operation != EAssetDocumentRuntimeOperation::Apply || !Exact(Request.RequestedAssetClassPath, TEXT("/Script/Engine.AnimSequence"))
  || !Exact(Intent.Requirement.PrimitiveId, Primitive) || Intent.Requirement.Version != 1 || Intent.StepIndex < 0
  || !Exact(Intent.Requirement.RequiredByRouteId, TEXT("animation_profile")) || !Exact(Intent.InputSchemaId, Schema)
  || (Intent.Phase != EAssetDocumentRuntimeRequestPhase::Apply && Intent.Phase != EAssetDocumentRuntimeRequestPhase::Repair)
  || !Parse(Intent.Intent, Projection) || !CanonicalPath(Request.TargetAssetPath)) return Result(false, TEXT("Editor.SequenceCompression.IntentShape"));
 auto* Target = Cast<UAnimSequence>(FSoftObjectPath(Request.TargetAssetPath).ResolveObject());
 if (!Canonical(Target, UAnimSequence::StaticClass()) || !Exact(Target->GetPathName(), Request.TargetAssetPath) || !Target->CanModify()
  || !Canonical(Target->GetSkeleton(), USkeleton::StaticClass()) || Target->GetSkeletonGuid() != Target->GetSkeleton()->GetGuid()
  || !FMath::IsFinite(Target->CompressionErrorThresholdScale)) return Result(false);
 auto S = MakeShared<FMutation>(Request, Intent.Phase, Projection, *Target);
 if (Projection.Present[0]) S->Desired.Scale = Projection.Scale;
 if (Projection.Present[4]) S->Desired.Keep = Projection.Keep;
 for (int32 I = 0; I < 3; ++I)
 {
  UObject* Old = S->Original.Settings[I];
  UObject* New = Projection.Present[I + 1] ? FSoftObjectPath(Projection.Paths[I]).ResolveObject() : Old;
  if (!SettingsValid(Old, I) || !SettingsValid(New, I) || Old->GetOutermost() == Target->GetOutermost()
   || New->GetOutermost() == Target->GetOutermost() || (Projection.Present[I + 1] && !Exact(New->GetPathName(), Projection.Paths[I]))) return Result(false);
  S->OriginalSettings[I] = MakeUnique<FIdentity>(Old); S->DesiredSettings[I] = MakeUnique<FIdentity>(New); S->Desired.Settings[I] = New;
 }
 S->Properties[0] = FindFProperty<FFloatProperty>(UAnimSequence::StaticClass(), GET_MEMBER_NAME_CHECKED(UAnimSequence, CompressionErrorThresholdScale));
 S->Properties[1] = FindFProperty<FObjectPropertyBase>(UAnimSequence::StaticClass(), GET_MEMBER_NAME_CHECKED(UAnimSequence, BoneCompressionSettings));
 S->Properties[2] = FindFProperty<FObjectPropertyBase>(UAnimSequence::StaticClass(), GET_MEMBER_NAME_CHECKED(UAnimSequence, CurveCompressionSettings));
 S->Properties[3] = FindFProperty<FObjectPropertyBase>(UAnimSequence::StaticClass(), GET_MEMBER_NAME_CHECKED(UAnimSequence, VariableFrameStrippingSettings));
 S->Properties[4] = FindFProperty<FBoolProperty>(UAnimSequence::StaticClass(), GET_MEMBER_NAME_CHECKED(UAnimSequence, bDoNotOverrideCompression));
 if (!S->PropertiesCurrent()) return Result(false);
 for (int32 I = 0; I < 3; ++I)
  if (CastFieldChecked<FObjectPropertyBase>(S->Properties[I + 1])->PropertyClass != SettingsClass(I)) return Result(false);
 if (!S->MakeValues(S->Original, S->OldValues) || !S->MakeValues(S->Desired, S->DesiredValues)) return Result(false);
 // The stock high-level helper skips copying native-identical floats. Retain
 // the captured byte representation for that native no-op (including +/-0),
 // while Projection continues binding the caller's original double literal.
 if (PropertyAccessUtil::IsSinglePropertyIdentical(S->Properties[0], S->OldValues[0]->GetMemory(),
  S->Properties[0], S->DesiredValues[0]->GetMemory()))
 {
  S->Desired.Scale = S->Original.Scale;
  CastFieldChecked<FFloatProperty>(S->Properties[0])->SetPropertyValue(S->DesiredValues[0]->GetMemory(), S->Desired.Scale);
 }
 if (!S->IsOriginalCurrent()) return Result(false);
 OutMutation = S; return Result(true);
}
