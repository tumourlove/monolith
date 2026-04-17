#include "Indexers/MeshCatalogIndexer.h"
#include "MonolithIndexDatabase.h"
#include "MonolithMemoryHelper.h"
#include "MonolithSettings.h"
#include "Engine/StaticMesh.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetCompilingManager.h"
#include "SQLiteDatabase.h"

bool FMeshCatalogIndexer::IndexAsset(const FAssetData& AssetData, UObject* LoadedAsset, FMonolithIndexDatabase& DB, int64 AssetId)
{
	FSQLiteDatabase* RawDB = DB.GetRawDatabase();
	if (!RawDB)
	{
		UE_LOG(LogMonolithIndex, Error, TEXT("MeshCatalogIndexer: No raw database available"));
		return false;
	}

	// Create table and indices
	{
		const FString CreateSQL = TEXT(
			"CREATE TABLE IF NOT EXISTS mesh_catalog ("
			"    asset_path TEXT PRIMARY KEY,"
			"    bounds_x REAL, bounds_y REAL, bounds_z REAL,"
			"    bounds_min REAL, bounds_mid REAL, bounds_max REAL,"
			"    volume REAL, size_class TEXT, category TEXT,"
			"    tri_count INTEGER, has_collision INTEGER, lod_count INTEGER,"
			"    pivot_offset_z REAL, degenerate INTEGER DEFAULT 0"
			");"
		);

		const FString IdxDims = TEXT("CREATE INDEX IF NOT EXISTS idx_mesh_sorted_dims ON mesh_catalog(bounds_min, bounds_mid, bounds_max);");
		const FString IdxCat = TEXT("CREATE INDEX IF NOT EXISTS idx_mesh_category ON mesh_catalog(category);");
		const FString IdxSize = TEXT("CREATE INDEX IF NOT EXISTS idx_mesh_size_class ON mesh_catalog(size_class);");

		FSQLitePreparedStatement CreateStmt;
		CreateStmt.Create(*RawDB, *CreateSQL);
		CreateStmt.Execute();

		FSQLitePreparedStatement Idx1; Idx1.Create(*RawDB, *IdxDims); Idx1.Execute();
		FSQLitePreparedStatement Idx2; Idx2.Create(*RawDB, *IdxCat); Idx2.Execute();
		FSQLitePreparedStatement Idx3; Idx3.Create(*RawDB, *IdxSize); Idx3.Execute();
	}

	// Clear existing catalog data for fresh rebuild
	{
		FSQLitePreparedStatement DeleteStmt;
		DeleteStmt.Create(*RawDB, TEXT("DELETE FROM mesh_catalog;"));
		DeleteStmt.Execute();
	}

	// Enumerate all StaticMesh assets via Asset Registry across all indexed paths
	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
	TArray<FAssetData> MeshAssets;
	{
		FARFilter Filter;
		Filter.ClassPaths.Add(UStaticMesh::StaticClass()->GetClassPathName());
		if (IndexedPaths.Num() > 0)
		{
			for (const FName& Path : IndexedPaths)
			{
				Filter.PackagePaths.Add(Path);
			}
		}
		else
		{
			Filter.PackagePaths.Add(FName(TEXT("/Game")));
		}
		Filter.bRecursivePaths = true;
		AssetRegistry.GetAssets(Filter, MeshAssets);
	}

	// Get settings for batching
	const UMonolithSettings* Settings = GetDefault<UMonolithSettings>();
	const int32 BatchSize = FMath::Max(1, Settings->PostPassBatchSize);
	const SIZE_T MemoryBudgetMB = static_cast<SIZE_T>(Settings->MemoryBudgetMB);
	const bool bLogMemory = Settings->bLogMemoryStats;

	UE_LOG(LogMonolithIndex, Log, TEXT("MeshCatalogIndexer: Found %d StaticMesh assets to catalog (batch size: %d)"),
		MeshAssets.Num(), BatchSize);

	if (bLogMemory)
	{
		FMonolithMemoryHelper::LogMemoryStats(TEXT("MeshCatalogIndexer start"));
	}

	// Prepare insert statement once — reuse via Reset() per iteration
	FSQLitePreparedStatement InsertStmt;
	InsertStmt.Create(*RawDB, TEXT(
		"INSERT OR REPLACE INTO mesh_catalog "
		"(asset_path, bounds_x, bounds_y, bounds_z, bounds_min, bounds_mid, bounds_max, "
		"volume, size_class, category, tri_count, has_collision, lod_count, pivot_offset_z, degenerate) "
		"VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);"
	));

	int32 Indexed = 0;
	int32 Errors = 0;
	int32 BatchNumber = 0;

	for (int32 i = 0; i < MeshAssets.Num(); i += BatchSize)
	{
		// Finish pending asset compilations before loading more assets
		// This prevents reentrant texture compiler crashes
		FAssetCompilingManager::Get().FinishAllCompilation();

		// Memory budget check before each batch
		if (FMonolithMemoryHelper::ShouldThrottle(MemoryBudgetMB))
		{
			UE_LOG(LogMonolithIndex, Log, TEXT("MeshCatalogIndexer: Memory budget exceeded, forcing GC..."));
			FMonolithMemoryHelper::ForceGarbageCollection(true);
			FMonolithMemoryHelper::YieldToEditor();

			if (bLogMemory)
			{
				FMonolithMemoryHelper::LogMemoryStats(TEXT("MeshCatalogIndexer after throttle GC"));
			}
		}

		int32 BatchEnd = FMath::Min(i + BatchSize, MeshAssets.Num());

		// Process batch
		for (int32 j = i; j < BatchEnd; ++j)
		{
			const FAssetData& MeshAssetData = MeshAssets[j];

			UStaticMesh* Mesh = Cast<UStaticMesh>(MeshAssetData.GetAsset());
			if (!Mesh)
			{
				Errors++;
				continue;
			}

			FStaticMeshRenderData* RenderData = Mesh->GetRenderData();
			if (!RenderData || RenderData->LODResources.Num() == 0)
			{
				FMonolithMemoryHelper::TryUnloadPackage(Mesh);
				Errors++;
				continue;
			}

			const FString Path = MeshAssetData.GetObjectPathString();

			// Bounds
			FBoxSphereBounds Bounds = Mesh->GetBounds();
			float ExtX = Bounds.BoxExtent.X * 2.0f;
			float ExtY = Bounds.BoxExtent.Y * 2.0f;
			float ExtZ = Bounds.BoxExtent.Z * 2.0f;

			// Sort axes for orientation-independent matching
			float Axes[3] = { ExtX, ExtY, ExtZ };
			if (Axes[0] > Axes[1]) Swap(Axes[0], Axes[1]);
			if (Axes[1] > Axes[2]) Swap(Axes[1], Axes[2]);
			if (Axes[0] > Axes[1]) Swap(Axes[0], Axes[1]);

			float Volume = ExtX * ExtY * ExtZ;

			// Size classification based on largest axis
			FString SizeClass;
			if (Axes[2] < 10.0f)        SizeClass = TEXT("tiny");
			else if (Axes[2] < 50.0f)   SizeClass = TEXT("small");
			else if (Axes[2] < 200.0f)  SizeClass = TEXT("medium");
			else if (Axes[2] < 500.0f)  SizeClass = TEXT("large");
			else                         SizeClass = TEXT("huge");

			// Infer category from folder path
			FString Category;
			{
				FString Folder = FPaths::GetPath(Path);
				if (!Folder.RemoveFromStart(TEXT("/Game/")))
				{
					if (Folder.StartsWith(TEXT("/")))
					{
						Folder.RemoveFromStart(TEXT("/"));
						int32 SlashIdx;
						if (Folder.FindChar(TEXT('/'), SlashIdx))
						{
							Folder.RightChopInline(SlashIdx + 1);
						}
					}
				}
				TArray<FString> Parts;
				Folder.ParseIntoArray(Parts, TEXT("/"));
				if (Parts.Num() >= 2)
					Category = Parts[0] + TEXT(".") + Parts[1];
				else if (Parts.Num() == 1)
					Category = Parts[0];
				else
					Category = TEXT("Uncategorized");
			}

			// Tri count from LOD0
			int32 TriCount = RenderData->LODResources[0].GetNumTriangles();

			// Collision
			bool bHasCollision = Mesh->GetBodySetup() != nullptr;

			// LOD count
			int32 LodCount = Mesh->GetNumLODs();

			// Pivot offset Z: mesh origin (0,0,0) relative to AABB minimum
			float PivotOffsetZ = 0.0f - (Bounds.Origin.Z - Bounds.BoxExtent.Z);

			// Degenerate: any axis < 1cm
			bool bDegenerate = (Axes[0] < 1.0f);

			// Bind and execute
			InsertStmt.Reset();
			InsertStmt.ClearBindings();
			InsertStmt.SetBindingValueByIndex(1, Path);
			InsertStmt.SetBindingValueByIndex(2, static_cast<double>(ExtX));
			InsertStmt.SetBindingValueByIndex(3, static_cast<double>(ExtY));
			InsertStmt.SetBindingValueByIndex(4, static_cast<double>(ExtZ));
			InsertStmt.SetBindingValueByIndex(5, static_cast<double>(Axes[0]));
			InsertStmt.SetBindingValueByIndex(6, static_cast<double>(Axes[1]));
			InsertStmt.SetBindingValueByIndex(7, static_cast<double>(Axes[2]));
			InsertStmt.SetBindingValueByIndex(8, static_cast<double>(Volume));
			InsertStmt.SetBindingValueByIndex(9, SizeClass);
			InsertStmt.SetBindingValueByIndex(10, Category);
			InsertStmt.SetBindingValueByIndex(11, static_cast<int64>(TriCount));
			InsertStmt.SetBindingValueByIndex(12, static_cast<int64>(bHasCollision ? 1 : 0));
			InsertStmt.SetBindingValueByIndex(13, static_cast<int64>(LodCount));
			InsertStmt.SetBindingValueByIndex(14, static_cast<double>(PivotOffsetZ));
			InsertStmt.SetBindingValueByIndex(15, static_cast<int64>(bDegenerate ? 1 : 0));

			if (InsertStmt.Execute())
			{
				Indexed++;
			}
			else
			{
				Errors++;
			}

			// Mark mesh for unloading to free render data memory
			FMonolithMemoryHelper::TryUnloadPackage(Mesh);
		}

		BatchNumber++;

		// GC after each batch - meshes with render data are memory heavy
		FMonolithMemoryHelper::ForceGarbageCollection(false);
		FMonolithMemoryHelper::YieldToEditor();

		// Log progress periodically
		if (BatchNumber % 10 == 0 || BatchEnd == MeshAssets.Num())
		{
			UE_LOG(LogMonolithIndex, Log, TEXT("MeshCatalogIndexer: cataloged %d / %d meshes"),
				Indexed, MeshAssets.Num());

			if (bLogMemory)
			{
				FMonolithMemoryHelper::LogMemoryStats(FString::Printf(TEXT("MeshCatalogIndexer batch %d"), BatchNumber));
			}
		}
	}

	// Final GC
	FMonolithMemoryHelper::ForceGarbageCollection(true);

	UE_LOG(LogMonolithIndex, Log, TEXT("MeshCatalogIndexer: Cataloged %d meshes (%d errors)"), Indexed, Errors);

	if (bLogMemory)
	{
		FMonolithMemoryHelper::LogMemoryStats(TEXT("MeshCatalogIndexer complete"));
	}

	return true;
}
