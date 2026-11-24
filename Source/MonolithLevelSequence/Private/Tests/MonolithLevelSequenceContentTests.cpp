// Automation coverage for the live-asset level_sequence actions (get_sequence_contents, retime, and
// the track / section / key / sequence writers). Each test builds a disposable Level Sequence in
// memory under /Game/Tests/Monolith/LevelSequence (never saved: every write passes save=false) and
// trashes it afterwards. Actions are invoked through the tool registry, the same path an MCP call takes.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "GameFramework/Actor.h"
#include "LevelSequence.h"
#include "Misc/Guid.h"
#include "MonolithToolRegistry.h"
#include "MovieScene.h"
#include "MovieSceneTrack.h"
#include "Sections/MovieSceneAudioSection.h"
#include "Sound/SoundWave.h"
#include "UObject/Package.h"

namespace MonolithLevelSequenceContentTestDetail
{
	/** A disposable 30 fps / 24000 ticks sequence, playback 0..300, with two possessable bindings. */
	struct FSequenceSandbox
	{
		UPackage* Package = nullptr;
		ULevelSequence* Sequence = nullptr;
		FString AssetPath;
		FGuid Hero;
		FGuid Camera;

		FSequenceSandbox()
		{
			const FString Name = FString::Printf(TEXT("LS_MonolithTest_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8));
			const FString PackageName = FString(TEXT("/Game/Tests/Monolith/LevelSequence/")) + Name;
			Package = CreatePackage(*PackageName);
			Sequence = NewObject<ULevelSequence>(Package, FName(*Name), RF_Public | RF_Standalone | RF_Transactional);
			Sequence->Initialize();
			UMovieScene* MS = Sequence->GetMovieScene();
			MS->SetDisplayRate(FFrameRate(30, 1));
			MS->SetTickResolutionDirectly(FFrameRate(24000, 1));
			MS->SetPlaybackRange(TRange<FFrameNumber>(FFrameNumber(0), FFrameNumber(300 * 800)));
			Hero = MS->AddPossessable(TEXT("Hero"), AActor::StaticClass());
			Camera = MS->AddPossessable(TEXT("Camera"), AActor::StaticClass());
			AssetPath = Sequence->GetPathName();
		}

		~FSequenceSandbox()
		{
			if (Sequence)
			{
				Sequence->ClearFlags(RF_Standalone | RF_Public);
				Sequence->MarkAsGarbage();
			}
			if (Package)
			{
				Package->SetDirtyFlag(false);
				Package->MarkAsGarbage();
			}
		}

		/** Params for a write: never saves the disposable asset. */
		TSharedPtr<FJsonObject> Params() const
		{
			TSharedPtr<FJsonObject> P = ReadParams();
			P->SetBoolField(TEXT("save"), false);
			return P;
		}

		/** Params for get_sequence_contents, which takes no `save`. */
		TSharedPtr<FJsonObject> ReadParams() const
		{
			TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
			P->SetStringField(TEXT("asset_path"), AssetPath);
			return P;
		}
	};

	FMonolithActionResult Run(const TCHAR* Action, const TSharedPtr<FJsonObject>& Params)
	{
		return FMonolithToolRegistry::Get().ExecuteAction(TEXT("level_sequence"), Action, Params);
	}

	TSharedPtr<FJsonObject> Key(int32 Frame, double Value)
	{
		TSharedPtr<FJsonObject> K = MakeShared<FJsonObject>();
		K->SetNumberField(TEXT("frame"), Frame);
		K->SetNumberField(TEXT("value"), Value);
		return K;
	}

	TArray<TSharedPtr<FJsonValue>> Keys(std::initializer_list<TSharedPtr<FJsonObject>> List)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		for (const TSharedPtr<FJsonObject>& K : List)
		{
			Out.Add(MakeShared<FJsonValueObject>(K));
		}
		return Out;
	}

