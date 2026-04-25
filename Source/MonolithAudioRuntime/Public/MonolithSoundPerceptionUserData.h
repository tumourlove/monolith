#pragma once

#include "CoreMinimal.h"
#include "Engine/AssetUserData.h"
#include "Templates/SubclassOf.h"
#include "MonolithSoundPerceptionUserData.generated.h"

class UAISense;

/**
 * Design-time perception binding stamped onto a USoundBase asset (Cue / MetaSoundSource / Wave).
 *
 * Pattern: UAssetUserData. Survives editor restart, Diversion commits, and packaging because
 * USoundBase serializes its AssetUserData array (SoundBase.h:246).
 *
 * Runtime side: UMonolithAudioPerceptionSubsystem hooks UAudioComponent::OnAudioPlayStateChangedNative.
 * On Playing (and optionally FadingIn), it reads this payload from the active sound and fires
 * AActor::MakeNoise on the AudioComponent's owner — the canonical entry point that routes through
 * UAIPerceptionSystem::MakeNoiseImpl (registered by UAISense_Hearing).
 *
 * Authoring is via the audio::bind_sound_to_perception MCP action (editor-side).
 */
UCLASS(BlueprintType, MinimalAPI,
       meta = (DisplayName = "Monolith: Sound Perception Binding"))
class UMonolithSoundPerceptionUserData : public UAssetUserData
{
	GENERATED_BODY()

public:
	/** Master switch — author can disable a binding without removing the UserData entirely. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monolith|Perception")
	bool bEnabled = true;

	/** Loudness multiplier passed to FAINoiseEvent::Loudness. Modifies effective range. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monolith|Perception", meta = (ClampMin = "0.0", UIMin = "0.0"))
	float Loudness = 1.0f;

	/**
	 * Max range (cm). 0 = no per-event cap; the listener's HearingRange wins.
	 * Mirrors the FAINoiseEvent::MaxRange contract (AISense_Hearing.h:101-103).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monolith|Perception", meta = (ClampMin = "0.0", UIMin = "0.0"))
	float MaxRange = 0.0f;

	/** Optional FName tag for downstream listener filtering / behaviour-tree branching. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monolith|Perception")
	FName Tag = NAME_None;

	/**
	 * Future-proofed sense class. Only Hearing supported in v1; runtime falls back to Hearing
	 * if this is null. Reserved for v2 (Sight visual stimuli, Damage, etc.).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monolith|Perception")
	TSubclassOf<UAISense> SenseClass;

	/** If true, fire a perception event when the AudioComponent enters FadingIn (in addition to Playing). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monolith|Perception")
	bool bFireOnFadeIn = true;

	/**
	 * If true, skip plays where AudioComponent->GetOwner() is null (2D / no-owner sounds).
	 * Default true — most projects want noise events tied to a real instigator.
	 * Set false to fall back to UAISense_Hearing::ReportNoiseEvent with no instigator.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Monolith|Perception")
	bool bRequireOwningActor = true;

	// --- AssetRegistrySearchable marker ---
	// Lets list_perception_bound_sounds query the registry without loading every audio asset.
	// FName GetMonolithPerceptionTag() {} is queried via AssetRegistry tags after PostEditChange.
};
