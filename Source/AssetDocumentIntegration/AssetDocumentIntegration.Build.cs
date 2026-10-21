using UnrealBuildTool;

public class AssetDocumentIntegration : ModuleRules
{
	public AssetDocumentIntegration(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PublicDependencyModuleNames.AddRange(new string[] { "Core" });
		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"CoreUObject", "Engine", "UnrealEd", "Json", "AssetDocument", "AssetDocumentPython",
			"AssetDocumentEditor", "AssetDocumentAI", "AssetDocumentAnimation", "AssetDocumentGraph", "AssetDocumentWidget", "AssetDocumentData",
			"AssetDocumentNiagara",
			"AssetDocumentAudio",
			"AssetDocumentRig",
			"AssetDocumentPhysics",
			"AssetDocumentSequencer"
		});
	}
}
