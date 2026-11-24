using System.IO;
using UnrealBuildTool;

public class MonolithLevelSequence : ModuleRules
{
	// LevelSequenceEditor is an engine plugin with EnabledByDefault:true, so (unlike the gated plugins in
	// MonolithAI / MonolithUI, which default to off) it is ON unless the target or the .uproject turns it off.
	// It ships with every editor, so MONOLITH_RELEASE_BUILD does not force it off: a release build keeps
	// refresh_current_sequence working, and a project that disables the plugin still loads Monolith.
	private static bool IsLevelSequenceEditorEnabled(ReadOnlyTargetRules Target)
	{
		const string PluginName = "LevelSequenceEditor";

		string EngineDir = Path.GetFullPath(Target.RelativeEnginePath);
		if (!Directory.Exists(Path.Combine(EngineDir, "Plugins", "MovieScene", PluginName)))
		{
			return false;
		}

		if (Target.DisablePlugins != null && System.Linq.Enumerable.Contains(Target.DisablePlugins, PluginName)) { return false; }
		if (Target.EnablePlugins != null && System.Linq.Enumerable.Contains(Target.EnablePlugins, PluginName)) { return true; }

		if (Target.ProjectFile != null)
		{
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
		}

		return true;   // no explicit entry -> EnabledByDefault
	}

	public MonolithLevelSequence(ReadOnlyTargetRules Target) : base(Target)
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
			"AssetRegistry",
			"MovieScene",
			"MovieSceneTracks",
			"LevelSequence",
			"BlueprintGraph",
			"Kismet",
			"EditorSubsystem",
			"Json",
			"JsonUtilities"
		});

		// refresh_current_sequence reaches the open Sequencer through ULevelSequenceEditorBlueprintLibrary.
		if (IsLevelSequenceEditorEnabled(Target))
		{
			PrivateDependencyModuleNames.Add("LevelSequenceEditor");
			PrivateDefinitions.Add("WITH_LEVELSEQUENCEEDITOR=1");
		}
		else
		{
			PrivateDefinitions.Add("WITH_LEVELSEQUENCEEDITOR=0");
		}
	}
}
