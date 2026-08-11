#pragma once

#include "CoreMinimal.h"
#include "AssetRegistry/AssetData.h"

#if WITH_LOGICDRIVER

/**
 * FStateMachineIndexer -- deep indexer for Logic Driver SM assets.
 * Registers into MonolithIndex at startup. Indexes SM Blueprints,
 * Node Blueprints, and component references for cross-reference queries.
 */
class FStateMachineIndexer
{
public:
	static void Register();
	static void Unregister();

	/** Index a single SM Blueprint */
	static void IndexStateMachine(const FAssetData& AssetData);

	/** Full reindex of all SM assets */
	static void ReindexAll();
};

#endif // WITH_LOGICDRIVER
