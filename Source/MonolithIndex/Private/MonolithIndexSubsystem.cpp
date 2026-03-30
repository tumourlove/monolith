#include "MonolithIndexSubsystem.h"
#include "MonolithIndexDatabase.h"
#include "MonolithSettings.h"
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

void UMonolithIndexSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	Database = MakeUnique<FMonolithIndexDatabase>();
	FString DbPath = GetDatabasePath();

	if (!Database->Open(DbPath))
	{
		UE_LOG(LogMonolithIndex, Error, TEXT("Failed to open index database at %s"), *DbPath);
		return;
	}

	RegisterDefaultIndexers();

	IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

	if (ShouldAutoIndex())
	{
		UE_LOG(LogMonolithIndex, Log, TEXT("First launch — deferring full index until AR ready"));
		if (AR.IsLoadingAssets())
			AR.OnFilesLoaded().AddUObject(this, &UMonolithIndexSubsystem::OnAssetRegistryFilesLoaded);
		else
			StartFullIndex();
	}
	else if (CanDoIncrementalIndex())
	{
		UE_LOG(LogMonolithIndex, Log, TEXT("Existing index found — deferring incremental catch-up until AR ready"));
		if (AR.IsLoadingAssets())
			AR.OnFilesLoaded().AddUObject(this, &UMonolithIndexSubsystem::StartIncrementalIndex);
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
		StartFullIndex();
	}
	else
	{
		UE_LOG(LogMonolithIndex, Log, TEXT("Asset Registry loaded but auto-index no longer needed (already indexed)"));
	}
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

	UE_LOG(LogMonolithIndex, Log, TEXT("Registered %d indexers"), Indexers.Num());
}

