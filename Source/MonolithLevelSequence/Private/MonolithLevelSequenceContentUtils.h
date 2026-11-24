#pragma once

#include "CoreMinimal.h"
#include "Misc/FrameRate.h"
#include "Misc/FrameNumber.h"
#include "Misc/FrameTime.h"
#include "MonolithToolRegistry.h"

class FJsonObject;
class FJsonValue;
class UMovieScene;
class UMovieSceneSequence;
class UMovieSceneSection;
class UMovieSceneTrack;
struct FMovieSceneChannel;

/**
 * Shared plumbing for the live-asset level_sequence actions (get_sequence_contents, retime and the
 * track / section / key / sequence writers). Unlike the index-backed actions, these load the asset.
 *
 * Time convention (mirrors UE's Sequencer scripting default, EMovieSceneTimeUnit::DisplayRate):
 * every frame a caller sends or reads is a display-rate frame (what Sequencer shows). Keys also carry
 * an optional sub_frame in [0, 1). Data is stored in ticks (TickResolution); conversion happens here.
 */
namespace MonolithLevelSequence::Content
{
	/** A loaded sequence plus its rates. */
	struct FSequenceContext
	{
		UMovieSceneSequence* Sequence = nullptr;
		UMovieScene* MovieScene = nullptr;
		FFrameRate DisplayRate;
		FFrameRate TickResolution;
		FString AssetPath;

		/** Display frame (+ sub-frame) to a tick time, rounded to the nearest tick. */
		FFrameNumber ToTicks(int32 Frame, double SubFrame = 0.0) const;
		/** Display frames (possibly fractional) to ticks, rounded. */
		FFrameNumber DecimalToTicks(double Frames) const;
		/** Tick time to display time (frame + sub-frame). */
		FFrameTime ToDisplay(FFrameNumber Ticks) const;
		/** Tick time to the nearest display frame. */
		int32 ToDisplayFrame(FFrameNumber Ticks) const;
		/** Tick duration to display frames, as a decimal. */
		double ToDisplayDecimal(FFrameNumber Ticks) const;
		/** Display frames to seconds (view / work ranges are stored in seconds). */
		double FramesToSeconds(double Frames) const;
		/** Seconds to display frames, as a decimal. */
		double SecondsToFrames(double Seconds) const;

		/** {display_rate, fps, tick_resolution, ticks_per_frame?} */
		void WriteRates(const TSharedPtr<FJsonObject>& Out) const;
	};

	/** Load `asset_path` as a UMovieSceneSequence with a movie scene. */
	bool LoadSequence(const TSharedPtr<FJsonObject>& Params, FSequenceContext& Out, FString& OutError);

	/** Resolve a binding reference (GUID in any FGuid format, or display name) to a binding GUID. */
	bool ResolveBindingRef(const FSequenceContext& Ctx, const FString& Ref, FGuid& OutGuid, FString& OutError);

	/** Display name of a binding (Sequencer label). */
	FString GetBindingDisplayName(const FSequenceContext& Ctx, const FGuid& Guid);

	/** Tracks of one scope: the binding's tracks, or (invalid GUID) the camera cut track then the sequence tracks. */
	TArray<UMovieSceneTrack*> GetScopeTracks(const FSequenceContext& Ctx, const FGuid& BindingGuid);

	/** Resolve a track class name ("MovieSceneSkeletalAnimationTrack", with or without the U prefix). */
	UClass* ResolveTrackClass(const FString& Name, FString& OutError);

	/** Index of Track among the tracks of the same exact class in its scope (the index callers pass back). */
	int32 GetTrackIndexInClass(const TArray<UMovieSceneTrack*>& ScopeTracks, const UMovieSceneTrack* Track);

	/** A resolved write target. Section fields are set only by ResolveSection. */
	struct FTarget
	{
		FGuid BindingGuid;
		UMovieSceneTrack* Track = nullptr;
		int32 TrackIndex = 0;
		UMovieSceneSection* Section = nullptr;
		int32 SectionIndex = 0;
	};

	/** binding? + track_class + track_index (default 0). */
	bool ResolveTrack(const FSequenceContext& Ctx, const TSharedPtr<FJsonObject>& Params, FTarget& Out, FString& OutError);

	/** ResolveTrack + section_index (default 0). */
	bool ResolveSection(const FSequenceContext& Ctx, const TSharedPtr<FJsonObject>& Params, FTarget& Out, FString& OutError);

	/** One channel of a section, as listed by its channel proxy. */
	struct FChannelRef
	{
		FMovieSceneChannel* Channel = nullptr;
		FName TypeName;
		FString Name;
		/**
		 * Empty when Name is the channel's metadata name. "display_text" when the channel has no name and its display
		 * text stands in; "track" when the section's only channel is unnamed and the track's display name stands in.
		 */
		FString NameSource;
		/** Position across every channel of the section, in proxy order. */
		int32 Index = 0;
	};

	TArray<FChannelRef> GetChannels(UMovieSceneSection* Section);

	/** Resolve `channel`: a channel name (Location.X, Weight...) or "#N" (the index from the dump). */
	bool ResolveChannel(UMovieSceneSection* Section, const FString& Ref, FChannelRef& Out, FString& OutError);

	/** Short type label: float, double, bool, integer, byte, or the struct name for other channels. */
	FString GetChannelTypeLabel(FName TypeName);

	/** True for channel types set_keys / remove_keys / set_channel_default can write. */
	bool IsWritableChannelType(FName TypeName);

	// --- JSON ---------------------------------------------------------------------------------

	TSharedPtr<FJsonObject> ChannelToJson(const FSequenceContext& Ctx, const FChannelRef& Ref, bool bIncludeKeys, int32 MaxKeys);
	TSharedPtr<FJsonObject> SectionToJson(const FSequenceContext& Ctx, UMovieSceneSection* Section, int32 Index, bool bIncludeChannels, bool bIncludeKeys, int32 MaxKeys);
	TSharedPtr<FJsonObject> TrackToJson(const FSequenceContext& Ctx, UMovieSceneTrack* Track, int32 IndexInClass, bool bIncludeChannels, bool bIncludeKeys, int32 MaxKeys);

	/** Every marked frame: {index, frame, label, comment?}. */
	TArray<TSharedPtr<FJsonValue>> MarkedFramesToJson(const FSequenceContext& Ctx);

	// --- Writes -------------------------------------------------------------------------------

	/** `save` param, default true. */
	bool ShouldSave(const TSharedPtr<FJsonObject>& Params);

	/**
	 * Save the sequence when `save` is true (default), and record saved / save_error on Result.
	 * Writes always run in an FScopedTransaction before this, so an unsaved change is undoable.
	 */
	FMonolithActionResult FinishWrite(const FSequenceContext& Ctx, const TSharedPtr<FJsonObject>& Params, const TSharedPtr<FJsonObject>& Result);

	/** Read an integer frame field; false (no error) when absent, false + error when not a number. */
	bool TryGetFrame(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field, int32& OutFrame, FString& OutError);
}
