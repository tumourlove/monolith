#include "MonolithAudioPerceptionStatics.h"
#include "MonolithAudioRuntimeModule.h"
#include "MonolithSoundPerceptionUserData.h"

#include "Kismet/GameplayStatics.h"
#include "Sound/SoundBase.h"
#include "Sound/SoundAttenuation.h"
#include "Sound/SoundConcurrency.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/Controller.h"
#include "Engine/World.h"

#include "Perception/AISense_Hearing.h"

void UMonolithAudioPerceptionStatics::PlaySoundAndReportNoise(
	const UObject* WorldContextObject,
	USoundBase* Sound,
	FVector Location,
	float VolumeMultiplier,
	float LoudnessOverride,
	AActor* Instigator,
	FName TagOverride,
	USoundAttenuation* AttenuationSettings,
	USoundConcurrency* ConcurrencySettings)
{
	if (!Sound)
	{
		return;
	}

	// Game-thread guard mirrors the subsystem (BP-callable can be called from anywhere by mistake).
	if (!ensureMsgf(IsInGameThread(),
		TEXT("PlaySoundAndReportNoise must be called on the game thread")))
	{
		return;
	}

	// 1. Audio playback — same surface as Play Sound at Location.
	UGameplayStatics::PlaySoundAtLocation(
		WorldContextObject,
		Sound,
		Location,
		FRotator::ZeroRotator,
		VolumeMultiplier,
		/*PitchMultiplier=*/1.0f,
		/*StartTime=*/0.0f,
		AttenuationSettings,
		ConcurrencySettings,
		Instigator);

	// 2. Perception dispatch — only if the sound carries a binding and is enabled.
	UMonolithSoundPerceptionUserData* Data = Cast<UMonolithSoundPerceptionUserData>(
		Sound->GetAssetUserDataOfClass(UMonolithSoundPerceptionUserData::StaticClass()));
	if (!Data || !Data->bEnabled)
	{
		return;
	}

	// Resolve effective values (override wins when explicitly set).
	const float EffectiveLoudness = (LoudnessOverride >= 0.0f) ? LoudnessOverride : Data->Loudness;
	const FName EffectiveTag = (TagOverride != NAME_None) ? TagOverride : Data->Tag;

	UWorld* World = GEngine ? GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::LogAndReturnNull) : nullptr;
	if (!World)
	{
		return;
	}

	if (Instigator)
	{
		// Authority gate.
		if (!Instigator->HasAuthority())
		{
			return;
		}

		APawn* InstigatorPawn = Cast<APawn>(Instigator);
		if (!InstigatorPawn)
		{
			if (AController* Ctrl = Cast<AController>(Instigator))
			{
				InstigatorPawn = Ctrl->GetPawn();
			}
		}

		Instigator->MakeNoise(
			EffectiveLoudness,
			InstigatorPawn,
			Location,
			Data->MaxRange,
			EffectiveTag);
	}
	else
	{
		if (Data->bRequireOwningActor)
		{
			// No instigator and the binding requires one — silently skip.
			return;
		}

		UAISense_Hearing::ReportNoiseEvent(
			World,
			Location,
			EffectiveLoudness,
			/*Instigator=*/nullptr,
			Data->MaxRange,
			EffectiveTag);
	}
}
