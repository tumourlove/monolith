using UnrealBuildTool;

public class MonolithMesh : ModuleRules
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

	public MonolithMesh(ReadOnlyTargetRules Target) : base(Target)
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
			"MonolithIndex",
			"SQLiteCore",
			"UnrealEd",
			"EditorSubsystem",
			"MeshDescription",
			"StaticMeshDescription",
			"MeshConversion",
			"PhysicsCore",
			"NavigationSystem",
			"RenderCore",
			"RHI",
			"EditorScriptingUtilities",
			"Json",
			"JsonUtilities",
			"Slate",
			"SlateCore",
			"AssetRegistry",
			"AssetTools",
			"MeshReductionInterface",
			"MeshMergeUtilities",
			"LevelInstanceEditor",
			"ImageCore"
		});

		// Optional: GeometryScripting (Tier 5 mesh operations only)
		//
		// Release builds: set MONOLITH_RELEASE_BUILD=1 to force this dep off so
		// the released DLL doesn't carry a hard import on UnrealEditor-GeometryScriptingCore.dll
		// (users who don't have the GeometryScripting plugin enabled in their .uproject
		// would otherwise hit GetLastError=126 at module load — see #26 / #30).
		// Source-tree users with GeometryScripting enabled still get full functionality.
		bool bReleaseBuild = System.Environment.GetEnvironmentVariable("MONOLITH_RELEASE_BUILD") == "1";

		// Issue #71: gate on ENABLEMENT (read from the .uproject), not disk presence.
		// GeometryScripting ships under Engine/Plugins/Runtime on every UE 5.7 install but is
		// EnabledByDefault:false; a source builder who hasn't enabled it would otherwise
		// hard-link GeometryScriptingCore → GetLastError=126 (same class as #30). The
		// PublicDelayLoadDLLs below remain as defense-in-depth (issue #70).
		bool bHasGeometryScripting = !bReleaseBuild && IsPluginEnabled(Target, "GeometryScripting");

		if (bHasGeometryScripting)
		{
			PrivateDependencyModuleNames.Add("GeometryScriptingCore");
			PrivateDependencyModuleNames.Add("GeometryFramework");
			PrivateDependencyModuleNames.Add("GeometryCore");
			PublicDefinitions.Add("WITH_GEOMETRYSCRIPT=1");

			// Delay-bind the GeometryScripting module DLLs so MonolithMesh.dll loads
			// even if these aren't resolvable at module-load time (issue #70 — the
			// import resolves lazily on first Tier-5 call instead of at LoadLibrary,
			// removing the CouldNotBeLoadedByOS / GetLastError=126 first-build/load race).
			// Verified: PublicDelayLoadDLLs at ModuleRules.cs:1308 (UE 5.7). Confirm via
			// dumpbin that these imports move to the Delay Import table.
			//
			// Windows-only: delay-load is a PE concept, and the hardcoded "UnrealEditor-"
			// prefix is only valid in a Shared build environment. Under
			// BuildEnvironment.Unique the engine modules carry the project target prefix
			// (e.g. "CookingHeartEditor-"), so a hardcoded "UnrealEditor-..." name fails to
			// resolve at link time (Mac: `ld: library 'UnrealEditor-GeometryScriptingCore.dll'
			// not found`). On non-Windows the PrivateDependencyModuleNames above already link
			// these modules with the correctly-decorated name, so no delay-load is needed.
			if (Target.Platform == UnrealTargetPlatform.Win64)
			{
				PublicDelayLoadDLLs.Add("UnrealEditor-GeometryScriptingCore.dll");
				PublicDelayLoadDLLs.Add("UnrealEditor-GeometryFramework.dll");
				PublicDelayLoadDLLs.Add("UnrealEditor-GeometryCore.dll");
			}
		}
		else
		{
			PublicDefinitions.Add("WITH_GEOMETRYSCRIPT=0");
		}
	}
}
