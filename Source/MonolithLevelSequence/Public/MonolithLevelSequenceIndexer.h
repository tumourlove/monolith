#pragma once

#include "CoreMinimal.h"
#include "MonolithIndexer.h"

class FMonolithIndexDatabase;

/**
 * Indexes ULevelSequence assets that have a Director Blueprint.
 *
 * Stage 1 of incremental rollout: only inserts a row per Level Sequence
 * with a Director into level_sequence_directors. Functions, variables,
 * and event-track bindings are added in subsequent commits.
 *
 * Tables created:
 *   level_sequence_directors           (this stage)
 *   level_sequence_director_functions  (Step 3)
 *   level_sequence_director_variables  (Step 3)
 *   level_sequence_event_bindings      (Step 4)
 */
class FLevelSequenceIndexer : public IMonolithIndexer
{
public:
	virtual TArray<FString> GetSupportedClasses() const override;
	virtual bool IndexAsset(const FAssetData& AssetData, UObject* LoadedAsset, FMonolithIndexDatabase& DB, int64 AssetId) override;
	virtual FString GetName() const override { return TEXT("LevelSequence"); }

private:
	void EnsureTablesExist(FMonolithIndexDatabase& DB);
	bool bTablesCreated = false;
};
