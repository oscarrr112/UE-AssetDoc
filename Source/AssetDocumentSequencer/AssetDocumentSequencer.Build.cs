// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

using UnrealBuildTool;

// Sequencer provider ("seq" primitives, owner Cinematics): the Level Sequence document
// (docs/architecture/asset-document-sequencer-design.md). Phases S1 (read) and S2 (core authoring): the movie scene,
// binding references, tracks, sections and channels are read and written through the exported MovieScene /
// MovieSceneTracks / LevelSequence API, reflection and the Editor typed value codec. The invocation plumbing is the graph
// module's table provider.
public class AssetDocumentSequencer : ModuleRules
{
	public AssetDocumentSequencer(ReadOnlyTargetRules Target) : base(Target)
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
			// Tests: the shared open-editor test support (AssetDocumentOpenEditorTestSupport.h, the editor's Save).
			"GraphEditor",
			"PropertyEditor",
			"Slate",
			"AssetDocumentGraph",
			"UnrealEd",
			"Json",
			"JsonUtilities",
			"AssetRegistry",
			"MovieScene",
			"MovieSceneTracks",
			"LevelSequence",
			// Template / Camera Animation Sequences (profile.template_sequence.v1, design §7.2).
			"TemplateSequence",
			"UniversalObjectLocator",
			// Locator payloads written percent-encoded are read back decoded (FGenericPlatformHttp::UrlDecode).
			"HTTP",
			// Director endpoints (function entries, custom events) and the Sequencer's animatable-property rule.
			"BlueprintGraph",
			"Sequencer",
			"CinematicCamera",
			// Tests: corpus entries skip when their plugin is off; corpus maps are opened for locator resolution; the
			// free-edit scenarios duplicate sequences as the Content Browser does.
			"Projects",
			"AssetTools",
			// The Director Blueprint (FMovieSceneSequenceEditor, FMovieSceneEventUtils, FMovieSceneDynamicBindingUtils); tests: a
			// Blueprint "Get Sequence Binding" node as a dependent.
			"MovieSceneTools",
			// Tests: a Camera Animation Sequence played as a camera shake (UDefaultCameraShakeBase).
			"EngineCameras",
			// Tests: the open Sequencer's outliner tree (its view models).
			"SequencerCore"
		});
	}
}
