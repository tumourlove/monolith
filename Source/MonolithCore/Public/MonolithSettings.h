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

	/** Master enable for the Monolith MCP HTTP server. Set false to disable
	 *  the localhost listener entirely (e.g. when working on an untrusted
	 *  network and you don't need AI tooling for the session). UE's
	 *  FHttpServerModule has no bind-address parameter, so the listener is
	 *  reachable on all network interfaces — this flag is the user-facing
	 *  kill-switch. Takes effect on next editor restart. (Issue #38) */
	UPROPERTY(config, EditAnywhere, Category="MCP Server")
	bool bMcpServerEnabled = true;

	/** Port for the embedded MCP HTTP server */
	UPROPERTY(config, EditAnywhere, Category="MCP Server", meta=(ClampMin="1024", ClampMax="65535"))
	int32 ServerPort = 9316;

	// --- Auto-Update ---

	/** Check GitHub Releases for updates on editor startup. Off by default
	 *  (Issue #38) — auto-fetching prebuilt zips from GitHub without
	 *  integrity verification or explicit user opt-in is supply-chain risk
	 *  for a freshly-installed plugin. Users who want auto-update enable
	 *  it explicitly here. */
	UPROPERTY(config, EditAnywhere, Category="Auto-Update")
	bool bAutoUpdateEnabled = false;

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

	/** Enable DataAsset property indexing (serializes all UPROPERTY defaults to JSON) */
	UPROPERTY(config, EditAnywhere, Category="Indexing|Deep Indexers")
	bool bIndexDataAssets = true;

	/** Enable Gameplay Ability System indexing (abilities, effects, attribute sets, cues) */
	UPROPERTY(config, EditAnywhere, Category="Indexing|Deep Indexers")
	bool bIndexGAS = true;

	/** Enable AI asset indexing (behavior trees, blackboards, state trees, EQS, smart objects) */
	UPROPERTY(config, EditAnywhere, Category="Indexing|Deep Indexers")
	bool bIndexAI = true;

	/** Enable Level Sequence Director Blueprint indexing (director functions, variables, event-track bindings) */
	UPROPERTY(config, EditAnywhere, Category="Indexing|Deep Indexers")
	bool bIndexLevelSequences = true;

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

	/** Enable mesh catalog indexing (bounds, size class, category for all StaticMesh assets) */
	UPROPERTY(config, EditAnywhere, Category="Indexing|Post-Pass Indexers")
	bool bIndexMeshCatalog = true;

	/** Index content from enabled marketplace plugins (installed via Fab/Epic launcher) */
	UPROPERTY(config, EditAnywhere, Category="Indexing")
	bool bIndexMarketplacePlugins = true;

	// --- Indexing Performance ---

	/** Memory budget for indexing in megabytes. 0 = auto-detect from installed RAM. Indexing will pause and run GC when this limit is exceeded. */
	UPROPERTY(config, EditAnywhere, Category="Indexing|Performance", DisplayName="Memory Budget (MB)",
		meta=(ClampMin="0", ClampMax="65536", ToolTip="0 = auto-detect tier based on installed RAM (64+ GB -> 32 GB, 32+ GB -> 16 GB, 16 GB -> 6 GB, <16 GB -> 3 GB). Set explicitly to override."))
	int32 MemoryBudgetMB = 0;

	/** Number of assets to process per batch during deep indexing. 0 = auto-detect from installed RAM. Lower values reduce memory spikes but increase indexing time. */
	UPROPERTY(config, EditAnywhere, Category="Indexing|Performance", DisplayName="Deep Index Batch Size",
		meta=(ClampMin="0", ClampMax="64", ToolTip="0 = auto-detect tier (32+ GB -> 8, 16 GB -> 4, <16 GB -> 2). Set explicitly to override. Lower = less memory, slower indexing."))
	int32 DeepIndexBatchSize = 0;

	/** Number of assets to process per batch for post-pass indexers (levels, meshes). 0 = auto-detect from installed RAM. These are memory-heavy so use smaller batches. */
	UPROPERTY(config, EditAnywhere, Category="Indexing|Performance", DisplayName="Post-Pass Batch Size",
		meta=(ClampMin="0", ClampMax="32", ToolTip="0 = auto-detect tier (32+ GB -> 4, 16 GB -> 2, <16 GB -> 1). Set explicitly to override. Lower for large assets."))
	int32 PostPassBatchSize = 0;

	/** Run garbage collection every N batches during indexing. Lower values keep memory lower but slow down indexing. */
	UPROPERTY(config, EditAnywhere, Category="Indexing|Performance", DisplayName="GC Frequency (Batches)",
		meta=(ClampMin="1", ClampMax="20", ToolTip="Run GC every N batches. 1 = every batch, higher = less frequent."))
	int32 GCFrequencyBatches = 2;

	/** Time to yield between batches when memory pressure is detected (seconds). Allows editor to remain responsive. */
	UPROPERTY(config, EditAnywhere, Category="Indexing|Performance", DisplayName="Yield Time (seconds)",
		meta=(ClampMin="0.0", ClampMax="1.0", ToolTip="Sleep time when throttling due to memory pressure."))
	float YieldTimeSeconds = 0.1f;

	/** Defer first-time indexing until explicitly triggered via console command. Useful for very large projects. */
	UPROPERTY(config, EditAnywhere, Category="Indexing|Performance", DisplayName="Defer First-Time Index",
		meta=(ToolTip="If true, first-time indexing won't run automatically. Use 'Monolith.StartIndex' console command to trigger."))
	bool bDeferFirstTimeIndex = false;

	/** Log memory statistics periodically during indexing. Off by default to keep shipped-project logs quiet — opt in when debugging indexer behavior. */
	UPROPERTY(config, EditAnywhere, Category="Indexing|Performance", DisplayName="Log Memory Stats",
		meta=(ToolTip="Log memory usage during indexing for debugging. Default off — enable when investigating memory pressure."))
	bool bLogMemoryStats = false;

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

	UPROPERTY(config, EditAnywhere, Category="Modules")
	bool bEnableUI = true;

	UPROPERTY(config, EditAnywhere, Category="Modules", DisplayName="Enable Mesh Module")
	bool bEnableMesh = true;

	// --- Optional Module Toggles ---

	UPROPERTY(config, EditAnywhere, Category="Modules|Optional",
		meta=(DisplayName="Enable Blueprint Assist Integration",
			  ToolTip="When enabled and Blueprint Assist is installed, provides enhanced graph formatting via the IMonolithGraphFormatter bridge."))
	bool bEnableBlueprintAssist = true;

	UPROPERTY(config, EditAnywhere, Category="Modules|Optional",
		meta=(DisplayName="Enable GAS Integration",
			  ToolTip="When enabled, registers gas_query actions for Gameplay Ability System manipulation. Requires GameplayAbilities plugin (engine-bundled)."))
	bool bEnableGAS = true;

	UPROPERTY(config, EditAnywhere, Category="Modules|Optional",
		meta=(DisplayName="Enable ComboGraph Integration",
			  ToolTip="When enabled and ComboGraph is installed, registers combograph_query actions for combo graph manipulation."))
	bool bEnableComboGraph = true;

	UPROPERTY(config, EditAnywhere, Category="Modules|Optional",
		meta=(DisplayName="Enable Logic Driver Integration",
			  ToolTip="When enabled and Logic Driver Pro is installed, registers logicdriver_query actions for state machine manipulation."))
	bool bEnableLogicDriver = true;

	UPROPERTY(config, EditAnywhere, Category="Modules|Optional",
		meta=(DisplayName="Enable AI Module",
			  ToolTip="Registers ai_query actions for AI asset manipulation (BT, BB, ST, EQS, SO, Navigation, Perception)."))
	bool bEnableAI = true;

	UPROPERTY(config, EditAnywhere, Category="Modules|Optional",
		meta=(DisplayName="Enable Inventory System X Module",
			  ToolTip="Registers inventory_query actions for InventorySystemX integration (items, equipment, storage, crafting, runtime PIE, AI, accessibility)."))
	bool bEnableInventorySystemX = true;

	UPROPERTY(config, EditAnywhere, Category="Modules|Optional",
		meta=(DisplayName="Enable Audio Module",
			  ToolTip="Registers audio_query actions for audio asset creation, inspection, batch management, Sound Cue graph building, and MetaSound graph building."))
	bool bEnableAudio = true;

	UPROPERTY(config, EditAnywhere, Category="Modules|Optional",
		meta=(DisplayName="Enable Level Sequence Module",
			  ToolTip="Registers level_sequence_query actions for Level Sequence Director Blueprint introspection (director functions, variables, event-track bindings to director functions, cross-sequence reverse lookup of function callers)."))
	bool bEnableLevelSequence = true;

	// --- Modules|Mesh ---

	UPROPERTY(config, EditAnywhere, Category="Modules|Mesh",
		DisplayName="Enable Procedural Town Generation (Experimental)",
		Meta=(EditCondition="bEnableMesh",
			  ToolTip="Registers town gen actions (city blocks, buildings, facades, roofs, floor plans, furnishing, terrain, spatial registry, debug views). EXPERIMENTAL — known geometry issues. Disable to hide these actions from MCP."))
	bool bEnableProceduralTownGen = false;

	UPROPERTY(config, EditAnywhere, Category="Modules|Mesh", DisplayName="Handle Pool Timeout (seconds)",
		Meta=(ClampMin="10.0", ClampMax="3600.0", EditCondition="bEnableMesh"))
	float MeshHandleTimeoutSeconds = 300.0f;

	UPROPERTY(config, EditAnywhere, Category="Modules|Mesh", DisplayName="Max Active Handles",
		Meta=(ClampMin="1", ClampMax="256", EditCondition="bEnableMesh"))
	int32 MaxActiveHandles = 32;

	UPROPERTY(config, EditAnywhere, Category="Modules|Mesh", DisplayName="Default Size Match Tolerance %",
		Meta=(ClampMin="1.0", ClampMax="100.0", EditCondition="bEnableMesh"))
	float DefaultSizeMatchTolerance = 20.0f;

	UPROPERTY(config, EditAnywhere, Category="Modules|Mesh", DisplayName="Surface Acoustics DataTable Path",
		Meta=(EditCondition="bEnableMesh"))
	FString SurfaceAcousticsTablePath = TEXT("/Game/Data/DT_SurfaceAcoustics");

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
