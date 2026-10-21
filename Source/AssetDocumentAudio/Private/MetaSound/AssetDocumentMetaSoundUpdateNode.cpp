// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// The MetaSound editor's Update Node (MetasoundEditor.cpp FEditor::UpdateSelectedNodeClasses), the one edit the 5.7
// editor still makes through the frontend controller API (design §4.1, §7.1: isolated here, behaviour test
// UEAssetDoc.MetaSound.UpdateNode). The document builder has no equivalent: ReplaceWithVersion swaps
// the node for the given registered class version under the same node ID, moving its style, configuration and class
// interface override over and reconnecting edges and literals by vertex name and type.

#include "AssetDocumentMetaSound.h"

#include "MetasoundAssetBase.h"
#include "MetasoundAssetManager.h"
#include "MetasoundDocumentInterface.h"
#include "MetasoundFrontendController.h"

namespace UE::AssetDocument::Graph::Audio::MetaSound
{
bool ReplaceNodeWithVersion(UObject& Asset, const FGuid& NodeID, const FMetasoundFrontendVersionNumber& Version, FString& OutError)
{
	using namespace Metasound::Frontend;
	IMetaSoundAssetManager* Manager = IMetaSoundAssetManager::Get();
	FMetasoundAssetBase* MetaSound = Manager != nullptr ? Manager->GetAsAsset(Asset) : nullptr;
	const IMetaSoundDocumentInterface* Document = AsMetaSound(Asset);
	if (MetaSound == nullptr || Document == nullptr)
	{
		OutError = TEXT("not a MetaSound asset");
		return false;
	}
	PRAGMA_DISABLE_DEPRECATION_WARNINGS
	FNodeHandle NodeHandle = MetaSound->GetRootGraphHandle()->GetNodeWithID(NodeID);
	if (!NodeHandle->IsValid())
	{
		OutError = TEXT("the node is not on the graph");
		return false;
	}
	// The editor ignores the vertices that could not be reconnected ("the user is actively forcing an update").
	FNodeHandle Replacement = NodeHandle->ReplaceWithVersion(Version, nullptr, nullptr);
	if (!Replacement->IsValid() || Replacement->GetClassMetadata().GetVersion() != Version)
	{
		OutError = FString::Printf(TEXT("version %s is not registered"), *Version.ToString());
		return false;
	}
	FDocumentHandle DocumentHandle = MetaSound->GetDocumentHandle();
	DocumentHandle->RemoveUnreferencedDependencies();
	DocumentHandle->SynchronizeDependencyMetadata();
	PRAGMA_ENABLE_DEPRECATION_WARNINGS
	MetaSound->GetModifyContext().SetDocumentModified();
	// The controller wrote the document directly: the asset's builder caches are rebuilt as the editor's PostEditUndo
	// rebuilds them.
	IDocumentBuilderRegistry::GetChecked().ReloadBuilder(Document->GetConstDocument().RootGraph.Metadata.GetClassName());
	return true;
}
} // namespace UE::AssetDocument::Graph::Audio::MetaSound
