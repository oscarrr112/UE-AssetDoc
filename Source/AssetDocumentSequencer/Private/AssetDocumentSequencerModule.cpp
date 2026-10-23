// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentCompileStatus.h"
#include "AssetDocumentGraphProvider.h"
#include "AssetDocumentRuntimeModule.h"
#include "AssetDocumentSequencerPrimitives.h"
#include "AssetDocumentTypeListers.h"
#include "LevelSequence.h"
#include "Modules/ModuleManager.h"
#include "Runtime/AssetDocumentPackageSnapshot.h"

// The "seq" provider is assembled by the runtime bootstrap. The module registers the Level Sequence status describer
// (the read checks and Sequence Validator rules of design §5.1, the "Compile" of Extract / Inspect) and the list_types
// kinds sequencer_track, sequencer_section and sequencer_binding, the rollback rebuilder of seq.edit.v1
// (AssetDocumentSequencerRebuilder.cpp) and the Level Sequence as the host of its Director Blueprint (the Blueprint
// providers resolve the sequence's handle to the director; the graph module's hosted-Blueprint rebuilder recompiles it
// after a rollback), and the open-editor apply handler (AssetDocumentSequencerOpenEditor.cpp: open Sequencers are
// refreshed after an apply).
class FAssetDocumentSequencerModule final : public IAssetDocumentRuntimeModule
{
public:
	virtual void StartupModule() override
	{
		using namespace UE::AssetDocument::Graph::Sequencer;
		RegisterAssetDocumentCompileStatusDescriber(CompileDescriberId, &DescribeStatus);
		RegisterAssetDocumentTypeLister(TEXT("sequencer_track"), &ListTrackTypes);
		RegisterAssetDocumentTypeLister(TEXT("sequencer_section"), &ListSectionTypes);
		RegisterAssetDocumentTypeLister(TEXT("sequencer_binding"), &ListBindingTypes);
		Rebuilder = MakeRebuilder();
		LevelSequencePath = ULevelSequence::StaticClass()->GetClassPathName();
		RegisterAssetDocumentDerivedStateRebuilder(Rebuilder.ToSharedRef());
		// The Director Blueprint region: the Blueprint providers edit the director through the sequence's handles.
		// By class path: ShutdownModule runs after the UObject exit purge on a normal editor exit.
		RegisterAssetDocumentHostedBlueprint(LevelSequencePath, &ResolveHostedDirector);
		RegisterOpenEditorHandler();
	}

	virtual void ShutdownModule() override
	{
		UE::AssetDocument::Graph::Sequencer::UnregisterOpenEditorHandler();
		UnregisterAssetDocumentHostedBlueprint(LevelSequencePath);
		if (Rebuilder.IsValid())
		{
			UnregisterAssetDocumentDerivedStateRebuilder(Rebuilder->GetId());
			Rebuilder.Reset();
		}
		UnregisterAssetDocumentTypeLister(TEXT("sequencer_binding"));
		UnregisterAssetDocumentTypeLister(TEXT("sequencer_section"));
		UnregisterAssetDocumentTypeLister(TEXT("sequencer_track"));
		UnregisterAssetDocumentCompileStatusDescriber(CompileDescriberId);
	}

	virtual FName GetRuntimeModuleName() const override { return TEXT("AssetDocumentSequencer"); }

private:
	TSharedPtr<IAssetDocumentDerivedStateRebuilder> Rebuilder;
	FTopLevelAssetPath LevelSequencePath;
	static inline const FName CompileDescriberId = TEXT("AssetDocumentSequencer.LevelSequence");
};

IMPLEMENT_MODULE(FAssetDocumentSequencerModule, AssetDocumentSequencer)
