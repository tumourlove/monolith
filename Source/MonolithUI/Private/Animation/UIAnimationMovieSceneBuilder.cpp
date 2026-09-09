// Copyright tumourlove. All Rights Reserved.
#include "Animation/UIAnimationMovieSceneBuilder.h"
#include "UObject/Package.h"

#include "Spec/UISpec.h"
#include "Actions/Hoisted/AnimationCoreActions.h"

#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Widget.h"

#include "Animation/WidgetAnimation.h"
#include "MovieScene.h"
#include "Channels/MovieSceneFloatChannel.h"

#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/FrameRate.h"  // FFrameNumber is declared transitively via FFrameRate

namespace MonolithUI::AnimationBuilderInternal
{
#if WITH_EDITORONLY_DATA
    /**
     * Pop an existing UWidgetAnimation off the WBP's animation list so a
     * subsequent FindOrCreate call rebuilds from scratch. The animation object
     * itself is renamed into the transient package — its old GUID entry in
     * `WidgetVariableNameToGuidMap` is also cleared so the new entry's
     * deterministic GUID claim doesn't collide.
     */
    static void RemoveExistingAnimation(UWidgetBlueprint* WBP, const FString& AnimationName)
    {
        if (!WBP)
        {
            return;
        }

        UWidgetAnimation* Existing = nullptr;
        for (UWidgetAnimation* Anim : WBP->Animations)
        {
            if (Anim && Anim->GetDisplayLabel() == AnimationName)
            {
                Existing = Anim;
                break;
            }
        }

        if (!Existing)
        {
            return;
        }

        WBP->Animations.Remove(Existing);
        WBP->WidgetVariableNameToGuidMap.Remove(Existing->GetFName());

        // Rename out so a same-name FindOrCreate doesn't trip over a stale UObject.
        // REN_ForceNoResetLoaders is omitted: Rename stopped calling ResetLoaders
        // before UE 5.7 (the flag is already a no-op there) and 5.8 deprecates it.
        Existing->Rename(
            nullptr,
            GetTransientPackage(),
            REN_DoNotDirty | REN_DontCreateRedirectors);
    }

    /**
     * Translate a FUISpecKeyframe into the FAnimationCoreKey shape consumed by
     * `MonolithUI::InsertFloatChannelKey`. v1 honours scalar values only —
     * Vector2D / Color tracks are deferred to a future expansion that adds
     * `UMovieSceneVectorTrack` / `UMovieSceneColorTrack` ensure helpers.
     */
    static MonolithUI::FAnimationCoreKey TranslateKeyframe(const FUISpecKeyframe& InKey)
    {
        MonolithUI::FAnimationCoreKey OutKey;
        OutKey.Time = InKey.Time;
        OutKey.Value = InKey.ScalarValue;

        // Easing token -> interp string. Default to cubic so `ComputeBezierWeightedTangents`
        // weight outputs interpolate correctly when the spec carried bezier control points.
        const FName EasingTok = InKey.Easing;
        if (EasingTok == NAME_None || EasingTok == FName(TEXT("Cubic")) || EasingTok == FName(TEXT("Bezier")))
        {
            OutKey.Interp = TEXT("cubic");
        }
        else if (EasingTok == FName(TEXT("Linear")))
        {
            OutKey.Interp = TEXT("linear");
        }
        else if (EasingTok == FName(TEXT("Constant")))
        {
            OutKey.Interp = TEXT("constant");
        }
        else
        {
            // Unknown token — fall back to cubic. Validator catches this in Phase K.
            OutKey.Interp = TEXT("cubic");
        }

        if (InKey.bUseCustomTangents)
        {
            OutKey.ArriveTangent = InKey.ArriveTangent;
            OutKey.LeaveTangent = InKey.LeaveTangent;
            OutKey.ArriveTangentWeight = InKey.ArriveWeight;
            OutKey.LeaveTangentWeight = InKey.LeaveWeight;
            OutKey.bHasWeights = (InKey.ArriveWeight != 0.0f) || (InKey.LeaveWeight != 0.0f);
        }
        return OutKey;
    }

