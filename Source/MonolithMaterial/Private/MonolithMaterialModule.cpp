#include "MonolithMaterialModule.h"
#include "MonolithMaterialActions.h"
#include "MonolithMaterialBulkFillAdapter.h"
#include "MonolithToolRegistry.h"
#include "MonolithJsonUtils.h"
#include "MonolithSettings.h"

#define LOCTEXT_NAMESPACE "FMonolithMaterialModule"

void FMonolithMaterialModule::StartupModule()
{
	if (!GetDefault<UMonolithSettings>()->bEnableMaterial) return;

	FMonolithMaterialActions::RegisterActions(FMonolithToolRegistry::Get());

	// Phase 5 Step 3 (MCP Ergonomics, 2026-05-11) — register the material adapter.
	// No WITH_* gate (material editor always-on). Body uses MIC's typed setters
	// (SetScalarParameterValueEditorOnly, etc.) for MICParameters fill_kind and
	// audits BuildMaterialGraph for SilentDrops; rejects MaterialAttributeLayers
	// writes per design Non-Goals §29.
	FMonolithMaterialBulkFillAdapter::Register();

	UE_LOG(LogMonolith, Log, TEXT("Monolith — Material module loaded (25 actions)"));
}

void FMonolithMaterialModule::ShutdownModule()
{
	FMonolithMaterialBulkFillAdapter::Unregister();
	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("material"));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FMonolithMaterialModule, MonolithMaterial)
