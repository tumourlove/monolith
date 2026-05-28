// SPDX-License-Identifier: MIT
// Private declaration for Phase 2 gas adapter.
// Pattern: mirrors Phase 1 Plugins/Monolith/Source/MonolithBlueprint/Private/MonolithBlueprintBulkFillAdapter.h.

#pragma once

#include "CoreMinimal.h"

struct FBulkFillSpec;
struct FDryRunReport;
struct FSchemaDescriptor;

/**
 * Phase 2 — bulk_fill / describe adapter for target_namespace="gas".
 * Self-registers with FMonolithBulkFillRegistry from FMonolithGASModule::StartupModule.
 *
 * **H5 stub-adapter invariant:** Register() ALWAYS runs (regardless of WITH_GBA).
 * The adapter body switches on WITH_GBA:
 *   - WITH_GBA=1: full AttributeInit DT bulk_fill (200-cell pressure target), FGameplayAttribute
 *                rename-hazard resolver, modifier-magnitude tagged-union describe tree.
 *   - WITH_GBA=0: returns a clean "GAS optional dep not available" error in the FDryRunReport so
 *                `monolith_discover("gas")` action surface stays identical across dev + release builds.
 *
 * Targets in v1:
 *   - UDataTable (RowStruct == FAttributeMetaData) — 20-attr × 10-level = 200 cells in one call.
 *
 * Tree shape (per design B.10 / plan §Phase 2):
 *   {
 *     "target_asset": "/Game/Tests/Monolith/BulkFill/DT_AttrInit200",
 *     "fill_kind": "AttributeInitDataTable",
 *     "attribute_set": "ULeviathanVitalsSet" | "/Game/.../BP_VitalsSet",
 *     "rows": {
 *       "Player.1": { "MaxHealth": 100.0, "HealthRegenRate": 1.0, ... },
 *       "Player.2": { "MaxHealth": 200.0, "HealthRegenRate": 1.0, ... },
 *       ...
 *     }
 *   }
 *
 * Row name convention is engine-native (matches UAttributeSet.h:303-313 doc-comment).
 * FAttributeMetaData carries (BaseValue, MinValue, MaxValue, DerivedAttributeInfo, bCanStack);
 * the column-name "MaxHealth" maps onto BaseValue by default — the row name carries the
 * Group.AttributeSet.Attribute triple per existing FAttributeSetInitterDiscreteLevels semantics.
 */
class FMonolithGASBulkFillAdapter
{
public:
	/** Register the adapter pair with FMonolithBulkFillRegistry under namespace "gas". */
	static void Register();

	/** Unregister (called from FMonolithGASModule::ShutdownModule). */
	static void Unregister();

	/** Internal: bulk_fill.apply handler for target_namespace="gas". */
	static FDryRunReport GasBulkFill(const FBulkFillSpec& Spec);

	/** Internal: describe.schema handler for target_namespace="gas". */
	static FSchemaDescriptor GasDescribe(const FString& TargetAsset);
};
