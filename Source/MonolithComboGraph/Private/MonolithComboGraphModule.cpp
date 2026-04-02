#include "MonolithComboGraphModule.h"
#include "MonolithToolRegistry.h"
#include "MonolithSettings.h"
#include "MonolithComboGraphActions.h"

DEFINE_LOG_CATEGORY(LogMonolithComboGraph);

void FMonolithComboGraphModule::StartupModule()
{
	const UMonolithSettings* Settings = GetDefault<UMonolithSettings>();
	if (!Settings || !Settings->bEnableComboGraph)
	{
		UE_LOG(LogMonolithComboGraph, Log,
			TEXT("MonolithComboGraph: ComboGraph integration disabled in settings"));
		return;
	}

#if WITH_COMBOGRAPH
	FMonolithComboGraphActions::RegisterActions(FMonolithToolRegistry::Get());
	int32 ActionCount = FMonolithToolRegistry::Get().GetActions(TEXT("combograph")).Num();
	UE_LOG(LogMonolithComboGraph, Log,
		TEXT("MonolithComboGraph: Loaded (%d actions)"), ActionCount);
#else
	UE_LOG(LogMonolithComboGraph, Log,
		TEXT("MonolithComboGraph: ComboGraph plugin not found at compile time, bridge inactive"));
#endif
}

void FMonolithComboGraphModule::ShutdownModule()
{
	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("combograph"));
}

IMPLEMENT_MODULE(FMonolithComboGraphModule, MonolithComboGraph)
