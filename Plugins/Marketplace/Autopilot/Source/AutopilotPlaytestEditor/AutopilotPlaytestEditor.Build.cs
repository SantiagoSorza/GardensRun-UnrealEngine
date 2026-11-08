using UnrealBuildTool;

public class AutopilotPlaytestEditor : ModuleRules
{
	public AutopilotPlaytestEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"Json",
			"ApplicationCore",
			"InputCore",
			"Slate",
			"SlateCore",
			"UnrealEd",
			"LevelEditor",
			"ToolMenus",
			"WorkspaceMenuStructure",
			"Projects",
			"Settings",
			"MediaAssets",  // (a run's recording in its result: UMediaPlayer, UFileMediaSource, UMediaTexture)
			"AutopilotPlaytest",
		});
	}
}