    /**
     * One-key-pair from-to sugar expansion. When `Keyframes` is empty but
     * `Duration > 0`, build a two-key cubic segment from value 0 to ScalarValue
     * inferred from the spec's existing target. v1 always uses 0 -> 1 (the
     * caller asks for opacity semantics by setting `TargetProperty=RenderOpacity`);
     * future work will read the current widget value as the from-value.
     */
    static void ExpandFromToSugar(
        const FUISpecAnimation& Animation,
        TArray<MonolithUI::FAnimationCoreKey>& OutKeys)
    {
        if (Animation.Duration <= 0.f)
        {
            return;
        }

        MonolithUI::FAnimationCoreKey K0;
        K0.Time = Animation.Delay;
        K0.Value = 0.f;
        K0.Interp = TEXT("cubic");

        MonolithUI::FAnimationCoreKey K1;
        K1.Time = Animation.Delay + Animation.Duration;
        K1.Value = 1.f;
        K1.Interp = TEXT("cubic");

        // Bezier easing token expands to a CSS ease-in-out (0.42, 0, 0.58, 1.0).
        // Linear / Constant pass through as-is.
        if (Animation.Easing == FName(TEXT("Bezier")) || Animation.Easing == NAME_None)
        {
            MonolithUI::ComputeBezierWeightedTangents(
                /*X1=*/0.42, /*Y1=*/0.0, /*X2=*/0.58, /*Y2=*/1.0,
                /*FromValue=*/0.0, /*ToValue=*/1.0,
                /*StartTime=*/K0.Time, /*EndTime=*/K1.Time,
                K0, K1);
        }
        else if (Animation.Easing == FName(TEXT("Linear")))
        {
            K0.Interp = TEXT("linear");
            K1.Interp = TEXT("linear");
        }
        else if (Animation.Easing == FName(TEXT("Constant")))
        {
            K0.Interp = TEXT("constant");
            K1.Interp = TEXT("constant");
        }

        OutKeys.Add(K0);
        OutKeys.Add(K1);
    }
#endif // WITH_EDITORONLY_DATA
} // namespace MonolithUI::AnimationBuilderInternal


