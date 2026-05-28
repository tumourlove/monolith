// SPDX-License-Identifier: MIT
// Private declaration for Phase 5 Step 3 material adapter.

#pragma once

#include "CoreMinimal.h"

struct FBulkFillSpec;
struct FDryRunReport;
struct FSchemaDescriptor;

/**
 * Phase 5 Step 3 — bulk_fill / describe adapter for target_namespace="material".
 * Self-registers with FMonolithBulkFillRegistry from FMonolithMaterialModule::StartupModule.
 *
 * No optional-dep gate — material editor is always-on UE 5.7 engine surface.
 *
 * Tree shape (per design B.5 / plan §Phase 5 Step 3):
 *
 *   1. fill_kind=MICParameters (the canonical RL_LWSkin_Array_DCR 30+ params pain):
 *      {
 *        "fill_kind": "MICParameters",
 *        "scalars":   { "Roughness": 0.4, "Metallic": 0.0, ... },
 *        "vectors":   { "BaseColor": {"r":0.2,"g":0.3,"b":0.4,"a":1.0}, ... },
 *        "textures":  { "Diffuse": "/Game/T/T_Diffuse", ... },
 *        "switches":  { "UseFurMask": true, ... }
 *      }
 *
 *   2. fill_kind=BuildMaterialGraph — wrapper for build_material_graph. Surfaces
 *      `VectorParameter.DefaultValue` silent-drop + `material_outputs` no-op +
 *      `clear_existing:false` sometimes-still-clears as SilentDrops entries in the
 *      report (per design Cross-Cutting Engine Quirks row).
 *
 * MaterialAttributeLayers writes are EXPLICITLY rejected (WISHLIST in design
 * Non-Goals §29).
 *
 * Describe (`describe.schema`) emits the MIC param-group schema (scalars/vectors/
 * textures/switches) with `bRequired=false` defaults + RangeMin/RangeMax populated
 * from any `UIMin`/`ClampMin`/`UIMax`/`ClampMax` metadata on the parent material's
 * UPROPERTYs.
 */
class FMonolithMaterialBulkFillAdapter
{
public:
	static void Register();
	static void Unregister();
	static FDryRunReport MaterialBulkFill(const FBulkFillSpec& Spec);
	static FSchemaDescriptor MaterialDescribe(const FString& TargetAsset);
};
