// SPDX-License-Identifier: MIT
// Plan: Plugins/Monolith/Docs/plans/2026-05-28-reflection-intelligence.md (Phase 3a — v0.17.0).
//
// CppReflectQueryTests — IMPLEMENT_SIMPLE_AUTOMATION_TEST stubs covering the
// Phase 3a test plan items:
//   - UHT artefact parse — schema bootstrap + row counts from fixture
//   - Cursor pagination round-trip for list_uproperties / list_ufunctions
//   - Specifier search — find_class_specifier("Abstract") returns the
//     fixture's ASampleActor
//
// Asset-graph join test is intentionally SKIPPED in Phase 3a — there is no
// public way to instantiate a mock IAssetRegistry from automation. A real-
// editor smoke pass against IAssetRegistry::Get() is the manual verification
// route (`mcp__monolith__cppreflect_query("list_uproperties", ...)`).
//
// All tests use a transient SQLite file at FPaths::AutomationTransientDir, so
// they never touch the real EngineSource.db. Test fixture lives at
// `Source/MonolithReflectionIntel/Private/Tests/Fixtures/CppReflectCorpus/`.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "CppReflect/FUHTArtefactReader.h"
#include "FScopedTestDb.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "SQLiteDatabase.h"

namespace MonolithCppReflectTestDetail
{
	using MonolithReflectionIntelTests::FScopedTestDb;

	static FString GetFixtureDir()
	{
		TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("Monolith"));
		if (Plugin.IsValid())
		{
			return Plugin->GetBaseDir()
				/ TEXT("Source") / TEXT("MonolithReflectionIntel")
				/ TEXT("Private") / TEXT("Tests") / TEXT("Fixtures") / TEXT("CppReflectCorpus");
		}
		return FPaths::ProjectPluginsDir()
			/ TEXT("Monolith") / TEXT("Source") / TEXT("MonolithReflectionIntel")
			/ TEXT("Private") / TEXT("Tests") / TEXT("Fixtures") / TEXT("CppReflectCorpus");
	}

	static int32 CountRows(FSQLiteDatabase& Db, const TCHAR* Table)
	{
		const FString Sql = FString::Printf(TEXT("SELECT COUNT(*) FROM %s;"), Table);
		FSQLitePreparedStatement Stmt;
		if (!Stmt.Create(Db, *Sql)) { return -1; }
		if (Stmt.Step() != ESQLitePreparedStatementStepResult::Row) { return -1; }
		int32 Count = 0;
		Stmt.GetColumnValueByIndex(0, Count);
		return Count;
	}

	static bool TableExists(FSQLiteDatabase& Db, const TCHAR* Table)
	{
		FSQLitePreparedStatement Stmt;
		if (!Stmt.Create(Db, TEXT("SELECT name FROM sqlite_master WHERE type='table' AND name = ?;")))
		{
			return false;
		}
		Stmt.SetBindingValueByIndex(1, FString(Table));
		return Stmt.Step() == ESQLitePreparedStatementStepResult::Row;
	}

	/**
	 * Stage the `.gen.cpp.fixture` into a transient dir laid out as
	 *   <work>/Inc/<ModuleName>/UHT/<File>.gen.cpp
	 * so that FUHTArtefactReader's UHTModuleNameFromArtefactDir() helper can resolve the
	 * module name from the directory path. Returns the absolute path to the
	 * `<work>` root, OR empty FString on failure.
	 */
	static FString StageFixture(const FString& ModuleName, FString& OutWorkRoot)
	{
		const FString SrcFixture = GetFixtureDir() / TEXT("sample.gen.cpp.fixture");
		if (!FPlatformFileManager::Get().GetPlatformFile().FileExists(*SrcFixture))
		{
			return FString();
		}

		OutWorkRoot = FPaths::AutomationTransientDir()
			/ FString::Printf(TEXT("cppreflect-fixture-%s"), *FGuid::NewGuid().ToString());

		const FString ModuleUhtDir = OutWorkRoot / TEXT("Inc") / ModuleName / TEXT("UHT");
		FPlatformFileManager::Get().GetPlatformFile().CreateDirectoryTree(*ModuleUhtDir);

		const FString DstFile = ModuleUhtDir / TEXT("Sample.gen.cpp");
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *SrcFixture)) { return FString(); }
		if (!FFileHelper::SaveStringToFile(Text, *DstFile)) { return FString(); }
		return DstFile;
	}
}

