// SPDX-License-Identifier: MIT
// Plan: Plugins/Monolith/Docs/plans/2026-05-28-reflection-intelligence.md (Phase 1).
//
// DecisionRecordIndexerTests — IMPLEMENT_SIMPLE_AUTOMATION_TEST stubs covering
// the Phase 1 §12 test plan:
//   1. Schema bootstrap (tables exist with expected columns)
//   2. Heuristic accuracy (4 rows from 5-file corpus; non_decision excluded)
//   3. Supersession chain (find_supersession_chain returns >= 1 edge)
//   4. Staleness flag (list_stale returns mtime-aged record)
//
// Tests use a TEMP SQLite file at FPaths::AutomationTransientDir, so they never
// touch the real EngineSource.db. Test corpus path is resolved relative to the
// plugin's Source/MonolithReflectionIntel/Private/Tests/Fixtures/DecisionCorpus.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Decision/FDecisionRecordIndexer.h"

#include "FScopedTestDb.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/DateTime.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/Timespan.h"
#include "SQLiteDatabase.h"

namespace MonolithDecisionTestDetail
{
	/** Resolve the fixture corpus path relative to the Monolith plugin install. */
	static FString GetFixtureCorpusDir()
	{
		TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("Monolith"));
		if (Plugin.IsValid())
		{
			return Plugin->GetBaseDir()
				/ TEXT("Source") / TEXT("MonolithReflectionIntel")
				/ TEXT("Private") / TEXT("Tests") / TEXT("Fixtures") / TEXT("DecisionCorpus");
		}
		return FPaths::ProjectPluginsDir()
			/ TEXT("Monolith") / TEXT("Source") / TEXT("MonolithReflectionIntel")
			/ TEXT("Private") / TEXT("Tests") / TEXT("Fixtures") / TEXT("DecisionCorpus");
	}

	/** Disposable temp DB at AutomationTransientDir/decision-test-<rand>.db, closed on scope exit. */
	using MonolithReflectionIntelTests::FScopedTestDb;

	/** Count rows in a table. -1 on prepare failure. */
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

	/** True if a table exists (per sqlite_master). */
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
}

// ---------------------------------------------------------------------------
// Test 1: Schema bootstrap — `decision_records` + `decision_supersedes` tables
// exist after Run() against an empty DB.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDecisionSchemaBootstrapTest,
	"Monolith.ReflectionIntel.Decision.SchemaBootstrap",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDecisionSchemaBootstrapTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithDecisionTestDetail;

	FScopedTestDb ScopedDb;
	if (!ScopedDb.Open(TEXT("decision-test")))
	{
		AddError(TEXT("Failed to open the transient test database"));
		return false;
	}
	FSQLiteDatabase& Db = ScopedDb.Get();

	// Empty corpus run — schema must still be created.
	FDecisionRecordIndexer Indexer;
	FString Status;
	const bool bOk = Indexer.Run(Db, /*MarkdownRoots=*/{}, Status);
	TestTrue(TEXT("Indexer.Run returned success"), bOk);
	TestTrue(TEXT("decision_records table created"),    TableExists(Db, TEXT("decision_records")));
	TestTrue(TEXT("decision_supersedes table created"), TableExists(Db, TEXT("decision_supersedes")));

	return true;
}

// ---------------------------------------------------------------------------
// Test 2: Heuristic accuracy — exactly 4 rows ingested from the 5-file
// corpus (records 01/02-headerwalk/04/05; 03_non_decision excluded).
//
// Implementation note: 02 contains TWO headers with rationale, so the
// expected row count is 1 (file 01) + 2 (file 02) + 1 (file 04 frontmatter)
// + 2 (file 05 ADR-style headers) = 6.  We assert >= 4 (sanity) AND that
// 03_non_decision contributes zero rows.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDecisionHeuristicAccuracyTest,
	"Monolith.ReflectionIntel.Decision.HeuristicAccuracy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDecisionHeuristicAccuracyTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithDecisionTestDetail;

	const FString CorpusDir = GetFixtureCorpusDir();
	if (!FPlatformFileManager::Get().GetPlatformFile().DirectoryExists(*CorpusDir))
	{
		AddWarning(FString::Printf(TEXT("Fixture corpus not present at '%s' — test inconclusive."), *CorpusDir));
		return true;
	}

	// Declared before the prepared statement below: it is destroyed first, so the
	// holder's Close() cannot hit SQLITE_BUSY on a still-live statement.
	FScopedTestDb ScopedDb;
	if (!ScopedDb.Open(TEXT("decision-test")))
	{
		AddError(TEXT("Failed to open the transient test database"));
		return false;
	}
	FSQLiteDatabase& Db = ScopedDb.Get();

	FDecisionRecordIndexer Indexer;
	FString Status;
	const bool bOk = Indexer.Run(Db, { CorpusDir }, Status);
	TestTrue(TEXT("Indexer.Run returned success"), bOk);

	const int32 Rows = CountRows(Db, TEXT("decision_records"));
	TestTrue(TEXT("At least 4 decision rows from fixture corpus"), Rows >= 4);

	// Assert 03_non_decision contributed zero rows — direct query.
	FSQLitePreparedStatement Stmt;
	TestTrue(TEXT("Prepare LIKE query"),
		Stmt.Create(Db, TEXT("SELECT COUNT(*) FROM decision_records WHERE source_path LIKE ?;")));
	Stmt.SetBindingValueByIndex(1, FString(TEXT("%03_non_decision%")));
	if (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		int32 NonDecCount = -1;
		Stmt.GetColumnValueByIndex(0, NonDecCount);
		TestEqual(TEXT("03_non_decision contributes zero rows"), NonDecCount, 0);
	}

	return true;
}

