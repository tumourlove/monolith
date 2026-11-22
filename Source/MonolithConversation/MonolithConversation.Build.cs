using UnrealBuildTool;
using System.IO;

public class MonolithConversation : ModuleRules
{
	public MonolithConversation(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// CommonConversation is an Experimental engine plugin. Detect it at build time so Monolith
		// still builds (with this namespace inactive) where it is not installed.
		// Release builds: set MONOLITH_RELEASE_BUILD=1 to force all optional deps off.
		bool bHasCommonConversation = false;
		bool bReleaseBuild = System.Environment.GetEnvironmentVariable("MONOLITH_RELEASE_BUILD") == "1";

		if (!bReleaseBuild)
		{
			// 1. Project Plugins/ folder
			string ProjectPluginsDir = Path.Combine(Target.ProjectFile.Directory.FullName, "Plugins");
			if (Directory.Exists(ProjectPluginsDir))
			{
				bHasCommonConversation = Directory.GetDirectories(
					ProjectPluginsDir, "CommonConversation", SearchOption.AllDirectories).Length > 0;
			}

			// 2. Engine Plugins/ (the plugin lives under Experimental/ in-tree)
			if (!bHasCommonConversation)
			{
				string EnginePluginsDir = Path.Combine(Path.GetFullPath(Target.RelativeEnginePath), "Plugins");
				if (Directory.Exists(EnginePluginsDir))
				{
					bHasCommonConversation = Directory.GetFiles(
						EnginePluginsDir, "CommonConversation.uplugin", SearchOption.AllDirectories).Length > 0;
				}
			}
		}

		if (bHasCommonConversation)
		{
			PrivateDependencyModuleNames.AddRange(new string[]
			{
				"Core", "CoreUObject", "Engine",
				"MonolithCore",
				"UnrealEd",
				"AIGraph",
				"AssetRegistry",
				"GameplayTags",
				"CommonConversationRuntime", "CommonConversationGraph",
				"Json", "JsonUtilities"
			});
			PublicDefinitions.Add("WITH_COMMONCONVERSATION=1");
		}
		else
		{
			PrivateDependencyModuleNames.AddRange(new string[]
			{
				"Core", "CoreUObject", "Engine",
				"MonolithCore"
			});
			PublicDefinitions.Add("WITH_COMMONCONVERSATION=0");
		}
	}
}
