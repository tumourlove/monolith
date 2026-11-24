#include "MonolithLevelSequenceContentActions.h"
#include "MonolithLevelSequenceContentUtils.h"
#include "MonolithLevelSequenceBindingUtils.h"
#include "MonolithParamSchema.h"
#include "MonolithAssetUtils.h"

#include "MovieScene.h"
#include "MovieSceneBinding.h"
#include "MovieSceneSequence.h"
#include "MovieSceneSection.h"
#include "MovieSceneTrack.h"
#include "MovieSceneMarkedFrame.h"
#include "MovieScenePossessable.h"
#include "MovieSceneSpawnable.h"
#include "MovieSceneBindingReferences.h"
#include "MovieSceneObjectBindingID.h"
#include "Bindings/MovieSceneCustomBinding.h"
#include "Bindings/MovieSceneSpawnableBinding.h"
#include "Channels/MovieSceneChannel.h"
#include "Channels/MovieSceneFloatChannel.h"
#include "Channels/MovieSceneDoubleChannel.h"
#include "Channels/MovieSceneBoolChannel.h"
#include "Channels/MovieSceneIntegerChannel.h"
#include "Channels/MovieSceneByteChannel.h"
#include "Generators/MovieSceneEasingCurves.h"
#include "Sections/MovieSceneSkeletalAnimationSection.h"
#include "Sections/MovieSceneCameraCutSection.h"
#include "Sections/MovieSceneSubSection.h"
#include "Tracks/MovieSceneCameraCutTrack.h"
#include "Animation/AnimSequenceBase.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "GameFramework/Actor.h"
#include "ScopedTransaction.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#if WITH_LEVELSEQUENCEEDITOR
#include "LevelSequenceEditorBlueprintLibrary.h"
#include "LevelSequence.h"
#endif

#define LOCTEXT_NAMESPACE "MonolithLevelSequenceContent"

using namespace MonolithLevelSequence::Content;

// ============================================================================
// Registration
// ============================================================================

namespace
{
	const TCHAR* const NS = TEXT("level_sequence");

	FParamSchemaBuilder& AddTrackTargetParams(FParamSchemaBuilder& Builder)
	{
		return Builder
			.Optional(TEXT("binding"), TEXT("string"), TEXT("Binding GUID or display name; omit for sequence tracks (camera cuts, audio, subsequences...)"))
			.Required(TEXT("track_class"), TEXT("string"), TEXT("Exact track class, e.g. MovieSceneSkeletalAnimationTrack, MovieScene3DTransformTrack, MovieSceneCameraCutTrack"))
			.Optional(TEXT("track_index"), TEXT("integer"), TEXT("Index among the tracks of that class in the scope (default 0), as listed by get_sequence_contents"), TEXT("0"));
	}

	FParamSchemaBuilder& AddSectionTargetParams(FParamSchemaBuilder& Builder)
	{
		return AddTrackTargetParams(Builder)
			.Optional(TEXT("section_index"), TEXT("integer"), TEXT("Index of the section in the track (default 0), as listed by get_sequence_contents"), TEXT("0"));
	}

	FParamSchemaBuilder& AddSectionFieldParams(FParamSchemaBuilder& Builder)
	{
		return Builder
			.Optional(TEXT("row"), TEXT("integer"), TEXT("Row index in the track"))
			.Optional(TEXT("active"), TEXT("boolean"), TEXT("Section active (evaluated) or muted"))
			.Optional(TEXT("ease_in"), TEXT("any"), TEXT("Manual ease-in length in display frames, or \"auto\" to go back to automatic easing"))
			.Optional(TEXT("ease_out"), TEXT("any"), TEXT("Manual ease-out length in display frames, or \"auto\""))
			.Optional(TEXT("ease_in_type"), TEXT("string"), TEXT("Built-in easing function (EMovieSceneBuiltInEasing): Linear, SinIn, SinOut, SinInOut, QuadIn..., Cubic, CubicInOut, HermiteCubicInOut, ExpoIn..., CircInOut"))
			.Optional(TEXT("ease_out_type"), TEXT("string"), TEXT("Built-in easing function for the ease-out"))
			.OptionalAssetPath(TEXT("animation"), TEXT("Skeletal animation sections: UAnimSequenceBase asset path"))
			.Optional(TEXT("first_loop_start_offset"), TEXT("number"), TEXT("Skeletal animation sections: offset into the animation for the first loop, in display frames"))
			.Optional(TEXT("start_offset"), TEXT("number"), TEXT("Skeletal animation sections: frames trimmed from the start of the animation"))
			.Optional(TEXT("end_offset"), TEXT("number"), TEXT("Skeletal animation sections: frames trimmed from the end of the animation"))
			.Optional(TEXT("play_rate"), TEXT("number"), TEXT("Skeletal animation sections: fixed play rate"))
			.Optional(TEXT("slot"), TEXT("string"), TEXT("Skeletal animation sections: animation slot name"))
			.Optional(TEXT("camera_binding"), TEXT("string"), TEXT("Camera cut sections: GUID or display name of the camera binding"))
			.OptionalAssetPath(TEXT("sequence"), TEXT("Subsequence / shot sections: the sequence asset path"));
	}

	TSharedPtr<FJsonObject> Build(FParamSchemaBuilder& Builder)
	{
		return Builder.Build();
	}
}

void FMonolithLevelSequenceContentActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	{
		FParamSchemaBuilder B;
		B.RequiredAssetPath(TEXT("asset_path"), TEXT("Level Sequence asset path"))
			.Optional(TEXT("binding"), TEXT("string"), TEXT("Only this binding (GUID or display name) and its children; sequence tracks are left out"))
			.Optional(TEXT("track_class"), TEXT("string"), TEXT("Only tracks of this exact class"))
			.Optional(TEXT("include_keys"), TEXT("boolean"), TEXT("List keys (default true); false lists channels with key counts only"), TEXT("true"))
			.Optional(TEXT("max_keys"), TEXT("integer"), TEXT("Keys listed per channel (default 40, 0 = all)"), TEXT("40"));
		Registry.RegisterAction(NS, TEXT("get_sequence_contents"),
			TEXT("Read a Level Sequence from the asset (not the index): display rate and tick resolution, playback / view / work ranges, marked frames, sequence tracks (camera cuts, audio, subsequences), then every binding (kind, template class, skeletal mesh, child possessables nested) with its tracks, sections (range, row, active, easing, animation + offsets, camera binding, subsequence) and every channel's keys (frame, value, interpolation, tangents) or default. All frames are display frames. Track and section indexes in the output are what the write actions take."),
			FMonolithActionHandler::CreateStatic(&GetSequenceContents), Build(B));
	}
	{
		FParamSchemaBuilder B;
		Registry.RegisterAction(NS, TEXT("refresh_current_sequence"),
			TEXT("Refresh the Sequencer editor after a write (writes don't notify an open Sequencer) and return the asset path of the sequence open in it, or null when none is open. Needs the LevelSequenceEditor plugin (enabled by default)."),
			FMonolithActionHandler::CreateStatic(&RefreshCurrentSequence), Build(B));
	}
	{
		FParamSchemaBuilder B;
		B.RequiredAssetPath(TEXT("asset_path"), TEXT("Level Sequence asset path"))
			.Required(TEXT("at"), TEXT("integer"), TEXT("Display frame F where time is inserted or removed"))
			.Required(TEXT("frames"), TEXT("integer"), TEXT("N > 0 inserts N frames at F; N < 0 removes the |N| frames just before F (the range [F+N, F)), so what was at F lands on F+N"))
			.Optional(TEXT("move_whole_classes"), TEXT("array"), TEXT("Section classes (e.g. MovieSceneAudioSection) that move whole, keys included, when they span F, instead of being stretched"))
			.Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("Report what would change without changing anything (default true)"), TEXT("true"))
			.Optional(TEXT("save"), TEXT("boolean"), TEXT("Save after a real run (default true)"), TEXT("true"));
		Registry.RegisterAction(NS, TEXT("retime"),
			TEXT("Insert or remove time across a whole Level Sequence. Every key at F or later moves by N in every channel of every track; when removing, a key exactly at F+N is deleted if the same channel has a key at F, and other keys inside the removed range stay and are reported. A bounded section starting at F or later moves by N; one spanning F keeps its start and its end moves by N (or moves whole, keys included, for move_whole_classes); one ending inside the removed range is reported. Marked frames at F or later move by N. The playback, view and work range ends move by N. dry_run defaults to true."),
			FMonolithActionHandler::CreateStatic(&Retime), Build(B));
	}
	{
		FParamSchemaBuilder B;
		B.RequiredAssetPath(TEXT("asset_path"), TEXT("Level Sequence asset path"))
			.Optional(TEXT("binding"), TEXT("string"), TEXT("Binding GUID or display name; omit to add a sequence track"))
			.Required(TEXT("track_class"), TEXT("string"), TEXT("Track class to add, e.g. MovieSceneSkeletalAnimationTrack, MovieScene3DTransformTrack, MovieSceneCameraCutTrack, MovieSceneSubTrack"))
			.Optional(TEXT("save"), TEXT("boolean"), TEXT("Save after the change (default true)"), TEXT("true"));
		Registry.RegisterAction(NS, TEXT("add_track"),
			TEXT("Add a track to a binding, or to the sequence when binding is omitted (a camera cut track goes in the sequence's camera cut slot, one per sequence). Returns the new track's class and track_index."),
			FMonolithActionHandler::CreateStatic(&AddTrack), Build(B));
	}
	{
		FParamSchemaBuilder B;
		B.RequiredAssetPath(TEXT("asset_path"), TEXT("Level Sequence asset path"));
		AddTrackTargetParams(B).Optional(TEXT("save"), TEXT("boolean"), TEXT("Save after the change (default true)"), TEXT("true"));
		Registry.RegisterAction(NS, TEXT("remove_track"),
			TEXT("Remove a track (and its sections and keys) from a binding or from the sequence."),
			FMonolithActionHandler::CreateStatic(&RemoveTrack), Build(B));
	}
	{
		FParamSchemaBuilder B;
		B.RequiredAssetPath(TEXT("asset_path"), TEXT("Level Sequence asset path"));
		AddTrackTargetParams(B)
			.Required(TEXT("start"), TEXT("integer"), TEXT("Start display frame"))
			.Optional(TEXT("end"), TEXT("integer"), TEXT("End display frame (exclusive). Defaults to the animation's length for skeletal animation sections and to the subsequence's length for subsequence sections"));
		AddSectionFieldParams(B).Optional(TEXT("save"), TEXT("boolean"), TEXT("Save after the change (default true)"), TEXT("true"));
		Registry.RegisterAction(NS, TEXT("add_section"),
			TEXT("Add a section to a track, with its range, row, easing and the fields of its type: skeletal animation (animation, offsets, play_rate, slot), camera cut (camera_binding), subsequence (sequence). Back-to-back sections on one row cut from one animation to the next; to cross-fade, overlap them on two rows and set ease_in / ease_out. Returns the new section."),
			FMonolithActionHandler::CreateStatic(&AddSection), Build(B));
	}
	{
		FParamSchemaBuilder B;
		B.RequiredAssetPath(TEXT("asset_path"), TEXT("Level Sequence asset path"));
		AddSectionTargetParams(B)
			.Optional(TEXT("move_by"), TEXT("integer"), TEXT("Move the whole section by N display frames, its keys included (like dragging it in Sequencer). Applied before start/end"))
			.Optional(TEXT("start"), TEXT("integer"), TEXT("New start display frame. Changes the range only: keys stay where they are"))
			.Optional(TEXT("end"), TEXT("integer"), TEXT("New end display frame (exclusive). Changes the range only"));
		AddSectionFieldParams(B).Optional(TEXT("save"), TEXT("boolean"), TEXT("Save after the change (default true)"), TEXT("true"));
		Registry.RegisterAction(NS, TEXT("set_section"),
			TEXT("Change a section: move_by (moves the section with its keys), start / end (range only; keys stay), row, active, easing, and the fields of its type (skeletal animation, camera cut, subsequence). Returns the section after the change."),
			FMonolithActionHandler::CreateStatic(&SetSection), Build(B));
	}
	{
		FParamSchemaBuilder B;
		B.RequiredAssetPath(TEXT("asset_path"), TEXT("Level Sequence asset path"));
		AddSectionTargetParams(B).Optional(TEXT("save"), TEXT("boolean"), TEXT("Save after the change (default true)"), TEXT("true"));
		Registry.RegisterAction(NS, TEXT("remove_section"),
			TEXT("Remove a section (and its keys) from a track. Later sections' indexes shift down by one."),
			FMonolithActionHandler::CreateStatic(&RemoveSection), Build(B));
	}
	{
		FParamSchemaBuilder B;
		B.RequiredAssetPath(TEXT("asset_path"), TEXT("Level Sequence asset path"));
		AddSectionTargetParams(B)
			.Required(TEXT("channel"), TEXT("string"), TEXT("Channel name (Location.X, Rotation.Z, Weight...) or #index from get_sequence_contents"))
			.Required(TEXT("keys"), TEXT("array"), TEXT("[{frame, sub_frame?, value, interp?, tangent_mode?, arrive?, leave?}]. interp: constant | linear | cubic, or the shorthands auto / smart_auto / user / break (cubic with that tangent mode). tangent_mode: auto | smart_auto | user | break. arrive / leave: raw stored tangents (imply user unless break). interp and tangents apply to float/double channels; byte channels with an enum take the enumerator name"))
			.Optional(TEXT("save"), TEXT("boolean"), TEXT("Save after the change (default true)"), TEXT("true"));
		Registry.RegisterAction(NS, TEXT("set_keys"),
			TEXT("Add or update keys on one channel (float, double, bool, integer, byte/enum). A key at the same frame (and sub-frame) is updated, otherwise one is added; new float/double keys default to cubic with auto tangents. To flatten the tangents where two motions meet: interp cubic, tangent_mode user, arrive 0, leave 0. To hold a value until the next key: interp constant. Auto tangents are recomputed after the change."),
			FMonolithActionHandler::CreateStatic(&SetKeys), Build(B));
	}
	{
		FParamSchemaBuilder B;
		B.RequiredAssetPath(TEXT("asset_path"), TEXT("Level Sequence asset path"));
		AddSectionTargetParams(B)
			.Required(TEXT("channel"), TEXT("string"), TEXT("Channel name or #index"))
			.Required(TEXT("frames"), TEXT("array"), TEXT("Frames of the keys to remove: integers, or {frame, sub_frame} objects"))
			.Optional(TEXT("save"), TEXT("boolean"), TEXT("Save after the change (default true)"), TEXT("true"));
		Registry.RegisterAction(NS, TEXT("remove_keys"),
			TEXT("Remove the keys at the given frames from one channel (any channel type). Frames with no key are reported in not_found."),
			FMonolithActionHandler::CreateStatic(&RemoveKeys), Build(B));
	}
	{
		FParamSchemaBuilder B;
		B.RequiredAssetPath(TEXT("asset_path"), TEXT("Level Sequence asset path"));
		AddSectionTargetParams(B)
			.Required(TEXT("channel"), TEXT("string"), TEXT("Channel name or #index"))
			.Required(TEXT("value"), TEXT("any"), TEXT("Default value (number, bool, or enumerator name), or null to remove the default"))
			.Optional(TEXT("save"), TEXT("boolean"), TEXT("Save after the change (default true)"), TEXT("true"));
		Registry.RegisterAction(NS, TEXT("set_channel_default"),
			TEXT("Set or remove the default value of a channel: the value it has when it has no key."),
			FMonolithActionHandler::CreateStatic(&SetChannelDefault), Build(B));
	}
	{
		FParamSchemaBuilder B;
		B.RequiredAssetPath(TEXT("asset_path"), TEXT("Level Sequence asset path"))
			.Required(TEXT("op"), TEXT("string"), TEXT("add | set | remove | clear"))
			.Optional(TEXT("index"), TEXT("integer"), TEXT("set / remove: marked frame index"))
			.Optional(TEXT("label"), TEXT("string"), TEXT("add: label (default A, B, C...). set / remove: find the marked frame by label when index is omitted"))
			.Optional(TEXT("frame"), TEXT("integer"), TEXT("add (required) / set: display frame"))
			.Optional(TEXT("sub_frame"), TEXT("number"), TEXT("Sub-frame in [0, 1)"))
			.Optional(TEXT("new_label"), TEXT("string"), TEXT("set: new label"))
			.Optional(TEXT("comment"), TEXT("string"), TEXT("add / set: comment"))
			.Optional(TEXT("save"), TEXT("boolean"), TEXT("Save after the change (default true)"), TEXT("true"));
		Registry.RegisterAction(NS, TEXT("set_marked_frames"),
			TEXT("Add, change, remove or clear marked frames (timeline markers). Marked frames stay sorted by frame, so indexes can change: the response lists them all."),
			FMonolithActionHandler::CreateStatic(&SetMarkedFrames), Build(B));
	}
	{
		FParamSchemaBuilder B;
		B.RequiredAssetPath(TEXT("asset_path"), TEXT("Level Sequence asset path"))
			.Optional(TEXT("start"), TEXT("integer"), TEXT("Playback start display frame"))
			.Optional(TEXT("end"), TEXT("integer"), TEXT("Playback end display frame (exclusive)"))
			.Optional(TEXT("view_start"), TEXT("number"), TEXT("View range start, display frames"))
			.Optional(TEXT("view_end"), TEXT("number"), TEXT("View range end, display frames"))
			.Optional(TEXT("work_start"), TEXT("number"), TEXT("Working range start, display frames"))
			.Optional(TEXT("work_end"), TEXT("number"), TEXT("Working range end, display frames"))
			.Optional(TEXT("save"), TEXT("boolean"), TEXT("Save after the change (default true)"), TEXT("true"));
		Registry.RegisterAction(NS, TEXT("set_playback_range"),
			TEXT("Set the playback range and, optionally, the view and working ranges (all in display frames). Omitted bounds keep their value."),
			FMonolithActionHandler::CreateStatic(&SetPlaybackRange), Build(B));
	}
}

// ============================================================================
// Shared helpers for the handlers
// ============================================================================

namespace
{
	FMonolithActionResult Fail(const FString& Message)
	{
		return FMonolithActionResult::Error(Message);
	}

	bool LoadForWrite(const TSharedPtr<FJsonObject>& Params, FSequenceContext& Ctx, FString& OutError)
	{
		if (!LoadSequence(Params, Ctx, OutError))
		{
			return false;
		}
		if (Ctx.MovieScene->IsReadOnly())
		{
			OutError = FString::Printf(TEXT("%s is locked (read-only) in Sequencer; unlock it first"), *Ctx.AssetPath);
			return false;
		}
		return true;
	}

	FText TransactionText(const TCHAR* Action)
	{
		return FText::Format(LOCTEXT("MonolithLevelSequenceWrite", "Monolith: level_sequence {0}"), FText::FromString(Action));
	}

	/** Display frame + sub-frame of a JSON key / frame entry. */
	bool ParseFrameEntry(const TSharedPtr<FJsonValue>& Value, int32& OutFrame, double& OutSubFrame, FString& OutError)
	{
		OutSubFrame = 0.0;
		double Number = 0.0;
		if (Value->Type == EJson::Number && Value->TryGetNumber(Number))
		{
			if (FMath::Frac(Number) != 0.0)
			{
				OutError = TEXT("frames are integers; use {frame, sub_frame} for a sub-frame");
				return false;
			}
			OutFrame = static_cast<int32>(Number);
			return true;
		}
		const TSharedPtr<FJsonObject>* Object = nullptr;
		if (Value->TryGetObject(Object) && Object && (*Object)->HasField(TEXT("frame")))
		{
			const double Frame = (*Object)->GetNumberField(TEXT("frame"));
			if (FMath::Frac(Frame) != 0.0)
			{
				OutError = TEXT("'frame' must be an integer; put the fraction in sub_frame");
				return false;
			}
			OutFrame = static_cast<int32>(Frame);
			(*Object)->TryGetNumberField(TEXT("sub_frame"), OutSubFrame);
			if (OutSubFrame < 0.0 || OutSubFrame >= 1.0)
			{
				OutError = TEXT("'sub_frame' must be in [0, 1)");
				return false;
			}
			return true;
		}
		OutError = TEXT("each entry needs an integer frame (or a {frame, sub_frame} object)");
		return false;
	}

	// ------------------------------------------------------------------------
	// Bindings (for get_sequence_contents)
	// ------------------------------------------------------------------------

