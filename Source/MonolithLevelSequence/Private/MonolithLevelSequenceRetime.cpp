// level_sequence.retime: insert or remove time across a whole sequence (port of the rules a Powerz
// editor-Python script used, plus marked frames and a dry run).

#include "MonolithLevelSequenceContentActions.h"
#include "MonolithLevelSequenceContentUtils.h"

#include "MovieScene.h"
#include "MovieSceneBinding.h"
#include "MovieSceneSection.h"
#include "MovieSceneTrack.h"
#include "MovieSceneMarkedFrame.h"
#include "Channels/MovieSceneChannel.h"
#include "ScopedTransaction.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#define LOCTEXT_NAMESPACE "MonolithLevelSequenceRetime"

using namespace MonolithLevelSequence::Content;

namespace
{
	struct FRetimeParams
	{
		FSequenceContext Ctx;
		int32 At = 0;
		int32 Frames = 0;
		FFrameNumber AtTicks;
		FFrameNumber DeltaTicks;
		FFrameNumber LowTicks;   // start of the removed range when Frames < 0
		TSet<FString> WholeClasses;
		bool bApply = false;
	};

	struct FRetimeReport
	{
		TArray<TSharedPtr<FJsonValue>> SectionsChanged;
		TArray<TSharedPtr<FJsonValue>> SectionsLeft;
		TArray<TSharedPtr<FJsonValue>> KeysDeleted;
		TArray<TSharedPtr<FJsonValue>> KeysLeft;
		TArray<TSharedPtr<FJsonValue>> MarkedFramesLeft;
		int32 KeysMoved = 0;
		int32 MarkedFramesMoved = 0;
	};

	/** Where a section lives, for report rows. */
	struct FSectionWhere
	{
		FString Owner;
		FString TrackClass;
		int32 TrackIndex = 0;
		int32 SectionIndex = 0;
		FString SectionClass;

		TSharedPtr<FJsonObject> ToJson() const
		{
			TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetStringField(TEXT("owner"), Owner);
			Out->SetStringField(TEXT("track_class"), TrackClass);
			Out->SetNumberField(TEXT("track_index"), TrackIndex);
			Out->SetNumberField(TEXT("section_index"), SectionIndex);
			Out->SetStringField(TEXT("section_class"), SectionClass);
			return Out;
		}
	};

	TSharedPtr<FJsonValue> RangeJson(const FSequenceContext& Ctx, const TRange<FFrameNumber>& Range)
	{
		TArray<TSharedPtr<FJsonValue>> Pair;
		Pair.Add(Range.HasLowerBound() ? StaticCastSharedRef<FJsonValue>(MakeShared<FJsonValueNumber>(Ctx.ToDisplayFrame(Range.GetLowerBoundValue()))) : StaticCastSharedRef<FJsonValue>(MakeShared<FJsonValueNull>()));
		Pair.Add(Range.HasUpperBound() ? StaticCastSharedRef<FJsonValue>(MakeShared<FJsonValueNumber>(Ctx.ToDisplayFrame(Range.GetUpperBoundValue()))) : StaticCastSharedRef<FJsonValue>(MakeShared<FJsonValueNull>()));
		return MakeShared<FJsonValueArray>(Pair);
	}

	TSharedPtr<FJsonValue> KeyRow(const FRetimeParams& P, const FSectionWhere& Where, const FChannelRef& Channel, FFrameNumber Ticks)
	{
		TSharedPtr<FJsonObject> Row = Where.ToJson();
		Row->SetStringField(TEXT("channel"), Channel.Name.IsEmpty() ? FString::Printf(TEXT("#%d"), Channel.Index) : Channel.Name);
		const FFrameTime Time = P.Ctx.ToDisplay(Ticks);
		Row->SetNumberField(TEXT("frame"), Time.FrameNumber.Value);
		if (Time.GetSubFrame() > 0.0001f)
		{
			Row->SetNumberField(TEXT("sub_frame"), Time.GetSubFrame());
		}
		return MakeShared<FJsonValueObject>(Row);
	}

