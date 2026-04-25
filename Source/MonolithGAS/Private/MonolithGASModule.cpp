#include "MonolithGASModule.h"
#include "MonolithToolRegistry.h"
#include "MonolithSettings.h"
#include "MonolithGASAbilityActions.h"
#include "MonolithGASAttributeActions.h"
#include "MonolithGASEffectActions.h"
#include "MonolithGASASCActions.h"
#include "MonolithGASTagActions.h"
#include "MonolithGASCueActions.h"
#include "MonolithGASTargetActions.h"
#include "MonolithGASInputActions.h"
#include "MonolithGASInspectActions.h"
#include "MonolithGASScaffoldActions.h"
#include "MonolithGASUIBindingActions.h"

DEFINE_LOG_CATEGORY(LogMonolithGAS);

void FMonolithGASModule::StartupModule()
{
	const UMonolithSettings* Settings = GetDefault<UMonolithSettings>();
	if (!Settings || !Settings->bEnableGAS)
	{
		UE_LOG(LogMonolithGAS, Log,
			TEXT("MonolithGAS: GAS integration disabled in settings"));
		return;
	}

	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	FMonolithGASAbilityActions::RegisterActions(Registry);
	FMonolithGASAttributeActions::RegisterActions(Registry);
	FMonolithGASEffectActions::RegisterActions(Registry);
	FMonolithGASASCActions::RegisterActions(Registry);
	FMonolithGASTagActions::RegisterActions(Registry);
	FMonolithGASCueActions::RegisterActions(Registry);
	FMonolithGASTargetActions::RegisterActions(Registry);
	FMonolithGASInputActions::RegisterActions(Registry);
	FMonolithGASInspectActions::RegisterActions(Registry);
	FMonolithGASScaffoldActions::RegisterActions(Registry);
	FMonolithGASUIBindingActions::RegisterActions(Registry);

	int32 ActionCount = Registry.GetActions(TEXT("gas")).Num();
	const TCHAR* GbaStatus =
#if WITH_GBA
		TEXT("available");
#else
		TEXT("not installed");
#endif
	UE_LOG(LogMonolithGAS, Log, TEXT("MonolithGAS: Loaded (%d actions, GBA=%s)"), ActionCount, GbaStatus);
}

void FMonolithGASModule::ShutdownModule()
{
	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("gas"));
}

IMPLEMENT_MODULE(FMonolithGASModule, MonolithGAS)
