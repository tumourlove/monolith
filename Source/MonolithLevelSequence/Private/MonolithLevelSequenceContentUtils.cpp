#include "MonolithLevelSequenceContentUtils.h"
#include "MonolithAssetUtils.h"

#include "MovieScene.h"
#include "MovieSceneBinding.h"
#include "MovieScenePossessable.h"
#include "MovieSceneSpawnable.h"
#include "MovieSceneSequence.h"
#include "MovieSceneSection.h"
#include "MovieSceneTrack.h"
#include "MovieSceneMarkedFrame.h"
#include "Channels/MovieSceneChannel.h"
#include "Channels/MovieSceneChannelProxy.h"
#include "Channels/MovieSceneChannelEditorData.h"
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
#include "UObject/UnrealType.h"
#include "UObject/SoftObjectPtr.h"
#include "UObject/SoftObjectPath.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace MonolithLevelSequence::Content
{
	// ------------------------------------------------------------------------------------------
	// Time
	// ------------------------------------------------------------------------------------------

	FFrameNumber FSequenceContext::ToTicks(int32 Frame, double SubFrame) const
	{
		const float Sub = static_cast<float>(FMath::Clamp(SubFrame, 0.0, 0.99999));
		return FFrameRate::TransformTime(FFrameTime(FFrameNumber(Frame), Sub), DisplayRate, TickResolution).RoundToFrame();
	}

	FFrameNumber FSequenceContext::DecimalToTicks(double Frames) const
	{
		return FFrameRate::TransformTime(FFrameTime::FromDecimal(Frames), DisplayRate, TickResolution).RoundToFrame();
	}

	FFrameTime FSequenceContext::ToDisplay(FFrameNumber Ticks) const
	{
		return FFrameRate::TransformTime(FFrameTime(Ticks), TickResolution, DisplayRate);
	}

	int32 FSequenceContext::ToDisplayFrame(FFrameNumber Ticks) const
	{
		return ToDisplay(Ticks).RoundToFrame().Value;
	}

	double FSequenceContext::ToDisplayDecimal(FFrameNumber Ticks) const
	{
		return ToDisplay(Ticks).AsDecimal();
	}

	double FSequenceContext::FramesToSeconds(double Frames) const
	{
		return DisplayRate.AsSeconds(FFrameTime::FromDecimal(Frames));
	}

	double FSequenceContext::SecondsToFrames(double Seconds) const
	{
		return DisplayRate.AsFrameTime(Seconds).AsDecimal();
	}

	void FSequenceContext::WriteRates(const TSharedPtr<FJsonObject>& Out) const
	{
		Out->SetStringField(TEXT("display_rate"), FString::Printf(TEXT("%d/%d"), DisplayRate.Numerator, DisplayRate.Denominator));
		Out->SetNumberField(TEXT("fps"), DisplayRate.AsDecimal());
		Out->SetStringField(TEXT("tick_resolution"), FString::Printf(TEXT("%d/%d"), TickResolution.Numerator, TickResolution.Denominator));
		const FFrameNumber OneFrame = ToTicks(1) - ToTicks(0);
		if (FFrameRate::TransformTime(FFrameTime(OneFrame), TickResolution, DisplayRate).GetSubFrame() == 0.f)
		{
			Out->SetNumberField(TEXT("ticks_per_frame"), OneFrame.Value);
		}
	}

	namespace
	{
		double RoundTo(double Value, double Step)
		{
			return FMath::RoundToDouble(Value / Step) * Step;
		}

		/** frame (+ sub_frame when not whole) of a tick time. */
		void WriteKeyTime(const FSequenceContext& Ctx, FFrameNumber Ticks, const TSharedPtr<FJsonObject>& Out)
		{
			const FFrameTime Time = Ctx.ToDisplay(Ticks);
			Out->SetNumberField(TEXT("frame"), Time.FrameNumber.Value);
			if (Time.GetSubFrame() > 0.0001f)
			{
				Out->SetNumberField(TEXT("sub_frame"), RoundTo(Time.GetSubFrame(), 0.0001));
			}
		}

		/** A frame value as a number: an integer when whole, else a decimal rounded to 1/1000. */
		void SetFramesField(const TSharedPtr<FJsonObject>& Out, const TCHAR* Field, double Frames)
		{
			Out->SetNumberField(Field, RoundTo(Frames, 0.001));
		}

		FString GuidString(const FGuid& Guid)
		{
			return Guid.ToString(EGuidFormats::DigitsWithHyphens);
		}

		const TCHAR* InterpToString(ERichCurveInterpMode Mode)
		{
			switch (Mode)
			{
			case RCIM_Constant: return TEXT("constant");
			case RCIM_Linear:   return TEXT("linear");
			case RCIM_Cubic:    return TEXT("cubic");
			default:            return TEXT("none");
			}
		}

		const TCHAR* TangentModeToString(ERichCurveTangentMode Mode)
		{
			switch (Mode)
			{
			case RCTM_Auto:      return TEXT("auto");
			case RCTM_SmartAuto: return TEXT("smart_auto");
			case RCTM_User:      return TEXT("user");
			case RCTM_Break:     return TEXT("break");
			default:             return TEXT("none");
			}
		}

		template<typename ChannelType>
		void CurveChannelToJson(const FSequenceContext& Ctx, ChannelType* Channel, const TSharedPtr<FJsonObject>& Out, bool bIncludeKeys, int32 MaxKeys)
		{
			auto Data = Channel->GetData();
			const TArrayView<const FFrameNumber> Times = Data.GetTimes();
			const auto Values = Data.GetValues();
			Out->SetNumberField(TEXT("key_count"), Times.Num());
			if (const auto Default = Channel->GetDefault(); Default.IsSet())
			{
				Out->SetNumberField(TEXT("default"), static_cast<double>(Default.GetValue()));
			}
			if (!bIncludeKeys)
			{
				return;
			}
			const int32 Count = MaxKeys > 0 ? FMath::Min(MaxKeys, Times.Num()) : Times.Num();
			TArray<TSharedPtr<FJsonValue>> Keys;
			for (int32 i = 0; i < Count; ++i)
			{
				TSharedPtr<FJsonObject> Key = MakeShared<FJsonObject>();
				WriteKeyTime(Ctx, Times[i], Key);
				Key->SetNumberField(TEXT("value"), static_cast<double>(Values[i].Value));
				const ERichCurveInterpMode Interp = Values[i].InterpMode.GetValue();
				Key->SetStringField(TEXT("interp"), InterpToString(Interp));
				if (Interp == RCIM_Cubic)
				{
					const ERichCurveTangentMode TangentMode = Values[i].TangentMode.GetValue();
					Key->SetStringField(TEXT("tangent_mode"), TangentModeToString(TangentMode));
					if (TangentMode == RCTM_User || TangentMode == RCTM_Break)
					{
						Key->SetNumberField(TEXT("arrive"), Values[i].Tangent.ArriveTangent);
						Key->SetNumberField(TEXT("leave"), Values[i].Tangent.LeaveTangent);
					}
				}
				Keys.Add(MakeShared<FJsonValueObject>(Key));
			}
			Out->SetArrayField(TEXT("keys"), Keys);
			if (Count < Times.Num())
			{
				Out->SetBoolField(TEXT("keys_truncated"), true);
			}
		}

		template<typename ChannelType, typename ToJsonFn>
		void SimpleChannelToJson(const FSequenceContext& Ctx, ChannelType* Channel, const TSharedPtr<FJsonObject>& Out, bool bIncludeKeys, int32 MaxKeys, ToJsonFn&& ToJson)
		{
			auto Data = Channel->GetData();
			const TArrayView<const FFrameNumber> Times = Data.GetTimes();
			const auto Values = Data.GetValues();
			Out->SetNumberField(TEXT("key_count"), Times.Num());
			if (const auto Default = Channel->GetDefault(); Default.IsSet())
			{
				Out->SetField(TEXT("default"), ToJson(Default.GetValue()));
			}
			if (!bIncludeKeys)
			{
				return;
			}
			const int32 Count = MaxKeys > 0 ? FMath::Min(MaxKeys, Times.Num()) : Times.Num();
			TArray<TSharedPtr<FJsonValue>> Keys;
			for (int32 i = 0; i < Count; ++i)
			{
				TSharedPtr<FJsonObject> Key = MakeShared<FJsonObject>();
				WriteKeyTime(Ctx, Times[i], Key);
				Key->SetField(TEXT("value"), ToJson(Values[i]));
				Keys.Add(MakeShared<FJsonValueObject>(Key));
			}
			Out->SetArrayField(TEXT("keys"), Keys);
			if (Count < Times.Num())
			{
				Out->SetBoolField(TEXT("keys_truncated"), true);
			}
		}

		void WriteEasing(const FSequenceContext& Ctx, const UMovieSceneSection* Section, const TSharedPtr<FJsonObject>& Out)
		{
			auto EasingType = [](const TScriptInterface<IMovieSceneEasingFunction>& Fn) -> FString
			{
				if (const UMovieSceneBuiltInEasingFunction* BuiltIn = Cast<UMovieSceneBuiltInEasingFunction>(Fn.GetObject()))
				{
					return StaticEnum<EMovieSceneBuiltInEasing>()->GetNameStringByValue(static_cast<int64>(BuiltIn->Type));
				}
				return Fn.GetObject() ? Fn.GetObject()->GetClass()->GetName() : FString();
			};

			const FMovieSceneEasingSettings& Easing = Section->Easing;
			if (Easing.GetEaseInDuration() > 0)
			{
				SetFramesField(Out, TEXT("ease_in"), Ctx.ToDisplayDecimal(FFrameNumber(Easing.GetEaseInDuration())));
				Out->SetBoolField(TEXT("ease_in_manual"), Easing.bManualEaseIn);
				Out->SetStringField(TEXT("ease_in_type"), EasingType(Easing.EaseIn));
			}
			if (Easing.GetEaseOutDuration() > 0)
			{
				SetFramesField(Out, TEXT("ease_out"), Ctx.ToDisplayDecimal(FFrameNumber(Easing.GetEaseOutDuration())));
				Out->SetBoolField(TEXT("ease_out_manual"), Easing.bManualEaseOut);
				Out->SetStringField(TEXT("ease_out_type"), EasingType(Easing.EaseOut));
			}
		}

		/** Path of a referenced object, or "<subobject Class>" for an object the section owns (easing functions, instanced data). */
		FString DescribeReference(const UObject* Object, const UObject* Owner)
		{
			if (Object->IsIn(Owner))
			{
				return FString::Printf(TEXT("<subobject %s>"), *Object->GetClass()->GetName());
			}
			return Object->GetPathName();
		}

		void CollectValue(const FProperty* Property, const void* Value, const UObject* Owner, const FString& Path, int32 Depth, const TSharedPtr<FJsonObject>& Out);

		/**
		 * Non-null object references (hard, soft, class, FSoftObjectPath) among the properties of Struct below SkipBase,
		 * descending into structs and arrays. Channel structs (keys) and transient / deprecated properties are skipped.
		 * Keys are property paths: Event, Params.Sound, Overrides[2].Material.
		 */
		void CollectReferences(const UStruct* SkipBase, const UStruct* Struct, const void* Container, const UObject* Owner,
			const FString& Prefix, int32 Depth, const TSharedPtr<FJsonObject>& Out)
		{
			if (Depth > 3 || Out->Values.Num() >= 64)
			{
				return;
			}
			for (TFieldIterator<FProperty> It(Struct); It; ++It)
			{
				const FProperty* Property = *It;
				if (Property->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
				{
					continue;
				}
				if (SkipBase && SkipBase->IsChildOf(Property->GetOwnerStruct()))
				{
					continue;   // declared on UMovieSceneSection or above: range, easing, row... are reported elsewhere
				}
				const FString Path = Prefix + Property->GetName();
				for (int32 ArrayIndex = 0; ArrayIndex < Property->ArrayDim; ++ArrayIndex)
				{
					const FString ElementPath = Property->ArrayDim > 1 ? FString::Printf(TEXT("%s[%d]"), *Path, ArrayIndex) : Path;
					const void* Value = Property->ContainerPtrToValuePtr<void>(Container, ArrayIndex);
					CollectValue(Property, Value, Owner, ElementPath, Depth, Out);
				}
			}
		}

		void CollectValue(const FProperty* Property, const void* Value, const UObject* Owner, const FString& Path, int32 Depth, const TSharedPtr<FJsonObject>& Out)
		{
			if (const FSoftObjectProperty* Soft = CastField<FSoftObjectProperty>(Property))
			{
				const FSoftObjectPtr& Ptr = *static_cast<const FSoftObjectPtr*>(Value);
				if (!Ptr.IsNull())
				{
					Out->SetStringField(Path, Ptr.ToSoftObjectPath().ToString());
				}
			}
			else if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
			{
				if (const UObject* Object = ObjectProperty->GetObjectPropertyValue(Value))
				{
					Out->SetStringField(Path, DescribeReference(Object, Owner));
				}
			}
			else if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
			{
				const UScriptStruct* Struct = StructProperty->Struct;
				if (Struct == TBaseStructure<FSoftObjectPath>::Get())
				{
					const FSoftObjectPath& SoftPath = *static_cast<const FSoftObjectPath*>(Value);
					if (!SoftPath.IsNull())
					{
						Out->SetStringField(Path, SoftPath.ToString());
					}
				}
				else if (!Struct->IsChildOf(FMovieSceneChannel::StaticStruct()))
				{
					CollectReferences(nullptr, Struct, Value, Owner, Path + TEXT("."), Depth + 1, Out);
				}
			}
			else if (const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
			{
				const FProperty* Inner = ArrayProperty->Inner;
				if (Inner->IsA<FObjectPropertyBase>() || Inner->IsA<FStructProperty>())
				{
					FScriptArrayHelper Helper(ArrayProperty, Value);
					const int32 Count = FMath::Min(Helper.Num(), 32);
					for (int32 i = 0; i < Count && Depth <= 3; ++i)
					{
						CollectValue(Inner, Helper.GetRawPtr(i), Owner, FString::Printf(TEXT("%s[%d]"), *Path, i), Depth + 1, Out);
					}
				}
			}
		}

		void WriteTypedSectionFields(const FSequenceContext& Ctx, UMovieSceneSection* Section, const TSharedPtr<FJsonObject>& Out)
		{
			if (const UMovieSceneSkeletalAnimationSection* Anim = Cast<UMovieSceneSkeletalAnimationSection>(Section))
			{
				const FMovieSceneSkeletalAnimationParams& P = Anim->Params;
				if (P.Animation)
				{
					Out->SetStringField(TEXT("animation"), P.Animation->GetPathName());
					SetFramesField(Out, TEXT("animation_length"), Ctx.DisplayRate.AsDecimal() * P.Animation->GetPlayLength());
				}
				else
				{
					Out->SetField(TEXT("animation"), MakeShared<FJsonValueNull>());
				}
				SetFramesField(Out, TEXT("first_loop_start_offset"), Ctx.ToDisplayDecimal(P.FirstLoopStartFrameOffset));
				SetFramesField(Out, TEXT("start_offset"), Ctx.ToDisplayDecimal(P.StartFrameOffset));
				SetFramesField(Out, TEXT("end_offset"), Ctx.ToDisplayDecimal(P.EndFrameOffset));
				if (P.PlayRate.GetType() == EMovieSceneTimeWarpType::FixedPlayRate)
				{
					Out->SetNumberField(TEXT("play_rate"), P.PlayRate.AsFixedPlayRate());
				}
				else
				{
					Out->SetStringField(TEXT("play_rate"), TEXT("custom"));
				}
				Out->SetStringField(TEXT("slot"), P.SlotName.ToString());
				if (P.bReverse)
				{
					Out->SetBoolField(TEXT("reverse"), true);
				}
			}
			else if (const UMovieSceneCameraCutSection* Cut = Cast<UMovieSceneCameraCutSection>(Section))
			{
				const FGuid CameraGuid = Cut->GetCameraBindingID().GetGuid();
				Out->SetStringField(TEXT("camera_binding"), CameraGuid.IsValid() ? GuidString(CameraGuid) : FString());
				if (CameraGuid.IsValid())
				{
					Out->SetStringField(TEXT("camera_binding_name"), GetBindingDisplayName(Ctx, CameraGuid));
				}
			}
			else if (const UMovieSceneSubSection* Sub = Cast<UMovieSceneSubSection>(Section))
			{
				const UMovieSceneSequence* SubSequence = Sub->GetSequence();
				Out->SetStringField(TEXT("sequence"), SubSequence ? SubSequence->GetPathName() : FString());
			}
			else
			{
				// Any other section type (audio, Wwise events, materials, plugin sections...): report what it
				// references through its own UPROPERTYs, so the dump says which sound / asset plays.
				TSharedPtr<FJsonObject> References = MakeShared<FJsonObject>();
				CollectReferences(UMovieSceneSection::StaticClass(), Section->GetClass(), Section, Section, FString(), 0, References);
				if (References->Values.Num() > 0)
				{
					Out->SetObjectField(TEXT("references"), References);
				}
			}
		}
	}

	// ------------------------------------------------------------------------------------------
	// Loading and resolving
	// ------------------------------------------------------------------------------------------

	bool LoadSequence(const TSharedPtr<FJsonObject>& Params, FSequenceContext& Out, FString& OutError)
	{
		FString AssetPath;
		if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
		{
			OutError = TEXT("Missing required param 'asset_path'");
			return false;
		}

		UMovieSceneSequence* Sequence = FMonolithAssetUtils::LoadAssetByPath<UMovieSceneSequence>(AssetPath);
		if (!Sequence)
		{
			OutError = FString::Printf(TEXT("Level Sequence not found: '%s'. Pass the asset path, e.g. /Game/Cinematics/LS_Intro (or /Game/Cinematics/LS_Intro.LS_Intro)."), *AssetPath);
			return false;
		}
		UMovieScene* MovieScene = Sequence->GetMovieScene();
		if (!MovieScene)
		{
			OutError = FString::Printf(TEXT("'%s' has no movie scene"), *AssetPath);
			return false;
		}

		Out.Sequence = Sequence;
		Out.MovieScene = MovieScene;
		Out.DisplayRate = MovieScene->GetDisplayRate();
		Out.TickResolution = MovieScene->GetTickResolution();
		Out.AssetPath = Sequence->GetPathName();
		return true;
	}

	FString GetBindingDisplayName(const FSequenceContext& Ctx, const FGuid& Guid)
	{
		return Ctx.MovieScene->GetObjectDisplayName(Guid).ToString();
	}

	bool ResolveBindingRef(const FSequenceContext& Ctx, const FString& Ref, FGuid& OutGuid, FString& OutError)
	{
		const UMovieScene* MovieScene = Ctx.MovieScene;

		// Hex GUID formats only. FGuid::Parse also accepts EGuidFormats::Short (22 base64url characters) and
		// Base36Encoded, which read ordinary actor names such as BP_Demo_Tree_Waterable as GUIDs. A parsed GUID
		// that matches no binding still falls through to the name search: a name may look like a GUID too.
		static const EGuidFormats HexFormats[] =
		{
			EGuidFormats::DigitsWithHyphens, EGuidFormats::Digits, EGuidFormats::DigitsWithHyphensInBraces,
			EGuidFormats::DigitsWithHyphensInParentheses, EGuidFormats::HexValuesInBraces, EGuidFormats::UniqueObjectGuid
		};
		for (const EGuidFormats Format : HexFormats)
		{
			FGuid Parsed;
			if (FGuid::ParseExact(Ref, Format, Parsed) && MovieScene->FindBinding(Parsed))
			{
				OutGuid = Parsed;
				return true;
			}
		}

		TArray<FGuid> Matches;
		TArray<FString> All;
		for (const FMovieSceneBinding& Binding : MovieScene->GetBindings())
		{
			const FGuid Guid = Binding.GetObjectGuid();
			const FString Display = GetBindingDisplayName(Ctx, Guid);
			FString InternalName;
			if (const FMovieScenePossessable* Possessable = Ctx.MovieScene->FindPossessable(Guid))
			{
				InternalName = Possessable->GetName();
			}
			else if (const FMovieSceneSpawnable* Spawnable = Ctx.MovieScene->FindSpawnable(Guid))
			{
				InternalName = Spawnable->GetName();
			}
			All.Add(FString::Printf(TEXT("%s (%s)"), *Display, *GuidString(Guid)));
			if (Display.Equals(Ref, ESearchCase::IgnoreCase) || InternalName.Equals(Ref, ESearchCase::IgnoreCase))
			{
				Matches.AddUnique(Guid);
			}
		}

		if (Matches.Num() == 1)
		{
			OutGuid = Matches[0];
			return true;
		}
		if (Matches.Num() > 1)
		{
			TArray<FString> Candidates;
			for (const FGuid& Guid : Matches)
			{
				Candidates.Add(FString::Printf(TEXT("%s (%s)"), *GetBindingDisplayName(Ctx, Guid), *GuidString(Guid)));
			}
			OutError = FString::Printf(TEXT("Binding name '%s' is ambiguous; pass one of these GUIDs: %s"), *Ref, *FString::Join(Candidates, TEXT(", ")));
			return false;
		}

		if (All.Num() > 40)
		{
			All.SetNum(40);
			All.Add(TEXT("..."));
		}
		OutError = FString::Printf(TEXT("No binding with GUID or name '%s' in %s. Bindings: %s"), *Ref, *Ctx.AssetPath, All.Num() ? *FString::Join(All, TEXT(", ")) : TEXT("(none)"));
		return false;
	}

	TArray<UMovieSceneTrack*> GetScopeTracks(const FSequenceContext& Ctx, const FGuid& BindingGuid)
	{
		TArray<UMovieSceneTrack*> Tracks;
		if (!BindingGuid.IsValid())
		{
			if (UMovieSceneTrack* CameraCut = Ctx.MovieScene->GetCameraCutTrack())
			{
				Tracks.Add(CameraCut);
			}
			Tracks.Append(Ctx.MovieScene->GetTracks());
		}
		else if (const FMovieSceneBinding* Binding = static_cast<const UMovieScene*>(Ctx.MovieScene)->FindBinding(BindingGuid))
		{
			Tracks.Append(Binding->GetTracks());
		}
		Tracks.RemoveAll([](const UMovieSceneTrack* Track) { return Track == nullptr; });
		return Tracks;
	}

	UClass* ResolveTrackClass(const FString& Name, FString& OutError)
	{
		UClass* Found = nullptr;
		if (Name.StartsWith(TEXT("/")))
		{
			Found = FindObject<UClass>(nullptr, *Name);
		}
		else
		{
			Found = FindFirstObject<UClass>(*Name, EFindFirstObjectOptions::None, ELogVerbosity::NoLogging);
			if (!Found && Name.Len() > 1 && Name[0] == TEXT('U'))
			{
				Found = FindFirstObject<UClass>(*Name.Mid(1), EFindFirstObjectOptions::None, ELogVerbosity::NoLogging);
			}
		}

		if (!Found || !Found->IsChildOf(UMovieSceneTrack::StaticClass()))
		{
			OutError = FString::Printf(TEXT("'%s' is not a track class. Use a class name such as MovieSceneSkeletalAnimationTrack, MovieScene3DTransformTrack, MovieSceneCameraCutTrack, MovieSceneSubTrack."), *Name);
			return nullptr;
		}
		if (Found->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated))
		{
			OutError = FString::Printf(TEXT("Track class '%s' is abstract or deprecated"), *Found->GetName());
			return nullptr;
		}
		return Found;
	}

	int32 GetTrackIndexInClass(const TArray<UMovieSceneTrack*>& ScopeTracks, const UMovieSceneTrack* Track)
	{
		int32 Index = 0;
		for (const UMovieSceneTrack* Candidate : ScopeTracks)
		{
			if (Candidate == Track)
			{
				return Index;
			}
			if (Candidate->GetClass() == Track->GetClass())
			{
				++Index;
			}
		}
		return INDEX_NONE;
	}

	bool ResolveTrack(const FSequenceContext& Ctx, const TSharedPtr<FJsonObject>& Params, FTarget& Out, FString& OutError)
	{
		FString BindingRef;
		if (Params->TryGetStringField(TEXT("binding"), BindingRef) && !BindingRef.IsEmpty())
		{
			if (!ResolveBindingRef(Ctx, BindingRef, Out.BindingGuid, OutError))
			{
				return false;
			}
		}

		FString ClassName;
		if (!Params->TryGetStringField(TEXT("track_class"), ClassName) || ClassName.IsEmpty())
		{
			OutError = TEXT("Missing required param 'track_class' (e.g. MovieSceneSkeletalAnimationTrack). get_sequence_contents lists each track's class and index.");
			return false;
		}
		UClass* TrackClass = ResolveTrackClass(ClassName, OutError);
		if (!TrackClass)
		{
			return false;
		}

		int32 TrackIndex = 0;
		if (Params->HasField(TEXT("track_index")))
		{
			TrackIndex = static_cast<int32>(Params->GetNumberField(TEXT("track_index")));
		}

		const TArray<UMovieSceneTrack*> Tracks = GetScopeTracks(Ctx, Out.BindingGuid);
		TArray<UMovieSceneTrack*> OfClass;
		for (UMovieSceneTrack* Track : Tracks)
		{
			if (Track->GetClass() == TrackClass)
			{
				OfClass.Add(Track);
			}
		}

		const FString Scope = Out.BindingGuid.IsValid()
			? FString::Printf(TEXT("binding '%s'"), *GetBindingDisplayName(Ctx, Out.BindingGuid))
			: FString(TEXT("the sequence tracks (no binding)"));
		if (!OfClass.IsValidIndex(TrackIndex))
		{
			TArray<FString> Present;
			for (const UMovieSceneTrack* Track : Tracks)
			{
				Present.AddUnique(Track->GetClass()->GetName());
			}
			OutError = FString::Printf(TEXT("No %s at track_index %d in %s (%d of that class). Track classes there: %s"),
				*TrackClass->GetName(), TrackIndex, *Scope, OfClass.Num(), Present.Num() ? *FString::Join(Present, TEXT(", ")) : TEXT("(none)"));
			return false;
		}

		Out.Track = OfClass[TrackIndex];
		Out.TrackIndex = TrackIndex;
		return true;
	}

	bool ResolveSection(const FSequenceContext& Ctx, const TSharedPtr<FJsonObject>& Params, FTarget& Out, FString& OutError)
	{
		if (!ResolveTrack(Ctx, Params, Out, OutError))
		{
			return false;
		}

		int32 SectionIndex = 0;
		if (Params->HasField(TEXT("section_index")))
		{
			SectionIndex = static_cast<int32>(Params->GetNumberField(TEXT("section_index")));
		}
		const TArray<UMovieSceneSection*>& Sections = Out.Track->GetAllSections();
		if (!Sections.IsValidIndex(SectionIndex) || !Sections[SectionIndex])
		{
			OutError = FString::Printf(TEXT("%s has %d section(s); section_index %d is out of range"),
				*Out.Track->GetClass()->GetName(), Sections.Num(), SectionIndex);
			return false;
		}
		Out.Section = Sections[SectionIndex];
		Out.SectionIndex = SectionIndex;
		return true;
	}

	TArray<FChannelRef> GetChannels(UMovieSceneSection* Section)
	{
		TArray<FChannelRef> Result;
		if (!Section)
		{
			return Result;
		}
		const FMovieSceneChannelProxy& Proxy = Section->GetChannelProxy();
		int32 Index = 0;
		for (const FMovieSceneChannelEntry& Entry : Proxy.GetAllEntries())
		{
			const TArrayView<FMovieSceneChannel* const> Channels = Entry.GetChannels();
#if WITH_EDITOR
			const TArrayView<const FMovieSceneChannelMetaData> MetaData = Entry.GetMetaData();
#endif
			for (int32 i = 0; i < Channels.Num(); ++i)
			{
				FChannelRef Ref;
				Ref.Channel = Channels[i];
				Ref.TypeName = Entry.GetChannelTypeName();
				Ref.Index = Index++;
#if WITH_EDITOR
				if (MetaData.IsValidIndex(i))
				{
					Ref.Name = MetaData[i].Name.IsNone() ? FString() : MetaData[i].Name.ToString();
					if (Ref.Name.IsEmpty() && !MetaData[i].DisplayText.IsEmpty())
					{
						Ref.Name = MetaData[i].DisplayText.ToString();
						Ref.NameSource = TEXT("display_text");
					}
				}
#endif
				Result.Add(Ref);
			}
		}

		// Single-channel sections (spawn, visibility, bool / integer property tracks...) often leave the channel
		// unnamed: call it after its track, so callers can pass the name they see in the dump.
		if (Result.Num() == 1 && Result[0].Name.IsEmpty())
		{
			if (const UMovieSceneTrack* Track = Section->GetTypedOuter<UMovieSceneTrack>())
			{
				Result[0].Name = Track->GetDisplayName().ToString();
				Result[0].NameSource = TEXT("track");
			}
		}
		return Result;
	}

	bool ResolveChannel(UMovieSceneSection* Section, const FString& Ref, FChannelRef& Out, FString& OutError)
	{
		const TArray<FChannelRef> Channels = GetChannels(Section);
		if (Ref.StartsWith(TEXT("#")))
		{
			const int32 Index = FCString::Atoi(*Ref.Mid(1));
			if (Channels.IsValidIndex(Index) && Ref.Mid(1).IsNumeric())
			{
				Out = Channels[Index];
				return true;
			}
		}
		else if (Ref.IsEmpty() && Channels.Num() == 1)
		{
			Out = Channels[0];
			return true;
		}
		else
		{
			for (const FChannelRef& Channel : Channels)
			{
				if (!Channel.Name.IsEmpty() && Channel.Name.Equals(Ref, ESearchCase::IgnoreCase))
				{
					Out = Channel;
					return true;
				}
			}
		}

		TArray<FString> Names;
		for (const FChannelRef& Channel : Channels)
		{
			Names.Add(FString::Printf(TEXT("#%d %s (%s)"), Channel.Index, Channel.Name.IsEmpty() ? TEXT("<unnamed>") : *Channel.Name, *GetChannelTypeLabel(Channel.TypeName)));
		}
		OutError = FString::Printf(TEXT("No channel '%s' on this %s. Pass a name or #index: %s"),
			*Ref, *Section->GetClass()->GetName(), Names.Num() ? *FString::Join(Names, TEXT(", ")) : TEXT("(no channels)"));
		return false;
	}

	FString GetChannelTypeLabel(FName TypeName)
	{
		if (TypeName == FMovieSceneFloatChannel::StaticStruct()->GetFName())   return TEXT("float");
		if (TypeName == FMovieSceneDoubleChannel::StaticStruct()->GetFName())  return TEXT("double");
		if (TypeName == FMovieSceneBoolChannel::StaticStruct()->GetFName())    return TEXT("bool");
		if (TypeName == FMovieSceneIntegerChannel::StaticStruct()->GetFName()) return TEXT("integer");
		if (TypeName == FMovieSceneByteChannel::StaticStruct()->GetFName())    return TEXT("byte");
		return TypeName.ToString();
	}

	bool IsWritableChannelType(FName TypeName)
	{
		const FString Label = GetChannelTypeLabel(TypeName);
		return Label == TEXT("float") || Label == TEXT("double") || Label == TEXT("bool") || Label == TEXT("integer") || Label == TEXT("byte");
	}

	// ------------------------------------------------------------------------------------------
	// JSON
	// ------------------------------------------------------------------------------------------

	TSharedPtr<FJsonObject> ChannelToJson(const FSequenceContext& Ctx, const FChannelRef& Ref, bool bIncludeKeys, int32 MaxKeys)
	{
		TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetStringField(TEXT("name"), Ref.Name);
		if (!Ref.NameSource.IsEmpty())
		{
			Out->SetStringField(TEXT("name_from"), Ref.NameSource);
		}
		Out->SetNumberField(TEXT("index"), Ref.Index);
		const FString Type = GetChannelTypeLabel(Ref.TypeName);
		Out->SetStringField(TEXT("type"), Type);

		if (Type == TEXT("float"))
		{
			CurveChannelToJson(Ctx, static_cast<FMovieSceneFloatChannel*>(Ref.Channel), Out, bIncludeKeys, MaxKeys);
		}
		else if (Type == TEXT("double"))
		{
			CurveChannelToJson(Ctx, static_cast<FMovieSceneDoubleChannel*>(Ref.Channel), Out, bIncludeKeys, MaxKeys);
		}
		else if (Type == TEXT("bool"))
		{
			SimpleChannelToJson(Ctx, static_cast<FMovieSceneBoolChannel*>(Ref.Channel), Out, bIncludeKeys, MaxKeys,
				[](bool V) -> TSharedPtr<FJsonValue> { return MakeShared<FJsonValueBoolean>(V); });
		}
		else if (Type == TEXT("integer"))
		{
			SimpleChannelToJson(Ctx, static_cast<FMovieSceneIntegerChannel*>(Ref.Channel), Out, bIncludeKeys, MaxKeys,
				[](int32 V) -> TSharedPtr<FJsonValue> { return MakeShared<FJsonValueNumber>(V); });
		}
		else if (Type == TEXT("byte"))
		{
			FMovieSceneByteChannel* Channel = static_cast<FMovieSceneByteChannel*>(Ref.Channel);
			const UEnum* Enum = Channel->GetEnum();
			if (Enum)
			{
				Out->SetStringField(TEXT("enum"), Enum->GetPathName());
			}
			SimpleChannelToJson(Ctx, Channel, Out, bIncludeKeys, MaxKeys,
				[Enum](uint8 V) -> TSharedPtr<FJsonValue>
				{
					if (Enum)
					{
						return MakeShared<FJsonValueString>(Enum->GetNameStringByValue(V));
					}
					return MakeShared<FJsonValueNumber>(V);
				});
		}
		else if (Ref.Channel)
		{
			// Other channel types (events, object paths, audio triggers...): key times only.
			TArray<FFrameNumber> Times;
			Ref.Channel->GetKeys(TRange<FFrameNumber>::All(), &Times, nullptr);
			Out->SetNumberField(TEXT("key_count"), Times.Num());
			if (bIncludeKeys)
			{
				const int32 Count = MaxKeys > 0 ? FMath::Min(MaxKeys, Times.Num()) : Times.Num();
				TArray<TSharedPtr<FJsonValue>> Keys;
				for (int32 i = 0; i < Count; ++i)
				{
					TSharedPtr<FJsonObject> Key = MakeShared<FJsonObject>();
					WriteKeyTime(Ctx, Times[i], Key);
					Keys.Add(MakeShared<FJsonValueObject>(Key));
				}
				Out->SetArrayField(TEXT("keys"), Keys);
				if (Count < Times.Num())
				{
					Out->SetBoolField(TEXT("keys_truncated"), true);
				}
			}
		}
		return Out;
	}

	TSharedPtr<FJsonObject> SectionToJson(const FSequenceContext& Ctx, UMovieSceneSection* Section, int32 Index, bool bIncludeChannels, bool bIncludeKeys, int32 MaxKeys)
	{
		TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetNumberField(TEXT("index"), Index);
		Out->SetStringField(TEXT("class"), Section->GetClass()->GetName());
		if (Section->HasStartFrame())
		{
			Out->SetNumberField(TEXT("start"), Ctx.ToDisplayFrame(Section->GetInclusiveStartFrame()));
		}
		else
		{
			Out->SetField(TEXT("start"), MakeShared<FJsonValueNull>());
		}
		if (Section->HasEndFrame())
		{
			Out->SetNumberField(TEXT("end"), Ctx.ToDisplayFrame(Section->GetExclusiveEndFrame()));
		}
		else
		{
			Out->SetField(TEXT("end"), MakeShared<FJsonValueNull>());
		}
		Out->SetNumberField(TEXT("row"), Section->GetRowIndex());
		Out->SetBoolField(TEXT("active"), Section->IsActive());
		if (Section->IsLocked())
		{
			Out->SetBoolField(TEXT("locked"), true);
		}
		WriteEasing(Ctx, Section, Out);
		WriteTypedSectionFields(Ctx, Section, Out);

		if (bIncludeChannels)
		{
			TArray<TSharedPtr<FJsonValue>> Channels;
			for (const FChannelRef& Ref : GetChannels(Section))
			{
				Channels.Add(MakeShared<FJsonValueObject>(ChannelToJson(Ctx, Ref, bIncludeKeys, MaxKeys)));
			}
			Out->SetArrayField(TEXT("channels"), Channels);
		}
		return Out;
	}

	TSharedPtr<FJsonObject> TrackToJson(const FSequenceContext& Ctx, UMovieSceneTrack* Track, int32 IndexInClass, bool bIncludeChannels, bool bIncludeKeys, int32 MaxKeys)
	{
		TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetStringField(TEXT("class"), Track->GetClass()->GetName());
		Out->SetNumberField(TEXT("track_index"), IndexInClass);
		Out->SetStringField(TEXT("display_name"), Track->GetDisplayName().ToString());
		TArray<TSharedPtr<FJsonValue>> Sections;
		const TArray<UMovieSceneSection*>& All = Track->GetAllSections();
		for (int32 i = 0; i < All.Num(); ++i)
		{
			if (All[i])
			{
				Sections.Add(MakeShared<FJsonValueObject>(SectionToJson(Ctx, All[i], i, bIncludeChannels, bIncludeKeys, MaxKeys)));
			}
		}
		Out->SetArrayField(TEXT("sections"), Sections);
		return Out;
	}

	TArray<TSharedPtr<FJsonValue>> MarkedFramesToJson(const FSequenceContext& Ctx)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		const TArray<FMovieSceneMarkedFrame>& Marks = Ctx.MovieScene->GetMarkedFrames();
		for (int32 i = 0; i < Marks.Num(); ++i)
		{
			TSharedPtr<FJsonObject> Mark = MakeShared<FJsonObject>();
			Mark->SetNumberField(TEXT("index"), i);
			WriteKeyTime(Ctx, Marks[i].FrameNumber, Mark);
			Mark->SetStringField(TEXT("label"), Marks[i].Label);
			if (!Marks[i].Comment.IsEmpty())
			{
				Mark->SetStringField(TEXT("comment"), Marks[i].Comment);
			}
			Out.Add(MakeShared<FJsonValueObject>(Mark));
		}
		return Out;
	}

	// ------------------------------------------------------------------------------------------
	// Writes
	// ------------------------------------------------------------------------------------------

	bool ShouldSave(const TSharedPtr<FJsonObject>& Params)
	{
		bool bSave = true;
		if (Params.IsValid())
		{
			Params->TryGetBoolField(TEXT("save"), bSave);
		}
		return bSave;
	}

	FMonolithActionResult FinishWrite(const FSequenceContext& Ctx, const TSharedPtr<FJsonObject>& Params, const TSharedPtr<FJsonObject>& Result)
	{
		Result->SetStringField(TEXT("asset_path"), Ctx.AssetPath);
		if (ShouldSave(Params))
		{
			FString SaveError;
			const bool bSaved = FMonolithAssetUtils::SaveAssetPackage(Ctx.Sequence->GetPackage(), &SaveError);
			Result->SetBoolField(TEXT("saved"), bSaved);
			if (!bSaved)
			{
				Result->SetStringField(TEXT("save_error"), SaveError.IsEmpty() ? FString(TEXT("Save failed")) : SaveError);
			}
		}
		else
		{
			Result->SetBoolField(TEXT("saved"), false);
		}
		return FMonolithActionResult::Success(Result);
	}

	bool TryGetFrame(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field, int32& OutFrame, FString& OutError)
	{
		const TSharedPtr<FJsonValue> Value = Params->TryGetField(Field);
		if (!Value.IsValid() || Value->IsNull())
		{
			return false;
		}
		double Number = 0.0;
		if (!Value->TryGetNumber(Number) || FMath::Frac(Number) != 0.0)
		{
			OutError = FString::Printf(TEXT("'%s' must be an integer display frame"), Field);
			return false;
		}
		OutFrame = static_cast<int32>(Number);
		return true;
	}
}
