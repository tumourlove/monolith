#include "MonolithNiagaraModule.h"
#include "MonolithJsonUtils.h"
#include "MonolithNiagaraActions.h"
#include "MonolithNiagaraLayoutActions.h"
#include "MonolithNiagaraBulkFillAdapter.h"
#include "MonolithToolRegistry.h"

#define LOCTEXT_NAMESPACE "FMonolithNiagaraModule"

void FMonolithNiagaraModule::StartupModule()
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	FMonolithNiagaraActions::RegisterActions(Registry);
	FMonolithNiagaraLayoutActions::RegisterActions(Registry);

	// Phase 5 Step 2 (MCP Ergonomics, 2026-05-11) — register the niagara adapter.
	// No WITH_* gate (Niagara is a core engine plugin, always-on). Body rejects
	// GPU-sim params with a WISHLIST error and delegates per-field writes to the
	// FMonolithReflectionWalker.
	FMonolithNiagaraBulkFillAdapter::Register();

	UE_LOG(LogMonolith, Verbose, TEXT("Monolith — Niagara module loaded (42 actions)"));
}

void FMonolithNiagaraModule::ShutdownModule()
{
	FMonolithNiagaraBulkFillAdapter::Unregister();
	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("niagara"));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FMonolithNiagaraModule, MonolithNiagara)