	/** Keys of one channel: delete the collision, move keys at/after the threshold, report keys left in the removed range. */
	void RetimeChannel(const FRetimeParams& P, const FSectionWhere& Where, const FChannelRef& Channel, bool bMoveAll, FRetimeReport& Report)
	{
		FMovieSceneChannel* Ch = Channel.Channel;
		TArray<FFrameNumber> Times;
		TArray<FKeyHandle> Handles;
		Ch->GetKeys(TRange<FFrameNumber>::All(), &Times, &Handles);
		if (Times.Num() == 0)
		{
			return;
		}

		const bool bRemoving = P.Frames < 0;
		TSet<int32> Deleted;
		if (bRemoving && !bMoveAll && Times.Contains(P.AtTicks))
		{
			// The key at F lands on F+N: a key already there would end up doubled.
			TArray<FKeyHandle> ToDelete;
			for (int32 i = 0; i < Times.Num(); ++i)
			{
				if (Times[i] == P.LowTicks)
				{
					ToDelete.Add(Handles[i]);
					Deleted.Add(i);
					Report.KeysDeleted.Add(KeyRow(P, Where, Channel, Times[i]));
				}
			}
			if (P.bApply && ToDelete.Num() > 0)
			{
				Ch->DeleteKeys(ToDelete);
			}
		}

		TArray<FKeyHandle> MoveHandles;
		TArray<FFrameNumber> MoveTimes;
		for (int32 i = 0; i < Times.Num(); ++i)
		{
			if (Deleted.Contains(i))
			{
				continue;
			}
			if (bMoveAll || Times[i] >= P.AtTicks)
			{
				MoveHandles.Add(Handles[i]);
				MoveTimes.Add(Times[i] + P.DeltaTicks);
			}
			else if (bRemoving && Times[i] >= P.LowTicks)
			{
				Report.KeysLeft.Add(KeyRow(P, Where, Channel, Times[i]));
			}
		}
		Report.KeysMoved += MoveHandles.Num();
		if (P.bApply && MoveHandles.Num() > 0)
		{
			Ch->SetKeyTimes(MoveHandles, MoveTimes);
		}
	}

	void RetimeSection(const FRetimeParams& P, const FSectionWhere& Where, UMovieSceneSection* Section, FRetimeReport& Report)
	{
		const TRange<FFrameNumber> Range = Section->GetRange();
		const bool bHasStart = Range.HasLowerBound();
		const bool bHasEnd = Range.HasUpperBound();
		const FFrameNumber S = bHasStart ? Range.GetLowerBoundValue() : FFrameNumber(TNumericLimits<int32>::Lowest());
		const FFrameNumber E = bHasEnd ? Range.GetUpperBoundValue() : FFrameNumber(TNumericLimits<int32>::Max());
		const bool bWholeClass = P.WholeClasses.Contains(Where.SectionClass);

		TRange<FFrameNumber> NewRange = Range;
		FString Note;
		bool bMoveAllKeys = false;
		if (bHasStart && S >= P.AtTicks)
		{
			NewRange.SetLowerBoundValue(S + P.DeltaTicks);
			if (bHasEnd)
			{
				NewRange.SetUpperBoundValue(E + P.DeltaTicks);
			}
		}
		else if (bHasEnd && E > P.AtTicks)
		{
			if (bWholeClass && bHasStart)
			{
				NewRange.SetLowerBoundValue(S + P.DeltaTicks);
				NewRange.SetUpperBoundValue(E + P.DeltaTicks);
				Note = TEXT("moved whole, keys included");
				bMoveAllKeys = true;
			}
			else if (bHasStart && E + P.DeltaTicks <= S)
			{
				TSharedPtr<FJsonObject> Row = Where.ToJson();
				Row->SetField(TEXT("range"), RangeJson(P.Ctx, Range));
				Row->SetStringField(TEXT("reason"), TEXT("starts inside the removed range and would end before its start: left in place"));
				Report.SectionsLeft.Add(MakeShared<FJsonValueObject>(Row));
				NewRange = Range;
			}
			else
			{
				NewRange.SetUpperBoundValue(E + P.DeltaTicks);
				Note = FString::Printf(TEXT("spans %d: end moved"), P.At);
			}
		}
		else if (P.Frames < 0 && bHasEnd && E > P.LowTicks)
		{
			TSharedPtr<FJsonObject> Row = Where.ToJson();
			Row->SetField(TEXT("range"), RangeJson(P.Ctx, Range));
			Row->SetStringField(TEXT("reason"), TEXT("ends inside the removed range: left in place"));
			Report.SectionsLeft.Add(MakeShared<FJsonValueObject>(Row));
		}

		if (P.bApply)
		{
			Section->Modify();
		}
		for (const FChannelRef& Channel : GetChannels(Section))
		{
			RetimeChannel(P, Where, Channel, bMoveAllKeys, Report);
		}

		if (NewRange != Range)
		{
			TSharedPtr<FJsonObject> Row = Where.ToJson();
			Row->SetField(TEXT("from"), RangeJson(P.Ctx, Range));
			Row->SetField(TEXT("to"), RangeJson(P.Ctx, NewRange));
			if (!Note.IsEmpty())
			{
				Row->SetStringField(TEXT("note"), Note);
			}
			Report.SectionsChanged.Add(MakeShared<FJsonValueObject>(Row));
			if (P.bApply)
			{
				Section->SetRange(NewRange);
			}
		}
	}

