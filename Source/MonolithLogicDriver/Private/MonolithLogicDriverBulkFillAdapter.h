// SPDX-License-Identifier: MIT
// Private declaration for Phase 5 Step 7 logicdriver adapter.

#pragma once

#include "CoreMinimal.h"

struct FBulkFillSpec;
struct FDryRunReport;
struct FSchemaDescriptor;

/**
 * Phase 5 Step 7 — bulk_fill / describe adapter for target_namespace="logicdriver".
 * Self-registers from FMonolithLogicDriverModule::StartupModule.
 *
 * **H5 stub-adapter invariant:** Register() ALWAYS runs (regardless of WITH_LOGICDRIVER).
 * The adapter body switches on WITH_LOGICDRIVER:
 *   - WITH_LOGICDRIVER=1: real implementation per design B.8 (set_state_properties_bulk
 *     shape with `{state_name: {prop_map}}`, wildcard '*' fanout).
 *   - WITH_LOGICDRIVER=0: returns "LogicDriver not available — WITH_LOGICDRIVER=0
 *     in this build" error in FDryRunReport so `monolith_discover("logicdriver")`
 *     action surface stays identical across dev + release builds.
 *
 * Tree shape (per design B.8 / plan §Phase 5 Step 7):
 *
 *   1. fill_kind=StatePropertiesBulk — bulk-set exposed-properties across N states:
 *      {
 *        "fill_kind": "StatePropertiesBulk",
 *        "states": {
 *          "Idle":   {"AnimToPlay": "/Game/A_Idle"},
 *          "Walk":   {"AnimToPlay": "/Game/A_Walk", "Speed": 200.0},
 *          "*":      {"AmbientLoop": "/Game/SC_AmbientLow"}
 *        }
 *      }
 *
 *   2. fill_kind=TransitionPredicatesBulk — bulk-set transition predicate fields.
 *
 * Describe surfaces:
 *   - `runtime_*` PIE-only annotation (per design Cross-Cutting Engine Quirks
 *     row "runtime_* actions only work in PIE"); bPieBlocked=true on those entries.
 *   - Instanced sub-object GUID stability annotation (per Cross-Cutting Engine
 *     Quirks row "Instanced subobject properties on CDOs reflection-hostile").
 */
class FMonolithLogicDriverBulkFillAdapter
{
public:
	static void Register();
	static void Unregister();
	static FDryRunReport LogicDriverBulkFill(const FBulkFillSpec& Spec);
	static FSchemaDescriptor LogicDriverDescribe(const FString& TargetAsset);
};
