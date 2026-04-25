using UnrealBuildTool;
using System.IO;

public class MonolithUI : ModuleRules
{
    public MonolithUI(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core", "CoreUObject", "Engine"
        });

        PrivateDependencyModuleNames.AddRange(new string[]
        {
            "MonolithCore", "UnrealEd", "UMG", "UMGEditor",
            "Slate", "SlateCore", "Json", "JsonUtilities",
            "KismetCompiler", "MovieScene", "MovieSceneTracks"
        });

        // CommonUI optional integration — detected across 3 install vectors so the
        // public Monolith free release can ship without hard-requiring CommonUI:
        //   1. Project-local Plugins/ folder (user copied CommonUI into their project)
        //   2. Engine Plugins/Marketplace/ (Fab/marketplace install)
        //   3. Engine Plugins/Runtime/ (stock UE 5.7 — first-party Epic plugin)
        // Set MONOLITH_RELEASE_BUILD=1 to force detection off (validates WITH_COMMONUI=0 path).
        //
        // IMPORTANT: Even if CommonUI exists in the engine, we must also verify that
        // Monolith's own CommonUI source files are present. The public release zip
        // gitignores these files — without this gate, end users get WITH_COMMONUI=1
        // but missing headers (C1083). See GitHub issue #36.
        bool bHasCommonUI = false;
        bool bReleaseBuild = System.Environment.GetEnvironmentVariable("MONOLITH_RELEASE_BUILD") == "1";

        // Self-check: do our own CommonUI source files exist? Release zips strip them.
        string OurCommonUIDir = Path.Combine(ModuleDirectory, "Public", "CommonUI");
        bool bHasOurCommonUISources = Directory.Exists(OurCommonUIDir)
            && File.Exists(Path.Combine(OurCommonUIDir, "MonolithCommonUIActions.h"));

        if (!bReleaseBuild && bHasOurCommonUISources)
        {
            // Location 1: project plugins (guard Target.ProjectFile — null for engine-only / Program targets)
            if (Target.ProjectFile != null)
            {
                string ProjectPluginsDir = Path.Combine(Target.ProjectFile.Directory.FullName, "Plugins");
                if (Directory.Exists(ProjectPluginsDir))
                {
                    bHasCommonUI = Directory.Exists(Path.Combine(ProjectPluginsDir, "CommonUI"));
                }
            }

            if (!bHasCommonUI)
            {
                string EngineDir = Path.GetFullPath(Target.RelativeEnginePath);
                string MarketplaceDir = Path.Combine(EngineDir, "Plugins", "Marketplace");
                if (Directory.Exists(MarketplaceDir))
                {
                    bHasCommonUI = Directory.Exists(Path.Combine(MarketplaceDir, "CommonUI"));
                }

                if (!bHasCommonUI)
                {
                    string RuntimeDir = Path.Combine(EngineDir, "Plugins", "Runtime", "CommonUI");
                    bHasCommonUI = Directory.Exists(RuntimeDir);
                }
            }
        }

        if (bHasCommonUI)
        {
            PrivateDependencyModuleNames.AddRange(new[] { "CommonUI", "CommonInput" });
            PublicDefinitions.Add("WITH_COMMONUI=1");
        }
        else
        {
            PublicDefinitions.Add("WITH_COMMONUI=0");
        }
    }
}
