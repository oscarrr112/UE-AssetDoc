// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// Provider "animation_asset": descriptors, bindings and the helpers the per-class primitive files
// share. The per-class files do what the corresponding editor does for the same edit:
//   Skeleton     -> FEditableSkeleton (Skeleton editor: Anim Slot Manager, skeleton tree sockets /
//                   virtual bones / retargeting, blend profile picker, Anim Curves tab);
//   Blend spaces -> SBlendSpaceEditor (sample add / move / replace / remove) and FBlendSpaceDetails;
//   PoseAsset    -> SPoseViewer (rename / delete poses) and FPoseAssetDetails (additive, update source);
//   AnimComposite-> SAnimCompositePanel / SAnimSegmentsPanel (segments, collapse, composite length).
// There is no inverse code: Core rolls back the whole package snapshot.

#include "AnimAssets/AssetDocumentAnimAssetPrimitives.h"

#include "AssetDocumentAnimationAssetProvider.h"
#include "AssetDocumentGraphProvider.h"

#include "Animation/AnimationAsset.h"
#include "Animation/Skeleton.h"
#include "Engine/SkeletalMesh.h"
#include "UObject/SoftObjectPath.h"

namespace UE::AssetDocument::Graph::AnimAssets
{
namespace
{
constexpr uint32 AnimAssetProviderVersion = 1;

FAssetDocumentPrimitiveDescriptor MakeDescriptor(const TCHAR* PrimitiveId, bool bMutating, const TCHAR* AuthoritativeApi)
{
	FAssetDocumentPrimitiveDescriptor Descriptor;
	Descriptor.PrimitiveId = PrimitiveId;
	Descriptor.Version = AnimAssetProviderVersion;
	Descriptor.ProviderId = ProviderId;
	Descriptor.Owner = EAssetDocumentPrimitiveOwner::AnimationAsset;
	Descriptor.ExecutionThread = EAssetDocumentExecutionThread::GameThread;
	Descriptor.Access = bMutating ? EAssetDocumentPrimitiveAccess::Mutating : EAssetDocumentPrimitiveAccess::ReadOnly;
	if (bMutating)
	{
		Descriptor.AllowedPhases = {EAssetDocumentRuntimeRequestPhase::Apply, EAssetDocumentRuntimeRequestPhase::Repair};
		Descriptor.MutationFootprintSchemaId = TEXT("graph.footprint.whole-package.v1");
	}
	else
	{
		Descriptor.AllowedPhases = {EAssetDocumentRuntimeRequestPhase::Validate, EAssetDocumentRuntimeRequestPhase::Preflight,
			EAssetDocumentRuntimeRequestPhase::Apply, EAssetDocumentRuntimeRequestPhase::Repair,
			EAssetDocumentRuntimeRequestPhase::Verify};
	}
	Descriptor.bRequiresActiveTransaction = bMutating;
	Descriptor.InputSchemaId = Descriptor.PrimitiveId + TEXT(".input.v1");
	Descriptor.OutputSchemaId = Descriptor.PrimitiveId + TEXT(".output.v1");
	Descriptor.StableDiagnosticCodes = {
		TEXT("AD.RUNTIME.PRIMITIVE_FAILED"), TEXT("Graph.UnknownField"), TEXT("Graph.InvalidArgument"),
		TEXT("Graph.UnsupportedSchemaVersion"), TEXT("Graph.PieActive"), TEXT("Graph.ResultTooLarge"),
		TEXT("AnimationAsset.UnsupportedAsset"), TEXT("AnimationAsset.InvalidOp"), TEXT("AnimationAsset.ReferenceNotFound"),
		TEXT("AnimationAsset.IncompatibleSkeleton"),
		TEXT("Skeleton.BoneNotFound"), TEXT("Skeleton.InvalidName"), TEXT("Skeleton.NameExists"),
		TEXT("Skeleton.SlotNotFound"), TEXT("Skeleton.SlotGroupNotFound"), TEXT("Skeleton.SlotInUse"),
		TEXT("Skeleton.DefaultGroupRequired"), TEXT("Skeleton.SocketNotFound"), TEXT("Skeleton.VirtualBoneNotFound"),
		TEXT("Skeleton.InvalidVirtualBone"), TEXT("Skeleton.BlendProfileNotFound"), TEXT("Skeleton.InvalidBlendProfileMode"),
		TEXT("Skeleton.InvalidRetargetingMode"), TEXT("Skeleton.CurveNotFound"),
		TEXT("BlendSpace.InvalidAxis"), TEXT("BlendSpace.SampleNotFound"), TEXT("BlendSpace.InvalidSample"),
		TEXT("BlendSpace.SampleOutOfRange"), TEXT("BlendSpace.IncompatibleAnimation"),
		TEXT("PoseAsset.PoseNotFound"), TEXT("PoseAsset.PoseExists"), TEXT("PoseAsset.InvalidPoseName"),
		TEXT("AnimComposite.InvalidSegment"), TEXT("AnimComposite.IncompatibleAnimation")};
	Descriptor.RequiredConsumerIds = {TEXT("route.generic.v1")};
	Descriptor.AllowedHandleKinds = {EAssetDocumentHandleKind::Asset};
	Descriptor.HandleArgumentSchemas.Add({TEXT("asset"), EAssetDocumentHandleKind::Asset,
		bMutating ? EAssetDocumentHandleAccess::Mutable : EAssetDocumentHandleAccess::ReadOnly, true});

	FAssetDocumentPrimitiveAdmissionEvidence& Evidence = Descriptor.Admission;
	Evidence.Disposition = EAssetDocumentPrimitiveDisposition::OrdinaryHelper;
	Evidence.AuthorityPath = EAssetDocumentPrimitiveAuthorityPath::ExistingExportedHelper;
	Evidence.ExactEngineVersion = TEXT("5.7.4");
	Evidence.PhysicalOwner = EAssetDocumentPrimitiveOwner::AnimationAsset;
	Evidence.PublicModule = TEXT("AssetDocumentAnimation");
	Evidence.PublicHeader = TEXT("AssetDocumentAnimationAssetProvider.h");
	Evidence.ExportedAuthoritativeApi = AuthoritativeApi;
	Evidence.AuthorityContextId = TEXT("assetdocument.editor.game-thread.editor-ready.v1");
	Evidence.PrimaryOperationId = Descriptor.PrimitiveId;
	Evidence.IdentityLifetimeContractId = TEXT("identity.request-transaction-revision-bound.v1");
	Evidence.TransactionContractId = TEXT("transaction.descriptor-governed.v1");
	Evidence.RollbackContractId = TEXT("rollback.core-package-snapshot.v1");
	Evidence.CompletionContractId = Descriptor.PrimitiveId + TEXT(".completion.v1");
	Evidence.SaveReloadEvidenceId = TEXT("UEAssetDoc.AnimationAssets");
	Evidence.FailureInjectionTestIds = {TEXT("UEAssetDoc.AnimationAssets")};
	Evidence.VerificationTestIds = {TEXT("UEAssetDoc.AnimationAssets")};
	Evidence.AuthorityFingerprint = FString::Printf(TEXT("ue-5.7.4|animation_asset|%s|%s"), PrimitiveId, AuthoritativeApi);
	return Descriptor;
}

TArray<FAssetDocumentPrimitiveDescriptor> BuildDescriptors()
{
	return {
		MakeDescriptor(TEXT("animation_asset.skeleton.read.v1"), false,
			TEXT("USkeleton::GetReferenceSkeleton;USkeleton::GetSlotGroups;USkeleton::GetVirtualBones;USkeleton::GetBlendProfile;USkeleton::GetCurveMetaData")),
		MakeDescriptor(TEXT("animation_asset.skeleton.edit.v1"), true,
			TEXT("USkeleton::AddSlotGroupName;USkeleton::SetSlotGroupName;USkeleton::RemoveSlotName;USkeleton::RemoveSlotGroup;USkeleton::RenameSlotName;USkeleton::AddNewNamedVirtualBone;USkeleton::RemoveVirtualBones;USkeleton::RenameVirtualBone;USkeleton::CreateNewBlendProfile;USkeleton::RenameBlendProfile;UBlendProfile::SetBoneBlendScale;UBlendProfile::RemoveEntry;USkeleton::SetBoneTranslationRetargetingMode;USkeleton::AddCurveMetaData;USkeleton::RenameCurveMetaData;USkeleton::RemoveCurveMetaData;USkeleton::SetCurveMetaDataMaterial;USkeleton::SetCurveMetaDataMorphTarget")),
		MakeDescriptor(TEXT("animation_asset.blend_space.read.v1"), false,
			TEXT("UBlendSpace::GetBlendParameter;UBlendSpace::GetBlendSample")),
		MakeDescriptor(TEXT("animation_asset.blend_space.edit.v1"), true,
			TEXT("UBlendSpace::AddSample;UBlendSpace::DeleteSample;UBlendSpace::ReplaceSampleAnimation;UBlendSpace::ValidateSampleData;UBlendSpace::ResampleData;UObject::PostEditChangeProperty;UAnimationAsset::SetPreviewMesh")),
		MakeDescriptor(TEXT("animation_asset.pose_asset.read.v1"), false,
			TEXT("UPoseAsset::GetPoseFNames;UPoseAsset::GetCurveFNames;UPoseAsset::GetBasePoseIndex")),
		MakeDescriptor(TEXT("animation_asset.pose_asset.edit.v1"), true,
			TEXT("UPoseAsset::ModifyPoseName;UPoseAsset::DeletePoses;UPoseAsset::ConvertSpace;UPoseAsset::UpdatePoseFromAnimation;UAnimationAsset::SetPreviewMesh")),
		MakeDescriptor(TEXT("animation_asset.composite.read.v1"), false,
			TEXT("UAnimComposite::AnimationTrack;UAnimSequenceBase::GetPlayLength")),
		MakeDescriptor(TEXT("animation_asset.composite.edit.v1"), true,
			TEXT("FAnimSegment::SetAnimReference;FAnimTrack::IsValidToAdd;FAnimTrack::CollapseAnimSegments;UAnimComposite::UpdateCommonTargetFrameRate;UAnimComposite::SetCompositeLength;UAnimationAsset::SetPreviewMesh")),
	};
}

const FBinding Bindings[] = {
	{TEXT("animation_asset.skeleton.read.v1"), &HandleSkeletonRead},
	{TEXT("animation_asset.skeleton.edit.v1"), &HandleSkeletonEdit},
	{TEXT("animation_asset.blend_space.read.v1"), &HandleBlendSpaceRead},
	{TEXT("animation_asset.blend_space.edit.v1"), &HandleBlendSpaceEdit},
	{TEXT("animation_asset.pose_asset.read.v1"), &HandlePoseAssetRead},
	{TEXT("animation_asset.pose_asset.edit.v1"), &HandlePoseAssetEdit},
	{TEXT("animation_asset.composite.read.v1"), &HandleCompositeRead},
	{TEXT("animation_asset.composite.edit.v1"), &HandleCompositeEdit},
};
} // namespace

bool Fail(FCall& Call, FStringView Code, FStringView Message, FStringView Path)
{
	if (Call.Error.Code.IsEmpty())
	{
		Call.Error.Set(Code, Message, Path);
	}
	return false;
}

FString PathOf(const UObject* Object)
{
	return Object != nullptr ? Object->GetPathName() : FString();
}

UObject* LoadReference(FReader& Op, const TCHAR* Key, UClass* Class, const TCHAR* What)
{
	FString Path;
	if (!Op.String(Key, Path))
	{
		return nullptr;
	}
	const FSoftObjectPath SoftPath(Path);
	UObject* Object = SoftPath.IsValid() && !Path.Contains(TEXT(":")) ? SoftPath.TryLoad() : nullptr;
	if (Object == nullptr || (Class != nullptr && !Object->IsA(Class)))
	{
		Op.Fail(TEXT("AnimationAsset.ReferenceNotFound"), FString::Printf(TEXT("'%s' is not the object path of %s (e.g. /Game/Anims/Walk.Walk)."),
			*Path, What), Key);
		return nullptr;
	}
	return Object;
}

void AddCommonFields(const UAnimationAsset& Asset, FObjectBuilder& Result)
{
	Result.AddString(TEXT("skeleton"), PathOf(Asset.GetSkeleton()));
	Result.AddString(TEXT("preview_mesh"), Asset.GetPreviewMesh() != nullptr ? Asset.GetPreviewMesh()->GetPathName()
		: FString());
}

bool SetPreviewMesh(FReader& Op, UAnimationAsset& Asset)
{
	FString Path;
	if (!Op.OnlyFields({TEXT("op"), TEXT("path")}) || !Op.String(TEXT("path"), Path))
	{
		return false;
	}
	USkeletalMesh* Mesh = nullptr;
	if (!Path.IsEmpty())
	{
		Mesh = Cast<USkeletalMesh>(LoadReference(Op, TEXT("path"), USkeletalMesh::StaticClass(), TEXT("a Skeletal Mesh")));
		if (Mesh == nullptr)
		{
			return false;
		}
		const USkeleton* Skeleton = Asset.GetSkeleton();
		if (Skeleton == nullptr || Mesh->GetSkeleton() == nullptr || !Skeleton->IsCompatibleForEditor(Mesh->GetSkeleton()))
		{
			return Op.Fail(TEXT("AnimationAsset.IncompatibleSkeleton"), FString::Printf(
				TEXT("Preview mesh %s uses skeleton %s, which is not compatible with the asset's skeleton %s."),
				*Path, *PathOf(Mesh->GetSkeleton()), *PathOf(Skeleton)), TEXT("path"));
		}
	}
	if (static_cast<const UAnimationAsset&>(Asset).GetPreviewMesh() != Mesh)
	{
		// FPersonaPreviewScene::SetPreviewMesh with "apply to asset": SetPreviewMesh marks the package dirty.
		Asset.Modify();
		Asset.SetPreviewMesh(Mesh, true);
	}
	return true;
}

double ShortestFloat(float Value)
{
	if (!FMath::IsFinite(Value))
	{
		return Value;
	}
	for (int32 Digits = 1; Digits <= 9; ++Digits)
	{
		const FString Text = FString::Printf(TEXT("%.*g"), Digits, static_cast<double>(Value));
		const double Parsed = FCString::Atod(*Text);
		if (static_cast<float>(Parsed) == Value)
		{
			return Parsed;
		}
	}
	return Value;
}

FValue FloatValue(double Value)
{
	return MakeNumber(ShortestFloat(static_cast<float>(Value)));
}

bool ReadFloat(FReader& Op, const TCHAR* Key, double& Out, bool bRequired)
{
	if (!Op.Has(Key) && !bRequired)
	{
		return true;
	}
	if (!Op.Number(Key, Out, bRequired))
	{
		return false;
	}
	if (!FMath::IsFinite(Out) || FMath::Abs(Out) > 1.0e30)
	{
		return Op.Fail(TEXT("Graph.InvalidArgument"), FString::Printf(TEXT("%s must be a finite number."), Key), Key);
	}
	return true;
}

TConstArrayView<FAssetDocumentPrimitiveDescriptor> GetDescriptors()
{
	static const TArray<FAssetDocumentPrimitiveDescriptor> Descriptors = BuildDescriptors();
	return Descriptors;
}

TConstArrayView<FBinding> GetBindings()
{
	return Bindings;
}
} // namespace UE::AssetDocument::Graph::AnimAssets

TSharedRef<IAssetDocumentPrimitiveProvider> CreateAssetDocumentAnimationAssetPrimitiveProvider()
{
	using namespace UE::AssetDocument::Graph;
	return CreateAssetDocumentAssetTablePrimitiveProvider(AnimAssets::ProviderId, &AnimAssets::GetDescriptors, &AnimAssets::GetBindings);
}
