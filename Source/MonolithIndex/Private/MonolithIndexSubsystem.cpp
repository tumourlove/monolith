#include "MonolithIndexSubsystem.h"
#include "Engine/TimerHandle.h"
#include "MonolithIndexDatabase.h"
#include "MonolithSettings.h"
#include "MonolithMemoryHelper.h"
#include "MonolithCompilerSafeDispatch.h"
#include "Misc/AsyncTaskNotification.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "HAL/RunnableThread.h"
#include "IO/IoHash.h"
#include "Async/Async.h"
#include "Editor.h"
#include "Interfaces/IPluginManager.h"
#include "HAL/IConsoleManager.h"

// Indexers
#include "Indexers/BlueprintIndexer.h"
#include "Indexers/MaterialIndexer.h"
#include "Indexers/GenericAssetIndexer.h"
#include "Indexers/DependencyIndexer.h"
#include "Indexers/LevelIndexer.h"
#include "Indexers/ConfigIndexer.h"
#include "Indexers/DataTableIndexer.h"
#include "Indexers/GameplayTagIndexer.h"
#include "Indexers/CppIndexer.h"
#include "Indexers/AnimationIndexer.h"
#include "Indexers/NiagaraIndexer.h"
#include "Indexers/UserDefinedEnumIndexer.h"
#include "Indexers/UserDefinedStructIndexer.h"
#include "Indexers/InputActionIndexer.h"
#include "Indexers/DataAssetIndexer.h"
#include "Indexers/MeshCatalogIndexer.h"
#include "Indexers/GASIndexer.h"
#include "Indexers/MetaSoundIndexer.h"

// ============================================================
// Incremental-reachability GC override (RAII)
// ============================================================
// UE 5.7's INCREMENTAL reachability GC (gc.AllowIncrementalReachability=1,
// gc.IncrementalReachabilityTimeLimit=0.002 by editor default) leaks GC
// worker-context bits from the process-global GWorkerIndices bitmask: when an
// incremental pass hits its 2ms budget it SUSPENDS and retains its worker
// contexts across frames; ReleaseAsyncProcessingContexts only frees them when
// no pass is suspended (GarbageCollection.cpp:7310-7313). The deep-index run
// drives GC continuously (forced collects per batch + per-asset GetAsset() ->
// LoadPackage -> FlushAsyncLoading), so suspended passes accumulate and exhaust
// the 64-slot pool -> "Exceeded max active GC worker contexts" assert.
//
// Forcing gc.AllowIncrementalReachability=0 for the run's duration makes every
// GC a BLOCKING collection, which always runs ReleaseAsyncProcessingContexts to
// completion in a single call -> worker bits are always freed -> the leak is
// structurally impossible. The original value is captured at run start and
// restored on the dtor (game thread), covering normal completion, error, and
// abort/cancel because all of those converge on OnIndexingFinished() (and the
// editor-shutdown-mid-index path resets it in Deinitialize()).
//
// File-static + single-flight (bIsIndexing) means at most one override is ever
// live, so a file-static TUniquePtr is a safe owner and keeps the fix .cpp-only.
namespace
{
	class FIncrementalReachabilityGCOverride
	{
	public:
		FIncrementalReachabilityGCOverride()
		{
			CVar = IConsoleManager::Get().FindConsoleVariable(TEXT("gc.AllowIncrementalReachability"));
			if (CVar)
			{
				OriginalValue = CVar->GetInt();
				CVar->Set(0, ECVF_SetByCode);
				UE_LOG(LogMonolithIndex, Log,
					TEXT("Deep index: forced gc.AllowIncrementalReachability=0 (was %d) to prevent GC worker-context leak"),
					OriginalValue);
			}
			else
			{
				UE_LOG(LogMonolithIndex, Warning,
					TEXT("Deep index: gc.AllowIncrementalReachability CVar not found — cannot disable incremental reachability GC"));
			}
		}

		~FIncrementalReachabilityGCOverride()
		{
			if (CVar)
			{
				CVar->Set(OriginalValue, ECVF_SetByCode);
				UE_LOG(LogMonolithIndex, Log,
					TEXT("Deep index: restored gc.AllowIncrementalReachability=%d"), OriginalValue);
			}
		}

	private:
		IConsoleVariable* CVar = nullptr;
		int32 OriginalValue = 1;
	};

	// At most one full-index run is live at a time (guarded by bIsIndexing), so a
	// single file-static owner is sufficient. Reset on the game thread only.
	static TUniquePtr<FIncrementalReachabilityGCOverride> GIncrementalGCOverride;
}

// Manual trigger for a project index. Primary use: when bDeferFirstTimeIndex
// is set, the DB starts empty and the user kicks the index with this command.
// File-static FAutoConsoleCommand (self-unregistering at module unload) keeps the
// fix .cpp-only — no header member required. Resolves the live editor subsystem at
// invoke time so it stays valid across editor lifecycle.
//
// Bare `Monolith.StartIndex` RESUMES an interrupted index; `Monolith.StartIndex
// force` wipes and starts over. Force is also the documented recovery for assets
// the poison-pill rule dropped from deep indexing — it clears their counters.
static FAutoConsoleCommand GMonolithStartIndexCommand(
	TEXT("Monolith.StartIndex"),
	TEXT("Starts a Monolith project index, resuming an interrupted one if present. Pass 'force' to wipe and start over."),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		if (!GEditor)
		{
			UE_LOG(LogMonolithIndex, Warning, TEXT("Monolith.StartIndex: GEditor not available — cannot start index"));
			return;
		}

		UMonolithIndexSubsystem* Subsystem = GEditor->GetEditorSubsystem<UMonolithIndexSubsystem>();
		if (!Subsystem)
		{
			UE_LOG(LogMonolithIndex, Warning, TEXT("Monolith.StartIndex: MonolithIndex subsystem not available — cannot start index"));
			return;
		}

		bool bForce = false;
		for (const FString& Arg : Args)
		{
			if (Arg.Equals(TEXT("force"), ESearchCase::IgnoreCase))
			{
				bForce = true;
				break;
			}
		}

		UE_LOG(LogMonolithIndex, Log, TEXT("Monolith.StartIndex: manual %s requested"),
			bForce ? TEXT("full index (forced wipe)") : TEXT("index (resume if interrupted)"));

		const bool bStarted = bForce ? Subsystem->StartFullIndex() : Subsystem->ResumeFullIndex();
		if (!bStarted)
		{
			UE_LOG(LogMonolithIndex, Warning, TEXT("Monolith.StartIndex: index did not start — see the preceding message"));
		}
	}));

void UMonolithIndexSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// Commandlet mode (cook/compile): skip DB open entirely. The running editor holds a WAL lock
	// on ProjectIndex.db and a second open surfaces as "disk I/O error" → UAT ExitCode=1.
	// The commandlet has no consumer of the index anyway.
	if (IsRunningCommandlet())
	{
		return;
	}

	Database = MakeUnique<FMonolithIndexDatabase>();
	FString DbPath = GetDatabasePath();

	if (!Database->Open(DbPath))
	{
		UE_LOG(LogMonolithIndex, Error, TEXT("Failed to open index database at %s"), *DbPath);
		return;
	}

	RegisterDefaultIndexers();

	// bEnableIndex gates only the indexing RUN, not action registration — query
	// actions stay registered so project_query keeps answering from an existing DB.
	if (!GetDefault<UMonolithSettings>()->bEnableIndex)
	{
		UE_LOG(LogMonolithIndex, Log, TEXT("MonolithIndex: indexing disabled via bEnableIndex=false; skipping index run"));
		return;
	}

	IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

	if (ShouldAutoIndex())
	{
		const bool bResumable = Database->SupportsIndexResume() && Database->IsFullIndexInProgress();

		// First-time index can be deferred for very large projects — leaves the DB
		// empty until 'Monolith.StartIndex' is run manually (escape hatch for the
		// GC worker-context crash class on huge / high-core-count environments).
		//
		// This is honoured on the RESUME path too. The flag exists for exactly the
		// crash class an interrupted index is evidence of, so overriding it here
		// would strip the user's escape hatch at the moment they need it, leaving
		// them to delete ProjectIndex.db by hand. The checkpoints are durable, so
		// waiting costs nothing.
		if (GetDefault<UMonolithSettings>()->bDeferFirstTimeIndex)
		{
			// The message is selected into a %s argument rather than being the format
			// string itself: UE 5.7's UE_LOG format-string sanitizer requires a
			// compile-time constant literal, so a ternary there is a static_assert.
			UE_LOG(LogMonolithIndex, Log, TEXT("%s"), bResumable
				? TEXT("MonolithIndex: an interrupted index is pending but deferred via bDeferFirstTimeIndex; run Monolith.StartIndex to resume it")
				: TEXT("MonolithIndex: first-time index deferred via bDeferFirstTimeIndex; run Monolith.StartIndex to begin"));
			return;
		}

		UE_LOG(LogMonolithIndex, Log, TEXT("%s"), bResumable
			? TEXT("Interrupted full index detected — deferring resume until AR ready")
			: TEXT("First launch — deferring full index until AR ready"));
		if (AR.IsLoadingAssets())
			AR.OnFilesLoaded().AddUObject(this, &UMonolithIndexSubsystem::OnAssetRegistryFilesLoaded);
		else
			ResumeFullIndex();
	}
	else if (CanDoIncrementalIndex())
	{
		UE_LOG(LogMonolithIndex, Log, TEXT("Existing index found — deferring incremental catch-up until AR ready"));
		if (AR.IsLoadingAssets())
			AR.OnFilesLoaded().AddUObject(this, &UMonolithIndexSubsystem::OnAssetRegistryFilesLoadedIncremental);
		else
			StartIncrementalIndex();
	}
	else
	{
		UE_LOG(LogMonolithIndex, Log, TEXT("Schema v1 DB — forcing full reindex to populate hashes"));
		if (AR.IsLoadingAssets())
			AR.OnFilesLoaded().AddUObject(this, &UMonolithIndexSubsystem::OnAssetRegistryFilesLoaded);
		else
			StartFullIndex();
	}
}

void UMonolithIndexSubsystem::OnAssetRegistryFilesLoaded()
{
	// Unbind ourselves — this is a one-shot callback
	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
	AssetRegistry.OnFilesLoaded().RemoveAll(this);

	if (ShouldAutoIndex())
	{
		UE_LOG(LogMonolithIndex, Log, TEXT("Asset Registry fully loaded -- starting full project index"));
		ResumeFullIndex();
	}
	else
	{
		UE_LOG(LogMonolithIndex, Log, TEXT("Asset Registry loaded but auto-index no longer needed (already indexed)"));
	}
}

void UMonolithIndexSubsystem::OnAssetRegistryFilesLoadedIncremental()
{
	// OnFilesLoaded is a void multicast; StartIncrementalIndex reports a result,
	// so it needs an adapter rather than binding directly.
	StartIncrementalIndex();
}

void UMonolithIndexSubsystem::Deinitialize()
{
	UnregisterLiveCallbacks();

	// Unbind from Asset Registry delegate if still bound
	if (FModuleManager::Get().IsModuleLoaded("AssetRegistry"))
	{
		IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
		AssetRegistry.OnFilesLoaded().RemoveAll(this);
	}

	// Stop any running indexing
	if (IndexingTaskPtr.IsValid())
	{
		if (bIsIndexing)
		{
			UE_LOG(LogMonolithIndex, Warning, TEXT("Indexing was still in progress during shutdown — force-stopped"));
		}
		IndexingTaskPtr->Stop();
		if (IndexingThread)
		{
			IndexingThread->WaitForCompletion();
			IndexingThread.Reset();
		}
		IndexingTaskPtr.Reset();
	}

	bIsIndexing = false;

	// Restore GC setting if the editor is shutting down mid-index (this abort
	// path force-stops the worker without routing through OnIndexingFinished).
	GIncrementalGCOverride.Reset();

	TaskNotification.Reset();

	if (Database.IsValid())
	{
		Database->Close();
	}

	Super::Deinitialize();
}

