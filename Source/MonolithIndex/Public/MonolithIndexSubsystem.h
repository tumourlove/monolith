#pragma once

#include "CoreMinimal.h"
#include "EditorSubsystem.h"
#include "Misc/AsyncTaskNotification.h"
#include "MonolithIndexDatabase.h"
#include "MonolithIndexer.h"
#include "MonolithIndexSubsystem.generated.h"

DECLARE_MULTICAST_DELEGATE_TwoParams(FOnIndexingProgress, int32 /*Current*/, int32 /*Total*/);
DECLARE_MULTICAST_DELEGATE_OneParam(FOnIndexingComplete, bool /*bSuccess*/);

enum class EIndexChangeType : uint8
{
    Added,
    Removed,
    Renamed,
    Updated
};

struct FPendingIndexChange
{
    EIndexChangeType Type;
    FAssetData AssetData;
    FString OldObjectPath; // Only for Renamed
};

/** Info about an indexed plugin */
struct FIndexedPluginInfo
{
    FString PluginName;     // Logical name (e.g., "ExampleInventory")
    FString MountPath;      // AR virtual root (e.g., "/ExampleInventory/")
    FString ContentDir;     // Disk path to Content/
    FString FriendlyName;   // Display name from .uplugin
};

/**
 * Editor subsystem that orchestrates the Monolith project index.
 * Owns the SQLite database, manages indexers, runs background indexing.
 */
UCLASS()
class MONOLITHINDEX_API UMonolithIndexSubsystem : public UEditorSubsystem
{
	GENERATED_BODY()

public:
	// --- UEditorSubsystem interface ---
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	/**
	 * Trigger a full re-index (wipes DB, re-scans everything).
	 * This is the explicit "start over" entry point — it always resets, even when
	 * an interrupted index could have been resumed.
	 * @return true if a run actually started. False means nothing is happening —
	 *         see CanAcceptIndexRequest, or the worker thread failed to spawn.
	 */
	UFUNCTION()
	bool StartFullIndex();

	/**
	 * Continue an interrupted full index if one is recoverable, otherwise start a
	 * fresh one. This is what the automatic startup path and `Monolith.StartIndex`
	 * use: a crash mid-index must not cost the batches already committed.
	 *
	 * UFUNCTION because MonolithCore reaches MonolithIndex only through reflection,
	 * and `monolith_reindex` without `force` must be able to land here. Without it
	 * that call falls through to StartFullIndex -- which always resets -- and an
	 * agent kicking the index after a crash silently destroys the very progress
	 * this recovery path exists to preserve.
	 * @return true if a run actually started.
	 */
	UFUNCTION()
	bool ResumeFullIndex();

	/**
	 * Trigger an incremental catch-up index (delta engine).
	 * @return true if the delta pass ran. False means it was refused.
	 */
	UFUNCTION()
	bool StartIncrementalIndex();

	/** Can we do an incremental index? (requires schema v2+ and a prior full index) */
	UFUNCTION()
	bool CanDoIncrementalIndex() const;

	/**
	 * Is the subsystem in a state where it could take an index request right now?
	 * Used to grey out the Project Settings re-index buttons.
	 */
	UFUNCTION()
	bool CanAcceptIndexRequest() const;

	/** Is indexing currently in progress? */
	bool IsIndexing() const { return bIsIndexing; }

	/** Get indexing progress (0.0 - 1.0) */
	float GetProgress() const;

	/** Get current indexing status message */
	FString GetStatusMessage() const { return IndexingStatusMessage; }

	/** Get the database (for queries). May be null if not initialized. */
	FMonolithIndexDatabase* GetDatabase() { return Database.Get(); }

	// --- Query API (called by MCP actions) ---
	TArray<FSearchResult> Search(const FString& Query, int32 Limit = 50);
	TSharedPtr<FJsonObject> FindReferences(const FString& PackagePath);
	TArray<FIndexedAsset> FindByType(const FString& AssetClass, int32 Limit = 100, int32 Offset = 0);
	TSharedPtr<FJsonObject> GetStats();
	TSharedPtr<FJsonObject> GetAssetDetails(const FString& PackagePath);

	/** Register an indexer. Takes ownership. */
	void RegisterIndexer(TSharedPtr<IMonolithIndexer> Indexer);

	// --- Delegates ---
	FOnIndexingProgress OnProgress;
	FOnIndexingComplete OnComplete;

private:
	/** Background indexing task */
	class FIndexingTask : public FRunnable
	{
	public:
		FIndexingTask(UMonolithIndexSubsystem* InOwner);

		virtual bool Init() override { return true; }
		virtual uint32 Run() override;
		virtual void Stop() override { bShouldStop = true; }