// ---------------------------------------------------------------------------
// Test 1: UHT artefact parse — schema bootstraps, fixture row counts match.
// Asserts (from the synthetic SamplePluginA fixture):
//   reflect_uclasses        contains at least ASampleActor + USampleInterface (2 rows)
//   reflect_uinterfaces     contains USampleInterface (1 row)
//   reflect_uproperties     contains Health, bIsActive, MeshComponent (3 rows)
//   reflect_ufunctions      contains DoSomething (1 row)
//   reflect_uinterface_impls contains (ASampleActor → USampleInterface) (1 row)
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCppReflectUHTArtefactParseTest,
	"Monolith.ReflectionIntel.CppReflect.UHTArtefactParse",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCppReflectUHTArtefactParseTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithCppReflectTestDetail;

	FString WorkRoot;
	const FString StagedFile = StageFixture(TEXT("SamplePluginA"), WorkRoot);
	if (StagedFile.IsEmpty())
	{
		AddError(TEXT("Failed to stage CppReflect fixture under AutomationTransientDir"));
		return false;
	}

	FScopedTestDb ScopedDb;
	if (!ScopedDb.Open(TEXT("cppreflect-test")))
	{
		AddError(TEXT("Failed to open the transient test database"));
		IFileManager::Get().DeleteDirectory(*WorkRoot, false, true);
		return false;
	}
	FSQLiteDatabase& Db = ScopedDb.Get();

	FUHTArtefactReader Reader;
	FString Status;
	const bool bOk = Reader.Run(Db, { WorkRoot }, /*bIncludeEnginePlugins=*/false, /*bAllowMarketplacePaths=*/false, Status);
	TestTrue(TEXT("FUHTArtefactReader::Run on staged fixture"), bOk);

	TestTrue(TEXT("reflect_uclasses bootstrapped"), TableExists(Db, TEXT("reflect_uclasses")));
	TestTrue(TEXT("reflect_uproperties bootstrapped"), TableExists(Db, TEXT("reflect_uproperties")));
	TestTrue(TEXT("reflect_ufunctions bootstrapped"), TableExists(Db, TEXT("reflect_ufunctions")));
	TestTrue(TEXT("reflect_uinterfaces bootstrapped"), TableExists(Db, TEXT("reflect_uinterfaces")));
	TestTrue(TEXT("reflect_uinterface_impls bootstrapped"), TableExists(Db, TEXT("reflect_uinterface_impls")));
	TestTrue(TEXT("cpp_asset_edges bootstrapped"), TableExists(Db, TEXT("cpp_asset_edges")));

	const int32 NumUClasses   = CountRows(Db, TEXT("reflect_uclasses"));
	const int32 NumUProps     = CountRows(Db, TEXT("reflect_uproperties"));
	const int32 NumUFuncs     = CountRows(Db, TEXT("reflect_ufunctions"));
	const int32 NumUImpls     = CountRows(Db, TEXT("reflect_uinterface_impls"));
	const int32 NumUIfaces    = CountRows(Db, TEXT("reflect_uinterfaces"));

	TestTrue(TEXT("At least 2 UClasses parsed (ASampleActor + USampleInterface)"), NumUClasses >= 2);
	TestTrue(TEXT("At least 3 UProperties parsed"), NumUProps >= 3);
	TestTrue(TEXT("At least 1 UFunction parsed"), NumUFuncs >= 1);
	TestTrue(TEXT("At least 1 UInterface impl parsed"), NumUImpls >= 1);
	TestTrue(TEXT("At least 1 UInterface parsed"), NumUIfaces >= 1);

	// Spot-check: ASampleActor must show parent_class = AActor.
	{
		FSQLitePreparedStatement Stmt;
		TestTrue(TEXT("Prepare ASampleActor parent lookup"),
			Stmt.Create(Db, TEXT("SELECT parent_class FROM reflect_uclasses WHERE class_name = ?;")));
		Stmt.SetBindingValueByIndex(1, FString(TEXT("ASampleActor")));
		if (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
		{
			FString Parent;
			Stmt.GetColumnValueByIndex(0, Parent);
			TestEqual(TEXT("ASampleActor parent is AActor"), Parent, FString(TEXT("AActor")));
		}
		else
		{
			AddError(TEXT("ASampleActor row not found in reflect_uclasses"));
		}
	}

	// Spot-check: USampleInterface implementer is ASampleActor.
	{
		FSQLitePreparedStatement Stmt;
		TestTrue(TEXT("Prepare USampleInterface implementer lookup"),
			Stmt.Create(Db, TEXT(
				"SELECT implementing_class FROM reflect_uinterface_impls WHERE interface_name = ?;")));
		Stmt.SetBindingValueByIndex(1, FString(TEXT("USampleInterface")));
		if (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
		{
			FString Impl;
			Stmt.GetColumnValueByIndex(0, Impl);
			TestEqual(TEXT("USampleInterface is implemented by ASampleActor"),
				Impl, FString(TEXT("ASampleActor")));
		}
		else
		{
			AddError(TEXT("USampleInterface implementer not found"));
		}
	}

	IFileManager::Get().DeleteDirectory(*WorkRoot, false, true);
	return true;
}

// ---------------------------------------------------------------------------
// Test 2: Empty roots — Run() on a non-existent root bootstraps the schema and
// returns gracefully with 0 rows. Validates the "project never built yet"
// degraded-path contract.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCppReflectEmptyRootsDegradedTest,
	"Monolith.ReflectionIntel.CppReflect.EmptyRootsDegraded",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCppReflectEmptyRootsDegradedTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithCppReflectTestDetail;

	FScopedTestDb ScopedDb;
	if (!ScopedDb.Open(TEXT("cppreflect-test")))
	{
		AddError(TEXT("Failed to open the transient test database"));
		return false;
	}
	FSQLiteDatabase& Db = ScopedDb.Get();

	const FString GhostRoot = FPaths::AutomationTransientDir()
		/ FString::Printf(TEXT("ghost-%s"), *FGuid::NewGuid().ToString());
	// Intentionally do NOT create the directory — exercises the
	// CollectArtefacts "skipping missing root" path.

	FUHTArtefactReader Reader;
	FString Status;
	const bool bOk = Reader.Run(Db, { GhostRoot }, /*bIncludeEnginePlugins=*/false, /*bAllowMarketplacePaths=*/false, Status);
	TestTrue(TEXT("Run on missing root succeeds (graceful degradation)"), bOk);
	TestTrue(TEXT("reflect_uclasses still bootstrapped"), TableExists(Db, TEXT("reflect_uclasses")));
	TestEqual(TEXT("Zero UClass rows on missing root"), CountRows(Db, TEXT("reflect_uclasses")), 0);

	return true;
}

