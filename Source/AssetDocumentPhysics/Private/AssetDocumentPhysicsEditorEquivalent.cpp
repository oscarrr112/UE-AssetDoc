// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// Structural edits of a Physics Asset: adding and removing bodies and joints and switching collision between two
// bodies. Each leaves the asset as the Physics Asset Editor leaves it after the same command (the bodies, joints, joint
// frames and profiles, and the collision table), without the editor's selection, tree and viewport refreshes. Every
// engine call used is public (FPhysicsAssetUtils, UPhysicsAsset, FConstraintInstance). Nothing else in the provider
// adds or removes bodies or joints.
//
//   AddBody            Add Body on a bone: with a fit (the document's Fit) the fitted shapes are named after the bone;
//                      without one the caller writes the document's shapes afterwards. Physical bodies below the bone
//                      are re-hung from the new body and the new body hangs from the body that controlled the bone.
//   RemoveBody         Delete Body: the nearest bodies below get copies of their joints hanging from the body above.
//   AddJoint           a joint between two bodies, named by the document instead of the editor's UserConstraint[_N].
//   ConnectJoint       points a joint at a child and a parent body, at the child bone's origin, with the default
//                      profile, and turns collision between the two bodies off.
//   SetPairCollision   collision on or off between two bodies.

#include "AssetDocumentPhysicsPrimitives.h"

#include "Engine/SkeletalMesh.h"
#include "MeshUtilitiesCommon.h"
#include "Misc/EngineVersionComparison.h"
#include "PhysicsAssetUtils.h"
#include "PhysicsEngine/AggregateGeom.h"
#include "PhysicsEngine/ConstraintInstance.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/PhysicsConstraintTemplate.h"
#include "PhysicsEngine/SkeletalBodySetup.h"
#include "ReferenceSkeleton.h"

