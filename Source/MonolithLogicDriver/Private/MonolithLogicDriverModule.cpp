#include "MonolithLogicDriverModule.h"
#include "MonolithToolRegistry.h"
#include "MonolithSettings.h"
#include "MonolithLogicDriverAssetActions.h"
#include "MonolithLogicDriverGraphActions.h"
#include "MonolithLogicDriverNodeActions.h"
#include "MonolithLogicDriverRuntimeActions.h"
#include "MonolithLogicDriverSpecActions.h"
#include "MonolithLogicDriverScaffoldActions.h"
#include "MonolithLogicDriverDiscoveryActions.h"
#include "MonolithLogicDriverComponentActions.h"
#include "MonolithLogicDriverTextGraphActions.h"
#include "MonolithLogicDriverBulkFillAdapter.h"

DECLARE_LOG_CATEGORY_EXTERN(LogMonolithLogicDriver, Log, All);
DEFINE_LOG_CATEGORY(LogMonolithLogicDriver);

void FMonolithLogicDriverModule::StartupModule()
{
	const UMonolithSettings* Settings = GetDefault<UMonolithSettings>();
	if (!Settings || !Settings->bEnableLogicDriver)
	{
		UE_LOG(LogMonolithLogicDriver, Log,
			TEXT("MonolithLogicDriver: LogicDriver integration disabled in settings"));
		return;
	}

#if WITH_LOGICDRIVER
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	FMonolithLogicDriverAssetActions::RegisterActions(Registry);
	FMonolithLogicDriverGraphActions::RegisterActions(Registry);
	FMonolithLogicDriverNodeActions::RegisterActions(Registry);
	FMonolithLogicDriverRuntimeActions::RegisterActions(Registry);
	FMonolithLogicDriverSpecActions::RegisterActions(Registry);
	FMonolithLogicDriverScaffoldActions::RegisterActions(Registry);
	FMonolithLogicDriverDiscoveryActions::RegisterActions(Registry);
	FMonolithLogicDriverComponentActions::RegisterActions(Registry);
	FMonolithLogicDriverTextGraphActions::RegisterActions(Registry);
	int32 ActionCount = Registry.GetActions(TEXT("logicdriver")).Num();
	UE_LOG(LogMonolithLogicDriver, Log,
		TEXT("MonolithLogicDriver: Loaded (%d actions)"), ActionCount);
#else
	UE_LOG(LogMonolithLogicDriver, Log,
		TEXT("MonolithLogicDriver: Logic Driver Pro not found at compile time, bridge inactive"));
#endif

	// Phase 5 Step 7 (MCP Ergonomics, 2026-05-11) — register the logicdriver adapter
	// UNCONDITIONALLY per H5 stub-adapter invariant. Body switches on WITH_LOGICDRIVER:
	// dev build wires real handlers, release/no-LogicDriver build returns a clean
	// error so `monolith_discover("logicdriver")` action surface stays identical
	// across dev + release builds.
	FMonolithLogicDriverBulkFillAdapter::Register();
}

void FMonolithLogicDriverModule::ShutdownModule()
{
	FMonolithLogicDriverBulkFillAdapter::Unregister();
	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("logicdriver"));
}

IMPLEMENT_MODULE(FMonolithLogicDriverModule, MonolithLogicDriver)