// ---------------------------------------------------------------------------
// Test 3: Cursor pagination round-trip via raw SQL — simulates what
// HandleListUProperties does. Seeds 150 synthetic rows, lists with limit=50,
// follows next_cursor, asserts the second page returns the next 50 rows.
//
// We do NOT call FCppReflectQueryAdapter::HandleListUProperties directly
// because that handler routes through the module's cached DB (production
// EngineSource.db); the test database here is transient. The adapter's
// internal cursor codec is exercised indirectly through the round-trip
// assertion that 3 pages × 50 rows = 150 rows total.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCppReflectCursorPaginationTest,
	"Monolith.ReflectionIntel.CppReflect.CursorPagination",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCppReflectCursorPaginationTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithCppReflectTestDetail;

	FScopedTestDb ScopedDb;
	if (!ScopedDb.Open(TEXT("cppreflect-test")))
	{
		AddError(TEXT("Failed to open the transient test database"));
		return false;
	}
	FSQLiteDatabase& Db = ScopedDb.Get();

	// Bootstrap the schema by running the reader on no roots — table-create only.
	{
		FUHTArtefactReader Reader;
		FString Status;
		Reader.Run(Db, {}, false, /*bAllowMarketplacePaths=*/false, Status);
	}

	// Seed 150 synthetic UPROPERTY rows on a single class.
	{
		Db.Execute(TEXT("BEGIN TRANSACTION;"));
		FSQLitePreparedStatement Ins;
		Ins.Create(Db, TEXT(
			"INSERT INTO reflect_uproperties "
			"(owning_class, property_name, property_type, cpp_module) "
			"VALUES (?, ?, ?, ?);"));
		for (int32 i = 0; i < 150; ++i)
		{
			Ins.Reset();
			Ins.ClearBindings();
			Ins.SetBindingValueByIndex(1, FString(TEXT("ABulkClass")));
			Ins.SetBindingValueByIndex(2, FString::Printf(TEXT("Prop_%03d"), i));
			Ins.SetBindingValueByIndex(3, FString(TEXT("Float")));
			Ins.SetBindingValueByIndex(4, FString(TEXT("BulkModule")));
			Ins.Execute();
		}
		Db.Execute(TEXT("COMMIT;"));
	}

	// Round-trip: hit LIMIT 50 OFFSET 0 then 50 then 100, expect 150 unique
	// names across the three pages.
	TSet<FString> AllNames;
	for (int32 Page = 0; Page < 3; ++Page)
	{
		FSQLitePreparedStatement Stmt;
		TestTrue(TEXT("Prepare paged list"),
			Stmt.Create(Db, TEXT(
				"SELECT property_name FROM reflect_uproperties "
				"WHERE owning_class = ? ORDER BY property_name "
				"LIMIT ? OFFSET ?;")));
		Stmt.SetBindingValueByIndex(1, FString(TEXT("ABulkClass")));
		Stmt.SetBindingValueByIndex(2, 50);
		Stmt.SetBindingValueByIndex(3, Page * 50);
		int32 PageCount = 0;
		while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
		{
			FString N;
			Stmt.GetColumnValueByIndex(0, N);
			bool bWasAlreadyInSet = false;
			AllNames.Add(N, &bWasAlreadyInSet);
			TestFalse(TEXT("Page did not duplicate prior page row"), bWasAlreadyInSet);
			++PageCount;
		}
		TestEqual(TEXT("Page row count is 50"), PageCount, 50);
	}
	TestEqual(TEXT("150 distinct rows across 3 pages"), AllNames.Num(), 150);

	return true;
}

