// MonolithCommonUIActionsAggregator.cpp
// Central registration aggregator for all CommonUI categories. Each category file implements
// a namespace-scoped Register() function that this aggregator calls from StartupModule().
#if WITH_COMMONUI

#include "CommonUI/MonolithCommonUIActions.h"
#include "MonolithToolRegistry.h"

// Forward declarations — each category .cpp implements these.
namespace MonolithCommonUIActivatable    { void Register(FMonolithToolRegistry&); }
namespace MonolithCommonUIButton         { void Register(FMonolithToolRegistry&); }
namespace MonolithCommonUIInput          { void Register(FMonolithToolRegistry&); }
namespace MonolithCommonUINavigation     { void Register(FMonolithToolRegistry&); }
namespace MonolithCommonUIList           { void Register(FMonolithToolRegistry&); }
namespace MonolithCommonUIContent        { void Register(FMonolithToolRegistry&); }
namespace MonolithCommonUIDialog         { void Register(FMonolithToolRegistry&); }
namespace MonolithCommonUIAudit          { void Register(FMonolithToolRegistry&); }
namespace MonolithCommonUIAccessibility  { void Register(FMonolithToolRegistry&); }
// Phase 3 (2026-05-16 UI Gap Audit) — Tier-3 headline scaffolders
// (scaffold_main_menu / scaffold_settings_panel_with_tabs / scaffold_pause_menu).
namespace MonolithCommonUITemplate        { void Register(FMonolithToolRegistry&); }

void FMonolithCommonUIActions::RegisterAll(FMonolithToolRegistry& Registry)
{
	MonolithCommonUIActivatable::Register(Registry);
	MonolithCommonUIButton::Register(Registry);
	MonolithCommonUIInput::Register(Registry);
	MonolithCommonUINavigation::Register(Registry);
	MonolithCommonUIList::Register(Registry);
	MonolithCommonUIContent::Register(Registry);
	MonolithCommonUIDialog::Register(Registry);
	MonolithCommonUIAudit::Register(Registry);
	MonolithCommonUIAccessibility::Register(Registry);
	MonolithCommonUITemplate::Register(Registry);
}

#endif // WITH_COMMONUI
