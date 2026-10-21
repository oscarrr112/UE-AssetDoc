// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// physics.status.v1 and the "Compile" describer of Physics Assets (design §5.1). There is no compile; the status is
// what data validation and cook report (UBodySetup::IsDataValid per body) plus the structural checks under which
// the runtime cannot build the ragdoll:
//   Errors:   IsDataValid (a body without shapes; a simulated body where no shape contributes to mass); a constraint
//             whose Child or Parent has no body; Child == Parent; a profile entry for a profile the asset does not list.
//   Warnings: bodies whose bone the preview mesh lacks (the editor greys them out); a body that is not the root of its
//             chain and has no constraint (it falls off when simulated); a convex shape without a cooked hull;
//             a constraint whose child bone is an ancestor of its parent bone (inverted).
// Status: UpToDate, UpToDateWithWarnings, Error, or Unknown when there is no preview mesh (bone checks skipped).
// Messages carry document paths. Never changes the asset.

#include "AssetDocumentPhysicsPrimitives.h"

#include "AssetDocumentGraphValue.h"
#include "Chaos/Convex.h"
#include "Dom/JsonObject.h"
#include "Engine/SkeletalMesh.h"
#include "Misc/DataValidation.h"
#include "PhysicsEngine/AggregateGeom.h"
#include "PhysicsEngine/ConvexElem.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/PhysicsConstraintTemplate.h"
#include "PhysicsEngine/SkeletalBodySetup.h"
#include "ReferenceSkeleton.h"

