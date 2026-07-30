#pragma once

#include "CoreMinimal.h"
#include "MonolithIndexDatabase.h"

class IAssetRegistry;
struct FAssetData;

/**
 * Base interface for all asset indexers.
 * Each indexer knows how to deeply inspect one or more asset types
 * and write structured data into the index database.
 */
class MONOLITHINDEX_API IMonolithIndexer
{
public:
	virtual ~IMonolithIndexer() = default;

	/** Return the asset classes this indexer handles (e.g. "Blueprint", "Material") */
	virtual TArray<FString> GetSupportedClasses() const = 0;

	/**
	 * Index a single asset. Called on a background thread.
	 * The asset is already loaded -- inspect it and write to DB.
	 * @return true if indexing succeeded
	 */
	virtual bool IndexAsset(const FAssetData& AssetData, UObject* LoadedAsset, FMonolithIndexDatabase& DB, int64 AssetId) = 0;

	/**
	 * Stable, unique indexer name used for logging and durable full-index
	 * checkpoints. Changing it invalidates interrupted-run progress.
	 */
	virtual FString GetName() const = 0;

	/**
	 * Whether this is a sentinel indexer (does its own enumeration). Sentinels
	 * without specialized orchestration run through the generic compiler-idle
	 * full-index path, so IndexAsset must report failure and rebuild derived
	 * data idempotently.
	 */
	virtual bool IsSentinel() const { return false; }

	/** Whether this sentinel supports scoped (incremental) indexing */
	virtual bool SupportsIncrementalIndex() const { return false; }

	/** Re-index only specified assets. Called AFTER main transaction commits — may open own transaction. */
	virtual bool IndexScoped(const TSet<FString>& ChangedPaths, const TSet<FString>& RemovedPaths, FMonolithIndexDatabase& DB) { return false; }
};
