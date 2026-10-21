// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

using System.IO;
using UnrealBuildTool;

// Automation tests that need test-only reflected types (fixture classes, structs, enums, row structs),
// together with those types. The plugin lists this module with LoadingPhase "None", so a normal editor
// session never loads it and its types never appear in class / struct pickers, Blueprint actions or
// list_types. The automation framework loads it before it lists the tests (Config/Engine.ini:
// AutomationTestSettings EditorTestModules).
//
// The tests keep the folder layout of the module they test (Private/<Module>/...): each tested module's
// Private folder, and the matching folders here, are include paths, so their includes are unchanged.
public class AssetDocumentTests : ModuleRules
{
	public AssetDocumentTests(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		string[] TestedModules =
		{
			"AssetDocument",
			"AssetDocumentAI",
			"AssetDocumentAnimation",
			"AssetDocumentData",
			"AssetDocumentEditor",
			"AssetDocumentGraph",
			"AssetDocumentLevel",
			"AssetDocumentServer",
			"AssetDocumentUMG"
		};
		string SourceDirectory = Path.GetDirectoryName(ModuleDirectory);
		foreach (string TestedModule in TestedModules)
		{
			foreach (string Root in new[]
			{
				Path.Combine(ModuleDirectory, "Private", TestedModule),
				Path.Combine(SourceDirectory, TestedModule, "Private")
			})
			{
				foreach (string Sub in new[] { "", "Tests", Path.Combine("Tests", "Fixtures") })
				{
					string IncludePath = Path.Combine(Root, Sub);
					if (Directory.Exists(IncludePath))
					{
						PrivateIncludePaths.Add(IncludePath);
					}
				}
			}
		}

		PrivateDependencyModuleNames.AddRange(TestedModules);
		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"AIGraph",
			"AIModule",
			"AnimGraph",
			"AnimationBlueprintEditor",
			"AnimationBlueprintLibrary",
			"AnimationEditor",
			"ApplicationCore",
			"AssetRegistry",
			"AssetTools",
			"AudioExtensions",
			"AudioMixerCore",
			"BehaviorTreeEditor",
			"BlueprintGraph",
			"Blutility",
			"ContentBrowser",
			"CurveEditor",
			"DataLayerEditor",
			"DeveloperSettings",
			"DirectoryWatcher",
			"EditorFramework",
			"EditorSubsystem",
			"EngineSettings",
			"EnhancedInput",
			"GameplayTags",
			"GraphEditor",
			"HTTP",
			"HTTPServer",
			"ImageCore",
			"ImageWrapper",
			"InputBlueprintNodes",
			"InputCore",
			"InterchangeCore",
			"InterchangeEngine",
			"Json",
			"JsonUtilities",
			"Kismet",
			"LevelEditor",
			"LevelSequence",
			"MaterialEditor",
			"MovieScene",
			"MovieSceneTracks",
			"Persona",
			"Projects",
			"PropertyBindingUtils",
			"PropertyEditor",
			"PythonScriptPlugin",
			"RenderCore",
			"RHI",
			"Settings",
			"SettingsEditor",
			"SharedSettingsWidgets",
			"SignalProcessing",
			"SkeletonEditor",
			"Slate",
			"SlateCore",
			"Sockets",
			"SourceControl",
			"StateTreeEditorModule",
			"StateTreeModule",
			"StructUtilsEditor",
			"SubobjectDataInterface",
			"SubobjectEditor",
			"TypedElementFramework",
			"TypedElementRuntime",
			"UMG",
			"UMGEditor",
			"UnrealEd"
		});
	}
}