	UObject* GetBindingTemplate(const FSequenceContext& Ctx, const FGuid& Guid)
	{
		if (FMovieSceneSpawnable* Spawnable = Ctx.MovieScene->FindSpawnable(Guid))
		{
			return Spawnable->GetObjectTemplate();
		}
		if (FMovieSceneBindingReferences* Refs = Ctx.Sequence->GetBindingReferences())
		{
			if (UMovieSceneSpawnableBindingBase* Custom = Cast<UMovieSceneSpawnableBindingBase>(Refs->GetCustomBinding(Guid, 0)))
			{
				return Custom->GetObjectTemplate();
			}
		}
		return nullptr;
	}

	struct FDumpOptions
	{
		UClass* TrackClass = nullptr;
		bool bIncludeKeys = true;
		int32 MaxKeys = 40;
	};

	TArray<TSharedPtr<FJsonValue>> TracksToJson(const FSequenceContext& Ctx, const FGuid& BindingGuid, const FDumpOptions& Options)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		const TArray<UMovieSceneTrack*> Tracks = GetScopeTracks(Ctx, BindingGuid);
		for (UMovieSceneTrack* Track : Tracks)
		{
			if (Options.TrackClass && Track->GetClass() != Options.TrackClass)
			{
				continue;
			}
			Out.Add(MakeShared<FJsonValueObject>(TrackToJson(Ctx, Track, GetTrackIndexInClass(Tracks, Track), true, Options.bIncludeKeys, Options.MaxKeys)));
		}
		return Out;
	}

	/** One binding with its tracks and nested children; null when a track filter leaves nothing in its subtree. */
	TSharedPtr<FJsonObject> BindingToJson(const FSequenceContext& Ctx, const FGuid& Guid, const TMultiMap<FGuid, FGuid>& Children, const FDumpOptions& Options)
	{
		TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetStringField(TEXT("guid"), Guid.ToString(EGuidFormats::DigitsWithHyphens));
		Out->SetStringField(TEXT("name"), GetBindingDisplayName(Ctx, Guid));

		FString Name, Kind, BoundClass, CustomClass, CustomPretty;
		MonolithLevelSequence::ClassifyBinding(Ctx.Sequence, Ctx.MovieScene, Guid, 0, Name, Kind, BoundClass, CustomClass, CustomPretty);
		Out->SetStringField(TEXT("kind"), Kind);
		if (!BoundClass.IsEmpty())
		{
			Out->SetStringField(TEXT("bound_class"), BoundClass);
		}
		if (!CustomClass.IsEmpty())
		{
			Out->SetStringField(TEXT("custom_binding_class"), CustomClass);
		}

		if (UObject* Template = GetBindingTemplate(Ctx, Guid))
		{
			Out->SetStringField(TEXT("template_class"), Template->GetClass()->GetName());
			if (const AActor* Actor = Cast<AActor>(Template))
			{
				if (const USkeletalMeshComponent* Mesh = Actor->FindComponentByClass<USkeletalMeshComponent>())
				{
					const USkeletalMesh* Asset = Mesh->GetSkeletalMeshAsset();
					Out->SetStringField(TEXT("skeletal_mesh"), Asset ? Asset->GetPathName() : FString());
				}
			}
		}

		TArray<TSharedPtr<FJsonValue>> Tracks = TracksToJson(Ctx, Guid, Options);
		const bool bHasTracks = Tracks.Num() > 0;
		Out->SetArrayField(TEXT("tracks"), Tracks);

		TArray<FGuid> ChildGuids;
		Children.MultiFind(Guid, ChildGuids, /*bMaintainOrder=*/true);
		TArray<TSharedPtr<FJsonValue>> ChildJson;
		for (const FGuid& Child : ChildGuids)
		{
			if (TSharedPtr<FJsonObject> ChildObject = BindingToJson(Ctx, Child, Children, Options))
			{
				ChildJson.Add(MakeShared<FJsonValueObject>(ChildObject));
			}
		}
		if (ChildJson.Num() > 0)
		{
			Out->SetArrayField(TEXT("children"), ChildJson);
		}

		if (Options.TrackClass && !bHasTracks && ChildJson.Num() == 0)
		{
			return nullptr;
		}
		return Out;
	}

	// ------------------------------------------------------------------------
	// Section edits (add_section / set_section): parse, check against the section, apply
	// ------------------------------------------------------------------------

	struct FEaseEdit
	{
		bool bSet = false;
		bool bAuto = false;
		int32 Ticks = 0;
		TOptional<EMovieSceneBuiltInEasing> Type;
	};

	struct FSectionEdit
	{
		TOptional<int32> MoveBy;
		TOptional<int32> Start;
		TOptional<int32> End;
		TOptional<int32> Row;
		TOptional<bool> Active;
		FEaseEdit EaseIn;
		FEaseEdit EaseOut;

		UAnimSequenceBase* Animation = nullptr;
		TOptional<double> FirstLoopStartOffset;
		TOptional<double> StartOffset;
		TOptional<double> EndOffset;
		TOptional<double> PlayRate;
		TOptional<FName> Slot;

		TOptional<FGuid> CameraBinding;
		UMovieSceneSequence* SubSequence = nullptr;

		bool HasAnimFields() const { return Animation || FirstLoopStartOffset || StartOffset || EndOffset || PlayRate || Slot; }
	};

	bool ParseEase(const TSharedPtr<FJsonObject>& Params, const TCHAR* LengthField, const TCHAR* TypeField, const FSequenceContext& Ctx, FEaseEdit& Out, FString& OutError)
	{
		if (const TSharedPtr<FJsonValue> Value = Params->TryGetField(LengthField); Value.IsValid() && !Value->IsNull())
		{
			FString AsString;
			double Frames = 0.0;
			if (Value->Type == EJson::String && Value->TryGetString(AsString) && AsString.Equals(TEXT("auto"), ESearchCase::IgnoreCase))
			{
				Out.bSet = true;
				Out.bAuto = true;
			}
			else if (Value->Type == EJson::Number && Value->TryGetNumber(Frames) && Frames >= 0.0)
			{
				Out.bSet = true;
				Out.Ticks = Ctx.DecimalToTicks(Frames).Value;
			}
			else
			{
				OutError = FString::Printf(TEXT("'%s' must be a length in display frames (>= 0) or \"auto\""), LengthField);
				return false;
			}
		}

		FString TypeName;
		if (Params->TryGetStringField(TypeField, TypeName) && !TypeName.IsEmpty())
		{
			const UEnum* Enum = StaticEnum<EMovieSceneBuiltInEasing>();
			int64 Value = Enum->GetValueByNameString(TypeName);
			if (Value == INDEX_NONE)
			{
				for (int32 i = 0; i < Enum->NumEnums() - 1; ++i)
				{
					if (Enum->GetDisplayNameTextByIndex(i).ToString().Equals(TypeName, ESearchCase::IgnoreCase))
					{
						Value = Enum->GetValueByIndex(i);
						break;
					}
				}
			}
			if (Value == INDEX_NONE || Value == static_cast<int64>(EMovieSceneBuiltInEasing::Custom))
			{
				TArray<FString> Names;
				for (int32 i = 0; i < Enum->NumEnums() - 1; ++i)
				{
					if (Enum->GetValueByIndex(i) != static_cast<int64>(EMovieSceneBuiltInEasing::Custom))
					{
						Names.Add(Enum->GetNameStringByIndex(i));
					}
				}
				OutError = FString::Printf(TEXT("Unknown %s '%s'. Use one of: %s"), TypeField, *TypeName, *FString::Join(Names, TEXT(", ")));
				return false;
			}
			Out.Type = static_cast<EMovieSceneBuiltInEasing>(Value);
		}
		return true;
	}

	bool ParseOptionalNumber(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field, TOptional<double>& Out, FString& OutError)
	{
		if (const TSharedPtr<FJsonValue> Value = Params->TryGetField(Field); Value.IsValid() && !Value->IsNull())
		{
			double Number = 0.0;
			if (Value->Type != EJson::Number || !Value->TryGetNumber(Number))
			{
				OutError = FString::Printf(TEXT("'%s' must be a number"), Field);
				return false;
			}
			Out = Number;
		}
		return true;
	}

	bool ParseOptionalInt(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field, TOptional<int32>& Out, FString& OutError)
	{
		int32 Value = 0;
		if (TryGetFrame(Params, Field, Value, OutError))
		{
			Out = Value;
		}
		return OutError.IsEmpty();
	}

	bool ParseSectionEdit(const FSequenceContext& Ctx, const TSharedPtr<FJsonObject>& Params, FSectionEdit& Out, FString& OutError)
	{
		if (!ParseOptionalInt(Params, TEXT("move_by"), Out.MoveBy, OutError)
			|| !ParseOptionalInt(Params, TEXT("start"), Out.Start, OutError)
			|| !ParseOptionalInt(Params, TEXT("end"), Out.End, OutError)
			|| !ParseOptionalInt(Params, TEXT("row"), Out.Row, OutError))
		{
			return false;
		}
		if (Out.Row && Out.Row.GetValue() < 0)
		{
			OutError = TEXT("'row' must be >= 0");
			return false;
		}
		bool bActive = true;
		if (Params->TryGetBoolField(TEXT("active"), bActive))
		{
			Out.Active = bActive;
		}
		if (!ParseEase(Params, TEXT("ease_in"), TEXT("ease_in_type"), Ctx, Out.EaseIn, OutError)
			|| !ParseEase(Params, TEXT("ease_out"), TEXT("ease_out_type"), Ctx, Out.EaseOut, OutError))
		{
			return false;
		}

		FString AnimationPath;
		if (Params->TryGetStringField(TEXT("animation"), AnimationPath) && !AnimationPath.IsEmpty())
		{
			Out.Animation = FMonolithAssetUtils::LoadAssetByPath<UAnimSequenceBase>(AnimationPath);
			if (!Out.Animation)
			{
				OutError = FString::Printf(TEXT("Animation not found (or not an animation sequence / montage / composite): '%s'"), *AnimationPath);
				return false;
			}
		}
		if (!ParseOptionalNumber(Params, TEXT("first_loop_start_offset"), Out.FirstLoopStartOffset, OutError)
			|| !ParseOptionalNumber(Params, TEXT("start_offset"), Out.StartOffset, OutError)
			|| !ParseOptionalNumber(Params, TEXT("end_offset"), Out.EndOffset, OutError)
			|| !ParseOptionalNumber(Params, TEXT("play_rate"), Out.PlayRate, OutError))
		{
			return false;
		}
		if (Out.PlayRate && Out.PlayRate.GetValue() <= 0.0)
		{
			OutError = TEXT("'play_rate' must be > 0 (use the section's reverse option in Sequencer to play backwards)");
			return false;
		}
		FString Slot;
		if (Params->TryGetStringField(TEXT("slot"), Slot) && !Slot.IsEmpty())
		{
			Out.Slot = FName(*Slot);
		}

		FString CameraRef;
		if (Params->TryGetStringField(TEXT("camera_binding"), CameraRef) && !CameraRef.IsEmpty())
		{
			FGuid CameraGuid;
			if (!ResolveBindingRef(Ctx, CameraRef, CameraGuid, OutError))
			{
				return false;
			}
			Out.CameraBinding = CameraGuid;
		}

		FString SequencePath;
		if (Params->TryGetStringField(TEXT("sequence"), SequencePath) && !SequencePath.IsEmpty())
		{
			Out.SubSequence = FMonolithAssetUtils::LoadAssetByPath<UMovieSceneSequence>(SequencePath);
			if (!Out.SubSequence)
			{
				OutError = FString::Printf(TEXT("Sequence not found: '%s'"), *SequencePath);
				return false;
			}
			if (Out.SubSequence == Ctx.Sequence)
			{
				OutError = TEXT("A sequence can't contain itself as a subsequence");
				return false;
			}
		}
		return true;
	}

	/** Type checks and the resulting range, before anything changes. */
	bool CheckSectionEdit(const FSequenceContext& Ctx, UMovieSceneSection* Section, const FSectionEdit& Edit, FString& OutError)
	{
		const FString ClassName = Section->GetClass()->GetName();
		if (Edit.HasAnimFields() && !Section->IsA<UMovieSceneSkeletalAnimationSection>())
		{
			OutError = FString::Printf(TEXT("animation / offsets / play_rate / slot apply to skeletal animation sections, not %s"), *ClassName);
			return false;
		}
		if (Edit.CameraBinding && !Section->IsA<UMovieSceneCameraCutSection>())
		{
			OutError = FString::Printf(TEXT("camera_binding applies to camera cut sections, not %s"), *ClassName);
			return false;
		}
		if (Edit.SubSequence && !Section->IsA<UMovieSceneSubSection>())
		{
			OutError = FString::Printf(TEXT("sequence applies to subsequence / shot sections, not %s"), *ClassName);
			return false;
		}
		if (Section->IsLocked() && (Edit.MoveBy || Edit.Start || Edit.End))
		{
			OutError = TEXT("The section is locked; unlock it in Sequencer before moving or trimming it");
			return false;
		}

		// Range after move_by then start / end.
		TRange<FFrameNumber> Range = Section->GetRange();
		const FFrameNumber Delta = Edit.MoveBy ? Ctx.ToTicks(Edit.MoveBy.GetValue()) - Ctx.ToTicks(0) : FFrameNumber(0);
		if (Range.HasLowerBound())
		{
			Range.SetLowerBoundValue(Range.GetLowerBoundValue() + Delta);
		}
		if (Range.HasUpperBound())
		{
			Range.SetUpperBoundValue(Range.GetUpperBoundValue() + Delta);
		}
		if (Edit.Start)
		{
			Range.SetLowerBound(TRangeBound<FFrameNumber>::Inclusive(Ctx.ToTicks(Edit.Start.GetValue())));
		}
		if (Edit.End)
		{
			Range.SetUpperBound(TRangeBound<FFrameNumber>::Exclusive(Ctx.ToTicks(Edit.End.GetValue())));
		}
		if (Range.HasLowerBound() && Range.HasUpperBound() && Range.GetLowerBoundValue() >= Range.GetUpperBoundValue())
		{
			OutError = TEXT("The section would end at or before its start");
			return false;
		}
		return true;
	}

	void ApplyEase(UMovieSceneSection* Section, const FEaseEdit& Edit, bool bIn)
	{
		FMovieSceneEasingSettings& Easing = Section->Easing;
		if (Edit.bSet)
		{
			bool& bManual = bIn ? Easing.bManualEaseIn : Easing.bManualEaseOut;
			int32& Duration = bIn ? Easing.ManualEaseInDuration : Easing.ManualEaseOutDuration;
			bManual = !Edit.bAuto;
			if (!Edit.bAuto)
			{
				Duration = Edit.Ticks;
			}
		}
		if (Edit.Type)
		{
			TScriptInterface<IMovieSceneEasingFunction>& Function = bIn ? Easing.EaseIn : Easing.EaseOut;
			UMovieSceneBuiltInEasingFunction* BuiltIn = Cast<UMovieSceneBuiltInEasingFunction>(Function.GetObject());
			if (BuiltIn)
			{
				BuiltIn->Modify();
			}
			else
			{
				BuiltIn = NewObject<UMovieSceneBuiltInEasingFunction>(Section, NAME_None, RF_Transactional);
				Function = TScriptInterface<IMovieSceneEasingFunction>(BuiltIn);
			}
			BuiltIn->Type = Edit.Type.GetValue();
		}
	}

	void ApplySectionEdit(const FSequenceContext& Ctx, UMovieSceneSection* Section, const FSectionEdit& Edit)
	{
		Section->Modify();

		if (Edit.MoveBy && Edit.MoveBy.GetValue() != 0)
		{
			Section->MoveSection(Ctx.ToTicks(Edit.MoveBy.GetValue()) - Ctx.ToTicks(0));
		}
		if (Edit.Start || Edit.End)
		{
			TRange<FFrameNumber> Range = Section->GetRange();
			if (Edit.Start)
			{
				Range.SetLowerBound(TRangeBound<FFrameNumber>::Inclusive(Ctx.ToTicks(Edit.Start.GetValue())));
			}
			if (Edit.End)
			{
				Range.SetUpperBound(TRangeBound<FFrameNumber>::Exclusive(Ctx.ToTicks(Edit.End.GetValue())));
			}
			Section->SetRange(Range);
		}
		if (Edit.Row)
		{
			Section->SetRowIndex(Edit.Row.GetValue());
		}
		if (Edit.Active)
		{
			Section->SetIsActive(Edit.Active.GetValue());
		}
		ApplyEase(Section, Edit.EaseIn, true);
		ApplyEase(Section, Edit.EaseOut, false);

		if (UMovieSceneSkeletalAnimationSection* Anim = Cast<UMovieSceneSkeletalAnimationSection>(Section))
		{
			FMovieSceneSkeletalAnimationParams& P = Anim->Params;
			if (Edit.Animation)
			{
				P.Animation = Edit.Animation;
			}
			if (Edit.FirstLoopStartOffset)
			{
				P.FirstLoopStartFrameOffset = Ctx.DecimalToTicks(Edit.FirstLoopStartOffset.GetValue()) - Ctx.ToTicks(0);
			}
			if (Edit.StartOffset)
			{
				P.StartFrameOffset = Ctx.DecimalToTicks(Edit.StartOffset.GetValue()) - Ctx.ToTicks(0);
			}
			if (Edit.EndOffset)
			{
				P.EndFrameOffset = Ctx.DecimalToTicks(Edit.EndOffset.GetValue()) - Ctx.ToTicks(0);
			}
			if (Edit.PlayRate)
			{
				P.PlayRate = FMovieSceneTimeWarpVariant(Edit.PlayRate.GetValue());
			}
			if (Edit.Slot)
			{
				P.SlotName = Edit.Slot.GetValue();
			}
		}
		if (UMovieSceneCameraCutSection* Cut = Cast<UMovieSceneCameraCutSection>(Section); Cut && Edit.CameraBinding)
		{
			Cut->SetCameraBindingID(UE::MovieScene::FRelativeObjectBindingID(Edit.CameraBinding.GetValue()));
		}
		if (UMovieSceneSubSection* Sub = Cast<UMovieSceneSubSection>(Section); Sub && Edit.SubSequence)
		{
			Sub->SetSequence(Edit.SubSequence);
		}
	}

	/** Index of Section in its track. */
	int32 SectionIndexOf(const UMovieSceneTrack* Track, const UMovieSceneSection* Section)
	{
		return Track->GetAllSections().IndexOfByKey(Section);
	}

	// ------------------------------------------------------------------------
	// Keys
	// ------------------------------------------------------------------------

	struct FKeyEdit
	{
		int32 Frame = 0;
		double SubFrame = 0.0;
		FFrameNumber Ticks;
		TSharedPtr<FJsonValue> Value;
		TOptional<ERichCurveInterpMode> Interp;
		TOptional<ERichCurveTangentMode> TangentMode;
		TOptional<float> Arrive;
		TOptional<float> Leave;
	};

	bool ParseTangentMode(const FString& Name, ERichCurveTangentMode& Out)
	{
		if (Name.Equals(TEXT("auto"), ESearchCase::IgnoreCase))       { Out = RCTM_Auto; return true; }
		if (Name.Equals(TEXT("smart_auto"), ESearchCase::IgnoreCase)) { Out = RCTM_SmartAuto; return true; }
		if (Name.Equals(TEXT("user"), ESearchCase::IgnoreCase))       { Out = RCTM_User; return true; }
		if (Name.Equals(TEXT("break"), ESearchCase::IgnoreCase))      { Out = RCTM_Break; return true; }
		return false;
	}

	bool ParseKeyEdits(const FSequenceContext& Ctx, const TSharedPtr<FJsonObject>& Params, bool bCurve, TArray<FKeyEdit>& Out, FString& OutError)
	{
		const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
		if (!Params->TryGetArrayField(TEXT("keys"), Keys) || !Keys || Keys->Num() == 0)
		{
			OutError = TEXT("'keys' must be a non-empty array of {frame, value, ...}");
			return false;
		}
		for (int32 i = 0; i < Keys->Num(); ++i)
		{
			const TSharedPtr<FJsonObject>* Object = nullptr;
			if (!(*Keys)[i]->TryGetObject(Object) || !Object)
			{
				OutError = FString::Printf(TEXT("keys[%d] must be an object"), i);
				return false;
			}
			const TSharedPtr<FJsonObject>& K = *Object;
			FKeyEdit Edit;
			if (!ParseFrameEntry((*Keys)[i], Edit.Frame, Edit.SubFrame, OutError))
			{
				OutError = FString::Printf(TEXT("keys[%d]: %s"), i, *OutError);
				return false;
			}
			Edit.Ticks = Ctx.ToTicks(Edit.Frame, Edit.SubFrame);
			Edit.Value = K->TryGetField(TEXT("value"));
			if (!Edit.Value.IsValid() || Edit.Value->IsNull())
			{
				OutError = FString::Printf(TEXT("keys[%d]: missing 'value'"), i);
				return false;
			}

			FString Interp;
			if (K->TryGetStringField(TEXT("interp"), Interp) && !Interp.IsEmpty())
			{
				ERichCurveTangentMode Shorthand;
				if (Interp.Equals(TEXT("constant"), ESearchCase::IgnoreCase))    { Edit.Interp = RCIM_Constant; }
				else if (Interp.Equals(TEXT("linear"), ESearchCase::IgnoreCase)) { Edit.Interp = RCIM_Linear; }
				else if (Interp.Equals(TEXT("cubic"), ESearchCase::IgnoreCase))  { Edit.Interp = RCIM_Cubic; }
				else if (ParseTangentMode(Interp, Shorthand))                     { Edit.Interp = RCIM_Cubic; Edit.TangentMode = Shorthand; }
				else
				{
					OutError = FString::Printf(TEXT("keys[%d]: unknown interp '%s' (constant, linear, cubic, auto, smart_auto, user, break)"), i, *Interp);
					return false;
				}
			}
			FString TangentMode;
			if (K->TryGetStringField(TEXT("tangent_mode"), TangentMode) && !TangentMode.IsEmpty())
			{
				ERichCurveTangentMode Mode;
				if (!ParseTangentMode(TangentMode, Mode))
				{
					OutError = FString::Printf(TEXT("keys[%d]: unknown tangent_mode '%s' (auto, smart_auto, user, break)"), i, *TangentMode);
					return false;
				}
				Edit.TangentMode = Mode;
			}
			double Tangent = 0.0;
			if (K->TryGetNumberField(TEXT("arrive"), Tangent)) { Edit.Arrive = static_cast<float>(Tangent); }
			if (K->TryGetNumberField(TEXT("leave"), Tangent))  { Edit.Leave = static_cast<float>(Tangent); }
			if ((Edit.Arrive || Edit.Leave) && !Edit.TangentMode)
			{
				Edit.TangentMode = RCTM_User;
			}
			if (!bCurve && (Edit.Interp || Edit.TangentMode))
			{
				OutError = FString::Printf(TEXT("keys[%d]: interp / tangents apply to float and double channels only"), i);
				return false;
			}
			Out.Add(Edit);
		}
		return true;
	}

	bool JsonToNumber(const TSharedPtr<FJsonValue>& Value, double& Out)
	{
		return Value.IsValid() && Value->Type == EJson::Number && Value->TryGetNumber(Out);
	}

	bool JsonToBool(const TSharedPtr<FJsonValue>& Value, bool& Out)
	{
		if (!Value.IsValid())
		{
			return false;
		}
		if (Value->Type == EJson::Boolean)
		{
			Out = Value->AsBool();
			return true;
		}
		double Number = 0.0;
		if (Value->Type == EJson::Number && Value->TryGetNumber(Number) && (Number == 0.0 || Number == 1.0))
		{
			Out = Number != 0.0;
			return true;
		}
		return false;
	}

	bool JsonToInt(const TSharedPtr<FJsonValue>& Value, int32& Out)
	{
		double Number = 0.0;
		if (JsonToNumber(Value, Number) && FMath::Frac(Number) == 0.0)
		{
			Out = static_cast<int32>(Number);
			return true;
		}
		return false;
	}

	bool JsonToByte(const TSharedPtr<FJsonValue>& Value, const UEnum* Enum, uint8& Out, FString& OutError)
	{
		int32 Number = 0;
		if (JsonToInt(Value, Number) && Number >= 0 && Number <= 255)
		{
			Out = static_cast<uint8>(Number);
			return true;
		}
		FString Name;
		if (Enum && Value.IsValid() && Value->TryGetString(Name))
		{
			int64 EnumValue = Enum->GetValueByNameString(Name);
			if (EnumValue == INDEX_NONE)
			{
				EnumValue = Enum->GetValueByNameString(Enum->GenerateFullEnumName(*Name));
			}
			if (EnumValue != INDEX_NONE && EnumValue >= 0 && EnumValue <= 255)
			{
				Out = static_cast<uint8>(EnumValue);
				return true;
			}
			OutError = FString::Printf(TEXT("'%s' is not an enumerator of %s"), *Name, *Enum->GetName());
			return false;
		}
		OutError = Enum ? FString::Printf(TEXT("expected an enumerator name of %s or an integer 0-255"), *Enum->GetName()) : FString(TEXT("expected an integer 0-255"));
		return false;
	}

	/** Check every key value against the channel type before anything changes. */
	bool CheckKeyValues(const FChannelRef& Ref, const TArray<FKeyEdit>& Edits, FString& OutError)
	{
		const FString Type = GetChannelTypeLabel(Ref.TypeName);
		const UEnum* Enum = Type == TEXT("byte") ? static_cast<FMovieSceneByteChannel*>(Ref.Channel)->GetEnum() : nullptr;
		for (int32 i = 0; i < Edits.Num(); ++i)
		{
			double Number;
			bool Bool;
			int32 Int;
			uint8 Byte;
			FString Error;
			const bool bOk = (Type == TEXT("float") || Type == TEXT("double")) ? JsonToNumber(Edits[i].Value, Number)
				: Type == TEXT("bool") ? JsonToBool(Edits[i].Value, Bool)
				: Type == TEXT("integer") ? JsonToInt(Edits[i].Value, Int)
				: JsonToByte(Edits[i].Value, Enum, Byte, Error);
			if (!bOk)
			{
				OutError = FString::Printf(TEXT("keys[%d] (frame %d): value doesn't fit a %s channel%s%s"), i, Edits[i].Frame, *Type,
					Error.IsEmpty() ? TEXT("") : TEXT(": "), *Error);
				return false;
			}
		}
		return true;
	}

	template<typename ChannelType, typename KeyValueType>
	void ApplyCurveKeys(ChannelType* Channel, const TArray<FKeyEdit>& Edits, const FSequenceContext& Ctx, TArray<TSharedPtr<FJsonValue>>& OutReport)
	{
		for (const FKeyEdit& Edit : Edits)
		{
			double Number = 0.0;
			JsonToNumber(Edit.Value, Number);

			auto Data = Channel->GetData();
			int32 Index = Data.FindKey(Edit.Ticks);
			const bool bAdded = Index == INDEX_NONE;
			if (bAdded)
			{
				KeyValueType NewValue(static_cast<decltype(KeyValueType::Value)>(Number));
				NewValue.InterpMode = RCIM_Cubic;
				NewValue.TangentMode = RCTM_Auto;
				Index = Data.AddKey(Edit.Ticks, NewValue);
			}
			KeyValueType& Key = Data.GetValues()[Index];
			Key.Value = static_cast<decltype(KeyValueType::Value)>(Number);
			if (Edit.Interp)
			{
				Key.InterpMode = Edit.Interp.GetValue();
			}
			if (Edit.TangentMode)
			{
				Key.TangentMode = Edit.TangentMode.GetValue();
			}
			if (Edit.Arrive)
			{
				Key.Tangent.ArriveTangent = Edit.Arrive.GetValue();
			}
			if (Edit.Leave)
			{
				Key.Tangent.LeaveTangent = Edit.Leave.GetValue();
			}

			TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetNumberField(TEXT("frame"), Edit.Frame);
			if (Edit.SubFrame > 0.0)
			{
				Row->SetNumberField(TEXT("sub_frame"), Edit.SubFrame);
			}
			Row->SetStringField(TEXT("action"), bAdded ? TEXT("added") : TEXT("updated"));
			OutReport.Add(MakeShared<FJsonValueObject>(Row));
		}
		Channel->AutoSetTangents();
	}

	template<typename ChannelType, typename ValueType>
	void ApplySimpleKey(ChannelType* Channel, const FKeyEdit& Edit, ValueType Value, TArray<TSharedPtr<FJsonValue>>& OutReport)
	{
		auto Data = Channel->GetData();
		const int32 Index = Data.FindKey(Edit.Ticks);
		if (Index == INDEX_NONE)
		{
			Data.AddKey(Edit.Ticks, Value);
		}
		else
		{
			Data.GetValues()[Index] = Value;
		}
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetNumberField(TEXT("frame"), Edit.Frame);
		if (Edit.SubFrame > 0.0)
		{
			Row->SetNumberField(TEXT("sub_frame"), Edit.SubFrame);
		}
		Row->SetStringField(TEXT("action"), Index == INDEX_NONE ? TEXT("added") : TEXT("updated"));
		OutReport.Add(MakeShared<FJsonValueObject>(Row));
	}

	/** Resolve asset + section + channel shared by the key actions. */
	bool ResolveChannelTarget(const TSharedPtr<FJsonObject>& Params, FSequenceContext& Ctx, FTarget& Target, FChannelRef& Channel, FString& OutError)
	{
		if (!LoadForWrite(Params, Ctx, OutError) || !ResolveSection(Ctx, Params, Target, OutError))
		{
			return false;
		}
		FString ChannelName;
		if (!Params->TryGetStringField(TEXT("channel"), ChannelName))
		{
			OutError = TEXT("Missing required param 'channel' (a channel name such as Location.X, or #index)");
			return false;
		}
		return ResolveChannel(Target.Section, ChannelName, Channel, OutError);
	}
}