	/** Frames of the keys of channel `Channel` in the first section of the first track of `TrackClass` on the binding. */
	TArray<int32> ReadKeyFrames(const FSequenceSandbox& Box, const FString& Binding, const FString& TrackClass, const FString& Channel)
	{
		TArray<int32> Frames;
		TSharedPtr<FJsonObject> P = Box.ReadParams();
		P->SetStringField(TEXT("binding"), Binding);
		P->SetStringField(TEXT("track_class"), TrackClass);
		P->SetNumberField(TEXT("max_keys"), 0);
		const FMonolithActionResult R = Run(TEXT("get_sequence_contents"), P);
		if (!R.bSuccess)
		{
			return Frames;
		}
		for (const TSharedPtr<FJsonValue>& B : R.Result->GetArrayField(TEXT("bindings")))
		{
			for (const TSharedPtr<FJsonValue>& T : B->AsObject()->GetArrayField(TEXT("tracks")))
			{
				for (const TSharedPtr<FJsonValue>& S : T->AsObject()->GetArrayField(TEXT("sections")))
				{
					for (const TSharedPtr<FJsonValue>& C : S->AsObject()->GetArrayField(TEXT("channels")))
					{
						if (C->AsObject()->GetStringField(TEXT("name")) == Channel)
						{
							for (const TSharedPtr<FJsonValue>& K : C->AsObject()->GetArrayField(TEXT("keys")))
							{
								Frames.Add(static_cast<int32>(K->AsObject()->GetNumberField(TEXT("frame"))));
							}
							return Frames;
						}
					}
				}
			}
		}
		return Frames;
	}

	TSharedPtr<FJsonObject> FirstSection(const FMonolithActionResult& Contents, const FString& TrackClass)
	{
		auto FromTracks = [&TrackClass](const TArray<TSharedPtr<FJsonValue>>& Tracks) -> TSharedPtr<FJsonObject>
		{
			for (const TSharedPtr<FJsonValue>& T : Tracks)
			{
				if (T->AsObject()->GetStringField(TEXT("class")) == TrackClass)
				{
					const TArray<TSharedPtr<FJsonValue>>& Sections = T->AsObject()->GetArrayField(TEXT("sections"));
					return Sections.Num() ? Sections[0]->AsObject() : nullptr;
				}
			}
			return nullptr;
		};
		const TArray<TSharedPtr<FJsonValue>>* SequenceTracks = nullptr;
		if (Contents.Result->TryGetArrayField(TEXT("tracks"), SequenceTracks))
		{
			if (TSharedPtr<FJsonObject> S = FromTracks(*SequenceTracks))
			{
				return S;
			}
		}
		for (const TSharedPtr<FJsonValue>& B : Contents.Result->GetArrayField(TEXT("bindings")))
		{
			if (TSharedPtr<FJsonObject> S = FromTracks(B->AsObject()->GetArrayField(TEXT("tracks"))))
			{
				return S;
			}
		}
		return nullptr;
	}