		TAtomic<bool> bShouldStop{false};
		TAtomic<int32> CurrentIndex{0};
		TAtomic<int32> TotalAssets{0};
		TArray<FIndexedPluginInfo> PluginsToIndex;

		/** True when this run continues an interrupted index rather than starting one. */
		bool bIsResume = false;

	private:
		UMonolithIndexSubsystem* Owner;

		/**
		 * Wait for an event that only a game-thread task can trigger, without
		 * deadlocking shutdown: at editor exit the game thread blocks in
		 * Deinitialize's WaitForCompletion, so a queued task can never run and
		 * a bare Wait() hangs the exit forever (the "QUIT_EDITOR during first
		 * index" hang). Polls in 100ms slices; bails when stop is requested
		 * AND the engine is exiting (a plain user cancel keeps waiting — the
		 * game thread is alive and will serve the task).
		 * @return true = event triggered (return it to the pool as usual);
		 *         false = wait abandoned — the caller must NOT return the
		 *         event to the pool (a later Trigger on a leaked event is
		 *         harmless; on a recycled one it is corruption).
		 */
		bool WaitForGameThreadEvent(FEvent* Event);

		/**
		 * RunOnGameThreadWhenCompilerIdle + WaitForGameThreadEvent + the
		 * abandon protocol (sets the dispatcher's abort token so the pending
		 * Work — whose captures may dangle once Run() unwinds — is never
		 * invoked). @return false = abandoned; caller should stop its pass.
		 */
		bool DispatchToGameThreadAndWait(TUniqueFunction<void()> Work);
	};

	/**
	 * Shared body of StartFullIndex/ResumeFullIndex.
	 * @param bForceReset true wipes the database first; false resumes an
	 *        interrupted run when the schema and the in-progress marker allow it,
	 *        and falls back to a wipe when they do not.
	 */
	bool StartFullIndexInternal(bool bForceReset);

	void OnIndexingFinished(bool bSuccess);
	void OnAssetRegistryFilesLoaded();

	/** void adapter so StartIncrementalIndex can be deferred onto OnFilesLoaded. */
	void OnAssetRegistryFilesLoadedIncremental();
	void RegisterDefaultIndexers();
	FString GetDatabasePath() const;
	bool ShouldAutoIndex() const;

	/** Gather mount paths for enabled marketplace plugins */
	TArray<FIndexedPluginInfo> GatherMarketplacePluginPaths() const;

	/** Deep-index a set of asset paths (stub — implemented in Task 5) */
	void ProcessDeepIndexQueue(const TSet<FString>& PathsToIndex);

	/** Run scoped sentinel indexers for changed/removed paths (stub — implemented in Task 6) */
	void RunScopedSentinels(const TSet<FString>& ChangedPaths, const TSet<FString>& RemovedPaths);

	/** Register live AR callbacks for real-time tracking (stub — implemented in Task 4) */
	void RegisterLiveCallbacks();

	/** Unregister live AR callbacks (stub — implemented in Task 4) */
	void UnregisterLiveCallbacks();

	// --- Live AR callback handlers ---
	void OnAssetsAddedCallback(TConstArrayView<FAssetData> Assets);
	void OnAssetsRemovedCallback(TConstArrayView<FAssetData> Assets);
	void OnAssetRenamedCallback(const FAssetData& AssetData, const FString& OldObjectPath);
	void OnAssetsUpdatedOnDiskCallback(TConstArrayView<FAssetData> Assets);
	void ProcessPendingChanges();

	// --- Live incremental tracking ---
	TArray<FPendingIndexChange> PendingChanges;
	FTimerHandle LiveIndexTimerHandle;

	// AR delegate handles
	FDelegateHandle OnAssetsAddedHandle;
	FDelegateHandle OnAssetsRemovedHandle;
	FDelegateHandle OnAssetRenamedHandle;
	FDelegateHandle OnAssetsUpdatedOnDiskHandle;

	/** Cached list of plugin paths being indexed (set during StartFullIndex) */
	TArray<FIndexedPluginInfo> IndexedPlugins;

	TUniquePtr<FMonolithIndexDatabase> Database;
	TArray<TSharedPtr<IMonolithIndexer>> Indexers;
	TMap<FString, TSharedPtr<IMonolithIndexer>> ClassToIndexer;

	TUniquePtr<FRunnableThread> IndexingThread;
	TUniquePtr<FIndexingTask> IndexingTaskPtr;
	TAtomic<bool> bIsIndexing{false};

	FString IndexingStatusMessage;
	TUniquePtr<FAsyncTaskNotification> TaskNotification;
};
