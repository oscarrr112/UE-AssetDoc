// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

using UnrealBuildTool;

// Structured level/actor tools for the MCP (query_level, edit_level, import_assets,
// settings), C++ only:
// docs/architecture/level-editing-tools-design.md. Not an AssetDocument profile; it
// reuses AssetDocument's result/diagnostic/DTO types and the editor object-property API.
public class AssetDocumentLevel : ModuleRules
{
	public AssetDocumentLevel(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"Json"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"AssetDocument",
			"AssetDocumentEditor",
			"AssetDocumentServer",
			"UnrealEd",
			"LevelEditor",
			"AssetRegistry",
			"AssetTools",
			"EditorFramework",
			"EditorSubsystem",
			"Slate",
			"SlateCore",
			"TypedElementFramework",
			"TypedElementRuntime",
			"SubobjectDataInterface",
			"DataLayerEditor",
			// import_assets: Interchange import (pipelines are edited by reflection; no
			// dependency on the InterchangePipelines plugin module).
			"InterchangeCore",
			"InterchangeEngine",
			// settings: the Project Settings sections and their config write path.
			"Settings",
			"SettingsEditor",
			"SharedSettingsWidgets",
			"SourceControl",
			// Tests: generated image sources; the Maps & Modes settings class.
			"ImageCore",
			"EngineSettings",
			// Tests: a Level Sequence binding as a soft referencer of a deleted actor.
			"LevelSequence",
			"MovieScene",
			// Playtest tools (play, query_play, game-view screenshot).
			"AIModule",
			// edit_level: the navigation actors a placed Nav Mesh Bounds Volume brings, made before the save.
			"NavigationSystem",
			// query_play what "audio": an audio engine subsystem notified of every sound started.
			"AudioExtensions",
			"AudioMixerCore",
			"SignalProcessing",
			"ApplicationCore",
			"EnhancedInput",
			"ImageWrapper",
			"InputCore",
			"RenderCore",
			// Tests: a pawn Blueprint with an Enhanced Input action event.
			"BlueprintGraph",
			"InputBlueprintNodes",
			"Kismet",
			// Tests: an unapplied edit in an open Material Editor (asset_editor close / apply_editor).
			"MaterialEditor"
		});
	}
}
