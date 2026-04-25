#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "MonolithAudioPerceptionStatics.generated.h"

class USoundBase;
class USoundAttenuation;
class USoundConcurrency;
class AActor;

/**
 * Fire-and-forget mitigation for the H3 plan trap #2:
 * UGameplayStatics::PlaySoundAtLocation builds an FActiveSound DIRECTLY in the audio device
 * without spawning a UAudioComponent — so the perception subsystem's
 * UAudioComponent::OnAudioPlayStateChangedNative hook MISSES every such call.
 *
 * Project authors who need perception coverage on fire-and-forget plays should swap
 * Play Sound At Location for Play Sound And Report Noise — same playback, plus an
 * AActor::MakeNoise dispatch driven from the design-time UMonolithSoundPerceptionUserData binding.
 */
UCLASS(MinimalAPI)
class UMonolithAudioPerceptionStatics : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/**
	 * Plays a sound at a world location AND, if the sound has a Monolith perception binding,
	 * fires AActor::MakeNoise from Instigator (or UAISense_Hearing::ReportNoiseEvent if Instigator is null).
	 *
	 * Honors the binding's Loudness/MaxRange/Tag/bEnabled flags. LoudnessOverride > 0 wins over the binding.
	 * TagOverride != NAME_None wins over the binding.
	 *
	 * Authority: AActor::MakeNoise is BlueprintAuthorityOnly. On clients, the noise dispatch is skipped
	 * but the audio still plays locally (audio is not authority-gated).
	 *
	 * Coverage: Use this whenever you'd reach for Play Sound At Location and you care about
	 * AI hearing. For component-routed plays (UAudioComponent::Play, SpawnSound*), the runtime
	 * subsystem already covers you — call Play directly.
	 */
	UFUNCTION(BlueprintCallable,
		Category = "Monolith|Audio|Perception",
		meta = (WorldContext = "WorldContextObject",
		        AdvancedDisplay = "VolumeMultiplier,LoudnessOverride,Instigator,TagOverride,AttenuationSettings,ConcurrencySettings",
		        Keywords = "play sound noise hearing perception ai"))
	static MONOLITHAUDIORUNTIME_API void PlaySoundAndReportNoise(
		const UObject* WorldContextObject,
		USoundBase* Sound,
		FVector Location,
		float VolumeMultiplier = 1.0f,
		float LoudnessOverride = -1.0f,
		AActor* Instigator = nullptr,
		FName TagOverride = NAME_None,
		USoundAttenuation* AttenuationSettings = nullptr,
		USoundConcurrency* ConcurrencySettings = nullptr);
};
