#include "MonolithAudioRuntimeModule.h"

DEFINE_LOG_CATEGORY(LogMonolithAudioRuntime);

void FMonolithAudioRuntimeModule::StartupModule()
{
	UE_LOG(LogMonolithAudioRuntime, Log,
		TEXT("MonolithAudioRuntime: Module loaded (UWorldSubsystem auto-registers per world)"));
}

void FMonolithAudioRuntimeModule::ShutdownModule()
{
	// UWorldSubsystem instances are torn down with their owning UWorld; nothing to do here.
}

IMPLEMENT_MODULE(FMonolithAudioRuntimeModule, MonolithAudioRuntime)