// ============================================================================
// Read
// ============================================================================

FMonolithActionResult FMonolithLevelSequenceContentActions::GetSequenceContents(const TSharedPtr<FJsonObject>& Params)
{
	FSequenceContext Ctx;
	FString Error;
	if (!LoadSequence(Params, Ctx, Error))
	{
		return Fail(Error);
	}

	FDumpOptions Options;
	Params->TryGetBoolField(TEXT("include_keys"), Options.bIncludeKeys);
	if (Params->HasField(TEXT("max_keys")))
	{
		Options.MaxKeys = FMath::Max(0, static_cast<int32>(Params->GetNumberField(TEXT("max_keys"))));
	}
	FString TrackClassName;
	if (Params->TryGetStringField(TEXT("track_class"), TrackClassName) && !TrackClassName.IsEmpty())
	{
		Options.TrackClass = ResolveTrackClass(TrackClassName, Error);
		if (!Options.TrackClass)
		{
			return Fail(Error);
		}
	}
	FGuid OnlyBinding;
	FString BindingRef;
	if (Params->TryGetStringField(TEXT("binding"), BindingRef) && !BindingRef.IsEmpty())
	{
		if (!ResolveBindingRef(Ctx, BindingRef, OnlyBinding, Error))
		{
			return Fail(Error);
		}
	}

	UMovieScene* MS = Ctx.MovieScene;
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), Ctx.AssetPath);
	Ctx.WriteRates(Result);

	const TRange<FFrameNumber> Playback = MS->GetPlaybackRange();
	TSharedPtr<FJsonObject> PlaybackJson = MakeShared<FJsonObject>();
	PlaybackJson->SetNumberField(TEXT("start"), Ctx.ToDisplayFrame(Playback.GetLowerBoundValue()));
	PlaybackJson->SetNumberField(TEXT("end"), Ctx.ToDisplayFrame(Playback.GetUpperBoundValue()));
	Result->SetObjectField(TEXT("playback"), PlaybackJson);
