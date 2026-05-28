#include "MonolithBlueprintModule.h"
#include "MonolithBlueprintActions.h"
#include "MonolithBlueprintVariableActions.h"
#include "MonolithBlueprintComponentActions.h"
#include "MonolithBlueprintGraphActions.h"
#include "MonolithBlueprintNodeActions.h"
#include "MonolithBlueprintCompileActions.h"
#include "MonolithBlueprintCDOActions.h"
#include "MonolithBlueprintStructActions.h"
#include "MonolithBlueprintDataTableActions.h"
#include "MonolithBlueprintCurveTableActions.h"
#include "MonolithBlueprintStringTableActions.h"
#include "MonolithBlueprintBuildActions.h"
#include "MonolithBlueprintDiffActions.h"
#include "MonolithBlueprintTemplateActions.h"
#include "MonolithBlueprintGraphExportActions.h"
#include "MonolithBlueprintLayoutActions.h"
#include "MonolithBlueprintSpawnActions.h"
#include "MonolithBlueprintBulkFillAdapter.h"
#include "MonolithToolRegistry.h"
#include "MonolithJsonUtils.h"
#include "MonolithSettings.h"

#define LOCTEXT_NAMESPACE "FMonolithBlueprintModule"

void FMonolithBlueprintModule::StartupModule()
{
	if (!GetDefault<UMonolithSettings>()->bEnableBlueprint) return;

	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	FMonolithBlueprintActions::RegisterActions();
	FMonolithBlueprintVariableActions::RegisterActions(Registry);
	FMonolithBlueprintComponentActions::RegisterActions(Registry);
	FMonolithBlueprintGraphActions::RegisterActions(Registry);
	FMonolithBlueprintNodeActions::RegisterActions(Registry);
	FMonolithBlueprintCompileActions::RegisterActions(Registry);
	FMonolithBlueprintCDOActions::RegisterActions(Registry);
	FMonolithBlueprintStructActions::RegisterActions(Registry);
	FMonolithBlueprintDataTableActions::RegisterActions(Registry);
	FMonolithBlueprintCurveTableActions::RegisterActions(Registry);
	FMonolithBlueprintStringTableActions::RegisterActions(Registry);
	FMonolithBlueprintBuildActions::RegisterActions(Registry);
	FMonolithBlueprintDiffActions::RegisterActions(Registry);
	FMonolithBlueprintTemplateActions::RegisterActions(Registry);
	FMonolithBlueprintGraphExportActions::RegisterActions(Registry);
	FMonolithBlueprintLayoutActions::RegisterActions(Registry);
	FMonolithBlueprintSpawnActions::RegisterActions(Registry);

	// Phase 1 bulk_fill / describe pilot adapter. Self-registers with
	// FMonolithBulkFillRegistry under namespace "blueprint"; routed-to by the
	// central bulk_fill.apply / describe.schema dispatchers (Phase 0).
	FMonolithBlueprintBulkFillAdapter::Register();

	UE_LOG(LogMonolith, Log, TEXT("Monolith — Blueprint module loaded (105 actions + bulk_fill/describe adapter)"));
}

void FMonolithBlueprintModule::ShutdownModule()
{
	FMonolithBlueprintBulkFillAdapter::Unregister();
	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("blueprint"));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FMonolithBlueprintModule, MonolithBlueprint)
