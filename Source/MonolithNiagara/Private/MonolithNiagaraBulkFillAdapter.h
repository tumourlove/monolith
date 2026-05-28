// SPDX-License-Identifier: MIT
// Private declaration for Phase 5 Step 2 niagara adapter.
// Pattern: mirrors Phase 1-4 adapters and Phase 5 Step 1 (ai).

#pragma once

#include "CoreMinimal.h"

struct FBulkFillSpec;
struct FDryRunReport;
struct FSchemaDescriptor;

/**
 * Phase 5 Step 2 — bulk_fill / describe adapter for target_namespace="niagara".
 * Self-registers with FMonolithBulkFillRegistry from FMonolithNiagaraModule::StartupModule.
 *
 * No optional-dep gate — Niagara is a core engine plugin (always-on in UE 5.7
 * Leviathan build).
 *
 * Tree shape (per design B.4 / plan §Phase 5 Step 2):
 *
 *   1. fill_kind=DataInterfaceArray (e.g. fill an FNiagaraDataInterfaceArrayFloat
 *      from an external rows-of-floats JSON):
 *      {
 *        "fill_kind": "DataInterfaceArray",
 *        "user_param": "User.DamageCurve",
 *        "rows": [0.0, 0.1, 0.4, 0.9, 1.0]
 *      }
 *
 *   2. fill_kind=Curve — write FRichCurve keys onto a User.* float-curve param.
 *
 *   3. fill_kind=ParameterOverrides — generic Parameter Overrides write
 *      against the UNiagaraSystem's reflection surface. Rejects GPU-sim params
 *      with the WISHLIST error per Cross-Cutting Engine Quirks row "GPU emitter
 *      introspection one-way".
 *
 * Describe (`describe.schema`) surfaces module_node GUID-stability annotation
 * (GUIDs change on duplicate — Cross-Cutting Engine Quirks row), the
 * `include_overrides_only` flag note (override-vs-inherit invisibility), and
 * the GPU-emitter introspection WISHLIST error tagged on relevant fields.
 */
class FMonolithNiagaraBulkFillAdapter
{
public:
	static void Register();
	static void Unregister();
	static FDryRunReport NiagaraBulkFill(const FBulkFillSpec& Spec);
	static FSchemaDescriptor NiagaraDescribe(const FString& TargetAsset);
};
