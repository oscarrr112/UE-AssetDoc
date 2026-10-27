// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

using UnrealBuildTool;

public class AssetDocumentIntegration : ModuleRules
{
	public AssetDocumentIntegration(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PublicDependencyModuleNames.AddRange(new string[] { "Core" });
		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"CoreUObject", "Engine", "UnrealEd", "Json", "AssetDocument", "AssetDocumentPython", "AssetDocumentServer",
			"AssetDocumentEditor", "AssetDocumentAI", "AssetDocumentAnimation", "AssetDocumentGraph", "AssetDocumentWidget", "AssetDocumentData",
			"AssetDocumentNiagara",
			"AssetDocumentAudio",
			"AssetDocumentRig",
			"AssetDocumentPhysics",
			"AssetDocumentSequencer"
		});
	}
}
