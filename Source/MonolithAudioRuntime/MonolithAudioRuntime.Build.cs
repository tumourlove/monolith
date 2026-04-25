using UnrealBuildTool;

public class MonolithAudioRuntime : ModuleRules
{
	public MonolithAudioRuntime(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// Runtime-typed module — ships in packaged builds. NO editor deps.
		// Sub-module of Monolith (sibling to MonolithAudio). Authority-gated MakeNoise
		// requires AIModule + Engine. AssetUserData lives in CoreUObject/Engine.
		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"AIModule"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
		});
	}
}
