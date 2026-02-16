using UnrealBuildTool;

public class UEAgentBridge : ModuleRules
{
	public UEAgentBridge(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"DeveloperSettings",
				"InputCore",
				"AssetRegistry",
				"AssetTools",
				"HTTP",
				"Json",
				"JsonUtilities",
				"HTTPServer",
				"BlueprintGraph",
				"Kismet",
				"KismetCompiler",
				"UMG",
				"UMGEditor",
				"Slate",
				"SlateCore",
				"ToolMenus",
				"UnrealEd"
			}
		);
	}
}
