// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

// Validate's trial target of a MetaSound (AssetDocumentEditorTrialScope.h, staged trial classes). The MetaSound
// document builder (FDocumentBuilderRegistry::FindOrBeginBuilding) asserts that the MetaSound is a package asset, and
// so does the duplication of one (FAssetHelper::PostDuplicate), so the trial target is a staged asset: made by the
// editor's factory (Create) or the editor's Duplicate (Update: a class name of its own, so registering it leaves the
// original's frontend registration alone). When the trial ends, what the MetaSound registered outside its package is
// dropped as the engine drops it for a destroyed MetaSound (FMetasoundAssetBase::OnNotifyBeginDestroy) after its
// builder is finished (FDocumentBuilderRegistry::FinishBuilding, as the duplication does for the copied class name).

#include "AssetDocumentEditorTrialScope.h"
#include "MetaSound/AssetDocumentMetaSound.h"

#include "Metasound.h"
#include "MetasoundAssetBase.h"
#include "MetasoundAssetManager.h"
#include "MetasoundDocumentBuilderRegistry.h"
#include "MetasoundDocumentInterface.h"
#include "MetasoundGlobals.h"
#include "MetasoundSource.h"

namespace AssetDocumentMetaSoundTrialLocal
{
void DiscardStagedMetaSound(UObject& Asset)
{
	using namespace Metasound;
	using namespace Metasound::Frontend;
	const IMetaSoundDocumentInterface* Document = Cast<IMetaSoundDocumentInterface>(&Asset);
	if (Document == nullptr)
	{
		return;
	}
	const FMetasoundFrontendClassName ClassName = Document->GetConstDocument().RootGraph.Metadata.GetClassName();
	if (ClassName.IsValid())
	{
		constexpr bool bForceUnregisterNodeClass = true;
		Engine::FDocumentBuilderRegistry::GetChecked().FinishBuilding(ClassName, FTopLevelAssetPath(&Asset), bForceUnregisterNodeClass);
	}
	IMetaSoundAssetManager* Manager = IMetaSoundAssetManager::Get();
	FMetasoundAssetBase* MetaSound = Manager != nullptr ? Manager->GetAsAsset(Asset) : nullptr;
	if (MetaSound != nullptr && CanEverExecuteGraph())
	{
		MetaSound->UnregisterGraphWithFrontend();
	}
	if (Manager != nullptr)
	{
		Manager->RemoveAsset(Asset);
	}
}
} // namespace AssetDocumentMetaSoundTrialLocal

namespace UE::AssetDocument::Graph::Audio::MetaSound
{
void RegisterStagedTrial()
{
	using namespace AssetDocumentMetaSoundTrialLocal;
	FAssetDocumentEditorTrialScope::RegisterStagedTrialClass(UMetaSoundSource::StaticClass()->GetClassPathName(), &DiscardStagedMetaSound);
	FAssetDocumentEditorTrialScope::RegisterStagedTrialClass(UMetaSoundPatch::StaticClass()->GetClassPathName(), &DiscardStagedMetaSound);
}

void UnregisterStagedTrial()
{
	FAssetDocumentEditorTrialScope::UnregisterStagedTrialClass(FTopLevelAssetPath(TEXT("/Script/MetasoundEngine"), TEXT("MetaSoundSource")));
	FAssetDocumentEditorTrialScope::UnregisterStagedTrialClass(FTopLevelAssetPath(TEXT("/Script/MetasoundEngine"), TEXT("MetaSoundPatch")));
}
} // namespace UE::AssetDocument::Graph::Audio::MetaSound
