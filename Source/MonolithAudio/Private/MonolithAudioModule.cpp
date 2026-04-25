#include "MonolithAudioModule.h"
#include "MonolithToolRegistry.h"
#include "MonolithSettings.h"
#include "MonolithJsonUtils.h"
#include "MonolithAudioAssetActions.h"
#include "MonolithAudioQueryActions.h"
#include "MonolithAudioBatchActions.h"
#include "MonolithAudioSoundCueActions.h"
#include "MonolithAudioPerceptionActions.h"
#if WITH_METASOUND
#include "MonolithAudioMetaSoundActions.h"
#endif

void FMonolithAudioModule::StartupModule()
{
	const UMonolithSettings* Settings = GetDefault<UMonolithSettings>();
	if (!Settings || !Settings->bEnableAudio)
	{
		UE_LOG(LogMonolith, Log,
			TEXT("MonolithAudio: Audio module disabled in settings"));
		return;
	}

	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	FMonolithAudioAssetActions::RegisterActions(Registry);
	FMonolithAudioQueryActions::RegisterActions(Registry);
	FMonolithAudioBatchActions::RegisterActions(Registry);
	FMonolithAudioSoundCueActions::RegisterActions(Registry);
	FMonolithAudioPerceptionActions::RegisterActions(Registry);
#if WITH_METASOUND
	FMonolithAudioMetaSoundActions::RegisterActions(Registry);
#endif

	int32 ActionCount = Registry.GetActions(TEXT("audio")).Num();
	const TCHAR* MetaSoundStatus =
#if WITH_METASOUND
		TEXT("available");
#else
		TEXT("not installed");
#endif
	UE_LOG(LogMonolith, Log, TEXT("MonolithAudio: Loaded (%d actions, MetaSound=%s)"), ActionCount, MetaSoundStatus);
}

void FMonolithAudioModule::ShutdownModule()
{
	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("audio"));
}

IMPLEMENT_MODULE(FMonolithAudioModule, MonolithAudio)