void UMonolithIndexSubsystem::StartFullIndex()
{
	if (bIsIndexing)
	{
		UE_LOG(LogMonolithIndex, Warning, TEXT("Indexing already in progress"));
		return;
	}

	bIsIndexing = true;

	// Reset the database for a full re-index
	Database->ResetDatabase();

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
	IndexingThread.Reset(FRunnableThread::Create(
		IndexingTaskPtr.Get(),
		TEXT("MonolithIndexing"),
		0,
		TPri_BelowNormal
	));

	UE_LOG(LogMonolithIndex, Log, TEXT("Background indexing started"));
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
		AsyncTask(ENamedThreads::GameThread, [this]()
		{
			Owner->OnIndexingFinished(false);
		});
		return 1;
	}

	DB->BeginTransaction();

	int32 BatchSize = 100;
	int32 Indexed = 0;
	int32 Errors = 0;

	// Collect assets that have deep indexers for a second pass
	struct FDeepIndexEntry
	{
		FAssetData AssetData;
		int64 AssetId;
		TSharedPtr<IMonolithIndexer> Indexer;
	};
	TArray<FDeepIndexEntry> DeepIndexQueue;

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
			if (FPackageName::DoesPackageExist(AssetData.PackageName.ToString(), nullptr, &PackageFilename))
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

		int64 AssetId = DB->InsertAsset(IndexedAsset);
		if (AssetId < 0)
		{
			Errors++;
			continue;
		}

		// Queue assets that have deep indexers (Blueprint, Material, etc.)
		TSharedPtr<IMonolithIndexer>* FoundIndexer = Owner->ClassToIndexer.Find(IndexedAsset.AssetClass);
		if (FoundIndexer && FoundIndexer->IsValid())
		{
			DeepIndexQueue.Add({ AssetData, AssetId, *FoundIndexer });
			QueuedClassDistribution.FindOrAdd(IndexedAsset.AssetClass)++;
		}

		Indexed++;

		// Commit in batches
		if (Indexed % BatchSize == 0)
		{
			DB->CommitTransaction();
			DB->BeginTransaction();

			UE_LOG(LogMonolithIndex, Log, TEXT("Indexed %d / %d assets (%d errors)"),
				Indexed, TotalAssets.Load(), Errors);

			if (Owner->TaskNotification)
			{
				Owner->TaskNotification->SetProgressText(FText::FromString(
					FString::Printf(TEXT("Indexing %d / %d assets..."), CurrentIndex.Load(), TotalAssets.Load())));
			}

			AsyncTask(ENamedThreads::GameThread, [this]()
			{
				Owner->OnProgress.Broadcast(CurrentIndex.Load(), TotalAssets.Load());
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

	DB->CommitTransaction();

	UE_LOG(LogMonolithIndex, Log, TEXT("Metadata pass complete: %d assets indexed, %d errors"), Indexed, Errors);

	// ============================================================
	// Deep indexing pass — load assets on game thread in time-budgeted batches
	// Assets must be loaded on the game thread to avoid texture compiler crashes.
	// We process in small batches, yielding when the frame budget is exceeded.
	// ============================================================
	Owner->IndexingStatusMessage = FString::Printf(TEXT("Deep indexing %d assets..."), DeepIndexQueue.Num());

	if (!bShouldStop && DeepIndexQueue.Num() > 0)
	{
		UE_LOG(LogMonolithIndex, Log, TEXT("Starting deep indexing pass for %d assets..."), DeepIndexQueue.Num());

		constexpr int32 DeepBatchSize = 16;
		constexpr double FrameBudgetSeconds = 0.016; // ~16ms per batch to stay interactive
		TAtomic<int32> DeepIndexed{0};
		TAtomic<int32> DeepErrors{0};
		int32 TotalDeep = DeepIndexQueue.Num();

		for (int32 BatchStart = 0; BatchStart < TotalDeep && !bShouldStop; BatchStart += DeepBatchSize)
		{
			int32 BatchEnd = FMath::Min(BatchStart + DeepBatchSize, TotalDeep);

			// Capture the slice for this batch
			TArray<FDeepIndexEntry> BatchSlice;
			BatchSlice.Reserve(BatchEnd - BatchStart);
			for (int32 j = BatchStart; j < BatchEnd; ++j)
			{
				BatchSlice.Add(DeepIndexQueue[j]);
			}

			FEvent* BatchEvent = FPlatformProcess::GetSynchEventFromPool(true);

			AsyncTask(ENamedThreads::GameThread, [DB, BatchSlice = MoveTemp(BatchSlice), &DeepIndexed, &DeepErrors, FrameBudgetSeconds, BatchEvent]()
			{
				DB->BeginTransaction();
				double BatchStartTime = FPlatformTime::Seconds();

				for (const FDeepIndexEntry& Entry : BatchSlice)
				{
					// Load asset on game thread (safe for texture compiler)
					UObject* LoadedAsset = Entry.AssetData.GetAsset();
					if (LoadedAsset)
					{
						if (Entry.Indexer->IndexAsset(Entry.AssetData, LoadedAsset, *DB, Entry.AssetId))
						{
							DeepIndexed++;
						}
						else
						{
							DeepErrors++;
							UE_LOG(LogMonolithIndex, Warning, TEXT("Deep indexer '%s' failed for: %s"),
								*Entry.Indexer->GetName(),
								*Entry.AssetData.PackageName.ToString());
						}
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
						DB->BeginTransaction();
						BatchStartTime = FPlatformTime::Seconds();
					}
				}

				DB->CommitTransaction();
				BatchEvent->Trigger();
			});

			BatchEvent->Wait();
			FPlatformProcess::ReturnSynchEventToPool(BatchEvent);

			// Update progress — report deep pass as second half of overall progress
			CurrentIndex = Indexed + BatchEnd;
			TotalAssets = Indexed + TotalDeep;

			if (Owner->TaskNotification)
			{
				Owner->TaskNotification->SetProgressText(FText::FromString(
					FString::Printf(TEXT("Deep indexing %d / %d assets..."), BatchEnd, TotalDeep)));
			}

			AsyncTask(ENamedThreads::GameThread, [this]()
			{
				Owner->OnProgress.Broadcast(CurrentIndex.Load(), TotalAssets.Load());
			});

			UE_LOG(LogMonolithIndex, Log, TEXT("Deep indexed %d / %d assets (%d ok, %d errors)"),
				BatchEnd, TotalDeep, DeepIndexed.Load(), DeepErrors.Load());
		}

		UE_LOG(LogMonolithIndex, Log, TEXT("Deep indexing complete: %d indexed, %d errors"),
			DeepIndexed.Load(), DeepErrors.Load());
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

	// Run dependency indexer on game thread (Asset Registry requires it)
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
		AsyncTask(ENamedThreads::GameThread, [DB, DepIndexerCopy, DepEvent]()
		{
			DB->BeginTransaction();
			FAssetData DummyData;
			DepIndexerCopy->IndexAsset(DummyData, nullptr, *DB, 0);
			DB->CommitTransaction();
			DepEvent->Trigger();
		});
		DepEvent->Wait();
		FPlatformProcess::ReturnSynchEventToPool(DepEvent);
		UE_LOG(LogMonolithIndex, Log, TEXT("Dependency indexer completed in %.2fs"), FPlatformTime::Seconds() - SentinelStart);
	}

	// Run level indexer on game thread (asset loading requires it)
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
		AsyncTask(ENamedThreads::GameThread, [DB, LevelIndexerCopy, LevelEvent]()
		{
			DB->BeginTransaction();
			FAssetData DummyData;
			LevelIndexerCopy->IndexAsset(DummyData, nullptr, *DB, 0);
			DB->CommitTransaction();
			LevelEvent->Trigger();
		});
		LevelEvent->Wait();
		FPlatformProcess::ReturnSynchEventToPool(LevelEvent);
		UE_LOG(LogMonolithIndex, Log, TEXT("Level indexer completed in %.2fs"), FPlatformTime::Seconds() - SentinelStart);
	}

	// Run DataTable indexer on game thread (requires asset loading)
	Owner->IndexingStatusMessage = TEXT("Indexing DataTable rows...");
	TSharedPtr<IMonolithIndexer>* DTIndexer = Owner->ClassToIndexer.Find(TEXT("__DataTables__"));
	if (DTIndexer && DTIndexer->IsValid())
	{
		double SentinelStart = FPlatformTime::Seconds();
		UE_LOG(LogMonolithIndex, Log, TEXT("Running DataTable indexer..."));
		TSharedPtr<IMonolithIndexer> DTIndexerCopy = *DTIndexer;
		FEvent* DTEvent = FPlatformProcess::GetSynchEventFromPool(true);
		AsyncTask(ENamedThreads::GameThread, [DB, DTIndexerCopy, DTEvent]()
		{
			DB->BeginTransaction();
			FAssetData DummyData;
			DTIndexerCopy->IndexAsset(DummyData, nullptr, *DB, 0);
			DB->CommitTransaction();
			DTEvent->Trigger();
		});
		DTEvent->Wait();
		FPlatformProcess::ReturnSynchEventToPool(DTEvent);
		UE_LOG(LogMonolithIndex, Log, TEXT("DataTable indexer completed in %.2fs"), FPlatformTime::Seconds() - SentinelStart);
	}

	// Run config indexer (file I/O only, no game thread needed)
	Owner->IndexingStatusMessage = TEXT("Indexing config files...");
	TSharedPtr<IMonolithIndexer>* CfgIndexer = Owner->ClassToIndexer.Find(TEXT("__Configs__"));
	if (CfgIndexer && CfgIndexer->IsValid())
	{
		double SentinelStart = FPlatformTime::Seconds();
		UE_LOG(LogMonolithIndex, Log, TEXT("Running config indexer..."));
		DB->BeginTransaction();
		FAssetData DummyCfgData;
		(*CfgIndexer)->IndexAsset(DummyCfgData, nullptr, *DB, 0);
		DB->CommitTransaction();
		UE_LOG(LogMonolithIndex, Log, TEXT("Config indexer completed in %.2fs"), FPlatformTime::Seconds() - SentinelStart);
	}

	// Run C++ symbol indexer (file I/O only, no game thread needed)
	Owner->IndexingStatusMessage = TEXT("Indexing C++ symbols...");
	TSharedPtr<IMonolithIndexer>* CppIndexer = Owner->ClassToIndexer.Find(TEXT("__CppSymbols__"));
	if (CppIndexer && CppIndexer->IsValid())
	{
		double SentinelStart = FPlatformTime::Seconds();
		UE_LOG(LogMonolithIndex, Log, TEXT("Running C++ symbol indexer..."));
		DB->BeginTransaction();
		FAssetData DummyCppData;
		(*CppIndexer)->IndexAsset(DummyCppData, nullptr, *DB, 0);
		DB->CommitTransaction();
		UE_LOG(LogMonolithIndex, Log, TEXT("C++ symbol indexer completed in %.2fs"), FPlatformTime::Seconds() - SentinelStart);
	}

	// Run animation indexer on game thread (asset loading requires it)
	Owner->IndexingStatusMessage = TEXT("Indexing animations...");
	TSharedPtr<IMonolithIndexer>* AnimIndexer = Owner->ClassToIndexer.Find(TEXT("__Animations__"));
	if (AnimIndexer && AnimIndexer->IsValid())
	{
		double SentinelStart = FPlatformTime::Seconds();
		UE_LOG(LogMonolithIndex, Log, TEXT("Running animation indexer..."));
		TSharedPtr<IMonolithIndexer> AnimIndexerCopy = *AnimIndexer;
		FEvent* AnimEvent = FPlatformProcess::GetSynchEventFromPool(true);
		AsyncTask(ENamedThreads::GameThread, [DB, AnimIndexerCopy, AnimEvent]()
		{
			DB->BeginTransaction();
			FAssetData DummyData;
			AnimIndexerCopy->IndexAsset(DummyData, nullptr, *DB, 0);
			DB->CommitTransaction();
			AnimEvent->Trigger();
		});
		AnimEvent->Wait();
		FPlatformProcess::ReturnSynchEventToPool(AnimEvent);
		UE_LOG(LogMonolithIndex, Log, TEXT("Animation indexer completed in %.2fs"), FPlatformTime::Seconds() - SentinelStart);
	}

	// Run gameplay tag indexer on game thread (GameplayTagsManager requires it)
	Owner->IndexingStatusMessage = TEXT("Indexing gameplay tags...");
	TSharedPtr<IMonolithIndexer>* TagIndexer = Owner->ClassToIndexer.Find(TEXT("__GameplayTags__"));
	if (TagIndexer && TagIndexer->IsValid())
	{
		double SentinelStart = FPlatformTime::Seconds();
		UE_LOG(LogMonolithIndex, Log, TEXT("Running gameplay tag indexer..."));
		TSharedPtr<IMonolithIndexer> TagIndexerCopy = *TagIndexer;
		FEvent* TagEvent = FPlatformProcess::GetSynchEventFromPool(true);
		AsyncTask(ENamedThreads::GameThread, [DB, TagIndexerCopy, TagEvent]()
		{
			DB->BeginTransaction();
			FAssetData DummyData;
			TagIndexerCopy->IndexAsset(DummyData, nullptr, *DB, 0);
			DB->CommitTransaction();
			TagEvent->Trigger();
		});
		TagEvent->Wait();
		FPlatformProcess::ReturnSynchEventToPool(TagEvent);
		UE_LOG(LogMonolithIndex, Log, TEXT("Gameplay tag indexer completed in %.2fs"), FPlatformTime::Seconds() - SentinelStart);
	}

	// Run Niagara indexer on game thread (requires asset loading)
	Owner->IndexingStatusMessage = TEXT("Indexing Niagara systems...");
	TSharedPtr<IMonolithIndexer>* NiagaraIndexerPtr = Owner->ClassToIndexer.Find(TEXT("__Niagara__"));
	if (NiagaraIndexerPtr && NiagaraIndexerPtr->IsValid())
	{
		double SentinelStart = FPlatformTime::Seconds();
		UE_LOG(LogMonolithIndex, Log, TEXT("Running Niagara indexer..."));
		TSharedPtr<IMonolithIndexer> NiagaraIndexerCopy = *NiagaraIndexerPtr;
		FEvent* NiagaraEvent = FPlatformProcess::GetSynchEventFromPool(true);
		AsyncTask(ENamedThreads::GameThread, [DB, NiagaraIndexerCopy, NiagaraEvent]()
		{
			DB->BeginTransaction();
			FAssetData DummyData;
			NiagaraIndexerCopy->IndexAsset(DummyData, nullptr, *DB, 0);
			DB->CommitTransaction();
			NiagaraEvent->Trigger();
		});
		NiagaraEvent->Wait();
		FPlatformProcess::ReturnSynchEventToPool(NiagaraEvent);
		UE_LOG(LogMonolithIndex, Log, TEXT("Niagara indexer completed in %.2fs"), FPlatformTime::Seconds() - SentinelStart);
	}

	// Run mesh catalog indexer on game thread (requires asset loading)
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
		AsyncTask(ENamedThreads::GameThread, [DB, MeshCatIndexerCopy, MeshCatEvent]()
		{
			DB->BeginTransaction();
			FAssetData DummyData;
			MeshCatIndexerCopy->IndexAsset(DummyData, nullptr, *DB, 0);
			DB->CommitTransaction();
			MeshCatEvent->Trigger();
		});
		MeshCatEvent->Wait();
		FPlatformProcess::ReturnSynchEventToPool(MeshCatEvent);
		UE_LOG(LogMonolithIndex, Log, TEXT("Mesh catalog indexer completed in %.2fs"), FPlatformTime::Seconds() - SentinelStart);
	}

	// Write index timestamp to meta (only if not cancelled and asset count looks valid)
	if (!bShouldStop)
	{
		constexpr int32 MinAssetCountThreshold = 500;
		if (Indexed < MinAssetCountThreshold)
		{
			UE_LOG(LogMonolithIndex, Warning, TEXT("Index only found %d assets — Asset Registry may not have been fully loaded. Skipping last_full_index write so next launch will re-index."), Indexed);
		}
		else
		{
			DB->WriteMeta(TEXT("last_full_index"), FDateTime::UtcNow().ToString());
			UE_LOG(LogMonolithIndex, Log, TEXT("Wrote last_full_index timestamp (%d assets indexed)"), Indexed);
		}
	}

	AsyncTask(ENamedThreads::GameThread, [this]()
	{
		Owner->OnIndexingFinished(!bShouldStop);
	});

	return 0;
}

void UMonolithIndexSubsystem::OnIndexingFinished(bool bSuccess)
{
	bIsIndexing = false;
	IndexingStatusMessage.Empty();

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

void UMonolithIndexSubsystem::StartIncrementalIndex()
{
	check(IsInGameThread());
	if (bIsIndexing) return;
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
		return;
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
		if (FPackageName::DoesPackageExist(PathStr, nullptr, &PackageFilename))
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
