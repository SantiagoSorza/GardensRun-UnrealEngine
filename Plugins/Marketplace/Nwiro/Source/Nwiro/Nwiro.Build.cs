// Copyright 2026 Nwiro. All Rights Reserved.

using System.IO;
using UnrealBuildTool;

public class Nwiro : ModuleRules
{
	public Nwiro(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
		bUseUnity = true;
		MinSourceFilesForUnityBuildOverride = 1;

		ShadowVariableWarningLevel = WarningLevel.Off;

		// Bundled miniz for the ContentPipeline (zip extraction).
		PublicIncludePaths.AddRange(new string[] {
			Path.Combine(ModuleDirectory, "../ThirdParty/miniz")
		});

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"CoreUObject",
				"Engine",
				"Slate",
				"SlateCore",
				"WebBrowser",
				"UMG",
				"InputCore",
				"LevelEditor",
				"ToolMenus",
				"Projects",
				"Json",
				"JsonUtilities",
				"UnrealEd",
				"AssetTools",
				"BlueprintGraph",
				"KismetCompiler",
				"Kismet",
				"GraphEditor",
				"InputBlueprintNodes",
				"EnhancedInput",
				"PythonScriptPlugin",
				"EngineSettings",
				"Networking",
				"Sockets",
				"HTTP",
				"HTTPServer",
				"MaterialEditor",
				"EditorScriptingUtilities",
				"NwiroRuntime",
				"ApplicationCore",
				"DesktopPlatform",
				"ImageCore",
				"ImageWrapper",
				"RenderCore",
				"LevelSequence",
				"MovieScene",
				"MovieSceneTracks",
				"AIModule",
				"UMG",
				"UMGEditor",
				"Niagara",
				"GameplayTasks",
				"NavigationSystem",
				"StateTreeModule",
				"StateTreeEditorModule",
				"IKRig",
				"IKRigEditor",
				"PoseSearch",
				"PoseSearchEditor",
				"Landscape",
				"LandscapeEditor",
				"Foliage",
				"PhysicsCore",
				"EditorSubsystem",
				"PCG",
				"ContentBrowser",
				"AssetRegistry",
				// Movie Render Queue / Movie Render Graph (render tools)
				"MovieRenderPipelineCore",
				"MovieRenderPipelineSettings",
				"MovieRenderPipelineRenderPasses",
				"MovieRenderPipelineEditor",
				"CinematicCamera"
			}
		);

		// Link the experimental ToolsetRegistry module (engine's native MCP tools)
		// only on engine versions VERIFIED end-to-end on a clean Launcher engine
		// + Blueprint-only project.
		//
		// This is an explicit allowlist, NOT `>= 5.8`. ToolsetRegistry and
		// EditorToolset are Experimental — the tier Epic renames/removes between
		// minors. With an open-ended check, a 5.9 rename makes this hard module
		// link fail to resolve at load ("Missing import:
		// UnrealEditor-ToolsetRegistry.dll", GetLastError=126) and the ENTIRE
		// plugin refuses to load, taking the editor down with it. That is exactly
		// the 1.0.9 / UE 5.8 outage. A new engine version opts in only after it
		// passes the clean-install smoke test.
		//
		// Keep in lockstep with TOOLSET_VERIFIED_ENGINES in scripts/descriptor-policy.ts,
		// the single source of truth that injects the matching `EditorToolset`
		// descriptor reference. Both extract-plugin.ts (packaging) and
		// deploy-plugin.ts (local deploy) apply it through that module. Both must
		// agree: the descriptor reference is what mounts the plugin so this link can
		// resolve at runtime.
		bool bHasToolsetRegistry =
			Target.Version.MajorVersion == 5 && Target.Version.MinorVersion == 8;
		if (bHasToolsetRegistry)
		{
			PrivateDependencyModuleNames.Add("ToolsetRegistry");
			PublicDefinitions.Add("NWIRO_HAS_TOOLSET_REGISTRY=1");
		}
		else
		{
			PublicDefinitions.Add("NWIRO_HAS_TOOLSET_REGISTRY=0");
		}
	}
}