void UMonolithIndexSubsystem::RegisterIndexer(TSharedPtr<IMonolithIndexer> Indexer)
{
	if (!Indexer.IsValid()) return;

	Indexers.Add(Indexer);
	for (const FString& ClassName : Indexer->GetSupportedClasses())
	{
		ClassToIndexer.Add(ClassName, Indexer);
	}

	UE_LOG(LogMonolithIndex, Verbose, TEXT("Registered indexer: %s (%d classes)"),
		*Indexer->GetName(), Indexer->GetSupportedClasses().Num());
}

void UMonolithIndexSubsystem::RegisterDefaultIndexers()
{
	const UMonolithSettings* Settings = GetDefault<UMonolithSettings>();

	if (Settings->bIndexBlueprints)
		RegisterIndexer(MakeShared<FBlueprintIndexer>());
	if (Settings->bIndexMaterials)
		RegisterIndexer(MakeShared<FMaterialIndexer>());
	if (Settings->bIndexGenericAssets)
		RegisterIndexer(MakeShared<FGenericAssetIndexer>());
	if (Settings->bIndexDependencies)
		RegisterIndexer(MakeShared<FDependencyIndexer>());
	if (Settings->bIndexLevels)
		RegisterIndexer(MakeShared<FLevelIndexer>());
	if (Settings->bIndexDataTables)
		RegisterIndexer(MakeShared<FDataTableIndexer>());
	if (Settings->bIndexGameplayTags)
		RegisterIndexer(MakeShared<FGameplayTagIndexer>());
	if (Settings->bIndexConfigs)
		RegisterIndexer(MakeShared<FConfigIndexer>());
	if (Settings->bIndexCppSymbols)
		RegisterIndexer(MakeShared<FCppIndexer>());
	if (Settings->bIndexAnimations)
		RegisterIndexer(MakeShared<FAnimationIndexer>());
	if (Settings->bIndexNiagara)
		RegisterIndexer(MakeShared<FNiagaraIndexer>());
	if (Settings->bIndexUserDefinedEnums)
		RegisterIndexer(MakeShared<FUserDefinedEnumIndexer>());
	if (Settings->bIndexUserDefinedStructs)
		RegisterIndexer(MakeShared<FUserDefinedStructIndexer>());
	if (Settings->bIndexInputActions)
		RegisterIndexer(MakeShared<FInputActionIndexer>());
	if (Settings->bIndexDataAssets)
		RegisterIndexer(MakeShared<FDataAssetIndexer>());
	if (Settings->bIndexMeshCatalog)
		RegisterIndexer(MakeShared<FMeshCatalogIndexer>());
	if (Settings->bIndexGAS)
		RegisterIndexer(MakeShared<FGASIndexer>());
#if WITH_METASOUND
	if (Settings->bIndexMetaSounds)
		RegisterIndexer(MakeShared<FMetaSoundIndexer>());
#endif

	UE_LOG(LogMonolithIndex, Log, TEXT("Registered %d indexers"), Indexers.Num());
}

bool UMonolithIndexSubsystem::CanAcceptIndexRequest() const
{
	return !bIsIndexing && Database.IsValid() && Database->IsOpen();
}

bool UMonolithIndexSubsystem::StartFullIndex()
{
	return StartFullIndexInternal(/*bForceReset=*/true);
}

bool UMonolithIndexSubsystem::ResumeFullIndex()
{
	return StartFullIndexInternal(/*bForceReset=*/false);
}

bool UMonolithIndexSubsystem::StartFullIndexInternal(bool bForceReset)
{
	if (bIsIndexing)
	{
		UE_LOG(LogMonolithIndex, Warning, TEXT("Indexing already in progress"));
		return false;
	}

	if (!Database.IsValid() || !Database->IsOpen())
	{
		UE_LOG(LogMonolithIndex, Warning, TEXT("Index database is not open — cannot start a full index"));
		return false;
	}

	// Resume only when the schema can actually store checkpoints. `full_index_state`
	// is a `meta` row, so it survives a failed COLUMN migration — gating on the
	// marker alone would resume onto a v2 schema with nowhere to record progress.
	const bool bResume = !bForceReset
		&& Database->SupportsIndexResume()
		&& Database->IsFullIndexInProgress();

	bIsIndexing = true;

	// Force blocking (non-incremental) reachability GC for the whole run so the
	// engine cannot leak GC worker-context bits across suspended incremental
	// passes. Restored in OnIndexingFinished() (all worker exit paths) and
	// defensively in Deinitialize() (editor shutdown mid-index). Game thread.
	GIncrementalGCOverride = MakeUnique<FIncrementalReachabilityGCOverride>();

	if (bResume)
	{
		UE_LOG(LogMonolithIndex, Log,
			TEXT("Resuming an interrupted full index — already-indexed assets keep their data and are not re-deep-indexed"));
	}
	else
	{
		// Reset the database for a full re-index. The return value used to be
		// ignored: a failed reset left the old rows in place and the run then
		// fought UNIQUE constraints on every insert.
		if (!Database->ResetDatabase())
		{
			UE_LOG(LogMonolithIndex, Error, TEXT("Failed to reset the index database — aborting the full index"));
			OnIndexingFinished(false);
			return false;
		}

		// AFTER the reset: ResetDatabase() drops the `meta` table, which is where
		// the in-progress marker lives.
		if (!Database->BeginFullIndex())
		{
			UE_LOG(LogMonolithIndex, Warning,
				TEXT("Could not record the full-index start marker — this run will not be resumable if it is interrupted"));
		}
	}

	// Gather marketplace plugin paths for indexing
	IndexedPlugins = GatherMarketplacePluginPaths();

	// Show notification
	FAsyncTaskNotificationConfig NotifConfig;
	NotifConfig.TitleText = FText::FromString(TEXT("Monolith"));
	NotifConfig.ProgressText = FText::FromString(TEXT("Indexing project..."));
	NotifConfig.bCanCancel = true;
	NotifConfig.LogCategory = &LogMonolithIndex;
	TaskNotification = MakeUnique<FAsyncTaskNotification>(NotifConfig);

	// Launch background thread
	IndexingTaskPtr = MakeUnique<FIndexingTask>(this);
	IndexingTaskPtr->PluginsToIndex = IndexedPlugins;
	IndexingTaskPtr->bIsResume = bResume;
	IndexingThread.Reset(FRunnableThread::Create(
		IndexingTaskPtr.Get(),
		TEXT("MonolithIndexing"),
		0,
		TPri_BelowNormal
	));

	if (!IndexingThread)
	{
		// Run() will never execute, so nothing would ever route through
		// OnIndexingFinished and bIsIndexing would stay latched for the rest of
		// the session. Unwind here instead.
		UE_LOG(LogMonolithIndex, Error, TEXT("Failed to create the indexing thread — no index is running"));
		IndexingTaskPtr.Reset();
		OnIndexingFinished(false);
		return false;
	}

	UE_LOG(LogMonolithIndex, Log, TEXT("Background indexing started"));
	return true;
}

float UMonolithIndexSubsystem::GetProgress() const
{
	if (!IndexingTaskPtr.IsValid() || IndexingTaskPtr->TotalAssets == 0) return 0.0f;
	return static_cast<float>(IndexingTaskPtr->CurrentIndex) / static_cast<float>(IndexingTaskPtr->TotalAssets);
}

// ============================================================
// Query API wrappers
// ============================================================

TArray<FSearchResult> UMonolithIndexSubsystem::Search(const FString& Query, int32 Limit)
{
	if (!Database.IsValid() || !Database->IsOpen()) return {};
	return Database->FullTextSearch(Query, Limit);
}

TSharedPtr<FJsonObject> UMonolithIndexSubsystem::FindReferences(const FString& PackagePath)
{
	if (!Database.IsValid() || !Database->IsOpen()) return nullptr;
	return Database->FindReferences(PackagePath);
}

TArray<FIndexedAsset> UMonolithIndexSubsystem::FindByType(const FString& AssetClass, int32 Limit, int32 Offset)
{
	if (!Database.IsValid() || !Database->IsOpen()) return {};
	return Database->FindByType(AssetClass, Limit, Offset);
}

TSharedPtr<FJsonObject> UMonolithIndexSubsystem::GetStats()
{
	if (!Database.IsValid() || !Database->IsOpen()) return nullptr;
	return Database->GetStats();
}

TSharedPtr<FJsonObject> UMonolithIndexSubsystem::GetAssetDetails(const FString& PackagePath)
{
	if (!Database.IsValid() || !Database->IsOpen()) return nullptr;
	return Database->GetAssetDetails(PackagePath);
}

TArray<FIndexedPluginInfo> UMonolithIndexSubsystem::GatherMarketplacePluginPaths() const
{
    TArray<FIndexedPluginInfo> Result;

    const UMonolithSettings* Settings = GetDefault<UMonolithSettings>();
    if (!Settings->bIndexMarketplacePlugins)
    {
        return Result;
    }

    TArray<TSharedRef<IPlugin>> ContentPlugins = IPluginManager::Get().GetEnabledPluginsWithContent();
    for (const TSharedRef<IPlugin>& Plugin : ContentPlugins)
    {
        // Skip engine plugins — keep project/marketplace plugins that have content directories
        if (Plugin->GetType() == EPluginType::Engine)
        {
            continue;
        }
        FString PluginContentDir = Plugin->GetContentDir();
        if (!FPaths::DirectoryExists(PluginContentDir))
        {
            continue;
        }

        FIndexedPluginInfo Info;
        Info.PluginName = Plugin->GetName();
        Info.MountPath = Plugin->GetMountedAssetPath();
        Info.ContentDir = Plugin->GetContentDir();
        Info.FriendlyName = Plugin->GetDescriptor().FriendlyName;

        UE_LOG(LogMonolithIndex, Log, TEXT("Marketplace plugin found: %s (mount: %s)"),
            *Info.FriendlyName, *Info.MountPath);

        Result.Add(MoveTemp(Info));
    }

    UE_LOG(LogMonolithIndex, Log, TEXT("Found %d marketplace plugins to index"), Result.Num());
    return Result;
}

// ============================================================
// Background indexing task
// ============================================================

UMonolithIndexSubsystem::FIndexingTask::FIndexingTask(UMonolithIndexSubsystem* InOwner)
	: Owner(InOwner)
{
}

