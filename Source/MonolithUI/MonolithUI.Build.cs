using UnrealBuildTool;
using System.IO;

public class MonolithUI : ModuleRules
{
    public MonolithUI(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core", "CoreUObject", "Engine",
            // EditorSubsystem is Public because UMonolithUIRegistrySubsystem
            // (a UEditorSubsystem) is declared in Public/Registry/. Downstream
            // modules that include the header need the parent class visible.
            "EditorSubsystem"
        });

        PrivateDependencyModuleNames.AddRange(new string[]
        {
            "MonolithCore", "UnrealEd", "UMG", "UMGEditor",
            "Slate", "SlateCore", "Json", "JsonUtilities",
            "KismetCompiler", "MovieScene", "MovieSceneTracks",
            // Hoisted action requirements (Phase D — texture/font ingest, gradient MID factory):
            //   ImageWrapper / ImageCore        -- import_texture_from_bytes (PNG/JPG/etc decode)
            //   AssetTools                      -- CreateUniqueAssetName for new assets on disk
            //   Kismet                          -- FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified
            //   MaterialEditor                  -- UMaterialEditingLibrary::UpdateMaterialInstance
            "ImageWrapper", "ImageCore",
            "AssetTools",
            "Kismet",
            "MaterialEditor",
            // Phase G: UMonolithUISettings derives from UDeveloperSettings,
            // which lives in the DeveloperSettings module (NOT Engine).
            // Verified at C:\Program Files (x86)\UE_5.7\Engine\Source\Runtime\
            // DeveloperSettings\Public\Engine\DeveloperSettings.h:23
            // (UCLASS(Abstract, MinimalAPI) in module DeveloperSettings).
            // Missing this dep = LNK2019 on the constructor symbol.
            "DeveloperSettings",
            // Automation tests and editor helpers query assets generically
            // without hardcoding optional provider mount names.
            "AssetRegistry",
            // Phase 2 (2026-05-16 UI gap audit) — Item #10 apply_token_binding
            // probes IPluginManager::Get().FindPlugin("TokenforgeRuntime") to
            // emit -32011 when the optional Tokenforge provider is absent.
            // IPluginManager lives in the Projects module.
            "Projects",
            // Phase 2 (2026-05-16 UI gap audit) — Item #8 add_widget_variable
            // references UEdGraphSchema_K2::PC_* FName constants (Boolean,
            // Byte, Class, Double, Float, Int, Int64, Real, Name, Object,
            // SoftObject, String, Text, Struct, Wildcard, Enum) when parsing
            // var_type tokens into FEdGraphPinType. The constants are
            // dllimport'd from the BlueprintGraph module.
            "BlueprintGraph"
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
