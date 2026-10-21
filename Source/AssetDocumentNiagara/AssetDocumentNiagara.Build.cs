// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

using UnrealBuildTool;

// Niagara provider ("fx" primitives, owner FX): reads Niagara systems, emitters, scripts and
// parameter collections the way the Niagara editors show them. Systems and emitters are read
// through the editor's own stack view models (FNiagaraSystemViewModel built headlessly with
// bIsForDataProcessingOnly, as the engine's merge and upgrade paths do), so module inputs, value
// modes and "overridden" match what the stack shows. Uses the graph module's table-driven
// provider plumbing and the Editor typed value codec for reflected values.
public class AssetDocumentNiagara : ModuleRules
{
	public AssetDocumentNiagara(ReadOnlyTargetRules Target) : base(Target)
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
			"AssetDocumentServer",
			"AssetDocumentEditor",
			"AssetDocumentGraph",
			"UnrealEd",
			"Json",
			"JsonUtilities",
			"AssetRegistry",
			"AssetTools",
			"Slate",
			"SlateCore",
			// The Niagara runtime types, the editor view models (stack, system / emitter handle view
			// models), graphs and script sources.
			"Niagara",
			"NiagaraCore",
			"NiagaraEditor",
			"NiagaraShader",
			// The user parameter hierarchy (categories) of the User Parameters panel.
			"DataHierarchyEditor"
		});
	}
}