	/** Hero transform track + section 0..300 with Location.X keys 0, 60, 120; camera cut 0..150 on Camera; marker "Hit" at 90. */
	bool BuildScene(FAutomationTestBase& Test, const FSequenceSandbox& Box)
	{
		TSharedPtr<FJsonObject> P = Box.Params();
		P->SetStringField(TEXT("binding"), TEXT("Hero"));
		P->SetStringField(TEXT("track_class"), TEXT("MovieScene3DTransformTrack"));
		if (!Test.TestTrue(TEXT("add_track transform"), Run(TEXT("add_track"), P).bSuccess)) return false;

		P->SetNumberField(TEXT("start"), 0);
		P->SetNumberField(TEXT("end"), 300);
		const FMonolithActionResult Section = Run(TEXT("add_section"), P);
		if (!Test.TestTrue(TEXT("add_section transform"), Section.bSuccess)) { Test.AddError(Section.ErrorMessage); return false; }

		TSharedPtr<FJsonObject> K = Box.Params();
		K->SetStringField(TEXT("binding"), TEXT("Hero"));
		K->SetStringField(TEXT("track_class"), TEXT("MovieScene3DTransformTrack"));
		K->SetStringField(TEXT("channel"), TEXT("Location.X"));
		K->SetArrayField(TEXT("keys"), Keys({ Key(0, 0.0), Key(60, 100.0), Key(120, 200.0) }));
		const FMonolithActionResult SetKeys = Run(TEXT("set_keys"), K);
		if (!Test.TestTrue(TEXT("set_keys Location.X"), SetKeys.bSuccess)) { Test.AddError(SetKeys.ErrorMessage); return false; }

		TSharedPtr<FJsonObject> C = Box.Params();
		C->SetStringField(TEXT("track_class"), TEXT("MovieSceneCameraCutTrack"));
		if (!Test.TestTrue(TEXT("add_track camera cut"), Run(TEXT("add_track"), C).bSuccess)) return false;
		C->SetNumberField(TEXT("start"), 0);
		C->SetNumberField(TEXT("end"), 150);
		C->SetStringField(TEXT("camera_binding"), TEXT("Camera"));
		const FMonolithActionResult Cut = Run(TEXT("add_section"), C);
		if (!Test.TestTrue(TEXT("add_section camera cut"), Cut.bSuccess)) { Test.AddError(Cut.ErrorMessage); return false; }

		TSharedPtr<FJsonObject> M = Box.Params();
		M->SetStringField(TEXT("op"), TEXT("add"));
		M->SetNumberField(TEXT("frame"), 90);
		M->SetStringField(TEXT("label"), TEXT("Hit"));
		return Test.TestTrue(TEXT("add marked frame"), Run(TEXT("set_marked_frames"), M).bSuccess);
	}
}

using namespace MonolithLevelSequenceContentTestDetail;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithLevelSequenceContentsTest,
	"Monolith.LevelSequence.Content.ReadAndEdit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithLevelSequenceContentsTest::RunTest(const FString& Parameters)
{
	FSequenceSandbox Box;
	if (!BuildScene(*this, Box))
	{
		return false;
	}

	const FMonolithActionResult Contents = Run(TEXT("get_sequence_contents"), Box.ReadParams());
	if (!TestTrue(TEXT("get_sequence_contents succeeds"), Contents.bSuccess))
	{
		AddError(Contents.ErrorMessage);
		return false;
	}
	TestEqual(TEXT("display rate"), Contents.Result->GetStringField(TEXT("display_rate")), FString(TEXT("30/1")));
	TestEqual(TEXT("ticks per frame"), static_cast<int32>(Contents.Result->GetNumberField(TEXT("ticks_per_frame"))), 800);
	TestEqual(TEXT("playback end"), static_cast<int32>(Contents.Result->GetObjectField(TEXT("playback"))->GetNumberField(TEXT("end"))), 300);
	TestEqual(TEXT("two bindings"), Contents.Result->GetArrayField(TEXT("bindings")).Num(), 2);
	TestEqual(TEXT("one marked frame"), Contents.Result->GetArrayField(TEXT("marked_frames")).Num(), 1);

	const TSharedPtr<FJsonObject> Cut = FirstSection(Contents, TEXT("MovieSceneCameraCutTrack"));
	if (TestNotNull(TEXT("camera cut section listed"), Cut.Get()))
	{
		TestEqual(TEXT("camera cut camera name"), Cut->GetStringField(TEXT("camera_binding_name")), FString(TEXT("Camera")));
		TestEqual(TEXT("camera cut end"), static_cast<int32>(Cut->GetNumberField(TEXT("end"))), 150);
	}
	TestEqual(TEXT("Location.X keys"), ReadKeyFrames(Box, TEXT("Hero"), TEXT("MovieScene3DTransformTrack"), TEXT("Location.X")), TArray<int32>({ 0, 60, 120 }));

	// Tangents: flatten the key at 60, then read it back.
	{
		TSharedPtr<FJsonObject> P = Box.Params();
		P->SetStringField(TEXT("binding"), Box.Hero.ToString());
		P->SetStringField(TEXT("track_class"), TEXT("MovieScene3DTransformTrack"));
		P->SetStringField(TEXT("channel"), TEXT("Location.X"));
		TSharedPtr<FJsonObject> K = Key(60, 150.0);
		K->SetStringField(TEXT("interp"), TEXT("cubic"));
		K->SetStringField(TEXT("tangent_mode"), TEXT("user"));
		K->SetNumberField(TEXT("arrive"), 0.0);
		K->SetNumberField(TEXT("leave"), 0.0);
		P->SetArrayField(TEXT("keys"), Keys({ K }));
		const FMonolithActionResult R = Run(TEXT("set_keys"), P);
		if (TestTrue(TEXT("set_keys by GUID with user tangents"), R.bSuccess))
		{
			const TArray<TSharedPtr<FJsonValue>>& Report = R.Result->GetArrayField(TEXT("keys"));
			TestEqual(TEXT("key at 60 updated, not added"), Report[0]->AsObject()->GetStringField(TEXT("action")), FString(TEXT("updated")));
			const TSharedPtr<FJsonObject> Key60 = R.Result->GetObjectField(TEXT("channel"))->GetArrayField(TEXT("keys"))[1]->AsObject();
			TestEqual(TEXT("value updated"), Key60->GetNumberField(TEXT("value")), 150.0);
			TestEqual(TEXT("tangent mode user"), Key60->GetStringField(TEXT("tangent_mode")), FString(TEXT("user")));
			TestEqual(TEXT("leave tangent flattened"), Key60->GetNumberField(TEXT("leave")), 0.0);
		}
	}

	// Section easing and wrong-type fields.
	{
		TSharedPtr<FJsonObject> P = Box.Params();
		P->SetStringField(TEXT("binding"), TEXT("Hero"));
		P->SetStringField(TEXT("track_class"), TEXT("MovieScene3DTransformTrack"));
		P->SetNumberField(TEXT("ease_in"), 12);
		P->SetStringField(TEXT("ease_in_type"), TEXT("Linear"));
		const FMonolithActionResult R = Run(TEXT("set_section"), P);
		if (TestTrue(TEXT("set_section ease_in"), R.bSuccess))
		{
			TestEqual(TEXT("ease_in 12 frames"), R.Result->GetObjectField(TEXT("section"))->GetNumberField(TEXT("ease_in")), 12.0);
			TestEqual(TEXT("ease_in type"), R.Result->GetObjectField(TEXT("section"))->GetStringField(TEXT("ease_in_type")), FString(TEXT("Linear")));
		}

		TSharedPtr<FJsonObject> Bad = Box.Params();
		Bad->SetStringField(TEXT("binding"), TEXT("Hero"));
		Bad->SetStringField(TEXT("track_class"), TEXT("MovieScene3DTransformTrack"));
		Bad->SetNumberField(TEXT("play_rate"), 2.0);
		TestFalse(TEXT("animation fields refused on a transform section"), Run(TEXT("set_section"), Bad).bSuccess);
	}

	// move_by moves the keys with the section.
	{
		TSharedPtr<FJsonObject> P = Box.Params();
		P->SetStringField(TEXT("binding"), TEXT("Hero"));
		P->SetStringField(TEXT("track_class"), TEXT("MovieScene3DTransformTrack"));
		P->SetNumberField(TEXT("move_by"), 5);
		TestTrue(TEXT("set_section move_by"), Run(TEXT("set_section"), P).bSuccess);
		TestEqual(TEXT("keys moved with the section"), ReadKeyFrames(Box, TEXT("Hero"), TEXT("MovieScene3DTransformTrack"), TEXT("Location.X")), TArray<int32>({ 5, 65, 125 }));

		P->RemoveField(TEXT("move_by"));
		P->SetNumberField(TEXT("start"), 0);
		TestTrue(TEXT("set_section start"), Run(TEXT("set_section"), P).bSuccess);
		TestEqual(TEXT("start changes the range only"), ReadKeyFrames(Box, TEXT("Hero"), TEXT("MovieScene3DTransformTrack"), TEXT("Location.X")), TArray<int32>({ 5, 65, 125 }));
	}

	// remove_keys and set_channel_default.
	{
		TSharedPtr<FJsonObject> P = Box.Params();
		P->SetStringField(TEXT("binding"), TEXT("Hero"));
		P->SetStringField(TEXT("track_class"), TEXT("MovieScene3DTransformTrack"));
		P->SetStringField(TEXT("channel"), TEXT("Location.X"));
		TArray<TSharedPtr<FJsonValue>> Frames;
		Frames.Add(MakeShared<FJsonValueNumber>(65));
		Frames.Add(MakeShared<FJsonValueNumber>(66));
		P->SetArrayField(TEXT("frames"), Frames);
		const FMonolithActionResult R = Run(TEXT("remove_keys"), P);
		if (TestTrue(TEXT("remove_keys"), R.bSuccess))
		{
			TestEqual(TEXT("one removed"), R.Result->GetArrayField(TEXT("removed")).Num(), 1);
			TestEqual(TEXT("one not found"), R.Result->GetArrayField(TEXT("not_found")).Num(), 1);
		}

		TSharedPtr<FJsonObject> D = Box.Params();
		D->SetStringField(TEXT("binding"), TEXT("Hero"));
		D->SetStringField(TEXT("track_class"), TEXT("MovieScene3DTransformTrack"));
		D->SetStringField(TEXT("channel"), TEXT("Location.Z"));
		D->SetNumberField(TEXT("value"), 42.0);
		const FMonolithActionResult Def = Run(TEXT("set_channel_default"), D);
		if (TestTrue(TEXT("set_channel_default"), Def.bSuccess))
		{
			TestEqual(TEXT("default read back"), Def.Result->GetObjectField(TEXT("channel"))->GetNumberField(TEXT("default")), 42.0);
		}
	}

	// Errors: unknown binding, bad track index, bad channel.
	{
		TSharedPtr<FJsonObject> P = Box.Params();
		P->SetStringField(TEXT("binding"), TEXT("Nobody"));
		P->SetStringField(TEXT("track_class"), TEXT("MovieScene3DTransformTrack"));
		TestFalse(TEXT("unknown binding refused"), Run(TEXT("remove_track"), P).bSuccess);
		P->SetStringField(TEXT("binding"), TEXT("Hero"));
		P->SetNumberField(TEXT("track_index"), 3);
		TestFalse(TEXT("bad track_index refused"), Run(TEXT("remove_track"), P).bSuccess);
		P->SetNumberField(TEXT("track_index"), 0);
		P->SetStringField(TEXT("channel"), TEXT("Nope"));
		P->SetNumberField(TEXT("value"), 1.0);
		TestFalse(TEXT("unknown channel refused"), Run(TEXT("set_channel_default"), P).bSuccess);
	}

	// remove_section, then remove_track.
	{
		TSharedPtr<FJsonObject> P = Box.Params();
		P->SetStringField(TEXT("track_class"), TEXT("MovieSceneCameraCutTrack"));
		const FMonolithActionResult R = Run(TEXT("remove_section"), P);
		if (TestTrue(TEXT("remove_section"), R.bSuccess))
		{
			TestEqual(TEXT("no section left"), static_cast<int32>(R.Result->GetNumberField(TEXT("remaining_section_count"))), 0);
		}
		TestTrue(TEXT("remove_track camera cut"), Run(TEXT("remove_track"), P).bSuccess);
		TestNull(TEXT("camera cut track gone"), Box.Sequence->GetMovieScene()->GetCameraCutTrack());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithLevelSequenceRetimeTest,
	"Monolith.LevelSequence.Content.Retime",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithLevelSequenceRetimeTest::RunTest(const FString& Parameters)
{
	FSequenceSandbox Box;
	if (!BuildScene(*this, Box))
	{
		return false;
	}
	const FString Transform = TEXT("MovieScene3DTransformTrack");

	// Dry run: a report, no change.
	TSharedPtr<FJsonObject> P = Box.Params();
	P->SetNumberField(TEXT("at"), 60);
	P->SetNumberField(TEXT("frames"), 10);
	const FMonolithActionResult Dry = Run(TEXT("retime"), P);
	if (!TestTrue(TEXT("retime dry run"), Dry.bSuccess))
	{
		AddError(Dry.ErrorMessage);
		return false;
	}
	TestTrue(TEXT("dry_run defaults to true"), Dry.Result->GetBoolField(TEXT("dry_run")));
	TestEqual(TEXT("two keys would move"), static_cast<int32>(Dry.Result->GetNumberField(TEXT("keys_moved"))), 2);
	TestEqual(TEXT("dry run leaves the keys"), ReadKeyFrames(Box, TEXT("Hero"), Transform, TEXT("Location.X")), TArray<int32>({ 0, 60, 120 }));

	// Insert 10 at 60.
	P->SetBoolField(TEXT("dry_run"), false);
	const FMonolithActionResult Insert = Run(TEXT("retime"), P);
	if (!TestTrue(TEXT("retime insert"), Insert.bSuccess))
	{
		AddError(Insert.ErrorMessage);
		return false;
	}
	TestEqual(TEXT("keys at/after 60 moved"), ReadKeyFrames(Box, TEXT("Hero"), Transform, TEXT("Location.X")), TArray<int32>({ 0, 70, 130 }));
	TestEqual(TEXT("marked frame moved"), static_cast<int32>(Insert.Result->GetNumberField(TEXT("marked_frames_moved"))), 1);
	{
		const FMonolithActionResult Contents = Run(TEXT("get_sequence_contents"), Box.ReadParams());
		TestEqual(TEXT("playback end moved"), static_cast<int32>(Contents.Result->GetObjectField(TEXT("playback"))->GetNumberField(TEXT("end"))), 310);
		TestEqual(TEXT("marker at 100"), static_cast<int32>(Contents.Result->GetArrayField(TEXT("marked_frames"))[0]->AsObject()->GetNumberField(TEXT("frame"))), 100);
		const TSharedPtr<FJsonObject> Cut = FirstSection(Contents, TEXT("MovieSceneCameraCutTrack"));
		if (TestNotNull(TEXT("camera cut"), Cut.Get()))
		{
			TestEqual(TEXT("spanning section keeps its start"), static_cast<int32>(Cut->GetNumberField(TEXT("start"))), 0);
			TestEqual(TEXT("spanning section end moved"), static_cast<int32>(Cut->GetNumberField(TEXT("end"))), 160);
		}
		const TSharedPtr<FJsonObject> Section = FirstSection(Contents, Transform);
		if (TestNotNull(TEXT("transform section"), Section.Get()))
		{
			TestEqual(TEXT("transform section end moved"), static_cast<int32>(Section->GetNumberField(TEXT("end"))), 310);
		}
	}

	// A key at 60 next to the key at 70, then remove the 10 frames before 70: the key at 60 is deleted
	// (the key at 70 lands on it) and the rest moves back.
	{
		TSharedPtr<FJsonObject> K = Box.Params();
		K->SetStringField(TEXT("binding"), TEXT("Hero"));
		K->SetStringField(TEXT("track_class"), Transform);
		K->SetStringField(TEXT("channel"), TEXT("Location.X"));
		K->SetArrayField(TEXT("keys"), Keys({ Key(60, 50.0), Key(65, 75.0) }));
		TestTrue(TEXT("extra keys"), Run(TEXT("set_keys"), K).bSuccess);
	}
	TSharedPtr<FJsonObject> R = Box.Params();
	R->SetNumberField(TEXT("at"), 70);
	R->SetNumberField(TEXT("frames"), -10);
	R->SetBoolField(TEXT("dry_run"), false);
	const FMonolithActionResult Remove = Run(TEXT("retime"), R);
	if (TestTrue(TEXT("retime remove"), Remove.bSuccess))
	{
		TestEqual(TEXT("collision deleted"), Remove.Result->GetArrayField(TEXT("keys_deleted")).Num(), 1);
		TestEqual(TEXT("key inside the removed range reported"), Remove.Result->GetArrayField(TEXT("keys_left_in_removed_range")).Num(), 1);
		TestEqual(TEXT("keys after removal"), ReadKeyFrames(Box, TEXT("Hero"), Transform, TEXT("Location.X")), TArray<int32>({ 0, 60, 65, 120 }));
	}

	// Undo the removal: the keys come back.
	if (GEditor && GEditor->Trans)
	{
		GEditor->UndoTransaction();
		TestEqual(TEXT("undo restores the keys"), ReadKeyFrames(Box, TEXT("Hero"), Transform, TEXT("Location.X")), TArray<int32>({ 0, 60, 65, 70, 130 }));
	}

	TSharedPtr<FJsonObject> Bad = Box.Params();
	Bad->SetNumberField(TEXT("at"), 5);
	Bad->SetNumberField(TEXT("frames"), -10);
	TestFalse(TEXT("removing before the playback start is refused"), Run(TEXT("retime"), Bad).bSuccess);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithLevelSequenceSequenceEditsTest,
	"Monolith.LevelSequence.Content.SequenceEdits",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithLevelSequenceSequenceEditsTest::RunTest(const FString& Parameters)
{
	FSequenceSandbox Box;

	// Marked frames: add, rename, remove.
	TSharedPtr<FJsonObject> M = Box.Params();
	M->SetStringField(TEXT("op"), TEXT("add"));
	M->SetNumberField(TEXT("frame"), 40);
	M->SetStringField(TEXT("label"), TEXT("B"));
	TestTrue(TEXT("add B"), Run(TEXT("set_marked_frames"), M).bSuccess);
	M->SetNumberField(TEXT("frame"), 20);
	M->SetStringField(TEXT("label"), TEXT("A"));
	const FMonolithActionResult AddA = Run(TEXT("set_marked_frames"), M);
	if (TestTrue(TEXT("add A"), AddA.bSuccess))
	{
		TestEqual(TEXT("sorted: A first"), static_cast<int32>(AddA.Result->GetNumberField(TEXT("index"))), 0);
	}

	TSharedPtr<FJsonObject> S = Box.Params();
	S->SetStringField(TEXT("op"), TEXT("set"));
	S->SetStringField(TEXT("label"), TEXT("B"));
	S->SetStringField(TEXT("new_label"), TEXT("Impact"));
	S->SetNumberField(TEXT("frame"), 10);
	const FMonolithActionResult Set = Run(TEXT("set_marked_frames"), S);
	if (TestTrue(TEXT("set B"), Set.bSuccess))
	{
		const TSharedPtr<FJsonObject> First = Set.Result->GetArrayField(TEXT("marked_frames"))[0]->AsObject();
		TestEqual(TEXT("renamed and moved first"), First->GetStringField(TEXT("label")), FString(TEXT("Impact")));
		TestEqual(TEXT("moved to 10"), static_cast<int32>(First->GetNumberField(TEXT("frame"))), 10);
	}

	TSharedPtr<FJsonObject> Rm = Box.Params();
	Rm->SetStringField(TEXT("op"), TEXT("remove"));
	Rm->SetStringField(TEXT("label"), TEXT("A"));
	const FMonolithActionResult Removed = Run(TEXT("set_marked_frames"), Rm);
	if (TestTrue(TEXT("remove A"), Removed.bSuccess))
	{
		TestEqual(TEXT("one left"), Removed.Result->GetArrayField(TEXT("marked_frames")).Num(), 1);
	}

	// Playback range.
	TSharedPtr<FJsonObject> PR = Box.Params();
	PR->SetNumberField(TEXT("start"), 10);
	PR->SetNumberField(TEXT("end"), 200);
	const FMonolithActionResult Range = Run(TEXT("set_playback_range"), PR);
	if (TestTrue(TEXT("set_playback_range"), Range.bSuccess))
	{
		TestEqual(TEXT("start"), static_cast<int32>(Range.Result->GetNumberField(TEXT("start"))), 10);
		TestEqual(TEXT("end"), static_cast<int32>(Range.Result->GetNumberField(TEXT("end"))), 200);
	}
	PR->SetNumberField(TEXT("end"), 5);
	TestFalse(TEXT("end before start refused"), Run(TEXT("set_playback_range"), PR).bSuccess);

	// Ambiguous binding names list the GUIDs.
	Box.Sequence->GetMovieScene()->AddPossessable(TEXT("Hero"), AActor::StaticClass());
	TSharedPtr<FJsonObject> T = Box.Params();
	T->SetStringField(TEXT("binding"), TEXT("Hero"));
	T->SetStringField(TEXT("track_class"), TEXT("MovieScene3DTransformTrack"));
	const FMonolithActionResult Ambiguous = Run(TEXT("add_track"), T);
	TestFalse(TEXT("ambiguous binding refused"), Ambiguous.bSuccess);
	TestTrue(TEXT("error lists the GUID"), Ambiguous.ErrorMessage.Contains(Box.Hero.ToString(EGuidFormats::DigitsWithHyphens)));

	// refresh_current_sequence answers even with no Sequencer open.
	const FMonolithActionResult Refresh = Run(TEXT("refresh_current_sequence"), MakeShared<FJsonObject>());
#if WITH_LEVELSEQUENCEEDITOR
	TestTrue(TEXT("refresh_current_sequence succeeds"), Refresh.bSuccess);
#else
	TestFalse(TEXT("refresh_current_sequence reports the missing plugin"), Refresh.bSuccess);
#endif
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithLevelSequenceNamesAndReferencesTest,
	"Monolith.LevelSequence.Content.NamesAndReferences",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithLevelSequenceNamesAndReferencesTest::RunTest(const FString& Parameters)
{
	FSequenceSandbox Box;
	UMovieScene* MS = Box.Sequence->GetMovieScene();

	// A 22-character [A-Za-z0-9_-] name parses as a Short GUID with FGuid::Parse: it must still resolve as a name.
	const FString TreeName = TEXT("BP_Demo_Tree_Waterable");
	const FGuid Tree = MS->AddPossessable(TreeName, AActor::StaticClass());
	TSharedPtr<FJsonObject> P = Box.Params();
	P->SetStringField(TEXT("binding"), TreeName);
	P->SetStringField(TEXT("track_class"), TEXT("MovieSceneSpawnTrack"));
	const FMonolithActionResult AddTrack = Run(TEXT("add_track"), P);
	if (!TestTrue(TEXT("binding named like a Short GUID resolves by name"), AddTrack.bSuccess))
	{
		AddError(AddTrack.ErrorMessage);
		return false;
	}
	TestEqual(TEXT("track added to that binding"), AddTrack.Result->GetStringField(TEXT("binding")), Tree.ToString(EGuidFormats::DigitsWithHyphens));
	P->SetStringField(TEXT("binding"), Tree.ToString(EGuidFormats::Digits));
	P->SetNumberField(TEXT("start"), 0);
	P->SetNumberField(TEXT("end"), 100);
	TestTrue(TEXT("binding by Digits GUID"), Run(TEXT("add_section"), P).bSuccess);

	// The spawn section's only channel is unnamed: it takes the track's display name, which set_keys accepts.
	TSharedPtr<FJsonObject> R = Box.ReadParams();
	R->SetStringField(TEXT("binding"), TreeName);
	const FMonolithActionResult Contents = Run(TEXT("get_sequence_contents"), R);
	const TSharedPtr<FJsonObject> Spawn = Contents.bSuccess ? FirstSection(Contents, TEXT("MovieSceneSpawnTrack")) : nullptr;
	if (TestNotNull(TEXT("spawn section listed"), Spawn.Get()))
	{
		const TSharedPtr<FJsonObject> Channel = Spawn->GetArrayField(TEXT("channels"))[0]->AsObject();
		const FString ChannelName = Channel->GetStringField(TEXT("name"));
		TestFalse(TEXT("single channel named"), ChannelName.IsEmpty());
		TestEqual(TEXT("named after the track"), Channel->GetStringField(TEXT("name_from")), FString(TEXT("track")));

		TSharedPtr<FJsonObject> K = Box.Params();
		K->SetStringField(TEXT("binding"), TreeName);
		K->SetStringField(TEXT("track_class"), TEXT("MovieSceneSpawnTrack"));
		K->SetStringField(TEXT("channel"), ChannelName);
		TSharedPtr<FJsonObject> Spawned = MakeShared<FJsonObject>();
		Spawned->SetNumberField(TEXT("frame"), 10);
		Spawned->SetBoolField(TEXT("value"), true);
		K->SetArrayField(TEXT("keys"), Keys({ Spawned }));
		const FMonolithActionResult SetKeys = Run(TEXT("set_keys"), K);
		TestTrue(TEXT("set_keys by the track-derived channel name"), SetKeys.bSuccess);
		if (!SetKeys.bSuccess)
		{
			AddError(SetKeys.ErrorMessage);
		}
	}

	// An audio section's sound is reported through reflection.
	TSharedPtr<FJsonObject> A = Box.Params();
	A->SetStringField(TEXT("track_class"), TEXT("MovieSceneAudioTrack"));
	TestTrue(TEXT("add audio track"), Run(TEXT("add_track"), A).bSuccess);
	A->SetNumberField(TEXT("start"), 0);
	A->SetNumberField(TEXT("end"), 60);
	TestTrue(TEXT("add audio section"), Run(TEXT("add_section"), A).bSuccess);
	USoundWave* Wave = NewObject<USoundWave>(GetTransientPackage(), TEXT("MonolithLevelSequenceTestWave"), RF_Transient);
	UMovieSceneAudioSection* AudioSection = nullptr;
	for (UMovieSceneTrack* Track : MS->GetTracks())
	{
		if (Track->GetAllSections().Num() > 0)
		{
			AudioSection = Cast<UMovieSceneAudioSection>(Track->GetAllSections()[0]);
		}
	}
	if (TestNotNull(TEXT("audio section created"), AudioSection))
	{
		AudioSection->SetSound(Wave);
		const FMonolithActionResult Dump = Run(TEXT("get_sequence_contents"), Box.ReadParams());
		const TSharedPtr<FJsonObject> Audio = Dump.bSuccess ? FirstSection(Dump, TEXT("MovieSceneAudioTrack")) : nullptr;
		const TSharedPtr<FJsonObject>* References = nullptr;
		if (TestNotNull(TEXT("audio section listed"), Audio.Get()) && TestTrue(TEXT("references reported"), Audio->TryGetObjectField(TEXT("references"), References)))
		{
			TestEqual(TEXT("sound path"), (*References)->GetStringField(TEXT("Sound")), Wave->GetPathName());
		}
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
