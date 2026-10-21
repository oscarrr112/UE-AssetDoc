// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

using UnrealBuildTool;

// Widget Blueprint provider ("umg" primitives) and the Widget Blueprint hooks into the
// generic graph layer. Depends on AssetDocumentGraph (the layer providers plug adapters
// into); AssetDocumentGraph must not depend on UMG / MovieScene.
public class AssetDocumentWidget : ModuleRules
{
	public AssetDocumentWidget(ReadOnlyTargetRules Target) : base(Target)
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
			"GraphEditor", "PropertyEditor",
			"AssetDocumentEditor",
			"AssetDocumentGraph",
			"UnrealEd",
			"BlueprintGraph",
			"Kismet",
			"Json",
			"JsonUtilities",
			"UMG",
			"UMGEditor",
			"MovieScene",
			"MovieSceneTracks",
			"Slate",
			"SlateCore",
			// Tests (Widget Blueprint end to end).
			"AssetRegistry"
		});
	}
}
