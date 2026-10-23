// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// Rollback of IK Rigs and IK Retargeters (design §5.3). The Core package snapshot restores the authored state
// (the asset, its goal subobjects) and runs PostEditUndo; what remains is what the editors do after an Undo: the
// IK Rig controller's BroadcastNeedsReinitialized (re-initializes the solver stack and the goals' initial
// transforms, tells open editors to re-init) and, for a retargeter, a reinitialize scope on its controller plus a
// version bump (running retarget anim nodes re-init when the version changes).
// The IK Rig broadcast runs only when the rig already has a controller: its listeners (the IK Rig and IK Retarget
// editors) bind to that controller, and without them the broadcast would only re-base the goals on their bones, which
// moves goal offsets by a few millionths each time (ResetInitialGoalTransforms through a ref pose rotation that is not
// quite unit length) without restoring anything. With a controller, a goal whose bone transform the broadcast left as
// it was keeps the restored offset exactly (its current transform is put back).

#include "AssetDocumentRigControllerScope.h"
#include "AssetDocumentRigPrimitives.h"

#include "RetargetEditor/IKRetargeterController.h"
#include "Retargeter/IKRetargeter.h"
#include "RigEditor/IKRigController.h"
#include "Rig/IKRigDefinition.h"
#include "Runtime/AssetDocumentPackageSnapshot.h"

namespace UE::AssetDocument::Graph::Rig
{
namespace
{
class FRigRebuilder final : public IAssetDocumentDerivedStateRebuilder
{
public:
	virtual FName GetId() const override { return TEXT("AssetDocumentRig.IKRig"); }
	virtual bool Supports(const UObject& PrimaryAsset) const override
	{
		return PrimaryAsset.IsA<UIKRigDefinition>() || PrimaryAsset.IsA<UIKRetargeter>();
	}
	virtual FAssetDocumentContractValidationResult Rebuild(UObject& PrimaryAsset) override
	{
		if (const UIKRigDefinition* Rig = Cast<UIKRigDefinition>(&PrimaryAsset))
		{
			if (Rig->Controller != nullptr)
			{
				if (const UIKRigController* Controller = UIKRigController::GetController(Rig))
				{
					TArray<TTuple<FName, FTransform, FTransform>> Goals;
					for (const UIKRigEffectorGoal* Goal : Rig->GetGoalArray())
					{
						if (Goal != nullptr)
						{
							Goals.Emplace(Goal->GoalName, Goal->InitialTransform, Goal->CurrentTransform);
						}
					}
					Controller->BroadcastNeedsReinitialized();
					for (const TTuple<FName, FTransform, FTransform>& Restored : Goals)
					{
						const UIKRigEffectorGoal* Goal = Controller->GetGoal(Restored.Get<0>());
						if (Goal != nullptr && Goal->InitialTransform.Equals(Restored.Get<1>(), 0.0))
						{
							Controller->SetGoalCurrentTransform(Restored.Get<0>(), Restored.Get<2>());
						}
					}
				}
			}
		}
		else if (const UIKRetargeter* Retargeter = Cast<UIKRetargeter>(&PrimaryAsset))
		{
			Retargeter->IncrementVersion();
			const TScopedRigController<UIKRetargeter> ControllerScope(*Retargeter);
			if (const UIKRetargeterController* Controller = UIKRetargeterController::GetController(Retargeter))
			{
				const FScopedReinitializeIKRetargeter Reinitialize(Controller, ERetargetRefreshMode::ProcessorAndOpStack);
			}
		}
		FAssetDocumentContractValidationResult Result;
		Result.bValid = true;
		return Result;
	}
};
} // namespace

TSharedRef<IAssetDocumentDerivedStateRebuilder> MakeRigRebuilder()
{
	return MakeShared<FRigRebuilder>();
}
} // namespace UE::AssetDocument::Graph::Rig
