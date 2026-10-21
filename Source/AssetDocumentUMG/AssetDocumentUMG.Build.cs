using UnrealBuildTool;

public class AssetDocumentUMG : ModuleRules
{
	public AssetDocumentUMG(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PublicDependencyModuleNames.AddRange(new string[] { "Core", "AssetDocument" });
		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"CoreUObject", "Engine", "AssetDocumentEditor", "UMG", "UMGEditor",
			"MovieScene", "MovieSceneTracks", "Blutility"
		});
	}
}
