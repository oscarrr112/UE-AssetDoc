// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

using UnrealBuildTool;

public class AssetDocumentAI : ModuleRules
{
	public AssetDocumentAI(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PublicDependencyModuleNames.AddRange(new string[] { "Core", "AssetDocument" });
		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"CoreUObject", "Engine", "AssetDocumentEditor",
			"AIModule", "AIGraph", "BehaviorTreeEditor", "PropertyEditor", "SlateCore", "UnrealEd",
			// StateTree primitives (ai.state_tree.*): editor data, compiler, property bindings, parameter
			// bags; the graph module's table-driven provider plumbing and pin type DTOs.
			"AssetDocumentGraph", "StateTreeModule", "StateTreeEditorModule", "PropertyBindingUtils",
			"GameplayTags", "StructUtilsEditor", "BlueprintGraph", "Json", "AssetRegistry",
			// EnvQuery automation tests: the Content Browser's factory / duplicate.
			"AssetTools",
			// Open-editor apply: the open editors' graph panels.
			"Slate", "GraphEditor"
		});
	}
}
