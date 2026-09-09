#include "MonolithMeshCatalog.h"
#include "Misc/Paths.h"
#include "MonolithIndexDatabase.h"
#include "SQLiteDatabase.h"
#include "Dom/JsonValue.h"

bool FMonolithMeshCatalog::CreateTable(FSQLiteDatabase& DB)
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

	FSQLitePreparedStatement Stmt;
	Stmt.Create(DB, *CreateSQL);
	return Stmt.Execute();
}

bool FMonolithMeshCatalog::InsertEntry(
	FSQLiteDatabase& DB,
	const FString& AssetPath,
	float BoundsX, float BoundsY, float BoundsZ,
	float BoundsMin, float BoundsMid, float BoundsMax,
	float Volume,
	const FString& SizeClass,
	const FString& Category,
	int32 TriCount,
	bool bHasCollision,
	int32 LodCount,
	float PivotOffsetZ,
	bool bDegenerate)
{
	const FString SQL = TEXT(
		"INSERT OR REPLACE INTO mesh_catalog "
		"(asset_path, bounds_x, bounds_y, bounds_z, bounds_min, bounds_mid, bounds_max, "
		"volume, size_class, category, tri_count, has_collision, lod_count, pivot_offset_z, degenerate) "
		"VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);"
	);

	FSQLitePreparedStatement Stmt;
	Stmt.Create(DB, *SQL);
	Stmt.SetBindingValueByIndex(1, AssetPath);
	Stmt.SetBindingValueByIndex(2, static_cast<double>(BoundsX));
	Stmt.SetBindingValueByIndex(3, static_cast<double>(BoundsY));
	Stmt.SetBindingValueByIndex(4, static_cast<double>(BoundsZ));
	Stmt.SetBindingValueByIndex(5, static_cast<double>(BoundsMin));
	Stmt.SetBindingValueByIndex(6, static_cast<double>(BoundsMid));
	Stmt.SetBindingValueByIndex(7, static_cast<double>(BoundsMax));
	Stmt.SetBindingValueByIndex(8, static_cast<double>(Volume));
	Stmt.SetBindingValueByIndex(9, SizeClass);
	Stmt.SetBindingValueByIndex(10, Category);
	Stmt.SetBindingValueByIndex(11, static_cast<int64>(TriCount));
	Stmt.SetBindingValueByIndex(12, static_cast<int64>(bHasCollision ? 1 : 0));
	Stmt.SetBindingValueByIndex(13, static_cast<int64>(LodCount));
	Stmt.SetBindingValueByIndex(14, static_cast<double>(PivotOffsetZ));
	Stmt.SetBindingValueByIndex(15, static_cast<int64>(bDegenerate ? 1 : 0));

	return Stmt.Execute();
}

TSharedPtr<FJsonObject> FMonolithMeshCatalog::SearchBySize(
	FSQLiteDatabase& DB,
	const TArray<float>& MinBounds,
	const TArray<float>& MaxBounds,
	const FString& Category,
	const FString& ExcludeSizeClass,
	int32 Limit)
{
	auto Result = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> ResultsArray;

	// Build query — search sorted dimensions for orientation-independent matching
	FString SQL = TEXT(
		"SELECT asset_path, bounds_x, bounds_y, bounds_z, category, tri_count, size_class "
		"FROM mesh_catalog WHERE degenerate = 0"
		" AND bounds_min >= ? AND bounds_min <= ?"
		" AND bounds_mid >= ? AND bounds_mid <= ?"
		" AND bounds_max >= ? AND bounds_max <= ?"
	);

	// Sort input bounds smallest-to-largest
	float SortedMin[3] = { MinBounds[0], MinBounds[1], MinBounds[2] };
	float SortedMax[3] = { MaxBounds[0], MaxBounds[1], MaxBounds[2] };
	// Sort min bounds
	if (SortedMin[0] > SortedMin[1]) Swap(SortedMin[0], SortedMin[1]);
	if (SortedMin[1] > SortedMin[2]) Swap(SortedMin[1], SortedMin[2]);
	if (SortedMin[0] > SortedMin[1]) Swap(SortedMin[0], SortedMin[1]);
	// Sort max bounds
	if (SortedMax[0] > SortedMax[1]) Swap(SortedMax[0], SortedMax[1]);
	if (SortedMax[1] > SortedMax[2]) Swap(SortedMax[1], SortedMax[2]);
	if (SortedMax[0] > SortedMax[1]) Swap(SortedMax[0], SortedMax[1]);

	if (!Category.IsEmpty())
	{
		SQL += TEXT(" AND category LIKE ?");
	}

	if (!ExcludeSizeClass.IsEmpty())
	{
		SQL += TEXT(" AND size_class != ?");
	}

	SQL += TEXT(" LIMIT ?;");

	FSQLitePreparedStatement Stmt;
	Stmt.Create(DB, *SQL);

	// Bind all parameters (1-indexed)
	int32 BindIdx = 1;
	Stmt.SetBindingValueByIndex(BindIdx++, static_cast<double>(SortedMin[0]));
	Stmt.SetBindingValueByIndex(BindIdx++, static_cast<double>(SortedMax[0]));
	Stmt.SetBindingValueByIndex(BindIdx++, static_cast<double>(SortedMin[1]));
	Stmt.SetBindingValueByIndex(BindIdx++, static_cast<double>(SortedMax[1]));
	Stmt.SetBindingValueByIndex(BindIdx++, static_cast<double>(SortedMin[2]));
	Stmt.SetBindingValueByIndex(BindIdx++, static_cast<double>(SortedMax[2]));

	if (!Category.IsEmpty())
	{
		Stmt.SetBindingValueByIndex(BindIdx++, Category + TEXT("%"));
	}

	if (!ExcludeSizeClass.IsEmpty())
	{
		Stmt.SetBindingValueByIndex(BindIdx++, ExcludeSizeClass);
	}

	Stmt.SetBindingValueByIndex(BindIdx++, static_cast<int64>(Limit));

	int32 Total = 0;
	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		auto Entry = MakeShared<FJsonObject>();
		FString AssetPath;
		FString RowCategory;
		FString SizeClass;
		double BX = 0, BY = 0, BZ = 0;
		int64 TriCount = 0;
		Stmt.GetColumnValueByIndex(0, AssetPath);
		Stmt.GetColumnValueByIndex(1, BX);
		Stmt.GetColumnValueByIndex(2, BY);
		Stmt.GetColumnValueByIndex(3, BZ);
		Stmt.GetColumnValueByIndex(4, RowCategory);
		Stmt.GetColumnValueByIndex(5, TriCount);
		Stmt.GetColumnValueByIndex(6, SizeClass);

		Entry->SetStringField(TEXT("asset_path"), AssetPath);
		TArray<TSharedPtr<FJsonValue>> BoundsArr;
		BoundsArr.Add(MakeShared<FJsonValueNumber>(BX));
		BoundsArr.Add(MakeShared<FJsonValueNumber>(BY));
		BoundsArr.Add(MakeShared<FJsonValueNumber>(BZ));
		Entry->SetArrayField(TEXT("bounds"), BoundsArr);
		Entry->SetStringField(TEXT("category"), RowCategory);
		Entry->SetNumberField(TEXT("tri_count"), static_cast<double>(TriCount));
		Entry->SetStringField(TEXT("size_class"), SizeClass);

		ResultsArray.Add(MakeShared<FJsonValueObject>(Entry));
		Total++;
	}

	Result->SetArrayField(TEXT("results"), ResultsArray);
	Result->SetNumberField(TEXT("total"), Total);
	return Result;
}

