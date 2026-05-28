// SPDX-License-Identifier: MIT
// Private declaration for Phase 5 Step 6 animation adapter.

#pragma once

#include "CoreMinimal.h"

struct FBulkFillSpec;
struct FDryRunReport;
struct FSchemaDescriptor;

/**
 * Phase 5 Step 6 — bulk_fill / describe adapter for target_namespace="animation".
 * Self-registers from FMonolithAnimationModule::StartupModule.
 *
 * No optional-dep gate — animation/PoseSearch are core engine surface (PoseSearch
 * ships as an Engine plugin enabled by default).
 *
 * Tree shape (per design B.3 / plan §Phase 5 Step 6):
 *
 *   1. fill_kind=PoseSearchDatabase — populate UPoseSearchDatabase entries from
 *      JSON. The canonical 40+ `add_database_animation` round-trips per locomotion
 *      set, replaced by one call.
 *      {
 *        "fill_kind": "PoseSearchDatabase",
 *        "entries": [
 *          {"animation": "/Game/Anim/A_Idle",      "looping": true,  "sample_range": [0.0, 2.0]},
 *          {"animation": "/Game/Anim/A_Walk_F",    "looping": true,  "speed_multiplier": 1.0},
 *          {"animation": "/Game/Anim/A_Walk_F_45", "looping": true,  "rotation_yaw": 45.0}
 *        ]
 *      }
 *
 *   2. fill_kind=NotifyApplyTemplate — apply a notify/curve template to a set of
 *      AnimSequences via folder + name-glob. v1 surfaces audit only; commit
 *      routes through existing animation_query actions.
 *
 * Describe surfaces the CHT_ chooser-table gap (WISHLIST per design).
 */
class FMonolithAnimationBulkFillAdapter
{
public:
	static void Register();
	static void Unregister();
	static FDryRunReport AnimationBulkFill(const FBulkFillSpec& Spec);
	static FSchemaDescriptor AnimationDescribe(const FString& TargetAsset);
};
