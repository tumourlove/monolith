#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

/**
 * Live-asset level_sequence actions: read a sequence's tracks, sections and keys, insert or remove
 * time, and edit tracks, sections, keys, marked frames and ranges. These load the asset rather than
 * reading the index (the index-backed actions live in FMonolithLevelSequenceActions).
 *
 * Writes run in an FScopedTransaction (undoable in the editor) and save unless save=false. They do
 * not reindex: the index-backed actions catch up on the next index pass.
 */
class FMonolithLevelSequenceContentActions
{
public:
	static void RegisterActions(FMonolithToolRegistry& Registry);

	// Read
	static FMonolithActionResult GetSequenceContents(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult RefreshCurrentSequence(const TSharedPtr<FJsonObject>& Params);

	// Time
	static FMonolithActionResult Retime(const TSharedPtr<FJsonObject>& Params);

	// Tracks and sections
	static FMonolithActionResult AddTrack(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult RemoveTrack(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult AddSection(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult SetSection(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult RemoveSection(const TSharedPtr<FJsonObject>& Params);

	// Keys
	static FMonolithActionResult SetKeys(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult RemoveKeys(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult SetChannelDefault(const TSharedPtr<FJsonObject>& Params);

	// Sequence
	static FMonolithActionResult SetMarkedFrames(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult SetPlaybackRange(const TSharedPtr<FJsonObject>& Params);
};