uint32 UMonolithIndexSubsystem::FIndexingTask::Run()
{
	const UMonolithSettings* GlobalSettings = GetDefault<UMonolithSettings>();
	const bool bLogMemory = GlobalSettings ? GlobalSettings->bLogMemoryStats : true;

	// Every fire-and-forget AsyncTask below captures this instead of `this`.
	// Deinitialize() does Stop() -> WaitForCompletion() -> IndexingTaskPtr.Reset():
	// joining the worker does NOT drain the game thread's task queue, so an
	// already-queued lambda can run after this FIndexingTask has been destroyed.
	// Anything it needs must be copied into the capture list beforehand.
	const TWeakObjectPtr<UMonolithIndexSubsystem> WeakOwner(Owner);

	if (bLogMemory)
	{
		FMonolithMemoryHelper::LogMemoryStats(TEXT("Full index starting"));
	}

	// Asset Registry enumeration MUST happen on the game thread
	TArray<FAssetData> AllAssets;
	FEvent* RegistryEvent = FPlatformProcess::GetSynchEventFromPool(true);
	AsyncTask(ENamedThreads::GameThread, [this, &AllAssets, RegistryEvent]()
	{
		IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

		if (!AssetRegistry.IsSearchAllAssets())
		{
			AssetRegistry.SearchAllAssets(true);
		}
		AssetRegistry.WaitForCompletion();

		FARFilter Filter;
		Filter.PackagePaths.Add(FName(TEXT("/Game")));
		// Add marketplace plugin mount paths
		for (const FIndexedPluginInfo& PluginInfo : PluginsToIndex)
		{
			FString CleanPath = PluginInfo.MountPath;
			if (CleanPath.EndsWith(TEXT("/")))
			{
				CleanPath.LeftChopInline(1);
			}
			Filter.PackagePaths.Add(FName(*CleanPath));
		}
		// Add user-configured additional content paths
		{
			const UMonolithSettings* Settings = GetDefault<UMonolithSettings>();
			if (Settings)
			{
				for (const FString& CustomPath : Settings->AdditionalContentPaths)
				{
					if (!CustomPath.IsEmpty())
					{
						FString CleanPath = CustomPath;
						if (CleanPath.EndsWith(TEXT("/")))
						{
							CleanPath.LeftChopInline(1);
						}
						Filter.PackagePaths.AddUnique(FName(*CleanPath));
					}
				}
			}
		}
		Filter.bRecursivePaths = true;
		AssetRegistry.GetAssets(Filter, AllAssets);

		RegistryEvent->Trigger();
	});
	RegistryEvent->Wait();
	FPlatformProcess::ReturnSynchEventToPool(RegistryEvent);

	TotalAssets = AllAssets.Num();
	Owner->IndexingStatusMessage = FString::Printf(TEXT("Scanning %d assets..."), TotalAssets.Load());
	UE_LOG(LogMonolithIndex, Log, TEXT("Indexing %d assets..."), TotalAssets.Load());

	FMonolithIndexDatabase* DB = Owner->Database.Get();
	if (!DB || !DB->IsOpen())
	{
		AsyncTask(ENamedThreads::GameThread, [WeakOwner]()
		{
			if (UMonolithIndexSubsystem* Subsystem = WeakOwner.Get())
			{
				Subsystem->OnIndexingFinished(false);
			}
		});
		return 1;
	}

	// Structural failures — transaction/DB errors — are the ONLY thing besides
	// cancellation that may block the completion marker. Per-asset failures are
	// diagnostics: `Errors`/`DeepErrors` fire routinely on real projects (a class
	// from a disabled plugin, a redirector stub, an animation asset that crashes
	// on load — the SEH guard exists because those are expected), and gating on
	// them would mean one bad asset re-runs the whole index on every launch, which
	// is the #117 symptom reached from the other direction.
	TAtomic<bool> bTransactionFailure{false};
	auto RequireTransaction = [&bTransactionFailure](bool bOk, const TCHAR* What) -> bool
	{
		if (!bOk)
		{
			bTransactionFailure = true;
			UE_LOG(LogMonolithIndex, Error, TEXT("Index transaction failure (%s) — this run will not be marked complete"), What);
		}
		return bOk;
	};

	RequireTransaction(DB->BeginTransaction(), TEXT("metadata pass begin"));

	int32 BatchSize = 100;
	int32 Indexed = 0;
	int32 Errors = 0;

	// Collect assets that have deep indexers for a second pass
	struct FDeepIndexEntry
	{
		FAssetData AssetData;
		int64 AssetId;
		TSharedPtr<IMonolithIndexer> Indexer;
		FString SavedHash;
	};
	TArray<FDeepIndexEntry> DeepIndexQueue;

	// Resume bookkeeping.
	int32 AlreadyDeepIndexed = 0;
	TArray<FString> PoisonedPaths;

	TMap<FString, int32> ClassDistribution;
	TMap<FString, int32> QueuedClassDistribution;

	IAssetRegistry* AssetRegistryPtr = IAssetRegistry::Get();

	for (int32 i = 0; i < AllAssets.Num(); ++i)
	{
		if (bShouldStop) break;

		if (Owner->TaskNotification && Owner->TaskNotification->GetPromptAction() == EAsyncTaskNotificationPromptAction::Cancel)
		{
			bShouldStop = true;
			break;
		}

		const FAssetData& AssetData = AllAssets[i];
		CurrentIndex = i + 1;

		// Insert the base asset record
		FIndexedAsset IndexedAsset;
		IndexedAsset.PackagePath = AssetData.PackageName.ToString();
		IndexedAsset.AssetName = AssetData.AssetName.ToString();
		IndexedAsset.AssetClass = AssetData.AssetClassPath.GetAssetName().ToString();
		ClassDistribution.FindOrAdd(IndexedAsset.AssetClass)++;

		// Determine module name from package path
		if (!IndexedAsset.PackagePath.StartsWith(TEXT("/Game/")))
		{
			for (const FIndexedPluginInfo& PluginInfo : PluginsToIndex)
			{
				if (IndexedAsset.PackagePath.StartsWith(PluginInfo.MountPath))
				{
					IndexedAsset.ModuleName = PluginInfo.PluginName;
					break;
				}
			}
		}

		// If not matched to a marketplace plugin, check additional content paths
		if (IndexedAsset.ModuleName.IsEmpty() && !IndexedAsset.PackagePath.StartsWith(TEXT("/Game/")))
		{
			int32 SecondSlash = IndexedAsset.PackagePath.Find(TEXT("/"), ESearchCase::CaseSensitive, ESearchDir::FromStart, 1);
			if (SecondSlash > 1)
			{
				IndexedAsset.ModuleName = IndexedAsset.PackagePath.Mid(1, SecondSlash - 1);
			}
		}

		// Get disk file modification time for incremental change detection
		{
			FString PackageFilename;
			if (FPackageName::DoesPackageExist(AssetData.PackageName.ToString(), &PackageFilename))
			{
				FDateTime FileTime = IFileManager::Get().GetTimeStamp(*PackageFilename);
				IndexedAsset.LastModified = FileTime.ToIso8601();
			}
		}

		// Get Blake3 hash for move detection (available from AR without loading the package)
		if (AssetRegistryPtr)
		{
			TOptional<FAssetPackageData> PackageData = AssetRegistryPtr->GetAssetPackageDataCopy(AssetData.PackageName);
			if (PackageData.IsSet())
			{
				FIoHash Hash = PackageData->GetPackageSavedHash();
				IndexedAsset.SavedHash = LexToString(Hash);
			}
		}

		// UPSERT rather than a blind INSERT. On a resume the row already exists,
		// and the old insert would hit the package_path UNIQUE constraint, count an
		// error and `continue` — which also dropped the asset from the deep queue.
		int64 AssetId = -1;
		FString StoredDeepHash;
		int32 StoredAttempts = 0;
		bool bHadRow = false;

		if (TOptional<FIndexedAsset> Existing = DB->GetAssetByPath(IndexedAsset.PackagePath))
		{
			bHadRow = true;
			AssetId = Existing->Id;
			StoredDeepHash = Existing->DeepIndexedHash;
			StoredAttempts = Existing->DeepIndexAttempts;

			if (!DB->UpdateAssetMetadata(IndexedAsset))
			{
				// Count it, but keep going: the row and its id are valid, so the
				// asset can still be deep-indexed. Dropping it here would lose data
				// the resume exists to preserve.
				Errors++;
			}
		}
		else
		{
			AssetId = DB->InsertAsset(IndexedAsset);
		}

		if (AssetId < 0)
		{
			Errors++;
			continue;
		}

		// Queue assets that have deep indexers (Blueprint, Material, etc.)
		TSharedPtr<IMonolithIndexer>* FoundIndexer = Owner->ClassToIndexer.Find(IndexedAsset.AssetClass);
		if (FoundIndexer && FoundIndexer->IsValid())
		{
			// On a fresh run the table was just wiped, so every asset lands on
			// Queue and this filter is inert.
			const EMonolithDeepIndexQueueDecision Decision =
				MonolithDecideDeepIndexQueueEntry(StoredDeepHash, StoredAttempts, IndexedAsset.SavedHash);

			switch (Decision)
			{
			case EMonolithDeepIndexQueueDecision::SkipAlreadyIndexed:
				AlreadyDeepIndexed++;
				break;

			case EMonolithDeepIndexQueueDecision::SkipPoisonAsset:
				// Two runs started this asset and neither finished. Re-queueing it
				// is a crash-every-launch loop, so drop it and stamp the current
				// hash so it leaves the queue for good.
				//
				// The attempt counter is deliberately NOT cleared. Clearing it
				// would make the skip depend entirely on the hash stamp, and an
				// asset whose Asset Registry hash is empty would stamp '' , fail
				// the hash comparison on the next run, re-enter the queue, and
				// resume crashing. Leaving the counter at its limit keeps the
				// asset out on both gates. `force` is the documented recovery and
				// resets everything.
				DB->SetDeepIndexedHash(AssetId, IndexedAsset.SavedHash);
				PoisonedPaths.Add(IndexedAsset.PackagePath);
				break;

			case EMonolithDeepIndexQueueDecision::Queue:
			default:
				// Genuine re-index (the asset changed since it was last deep
				// indexed): clear the child rows the previous pass produced. On a
				// first pass there is nothing to delete, so this stays off the
				// fresh-index path.
				if (bHadRow && !StoredDeepHash.IsEmpty())
				{
					DB->DeleteChildDataForAsset(AssetId);
				}
				DeepIndexQueue.Add({ AssetData, AssetId, *FoundIndexer, IndexedAsset.SavedHash });
				QueuedClassDistribution.FindOrAdd(IndexedAsset.AssetClass)++;
				break;
			}
		}

		Indexed++;

		// Commit in batches
		if (Indexed % BatchSize == 0)
		{
			// A silently failed commit used to lose 100 assets with no signal.
			RequireTransaction(DB->CommitTransaction(), TEXT("metadata batch commit"));
			RequireTransaction(DB->BeginTransaction(), TEXT("metadata batch begin"));

			UE_LOG(LogMonolithIndex, Log, TEXT("Indexed %d / %d assets (%d errors)"),
				Indexed, TotalAssets.Load(), Errors);

			if (Owner->TaskNotification)
			{
				Owner->TaskNotification->SetProgressText(FText::FromString(
					FString::Printf(TEXT("Indexing %d / %d assets..."), CurrentIndex.Load(), TotalAssets.Load())));
			}

			// Snapshot the counters here, on the worker. Loading them inside the
			// lambda would read them through a task object that may already be gone.
			const int32 ProgressCurrent = CurrentIndex.Load();
			const int32 ProgressTotal = TotalAssets.Load();
			AsyncTask(ENamedThreads::GameThread, [WeakOwner, ProgressCurrent, ProgressTotal]()
			{
				if (UMonolithIndexSubsystem* Subsystem = WeakOwner.Get())
				{
					Subsystem->OnProgress.Broadcast(ProgressCurrent, ProgressTotal);
				}
			});
		}
	}

	// Log class distribution summary
	UE_LOG(LogMonolithIndex, Log, TEXT("Asset class distribution (top 20):"));
	ClassDistribution.ValueSort([](int32 A, int32 B) { return A > B; });
	int32 Shown = 0;
	for (const auto& Pair : ClassDistribution)
	{
		if (Shown++ >= 20) break;
		UE_LOG(LogMonolithIndex, Log, TEXT("  %s: %d"), *Pair.Key, Pair.Value);
	}

	UE_LOG(LogMonolithIndex, Log, TEXT("Deep index queue: %d assets across %d classes"),
		DeepIndexQueue.Num(), QueuedClassDistribution.Num());
	for (const auto& Pair : QueuedClassDistribution)
	{
		UE_LOG(LogMonolithIndex, Log, TEXT("  Queued %s: %d"), *Pair.Key, Pair.Value);
	}

	RequireTransaction(DB->CommitTransaction(), TEXT("metadata pass commit"));

	UE_LOG(LogMonolithIndex, Log, TEXT("Metadata pass complete: %d assets indexed, %d errors"), Indexed, Errors);

	if (bIsResume)
	{
		UE_LOG(LogMonolithIndex, Log,
			TEXT("Resume: %d assets were already deep-indexed at their current content hash and were not re-queued"),
			AlreadyDeepIndexed);
	}

	// One poison asset takes its whole batch out of deep indexing (the attempt
	// marker is batch-granular). That is a data-completeness loss the user cannot
	// otherwise discover, so it is reported at ERROR — a Warning is invisible in a
	// busy index log — and persisted so it outlives the session and the log.
	if (PoisonedPaths.Num() > 0)
	{
		for (const FString& Path : PoisonedPaths)
		{
			UE_LOG(LogMonolithIndex, Error,
				TEXT("Deep indexing skipped '%s': %d interrupted attempts. Its graph/variable data will be missing. Run 'Monolith.StartIndex force' (or monolith_reindex force=true) to clear the counters and retry."),
				*Path, MonolithMaxDeepIndexAttempts);
		}
		DB->RecordSkippedAssetPaths(PoisonedPaths);
		UE_LOG(LogMonolithIndex, Error,
			TEXT("%d asset(s) were dropped from deep indexing after repeated interrupted attempts — see project get_stats 'skipped_assets'"),
			PoisonedPaths.Num());
	}

	// ============================================================
	// Deep indexing pass — load assets on game thread in time-budgeted batches
	// Assets must be loaded on the game thread to avoid texture compiler crashes.
	// We process in small batches with GC and memory management to prevent OOM.
	// ============================================================
	Owner->IndexingStatusMessage = FString::Printf(TEXT("Deep indexing %d assets..."), DeepIndexQueue.Num());

	if (!bShouldStop && DeepIndexQueue.Num() > 0)
	{
		const UMonolithSettings* Settings = GetDefault<UMonolithSettings>();
		FMonolithMemoryHelper::LogTierStartupOnce();
		const int32 DeepBatchSize = FMath::Max(1, FMonolithMemoryHelper::GetResolvedDeepIndexBatchSize());
		const int32 GCFrequency = FMath::Max(1, Settings->GCFrequencyBatches);
		const SIZE_T MemoryBudgetMB = static_cast<SIZE_T>(FMonolithMemoryHelper::GetResolvedMemoryBudgetMB());
		const float YieldTime = Settings->YieldTimeSeconds;

		UE_LOG(LogMonolithIndex, Log, TEXT("Starting deep indexing pass for %d assets (batch size: %d, GC every %d batches, memory budget: %llu MB)..."),
			DeepIndexQueue.Num(), DeepBatchSize, GCFrequency, MemoryBudgetMB);

		if (bLogMemory)
		{
			FMonolithMemoryHelper::LogMemoryStats(TEXT("Deep index start"));
		}

		constexpr double FrameBudgetSeconds = 0.016; // ~16ms per batch to stay interactive
		TAtomic<int32> DeepIndexed{0};
		TAtomic<int32> DeepErrors{0};
		int32 TotalDeep = DeepIndexQueue.Num();
		int32 BatchNumber = 0;

		for (int32 BatchStart = 0; BatchStart < TotalDeep && !bShouldStop; BatchStart += DeepBatchSize)
		{
			// Check for cancellation from notification
			if (Owner->TaskNotification && Owner->TaskNotification->GetPromptAction() == EAsyncTaskNotificationPromptAction::Cancel)
			{
				bShouldStop = true;
				break;
			}

			// Memory budget check - throttle if over budget
			if (FMonolithMemoryHelper::ShouldThrottle(MemoryBudgetMB))
			{
				UE_LOG(LogMonolithIndex, Log, TEXT("Memory budget exceeded, forcing GC and yielding..."));
				
				FEvent* GCEvent = FPlatformProcess::GetSynchEventFromPool(true);
				AsyncTask(ENamedThreads::GameThread, [GCEvent, YieldTime]()
				{
					FMonolithMemoryHelper::ForceGarbageCollection(true);
					FMonolithMemoryHelper::YieldToEditor();
					if (YieldTime > 0.0f)
					{
						FPlatformProcess::Sleep(YieldTime);
					}
					GCEvent->Trigger();
				});
				GCEvent->Wait();
				FPlatformProcess::ReturnSynchEventToPool(GCEvent);

				if (bLogMemory)
				{
					FMonolithMemoryHelper::LogMemoryStats(TEXT("After throttle GC"));
				}
			}

			// Check for critical memory situation
			if (FMonolithMemoryHelper::IsMemoryCritical())
			{
				UE_LOG(LogMonolithIndex, Warning, TEXT("Critical memory situation detected (<2GB available). Pausing indexing..."));
				
				FEvent* CriticalGCEvent = FPlatformProcess::GetSynchEventFromPool(true);
				AsyncTask(ENamedThreads::GameThread, [CriticalGCEvent]()
				{
					FMonolithMemoryHelper::ForceGarbageCollection(true);
					FPlatformProcess::Sleep(1.0f); // Longer yield for critical situation
					CriticalGCEvent->Trigger();
				});
				CriticalGCEvent->Wait();
				FPlatformProcess::ReturnSynchEventToPool(CriticalGCEvent);
			}

			int32 BatchEnd = FMath::Min(BatchStart + DeepBatchSize, TotalDeep);

			// Capture the slice for this batch
			TArray<FDeepIndexEntry> BatchSlice;
			TArray<int64> BatchAssetIds;
			BatchSlice.Reserve(BatchEnd - BatchStart);
			BatchAssetIds.Reserve(BatchEnd - BatchStart);
			for (int32 j = BatchStart; j < BatchEnd; ++j)
			{
				BatchSlice.Add(DeepIndexQueue[j]);
				BatchAssetIds.Add(DeepIndexQueue[j].AssetId);
			}

			// ------------------------------------------------------------------
			// Poison-pill attempt marker — its OWN transaction, committed BEFORE
			// the batch work transaction opens.
			//
			// This ordering is the entire mechanism. A write inside an open
			// transaction is not durable: under `journal_mode=DELETE` a process
			// death inside the work transaction below makes SQLite roll that whole
			// transaction back on next open, which would take the marker with it.
			// The counter would read 0 on resume, the same batch would re-queue,
			// and the asset that killed the editor would kill it again — forever.
			// The frame-budget commit inside the work lambda does not rescue it
			// either: it only fires after at least one asset has been processed, so
			// it never covers the FIRST asset of a batch, which is exactly where a
			// resumed queue puts the poison one.
			//
			// Cost is one extra commit per BATCH (~6k on a 50k-asset project), not
			// per asset — per-asset transactions stay off the table.
			// ------------------------------------------------------------------
			if (RequireTransaction(DB->BeginTransaction(), TEXT("deep attempt-marker begin")))
			{
				if (!DB->BumpDeepIndexAttempts(BatchAssetIds))
				{
					UE_LOG(LogMonolithIndex, Warning,
						TEXT("Could not record deep-index attempts for batch %d — a crash in this batch would not be counted"),
						BatchNumber);
				}
				RequireTransaction(DB->CommitTransaction(), TEXT("deep attempt-marker commit"));
			}

			FEvent* BatchEvent = FPlatformProcess::GetSynchEventFromPool(true);

			// CRITICAL: Dispatch via FTSTicker (not AsyncTask(GT)) so our work only
			// fires once the asset compiler reports idle (GetNumRemainingAssets() == 0).
			// AsyncTask(GT) can be drained inside FTextureCompilingManager::PostCompilation's
			// bIsRoutingPostCompilation guard, and any asset load from there would fatal
			// in FinishAllCompilation (TextureCompiler.cpp:454). The previous fix
			// (calling FinishAllCompilation inside the lambda) was the exact trigger —
			// see GitHub issue #19, regression from commit 168c087.
			FMonolithCompilerSafeDispatch::RunOnGameThreadWhenCompilerIdle(
				[DB, BatchSlice = MoveTemp(BatchSlice), &DeepIndexed, &DeepErrors, &bTransactionFailure, FrameBudgetSeconds]()
			{
				if (!DB->BeginTransaction())
				{
					bTransactionFailure = true;
					UE_LOG(LogMonolithIndex, Error, TEXT("Index transaction failure (deep batch begin) — this run will not be marked complete"));
					return;
				}
				double BatchStartTime = FPlatformTime::Seconds();

				for (const FDeepIndexEntry& Entry : BatchSlice)
				{
					// Load asset on game thread — the dispatcher guarantees the asset
					// compiler is idle before this runs, so GetAsset() won't reenter
					// the texture compiler's PostCompilation guard.
					// Capture residency BEFORE GetAsset() may load it (issue #81).
					const bool bWasLoaded = Entry.AssetData.IsAssetLoaded();
					UObject* LoadedAsset = Entry.AssetData.GetAsset();
					if (LoadedAsset)
					{
						if (Entry.Indexer->IndexAsset(Entry.AssetData, LoadedAsset, *DB, Entry.AssetId))
						{
							DeepIndexed++;

							// Checkpoint INSIDE the transaction that carries this
							// asset's child rows, so the two are atomic: a rollback
							// loses both, never a checkpoint pointing at data that
							// is not there.
							DB->SetDeepIndexedHash(Entry.AssetId, Entry.SavedHash);
							DB->ClearDeepIndexAttempts(Entry.AssetId);
						}
						else
						{
							DeepErrors++;
							UE_LOG(LogMonolithIndex, Warning, TEXT("Deep indexer '%s' failed for: %s"),
								*Entry.Indexer->GetName(),
								*Entry.AssetData.PackageName.ToString());
						}

						// Mark asset for unloading to help GC
						FMonolithMemoryHelper::TryUnloadPackage(LoadedAsset, bWasLoaded);
					}
					else
					{
						DeepErrors++;
						UE_LOG(LogMonolithIndex, Warning, TEXT("Failed to load asset for deep indexing: %s (class: %s)"),
							*Entry.AssetData.PackageName.ToString(),
							*Entry.AssetData.AssetClassPath.GetAssetName().ToString());
					}

					// If we've exceeded our frame budget, commit what we have and yield
					double Elapsed = FPlatformTime::Seconds() - BatchStartTime;
					if (Elapsed > FrameBudgetSeconds)
					{
						DB->CommitTransaction();
						FMonolithMemoryHelper::YieldToEditor();
						DB->BeginTransaction();
						BatchStartTime = FPlatformTime::Seconds();
					}
				}

				if (!DB->CommitTransaction())
				{
					bTransactionFailure = true;
					UE_LOG(LogMonolithIndex, Error, TEXT("Index transaction failure (deep batch commit) — this run will not be marked complete"));
				}
			},
			BatchEvent);

			BatchEvent->Wait();
			FPlatformProcess::ReturnSynchEventToPool(BatchEvent);

			BatchNumber++;

			// Periodic GC based on configured frequency
			if (BatchNumber % GCFrequency == 0)
			{
				FEvent* PeriodicGCEvent = FPlatformProcess::GetSynchEventFromPool(true);
				AsyncTask(ENamedThreads::GameThread, [PeriodicGCEvent]()
				{
					FMonolithMemoryHelper::ForceGarbageCollection(false);
					FMonolithMemoryHelper::YieldToEditor();
					PeriodicGCEvent->Trigger();
				});
				PeriodicGCEvent->Wait();
				FPlatformProcess::ReturnSynchEventToPool(PeriodicGCEvent);
			}

			// Update progress — report deep pass as second half of overall progress
			CurrentIndex = Indexed + BatchEnd;
			TotalAssets = Indexed + TotalDeep;

			if (Owner->TaskNotification)
			{
				Owner->TaskNotification->SetProgressText(FText::FromString(
					FString::Printf(TEXT("Deep indexing %d / %d assets..."), BatchEnd, TotalDeep)));
			}

			// Snapshot the counters here, on the worker. Loading them inside the
			// lambda would read them through a task object that may already be gone.
			const int32 ProgressCurrent = CurrentIndex.Load();
			const int32 ProgressTotal = TotalAssets.Load();
			AsyncTask(ENamedThreads::GameThread, [WeakOwner, ProgressCurrent, ProgressTotal]()
			{
				if (UMonolithIndexSubsystem* Subsystem = WeakOwner.Get())
				{
					Subsystem->OnProgress.Broadcast(ProgressCurrent, ProgressTotal);
				}
			});

			// Log progress and memory periodically
			if (BatchNumber % 10 == 0)
			{
				UE_LOG(LogMonolithIndex, Log, TEXT("Deep indexed %d / %d assets (%d ok, %d errors)"),
					BatchEnd, TotalDeep, DeepIndexed.Load(), DeepErrors.Load());

				if (bLogMemory)
				{
					FMonolithMemoryHelper::LogMemoryStats(FString::Printf(TEXT("After batch %d"), BatchNumber));
				}
			}
		}

		// Final GC after deep indexing
		FEvent* FinalGCEvent = FPlatformProcess::GetSynchEventFromPool(true);
		AsyncTask(ENamedThreads::GameThread, [FinalGCEvent]()
		{
			FMonolithMemoryHelper::ForceGarbageCollection(true);
			FinalGCEvent->Trigger();
		});
		FinalGCEvent->Wait();
		FPlatformProcess::ReturnSynchEventToPool(FinalGCEvent);

		UE_LOG(LogMonolithIndex, Log, TEXT("Deep indexing complete: %d indexed, %d errors"),
			DeepIndexed.Load(), DeepErrors.Load());

		if (bLogMemory)
		{
			FMonolithMemoryHelper::LogMemoryStats(TEXT("Deep index complete"));
		}
	}

	// Build indexed paths list for post-pass indexers
	TArray<FName> IndexedPaths;
	IndexedPaths.Add(FName(TEXT("/Game")));
	for (const FIndexedPluginInfo& PluginInfo : PluginsToIndex)
	{
		FString CleanPath = PluginInfo.MountPath;
		if (CleanPath.EndsWith(TEXT("/")))
		{
			CleanPath.LeftChopInline(1);
		}
		IndexedPaths.Add(FName(*CleanPath));
	}
	// Add user-configured additional content paths
	{
		const UMonolithSettings* Settings = GetDefault<UMonolithSettings>();
		if (Settings)
		{
			for (const FString& CustomPath : Settings->AdditionalContentPaths)
			{
				if (!CustomPath.IsEmpty())
				{
					FString CleanPath = CustomPath;
					if (CleanPath.EndsWith(TEXT("/")))
					{
						CleanPath.LeftChopInline(1);
					}
					IndexedPaths.AddUnique(FName(*CleanPath));
				}
			}
		}
	}

	// Helper lambda to run GC and yield between indexers
	auto GCBetweenIndexers = [this]()
	{
		FEvent* GCEvent = FPlatformProcess::GetSynchEventFromPool(true);
		AsyncTask(ENamedThreads::GameThread, [GCEvent]()
		{
			FMonolithMemoryHelper::ForceGarbageCollection(true);
			FMonolithMemoryHelper::YieldToEditor();
			GCEvent->Trigger();
		});
		GCEvent->Wait();
		FPlatformProcess::ReturnSynchEventToPool(GCEvent);
	};

	// Every post-pass sentinel has the same shape: one transaction wrapping one
	// indexer call. A transaction failure here means the pass could not run at
	// all, which is structural — it feeds the completion gate. The indexer's own
	// per-asset failures do not.
	auto RunSentinelInTransaction = [&bTransactionFailure](FMonolithIndexDatabase* InDB, const TSharedPtr<IMonolithIndexer>& InIndexer)
	{
		if (!InDB->BeginTransaction())
		{
			bTransactionFailure = true;
			UE_LOG(LogMonolithIndex, Error, TEXT("Index transaction failure (post-pass begin) — this run will not be marked complete"));
			return;
		}

		FAssetData DummyData;
		InIndexer->IndexAsset(DummyData, nullptr, *InDB, 0);

		if (!InDB->CommitTransaction())
		{
			bTransactionFailure = true;
			UE_LOG(LogMonolithIndex, Error, TEXT("Index transaction failure (post-pass commit) — this run will not be marked complete"));
		}
	};

	// Helper to check for cancellation
	auto CheckCancellation = [this]() -> bool
	{
		if (bShouldStop) return true;
		if (Owner->TaskNotification && Owner->TaskNotification->GetPromptAction() == EAsyncTaskNotificationPromptAction::Cancel)
		{
			bShouldStop = true;
			return true;
		}
		return false;
	};

	UE_LOG(LogMonolithIndex, Log, TEXT("Starting post-pass indexers..."));

	// Run dependency indexer on game thread (Asset Registry requires it)
	if (!CheckCancellation())
	{
		Owner->IndexingStatusMessage = TEXT("Analyzing dependencies...");
		TSharedPtr<IMonolithIndexer>* DepIndexer = Owner->ClassToIndexer.Find(TEXT("__Dependencies__"));
		if (DepIndexer && DepIndexer->IsValid())
		{
			double SentinelStart = FPlatformTime::Seconds();
			UE_LOG(LogMonolithIndex, Log, TEXT("Running dependency indexer..."));
			TSharedPtr<IMonolithIndexer> DepIndexerCopy = *DepIndexer;
			if (FDependencyIndexer* DepRaw = static_cast<FDependencyIndexer*>(DepIndexerCopy.Get()))
			{
				DepRaw->SetIndexedPaths(IndexedPaths);
			}
			FEvent* DepEvent = FPlatformProcess::GetSynchEventFromPool(true);
			FMonolithCompilerSafeDispatch::RunOnGameThreadWhenCompilerIdle(
				[DB, DepIndexerCopy, &RunSentinelInTransaction]()
			{
				RunSentinelInTransaction(DB, DepIndexerCopy);
			},
			DepEvent);
			DepEvent->Wait();
			FPlatformProcess::ReturnSynchEventToPool(DepEvent);
			UE_LOG(LogMonolithIndex, Log, TEXT("Dependency indexer completed in %.2fs"), FPlatformTime::Seconds() - SentinelStart);
			GCBetweenIndexers();
		}
	}

	// Run level indexer on game thread (asset loading requires it)
	if (!CheckCancellation())
	{
		Owner->IndexingStatusMessage = TEXT("Indexing level actors...");
		TSharedPtr<IMonolithIndexer>* LevelIndexer = Owner->ClassToIndexer.Find(TEXT("__Levels__"));
		if (LevelIndexer && LevelIndexer->IsValid())
		{
			double SentinelStart = FPlatformTime::Seconds();
			UE_LOG(LogMonolithIndex, Log, TEXT("Running level indexer..."));
			TSharedPtr<IMonolithIndexer> LevelIndexerCopy = *LevelIndexer;
			if (FLevelIndexer* LevelRaw = static_cast<FLevelIndexer*>(LevelIndexerCopy.Get()))
			{
				LevelRaw->SetIndexedPaths(IndexedPaths);
			}
			FEvent* LevelEvent = FPlatformProcess::GetSynchEventFromPool(true);
			FMonolithCompilerSafeDispatch::RunOnGameThreadWhenCompilerIdle(
				[DB, LevelIndexerCopy, &RunSentinelInTransaction]()
			{
				RunSentinelInTransaction(DB, LevelIndexerCopy);
			},
			LevelEvent);
			LevelEvent->Wait();
			FPlatformProcess::ReturnSynchEventToPool(LevelEvent);
			UE_LOG(LogMonolithIndex, Log, TEXT("Level indexer completed in %.2fs"), FPlatformTime::Seconds() - SentinelStart);
			GCBetweenIndexers();
		}
	}

	// Run DataTable indexer on game thread (requires asset loading)
	if (!CheckCancellation())
	{
		Owner->IndexingStatusMessage = TEXT("Indexing DataTable rows...");
		TSharedPtr<IMonolithIndexer>* DTIndexer = Owner->ClassToIndexer.Find(TEXT("__DataTables__"));
		if (DTIndexer && DTIndexer->IsValid())
		{
			double SentinelStart = FPlatformTime::Seconds();
			UE_LOG(LogMonolithIndex, Log, TEXT("Running DataTable indexer..."));
			TSharedPtr<IMonolithIndexer> DTIndexerCopy = *DTIndexer;
			FEvent* DTEvent = FPlatformProcess::GetSynchEventFromPool(true);
			FMonolithCompilerSafeDispatch::RunOnGameThreadWhenCompilerIdle(
				[DB, DTIndexerCopy, &RunSentinelInTransaction]()
			{
				RunSentinelInTransaction(DB, DTIndexerCopy);
			},
			DTEvent);
			DTEvent->Wait();
			FPlatformProcess::ReturnSynchEventToPool(DTEvent);
			UE_LOG(LogMonolithIndex, Log, TEXT("DataTable indexer completed in %.2fs"), FPlatformTime::Seconds() - SentinelStart);
			GCBetweenIndexers();
		}
	}

	// Run config indexer (file I/O only, no game thread needed)
	if (!CheckCancellation())
	{
		Owner->IndexingStatusMessage = TEXT("Indexing config files...");
		TSharedPtr<IMonolithIndexer>* CfgIndexer = Owner->ClassToIndexer.Find(TEXT("__Configs__"));
		if (CfgIndexer && CfgIndexer->IsValid())
		{
			double SentinelStart = FPlatformTime::Seconds();
			UE_LOG(LogMonolithIndex, Log, TEXT("Running config indexer..."));
			RunSentinelInTransaction(DB, *CfgIndexer);
			UE_LOG(LogMonolithIndex, Log, TEXT("Config indexer completed in %.2fs"), FPlatformTime::Seconds() - SentinelStart);
		}
	}

	// Run C++ symbol indexer (file I/O only, no game thread needed)
	if (!CheckCancellation())
	{
		Owner->IndexingStatusMessage = TEXT("Indexing C++ symbols...");
		TSharedPtr<IMonolithIndexer>* CppIndexer = Owner->ClassToIndexer.Find(TEXT("__CppSymbols__"));
		if (CppIndexer && CppIndexer->IsValid())
		{
			double SentinelStart = FPlatformTime::Seconds();
			UE_LOG(LogMonolithIndex, Log, TEXT("Running C++ symbol indexer..."));
			RunSentinelInTransaction(DB, *CppIndexer);
			UE_LOG(LogMonolithIndex, Log, TEXT("C++ symbol indexer completed in %.2fs"), FPlatformTime::Seconds() - SentinelStart);
		}
	}

	// Run animation indexer on game thread (asset loading requires it)
	if (!CheckCancellation())
	{
		Owner->IndexingStatusMessage = TEXT("Indexing animations...");
		TSharedPtr<IMonolithIndexer>* AnimIndexer = Owner->ClassToIndexer.Find(TEXT("__Animations__"));
		if (AnimIndexer && AnimIndexer->IsValid())
		{
			double SentinelStart = FPlatformTime::Seconds();
			UE_LOG(LogMonolithIndex, Log, TEXT("Running animation indexer..."));
			TSharedPtr<IMonolithIndexer> AnimIndexerCopy = *AnimIndexer;
			FEvent* AnimEvent = FPlatformProcess::GetSynchEventFromPool(true);
			// No transaction here: FAnimationIndexer owns its own, one per batch.
			// The animation pass walks thousands of assets, and a single
			// transaction spanning all of them meant a crash discarded the whole
			// pass. It keeps the single dispatch and its batch structure — per-asset
			// dispatch would cost a game-thread frame per animation asset.
			FMonolithCompilerSafeDispatch::RunOnGameThreadWhenCompilerIdle(
				[DB, AnimIndexerCopy]()
			{
				FAssetData DummyData;
				AnimIndexerCopy->IndexAsset(DummyData, nullptr, *DB, 0);
			},
			AnimEvent);
			AnimEvent->Wait();
			FPlatformProcess::ReturnSynchEventToPool(AnimEvent);
			UE_LOG(LogMonolithIndex, Log, TEXT("Animation indexer completed in %.2fs"), FPlatformTime::Seconds() - SentinelStart);
			GCBetweenIndexers();
		}
	}

	// Run gameplay tag indexer on game thread (GameplayTagsManager requires it)
	if (!CheckCancellation())
	{
		Owner->IndexingStatusMessage = TEXT("Indexing gameplay tags...");
		TSharedPtr<IMonolithIndexer>* TagIndexer = Owner->ClassToIndexer.Find(TEXT("__GameplayTags__"));
		if (TagIndexer && TagIndexer->IsValid())
		{
			double SentinelStart = FPlatformTime::Seconds();
			UE_LOG(LogMonolithIndex, Log, TEXT("Running gameplay tag indexer..."));
			TSharedPtr<IMonolithIndexer> TagIndexerCopy = *TagIndexer;
			FEvent* TagEvent = FPlatformProcess::GetSynchEventFromPool(true);
			FMonolithCompilerSafeDispatch::RunOnGameThreadWhenCompilerIdle(
				[DB, TagIndexerCopy, &RunSentinelInTransaction]()
			{
				RunSentinelInTransaction(DB, TagIndexerCopy);
			},
			TagEvent);
			TagEvent->Wait();
			FPlatformProcess::ReturnSynchEventToPool(TagEvent);
			UE_LOG(LogMonolithIndex, Log, TEXT("Gameplay tag indexer completed in %.2fs"), FPlatformTime::Seconds() - SentinelStart);
			GCBetweenIndexers();
		}
	}

	// Run Niagara indexer on game thread (requires asset loading)
	if (!CheckCancellation())
	{
		Owner->IndexingStatusMessage = TEXT("Indexing Niagara systems...");
		TSharedPtr<IMonolithIndexer>* NiagaraIndexerPtr = Owner->ClassToIndexer.Find(TEXT("__Niagara__"));
		if (NiagaraIndexerPtr && NiagaraIndexerPtr->IsValid())
		{
			double SentinelStart = FPlatformTime::Seconds();
			UE_LOG(LogMonolithIndex, Log, TEXT("Running Niagara indexer..."));
			TSharedPtr<IMonolithIndexer> NiagaraIndexerCopy = *NiagaraIndexerPtr;
			FEvent* NiagaraEvent = FPlatformProcess::GetSynchEventFromPool(true);
			FMonolithCompilerSafeDispatch::RunOnGameThreadWhenCompilerIdle(
				[DB, NiagaraIndexerCopy, &RunSentinelInTransaction]()
			{
				RunSentinelInTransaction(DB, NiagaraIndexerCopy);
			},
			NiagaraEvent);
			NiagaraEvent->Wait();
			FPlatformProcess::ReturnSynchEventToPool(NiagaraEvent);
			UE_LOG(LogMonolithIndex, Log, TEXT("Niagara indexer completed in %.2fs"), FPlatformTime::Seconds() - SentinelStart);
			GCBetweenIndexers();
		}
	}

	// Run mesh catalog indexer on game thread (requires asset loading)
	if (!CheckCancellation())
	{
		Owner->IndexingStatusMessage = TEXT("Building mesh catalog...");
		TSharedPtr<IMonolithIndexer>* MeshCatIndexer = Owner->ClassToIndexer.Find(TEXT("__MeshCatalog__"));
		if (MeshCatIndexer && MeshCatIndexer->IsValid())
		{
			double SentinelStart = FPlatformTime::Seconds();
			UE_LOG(LogMonolithIndex, Log, TEXT("Running mesh catalog indexer..."));
			TSharedPtr<IMonolithIndexer> MeshCatIndexerCopy = *MeshCatIndexer;
			if (FMeshCatalogIndexer* MeshCatRaw = static_cast<FMeshCatalogIndexer*>(MeshCatIndexerCopy.Get()))
			{
				MeshCatRaw->SetIndexedPaths(IndexedPaths);
			}
			FEvent* MeshCatEvent = FPlatformProcess::GetSynchEventFromPool(true);
			FMonolithCompilerSafeDispatch::RunOnGameThreadWhenCompilerIdle(
				[DB, MeshCatIndexerCopy, &RunSentinelInTransaction]()
			{
				RunSentinelInTransaction(DB, MeshCatIndexerCopy);
			},
			MeshCatEvent);
			MeshCatEvent->Wait();
			FPlatformProcess::ReturnSynchEventToPool(MeshCatEvent);
			UE_LOG(LogMonolithIndex, Log, TEXT("Mesh catalog indexer completed in %.2fs"), FPlatformTime::Seconds() - SentinelStart);
			GCBetweenIndexers();
		}
	}

	UE_LOG(LogMonolithIndex, Log, TEXT("Post-pass indexers complete"));

	// Mark the run complete. The gate is STRUCTURAL only — cancellation and
	// transaction/DB failures. Per-asset errors are deliberately not gates: they
	// fire on any real project (a class from a disabled plugin, a redirector stub,
	// an animation asset that crashes on load) and a fail-closed gate would mean
	// one bad asset re-runs a full wipe-and-rebuild on every launch, forever.
	//
	// The old `Indexed < 500` guard is gone with it: it made every project with
	// fewer than 500 assets re-index from scratch on every single launch.
	const bool bStructurallyComplete = !bShouldStop.Load() && !bTransactionFailure.Load();
	if (bStructurallyComplete)
	{
		// Writes last_full_index and clears the in-progress marker in one
		// transaction — the only place either happens.
		if (DB->CompleteFullIndex(FDateTime::UtcNow().ToString()))
		{
			UE_LOG(LogMonolithIndex, Log, TEXT("Full index complete (%d assets indexed, %d errors)"), Indexed, Errors);
		}
		else
		{
			UE_LOG(LogMonolithIndex, Error,
				TEXT("Index finished but the completion marker could not be written — the next launch will resume this index"));
		}
	}
	else
	{
		UE_LOG(LogMonolithIndex, Warning,
			TEXT("Index did not complete (%s) — progress is checkpointed and the next launch will resume it"),
			bShouldStop.Load() ? TEXT("cancelled") : TEXT("transaction failure"));
	}

	if (bLogMemory)
	{
		FMonolithMemoryHelper::LogMemoryStats(TEXT("Full index complete"));
	}

	AsyncTask(ENamedThreads::GameThread, [WeakOwner, bStructurallyComplete]()
	{
		if (UMonolithIndexSubsystem* Subsystem = WeakOwner.Get())
		{
			Subsystem->OnIndexingFinished(bStructurallyComplete);
		}
	});

	return 0;
}