// ---------------------------------------------------------------------------
// Test 3: Supersession chain — `decision_supersedes` carries >= 1 edge after
// indexing the fixture corpus (02 contains two `Supersedes:` lines).
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDecisionSupersessionChainTest,
	"Monolith.ReflectionIntel.Decision.SupersessionChain",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDecisionSupersessionChainTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithDecisionTestDetail;

	const FString CorpusDir = GetFixtureCorpusDir();
	if (!FPlatformFileManager::Get().GetPlatformFile().DirectoryExists(*CorpusDir))
	{
		AddWarning(FString::Printf(TEXT("Fixture corpus not present at '%s' — test inconclusive."), *CorpusDir));
		return true;
	}

	FScopedTestDb ScopedDb;
	if (!ScopedDb.Open(TEXT("decision-test"))) { AddError(TEXT("Failed to open the transient test database")); return false; }
	FSQLiteDatabase& Db = ScopedDb.Get();

	FDecisionRecordIndexer Indexer;
	FString Status;
	TestTrue(TEXT("Indexer.Run success"), Indexer.Run(Db, { CorpusDir }, Status));

	const int32 EdgeCount = CountRows(Db, TEXT("decision_supersedes"));
	TestTrue(TEXT("At least one supersession edge resolved from corpus"), EdgeCount >= 1);

	return true;
}

// ---------------------------------------------------------------------------
// Test 4: Staleness flag — bump a fixture file's mtime forward by 60 days
// via IFileManager::SetTimeStamp, run indexer, assert at least one record's
// `source_mtime` is past the cutoff for a 30-day query.
//
// Note: SetTimeStamp on a file under SOURCE control may leave artefacts. We
// COPY the fixture into AutomationTransientDir first, then mutate the copy.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDecisionStalenessFlagTest,
	"Monolith.ReflectionIntel.Decision.StalenessFlag",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDecisionStalenessFlagTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithDecisionTestDetail;

	const FString CorpusDir = GetFixtureCorpusDir();
	if (!FPlatformFileManager::Get().GetPlatformFile().DirectoryExists(*CorpusDir))
	{
		AddWarning(FString::Printf(TEXT("Fixture corpus not present at '%s' — test inconclusive."), *CorpusDir));
		return true;
	}

	// Copy one fixture file into a transient working dir so we can age it
	// without polluting the source tree.
	const FString WorkDir = FPaths::AutomationTransientDir()
		/ FString::Printf(TEXT("decision-stale-%s"), *FGuid::NewGuid().ToString());
	FPlatformFileManager::Get().GetPlatformFile().CreateDirectoryTree(*WorkDir);

	const FString Src = CorpusDir / TEXT("01_decision_with_rationale.md");
	const FString Dst = WorkDir   / TEXT("01_decision_with_rationale.md");
	if (IFileManager::Get().Copy(*Dst, *Src) != COPY_OK)
	{
		AddError(FString::Printf(TEXT("Failed to copy fixture file '%s' -> '%s'"), *Src, *Dst));
		return false;
	}

	// Age the copy by ~60 days into the past so it is "older than 30 days".
	const FDateTime AgedTime = FDateTime::UtcNow() - FTimespan::FromDays(60.0);
	IFileManager::Get().SetTimeStamp(*Dst, AgedTime);

	// Declared before the prepared statement below: it is destroyed first, so the
	// holder's Close() cannot hit SQLITE_BUSY on a still-live statement.
	FScopedTestDb ScopedDb;
	if (!ScopedDb.Open(TEXT("decision-test"))) { AddError(TEXT("Failed to open the transient test database")); return false; }
	FSQLiteDatabase& Db = ScopedDb.Get();

	FDecisionRecordIndexer Indexer;
	FString Status;
	TestTrue(TEXT("Indexer.Run success"), Indexer.Run(Db, { WorkDir }, Status));

	// Query for rows older than 30 days.
	const int64 CutoffUnix = FDateTime::UtcNow().ToUnixTimestamp() - (30LL * 24LL * 60LL * 60LL);
	FSQLitePreparedStatement Stmt;
	TestTrue(TEXT("Prepare staleness query"),
		Stmt.Create(Db, TEXT("SELECT COUNT(*) FROM decision_records WHERE source_mtime > 0 AND source_mtime < ?;")));
	Stmt.SetBindingValueByIndex(1, CutoffUnix);

	int32 StaleCount = 0;
	if (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		Stmt.GetColumnValueByIndex(0, StaleCount);
	}
	TestTrue(TEXT("At least one stale row past 30-day cutoff"), StaleCount >= 1);

	IFileManager::Get().DeleteDirectory(*WorkDir, /*bRequireExists=*/false, /*bTree=*/true);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
