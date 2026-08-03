using UnrealBuildTool;

public class MonolithIndex : ModuleRules
{
	// Returns true iff `PluginName` is ENABLED for this target (not merely present on disk).
	// Mirrors UnrealBuildTool Plugins.IsPluginEnabledForTarget (UE 5.7
	// Engine/Source/Programs/UnrealBuildTool/System/Plugins.cs:693). Fixes issue #71:
	// engine plugins ship-but-default-off (EnabledByDefault:false), so disk presence != enablement.
	// Design + API citations: Docs/plans/2026-06-15-issue71-plugin-enablement-gating.md.
	// Keep BYTE-IDENTICAL with the copies in MonolithMesh/MonolithIndex/MonolithAudio/MonolithAnimation.
	private static bool IsPluginEnabled(ReadOnlyTargetRules Target, string PluginName)
	{
		if (Target.ProjectFile == null)
		{
			return false;   // engine/program target with no .uproject: every gated engine plugin is EnabledByDefault:false -> treat as OFF
		}

		// 1. Target-level overrides win outright (uncommon but correct: -EnablePlugin=/-DisablePlugin=).
		if (Target.DisablePlugins != null && System.Linq.Enumerable.Contains(Target.DisablePlugins, PluginName)) { return false; }
		if (Target.EnablePlugins  != null && System.Linq.Enumerable.Contains(Target.EnablePlugins,  PluginName)) { return true;  }

		// 2. The .uproject's explicit Plugins[] entry (non-optional) is the deciding signal.
		try
		{
			ProjectDescriptor Project = ProjectDescriptor.FromFile(Target.ProjectFile);
			if (Project.Plugins != null)
			{
				foreach (PluginReferenceDescriptor Ref in Project.Plugins)
				{
					if (string.Equals(Ref.Name, PluginName, System.StringComparison.OrdinalIgnoreCase) && !Ref.bOptional)
					{
						return Ref.bEnabled
							&& Ref.IsEnabledForPlatform(Target.Platform)
							&& Ref.IsEnabledForTargetConfiguration(Target.Configuration)
							&& Ref.IsEnabledForTarget(Target.Type);
					}
				}
			}
		}
		catch (System.Exception)
		{
			return false;   // unreadable .uproject -> fail safe to OFF (never hard-link)
		}

		// 3. No .uproject entry -> falls to the .uplugin EnabledByDefault. ALL gated plugins here are
		//    engine plugins with EnabledByDefault:false, so absence == disabled. Return false.
		//    (If a future gated plugin were EnabledByDefault:true, this branch would need to read its
		//    .uplugin; documented as a known limitation in the plan.)
		return false;
	}

	public MonolithIndex(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"MonolithCore",
			"UnrealEd",
			"AssetRegistry",
			"EditorScriptingUtilities",
			"Json",
			"JsonUtilities",
			"SQLiteCore",
			"RenderCore",
			"RHI",
			"Slate",
			"SlateCore",
			"BlueprintGraph",
			"KismetCompiler",
			"EditorSubsystem",
			"AnimationCore",
			"Niagara",
			"GameplayTags",
			"GameplayAbilities",
			"EnhancedInput",
			"Projects"
		});

		// --- Conditional: MetaSound (engine-shipped Runtime plugin) ---
		// Issue #71: gate on ENABLEMENT (read from the .uproject), not disk presence.
		// Metasound ships under Engine/Plugins/Runtime on every UE 5.7 install but is
		// EnabledByDefault:false (and Optional:true in Monolith.uplugin), so a source builder
		// who hasn't enabled it would otherwise hard-link MetasoundEngine/MetasoundFrontend → 126.
		// Release builds: MONOLITH_RELEASE_BUILD=1 still forces this OFF (Issue #30 defense).
		bool bReleaseBuild = System.Environment.GetEnvironmentVariable("MONOLITH_RELEASE_BUILD") == "1";
		bool bHasMetasound = !bReleaseBuild && IsPluginEnabled(Target, "Metasound");

		if (bHasMetasound)
		{
			PrivateDependencyModuleNames.AddRange(new string[] { "MetasoundEngine", "MetasoundFrontend" });
			PublicDefinitions.Add("WITH_METASOUND=1");
		}
		else
		{
			PublicDefinitions.Add("WITH_METASOUND=0");
		}
	}
}
