// SPDX-License-Identifier: MIT
// Private declaration for Phase 5 Step 8 combograph adapter (LAST — TargetType
// is non-UPROPERTY and returns an EXPLICIT unsupported-field error).

#pragma once

#include "CoreMinimal.h"

struct FBulkFillSpec;
struct FDryRunReport;
struct FSchemaDescriptor;

/**
 * Phase 5 Step 8 — bulk_fill / describe adapter for target_namespace="combograph".
 * Self-registers from FMonolithComboGraphModule::StartupModule.
 *
 * **H5 stub-adapter invariant:** Register() ALWAYS runs (regardless of WITH_COMBOGRAPH).
 * Body switches on WITH_COMBOGRAPH:
 *   - WITH_COMBOGRAPH=1: real handler against FComboGraphEffectContainerSpec
 *     reflection-bound fields per design B.9.
 *   - WITH_COMBOGRAPH=0: returns clean "ComboGraph not available" error.
 *
 * **TargetType post-review lock (load-bearing for Step 8):**
 * `TargetType` field on combograph effect containers is NOT a UPROPERTY — it
 * requires custom serialisation (per design Cross-Cutting Engine Quirks row).
 * `TargetType` writes return an EXPLICIT unsupported-field error pointing at
 * the future v1.1 custom-serialisation hook — NOT a silent no-op. Test:
 * confirm the error is loud and clear per §12 cross-phase smoke list.
 *
 * Tree shape (per design B.9 / plan §Phase 5 Step 8):
 *
 *   1. fill_kind=EffectContainers — bulk-set reflection-bound effect-container
 *      fields. `TargetType` writes are rejected with the explicit error described
 *      above. Edges-by-composite-key {from_id, to_id} per design.
 *
 *   2. fill_kind=Edges — bulk-set edge transitions keyed by composite (from, to).
 *
 * Describe surfaces the EdGraph WITH_EDITORONLY_DATA lazy-materialise quirk
 * (layout_combo_graph silently no-ops unless asset opened in editor at least once).
 */
class FMonolithComboGraphBulkFillAdapter
{
public:
	static void Register();
	static void Unregister();
	static FDryRunReport ComboGraphBulkFill(const FBulkFillSpec& Spec);
	static FSchemaDescriptor ComboGraphDescribe(const FString& TargetAsset);
};
