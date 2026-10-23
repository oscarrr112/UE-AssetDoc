// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

using UnrealBuildTool;

public class AssetDocumentPython : ModuleRules
{
	public AssetDocumentPython(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core", "AssetDocument"
		});
		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"CoreUObject", "Engine", "PythonScriptPlugin", "Python3"
		});
	}
}