#if WITH_EDITORONLY_DATA
	auto RangeJson = [&Ctx](double StartSeconds, double EndSeconds)
	{
		TSharedPtr<FJsonObject> Range = MakeShared<FJsonObject>();
		Range->SetNumberField(TEXT("start"), FMath::RoundToDouble(Ctx.SecondsToFrames(StartSeconds) * 1000.0) / 1000.0);
		Range->SetNumberField(TEXT("end"), FMath::RoundToDouble(Ctx.SecondsToFrames(EndSeconds) * 1000.0) / 1000.0);
		return Range;
	};
	const FMovieSceneEditorData& EditorData = MS->GetEditorData();
	Result->SetObjectField(TEXT("view_range"), RangeJson(EditorData.ViewStart, EditorData.ViewEnd));
	Result->SetObjectField(TEXT("work_range"), RangeJson(EditorData.WorkStart, EditorData.WorkEnd));
#endif
	if (MS->IsReadOnly())
	{
		Result->SetBoolField(TEXT("read_only"), true);
	}
	Result->SetArrayField(TEXT("marked_frames"), MarkedFramesToJson(Ctx));

	// Parent -> child possessables (components and attached actors under a spawnable / possessable).
	TMultiMap<FGuid, FGuid> Children;
	TSet<FGuid> HasParent;
	TSet<FGuid> AllBindings;
	for (const FMovieSceneBinding& Binding : static_cast<const UMovieScene*>(MS)->GetBindings())
	{
		AllBindings.Add(Binding.GetObjectGuid());
	}
	for (int32 i = 0; i < MS->GetPossessableCount(); ++i)
	{
		const FMovieScenePossessable& Possessable = MS->GetPossessable(i);
		if (Possessable.GetParent().IsValid() && AllBindings.Contains(Possessable.GetParent()))
		{
			Children.Add(Possessable.GetParent(), Possessable.GetGuid());
			HasParent.Add(Possessable.GetGuid());
		}
	}

	TArray<TSharedPtr<FJsonValue>> Bindings;
	if (OnlyBinding.IsValid())
	{
		if (TSharedPtr<FJsonObject> Binding = BindingToJson(Ctx, OnlyBinding, Children, Options))
		{
			Bindings.Add(MakeShared<FJsonValueObject>(Binding));
		}
	}
	else
	{
		Result->SetArrayField(TEXT("tracks"), TracksToJson(Ctx, FGuid(), Options));
		for (const FMovieSceneBinding& Binding : static_cast<const UMovieScene*>(MS)->GetBindings())
		{
			const FGuid Guid = Binding.GetObjectGuid();
			if (HasParent.Contains(Guid))
			{
				continue;
			}
			if (TSharedPtr<FJsonObject> Json = BindingToJson(Ctx, Guid, Children, Options))
			{
				Bindings.Add(MakeShared<FJsonValueObject>(Json));
			}
		}
	}
	Result->SetArrayField(TEXT("bindings"), Bindings);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithLevelSequenceContentActions::RefreshCurrentSequence(const TSharedPtr<FJsonObject>& Params)
{
#if WITH_LEVELSEQUENCEEDITOR
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	ULevelSequence* Current = ULevelSequenceEditorBlueprintLibrary::GetCurrentLevelSequence();
	if (Current)
	{
		ULevelSequenceEditorBlueprintLibrary::RefreshCurrentLevelSequence();
		Result->SetStringField(TEXT("asset_path"), Current->GetPathName());
		Result->SetBoolField(TEXT("refreshed"), true);
	}
	else
	{
		Result->SetField(TEXT("asset_path"), MakeShared<FJsonValueNull>());
		Result->SetBoolField(TEXT("refreshed"), false);
	}
	return FMonolithActionResult::Success(Result);
#else
	return Fail(TEXT("refresh_current_sequence needs the LevelSequenceEditor plugin, which this project disables"));
#endif
}