namespace UE::AssetDocument::Graph::Physics::EditorEquivalent
{
namespace
{
// Shape-name suffixes of the fitted geometry types; elements of a fitted body are named "<bone>_<suffix>".
template <typename TElements>
void NameElements(TElements& Elements, const FName Name)
{
	for (auto& Element : Elements)
	{
		Element.SetName(Name);
	}
}

void NameFittedShapes(USkeletalBodySetup& Body, EPhysAssetFitGeomType GeomType)
{
	FKAggregateGeom& Geometry = Body.AggGeom;
	auto Named = [&Body](const TCHAR* Suffix) { return FName(*FString::Printf(TEXT("%s_%s"), *Body.BoneName.ToString(), Suffix)); };
	switch (GeomType)
	{
	case EFG_Box: NameElements(Geometry.BoxElems, Named(TEXT("box"))); break;
	case EFG_Sphere: NameElements(Geometry.SphereElems, Named(TEXT("sphere"))); break;
	case EFG_Sphyl: NameElements(Geometry.SphylElems, Named(TEXT("capsule"))); break;
	case EFG_TaperedCapsule: NameElements(Geometry.TaperedCapsuleElems, Named(TEXT("tapered_capsule"))); break;
	case EFG_SingleConvexHull:
	case EFG_MultiConvexHull: NameElements(Geometry.ConvexElems, Named(TEXT("convex"))); break;
	case EFG_LevelSet: NameElements(Geometry.LevelSetElems, Named(TEXT("level_set"))); break;
	case EFG_SkinnedLevelSet: NameElements(Geometry.SkinnedLevelSetElems, Named(TEXT("skinned_level_set"))); break;
	case EFG_MLLevelSet: NameElements(Geometry.MLLevelSetElems, Named(TEXT("ml_level_set"))); break;
	case EFG_SkinnedTriangleMesh: NameElements(Geometry.SkinnedTriangleMeshElems, Named(TEXT("skinned_triangle_mesh"))); break;
	default: break;
	}
}

void ConnectIfValid(UPhysicsAsset& Asset, int32 JointIndex, int32 ChildBody, int32 ParentBody)
{
	if (Asset.ConstraintSetup.IsValidIndex(JointIndex) && Asset.ConstraintSetup[JointIndex] != nullptr)
	{
		ConnectJoint(Asset, *Asset.ConstraintSetup[JointIndex], ChildBody, ParentBody);
	}
}

// The joint that is to tie a physical body below a new body to it. A body without a joint (looked up by its bone name)
// gets a new one when joints are wanted. A joint that hangs from a body above the new bone is re-hung; when that body
// was the one controlling the new bone, the child and it may collide again, since the new body now sits between
// them. A joint within the new bone's subtree, or one hanging from a bone the mesh lacks, is left as it is.
int32 JointToRehang(UPhysicsAsset& Asset, const FReferenceSkeleton& Skeleton, int32 NewBone, int32 FormerController, int32 ChildBody,
	bool bCreateJoints)
{
	const FName ChildBone = Asset.SkeletalBodySetups[ChildBody]->BoneName;
	const int32 Existing = Asset.FindConstraintIndex(ChildBone);
	if (Existing == INDEX_NONE)
	{
		return bCreateJoints ? FPhysicsAssetUtils::CreateNewConstraint(&Asset, ChildBone) : INDEX_NONE;
	}
	const FName HangsFrom = Asset.ConstraintSetup[Existing]->DefaultInstance.ConstraintBone2;
	const int32 HangsFromBone = Skeleton.FindBoneIndex(HangsFrom);
	if (HangsFromBone == INDEX_NONE || Skeleton.BoneIsChildOf(HangsFromBone, NewBone))
	{
		return INDEX_NONE;
	}
	const int32 HangsFromBody = Asset.FindBodyIndex(HangsFrom);
	if (HangsFromBody != INDEX_NONE && HangsFromBody == FormerController)
	{
		SetPairCollision(Asset, ChildBody, HangsFromBody, true);
	}
	return Existing;
}

// Before a body goes: each nearest physical body below it whose joint hangs from it gets a copy of that joint, hanging
// from the nearest physical body above instead.
void RehangBodiesBelow(UPhysicsAsset& Asset, USkeletalMesh& Mesh, int32 BodyIndex)
{
	USkeletalBodySetup* Removed = Asset.SkeletalBodySetups[BodyIndex];
	Removed->Modify();
	TArray<int32> Joints;
	Asset.BodyFindConstraints(BodyIndex, Joints);
	for (const int32 JointIndex : Joints)
	{
		Asset.ConstraintSetup[JointIndex]->Modify();
	}
	const int32 Bone = Mesh.GetRefSkeleton().FindBoneIndex(Removed->BoneName);
	const int32 Above = Bone != INDEX_NONE ? Asset.FindParentBodyIndex(&Mesh, Bone) : INDEX_NONE;
	if (Above == INDEX_NONE || Asset.SkeletalBodySetups[Above] == nullptr)
	{
		return;
	}
	TArray<int32> Below;
	Asset.GetNearestBodyIndicesBelow(Below, Removed->BoneName, &Mesh);
	for (const int32 JointIndex : Joints)
	{
		UPhysicsConstraintTemplate* Joint = Asset.ConstraintSetup[JointIndex];
		for (const int32 ChildBody : Below)
		{
			const FName ChildBone = Asset.SkeletalBodySetups[ChildBody]->BoneName;
			if (Joint->DefaultInstance.ConstraintBone1 == ChildBone)
			{
				ConnectIfValid(Asset, FPhysicsAssetUtils::CreateNewConstraint(&Asset, ChildBone, Joint), ChildBody, Above);
			}
		}
	}
}
} // namespace

void SetPairCollision(UPhysicsAsset& Asset, int32 BodyA, int32 BodyB, bool bEnable)
{
	Asset.Modify();
	if (BodyA == INDEX_NONE || BodyB == INDEX_NONE || BodyA == BodyB)
	{
		return;
	}
	if (bEnable)
	{
		Asset.EnableCollision(BodyA, BodyB);
	}
	else
	{
		Asset.DisableCollision(BodyA, BodyB);
	}
}

void ConnectJoint(UPhysicsAsset& Asset, UPhysicsConstraintTemplate& Joint, int32 ChildBody, int32 ParentBody)
{
	Joint.Modify(false);
	const USkeletalBodySetup* Child = Asset.SkeletalBodySetups[ChildBody];
	const USkeletalBodySetup* Parent = Asset.SkeletalBodySetups[ParentBody];
	if (Child == nullptr || Parent == nullptr)
	{
		return;
	}
	FConstraintInstance& Instance = Joint.DefaultInstance;
	Instance.ConstraintBone1 = Child->BoneName;
	Instance.ConstraintBone2 = Parent->BoneName;
	// Both frames at the child bone's origin; a joint naming a bone the mesh lacks keeps its frames.
	if (CanSnap(Instance, Asset))
	{
		Instance.SnapTransformsToDefault(EConstraintTransformComponentFlags::All, &Asset);
	}
	Joint.SetDefaultProfile(Instance);
	SetPairCollision(Asset, ChildBody, ParentBody, false);
}

int32 AddJoint(UPhysicsAsset& Asset, FName JointName, int32 ChildBody, int32 ParentBody)
{
	if (!FPhysicsAssetUtils::CanCreateConstraints())
	{
		return INDEX_NONE;
	}
	const int32 Index = FPhysicsAssetUtils::CreateNewConstraint(&Asset, JointName);
	if (!Asset.ConstraintSetup.IsValidIndex(Index) || Asset.ConstraintSetup[Index] == nullptr)
	{
		return INDEX_NONE;
	}
	ConnectJoint(Asset, *Asset.ConstraintSetup[Index], ChildBody, ParentBody);
	return Index;
}

int32 AddBody(UPhysicsAsset& Asset, USkeletalMesh& Mesh, FName Bone, const FPhysAssetCreateParams& Params, const FBoneVertInfo* FitInfo,
	TConstArrayView<int32> BoneIndicesInCreation)
{
	const FReferenceSkeleton& Skeleton = Mesh.GetRefSkeleton();
	const int32 BoneIndex = Skeleton.FindBoneIndex(Bone);
	if (BoneIndex == INDEX_NONE)
	{
		return INDEX_NONE;
	}
	Asset.Modify();
	if (const int32 Previous = Asset.FindBodyIndex(Bone); Previous != INDEX_NONE)
	{
		RemoveBody(Asset, Previous);
	}
	const int32 FormerController = Asset.FindControllingBodyIndex(&Mesh, BoneIndex);

	const int32 BodyIndex = FPhysicsAssetUtils::CreateNewBody(&Asset, Bone, Params);
	USkeletalBodySetup* Body = Asset.SkeletalBodySetups[BodyIndex];
	Body->Modify();
	if (FitInfo != nullptr)
	{
		// Shapes fitted to the vertices the bone's infos hold (the fit's vertex weighting).
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
		const bool bFitted = FPhysicsAssetUtils::CreateCollisionFromBone(Body, &Mesh, BoneIndex, Params, *FitInfo, &Asset, BoneIndicesInCreation);
#else
		(void)BoneIndicesInCreation;
		const bool bFitted = FPhysicsAssetUtils::CreateCollisionFromBone(Body, &Mesh, BoneIndex, Params, *FitInfo);
#endif
		if (!bFitted)
		{
			FPhysicsAssetUtils::DestroyBody(&Asset, BodyIndex);
			return INDEX_NONE;
		}
		NameFittedShapes(*Body, Params.GeomType);
	}

	const bool bCreateJoints = Params.bCreateConstraints && FPhysicsAssetUtils::CanCreateConstraints();
	for (int32 Descendant = BoneIndex + 1; Descendant < Skeleton.GetRawBoneNum(); ++Descendant)
	{
		if (!Skeleton.BoneIsChildOf(Descendant, BoneIndex))
		{
			continue;
		}
		const int32 ChildBody = Asset.FindBodyIndex(Skeleton.GetBoneName(Descendant));
		if (ChildBody != INDEX_NONE)
		{
			ConnectIfValid(Asset, JointToRehang(Asset, Skeleton, BoneIndex, FormerController, ChildBody, bCreateJoints), ChildBody, BodyIndex);
		}
	}
	if (FormerController != INDEX_NONE && bCreateJoints)
	{
		ConnectIfValid(Asset, FPhysicsAssetUtils::CreateNewConstraint(&Asset, Bone), BodyIndex, FormerController);
	}
	Asset.RefreshPhysicsAssetChange();
	return BodyIndex;
}

void RemoveBody(UPhysicsAsset& Asset, int32 BodyIndex)
{
	if (!Asset.SkeletalBodySetups.IsValidIndex(BodyIndex))
	{
		return;
	}
	Asset.Modify();
	// Joints are re-hung only with a preview mesh to read the bone hierarchy from; without one the body still goes.
	if (USkeletalMesh* Mesh = Asset.GetPreviewMesh())
	{
		RehangBodiesBelow(Asset, *Mesh, BodyIndex);
	}
	// Also destroys the body's joints and fixes the collision table.
	FPhysicsAssetUtils::DestroyBody(&Asset, BodyIndex);
}

void Refresh(UPhysicsAsset& Asset)
{
	Asset.UpdateBodySetupIndexMap();
	Asset.UpdateBoundsBodiesArray();
	Asset.RefreshPhysicsAssetChange();
}
} // namespace UE::AssetDocument::Graph::Physics::EditorEquivalent
