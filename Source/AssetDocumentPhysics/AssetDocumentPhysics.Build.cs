// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0

using UnrealBuildTool;

// Physics provider ("physics" primitives, owner Physics): the Physics Asset document
// (docs/architecture/asset-document-physics-asset-design.md). Its own module: physics assets are used by meshes,
// vehicles and props, not only animation, and only this module links PhysicsUtilities (FPhysicsAssetUtils, the public
// calls the Physics Asset Editor makes). Everything is read through reflection and the Editor typed value codec and
// written with the editor's own engine calls; the invocation plumbing is the graph module's table provider.
public class AssetDocumentPhysics : ModuleRules
{
	public AssetDocumentPhysics(ReadOnlyTargetRules Target) : base(Target)
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
			"AssetDocumentEditor",
			// Tests: the shared open-editor test support (AssetDocumentOpenEditorTestSupport.h, the editor's Save).
			"GraphEditor",
			"PropertyEditor",
			"AssetDocumentGraph",
			"UnrealEd",
			"PhysicsCore",
			"PhysicsUtilities",
			"Json",
			"JsonUtilities",
			"AssetRegistry",
			// Tests: duplicates of real physics assets are made as the Content Browser does.
			"AssetTools",
			// Tests: what the open Physics Asset Editor holds (its Persona preview).
			"PhysicsAssetEditor",
			"Persona",
			"Slate",
			"SlateCore"
		});

		// The Physics Asset Editor's Add Body (Fit) fits shapes from the mesh's bone vertex infos.
		PrivateDependencyModuleNames.AddRange(new string[] { "MeshUtilitiesCommon", "MeshUtilitiesEngine" });
	}
}