namespace UE::AssetDocument::Graph::Physics
{
namespace
{
struct FStatusMessage
{
	FString Severity;
	FString Message;
	FString Path;
	FString Source;
};

FString BodyPath(FName Bone, const TCHAR* Suffix = TEXT(""))
{
	return FString::Printf(TEXT("/Body/Bodies/%s%s"), *Escape(Bone.ToString()), Suffix);
}

void Describe(const UPhysicsAsset& Asset, FString& OutStatus, TArray<FStatusMessage>& OutMessages)
{
	const USkeletalMesh* Mesh = Asset.GetPreviewMesh();
	const FReferenceSkeleton* Skeleton = Mesh != nullptr ? &Mesh->GetRefSkeleton() : nullptr;
	OutStatus = Skeleton != nullptr ? TEXT("") : TEXT("Unknown");
	if (Skeleton == nullptr)
	{
		OutMessages.Add({TEXT("Warning"), TEXT("The physics asset has no preview mesh; bone checks are skipped."), TEXT("/Body/PreviewMesh"),
			TEXT("PreviewMesh")});
	}

	const TArray<FString> Keys = ConstraintKeys(Asset);
	TSet<FName> Constrained;
	for (int32 Index = 0; Index < Asset.ConstraintSetup.Num(); ++Index)
	{
		const UPhysicsConstraintTemplate* Constraint = Asset.ConstraintSetup[Index];
		if (Constraint != nullptr)
		{
			Constrained.Add(Constraint->DefaultInstance.ConstraintBone1);
		}
	}

	for (const int32 Index : BodyOrder(Asset))
	{
		const USkeletalBodySetup& Body = *Asset.SkeletalBodySetups[Index];
		FDataValidationContext Context;
		Body.IsDataValid(Context);
		for (const FDataValidationContext::FIssue& Issue : Context.GetIssues())
		{
			const bool bError = Issue.Severity == EMessageSeverity::Error;
			OutMessages.Add({bError ? TEXT("Error") : TEXT("Warning"), Issue.Message.ToString(), BodyPath(Body.BoneName, TEXT("/Shapes")),
				TEXT("IsDataValid")});
		}
		// Only for a body whose physics meshes were built (CreatePhysicsMeshes; a loaded asset builds them when used).
		for (int32 Convex = 0; Body.bCreatedPhysicsMeshes && Convex < Body.AggGeom.ConvexElems.Num(); ++Convex)
		{
			if (!Body.AggGeom.ConvexElems[Convex].GetChaosConvexMesh().IsValid() && Body.AggGeom.ConvexElems[Convex].VertexData.Num() > 0)
			{
				OutMessages.Add({TEXT("Warning"), FString::Printf(TEXT("Convex %d of body %s has no cooked hull (degenerate or not cooked); it does "
					"not collide."), Convex, *Body.BoneName.ToString()), BodyPath(Body.BoneName, *FString::Printf(TEXT("/Shapes/Convexes/%d"), Convex)),
					TEXT("Cook")});
			}
		}
		if (Skeleton == nullptr)
		{
			continue;
		}
		const int32 Bone = Skeleton->FindBoneIndex(Body.BoneName);
		if (Bone == INDEX_NONE)
		{
			OutMessages.Add({TEXT("Warning"), FString::Printf(TEXT("The preview mesh has no bone %s; the body is not used with this mesh."),
				*Body.BoneName.ToString()), BodyPath(Body.BoneName), TEXT("Skeleton")});
			continue;
		}
		const int32 Parent = Asset.FindParentBodyIndex(Mesh, Bone);
		if (Parent != INDEX_NONE && !Constrained.Contains(Body.BoneName))
		{
			OutMessages.Add({TEXT("Warning"), FString::Printf(TEXT("Body %s has a body above it (%s) but no constraint; it falls off when "
				"simulated."), *Body.BoneName.ToString(), *Asset.SkeletalBodySetups[Parent]->BoneName.ToString()), BodyPath(Body.BoneName),
				TEXT("Constraints")});
		}
	}

	const TArray<FName>& ConstraintProfiles = Asset.GetConstraintProfileNames();
	const TArray<FName>& AnimationProfiles = Asset.GetPhysicalAnimationProfileNames();
	for (int32 Index = 0; Index < Asset.ConstraintSetup.Num(); ++Index)
	{
		const UPhysicsConstraintTemplate* Constraint = Asset.ConstraintSetup[Index];
		if (Constraint == nullptr)
		{
			continue;
		}
		const FConstraintInstance& Instance = Constraint->DefaultInstance;
		const FString Path = FString::Printf(TEXT("/Body/Constraints/%s"), *Escape(Keys[Index]));
		for (const FName Bone : {Instance.ConstraintBone1, Instance.ConstraintBone2})
		{
			if (Asset.FindBodyIndex(Bone) == INDEX_NONE)
			{
				OutMessages.Add({TEXT("Error"), FString::Printf(TEXT("Constraint %s names %s, which has no body."), *Keys[Index], *Bone.ToString()),
					Path + (Bone == Instance.ConstraintBone1 ? TEXT("/Child") : TEXT("/Parent")), TEXT("Constraints")});
			}
		}
		if (Instance.ConstraintBone1 == Instance.ConstraintBone2)
		{
			OutMessages.Add({TEXT("Error"), FString::Printf(TEXT("Constraint %s constrains %s to itself."), *Keys[Index],
				*Instance.ConstraintBone1.ToString()), Path + TEXT("/Parent"), TEXT("Constraints")});
		}
		else if (Skeleton != nullptr)
		{
			const int32 Child = Skeleton->FindBoneIndex(Instance.ConstraintBone1);
			const int32 Parent = Skeleton->FindBoneIndex(Instance.ConstraintBone2);
			if (Child != INDEX_NONE && Parent != INDEX_NONE && Skeleton->BoneIsChildOf(Parent, Child))
			{
				OutMessages.Add({TEXT("Warning"), FString::Printf(TEXT("Constraint %s is inverted: its child %s is above its parent %s in the "
					"skeleton."), *Keys[Index], *Instance.ConstraintBone1.ToString(), *Instance.ConstraintBone2.ToString()), Path, TEXT("Constraints")});
			}
		}
		for (const FPhysicsConstraintProfileHandle& Handle : Constraint->ProfileHandles)
		{
			if (!ConstraintProfiles.Contains(Handle.ProfileName))
			{
				OutMessages.Add({TEXT("Error"), FString::Printf(TEXT("Constraint %s has data for constraint profile %s, which the asset does not "
					"list."), *Keys[Index], *Handle.ProfileName.ToString()), FString::Printf(TEXT("%s/Profiles/%s"), *Path,
					*Escape(Handle.ProfileName.ToString())), TEXT("Profiles")});
			}
		}
	}
	for (const int32 Index : BodyOrder(Asset))
	{
		const USkeletalBodySetup& Body = *Asset.SkeletalBodySetups[Index];
		for (const FPhysicalAnimationProfile& Profile : Body.GetPhysicalAnimationProfiles())
		{
			if (!AnimationProfiles.Contains(Profile.ProfileName))
			{
				OutMessages.Add({TEXT("Error"), FString::Printf(TEXT("Body %s has data for physical animation profile %s, which the asset does not "
					"list."), *Body.BoneName.ToString(), *Profile.ProfileName.ToString()), BodyPath(Body.BoneName,
					*FString::Printf(TEXT("/PhysicalAnimation/%s"), *Escape(Profile.ProfileName.ToString()))), TEXT("Profiles")});
			}
		}
	}
}

FString StatusOf(const FString& Status, const TArray<FStatusMessage>& Messages)
{
	bool bWarning = false;
	for (const FStatusMessage& Message : Messages)
	{
		if (Message.Severity == TEXT("Error"))
		{
			return TEXT("Error");
		}
		bWarning |= Message.Severity == TEXT("Warning");
	}
	if (!Status.IsEmpty())
	{
		return Status;
	}
	return bWarning ? TEXT("UpToDateWithWarnings") : TEXT("UpToDate");
}
} // namespace

bool DescribeStatusValue(const UObject& Asset, FValue& OutStatus)
{
	const UPhysicsAsset* PhysicsAsset = Cast<UPhysicsAsset>(&Asset);
	if (PhysicsAsset == nullptr)
	{
		return false;
	}
	FString Status;
	TArray<FStatusMessage> Messages;
	Describe(*PhysicsAsset, Status, Messages);
	TArray<FValue> Rows;
	for (const FStatusMessage& Message : Messages)
	{
		FObjectBuilder Row;
		Row.AddString(TEXT("Severity"), Message.Severity);
		Row.AddString(TEXT("Message"), Message.Message);
		if (!Message.Path.IsEmpty())
		{
			Row.AddString(TEXT("Path"), Message.Path);
		}
		Row.AddString(TEXT("Source"), Message.Source);
		Rows.Add(Row.Build());
	}
	FObjectBuilder Object;
	Object.AddString(TEXT("Status"), StatusOf(Status, Messages));
	Object.Add(TEXT("Messages"), MakeArray(MoveTemp(Rows)));
	OutStatus = Object.Build();
	return true;
}

TSharedPtr<FJsonObject> DescribeStatus(const UObject& Asset)
{
	FValue Status;
	if (!DescribeStatusValue(Asset, Status))
	{
		return nullptr;
	}
	const TSharedPtr<FJsonValue> Json = ValueToJson(Status);
	return Json.IsValid() ? Json->AsObject() : nullptr;
}

bool HandleStatus(FCall& Call, FObjectBuilder& Result)
{
	if (!Call.Args.OnlyFields({}))
	{
		return false;
	}
	FValue Status;
	if (!DescribeStatusValue(Call.Asset, Status))
	{
		return Fail(Call, TEXT("PhysicsAsset.UnsupportedAsset"), TEXT("physics.status.v1 reads Physics Assets."), TEXT("/HandleArguments/asset"));
	}
	Result.Add(TEXT("compile"), MoveTemp(Status));
	return true;
}
} // namespace UE::AssetDocument::Graph::Physics
