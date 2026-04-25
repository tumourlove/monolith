#pragma once

#include "Modules/ModuleInterface.h"
#include "Logging/LogMacros.h"

MONOLITHAUDIORUNTIME_API DECLARE_LOG_CATEGORY_EXTERN(LogMonolithAudioRuntime, Log, All);

/**
 * Runtime-typed Monolith sub-module hosting the audio→AI perception subsystem.
 *
 * Hosts:
 *  - UMonolithSoundPerceptionUserData     (UAssetUserData payload stamped onto USoundBase)
 *  - UMonolithAudioPerceptionSubsystem    (UWorldSubsystem hooks UAudioComponent state changes)
 *  - UMonolithAudioPerceptionStatics      (UBlueprintFunctionLibrary fire-and-forget helper)
 *
 * Editor-side action handlers (audio::bind_sound_to_perception, ...) live in MonolithAudio
 * (Editor-typed) and depend publicly on this module to access the UserData class.
 */
class FMonolithAudioRuntimeModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
