// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// Rollback of Physics Assets (design §5.3). The Core package snapshot restores the authored state (the asset, its body
// and constraint subobjects and the custom-serialized collision table) and runs PostEditUndo (index map, bounds
// bodies). What remains is what load and the editor's Undo do for derived state:
//   * per body: InvalidatePhysicsData + CreatePhysicsMeshes when it has convex shapes (UBodySetup::PostEditUndo);
//   * per constraint: SetDefaultProfile(DefaultInstance) (UPhysicsConstraintTemplate::Serialize on load; the
//     snapshot's save wrote DefaultProfile into ProfileInstance);
//   * the asset: UpdateBodySetupIndexMap, UpdateBoundsBodiesArray, RefreshPhysicsAssetChange (live components).

#include "AssetDocumentPhysicsPrimitives.h"

#include "PhysicsEngine/BodySetup.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/PhysicsConstraintTemplate.h"
#include "PhysicsEngine/SkeletalBodySetup.h"
#include "Runtime/AssetDocumentPackageSnapshot.h"

namespace UE::AssetDocument::Graph::Physics
{
namespace
{
class FPhysicsRebuilder final : public IAssetDocumentDerivedStateRebuilder
{
public:
	virtual FName GetId() const override { return TEXT("AssetDocumentPhysics.PhysicsAsset"); }
	virtual bool Supports(const UObject& PrimaryAsset) const override { return PrimaryAsset.IsA<UPhysicsAsset>(); }
	virtual FAssetDocumentContractValidationResult Rebuild(UObject& PrimaryAsset) override
	{
		if (UPhysicsAsset* Asset = Cast<UPhysicsAsset>(&PrimaryAsset))
		{
			for (USkeletalBodySetup* Body : Asset->SkeletalBodySetups)
			{
				if (Body != nullptr && Body->AggGeom.ConvexElems.Num() > 0)
				{
					Body->InvalidatePhysicsData();
					Body->CreatePhysicsMeshes();
				}
			}
			for (UPhysicsConstraintTemplate* Constraint : Asset->ConstraintSetup)
			{
				if (Constraint != nullptr)
				{
					Constraint->SetDefaultProfile(Constraint->DefaultInstance);
				}
			}
			EditorEquivalent::Refresh(*Asset);
		}
		FAssetDocumentContractValidationResult Result;
		Result.bValid = true;
		return Result;
	}
};
} // namespace

TSharedRef<IAssetDocumentDerivedStateRebuilder> MakePhysicsRebuilder()
{
	return MakeShared<FPhysicsRebuilder>();
}
} // namespace UE::AssetDocument::Graph::Physics
