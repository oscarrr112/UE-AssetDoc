// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

using UnrealBuildTool;

// Audio asset provider ("audio" primitives, owner Audio): the MetaSound Source / Patch documents
// (docs/architecture/asset-document-metasound-design.md) and the SoundCue graph adapters
// (docs/architecture/asset-document-soundcue-design.md). Its own module so that only this module
// depends on the Metasound plugin (never on Harmonix). Uses the graph module's table-driven provider plumbing and the
// Editor typed value codec (node configurations, member widget settings).
public class AssetDocumentAudio : ModuleRules
{
	public AssetDocumentAudio(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"AssetDocument"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"AssetDocumentEditor",
			"AssetDocumentGraph",
			"UnrealEd",
			"Json",
			"JsonUtilities",
			"AssetRegistry",
			"AudioExtensions",
			"MetasoundFrontend",
			"MetasoundEngine",
			"MetasoundGraphCore",
			// Edit / compile: UMetaSoundEditorSubsystem (registration, the editor's default author).
			"MetasoundEditor",
			"EditorSubsystem",
			// Tests: duplicates of real MetaSounds are made as the Content Browser does.
			"AssetTools",
			// SoundCue: the Sound Cue editor's graph classes (public Classes/SoundCueGraph headers), its palette filter
			// (class viewer) and the editor's graph panel (the editor-delete behaviour test).
			"AudioEditor",
			"ClassViewer",
			"GraphEditor",
			"Slate",
			"SlateCore",
			"InputCore"
		});
	}
}
