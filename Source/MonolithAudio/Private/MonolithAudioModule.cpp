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
#include "MonolithAudioMetaSoundIntrospectionActions.h"
#endif

// Phase 5 Step 4 (MCP Ergonomics, 2026-05-11) — bulk_fill / describe adapter.
// H5 stub-adapter invariant: Register() ALWAYS runs from StartupModule regardless
// of WITH_METASOUND. M6 invariant: MetaSound paths are #if WITH_METASOUND gated
// INSIDE the adapter; vanilla USoundAttenuation/USoundConcurrency paths run gate-free.
#include "MonolithAudioBulkFillAdapter.h"

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
	FMonolithAudioMetaSoundIntrospectionActions::RegisterActions(Registry);
#endif

	// Phase 5 Step 4 — register the audio adapter on the central
	// FMonolithBulkFillRegistry. H5 invariant: this call runs unconditionally;
	// the BODY splits per fill_kind with WITH_METASOUND gating MetaSound paths only.
	FMonolithAudioBulkFillAdapter::Register();

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
	FMonolithAudioBulkFillAdapter::Unregister();
	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("audio"));
}

IMPLEMENT_MODULE(FMonolithAudioModule, MonolithAudio)
