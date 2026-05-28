// SPDX-License-Identifier: MIT
// Private declaration for Phase 5 Step 1 ai adapter.
// Pattern: mirrors Phase 1-4 (blueprint/gas/inventory/ui) bulk_fill adapter declarations.

#pragma once

#include "CoreMinimal.h"

struct FBulkFillSpec;
struct FDryRunReport;
struct FSchemaDescriptor;

/**
 * Phase 5 Step 1 — bulk_fill / describe adapter for target_namespace="ai".
 * Self-registers with FMonolithBulkFillRegistry from FMonolithAIModule::StartupModule.
 *
 * No optional-dep gate — MonolithAI is core (AIModule + GameplayTasks + NavigationSystem
 * are all built-in UE 5.7 engine modules, no WITH_* probe in Build.cs).
 *
 * Tree shape (per design B.1 / plan §Phase 5 Step 1):
 *
 *   1. fill_kind=EQSTests (the canonical 200-cell EQS test-array pain point):
 *      {
 *        "fill_kind": "EQSTests",
 *        "tests": [
 *          {"type": "Distance", "weight": 1.0, "score_equation": "Linear", "filter_min": 200, "filter_max": 2000},
 *          {"type": "Trace",    "weight": 2.0, "score_equation": "InverseLinear"},
 *          {"type": "Dot",      "weight": 0.5, "score_equation": "Constant"}
 *        ],
 *        "context_bindings": {"Querier": "Self", "Target": "PlayerPawn"}
 *      }
 *
 *   2. fill_kind=BlackboardKeys (vector-params dict-only quirk):
 *      {
 *        "fill_kind": "BlackboardKeys",
 *        "keys": {
 *          "MoveTarget":  {"type": "Vector", "default": {"x": 0, "y": 0, "z": 0}},
 *          "TargetActor": {"type": "Object", "default": null}
 *        }
 *      }
 *
 *   3. fill_kind=SmartObjectSlots — adapter walks `slots` JSON array via the
 *      reflection walker against USmartObjectDefinition.Slots (UPROPERTY array).
 *
 * Describe (`describe.schema`) surfaces set-once flags (sense affiliation, dominant
 * sense — `bSetOnce=true`), the `lose_sight_radius` 1.1x clamp annotation in
 * ImportTextForm, plus the EQS test-class enumeration via TObjectIterator.
 */
class FMonolithAIBulkFillAdapter
{
public:
	/** Register the adapter pair with FMonolithBulkFillRegistry under namespace "ai". */
	static void Register();

	/** Unregister (called from FMonolithAIModule::ShutdownModule). */
	static void Unregister();

	/** Internal: bulk_fill.apply handler for target_namespace="ai". */
	static FDryRunReport AIBulkFill(const FBulkFillSpec& Spec);

	/** Internal: describe.schema handler for target_namespace="ai". */
	static FSchemaDescriptor AIDescribe(const FString& TargetAsset);
};
