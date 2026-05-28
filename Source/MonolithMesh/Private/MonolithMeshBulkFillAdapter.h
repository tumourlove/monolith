// SPDX-License-Identifier: MIT
// Private declaration for Phase 5 Step 5 mesh adapter.

#pragma once

#include "CoreMinimal.h"

struct FBulkFillSpec;
struct FDryRunReport;
struct FSchemaDescriptor;

/**
 * Phase 5 Step 5 — bulk_fill / describe adapter for target_namespace="mesh".
 * Self-registers with FMonolithBulkFillRegistry from FMonolithMeshModule::StartupModule.
 *
 * No optional-dep gate — mesh module is always-on (StaticMesh + DataTable surfaces
 * are core engine; GeometryScript is conditionally compiled but the bulk_fill
 * surface is reflection-bound, not GeometryScript-bound).
 *
 * Tree shape (per design B.6 / plan §Phase 5 Step 5):
 *
 *   1. fill_kind=SurfaceDataTable — populate a surface-mapping DT from JSON rows.
 *      {
 *        "fill_kind": "SurfaceDataTable",
 *        "rows": {
 *          "Surface_Wood":  {"footstep_sc": "/Game/SC/Wood", "impact_decal": "/Game/D/Wood"},
 *          "Surface_Stone": {"footstep_sc": "/Game/SC/Stone", "impact_decal": "/Game/D/Stone"}
 *        }
 *      }
 *
 *   2. fill_kind=ActorProperties — bulk_fill of properties on a spawned actor
 *      (e.g. StaticMeshActor placement). Honours the Mobility-must-be-Movable-
 *      before-SimulatePhysics ordering (per design Cross-Cutting Engine Quirks
 *      row §Editor-state-bound).
 *
 * Describe (`describe.schema`) emits the silent-prerequisite annotation for
 * `monolith_reindex` on `search_meshes_by_size` (design quirk row), and the
 * Mobility-ordering note in any ActorProperties-targeted describe output.
 */
class FMonolithMeshBulkFillAdapter
{
public:
	static void Register();
	static void Unregister();
	static FDryRunReport MeshBulkFill(const FBulkFillSpec& Spec);
	static FSchemaDescriptor MeshDescribe(const FString& TargetAsset);
};