void UMonolithIndexSubsystem::OnIndexingFinished(bool bSuccess)
{
	bIsIndexing = false;
	IndexingStatusMessage.Empty();

	// Restore the incremental-reachability GC setting captured at run start. This
	// is the single completion point for ALL worker exit paths (normal, DB-open
	// failure, and bShouldStop cancel/abort), so the dtor always runs here.
	GIncrementalGCOverride.Reset();

	if (IndexingThread)
	{
		IndexingThread->WaitForCompletion();
		IndexingThread.Reset();
	}

	IndexingTaskPtr.Reset();

	if (TaskNotification)
	{
		TaskNotification->SetComplete(
			FText::FromString(TEXT("Monolith")),
			FText::FromString(bSuccess ? TEXT("Project indexing complete") : TEXT("Project indexing failed")),
			bSuccess);
		TaskNotification.Reset();
	}

	// Re-arm live tracking on EVERY outcome, not just success. A failed or
	// cancelled run used to leave the callbacks detached, so the subsystem
	// reported itself active while silently dropping every subsequent asset
	// change until a successful reindex or an editor restart.
	RegisterLiveCallbacks();

	OnComplete.Broadcast(bSuccess);
	OnProgress.Clear();

	UE_LOG(LogMonolithIndex, Log, TEXT("Indexing %s"),
		bSuccess ? TEXT("completed successfully") : TEXT("failed or was cancelled"));
}

