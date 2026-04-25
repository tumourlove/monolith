using UnrealBuildTool;
using System.IO;

public class MonolithAudio : ModuleRules
{
	public MonolithAudio(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core", "CoreUObject", "Engine"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"MonolithCore",            // Registry, FMonolithActionResult
			"MonolithAudioRuntime",    // UMonolithSoundPerceptionUserData (Phase I3)
			"AIModule",                // UAISense_Hearing for sense-class resolution (Phase I3)
			"AudioMixer",              // SoundSubmix, submix effects
			"AudioEditor",             // All UFactory classes, graph schemas
			"AssetTools",              // FAssetToolsModule for rename operations
			"Json", "JsonUtilities",
			"Slate", "SlateCore",      // Editor module transitive deps
			"UnrealEd"                 // GEditor for preview, asset tools
		});

		// --- Conditional: MetaSound support ---
		// MetaSound is a built-in engine plugin (ships with UE 5.7), single location check sufficient.
		// Release builds: set MONOLITH_RELEASE_BUILD=1 to force all optional deps off.
		bool bHasMetaSound = false;
		bool bReleaseBuild = System.Environment.GetEnvironmentVariable("MONOLITH_RELEASE_BUILD") == "1";

		if (!bReleaseBuild)
		{
			string EngineDir = Path.GetFullPath(Target.RelativeEnginePath);

			// Check Engine Plugins (built-in — most common)
			if (Directory.Exists(Path.Combine(EngineDir, "Plugins", "Runtime", "Metasound")))
				bHasMetaSound = true;
		}

		PublicDefinitions.Add("WITH_METASOUND=" + (bHasMetaSound ? "1" : "0"));

		if (bHasMetaSound)
		{
			PrivateDependencyModuleNames.AddRange(new string[]
			{
				"MetasoundEngine",    // UMetaSoundBuilderSubsystem, Builder API
				"MetasoundFrontend",  // FMetasoundFrontendDocument, structs
				"MetasoundEditor"     // UMetaSoundEditorSubsystem, BuildToAsset
			});
		}
	}
}
