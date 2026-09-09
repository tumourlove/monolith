#include "MonolithConfigModule.h"
#include "MonolithConfigActions.h"
#include "MonolithLocalizationActions.h"
#include "MonolithToolRegistry.h"
#include "MonolithJsonUtils.h"
#include "MonolithSettings.h"

#define LOCTEXT_NAMESPACE "FMonolithConfigModule"

void FMonolithConfigModule::StartupModule()
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();

	// Registered AHEAD of the bEnableConfig gate: culture discovery and
	// StringTable inspection are read-only and depend on nothing this gate
	// controls, so they stay available when config authoring is switched off.
	FMonolithLocalizationActions::RegisterActions(Registry);

	if (!GetDefault<UMonolithSettings>()->bEnableConfig)
	{
		UE_LOG(LogMonolith, Log, TEXT("Monolith — Config actions disabled (%d localization actions still registered)"),
			Registry.GetActions(TEXT("localization")).Num());
		return;
	}

	FMonolithConfigActions::RegisterActions(Registry);
	UE_LOG(LogMonolith, Log, TEXT("Monolith — Config module loaded (%d config actions, %d localization actions)"),
		Registry.GetActions(TEXT("config")).Num(),
		Registry.GetActions(TEXT("localization")).Num());
}

void FMonolithConfigModule::ShutdownModule()
{
	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("config"));
	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("localization"));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FMonolithConfigModule, MonolithConfig)
