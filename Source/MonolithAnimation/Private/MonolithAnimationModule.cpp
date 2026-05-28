#include "MonolithAnimationModule.h"
#include "MonolithJsonUtils.h"
#include "MonolithAnimationActions.h"
#include "MonolithPoseSearchActions.h"
#include "MonolithControlRigWriteActions.h"
#include "MonolithAbpWriteActions.h"
#include "MonolithAnimLayoutActions.h"
#include "MonolithAnimationBulkFillAdapter.h"
#include "MonolithToolRegistry.h"

#define LOCTEXT_NAMESPACE "FMonolithAnimationModule"

void FMonolithAnimationModule::StartupModule()
{
	FMonolithAnimationActions::RegisterActions(FMonolithToolRegistry::Get());
	FMonolithPoseSearchActions::RegisterActions(FMonolithToolRegistry::Get());
	FMonolithControlRigWriteActions::RegisterActions(FMonolithToolRegistry::Get());
	FMonolithAbpWriteActions::RegisterActions(FMonolithToolRegistry::Get());
	FMonolithAnimLayoutActions::RegisterActions(FMonolithToolRegistry::Get());

	// Phase 5 Step 6 (MCP Ergonomics, 2026-05-11) — register the animation adapter.
	// PoseSearchDatabase fill_kind replaces the 40+ add_database_animation
	// round-trips per locomotion set (design B.3 pain point).
	FMonolithAnimationBulkFillAdapter::Register();

	UE_LOG(LogMonolith, Verbose, TEXT("Monolith — Animation module loaded (81 actions)"));
}

void FMonolithAnimationModule::ShutdownModule()
{
	FMonolithAnimationBulkFillAdapter::Unregister();
	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("animation"));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FMonolithAnimationModule, MonolithAnimation)
