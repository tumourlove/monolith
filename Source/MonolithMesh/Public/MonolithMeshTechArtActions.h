#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

class UMonolithMeshHandlePool;

/**
 * Phase 16: Tech Art Pipeline (7 actions)
 * Import, quality fix, LOD generation, texel density, material cost,
 * collision setup, and lightmap density analysis.
 */
class FMonolithMeshTechArtActions
{
public:
	/** Register all 7 tech art actions with the tool registry */
	static void RegisterActions(FMonolithToolRegistry& Registry);

#if WITH_GEOMETRYSCRIPT
	/** Set the handle pool instance (called during module startup) */
	static void SetHandlePool(UMonolithMeshHandlePool* InPool);
#endif

private:
	/** Import FBX/glTF mesh via IAssetTools::ImportAssetsAutomated */
	static FMonolithActionResult ImportMesh(const TSharedPtr<FJsonObject>& Params);

	/** Export a UStaticMesh / USkeletalMesh asset to FBX file on disk via UAssetExportTask */
	static FMonolithActionResult ExportMesh(const TSharedPtr<FJsonObject>& Params);

	/** Auto-fix mesh quality: weld, degenerate removal, hole fill, normals (GeometryScript) */
	static FMonolithActionResult FixMeshQuality(const TSharedPtr<FJsonObject>& Params);

	/** One-shot LOD generation: simplify + write back to UStaticMesh source models */
	static FMonolithActionResult AutoGenerateLods(const TSharedPtr<FJsonObject>& Params);

	/** Texel density analysis: UV area vs world-space area ratio per section */
	static FMonolithActionResult AnalyzeTexelDensity(const TSharedPtr<FJsonObject>& Params);

	/** Cross-module: spatial query + shader instruction count per material */
	static FMonolithActionResult AnalyzeMaterialCostInRegion(const TSharedPtr<FJsonObject>& Params);

	/** Set collision on a static mesh asset (simple shapes, complex, auto-convex) */
	static FMonolithActionResult SetMeshCollision(const TSharedPtr<FJsonObject>& Params);

	/** Lightmap texel density analysis and resolution recommendations */
	static FMonolithActionResult AnalyzeLightmapDensity(const TSharedPtr<FJsonObject>& Params);

	// --- Helpers ---
	static TArray<TSharedPtr<FJsonValue>> VectorToJsonArray(const FVector& V);

#if WITH_GEOMETRYSCRIPT
	static UMonolithMeshHandlePool* Pool;
#endif
};