// ============================================================================
// Tracks and sections
// ============================================================================

FMonolithActionResult FMonolithLevelSequenceContentActions::AddTrack(const TSharedPtr<FJsonObject>& Params)
{
	FSequenceContext Ctx;
	FString Error;
	if (!LoadForWrite(Params, Ctx, Error))
	{
		return Fail(Error);
	}

	FGuid BindingGuid;
	FString BindingRef;
	if (Params->TryGetStringField(TEXT("binding"), BindingRef) && !BindingRef.IsEmpty() && !ResolveBindingRef(Ctx, BindingRef, BindingGuid, Error))
	{
		return Fail(Error);
	}
	FString ClassName;
	Params->TryGetStringField(TEXT("track_class"), ClassName);
	UClass* TrackClass = ResolveTrackClass(ClassName, Error);
	if (!TrackClass)
	{
		return Fail(Error);
	}

	const bool bCameraCut = TrackClass->IsChildOf(UMovieSceneCameraCutTrack::StaticClass());
	if (bCameraCut && BindingGuid.IsValid())
	{
		return Fail(TEXT("A camera cut track belongs to the sequence: omit 'binding'"));
	}
	if (bCameraCut && Ctx.MovieScene->GetCameraCutTrack())
	{
		return Fail(TEXT("The sequence already has a camera cut track (one per sequence); add sections to it instead"));
	}

	UMovieSceneTrack* NewTrack = nullptr;
	{
		FScopedTransaction Transaction(TransactionText(TEXT("add_track")));
		Ctx.MovieScene->Modify();
		if (bCameraCut)
		{
			NewTrack = Ctx.MovieScene->AddCameraCutTrack(TrackClass);
		}
		else if (BindingGuid.IsValid())
		{
			NewTrack = NewObject<UMovieSceneTrack>(Ctx.MovieScene, TrackClass, NAME_None, RF_Transactional);
			Ctx.MovieScene->AddGivenTrack(NewTrack, BindingGuid);
		}
		else
		{
			NewTrack = Ctx.MovieScene->AddTrack(TrackClass);
		}
		if (!NewTrack)
		{
			Transaction.Cancel();
			return Fail(FString::Printf(TEXT("The movie scene refused a %s track (the class may not be allowed in this sequence type)"), *TrackClass->GetName()));
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("track_class"), NewTrack->GetClass()->GetName());
	Result->SetNumberField(TEXT("track_index"), GetTrackIndexInClass(GetScopeTracks(Ctx, BindingGuid), NewTrack));
	if (BindingGuid.IsValid())
	{
		Result->SetStringField(TEXT("binding"), BindingGuid.ToString(EGuidFormats::DigitsWithHyphens));
	}
	return FinishWrite(Ctx, Params, Result);
}

FMonolithActionResult FMonolithLevelSequenceContentActions::RemoveTrack(const TSharedPtr<FJsonObject>& Params)
{
	FSequenceContext Ctx;
	FTarget Target;
	FString Error;
	if (!LoadForWrite(Params, Ctx, Error) || !ResolveTrack(Ctx, Params, Target, Error))
	{
		return Fail(Error);
	}

	const FString ClassName = Target.Track->GetClass()->GetName();
	const int32 SectionCount = Target.Track->GetAllSections().Num();
	{
		FScopedTransaction Transaction(TransactionText(TEXT("remove_track")));
		Ctx.MovieScene->Modify();
		if (Target.Track == Ctx.MovieScene->GetCameraCutTrack())
		{
			Ctx.MovieScene->RemoveCameraCutTrack();
		}
		else if (!Ctx.MovieScene->RemoveTrack(*Target.Track))
		{
			Transaction.Cancel();
			return Fail(TEXT("The movie scene could not remove the track"));
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("removed_track_class"), ClassName);
	Result->SetNumberField(TEXT("removed_track_index"), Target.TrackIndex);
	Result->SetNumberField(TEXT("removed_section_count"), SectionCount);
	return FinishWrite(Ctx, Params, Result);
}

FMonolithActionResult FMonolithLevelSequenceContentActions::AddSection(const TSharedPtr<FJsonObject>& Params)
{
	FSequenceContext Ctx;
	FTarget Target;
	FString Error;
	if (!LoadForWrite(Params, Ctx, Error) || !ResolveTrack(Ctx, Params, Target, Error))
	{
		return Fail(Error);
	}
	if (Params->HasField(TEXT("move_by")))
	{
		return Fail(TEXT("move_by applies to set_section; add_section takes start / end"));
	}

	FSectionEdit Edit;
	if (!ParseSectionEdit(Ctx, Params, Edit, Error))
	{
		return Fail(Error);
	}
	if (!Edit.Start)
	{
		return Fail(TEXT("Missing required param 'start' (display frame)"));
	}

	// Default end: the animation's or subsequence's length.
	if (!Edit.End)
	{
		double LengthFrames = 0.0;
		if (Edit.Animation)
		{
			const double Rate = Edit.PlayRate.Get(1.0);
			const double Trim = Edit.StartOffset.Get(0.0) + Edit.EndOffset.Get(0.0);
			LengthFrames = (Edit.Animation->GetPlayLength() * Ctx.DisplayRate.AsDecimal() - Trim) / Rate;
		}
		else if (Edit.SubSequence && Edit.SubSequence->GetMovieScene())
		{
			const UMovieScene* SubScene = Edit.SubSequence->GetMovieScene();
			const TRange<FFrameNumber> SubRange = SubScene->GetPlaybackRange();
			const double Seconds = SubScene->GetTickResolution().AsSeconds(FFrameTime(SubRange.GetUpperBoundValue() - SubRange.GetLowerBoundValue()));
			LengthFrames = Seconds * Ctx.DisplayRate.AsDecimal();
		}
		if (LengthFrames <= 0.0)
		{
			return Fail(TEXT("Missing 'end': it is only inferred from an animation or a subsequence"));
		}
		Edit.End = Edit.Start.GetValue() + FMath::Max(1, FMath::RoundToInt(LengthFrames));
	}

	// Create the section first so the type checks run on the real class; it is only added to the track below.
	UMovieSceneSection* Section = Target.Track->CreateNewSection();
	if (!Section)
	{
		return Fail(FString::Printf(TEXT("%s can't create sections"), *Target.Track->GetClass()->GetName()));
	}
	Section->SetRange(TRange<FFrameNumber>(Ctx.ToTicks(Edit.Start.GetValue()), Ctx.ToTicks(Edit.End.GetValue())));
	if (!CheckSectionEdit(Ctx, Section, Edit, Error))
	{
		Section->MarkAsGarbage();
		return Fail(Error);
	}

	{
		FScopedTransaction Transaction(TransactionText(TEXT("add_section")));
		Target.Track->Modify();
		ApplySectionEdit(Ctx, Section, Edit);
		Target.Track->AddSection(*Section);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetObjectField(TEXT("section"), SectionToJson(Ctx, Section, SectionIndexOf(Target.Track, Section), false, false, 0));
	Result->SetStringField(TEXT("track_class"), Target.Track->GetClass()->GetName());
	Result->SetNumberField(TEXT("track_index"), Target.TrackIndex);
	return FinishWrite(Ctx, Params, Result);
}

FMonolithActionResult FMonolithLevelSequenceContentActions::SetSection(const TSharedPtr<FJsonObject>& Params)
{
	FSequenceContext Ctx;
	FTarget Target;
	FString Error;
	if (!LoadForWrite(Params, Ctx, Error) || !ResolveSection(Ctx, Params, Target, Error))
	{
		return Fail(Error);
	}
	FSectionEdit Edit;
	if (!ParseSectionEdit(Ctx, Params, Edit, Error) || !CheckSectionEdit(Ctx, Target.Section, Edit, Error))
	{
		return Fail(Error);
	}

	{
		FScopedTransaction Transaction(TransactionText(TEXT("set_section")));
		Target.Track->Modify();
		ApplySectionEdit(Ctx, Target.Section, Edit);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetObjectField(TEXT("section"), SectionToJson(Ctx, Target.Section, SectionIndexOf(Target.Track, Target.Section), false, false, 0));
	return FinishWrite(Ctx, Params, Result);
}

FMonolithActionResult FMonolithLevelSequenceContentActions::RemoveSection(const TSharedPtr<FJsonObject>& Params)
{
	FSequenceContext Ctx;
	FTarget Target;
	FString Error;
	if (!LoadForWrite(Params, Ctx, Error) || !ResolveSection(Ctx, Params, Target, Error))
	{
		return Fail(Error);
	}

	const FString ClassName = Target.Section->GetClass()->GetName();
	{
		FScopedTransaction Transaction(TransactionText(TEXT("remove_section")));
		Target.Track->Modify();
		Target.Track->RemoveSection(*Target.Section);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("removed_section_class"), ClassName);
	Result->SetNumberField(TEXT("removed_section_index"), Target.SectionIndex);
	Result->SetNumberField(TEXT("remaining_section_count"), Target.Track->GetAllSections().Num());
	return FinishWrite(Ctx, Params, Result);
}

// ============================================================================
// Keys
// ============================================================================

FMonolithActionResult FMonolithLevelSequenceContentActions::SetKeys(const TSharedPtr<FJsonObject>& Params)
{
	FSequenceContext Ctx;
	FTarget Target;
	FChannelRef Channel;
	FString Error;
	if (!ResolveChannelTarget(Params, Ctx, Target, Channel, Error))
	{
		return Fail(Error);
	}
	if (!IsWritableChannelType(Channel.TypeName))
	{
		return Fail(FString::Printf(TEXT("Channel '%s' is a %s channel; set_keys writes float, double, bool, integer and byte channels"), *Channel.Name, *GetChannelTypeLabel(Channel.TypeName)));
	}

	const FString Type = GetChannelTypeLabel(Channel.TypeName);
	const bool bCurve = Type == TEXT("float") || Type == TEXT("double");
	TArray<FKeyEdit> Edits;
	if (!ParseKeyEdits(Ctx, Params, bCurve, Edits, Error) || !CheckKeyValues(Channel, Edits, Error))
	{
		return Fail(Error);
	}

	TArray<TSharedPtr<FJsonValue>> Report;
	{
		FScopedTransaction Transaction(TransactionText(TEXT("set_keys")));
		Target.Section->Modify();
		if (Type == TEXT("float"))
		{
			ApplyCurveKeys<FMovieSceneFloatChannel, FMovieSceneFloatValue>(static_cast<FMovieSceneFloatChannel*>(Channel.Channel), Edits, Ctx, Report);
		}
		else if (Type == TEXT("double"))
		{
			ApplyCurveKeys<FMovieSceneDoubleChannel, FMovieSceneDoubleValue>(static_cast<FMovieSceneDoubleChannel*>(Channel.Channel), Edits, Ctx, Report);
		}
		else
		{
			for (const FKeyEdit& Edit : Edits)
			{
				if (Type == TEXT("bool"))
				{
					bool Value = false;
					JsonToBool(Edit.Value, Value);
					ApplySimpleKey(static_cast<FMovieSceneBoolChannel*>(Channel.Channel), Edit, Value, Report);
				}
				else if (Type == TEXT("integer"))
				{
					int32 Value = 0;
					JsonToInt(Edit.Value, Value);
					ApplySimpleKey(static_cast<FMovieSceneIntegerChannel*>(Channel.Channel), Edit, Value, Report);
				}
				else
				{
					FMovieSceneByteChannel* ByteChannel = static_cast<FMovieSceneByteChannel*>(Channel.Channel);
					uint8 Value = 0;
					FString Unused;
					JsonToByte(Edit.Value, ByteChannel->GetEnum(), Value, Unused);
					ApplySimpleKey(ByteChannel, Edit, Value, Report);
				}
			}
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("keys"), Report);
	Result->SetObjectField(TEXT("channel"), ChannelToJson(Ctx, Channel, true, 0));
	return FinishWrite(Ctx, Params, Result);
}

FMonolithActionResult FMonolithLevelSequenceContentActions::RemoveKeys(const TSharedPtr<FJsonObject>& Params)
{
	FSequenceContext Ctx;
	FTarget Target;
	FChannelRef Channel;
	FString Error;
	if (!ResolveChannelTarget(Params, Ctx, Target, Channel, Error))
	{
		return Fail(Error);
	}

	const TArray<TSharedPtr<FJsonValue>>* Frames = nullptr;
	if (!Params->TryGetArrayField(TEXT("frames"), Frames) || !Frames || Frames->Num() == 0)
	{
		return Fail(TEXT("'frames' must be a non-empty array of frames"));
	}

	struct FFrameToRemove { int32 Frame; double SubFrame; FFrameNumber Ticks; };
	TArray<FFrameToRemove> ToRemove;
	for (int32 i = 0; i < Frames->Num(); ++i)
	{
		FFrameToRemove Entry;
		if (!ParseFrameEntry((*Frames)[i], Entry.Frame, Entry.SubFrame, Error))
		{
			return Fail(FString::Printf(TEXT("frames[%d]: %s"), i, *Error));
		}
		Entry.Ticks = Ctx.ToTicks(Entry.Frame, Entry.SubFrame);
		ToRemove.Add(Entry);
	}

	TArray<TSharedPtr<FJsonValue>> Removed;
	TArray<TSharedPtr<FJsonValue>> NotFound;
	TArray<FKeyHandle> Handles;
	for (const FFrameToRemove& Entry : ToRemove)
	{
		TArray<FKeyHandle> Found;
		Channel.Channel->GetKeys(TRange<FFrameNumber>::Inclusive(Entry.Ticks, Entry.Ticks), nullptr, &Found);
		(Found.Num() ? Removed : NotFound).Add(MakeShared<FJsonValueNumber>(Entry.Frame + Entry.SubFrame));
		Handles.Append(Found);
	}

	if (Handles.Num() > 0)
	{
		FScopedTransaction Transaction(TransactionText(TEXT("remove_keys")));
		Target.Section->Modify();
		Channel.Channel->DeleteKeys(Handles);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("removed"), Removed);
	Result->SetArrayField(TEXT("not_found"), NotFound);
	Result->SetObjectField(TEXT("channel"), ChannelToJson(Ctx, Channel, true, 0));
	if (Handles.Num() == 0)
	{
		Result->SetStringField(TEXT("asset_path"), Ctx.AssetPath);
		Result->SetBoolField(TEXT("saved"), false);
		return FMonolithActionResult::Success(Result);
	}
	return FinishWrite(Ctx, Params, Result);
}

FMonolithActionResult FMonolithLevelSequenceContentActions::SetChannelDefault(const TSharedPtr<FJsonObject>& Params)
{
	FSequenceContext Ctx;
	FTarget Target;
	FChannelRef Channel;
	FString Error;
	if (!ResolveChannelTarget(Params, Ctx, Target, Channel, Error))
	{
		return Fail(Error);
	}
	if (!IsWritableChannelType(Channel.TypeName))
	{
		return Fail(FString::Printf(TEXT("Channel '%s' is a %s channel; set_channel_default writes float, double, bool, integer and byte channels"), *Channel.Name, *GetChannelTypeLabel(Channel.TypeName)));
	}
	if (!Params->HasField(TEXT("value")))
	{
		return Fail(TEXT("Missing required param 'value' (null removes the default)"));
	}

	const TSharedPtr<FJsonValue> Value = Params->TryGetField(TEXT("value"));
	const bool bRemove = !Value.IsValid() || Value->IsNull();
	const FString Type = GetChannelTypeLabel(Channel.TypeName);

	double Number = 0.0;
	bool Bool = false;
	int32 Int = 0;
	uint8 Byte = 0;
	if (!bRemove)
	{
		FString ByteError;
		const bool bOk = (Type == TEXT("float") || Type == TEXT("double")) ? JsonToNumber(Value, Number)
			: Type == TEXT("bool") ? JsonToBool(Value, Bool)
			: Type == TEXT("integer") ? JsonToInt(Value, Int)
			: JsonToByte(Value, static_cast<FMovieSceneByteChannel*>(Channel.Channel)->GetEnum(), Byte, ByteError);
		if (!bOk)
		{
			return Fail(FString::Printf(TEXT("value doesn't fit a %s channel%s%s"), *Type, ByteError.IsEmpty() ? TEXT("") : TEXT(": "), *ByteError));
		}
	}

	{
		FScopedTransaction Transaction(TransactionText(TEXT("set_channel_default")));
		Target.Section->Modify();
		auto Apply = [bRemove](auto* Typed, auto NewValue)
		{
			if (bRemove)
			{
				Typed->RemoveDefault();
			}
			else
			{
				Typed->SetDefault(NewValue);
			}
		};
		if (Type == TEXT("float"))        { Apply(static_cast<FMovieSceneFloatChannel*>(Channel.Channel), static_cast<float>(Number)); }
		else if (Type == TEXT("double"))  { Apply(static_cast<FMovieSceneDoubleChannel*>(Channel.Channel), Number); }
		else if (Type == TEXT("bool"))    { Apply(static_cast<FMovieSceneBoolChannel*>(Channel.Channel), Bool); }
		else if (Type == TEXT("integer")) { Apply(static_cast<FMovieSceneIntegerChannel*>(Channel.Channel), Int); }
		else                              { Apply(static_cast<FMovieSceneByteChannel*>(Channel.Channel), Byte); }
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetObjectField(TEXT("channel"), ChannelToJson(Ctx, Channel, false, 0));
	return FinishWrite(Ctx, Params, Result);
}

// ============================================================================
// Sequence
// ============================================================================

FMonolithActionResult FMonolithLevelSequenceContentActions::SetMarkedFrames(const TSharedPtr<FJsonObject>& Params)
{
	FSequenceContext Ctx;
	FString Error;
	if (!LoadForWrite(Params, Ctx, Error))
	{
		return Fail(Error);
	}
	UMovieScene* MS = Ctx.MovieScene;

	FString Op;
	Params->TryGetStringField(TEXT("op"), Op);
	Op = Op.ToLower();
	if (Op != TEXT("add") && Op != TEXT("set") && Op != TEXT("remove") && Op != TEXT("clear"))
	{
		return Fail(TEXT("'op' must be add, set, remove or clear"));
	}

	int32 Frame = 0;
	const bool bHasFrame = TryGetFrame(Params, TEXT("frame"), Frame, Error);
	if (!Error.IsEmpty())
	{
		return Fail(Error);
	}
	double SubFrame = 0.0;
	Params->TryGetNumberField(TEXT("sub_frame"), SubFrame);
	if (SubFrame < 0.0 || SubFrame >= 1.0)
	{
		return Fail(TEXT("'sub_frame' must be in [0, 1)"));
	}
	FString Label, NewLabel, Comment;
	const bool bHasLabel = Params->TryGetStringField(TEXT("label"), Label) && !Label.IsEmpty();
	const bool bHasNewLabel = Params->TryGetStringField(TEXT("new_label"), NewLabel) && !NewLabel.IsEmpty();
	const bool bHasComment = Params->TryGetStringField(TEXT("comment"), Comment);

	// set / remove target
	int32 Index = INDEX_NONE;
	if (Op == TEXT("set") || Op == TEXT("remove"))
	{
		if (Params->HasField(TEXT("index")))
		{
			Index = static_cast<int32>(Params->GetNumberField(TEXT("index")));
		}
		else if (bHasLabel)
		{
			Index = MS->FindMarkedFrameByLabel(Label);
		}
		if (!MS->GetMarkedFrames().IsValidIndex(Index))
		{
			return Fail(FString::Printf(TEXT("No marked frame %s (there are %d). Pass a valid 'index' or an existing 'label'"),
				bHasLabel && !Params->HasField(TEXT("index")) ? *FString::Printf(TEXT("labelled '%s'"), *Label) : *FString::Printf(TEXT("at index %d"), Index),
				MS->GetMarkedFrames().Num()));
		}
	}
	if (Op == TEXT("add") && !bHasFrame)
	{
		return Fail(TEXT("op add needs 'frame'"));
	}

	int32 ResultIndex = INDEX_NONE;
	{
		FScopedTransaction Transaction(TransactionText(TEXT("set_marked_frames")));
		MS->Modify();
		if (Op == TEXT("add"))
		{
			FMovieSceneMarkedFrame Mark(Ctx.ToTicks(Frame, SubFrame));
			Mark.Label = bHasLabel ? Label : FString();
			Mark.Comment = Comment;
			ResultIndex = MS->AddMarkedFrame(Mark);
		}
		else if (Op == TEXT("set"))
		{
			FMovieSceneMarkedFrame Mark = MS->GetMarkedFrames()[Index];
			if (bHasFrame)
			{
				Mark.FrameNumber = Ctx.ToTicks(Frame, SubFrame);
			}
			if (bHasNewLabel)
			{
				Mark.Label = NewLabel;
			}
			if (bHasComment)
			{
				Mark.Comment = Comment;
			}
			// Re-add rather than edit in place: UMovieScene only exposes the list const, and
			// AddMarkedFrame keeps it sorted by frame.
			MS->DeleteMarkedFrame(Index);
			ResultIndex = MS->AddMarkedFrame(Mark);
		}
		else if (Op == TEXT("remove"))
		{
			MS->DeleteMarkedFrame(Index);
		}
		else
		{
			MS->DeleteMarkedFrames();
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("op"), Op);
	if (ResultIndex != INDEX_NONE)
	{
		Result->SetNumberField(TEXT("index"), ResultIndex);
	}
	Result->SetArrayField(TEXT("marked_frames"), MarkedFramesToJson(Ctx));
	return FinishWrite(Ctx, Params, Result);
}

FMonolithActionResult FMonolithLevelSequenceContentActions::SetPlaybackRange(const TSharedPtr<FJsonObject>& Params)
{
	FSequenceContext Ctx;
	FString Error;
	if (!LoadForWrite(Params, Ctx, Error))
	{
		return Fail(Error);
	}
	UMovieScene* MS = Ctx.MovieScene;

	TOptional<int32> Start, End;
	TOptional<double> ViewStart, ViewEnd, WorkStart, WorkEnd;
	if (!ParseOptionalInt(Params, TEXT("start"), Start, Error)
		|| !ParseOptionalInt(Params, TEXT("end"), End, Error)
		|| !ParseOptionalNumber(Params, TEXT("view_start"), ViewStart, Error)
		|| !ParseOptionalNumber(Params, TEXT("view_end"), ViewEnd, Error)
		|| !ParseOptionalNumber(Params, TEXT("work_start"), WorkStart, Error)
		|| !ParseOptionalNumber(Params, TEXT("work_end"), WorkEnd, Error))
	{
		return Fail(Error);
	}
	if (!Start && !End && !ViewStart && !ViewEnd && !WorkStart && !WorkEnd)
	{
		return Fail(TEXT("Nothing to change: pass start, end, view_start, view_end, work_start or work_end"));
	}

	const TRange<FFrameNumber> Playback = MS->GetPlaybackRange();
	const FFrameNumber NewStart = Start ? Ctx.ToTicks(Start.GetValue()) : Playback.GetLowerBoundValue();
	const FFrameNumber NewEnd = End ? Ctx.ToTicks(End.GetValue()) : Playback.GetUpperBoundValue();
	if (NewEnd <= NewStart)
	{
		return Fail(TEXT("The playback range would end at or before its start"));
	}
#if WITH_EDITORONLY_DATA
	const FMovieSceneEditorData& EditorData = MS->GetEditorData();
	const double NewViewStart = ViewStart ? Ctx.FramesToSeconds(ViewStart.GetValue()) : EditorData.ViewStart;
	const double NewViewEnd = ViewEnd ? Ctx.FramesToSeconds(ViewEnd.GetValue()) : EditorData.ViewEnd;
	const double NewWorkStart = WorkStart ? Ctx.FramesToSeconds(WorkStart.GetValue()) : EditorData.WorkStart;
	const double NewWorkEnd = WorkEnd ? Ctx.FramesToSeconds(WorkEnd.GetValue()) : EditorData.WorkEnd;
	if (NewViewEnd <= NewViewStart || NewWorkEnd <= NewWorkStart)
	{
		return Fail(TEXT("The view or working range would end at or before its start"));
	}
#endif

	{
		FScopedTransaction Transaction(TransactionText(TEXT("set_playback_range")));
		MS->Modify();
		if (Start || End)
		{
			MS->SetPlaybackRange(TRange<FFrameNumber>(NewStart, NewEnd));
		}
#if WITH_EDITORONLY_DATA
		if (ViewStart || ViewEnd)
		{
			MS->SetViewRange(NewViewStart, NewViewEnd);
		}
		if (WorkStart || WorkEnd)
		{
			MS->SetWorkingRange(NewWorkStart, NewWorkEnd);
		}
#endif
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	const TRange<FFrameNumber> After = MS->GetPlaybackRange();
	Result->SetNumberField(TEXT("start"), Ctx.ToDisplayFrame(After.GetLowerBoundValue()));
	Result->SetNumberField(TEXT("end"), Ctx.ToDisplayFrame(After.GetUpperBoundValue()));
#if WITH_EDITORONLY_DATA
	Result->SetNumberField(TEXT("view_start"), Ctx.SecondsToFrames(MS->GetEditorData().ViewStart));
	Result->SetNumberField(TEXT("view_end"), Ctx.SecondsToFrames(MS->GetEditorData().ViewEnd));
	Result->SetNumberField(TEXT("work_start"), Ctx.SecondsToFrames(MS->GetEditorData().WorkStart));
	Result->SetNumberField(TEXT("work_end"), Ctx.SecondsToFrames(MS->GetEditorData().WorkEnd));
#endif
	return FinishWrite(Ctx, Params, Result);
}

#undef LOCTEXT_NAMESPACE
