// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

using UnrealBuildTool;

public class AssetDocumentEditor : ModuleRules
{
	public AssetDocumentEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"AssetDocument"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			// Open-editor apply behaviour tests (*OpenEditorTests.cpp) read the open editors.
			"GraphEditor",
			"CoreUObject",
			"Engine",
			"Json",
			"JsonUtilities",
			"UnrealEd",
			"AssetTools",
			"AssetRegistry",
			"AssetDocumentServer",
			// The generic profile's Enhanced Input checks (Input Action / Mapping Context warnings).
			"EnhancedInput",
			"PropertyEditor",
			// FKey values are checked against the registered keys (EKeys), as the key picker lists them.
			"InputCore",
			"Slate",
			"SlateCore"
		});
	}
}