void FUIAnimationMovieSceneBuilder::BuildSingle(
    const FUISpecAnimation& Animation,
    UWidgetBlueprint* TargetWBP,
    const FUIAnimationBuildOptions& Options,
    FUIAnimationRebuildResult& OutResult)
{
#if WITH_EDITORONLY_DATA
    using namespace MonolithUI::AnimationBuilderInternal;

    if (!TargetWBP || !TargetWBP->WidgetTree)
    {
        OutResult.bSuccess = false;
        OutResult.ErrorMessage = TEXT("Target Widget Blueprint is null or has no WidgetTree.");
        return;
    }

    if (Animation.Name.IsNone())
    {
        OutResult.Warnings.Add(TEXT("Animation skipped: empty Name."));
        return;
    }
    if (Animation.TargetWidgetId.IsNone())
    {
        OutResult.Warnings.Add(FString::Printf(
            TEXT("Animation '%s' skipped: empty TargetWidgetId."),
            *Animation.Name.ToString()));
        return;
    }
    if (Animation.TargetProperty.IsNone())
    {
        OutResult.Warnings.Add(FString::Printf(
            TEXT("Animation '%s' skipped: empty TargetProperty."),
            *Animation.Name.ToString()));
        return;
    }

    const FString AnimNameStr = Animation.Name.ToString();
    const FString WidgetNameStr = Animation.TargetWidgetId.ToString();
    const FString PropertyNameStr = Animation.TargetProperty.ToString();

    // Preserve opt-in: skip touching the named animation entirely.
    if (Options.PreserveAnimationsNamed.Contains(Animation.Name))
    {
        ++OutResult.AnimationsPreserved;
        return;
    }

    // Resolve the target widget so we can flag a clear error before any mutation.
    UWidget* TargetWidget = TargetWBP->WidgetTree->FindWidget(Animation.TargetWidgetId);
    if (!TargetWidget)
    {
        OutResult.bSuccess = false;
        OutResult.ErrorMessage = FString::Printf(
            TEXT("Animation '%s' targets widget '%s' which does not exist in the WidgetTree."),
            *AnimNameStr, *WidgetNameStr);
        return;
    }

    // Track whether this is a fresh creation or a recreate, before the destroy step.
    bool bAnimationExisted = false;
    for (UWidgetAnimation* Anim : TargetWBP->Animations)
    {
        if (Anim && Anim->GetDisplayLabel() == AnimNameStr)
        {
            bAnimationExisted = true;
            break;
        }
    }

    if (Options.bDryRun)
    {
        if (bAnimationExisted)
        {
            ++OutResult.AnimationsRecreated;
        }
        else
        {
            ++OutResult.AnimationsCreated;
        }
        return;
    }

    // Recreate policy: drop the existing one so FindOrCreate rebuilds from scratch.
    // (Phase I: Recreate is the only implemented policy. MergeAdditive is reserved.)
    if (bAnimationExisted && Options.Policy == EUIAnimationRebuildPolicy::Recreate)
    {
        RemoveExistingAnimation(TargetWBP, AnimNameStr);
    }

    UWidgetAnimation* WidgetAnim = MonolithUI::FindOrCreateWidgetAnimation(TargetWBP, AnimNameStr);
    if (!WidgetAnim || !WidgetAnim->MovieScene)
    {
        OutResult.bSuccess = false;
        OutResult.ErrorMessage = FString::Printf(
            TEXT("Failed to create or resolve UWidgetAnimation '%s'."), *AnimNameStr);
        return;
    }

    UMovieScene* Scene = WidgetAnim->MovieScene;
    const FFrameRate TickRes = Scene->GetTickResolution();

    // Translate keyframes (or expand from-to sugar).
    TArray<MonolithUI::FAnimationCoreKey> Keys;
    Keys.Reserve(Animation.Keyframes.Num() + 2);
    if (Animation.Keyframes.Num() > 0)
    {
        for (const FUISpecKeyframe& InKey : Animation.Keyframes)
        {
            Keys.Add(TranslateKeyframe(InKey));
        }
    }
    else
    {
        ExpandFromToSugar(Animation, Keys);
    }

    if (Keys.Num() == 0)
    {
        OutResult.Warnings.Add(FString::Printf(
            TEXT("Animation '%s' produced no keyframes (empty Keyframes + zero Duration)."),
            *AnimNameStr));
        // Still count as created/recreated — the empty animation is a valid no-op shell.
    }

    // Determine the section frame range from the keyframe extents (clamped to >= 0).
    double MinTime = 0.0;
    double MaxTime = 0.0;
    for (const MonolithUI::FAnimationCoreKey& Key : Keys)
    {
        MinTime = FMath::Min(MinTime, Key.Time);
        MaxTime = FMath::Max(MaxTime, Key.Time);
    }
    if (MaxTime < Animation.Duration)
    {
        MaxTime = Animation.Duration + Animation.Delay;
    }

    const FFrameNumber StartFrame = TickRes.AsFrameNumber(FMath::Max(0.0, MinTime));
    const FFrameNumber EndFrame = TickRes.AsFrameNumber(FMath::Max(0.0, MaxTime));
    Scene->SetPlaybackRange(StartFrame, FMath::Max(1, (EndFrame - StartFrame).Value));

    const bool bIsRoot = (TargetWidget == TargetWBP->WidgetTree->RootWidget);
    const FGuid PossessableGuid = MonolithUI::EnsureWidgetPossessableBinding(
        WidgetAnim, Scene, WidgetNameStr, bIsRoot);

    const TRange<FFrameNumber> SectionRange(StartFrame, EndFrame);
    FMovieSceneFloatChannel* Channel = MonolithUI::EnsureFloatTrackSectionChannel(
        Scene, PossessableGuid, PropertyNameStr, SectionRange);
    if (!Channel)
    {
        OutResult.bSuccess = false;
        OutResult.ErrorMessage = FString::Printf(
            TEXT("Failed to create float track for '%s.%s' on animation '%s'."),
            *WidgetNameStr, *PropertyNameStr, *AnimNameStr);
        return;
    }
    ++OutResult.TracksCreated;

    for (const MonolithUI::FAnimationCoreKey& Key : Keys)
    {
        MonolithUI::InsertFloatChannelKey(Channel, TickRes, Key);
        ++OutResult.KeysInserted;
    }

    if (bAnimationExisted)
    {
        ++OutResult.AnimationsRecreated;
    }
    else
    {
        ++OutResult.AnimationsCreated;
    }
#else
    (void)Animation;
    (void)TargetWBP;
    (void)Options;
    OutResult.bSuccess = false;
    OutResult.ErrorMessage = TEXT("FUIAnimationMovieSceneBuilder requires WITH_EDITORONLY_DATA.");
#endif
}


FUIAnimationRebuildResult FUIAnimationMovieSceneBuilder::Build(
    const FUISpecDocument& Document,
    UWidgetBlueprint* TargetWBP,
    const FUIAnimationBuildOptions& Options)
{
    FUIAnimationRebuildResult Result;

#if WITH_EDITORONLY_DATA
    if (!TargetWBP)
    {
        Result.bSuccess = false;
        Result.ErrorMessage = TEXT("Target Widget Blueprint is null.");
        return Result;
    }

    for (const FUISpecAnimation& Animation : Document.Animations)
    {
        BuildSingle(Animation, TargetWBP, Options, Result);
        if (!Result.bSuccess)
        {
            // Bail on first hard error — partial-state animation lists are worse
            // than an aborted pass for the LLM to debug.
            return Result;
        }
    }

    if (!Options.bDryRun && Options.bCompileBlueprint)
    {
        FKismetEditorUtilities::CompileBlueprint(TargetWBP);
    }

    if (!Options.bDryRun)
    {
        TargetWBP->MarkPackageDirty();
    }
#else
    (void)Document;
    (void)TargetWBP;
    (void)Options;
    Result.bSuccess = false;
    Result.ErrorMessage = TEXT("FUIAnimationMovieSceneBuilder requires WITH_EDITORONLY_DATA.");
#endif

    return Result;
}
