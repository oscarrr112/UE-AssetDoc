// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

using UnrealBuildTool;

public class AssetDocumentGraph : ModuleRules
{
	public AssetDocumentGraph(ReadOnlyTargetRules Target) : base(Target)
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
			// Open-editor apply behaviour tests (*OpenEditorTests.cpp) read the open editors.
			"PropertyEditor", "SubobjectEditor",
			"AssetDocumentEditor",
			"UnrealEd",
			"BlueprintGraph",
			"Kismet",
			"GraphEditor",
			"Slate",
			"SlateCore",
			"AssetRegistry",
			"Json",
			"JsonUtilities"
		});
	}
}