FString UMonolithIndexSubsystem::GetDatabasePath() const
{
	TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("Monolith"));
	if (Plugin.IsValid())
	{
		return Plugin->GetBaseDir() / TEXT("Saved") / TEXT("ProjectIndex.db");
	}
	return FPaths::ProjectPluginsDir() / TEXT("Monolith") / TEXT("Saved") / TEXT("ProjectIndex.db");
}

bool UMonolithIndexSubsystem::ShouldAutoIndex() const
{
	if (!Database.IsValid() || !Database->IsOpen()) return false;

	FSQLiteDatabase* RawDB = Database->GetRawDatabase();
	if (!RawDB) return false;

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*RawDB, TEXT("SELECT value FROM meta WHERE key = 'last_full_index';"));
	if (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		return false; // Already indexed before
	}
	return true;
}

bool UMonolithIndexSubsystem::CanDoIncrementalIndex() const
{
	if (!Database || !Database->IsOpen()) return false;
	FString SchemaVersion = Database->ReadMeta(TEXT("schema_version"));
	if (SchemaVersion.IsEmpty() || FCString::Atoi(*SchemaVersion) < 2)
		return false;
	FString LastFullIndex = Database->ReadMeta(TEXT("last_full_index"));
	if (LastFullIndex.IsEmpty())
		return false;
	return true;
}

