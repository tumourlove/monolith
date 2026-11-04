#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

/**
 * Handlers for the `gameplay_tags` namespace.
 *
 * Reference data is read live from the Asset Registry rather than from Monolith's
 * SQLite project index. The engine records a SearchableName dependency for every
 * serialized FGameplayTag (FGameplayTagContainer.cpp calls MarkSearchableName), so
 * the registry already knows which packages reference which tag, wherever the tag
 * sits: an asset property, a graph node, a component on a level. Reading it live
 * also keeps these answers out of the "stale until a full reindex" class that the
 * index's post-pass tables fall into.
 *
 * Tag metadata (hierarchy, declaring source, dev comment) comes from
 * UGameplayTagsManager, which is likewise live.
 */
class FMonolithGameplayTagsActions
{
public:
	static void RegisterActions(FMonolithToolRegistry& Registry);

private:
	static FMonolithActionResult Ping(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult FindReferences(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult GetAssetTags(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult List(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult GetTagInfo(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult FindUnused(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult FindBroken(const TSharedPtr<FJsonObject>& Params);
};
