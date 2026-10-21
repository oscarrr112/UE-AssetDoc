using UnrealBuildTool;

public class AssetDocumentPython : ModuleRules
{
	public AssetDocumentPython(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core", "AssetDocument"
		});
		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"CoreUObject", "Engine", "PythonScriptPlugin", "Python3"
		});
	}
}