bool UMonolithIndexSubsystem::StartIncrementalIndex()
{
	check(IsInGameThread());
	if (bIsIndexing)
	{
		UE_LOG(LogMonolithIndex, Warning, TEXT("Indexing already in progress"));
		return false;
	}
	if (!Database.IsValid() || !Database->IsOpen())
	{
		UE_LOG(LogMonolithIndex, Warning, TEXT("Index database is not open — cannot start an incremental index"));
		return false;
	}
	bIsIndexing = true;
	UnregisterLiveCallbacks();

	IndexedPlugins = GatherMarketplacePluginPaths();

	UE_LOG(LogMonolithIndex, Log, TEXT("Starting incremental index..."));

	// PHASE 1: Build current AR state
	TSet<FName> CurrentPackages;
	TMap<FName, FIoHash> CurrentHashes;
	IAssetRegistry& AR = IAssetRegistry::GetChecked();

	TSet<FString> ValidPrefixes;
	ValidPrefixes.Add(TEXT("/Game/"));
	for (const FIndexedPluginInfo& Plugin : IndexedPlugins)
	{
		ValidPrefixes.Add(Plugin.MountPath);
	}
	// Include AdditionalContentPaths from settings
	if (const UMonolithSettings* Settings = GetDefault<UMonolithSettings>())
	{
		for (const FString& CustomPath : Settings->AdditionalContentPaths)
		{
			if (!CustomPath.IsEmpty())
				ValidPrefixes.Add(CustomPath);
		}
	}

	AR.EnumerateAllPackages([&](FName PackageName, const FAssetPackageData& PkgData)
	{
		FString PkgStr = PackageName.ToString();
		for (const FString& Prefix : ValidPrefixes)
		{
			if (PkgStr.StartsWith(Prefix))
			{
				CurrentPackages.Add(PackageName);
				CurrentHashes.Add(PackageName, PkgData.GetPackageSavedHash());
				break;
			}
		}
	});

	// PHASE 2: Build DB state
	TMap<FString, FString> DBPathsAndHashes = Database->GetAllPathsAndHashes();
	TSet<FName> DBPackages;
	TMap<FName, FIoHash> DBHashes;
	for (const auto& [Path, Hash] : DBPathsAndHashes)
	{
		FName PathName(*Path);
		DBPackages.Add(PathName);
		if (!Hash.IsEmpty())
		{
			FIoHash IoHash;
			LexFromString(IoHash, *Hash);
			DBHashes.Add(PathName, IoHash);
		}
	}

	// PHASE 3: Compute deltas
	TArray<FName> AddedPaths, DeletedPaths, ExistingPaths;
	for (FName Pkg : CurrentPackages)
	{
		if (!DBPackages.Contains(Pkg)) AddedPaths.Add(Pkg);
		else ExistingPaths.Add(Pkg);
	}
	for (FName Pkg : DBPackages)
	{
		if (!CurrentPackages.Contains(Pkg)) DeletedPaths.Add(Pkg);
	}

	// PHASE 4: Move detection
	TMultiMap<FIoHash, FName> DeletedHashMap;
	for (FName Deleted : DeletedPaths)
	{
		if (FIoHash* Hash = DBHashes.Find(Deleted))
		{
			if (!Hash->IsZero()) DeletedHashMap.Add(*Hash, Deleted);
		}
	}

	TArray<TPair<FName, FName>> Moves;
	TArray<FName> TrueAdds;
	for (FName Added : AddedPaths)
	{
		FIoHash* NewHash = CurrentHashes.Find(Added);
		if (NewHash && !NewHash->IsZero())
		{
			// TMultiMap::RemoveSingle(Key, Value) requires BOTH to match.
			// Must MultiFind first, then RemoveSingle with the found value.
			TArray<FName> FoundOldPaths;
			DeletedHashMap.MultiFind(*NewHash, FoundOldPaths);
			if (FoundOldPaths.Num() > 0)
			{
				FName MatchedOldPath = FoundOldPaths[0];
				DeletedHashMap.RemoveSingle(*NewHash, MatchedOldPath);
				Moves.Add({MatchedOldPath, Added});
				continue;
			}
		}
		TrueAdds.Add(Added);
	}

	TSet<FName> MovedOldPaths;
	for (const auto& [OldPath, NewPath] : Moves) MovedOldPaths.Add(OldPath);

	TArray<FName> TrueDeletes;
	for (FName Deleted : DeletedPaths)
	{
		if (!MovedOldPaths.Contains(Deleted)) TrueDeletes.Add(Deleted);
	}

	// PHASE 5: Modification detection
	TArray<FName> ModifiedPaths;
	for (FName Existing : ExistingPaths)
	{
		FIoHash* CurrentHash = CurrentHashes.Find(Existing);
		FIoHash* StoredHash = DBHashes.Find(Existing);
		if (CurrentHash && StoredHash && *CurrentHash != *StoredHash)
			ModifiedPaths.Add(Existing);
		else if (CurrentHash && !StoredHash)
			ModifiedPaths.Add(Existing);  // Pre-v2 asset with no stored hash
	}

	UE_LOG(LogMonolithIndex, Log,
		TEXT("Incremental delta: %d added, %d deleted, %d moved, %d modified, %d unchanged"),
		TrueAdds.Num(), TrueDeletes.Num(), Moves.Num(), ModifiedPaths.Num(),
		ExistingPaths.Num() - ModifiedPaths.Num());

	// Early return if no changes
	if (TrueDeletes.Num() == 0 && TrueAdds.Num() == 0 && Moves.Num() == 0 && ModifiedPaths.Num() == 0)
	{
		UE_LOG(LogMonolithIndex, Log, TEXT("No changes detected. Incremental index complete."));
		bIsIndexing = false;
		RegisterLiveCallbacks();
		return true;
	}

	// PHASE 6: Apply deltas
	Database->BeginTransaction();

	// 6a: Deletions
	for (FName Path : TrueDeletes)
		Database->DeleteAssetByPath(Path.ToString());

	// 6b: Moves
	for (const auto& [OldPath, NewPath] : Moves)
	{
		Database->UpdateAssetPath(OldPath.ToString(), NewPath.ToString());
		if (FIoHash* Hash = CurrentHashes.Find(NewPath))
			Database->UpdateSavedHash(NewPath.ToString(), LexToString(*Hash));
	}

	// 6c: Build paths needing (re-)indexing
	TSet<FName> PathsToIndex;
	for (FName Path : TrueAdds) PathsToIndex.Add(Path);
	for (FName Path : ModifiedPaths) PathsToIndex.Add(Path);
	for (const auto& [OldPath, NewPath] : Moves)
	{
		FIoHash* CurrentHash = CurrentHashes.Find(NewPath);
		FIoHash* StoredHash = DBHashes.Find(OldPath);
		if (CurrentHash && StoredHash && *CurrentHash != *StoredHash)
			PathsToIndex.Add(NewPath);
	}

	// 6d: Insert/update asset metadata for paths needing indexing
	for (FName Path : PathsToIndex)
	{
		FString PathStr = Path.ToString();
		int64 AssetId = Database->GetAssetId(PathStr);

		// Build FIndexedAsset from AR
		TArray<FAssetData> Assets;
		AR.GetAssetsByPackageName(Path, Assets);
		if (Assets.Num() == 0) continue;

		const FAssetData& AssetData = Assets[0];
		FIndexedAsset IndexedAsset;
		IndexedAsset.PackagePath = PathStr;
		IndexedAsset.AssetName = AssetData.AssetName.ToString();
		IndexedAsset.AssetClass = AssetData.AssetClassPath.GetAssetName().ToString();

		// Determine module name (same logic as full index path)
		if (!IndexedAsset.PackagePath.StartsWith(TEXT("/Game/")))
		{
			for (const FIndexedPluginInfo& PluginInfo : IndexedPlugins)
			{
				if (IndexedAsset.PackagePath.StartsWith(PluginInfo.MountPath))
				{
					IndexedAsset.ModuleName = PluginInfo.PluginName;
					break;
				}
			}
			// Fallback: extract from path
			if (IndexedAsset.ModuleName.IsEmpty())
			{
				int32 SecondSlash = IndexedAsset.PackagePath.Find(TEXT("/"), ESearchCase::CaseSensitive, ESearchDir::FromStart, 1);
				if (SecondSlash > 1)
				{
					IndexedAsset.ModuleName = IndexedAsset.PackagePath.Mid(1, SecondSlash - 1);
				}
			}
		}

		// Populate LastModified
		FString PackageFilename;
		if (FPackageName::DoesPackageExist(PathStr, &PackageFilename))
		{
			FDateTime FileTime = IFileManager::Get().GetTimeStamp(*PackageFilename);
			IndexedAsset.LastModified = FileTime.ToIso8601();
		}
		// Don't populate SavedHash yet — deferred to Phase 10 for crash recovery

		if (AssetId > 0)
		{
			// Existing asset — update metadata, clear children
			Database->UpdateAssetMetadata(IndexedAsset);
			Database->DeleteChildDataForAsset(AssetId);
		}
		else
		{
			// New asset
			Database->InsertAsset(IndexedAsset);
		}
	}

	// PHASE 7: Deep-index
	TSet<FString> PathStrings;
	for (FName Path : PathsToIndex) PathStrings.Add(Path.ToString());
	ProcessDeepIndexQueue(PathStrings);

	// PHASE 8: Commit
	Database->CommitTransaction();

	// PHASE 9: Sentinels (stub — implemented in Task 6)
	// TSet<FString> RemovedPathStrings;
	// for (FName Path : TrueDeletes) RemovedPathStrings.Add(Path.ToString());
	// RunScopedSentinels(PathStrings, RemovedPathStrings);

	// PHASE 10: Update hashes (deferred for crash recovery)
	Database->BeginTransaction();
	for (FName Path : PathsToIndex)
	{
		if (FIoHash* Hash = CurrentHashes.Find(Path))
			Database->UpdateSavedHash(Path.ToString(), LexToString(*Hash));
	}
	Database->CommitTransaction();

	UE_LOG(LogMonolithIndex, Log, TEXT("Incremental index complete."));
	bIsIndexing = false;
	RegisterLiveCallbacks();
	return true;
}

