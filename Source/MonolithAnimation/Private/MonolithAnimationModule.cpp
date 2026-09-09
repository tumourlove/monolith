#include "MonolithAnimationModule.h"
#include "MonolithJsonUtils.h"
#include "MonolithAnimationActions.h"
#include "MonolithAnimationRuntimeActions.h"
#include "MonolithPoseSearchActions.h"
#include "MonolithMirrorTableActions.h"
#include "MonolithControlRigWriteActions.h"
#include "MonolithAbpWriteActions.h"
#include "MonolithAnimLayoutActions.h"
#include "MonolithAnimationBulkFillAdapter.h"
#include "MonolithChooserReadActions.h"
#include "MonolithChooserActions.h"
#include "MonolithChooserAuthoringActions.h"
#include "MonolithAbpGraphSurgeryActions.h"
#include "MonolithRetargetSettingsActions.h"
#include "MonolithSkeletonRetargetActions.h"
#include "MonolithLocomotionAuthoringActions.h"
#include "MonolithToolRegistry.h"

#define LOCTEXT_NAMESPACE "FMonolithAnimationModule"

void FMonolithAnimationModule::StartupModule()
{
	FMonolithAnimationActions::RegisterActions(FMonolithToolRegistry::Get());
	FMonolithAnimationRuntimeActions::RegisterActions(FMonolithToolRegistry::Get());
	FMonolithPoseSearchActions::RegisterActions(FMonolithToolRegistry::Get());
	FMonolithMirrorTableActions::RegisterActions(FMonolithToolRegistry::Get());
	FMonolithControlRigWriteActions::RegisterActions(FMonolithToolRegistry::Get());
	FMonolithAbpWriteActions::RegisterActions(FMonolithToolRegistry::Get());
	FMonolithAnimLayoutActions::RegisterActions(FMonolithToolRegistry::Get());
	FMonolithChooserReadActions::RegisterActions(FMonolithToolRegistry::Get());
	FMonolithChooserActions::RegisterActions(FMonolithToolRegistry::Get());
	FMonolithChooserAuthoringActions::RegisterActions(FMonolithToolRegistry::Get());
	FMonolithAbpGraphSurgeryActions::RegisterActions(FMonolithToolRegistry::Get());
	FMonolithRetargetSettingsActions::RegisterActions(FMonolithToolRegistry::Get());
	FMonolithSkeletonRetargetActions::RegisterActions(FMonolithToolRegistry::Get());
	FMonolithLocomotionAuthoringActions::RegisterActions(FMonolithToolRegistry::Get());

	// Phase 5 Step 6 (MCP Ergonomics, 2026-05-11) — register the animation adapter.
	// PoseSearchDatabase fill_kind replaces the 40+ add_database_animation
	// round-trips per locomotion set (design B.3 pain point).
	FMonolithAnimationBulkFillAdapter::Register();

	UE_LOG(LogMonolith, Verbose, TEXT("Monolith - Animation module loaded"));
}

void FMonolithAnimationModule::ShutdownModule()
{
	FMonolithAnimationBulkFillAdapter::Unregister();
	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("animation"));
	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("chooser"));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FMonolithAnimationModule, MonolithAnimation)
