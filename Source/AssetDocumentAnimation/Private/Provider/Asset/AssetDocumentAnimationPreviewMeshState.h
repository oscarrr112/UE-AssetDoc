// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "Provider/Asset/AssetDocumentAnimationSkeletonState.h"
#include "Provider/Asset/AssetDocumentAnimationPreviewMeshOperations.h"
#include "Provider/Asset/AssetDocumentAnimationPreviewMeshRetainedState.h"
#include "Animation/AnimSequence.h"
#include "Engine/SkeletalMesh.h"

namespace AssetDocumentAnimationPreviewMesh
{
inline constexpr const TCHAR* Primitive = TEXT("animation.asset.preview_mesh.set.v1");
inline constexpr const TCHAR* Schema = TEXT("animation.asset.preview_mesh.set.v1.input.v1");
inline constexpr const TCHAR* RollbackId = TEXT("animation.asset.preview_mesh.set.v1.rollback-subaction.v1");
// Reuse private identity/lease/physical footprint helpers; no second Core mechanism.
using AssetDocumentAnimationSkeleton::ContextValid;
using AssetDocumentAnimationSkeleton::CanonicalPath;
using AssetDocumentAnimationSkeleton::ExactObject;
using AssetDocumentAnimationSkeleton::Acquire;
using AssetDocumentAnimationSkeleton::Footprint;
inline FAssetDocumentContractValidationResult Result(bool bValid, const TCHAR* Code = TEXT("AnimationProvider.PreviewMesh.InvalidTarget"))
{
 FAssetDocumentContractValidationResult R; R.bValid = bValid;
 if (!bValid)
 {
  auto& D = R.Diagnostics.AddDefaulted_GetRef(); D.Path = TEXT("/PreviewMesh"); D.Code = Code;
  D.Message = TEXT("PreviewMesh requires a stable, canonical compatible reference and prepared target.");
 }
 return R;
}
// Classification-only snapshot projection; no stored soft path escapes this helper.
bool ReadMontageProfilePreviewMesh(const UAnimMontage& Target, USkeletalMesh*& OutMesh);
inline USkeletalMesh* ReadPreviewMesh(const UAnimationAsset& Asset) { return Asset.GetPreviewMesh(); }
inline bool ExactTarget(const UObject* Object)
{
 return ExactObject(Object, UAnimMontage::StaticClass()) || ExactObject(Object, UAnimSequence::StaticClass());
}
inline bool CanonicalSkeleton(const UAnimationAsset& Target)
{
 const USkeleton* S = Target.GetSkeleton();
 return S ? ExactObject(S, USkeleton::StaticClass()) && Target.GetSkeletonGuid() == S->GetGuid()
  : !Target.GetSkeletonGuid().IsValid();
}
inline bool Compatible(const UAnimationAsset& Target, const USkeletalMesh* Mesh)
{
 return !Mesh || (ExactObject(Mesh, USkeletalMesh::StaticClass())
  && ExactObject(Mesh->GetSkeleton(), USkeleton::StaticClass())
  && ExactObject(Target.GetSkeleton(), USkeleton::StaticClass())
  && Mesh->GetSkeleton()->IsCompatibleForEditor(Target.GetSkeleton()));
}
inline FAssetDocumentContractValidationResult AcquireTarget(
 const FAssetDocumentPrimitiveInvocation& Invocation, IAssetDocumentHandleResolver& Handles,
 TUniquePtr<IAssetDocumentVerifiedHandleLease>& Lease, UObject*& Object)
{
 auto Acquired = AcquireAnimationInputLease(Handles, Invocation, TEXT("asset"), EAssetDocumentHandleKind::Asset, EAssetDocumentHandleAccess::Mutable);
 if (!Acquired.Lease)
 {
  auto Failure = Result(false, TEXT("AnimationProvider.InvalidHandlePayload"));
  if (!Acquired.Diagnostics.IsEmpty()) Failure.Diagnostics = MoveTemp(Acquired.Diagnostics);
  return Failure;
 }
 Lease = MoveTemp(Acquired.Lease);
 return WithAssetDocumentEditorAssetIdentity(*Lease, [&](const FAssetDocumentEditorAssetIdentityView& Identity)
 {
  UObject& Native = Identity.GetObject();
  if (!ExactTarget(&Native)) return Result(false, TEXT("AnimationProvider.TargetClassMismatch"));
  if (Identity.GetTypeRevision() != Native.GetUniqueID() || Identity.GetCanonicalObjectPath() != Native.GetPathName())
   return Result(false, TEXT("AnimationProvider.InvalidHandlePayload"));
  Object = &Native; return Result(true);
 });
}
inline bool ParseOperation(const FAssetDocumentRuntimeValueDto& Value, bool bPreparation, FString& Operation, FString& Path)
{
 if (!ValidateAssetDocumentRuntimeValue(Value).bValid || Value.Kind != EAssetDocumentRuntimeValueKind::Object
  || Value.ObjectKeys.Num() != (bPreparation ? 2 : 1) || Value.Elements.Num() != Value.ObjectKeys.Num()) return false;
 const int32 Op = Value.ObjectKeys.IndexOfByKey(TEXT("operation"));
 if (!Value.Elements.IsValidIndex(Op) || Value.Elements[Op].Kind != EAssetDocumentRuntimeValueKind::String) return false;
 Operation = Value.Elements[Op].StringValue;
 if (Operation != TEXT("set") && Operation != TEXT("clear")) return false;
 if (!bPreparation) return true;
 const int32 Ref = Value.ObjectKeys.IndexOfByKey(TEXT("preview_mesh_path"));
 if (!Value.Elements.IsValidIndex(Ref)) return false;
 if (Operation == TEXT("clear")) return Value.Elements[Ref].Kind == EAssetDocumentRuntimeValueKind::Null;
 if (Value.Elements[Ref].Kind != EAssetDocumentRuntimeValueKind::String) return false;
 Path = Value.Elements[Ref].StringValue; return CanonicalPath(Path);
}
inline bool ExactHandles(const FAssetDocumentPrimitiveInvocation& Invocation, bool bSet)
{
 if (Invocation.HandleArguments.Num() != (bSet ? 2 : 1)) return false;
 int32 Assets = 0, Meshes = 0;
 for (const auto& Arg : Invocation.HandleArguments)
 { Assets += Arg.Name == TEXT("asset"); Meshes += Arg.Name == TEXT("preview_mesh"); }
 return Assets == 1 && Meshes == (bSet ? 1 : 0);
}
// Capture target Skeleton independently: the preview leaf does not authorize its mutation.
struct FSkeletonIdentity
{
 TStrongObjectPtr<USkeleton> Object;
 FString Path;
 FGuid Guid;
 explicit FSkeletonIdentity(USkeleton* S) : Object(S), Path(S ? S->GetPathName() : FString()), Guid(S ? S->GetGuid() : FGuid()) {}
 bool Matches(USkeleton* S) const
 {
  return S == Object.Get() && (!S || (ExactObject(S, USkeleton::StaticClass())
   && S->GetPathName() == Path && S->GetGuid() == Guid && FSoftObjectPath(Path).ResolveObject() == S));
 }
};
#if WITH_DEV_AUTOMATION_TESTS
extern AssetDocumentAnimationPreviewMeshTesting::FHooks* TestHooks;
#endif
}