// ============================================================
// Stubs for Tasks 5-6
// ============================================================

void UMonolithIndexSubsystem::ProcessDeepIndexQueue(const TSet<FString>& PathsToIndex)
{
	if (PathsToIndex.Num() == 0) return;

	IAssetRegistry& AR = IAssetRegistry::GetChecked();
	int32 Indexed = 0;

	for (const FString& PackagePath : PathsToIndex)
	{
		TArray<FAssetData> Assets;
		AR.GetAssetsByPackageName(FName(*PackagePath), Assets);

		for (const FAssetData& AssetData : Assets)
		{
			FString ClassName = AssetData.AssetClassPath.GetAssetName().ToString();
			TSharedPtr<IMonolithIndexer>* Indexer = ClassToIndexer.Find(ClassName);
			if (!Indexer) continue;

			int64 AssetId = Database->GetAssetId(PackagePath);
			if (AssetId <= 0) continue;

			// Load the asset (must be game thread)
			UObject* LoadedAsset = AssetData.GetAsset();
			if (!LoadedAsset) continue;

			(*Indexer)->IndexAsset(AssetData, LoadedAsset, *Database, AssetId);
			++Indexed;
		}
	}

	UE_LOG(LogMonolithIndex, Log, TEXT("Deep-indexed %d assets from %d paths"), Indexed, PathsToIndex.Num());
}

void UMonolithIndexSubsystem::RunScopedSentinels(const TSet<FString>& ChangedPaths, const TSet<FString>& RemovedPaths)
{
	if (ChangedPaths.Num() == 0 && RemovedPaths.Num() == 0) return;

	for (const auto& Indexer : Indexers)
	{
		if (Indexer->IsSentinel() && Indexer->SupportsIncrementalIndex())
		{
			double StartTime = FPlatformTime::Seconds();
			Indexer->IndexScoped(ChangedPaths, RemovedPaths, *Database);
			double Duration = FPlatformTime::Seconds() - StartTime;
			UE_LOG(LogMonolithIndex, Log, TEXT("Scoped sentinel %s completed in %.2fs"), *Indexer->GetName(), Duration);
		}
	}
}

