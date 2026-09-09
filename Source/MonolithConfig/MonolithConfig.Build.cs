using UnrealBuildTool;

public class MonolithConfig : ModuleRules
{
	public MonolithConfig(ReadOnlyTargetRules Target) : base(Target)
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
			// Direct dep for the `localization` namespace's StringTable
			// discovery (FARFilter / IAssetRegistry). MonolithCore re-exports it,
			// but this module queries the registry itself.
			"AssetRegistry",
			"Json",
			"JsonUtilities",
			// `DeveloperSettings` is its OWN module (NOT part of Engine) — required
			// by the dev-gated `set_developer_setting` action which mutates
			// UDeveloperSettings CDOs at runtime. The header lives at
			// Engine/DeveloperSettings.h but resolves to this module.
			"DeveloperSettings"
		});
	}
}
