using UnrealBuildTool;
using System.IO;

public class MonolithGAS : ModuleRules
{
	public MonolithGAS(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// Always-available engine GAS modules
		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core", "CoreUObject", "Engine",
			"GameplayAbilities", "GameplayTags", "GameplayTasks",
			// UMG: needed publicly because MonolithGASAttributeBindingClassExtension subclasses
			// UWidgetBlueprintGeneratedClassExtension (UMG) and exposes USTRUCTs referenced by other modules.
			"UMG", "Slate", "SlateCore"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"MonolithCore", "MonolithBlueprint",
			"UnrealEd", "BlueprintGraph",
			"GameplayAbilitiesEditor", "GameplayTasksEditor",
			"GameplayTagsEditor",
			"EnhancedInput",
			"EditorScriptingUtilities",
			"Json", "JsonUtilities",
			// UMGEditor: editor-side UWidgetBlueprintExtension + FWidgetBlueprintCompilerContext
			// (used only by Phase H1 attribute-binding action handlers; module already gated as Type:"Editor").
			"UMGEditor"
		});

		// --- Conditional: GBA (Blueprint Attributes) ---
		// The actual UE module is "BlueprintAttributes", not "GBAPlugin".
		//
		// Release builds: set MONOLITH_RELEASE_BUILD=1 to force all optional deps off.
		// This ensures binary releases don't link against plugins the user may not have.
		bool bHasGBA = false;
		bool bReleaseBuild = System.Environment.GetEnvironmentVariable("MONOLITH_RELEASE_BUILD") == "1";

		if (!bReleaseBuild)
		{
			// 1. Project Plugins/ folder (manual install or symlink)
			string ProjectPluginsDir = Path.Combine(
				Target.ProjectFile.Directory.FullName, "Plugins");
			if (Directory.Exists(ProjectPluginsDir))
			{
				bHasGBA = Directory.Exists(
					Path.Combine(ProjectPluginsDir, "BlueprintAttributes"))
					|| Directory.GetDirectories(
						ProjectPluginsDir, "Gameplaya*",
						SearchOption.TopDirectoryOnly).Length > 0;
			}

			// 2. Engine Plugins/Marketplace/ folder (Fab install)
			if (!bHasGBA)
			{
				string EngineDir = Path.GetFullPath(Target.RelativeEnginePath);
				string MarketplaceDir = Path.Combine(
					EngineDir, "Plugins", "Marketplace");
				if (Directory.Exists(MarketplaceDir))
				{
					bHasGBA = Directory.GetDirectories(
						MarketplaceDir, "Gameplaya*",
						SearchOption.TopDirectoryOnly).Length > 0;
				}

				// 3. Engine Plugins/ root
				if (!bHasGBA)
				{
					string EnginePluginsDir = Path.Combine(EngineDir, "Plugins");
					bHasGBA = Directory.Exists(
						Path.Combine(EnginePluginsDir, "BlueprintAttributes"));
				}
			}
		}

		if (bHasGBA)
		{
			PrivateDependencyModuleNames.Add("BlueprintAttributes");
			PublicDefinitions.Add("WITH_GBA=1");
		}
		else
		{
			PublicDefinitions.Add("WITH_GBA=0");
		}
	}
}
