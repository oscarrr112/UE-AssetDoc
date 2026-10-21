// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

using UnrealBuildTool;

// Data table family provider ("data" primitives): rows of UDataTable / UCompositeDataTable /
// UMirrorDataTable, curves of UCurveTable / UCompositeCurveTable, entries of UStringTable and the
// members / enumerators of User Defined Structs / Enums (the row types of data tables) and the
// parent / parameter overrides of UMaterialInstanceConstant and the expression graphs of
// UMaterial / UMaterialFunction.
// Uses the graph module's table-driven provider plumbing (no graphs) and the Editor typed
// value codec for row values.
public class AssetDocumentData : ModuleRules
{
	public AssetDocumentData(ReadOnlyTargetRules Target) : base(Target)
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
			"PropertyEditor", "CurveEditor", "Slate", "SlateCore",
			"AssetDocumentEditor",
			"AssetDocumentGraph",
			"BlueprintGraph",
			"UnrealEd",
			"Json",
			"JsonUtilities",
			// Tests (end to end through the production service; parent materials are built with
			// UMaterialEditingLibrary as the Material Editor does).
			"AssetRegistry",
			"MaterialEditor",
			// FMaterialUpdateContext defaults to GMaxRHIShaderPlatform.
			"RHI",
			// New Material Editor comments take the graph editor's default comment colour.
			"GraphEditor",
			// Open-editor tests: the editors' toolbar commands, Slate ticks.
			"Slate"
		});
	}
}
