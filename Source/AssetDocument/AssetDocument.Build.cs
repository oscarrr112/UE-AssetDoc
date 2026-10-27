// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

using UnrealBuildTool;

public class AssetDocument : ModuleRules
{
	public AssetDocument(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"Json",
			"JsonUtilities",
			"AssetDocumentServer"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"UnrealEd",
			// An open editor's toolbar commands (OpenEditor::CanExecuteEditorCommand).
			"Slate",
			"SlateCore",
			// Material compile status (the editor's shader platform, GMaxRHIShaderPlatform).
			"RHI",
			"Kismet",
			"AssetRegistry",
			"AssetTools",
			"AnimGraph",
			"AIGraph",
			"AIModule",
			"BehaviorTreeEditor",
			"BlueprintGraph",
			"DirectoryWatcher",
			"GameplayTags",
			"Projects",
			"UMG",
			"UMGEditor",
			"MovieScene",
			"MovieSceneTracks"
		});
	}
}