TSharedPtr<FJsonObject> FMonolithMeshCatalog::GetStats(FSQLiteDatabase& DB)
{
	auto Result = MakeShared<FJsonObject>();

	// Total count
	{
		FSQLitePreparedStatement Stmt;
		Stmt.Create(DB, TEXT("SELECT COUNT(*) FROM mesh_catalog;"));
		if (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
		{
			int64 TotalMeshes = 0;
			Stmt.GetColumnValueByIndex(0, TotalMeshes);
			Result->SetNumberField(TEXT("total_meshes"), static_cast<double>(TotalMeshes));
		}
	}

	// Categories breakdown
	{
		auto Categories = MakeShared<FJsonObject>();
		FSQLitePreparedStatement Stmt;
		Stmt.Create(DB, TEXT("SELECT category, COUNT(*) as cnt FROM mesh_catalog GROUP BY category ORDER BY cnt DESC;"));
		while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
		{
			FString Cat;
			int64 Count = 0;
			Stmt.GetColumnValueByIndex(0, Cat);
			Stmt.GetColumnValueByIndex(1, Count);
			Categories->SetNumberField(Cat, static_cast<double>(Count));
		}
		Result->SetObjectField(TEXT("categories"), Categories);
	}

	// Size distribution
	{
		auto SizeDist = MakeShared<FJsonObject>();
		FSQLitePreparedStatement Stmt;
		Stmt.Create(DB, TEXT("SELECT size_class, COUNT(*) as cnt FROM mesh_catalog GROUP BY size_class ORDER BY cnt DESC;"));
		while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
		{
			FString SizeClass;
			int64 Count = 0;
			Stmt.GetColumnValueByIndex(0, SizeClass);
			Stmt.GetColumnValueByIndex(1, Count);
			SizeDist->SetNumberField(SizeClass, static_cast<double>(Count));
		}
		Result->SetObjectField(TEXT("size_distribution"), SizeDist);
	}

	return Result;
}

FString FMonolithMeshCatalog::ClassifySizeClass(float BoundsMax)
{
	if (BoundsMax < 10.0f)       return TEXT("tiny");
	if (BoundsMax < 50.0f)       return TEXT("small");
	if (BoundsMax < 200.0f)      return TEXT("medium");
	if (BoundsMax < 500.0f)      return TEXT("large");
	return TEXT("huge");
}

FString FMonolithMeshCatalog::InferCategory(const FString& AssetPath)
{
	FString Folder = FPaths::GetPath(AssetPath);
	Folder.RemoveFromStart(TEXT("/Game/"));
	TArray<FString> Parts;
	Folder.ParseIntoArray(Parts, TEXT("/"));
	if (Parts.Num() >= 2)
	{
		return Parts[0] + TEXT(".") + Parts[1];
	}
	if (Parts.Num() == 1)
	{
		return Parts[0];
	}
	return TEXT("Uncategorized");
}
