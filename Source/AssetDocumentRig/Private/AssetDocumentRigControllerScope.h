// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"

namespace UE::AssetDocument::Graph::Rig
{
// UIKRigController::GetController / UIKRetargeterController::GetController create a transient controller that the
// asset keeps (its Controller field) and that references the asset. The editor's delete (ObjectTools::DeleteObjects,
// FAssetDeleteModel) counts that controller as an in-memory referencer and refuses to delete the asset, so the
// rollback of a failed Create could not remove the new asset. A controller created only for an AssetDocument call is
// released when the call ends (garbage collection then frees it); a controller that existed before the call (made by
// an editor or a script) is kept.
template <typename AssetType>
class TScopedRigController
{
public:
	explicit TScopedRigController(const AssetType& InAsset)
		: Asset(const_cast<AssetType&>(InAsset))
		, bHadController(InAsset.Controller != nullptr)
	{
	}

	~TScopedRigController()
	{
		if (bHadController || Asset.Controller == nullptr)
		{
			return;
		}
		UObject* Created = Asset.Controller;
		Asset.Controller = nullptr;
		Created->MarkAsGarbage();
	}

	TScopedRigController(const TScopedRigController&) = delete;
	TScopedRigController& operator=(const TScopedRigController&) = delete;

private:
	AssetType& Asset;
	const bool bHadController;
};
} // namespace UE::AssetDocument::Graph::Rig
