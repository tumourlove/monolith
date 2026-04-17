#include "MonolithMetaSoundModule.h"
#include "MonolithMetaSoundActions.h"
#include "MonolithToolRegistry.h"
#include "MonolithSettings.h"
#include "MonolithJsonUtils.h"

#define LOCTEXT_NAMESPACE "FMonolithMetaSoundModule"

void FMonolithMetaSoundModule::StartupModule()
{
	if (!GetDefault<UMonolithSettings>()->bEnableMetaSound)
	{
		return;
	}

	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	FMonolithMetaSoundActions::RegisterActions(Registry);
	UE_LOG(LogMonolith, Log, TEXT("Monolith - MetaSound module loaded (12 actions)"));
}

void FMonolithMetaSoundModule::ShutdownModule()
{
	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("metasound"));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FMonolithMetaSoundModule, MonolithMetaSound)
