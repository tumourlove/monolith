using UnrealBuildTool;

public class MonolithAnimation : ModuleRules
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

	public MonolithAnimation(ReadOnlyTargetRules Target) : base(Target)
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
			"AssetRegistry", // IAssetRegistry / FAssetRegistryModule — used directly by the chooser read actions.
			"UnrealEd",
			"Slate",        // STableRow/SCompoundWidget instantiation (FRetargetChainElement rows).
			"SlateCore",    // FSlateAttributeDescriptor / SWidget::PrivateRegisterAttributes — was
			                // transitively satisfied on 5.7, not re-exported on 5.8; link explicitly on both.
			"AnimGraph",
			"AnimGraphRuntime",
			"BlueprintGraph",
			"AnimationBlueprintLibrary",
			"Json",
			"JsonUtilities",
			"PoseSearch",
			"EditorScriptingUtilities",
			"AnimationModifiers",
			"IKRig",
			"IKRigEditor",
			"ControlRig",
			"ControlRigDeveloper",
			"RigVM",
			"RigVMDeveloper",
			"PoseSearchEditor",    // UAnimGraphNode_MotionMatching (Wave 7 ABP graph wiring)
			"BlendStackEditor",    // UAnimGraphNode_BlendStack_Base (Sprint 4 BoundGraph-node spawn fix)
		});

		// --- Conditional: Chooser (UChooserTable authoring) ---
		// Issue #71: gate on ENABLEMENT (read from the .uproject), not disk presence.
		// Chooser ships under Engine/Plugins/Chooser on every UE 5.7 install but is
		// EnabledByDefault:false (Chooser.uplugin:13); a source builder who hasn't enabled it
		// would otherwise hard-link the Chooser module → GetLastError=126 (same class as #71).
		// The chooser handlers fall back to a clean "not available" error when WITH_CHOOSER=0.
		//
		// Release builds: MONOLITH_RELEASE_BUILD=1 still forces this OFF so binary releases
		// never hard-link against a plugin the end user may have disabled.
		bool bReleaseBuild = System.Environment.GetEnvironmentVariable("MONOLITH_RELEASE_BUILD") == "1";
		bool bHasChooser = !bReleaseBuild && IsPluginEnabled(Target, "Chooser");

		if (bHasChooser)
		{
			PrivateDependencyModuleNames.Add("Chooser");
			// FGameplayTagColumn cells use FGameplayTagContainer + UGameplayTagsManager.
			// Chooser keeps GameplayTags as a PRIVATE dep (not re-exported), so the
			// chooser-authoring GameplayTag cell-write path needs it directly. Only
			// linked when WITH_CHOOSER, matching the gated #if WITH_CHOOSER bodies.
			PrivateDependencyModuleNames.Add("GameplayTags");
			// PUBLIC so every TU in the module (including a sibling graph-surgery
			// file added later) sees WITH_CHOOSER consistently.
			PublicDefinitions.Add("WITH_CHOOSER=1");
		}
		else
		{
			PublicDefinitions.Add("WITH_CHOOSER=0");
		}
	}
}
