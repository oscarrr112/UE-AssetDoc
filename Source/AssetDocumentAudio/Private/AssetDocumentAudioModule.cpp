// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

#include "AssetDocumentCompileStatus.h"
#include "AssetDocumentRuntimeModule.h"
#include "AssetDocumentTypeListers.h"
#include "MetaSound/AssetDocumentMetaSound.h"
#include "Modules/ModuleManager.h"
#include "Runtime/AssetDocumentOpenEditor.h"
#include "Runtime/AssetDocumentPackageSnapshot.h"
#include "SoundCue/AssetDocumentSoundCue.h"

// The "audio" provider is assembled by the runtime bootstrap. The module registers the MetaSound compile
// status describer (validation without registration), the list_types kinds metasound_node,
// metasound_datatype and metasound_interface, the MetaSound rollback rebuilder (re-registration), the MetaSound
// factories' preset-parent context and the MetaSound staged trial classes (Validate's trial target is a package asset); for
// SoundCues the graph schema / host adapters of the shared graph primitives, the compile status describer
// (validation), the list_types kind sound_node, the rollback rebuilder (aggregate values, graph refresh) and the
// open-editor handler (refresh of an open Sound Cue editor after an apply).
class FAssetDocumentAudioModule final : public IAssetDocumentRuntimeModule
{
public:
	virtual void StartupModule() override
	{
		using namespace UE::AssetDocument::Graph::Audio::MetaSound;
		RegisterAssetDocumentCompileStatusDescriber(CompileDescriberId, &DescribeCompileStatus);
		RegisterAssetDocumentTypeLister(TEXT("metasound_node"), &ListNodeTypes);
		RegisterAssetDocumentTypeLister(TEXT("metasound_datatype"), &ListDataTypes);
		RegisterAssetDocumentTypeLister(TEXT("metasound_interface"), &ListInterfaces);
		Rebuilder = MakeMetaSoundRebuilder();
		RegisterAssetDocumentDerivedStateRebuilder(Rebuilder.ToSharedRef());
		RegisterStagedTrial();
		RegisterFactoryContext();

		// The Sound Cue editor's module (its schema, graph node classes and the cue's graph editor hooks).
		FModuleManager::Get().LoadModuleChecked(TEXT("AudioEditor"));
		namespace SoundCue = UE::AssetDocument::Graph::Audio::SoundCue;
		SoundCue::RegisterGraphAdapters();
		RegisterAssetDocumentCompileStatusDescriber(SoundCueDescriberId, &SoundCue::DescribeCompileStatus);
		RegisterAssetDocumentTypeLister(TEXT("sound_node"), &SoundCue::ListNodeTypes);
		SoundCueRebuilder = SoundCue::MakeRebuilder();
		RegisterAssetDocumentDerivedStateRebuilder(SoundCueRebuilder.ToSharedRef());
		SoundCueOpenEditor = SoundCue::MakeOpenEditorHandler();
		RegisterAssetDocumentOpenEditorHandler(SoundCueOpenEditor.ToSharedRef());
	}

	virtual void ShutdownModule() override
	{
		if (SoundCueOpenEditor.IsValid())
		{
			UnregisterAssetDocumentOpenEditorHandler(SoundCueOpenEditor->GetId());
			SoundCueOpenEditor.Reset();
		}
		if (SoundCueRebuilder.IsValid())
		{
			UnregisterAssetDocumentDerivedStateRebuilder(SoundCueRebuilder->GetId());
			SoundCueRebuilder.Reset();
		}
		UnregisterAssetDocumentTypeLister(TEXT("sound_node"));
		UnregisterAssetDocumentCompileStatusDescriber(SoundCueDescriberId);
		UE::AssetDocument::Graph::Audio::SoundCue::UnregisterGraphAdapters();
		UE::AssetDocument::Graph::Audio::MetaSound::UnregisterFactoryContext();
		UE::AssetDocument::Graph::Audio::MetaSound::UnregisterStagedTrial();
		if (Rebuilder.IsValid())
		{
			UnregisterAssetDocumentDerivedStateRebuilder(Rebuilder->GetId());
			Rebuilder.Reset();
		}
		UnregisterAssetDocumentTypeLister(TEXT("metasound_interface"));
		UnregisterAssetDocumentTypeLister(TEXT("metasound_datatype"));
		UnregisterAssetDocumentTypeLister(TEXT("metasound_node"));
		UnregisterAssetDocumentCompileStatusDescriber(CompileDescriberId);
	}

	virtual FName GetRuntimeModuleName() const override { return TEXT("AssetDocumentAudio"); }

private:
	TSharedPtr<IAssetDocumentDerivedStateRebuilder> Rebuilder;
	TSharedPtr<IAssetDocumentDerivedStateRebuilder> SoundCueRebuilder;
	TSharedPtr<IAssetDocumentOpenEditorHandler> SoundCueOpenEditor;
	static inline const FName CompileDescriberId = TEXT("AssetDocumentAudio.MetaSound");
	static inline const FName SoundCueDescriberId = TEXT("AssetDocumentAudio.SoundCue");
};

IMPLEMENT_MODULE(FAssetDocumentAudioModule, AssetDocumentAudio)
