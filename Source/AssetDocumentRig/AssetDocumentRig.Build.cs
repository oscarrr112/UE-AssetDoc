// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

using UnrealBuildTool;

// Rig provider ("rig" primitives, owner Rig): the IK Rig, IK Retargeter and Control Rig documents
// (docs/architecture/asset-document-ikrig-design.md, asset-document-controlrig-design.md). Its own module so that
// only this module depends on the IKRig, ControlRig and RigVM plugins. Everything is read and written through the IK Rig editor's public controllers
// (UIKRigController, UIKRetargeterController in IKRigEditor) and reflection; solver and op settings go through the
// Editor typed value codec. Uses the graph module's table-driven provider plumbing.
public class AssetDocumentRig : ModuleRules
{
	public AssetDocumentRig(ReadOnlyTargetRules Target) : base(Target)
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
			"Json",
			"JsonUtilities",
			"AssetRegistry",
			// retarget_animations: its HTTP route (FAssetDocumentHttpServer).
			"AssetDocumentServer",
			"IKRig",
			"IKRigEditor",
			// Control Rig (design asset-document-controlrig-design.md §4.2): the Blueprint, hierarchy, modular
			// model and RigVM graph model are read through their public headers.
			"ControlRig",
			"ControlRigDeveloper",
			"RigVM",
			"RigVMDeveloper",
			// Tests: what the open Control Rig editor holds (IRigVMEditor: its debugged rig and focused graph).
			"RigVMEditor",
			// Tests: the open editors' UI is ticked (FSlateApplication).
			"Slate",
			"SlateCore",
			// Edits (CR1): member variable pin types (RigVMDeveloperTypeUtils.h includes EdGraphSchema_K2.h).
			"BlueprintGraph",
			// Tests: duplicates of real rigs are made as the Content Browser does; Control Rig fixtures are made with
			// the Content Browser's factory.
			"AssetTools",
			"ControlRigEditor",
			// Tests: the retargeted animations' bone poses (UAnimPoseExtensions).
			"AnimationBlueprintLibrary"
		});
	}
}
