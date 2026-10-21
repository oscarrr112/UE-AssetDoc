// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

using UnrealBuildTool;

public class AssetDocumentServer : ModuleRules
{
	public AssetDocumentServer(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"Json",
			"JsonUtilities"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"UnrealEd",
			"Slate",
			"SlateCore",
			// Viewport screenshot
			"RenderCore",
			"ImageWrapper",
			// HTTP Server
			"HTTPServer",
			"Sockets", // HTTP bind failure test (a socket that holds the port)
			"HTTP", // HTTP route test (requests to the running server)
			// Settings
			"DeveloperSettings",
			// MCP endpoint: the plugin's tool manifest (MCP/tools.json)
			"Projects",
			// Property utilities (Utils/): GameplayTag, StateTreeReference and widget values
			"GameplayTags",
			"StateTreeModule",
			"UMG",
			// Editor state & Python execution
			"ContentBrowser",
			"PythonScriptPlugin"
		});

		// GameplayAbilities is optional — only link when the plugin is enabled
		if (Target.bBuildDeveloperTools || DoesModuleExist("GameplayAbilities"))
		{
			PrivateDependencyModuleNames.Add("GameplayAbilities");
			PrivateDefinitions.Add("WITH_GAMEPLAY_ABILITIES=1");
		}
	}

	private bool DoesModuleExist(string ModuleName)
	{
		try
		{
			// If the module can be resolved, it exists
			return !string.IsNullOrEmpty(GetModuleDirectory(ModuleName));
		}
		catch
		{
			return false;
		}
	}
}