void UMonolithIndexSubsystem::RegisterLiveCallbacks()
{
	// An index run owns the database exclusively; arming the callbacks now would
	// queue changes the run is about to overwrite anyway.
	if (bIsIndexing)
	{
		return;
	}

	if (!Database.IsValid() || !Database->IsOpen())
	{
		return;
	}

	// Already armed. Re-registering would bind a second copy of every delegate
	// and overwrite the handles, leaving the first copies permanently attached.
	if (OnAssetsAddedHandle.IsValid() || OnAssetsRemovedHandle.IsValid()
		|| OnAssetRenamedHandle.IsValid() || OnAssetsUpdatedOnDiskHandle.IsValid())
	{
		return;
	}

	IAssetRegistry& AR = IAssetRegistry::GetChecked();

	OnAssetsAddedHandle = AR.OnAssetsAdded().AddUObject(this, &UMonolithIndexSubsystem::OnAssetsAddedCallback);
	OnAssetsRemovedHandle = AR.OnAssetsRemoved().AddUObject(this, &UMonolithIndexSubsystem::OnAssetsRemovedCallback);
	OnAssetRenamedHandle = AR.OnAssetRenamed().AddUObject(this, &UMonolithIndexSubsystem::OnAssetRenamedCallback);
	OnAssetsUpdatedOnDiskHandle = AR.OnAssetsUpdatedOnDisk().AddUObject(this, &UMonolithIndexSubsystem::OnAssetsUpdatedOnDiskCallback);

	if (GEditor)
	{
		GEditor->GetTimerManager()->SetTimer(
			LiveIndexTimerHandle,
			FTimerDelegate::CreateUObject(this, &UMonolithIndexSubsystem::ProcessPendingChanges),
			2.0f, /*bLoop=*/ true);
	}

	UE_LOG(LogMonolithIndex, Log, TEXT("Live index callbacks registered."));
}

void UMonolithIndexSubsystem::UnregisterLiveCallbacks()
{
	if (IAssetRegistry* AR = IAssetRegistry::Get())
	{
		AR->OnAssetsAdded().Remove(OnAssetsAddedHandle);
		AR->OnAssetsRemoved().Remove(OnAssetsRemovedHandle);
		AR->OnAssetRenamed().Remove(OnAssetRenamedHandle);
		AR->OnAssetsUpdatedOnDisk().Remove(OnAssetsUpdatedOnDiskHandle);
	}

	// Reset unconditionally, including when the Asset Registry has already gone
	// away. A retained handle reads as "still armed" to RegisterLiveCallbacks and
	// would block every future re-arm.
	OnAssetsAddedHandle.Reset();
	OnAssetsRemovedHandle.Reset();
	OnAssetRenamedHandle.Reset();
	OnAssetsUpdatedOnDiskHandle.Reset();

	if (GEditor)
	{
		GEditor->GetTimerManager()->ClearTimer(LiveIndexTimerHandle);
	}
}

// ============================================================
// Live AR callback handlers
// ============================================================

static bool IsRedirector(const FAssetData& AssetData)
{
	static const FTopLevelAssetPath RedirectorPath(TEXT("/Script/CoreUObject"), TEXT("ObjectRedirector"));
	return AssetData.AssetClassPath == RedirectorPath;
}

void UMonolithIndexSubsystem::OnAssetsAddedCallback(TConstArrayView<FAssetData> Assets)
{
	if (bIsIndexing) return;
	for (const FAssetData& AssetData : Assets)
	{
		if (!IsRedirector(AssetData))
			PendingChanges.Add({EIndexChangeType::Added, AssetData, {}});
	}
}

void UMonolithIndexSubsystem::OnAssetsRemovedCallback(TConstArrayView<FAssetData> Assets)
{
	if (bIsIndexing) return;
	for (const FAssetData& AssetData : Assets)
	{
		if (!IsRedirector(AssetData))
			PendingChanges.Add({EIndexChangeType::Removed, AssetData, {}});
	}
}

void UMonolithIndexSubsystem::OnAssetRenamedCallback(const FAssetData& AssetData, const FString& OldObjectPath)
{
	if (bIsIndexing) return;
	PendingChanges.Add({EIndexChangeType::Renamed, AssetData, OldObjectPath});
}

void UMonolithIndexSubsystem::OnAssetsUpdatedOnDiskCallback(TConstArrayView<FAssetData> Assets)
{
	if (bIsIndexing) return;
	for (const FAssetData& AssetData : Assets)
		PendingChanges.Add({EIndexChangeType::Updated, AssetData, {}});
}

void UMonolithIndexSubsystem::ProcessPendingChanges()
{
	if (PendingChanges.Num() == 0) return;

	TArray<FPendingIndexChange> RawChanges = MoveTemp(PendingChanges);
	PendingChanges.Reset();

	if (!Database || !Database->IsOpen()) return;

	// DEDUP: Collapse multiple changes to same path
	TMap<FName, int32> PathToLastIndex;
	TArray<FPendingIndexChange> LocalChanges;
	LocalChanges.Reserve(RawChanges.Num());

	for (int32 i = 0; i < RawChanges.Num(); ++i)
	{
		FName PkgName = RawChanges[i].AssetData.PackageName;
		if (int32* ExistingIdx = PathToLastIndex.Find(PkgName))
		{
			EIndexChangeType PrevType = LocalChanges[*ExistingIdx].Type;
			EIndexChangeType NewType = RawChanges[i].Type;

			if (PrevType == EIndexChangeType::Renamed && NewType == EIndexChangeType::Updated)
			{
				// Keep the rename
			}
			else if (PrevType == EIndexChangeType::Removed && NewType == EIndexChangeType::Added)
			{
				RawChanges[i].Type = EIndexChangeType::Updated;
				LocalChanges[*ExistingIdx] = MoveTemp(RawChanges[i]);
			}
			else
			{
				LocalChanges[*ExistingIdx] = MoveTemp(RawChanges[i]);
			}
		}
		else
		{
			PathToLastIndex.Add(PkgName, LocalChanges.Num());
			LocalChanges.Add(MoveTemp(RawChanges[i]));
		}
	}

	UE_LOG(LogMonolithIndex, Log, TEXT("Processing %d pending index changes (%d raw)"),
		LocalChanges.Num(), RawChanges.Num());

	Database->BeginTransaction();

	TSet<FString> PathsToDeepIndex;
	TSet<FString> RemovedPaths;

	for (const FPendingIndexChange& Change : LocalChanges)
	{
		switch (Change.Type)
		{
		case EIndexChangeType::Added:
		{
			FIndexedAsset IndexedAsset;
			IndexedAsset.PackagePath = Change.AssetData.PackageName.ToString();
			IndexedAsset.AssetName = Change.AssetData.AssetName.ToString();
			IndexedAsset.AssetClass = Change.AssetData.AssetClassPath.GetAssetName().ToString();
			// Module name resolution
			if (!IndexedAsset.PackagePath.StartsWith(TEXT("/Game/")))
			{
				for (const FIndexedPluginInfo& PluginInfo : IndexedPlugins)
				{
					if (IndexedAsset.PackagePath.StartsWith(PluginInfo.MountPath))
					{
						IndexedAsset.ModuleName = PluginInfo.PluginName;
						break;
					}
				}
				if (IndexedAsset.ModuleName.IsEmpty())
				{
					int32 SecondSlash = IndexedAsset.PackagePath.Find(TEXT("/"), ESearchCase::CaseSensitive, ESearchDir::FromStart, 1);
					if (SecondSlash > 1)
						IndexedAsset.ModuleName = IndexedAsset.PackagePath.Mid(1, SecondSlash - 1);
				}
			}

			Database->InsertAsset(IndexedAsset);

			FString ClassName = Change.AssetData.AssetClassPath.GetAssetName().ToString();
			if (ClassToIndexer.Contains(ClassName))
				PathsToDeepIndex.Add(IndexedAsset.PackagePath);
			break;
		}
		case EIndexChangeType::Updated:
		{
			FIndexedAsset IndexedAsset;
			IndexedAsset.PackagePath = Change.AssetData.PackageName.ToString();
			IndexedAsset.AssetName = Change.AssetData.AssetName.ToString();
			IndexedAsset.AssetClass = Change.AssetData.AssetClassPath.GetAssetName().ToString();
			// Module name resolution (same as Added)
			if (!IndexedAsset.PackagePath.StartsWith(TEXT("/Game/")))
			{
				for (const FIndexedPluginInfo& PluginInfo : IndexedPlugins)
				{
					if (IndexedAsset.PackagePath.StartsWith(PluginInfo.MountPath))
					{
						IndexedAsset.ModuleName = PluginInfo.PluginName;
						break;
					}
				}
				if (IndexedAsset.ModuleName.IsEmpty())
				{
					int32 SecondSlash = IndexedAsset.PackagePath.Find(TEXT("/"), ESearchCase::CaseSensitive, ESearchDir::FromStart, 1);
					if (SecondSlash > 1)
						IndexedAsset.ModuleName = IndexedAsset.PackagePath.Mid(1, SecondSlash - 1);
				}
			}

			int64 AssetId = Database->GetAssetId(IndexedAsset.PackagePath);
			if (AssetId > 0)
			{
				Database->UpdateAssetMetadata(IndexedAsset);
				Database->DeleteChildDataForAsset(AssetId);
			}
			else
			{
				Database->InsertAsset(IndexedAsset);
			}

			FString ClassName = Change.AssetData.AssetClassPath.GetAssetName().ToString();
			if (ClassToIndexer.Contains(ClassName))
				PathsToDeepIndex.Add(IndexedAsset.PackagePath);
			break;
		}
		case EIndexChangeType::Removed:
		{
			FString Path = Change.AssetData.PackageName.ToString();
			Database->DeleteAssetByPath(Path);
			RemovedPaths.Add(Path);
			break;
		}
		case EIndexChangeType::Renamed:
		{
			FString OldPackageName, OldAssetName;
			Change.OldObjectPath.Split(TEXT("."), &OldPackageName, &OldAssetName);
			FString NewPath = Change.AssetData.PackageName.ToString();
			FString NewAssetName = Change.AssetData.AssetName.ToString();

			if (Database->UpdateAssetPath(OldPackageName, NewPath, NewAssetName))
			{
				UE_LOG(LogMonolithIndex, Verbose, TEXT("Asset moved: %s -> %s"), *OldPackageName, *NewPath);
			}
			else
			{
				FIndexedAsset IndexedAsset;
				IndexedAsset.PackagePath = NewPath;
				IndexedAsset.AssetName = NewAssetName;
				IndexedAsset.AssetClass = Change.AssetData.AssetClassPath.GetAssetName().ToString();
				Database->InsertAsset(IndexedAsset);
				PathsToDeepIndex.Add(NewPath);
			}
			break;
		}
		}
	}

	// Deep-index within same transaction
	if (PathsToDeepIndex.Num() > 0)
		ProcessDeepIndexQueue(PathsToDeepIndex);

	Database->CommitTransaction();

	// Sentinels after commit (they manage own transactions)
	if (PathsToDeepIndex.Num() > 0 || RemovedPaths.Num() > 0)
		RunScopedSentinels(PathsToDeepIndex, RemovedPaths);
}