	void RetimeTracks(const FRetimeParams& P, const FString& Owner, const FGuid& BindingGuid, FRetimeReport& Report)
	{
		const TArray<UMovieSceneTrack*> Tracks = GetScopeTracks(P.Ctx, BindingGuid);
		for (UMovieSceneTrack* Track : Tracks)
		{
			FSectionWhere Where;
			Where.Owner = Owner;
			Where.TrackClass = Track->GetClass()->GetName();
			Where.TrackIndex = GetTrackIndexInClass(Tracks, Track);
			if (P.bApply)
			{
				Track->Modify();
			}
			const TArray<UMovieSceneSection*>& Sections = Track->GetAllSections();
			for (int32 i = 0; i < Sections.Num(); ++i)
			{
				if (UMovieSceneSection* Section = Sections[i])
				{
					Where.SectionIndex = i;
					Where.SectionClass = Section->GetClass()->GetName();
					RetimeSection(P, Where, Section, Report);
				}
			}
		}
	}
}

FMonolithActionResult FMonolithLevelSequenceContentActions::Retime(const TSharedPtr<FJsonObject>& Params)
{
	FRetimeParams P;
	FString Error;
	if (!LoadSequence(Params, P.Ctx, Error))
	{
		return FMonolithActionResult::Error(Error);
	}
	UMovieScene* MS = P.Ctx.MovieScene;

	if (!TryGetFrame(Params, TEXT("at"), P.At, Error) || !TryGetFrame(Params, TEXT("frames"), P.Frames, Error))
	{
		return FMonolithActionResult::Error(Error.IsEmpty() ? FString(TEXT("'at' and 'frames' are required integers (display frames)")) : Error);
	}
	if (P.Frames == 0)
	{
		return FMonolithActionResult::Error(TEXT("'frames' must not be 0: N > 0 inserts, N < 0 removes"));
	}

	bool bDryRun = true;
	Params->TryGetBoolField(TEXT("dry_run"), bDryRun);
	P.bApply = !bDryRun;
	if (P.bApply && MS->IsReadOnly())
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("%s is locked (read-only) in Sequencer; unlock it first"), *P.Ctx.AssetPath));
	}

	const TArray<TSharedPtr<FJsonValue>>* Whole = nullptr;
	if (Params->TryGetArrayField(TEXT("move_whole_classes"), Whole) && Whole)
	{
		for (const TSharedPtr<FJsonValue>& Value : *Whole)
		{
			FString Name;
			if (Value->TryGetString(Name) && !Name.IsEmpty())
			{
				P.WholeClasses.Add(Name);
				if (Name.Len() > 1 && Name[0] == TEXT('U'))
				{
					P.WholeClasses.Add(Name.Mid(1));
				}
			}
		}
	}

	P.AtTicks = P.Ctx.ToTicks(P.At);
	P.DeltaTicks = P.Ctx.ToTicks(P.Frames) - P.Ctx.ToTicks(0);
	P.LowTicks = P.AtTicks + P.DeltaTicks;

	const TRange<FFrameNumber> Playback = MS->GetPlaybackRange();
	if (P.Frames < 0 && P.LowTicks < Playback.GetLowerBoundValue())
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Removing %d frames before %d would start before the playback start (%d)"),
			-P.Frames, P.At, P.Ctx.ToDisplayFrame(Playback.GetLowerBoundValue())));
	}
	const FFrameNumber NewPlaybackEnd = Playback.GetUpperBoundValue() + P.DeltaTicks;
	if (NewPlaybackEnd <= Playback.GetLowerBoundValue())
	{
		return FMonolithActionResult::Error(TEXT("The playback range would end at or before its start"));
	}

	FRetimeReport Report;
	TUniquePtr<FScopedTransaction> Transaction;
	if (P.bApply)
	{
		Transaction = MakeUnique<FScopedTransaction>(LOCTEXT("Retime", "Monolith: level_sequence retime"));
		MS->Modify();
	}

	RetimeTracks(P, TEXT("<sequence>"), FGuid(), Report);
	for (const FMovieSceneBinding& Binding : static_cast<const UMovieScene*>(MS)->GetBindings())
	{
		RetimeTracks(P, GetBindingDisplayName(P.Ctx, Binding.GetObjectGuid()), Binding.GetObjectGuid(), Report);
	}

	// Marked frames
	const TArray<FMovieSceneMarkedFrame> Marks = MS->GetMarkedFrames();
	for (int32 i = 0; i < Marks.Num(); ++i)
	{
		const FFrameNumber F = Marks[i].FrameNumber;
		if (F >= P.AtTicks)
		{
			++Report.MarkedFramesMoved;
			if (P.bApply)
			{
				MS->SetMarkedFrame(i, F + P.DeltaTicks);
			}
		}
		else if (P.Frames < 0 && F >= P.LowTicks)
		{
			TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("label"), Marks[i].Label);
			Row->SetNumberField(TEXT("frame"), P.Ctx.ToDisplayFrame(F));
			Report.MarkedFramesLeft.Add(MakeShared<FJsonValueObject>(Row));
		}
	}

	// Ranges
	const double DeltaSeconds = P.Ctx.FramesToSeconds(P.Frames);
	if (P.bApply)
	{
		MS->SortMarkedFrames();
		MS->SetPlaybackRange(TRange<FFrameNumber>(Playback.GetLowerBoundValue(), NewPlaybackEnd));
#if WITH_EDITORONLY_DATA
		const FMovieSceneEditorData& EditorData = MS->GetEditorData();
		const double ViewStart = EditorData.ViewStart, ViewEnd = EditorData.ViewEnd + DeltaSeconds;
		const double WorkStart = EditorData.WorkStart, WorkEnd = EditorData.WorkEnd + DeltaSeconds;
		if (ViewEnd > ViewStart)
		{
			MS->SetViewRange(ViewStart, ViewEnd);
		}
		if (WorkEnd > WorkStart)
		{
			MS->SetWorkingRange(WorkStart, WorkEnd);
		}
#endif
	}
	Transaction.Reset();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("dry_run"), bDryRun);
	Result->SetStringField(TEXT("operation"), P.Frames > 0 ? TEXT("insert") : TEXT("remove"));
	Result->SetNumberField(TEXT("at"), P.At);
	Result->SetNumberField(TEXT("frames"), P.Frames);
	if (P.Frames < 0)
	{
		TArray<TSharedPtr<FJsonValue>> Removed;
		Removed.Add(MakeShared<FJsonValueNumber>(P.At + P.Frames));
		Removed.Add(MakeShared<FJsonValueNumber>(P.At));
		Result->SetArrayField(TEXT("removed_range"), Removed);
	}
	Result->SetNumberField(TEXT("keys_moved"), Report.KeysMoved);
	Result->SetArrayField(TEXT("keys_deleted"), Report.KeysDeleted);
	Result->SetArrayField(TEXT("keys_left_in_removed_range"), Report.KeysLeft);
	Result->SetArrayField(TEXT("sections_changed"), Report.SectionsChanged);
	Result->SetArrayField(TEXT("sections_left"), Report.SectionsLeft);
	Result->SetNumberField(TEXT("marked_frames_moved"), Report.MarkedFramesMoved);
	Result->SetArrayField(TEXT("marked_frames_left_in_removed_range"), Report.MarkedFramesLeft);
	TSharedPtr<FJsonObject> PlaybackJson = MakeShared<FJsonObject>();
	PlaybackJson->SetNumberField(TEXT("start"), P.Ctx.ToDisplayFrame(Playback.GetLowerBoundValue()));
	PlaybackJson->SetNumberField(TEXT("end_before"), P.Ctx.ToDisplayFrame(Playback.GetUpperBoundValue()));
	PlaybackJson->SetNumberField(TEXT("end_after"), P.Ctx.ToDisplayFrame(NewPlaybackEnd));
	Result->SetObjectField(TEXT("playback"), PlaybackJson);

	if (bDryRun)
	{
		Result->SetStringField(TEXT("asset_path"), P.Ctx.AssetPath);
		Result->SetStringField(TEXT("next"), TEXT("Nothing changed. Read the report, then call again with dry_run=false."));
		return FMonolithActionResult::Success(Result);
	}
	return FinishWrite(P.Ctx, Params, Result);
}

#undef LOCTEXT_NAMESPACE
