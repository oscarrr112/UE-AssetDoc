using UnrealBuildTool;

public class AssetDocumentAnimation : ModuleRules
{
	public AssetDocumentAnimation(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PublicDependencyModuleNames.AddRange(new string[] { "Core", "AssetDocument" });
		PrivateDependencyModuleNames.AddRange(new string[]
		{
			// Open-editor apply behaviour tests (*OpenEditorTests.cpp) read the open editors.
			"AnimationBlueprintEditor", "GraphEditor", "PropertyEditor",
			"CoreUObject", "Engine", "AssetDocumentEditor", "AnimGraph", "AssetDocumentServer",
			// Animation Blueprint graph adapters (AnimBlueprint/).
			"AssetDocumentGraph", "BlueprintGraph", "UnrealEd", "Kismet",
			// Standalone animation assets (AnimAssets/): the Skeleton editor's IEditableSkeleton.
			"SkeletonEditor",
			// Open-editor apply: the Animation editor (IAnimationEditor), Persona's tabs and skeleton tree.
			"AnimationEditor", "Persona", "Slate", "SlateCore",
			// Tests (Animation Blueprint end to end).
			"AssetRegistry", "Json",
			// Bone sampling: the engine's pose evaluation (UAnimPoseExtensions).
			"AnimationBlueprintLibrary"
		});
	}
}