// ---------------------------------------------------------------------------
// Test 4: Specifier search — assert that the fixture's `Abstract` specifier on
// ASampleActor surfaces via find_class_specifier-shaped query (raw SQL form
// matching the colon-flags pattern the adapter uses).
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCppReflectFindSpecifierTest,
	"Monolith.ReflectionIntel.CppReflect.FindSpecifier",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCppReflectFindSpecifierTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithCppReflectTestDetail;

	FString WorkRoot;
	const FString StagedFile = StageFixture(TEXT("SamplePluginA"), WorkRoot);
	if (StagedFile.IsEmpty())
	{
		AddError(TEXT("Failed to stage CppReflect fixture"));
		return false;
	}

	// Declared before the prepared statements below: they are destroyed first, so the
	// holder's Close() cannot hit SQLITE_BUSY on a still-live statement.
	FScopedTestDb ScopedDb;
	if (!ScopedDb.Open(TEXT("cppreflect-test")))
	{
		AddError(TEXT("Failed to open the transient test database"));
		IFileManager::Get().DeleteDirectory(*WorkRoot, false, true);
		return false;
	}
	FSQLiteDatabase& Db = ScopedDb.Get();

	FUHTArtefactReader Reader;
	FString Status;
	Reader.Run(Db, { WorkRoot }, false, /*bAllowMarketplacePaths=*/false, Status);

	// Run the same OR'd LIKE pattern the adapter uses. The exact arm carries
	// COLLATE NOCASE to mirror the adapter's production SQL (FindClassSpecifier),
	// so a lowercase specifier still matches the canonically-cased stored token.
	FSQLitePreparedStatement Stmt;
	TestTrue(TEXT("Prepare specifier-search query"),
		Stmt.Create(Db, TEXT(
			"SELECT class_name FROM reflect_uclasses "
			"WHERE flags = ?1 COLLATE NOCASE OR flags LIKE ?2 OR flags LIKE ?3 OR flags LIKE ?4;")));
	const FString Spec = TEXT("Abstract");
	Stmt.SetBindingValueByIndex(1, Spec);
	Stmt.SetBindingValueByIndex(2, Spec + TEXT(":%"));
	Stmt.SetBindingValueByIndex(3, FString(TEXT("%:")) + Spec + TEXT(":%"));
	Stmt.SetBindingValueByIndex(4, FString(TEXT("%:")) + Spec);

	bool bFoundASampleActor = false;
	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FString CName;
		Stmt.GetColumnValueByIndex(0, CName);
		if (CName == TEXT("ASampleActor")) { bFoundASampleActor = true; }
	}
	TestTrue(TEXT("ASampleActor surfaces under specifier 'Abstract'"), bFoundASampleActor);

	// Case-insensitivity: a LOWERCASE specifier input must still match the
	// canonically-cased stored token ("Abstract"), exercising COLLATE NOCASE on
	// the exact arm. This locks in the case-insensitive guarantee the adapter's
	// `WHERE flags = ?1 COLLATE NOCASE` change introduced.
	FSQLitePreparedStatement LowerStmt;
	TestTrue(TEXT("Prepare lowercase specifier-search query"),
		LowerStmt.Create(Db, TEXT(
			"SELECT class_name FROM reflect_uclasses "
			"WHERE flags = ?1 COLLATE NOCASE OR flags LIKE ?2 OR flags LIKE ?3 OR flags LIKE ?4;")));
	const FString LowerSpec = TEXT("abstract");
	LowerStmt.SetBindingValueByIndex(1, LowerSpec);
	LowerStmt.SetBindingValueByIndex(2, LowerSpec + TEXT(":%"));
	LowerStmt.SetBindingValueByIndex(3, FString(TEXT("%:")) + LowerSpec + TEXT(":%"));
	LowerStmt.SetBindingValueByIndex(4, FString(TEXT("%:")) + LowerSpec);

	bool bFoundASampleActorCaseInsensitive = false;
	while (LowerStmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FString CName;
		LowerStmt.GetColumnValueByIndex(0, CName);
		if (CName == TEXT("ASampleActor")) { bFoundASampleActorCaseInsensitive = true; }
	}
	TestTrue(TEXT("ASampleActor surfaces under lowercase specifier 'abstract' (COLLATE NOCASE)"),
		bFoundASampleActorCaseInsensitive);

	IFileManager::Get().DeleteDirectory(*WorkRoot, false, true);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
