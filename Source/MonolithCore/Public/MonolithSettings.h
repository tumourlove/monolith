#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "MonolithSettings.generated.h"

UENUM()
enum class EMonolithLogVerbosity : uint8
{
	Quiet,
	Normal,
	Verbose,
	VeryVerbose
};

UCLASS(config=Monolith, defaultconfig, meta=(DisplayName="Monolith"))
class MONOLITHCORE_API UMonolithSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UMonolithSettings();

	// --- MCP Server ---

	/** Port for the embedded MCP HTTP server */
	UPROPERTY(config, EditAnywhere, Category="MCP Server", meta=(ClampMin="1024", ClampMax="65535"))
	int32 ServerPort = 9316;

	// --- Auto-Update ---

	/** Check GitHub Releases for updates on editor startup */
	UPROPERTY(config, EditAnywhere, Category="Auto-Update")
	bool bAutoUpdateEnabled = true;

	// --- Indexing ---

	/** Content paths to index in addition to /Game. Add plugin mount points like /MyPlugin. */
	UPROPERTY(config, EditAnywhere, Category="Indexing")
	TArray<FString> AdditionalContentPaths;

	/** Override path for ProjectIndex.db (empty = default Saved/ location) */
	UPROPERTY(config, EditAnywhere, Category="Indexing", meta=(RelativePath))
	FDirectoryPath DatabasePathOverride;

	/** Override path for engine source DB (empty = default Saved/ location) */
	UPROPERTY(config, EditAnywhere, Category="Indexing", meta=(RelativePath))
	FDirectoryPath EngineSourceDBPathOverride;

	/** Path to UE Engine/Source directory (empty = auto-detect) */
	UPROPERTY(config, EditAnywhere, Category="Indexing", meta=(RelativePath))
	FDirectoryPath EngineSourcePath;

	// --- Indexer Toggles ---

	/** Enable Blueprint deep indexing (graphs, nodes, variables) */
	UPROPERTY(config, EditAnywhere, Category="Indexing|Deep Indexers")
	bool bIndexBlueprints = true;

	/** Enable Material deep indexing (expressions, parameters) */
	UPROPERTY(config, EditAnywhere, Category="Indexing|Deep Indexers")
	bool bIndexMaterials = true;

	/** Enable generic asset metadata indexing (mesh stats, texture info, audio) */
	UPROPERTY(config, EditAnywhere, Category="Indexing|Deep Indexers")
	bool bIndexGenericAssets = true;

	/** Enable Niagara system indexing (emitters, renderers, sim targets) */
	UPROPERTY(config, EditAnywhere, Category="Indexing|Deep Indexers")
	bool bIndexNiagara = true;

	/** Enable UserDefinedEnum indexing (enum entries, values) */
	UPROPERTY(config, EditAnywhere, Category="Indexing|Deep Indexers")
	bool bIndexUserDefinedEnums = true;

	/** Enable UserDefinedStruct indexing (fields, types, defaults) */
	UPROPERTY(config, EditAnywhere, Category="Indexing|Deep Indexers")
	bool bIndexUserDefinedStructs = true;

	/** Enable InputAction indexing (value types, triggers, modifiers) */
	UPROPERTY(config, EditAnywhere, Category="Indexing|Deep Indexers")
	bool bIndexInputActions = true;

	/** Enable dependency graph indexing */
	UPROPERTY(config, EditAnywhere, Category="Indexing|Post-Pass Indexers")
	bool bIndexDependencies = true;

	/** Enable level/world actor indexing */
	UPROPERTY(config, EditAnywhere, Category="Indexing|Post-Pass Indexers")
	bool bIndexLevels = true;

	/** Enable DataTable row indexing */
	UPROPERTY(config, EditAnywhere, Category="Indexing|Post-Pass Indexers")
	bool bIndexDataTables = true;

	/** Enable config/INI indexing */
	UPROPERTY(config, EditAnywhere, Category="Indexing|Post-Pass Indexers")
	bool bIndexConfigs = true;

	/** Enable C++ symbol indexing (UCLASS, USTRUCT, etc.) */
	UPROPERTY(config, EditAnywhere, Category="Indexing|Post-Pass Indexers")
	bool bIndexCppSymbols = true;

	/** Enable animation asset indexing (sequences, montages, blend spaces) */
	UPROPERTY(config, EditAnywhere, Category="Indexing|Post-Pass Indexers")
	bool bIndexAnimations = true;

	/** Enable gameplay tag indexing */
	UPROPERTY(config, EditAnywhere, Category="Indexing|Post-Pass Indexers")
	bool bIndexGameplayTags = true;

	// --- Module Toggles ---

	UPROPERTY(config, EditAnywhere, Category="Modules")
	bool bEnableBlueprint = true;

	UPROPERTY(config, EditAnywhere, Category="Modules")
	bool bEnableMaterial = true;

	UPROPERTY(config, EditAnywhere, Category="Modules")
	bool bEnableAnimation = true;

	UPROPERTY(config, EditAnywhere, Category="Modules")
	bool bEnableNiagara = true;

	UPROPERTY(config, EditAnywhere, Category="Modules")
	bool bEnableEditor = true;

	UPROPERTY(config, EditAnywhere, Category="Modules")
	bool bEnableConfig = true;

	UPROPERTY(config, EditAnywhere, Category="Modules")
	bool bEnableIndex = true;

	UPROPERTY(config, EditAnywhere, Category="Modules")
	bool bEnableSource = true;

	// --- Logging ---

	/** Log verbosity for Monolith systems */
	UPROPERTY(config, EditAnywhere, Category="Logging")
	EMonolithLogVerbosity LogVerbosity = EMonolithLogVerbosity::Normal;

	// --- Helpers ---

	static const UMonolithSettings* Get();

	/** Returns /Game plus all AdditionalContentPaths as FName array for FARFilter usage */
	static TArray<FName> GetIndexedContentPaths();

	/** Returns true if the given package path starts with any indexed content path */
	static bool IsIndexedContentPath(const FString& PackagePath);

	/** Settings category path */
	virtual FName GetCategoryName() const override { return FName(TEXT("Plugins")); }
};
