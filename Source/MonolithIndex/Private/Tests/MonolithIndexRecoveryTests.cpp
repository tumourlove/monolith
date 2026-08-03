// SPDX-License-Identifier: MIT
// Automation tests for full-index interruption recovery (issue #117).
// Plan: Plugins/Monolith/Docs/plans/2026-08-01-pr-issue-sweep.md (Phase 6, T29)
//
// Goals:
//   - The v3 lifecycle markers survive a close/reopen, and completing an index
//     clears the in-progress state. This is what stops a crash from turning into
//     a wipe-and-rebuild on the next launch.
//   - A checkpoint and the child rows it vouches for are ATOMIC. If a rollback
//     could keep the checkpoint, a resume would trust data that is not there.
//   - The v2 -> v3 migration adds the two columns WITHOUT destroying existing
//     rows. This is the case that can silently cost a user their whole index.
//   - Exact-asset quarantine: a marker committed immediately before one package
//     load survives a rolled-back work transaction, while a healthy neighbour is
//     never marked. A counter written inside the work transaction would roll back,
//     read 0 on resume, and repeat the crash forever; the rollback assertion closes
//     that hole. The same fixture verifies targeted retry and marker migration.
//
// These are pure database tests against their own temp SQLite file: no project
// index, no editor subsystem, no indexing state.

#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "HAL/PlatformFileManager.h"
#include "SQLiteDatabase.h"
#include "MonolithIndexDatabase.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace MonolithIndexRecoveryTestDetail
{
	static const TCHAR* AssetPath = TEXT("/Game/Tests/Monolith/MonolithRecoveryFixtureAsset");
	static const TCHAR* SecondAssetPath = TEXT("/Game/Tests/Monolith/MonolithRecoveryFixtureAssetB");
	static const TCHAR* ContentHash = TEXT("0123456789abcdef0123456789abcdef01234567");

	/** Temp index database that can be closed and reopened in place. */
	struct FFixture
	{
		FMonolithIndexDatabase Database;
		FString DbPath;

		bool Open()
		{
			DbPath = FPaths::Combine(
				FPaths::ProjectSavedDir(),
				TEXT("MonolithTests"),
				FString::Printf(TEXT("IndexRecovery_%s.db"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));

			return Database.Open(DbPath);
		}

		/** Close and reopen the same file, so on-disk state is what is asserted. */
		bool Reopen()
		{
			Database.Close();
			return Database.Open(DbPath);
		}

		int64 InsertAsset(const TCHAR* PackagePath, const TCHAR* SavedHash)
		{
			FIndexedAsset Asset;
			Asset.PackagePath = PackagePath;
			Asset.AssetName = TEXT("MonolithRecoveryFixtureAsset");
			Asset.AssetClass = TEXT("Blueprint");
			Asset.ModuleName = TEXT("Game");
			Asset.Description = TEXT("index recovery fixture");
			Asset.SavedHash = SavedHash;
			return Database.InsertAsset(Asset);
		}

		void Destroy()
		{
			Database.Close();
			if (!DbPath.IsEmpty())
			{
				FPlatformFileManager::Get().GetPlatformFile().DeleteFile(*DbPath);
			}
		}
	};

	/** Count rows in a table, for the child-data assertions. */
	static int64 CountRows(FMonolithIndexDatabase& Database, const TCHAR* Table)
	{
		FSQLiteDatabase* Raw = Database.GetRawDatabase();
		if (!Raw)
		{
			return -1;
		}

		FSQLitePreparedStatement Stmt;
		Stmt.Create(*Raw, *FString::Printf(TEXT("SELECT COUNT(*) FROM %s;"), Table));
		if (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
		{
			int64 Count = 0;
			Stmt.GetColumnValueByIndex(0, Count);
			return Count;
		}
		return -1;
	}

	/** True when `assets` carries the named column. */
	static bool HasAssetColumn(FMonolithIndexDatabase& Database, const TCHAR* ColumnName)
	{
		FSQLiteDatabase* Raw = Database.GetRawDatabase();
		if (!Raw)
		{
			return false;
		}

		FSQLitePreparedStatement Stmt;
		if (!Stmt.Create(*Raw, TEXT("PRAGMA table_info(assets);")))
		{
			return false;
		}

		while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
		{
			FString ColName;
			Stmt.GetColumnValueByIndex(1, ColName);
			if (ColName == ColumnName)
			{
				return true;
			}
		}
		return false;
	}
}

// ---------------------------------------------------------------------------
// Test 1: the full-index lifecycle markers, including across a reopen.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithIndexRecoveryLifecycleTest,
	"Monolith.Index.Recovery.Lifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithIndexRecoveryLifecycleTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithIndexRecoveryTestDetail;

	FFixture Fixture;
	if (!TestTrue(TEXT("fixture database opens"), Fixture.Open()))
	{
		Fixture.Destroy();
		return false;
	}
	ON_SCOPE_EXIT { Fixture.Destroy(); };

	TestEqual(TEXT("a fresh database is schema v3"), Fixture.Database.ReadMeta(TEXT("schema_version")), FString(TEXT("3")));
	TestTrue(TEXT("a fresh database supports resume"), Fixture.Database.SupportsIndexResume());
	TestFalse(TEXT("no index is in progress on a fresh database"), Fixture.Database.IsFullIndexInProgress());

	TestTrue(TEXT("BeginFullIndex succeeds"), Fixture.Database.BeginFullIndex());
	TestTrue(TEXT("the index reads as in progress"), Fixture.Database.IsFullIndexInProgress());
	TestTrue(TEXT("BeginFullIndex clears any previous completion stamp"),
		Fixture.Database.ReadMeta(TEXT("last_full_index")).IsEmpty());

	// The whole point: the marker is on disk, not in memory. This is the state a
	// crashed editor leaves behind.
	if (!TestTrue(TEXT("database reopens"), Fixture.Reopen()))
	{
		return false;
	}
	TestTrue(TEXT("the in-progress marker survives a close/reopen"), Fixture.Database.IsFullIndexInProgress());

	TestTrue(TEXT("CompleteFullIndex succeeds"), Fixture.Database.CompleteFullIndex(TEXT("2026.08.01-00.00.00")));
	TestFalse(TEXT("completion clears the in-progress marker"), Fixture.Database.IsFullIndexInProgress());
	TestEqual(TEXT("completion writes the timestamp"),
		Fixture.Database.ReadMeta(TEXT("last_full_index")), FString(TEXT("2026.08.01-00.00.00")));

	// An explicit force-reindex still wipes: that behaviour is deliberate and is
	// the documented recovery from a skipped asset.
	TestTrue(TEXT("BeginFullIndex re-arms the marker"), Fixture.Database.BeginFullIndex());
	TestTrue(TEXT("ResetDatabase succeeds"), Fixture.Database.ResetDatabase());
	TestFalse(TEXT("a reset clears the in-progress marker"), Fixture.Database.IsFullIndexInProgress());
	TestEqual(TEXT("a reset restates the schema version"),
		Fixture.Database.ReadMeta(TEXT("schema_version")), FString(TEXT("3")));

	return true;
}

// ---------------------------------------------------------------------------
// Test 2: a checkpoint and the child rows it vouches for are atomic.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithIndexRecoveryCheckpointAtomicityTest,
	"Monolith.Index.Recovery.CheckpointAtomicity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithIndexRecoveryCheckpointAtomicityTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithIndexRecoveryTestDetail;

	FFixture Fixture;
	if (!TestTrue(TEXT("fixture database opens"), Fixture.Open()))
	{
		Fixture.Destroy();
		return false;
	}
	ON_SCOPE_EXIT { Fixture.Destroy(); };

	const int64 AssetId = Fixture.InsertAsset(AssetPath, ContentHash);
	if (!TestTrue(TEXT("fixture asset inserts"), AssetId > 0))
	{
		return false;
	}

	// Exactly what a deep-index batch does: child rows plus the checkpoint that
	// says those rows exist, in one transaction.
	TestTrue(TEXT("work transaction begins"), Fixture.Database.BeginTransaction());

	FIndexedNode Node;
	Node.AssetId = AssetId;
	Node.NodeName = TEXT("MonolithRecoveryFixtureNode");
	Node.NodeClass = TEXT("K2Node_IfThenElse");
	Node.NodeType = TEXT("flow");
	TestTrue(TEXT("child row inserts"), Fixture.Database.InsertNode(Node) > 0);
	TestTrue(TEXT("checkpoint writes"), Fixture.Database.SetDeepIndexedHash(AssetId, ContentHash));

	// The crash.
	TestTrue(TEXT("work transaction rolls back"), Fixture.Database.RollbackTransaction());

	TestEqual(TEXT("the child rows are gone"), CountRows(Fixture.Database, TEXT("nodes")), (int64)0);

	TOptional<FIndexedAsset> Reloaded = Fixture.Database.GetAssetByPath(AssetPath);
	if (!TestTrue(TEXT("the asset row itself survives"), Reloaded.IsSet()))
	{
		return false;
	}
	// A checkpoint that outlived its child rows would tell the resume to skip an
	// asset whose data was discarded -- silent, permanent data loss.
	TestTrue(TEXT("the checkpoint is gone with them"), Reloaded->DeepIndexedHash.IsEmpty());

	return true;
}

// ---------------------------------------------------------------------------
// Test 3: the v2 -> v3 migration is additive and non-destructive.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithIndexRecoverySchemaMigrationTest,
	"Monolith.Index.Recovery.SchemaMigrationV2ToV3",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithIndexRecoverySchemaMigrationTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithIndexRecoveryTestDetail;

	FFixture Fixture;
	if (!TestTrue(TEXT("fixture database opens"), Fixture.Open()))
	{
		Fixture.Destroy();
		return false;
	}
	ON_SCOPE_EXIT { Fixture.Destroy(); };

	const int64 AssetId = Fixture.InsertAsset(AssetPath, ContentHash);
	if (!TestTrue(TEXT("fixture asset inserts"), AssetId > 0))
	{
		return false;
	}

	FIndexedNode Node;
	Node.AssetId = AssetId;
	Node.NodeName = TEXT("MonolithRecoveryFixtureNode");
	Node.NodeClass = TEXT("K2Node_IfThenElse");
	Node.NodeType = TEXT("flow");
	TestTrue(TEXT("fixture child row inserts"), Fixture.Database.InsertNode(Node) > 0);

	// Rewind the schema to what v0.21.3 shipped: no resume columns, version 2.
	FSQLiteDatabase* Raw = Fixture.Database.GetRawDatabase();
	if (!TestNotNull(TEXT("raw database is available"), Raw))
	{
		return false;
	}
	if (!TestTrue(TEXT("deep_indexed_hash can be dropped"),
		Raw->Execute(TEXT("ALTER TABLE assets DROP COLUMN deep_indexed_hash;"))))
	{
		return false;
	}
	if (!TestTrue(TEXT("deep_index_attempts can be dropped"),
		Raw->Execute(TEXT("ALTER TABLE assets DROP COLUMN deep_index_attempts;"))))
	{
		return false;
	}
	TestTrue(TEXT("schema version rewinds to 2"), Fixture.Database.WriteMeta(TEXT("schema_version"), TEXT("2")));
	TestFalse(TEXT("a v2 database does not claim resume support"), Fixture.Database.SupportsIndexResume());

	// Reopen: this is the upgrade a user gets by launching the new build.
	if (!TestTrue(TEXT("database reopens"), Fixture.Reopen()))
	{
		return false;
	}

	TestTrue(TEXT("deep_indexed_hash is added"), HasAssetColumn(Fixture.Database, TEXT("deep_indexed_hash")));
	TestTrue(TEXT("deep_index_attempts is added"), HasAssetColumn(Fixture.Database, TEXT("deep_index_attempts")));
	TestEqual(TEXT("the schema version is stamped 3"),
		Fixture.Database.ReadMeta(TEXT("schema_version")), FString(TEXT("3")));
	TestTrue(TEXT("resume is available after migration"), Fixture.Database.SupportsIndexResume());

	// The assertion that matters: the migration must not cost the user the index
	// they already have.
	TOptional<FIndexedAsset> Reloaded = Fixture.Database.GetAssetByPath(AssetPath);
	if (!TestTrue(TEXT("the pre-existing asset row survives"), Reloaded.IsSet()))
	{
		return false;
	}
	TestEqual(TEXT("its id is unchanged"), Reloaded->Id, AssetId);
	TestEqual(TEXT("its class is intact"), Reloaded->AssetClass, FString(TEXT("Blueprint")));
	TestEqual(TEXT("its saved hash is intact"), Reloaded->SavedHash, FString(ContentHash));
	TestEqual(TEXT("its new checkpoint column defaults to empty"), Reloaded->DeepIndexedHash, FString());
	TestEqual(TEXT("its new attempt column defaults to zero"), Reloaded->DeepIndexAttempts, 0);
	TestEqual(TEXT("its child rows survive"), CountRows(Fixture.Database, TEXT("nodes")), (int64)1);

	// And the migrated columns are usable, not just present.
	TestTrue(TEXT("the migrated checkpoint column accepts a write"),
		Fixture.Database.SetDeepIndexedHash(AssetId, ContentHash));
	Reloaded = Fixture.Database.GetAssetByPath(AssetPath);
	if (!TestTrue(TEXT("the asset still reads back after the write"), Reloaded.IsSet()))
	{
		return false;
	}
	TestEqual(TEXT("the checkpoint reads back"), Reloaded->DeepIndexedHash, FString(ContentHash));

	return true;
}

// ---------------------------------------------------------------------------
// Test 4: exact-asset quarantine, retry, and the durability it rests on.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithExactAssetQuarantineTest,
	"Monolith.Index.Recovery.ExactAssetQuarantineAfterOneCrash",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithExactAssetQuarantineTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithIndexRecoveryTestDetail;

	FFixture Fixture;
	if (!TestTrue(TEXT("fixture database opens"), Fixture.Open()))
	{
		Fixture.Destroy();
		return false;
	}
	ON_SCOPE_EXIT { Fixture.Destroy(); };

	const int64 PoisonId = Fixture.InsertAsset(AssetPath, ContentHash);
	const int64 HealthyId = Fixture.InsertAsset(SecondAssetPath, ContentHash);
	if (!TestTrue(TEXT("fixture assets insert"), PoisonId > 0 && HealthyId > 0))
	{
		return false;
	}

	// Only the asset immediately about to load is marked. A process death after
	// this commit therefore implicates this package, not its healthy neighbour.
	TestTrue(TEXT("exact asset attempt records"), Fixture.Database.BumpDeepIndexAttempts(PoisonId));
	TOptional<FIndexedAsset> AfterOne = Fixture.Database.GetAssetByPath(AssetPath);
	if (!TestTrue(TEXT("asset reads back after one attempt"), AfterOne.IsSet()))
	{
		return false;
	}
	TestEqual(TEXT("one attempt is recorded"), AfterOne->DeepIndexAttempts, 1);
	TestEqual(TEXT("one interrupted exact-asset attempt quarantines immediately"),
		MonolithDecideDeepIndexQueueEntry(AfterOne->DeepIndexedHash, AfterOne->DeepIndexAttempts, ContentHash),
		EMonolithDeepIndexQueueDecision::SkipPoisonAsset);

	// The healthy asset was never marked and remains eligible.
	TOptional<FIndexedAsset> Healthy = Fixture.Database.GetAssetByPath(SecondAssetPath);
	if (!TestTrue(TEXT("healthy asset reads back"), Healthy.IsSet()))
	{
		return false;
	}
	TestEqual(TEXT("healthy neighbour has no interrupted attempt"), Healthy->DeepIndexAttempts, 0);
	TestEqual(TEXT("the healthy asset is still queued"),
		MonolithDecideDeepIndexQueueEntry(Healthy->DeepIndexedHash, Healthy->DeepIndexAttempts, ContentHash),
		EMonolithDeepIndexQueueDecision::Queue);

	// Skipping stamps the current hash so the asset leaves the queue for good
	// rather than being re-evaluated (and re-logged) on every later resume.
	TestTrue(TEXT("skipping stamps the current hash"), Fixture.Database.SetDeepIndexedHash(PoisonId, ContentHash));
	TOptional<FIndexedAsset> AfterSkip = Fixture.Database.GetAssetByPath(AssetPath);
	if (!TestTrue(TEXT("asset reads back after the skip"), AfterSkip.IsSet()))
	{
		return false;
	}
	TestEqual(TEXT("the skip is recorded on the row"), AfterSkip->DeepIndexedHash, FString(ContentHash));
	TestEqual(TEXT("the skipped asset stays out of the queue"),
		MonolithDecideDeepIndexQueueEntry(AfterSkip->DeepIndexedHash, AfterSkip->DeepIndexAttempts, ContentHash),
		EMonolithDeepIndexQueueDecision::SkipAlreadyIndexed);

	// The counter is left at its limit rather than cleared, so the asset is held
	// out on BOTH gates. An asset whose Asset Registry hash is empty would stamp
	// an empty checkpoint, fail the hash comparison next run, and start crashing
	// again if the counter were the only thing keeping it out.
	TestEqual(TEXT("the counter still holds it out when the hash stamp cannot"),
		MonolithDecideDeepIndexQueueEntry(FString(), AfterSkip->DeepIndexAttempts, FString()),
		EMonolithDeepIndexQueueDecision::SkipPoisonAsset);
	TestEqual(TEXT("even a content change does not revive it while the counter stands"),
		MonolithDecideDeepIndexQueueEntry(AfterSkip->DeepIndexedHash, AfterSkip->DeepIndexAttempts, TEXT("ffffffffffffffffffffffffffffffffffffffff")),
		EMonolithDeepIndexQueueDecision::SkipPoisonAsset);

	// Persist the team-review list, then exercise the settings-button backend.
	TestTrue(TEXT("quarantine list records"), Fixture.Database.RecordSkippedAssetPaths({ AssetPath }));
	TArray<FString> ReleasedPaths;
	if (!TestTrue(TEXT("quarantine retry prepares"), Fixture.Database.PrepareQuarantinedAssetRetry(ReleasedPaths)))
	{
		return false;
	}
	if (!TestEqual(TEXT("retry releases exactly one asset"), ReleasedPaths.Num(), 1))
	{
		return false;
	}
	TestEqual(TEXT("retry releases the quarantined path"), ReleasedPaths[0], FString(AssetPath));
	TestEqual(TEXT("review list is cleared for the retry"), Fixture.Database.GetSkippedAssetPaths().Num(), 0);
	TestTrue(TEXT("retry arms resumable indexing"), Fixture.Database.IsFullIndexInProgress());

	TOptional<FIndexedAsset> AfterClear = Fixture.Database.GetAssetByPath(AssetPath);
	if (!TestTrue(TEXT("asset reads back after release"), AfterClear.IsSet()))
	{
		return false;
	}
	TestEqual(TEXT("the counter is back to zero"), AfterClear->DeepIndexAttempts, 0);
	TestTrue(TEXT("the quarantine checkpoint is cleared"), AfterClear->DeepIndexedHash.IsEmpty());
	TestEqual(TEXT("a cleared, changed asset is queued again"),
		MonolithDecideDeepIndexQueueEntry(AfterClear->DeepIndexedHash, AfterClear->DeepIndexAttempts, TEXT("ffffffffffffffffffffffffffffffffffffffff")),
		EMonolithDeepIndexQueueDecision::Queue);

	// Databases created by the former batch-wide marker cannot attribute their
	// counters safely. Opening under marker format v2 clears those ambiguous rows.
	TestTrue(TEXT("legacy marker format can be staged"),
		Fixture.Database.WriteMeta(TEXT("deep_index_attempt_marker_version"), TEXT("1")));
	TestTrue(TEXT("legacy skipped checkpoint stages"), Fixture.Database.SetDeepIndexedHash(HealthyId, ContentHash));
	TestTrue(TEXT("legacy batch counter stages"), Fixture.Database.BumpDeepIndexAttempts(HealthyId));
	TestTrue(TEXT("legacy review path stages"), Fixture.Database.RecordSkippedAssetPaths({ SecondAssetPath }));
	if (!TestTrue(TEXT("database reopens for marker migration"), Fixture.Reopen()))
	{
		return false;
	}
	Healthy = Fixture.Database.GetAssetByPath(SecondAssetPath);
	if (!TestTrue(TEXT("healthy asset survives marker migration"), Healthy.IsSet()))
	{
		return false;
	}
	TestEqual(TEXT("legacy batch counter is cleared"), Healthy->DeepIndexAttempts, 0);
	TestTrue(TEXT("legacy ambiguous checkpoint is cleared"), Healthy->DeepIndexedHash.IsEmpty());
	TestEqual(TEXT("legacy batch review list is cleared"), Fixture.Database.GetSkippedAssetPaths().Num(), 0);
	TestEqual(TEXT("exact marker format is stamped"),
		Fixture.Database.ReadMeta(TEXT("deep_index_attempt_marker_version")), FString(TEXT("2")));

	// --- Part 2: durability. This half is the mechanism. ------------------
	//
	// The marker only works because it is committed BEFORE the work transaction
	// opens. Written inside that transaction it would be rolled back with it on
	// the next open, the counter would read 0 on resume, the same batch would
	// re-queue, and the editor would crash again -- forever. A skip test alone
	// has no transaction boundary and passes under that broken design, so this
	// sequence is what actually locks the fix in.
	const int64 DurabilityId = Fixture.InsertAsset(TEXT("/Game/Tests/Monolith/MonolithRecoveryDurability"), ContentHash);
	if (!TestTrue(TEXT("durability fixture asset inserts"), DurabilityId > 0))
	{
		return false;
	}

	// The attempt marker: its own transaction, committed.
	TestTrue(TEXT("marker transaction begins"), Fixture.Database.BeginTransaction());
	TestTrue(TEXT("marker writes"), Fixture.Database.BumpDeepIndexAttempts(DurabilityId));
	TestTrue(TEXT("marker transaction commits"), Fixture.Database.CommitTransaction());

	// The batch work, which the crash discards.
	TestTrue(TEXT("work transaction begins"), Fixture.Database.BeginTransaction());
	FIndexedNode WorkNode;
	WorkNode.AssetId = DurabilityId;
	WorkNode.NodeName = TEXT("MonolithRecoveryDurabilityNode");
	WorkNode.NodeClass = TEXT("K2Node_IfThenElse");
	WorkNode.NodeType = TEXT("flow");
	TestTrue(TEXT("work row inserts"), Fixture.Database.InsertNode(WorkNode) > 0);
	TestTrue(TEXT("work transaction rolls back"), Fixture.Database.RollbackTransaction());

	TOptional<FIndexedAsset> AfterRollback = Fixture.Database.GetAssetByPath(TEXT("/Game/Tests/Monolith/MonolithRecoveryDurability"));
	if (!TestTrue(TEXT("durability asset reads back"), AfterRollback.IsSet()))
	{
		return false;
	}
	TestEqual(TEXT("the attempt counter SURVIVES the rolled-back work transaction"),
		AfterRollback->DeepIndexAttempts, 1);

	// And it survives the process restart the rollback actually happens on.
	if (!TestTrue(TEXT("database reopens"), Fixture.Reopen()))
	{
		return false;
	}
	AfterRollback = Fixture.Database.GetAssetByPath(TEXT("/Game/Tests/Monolith/MonolithRecoveryDurability"));
	if (!TestTrue(TEXT("durability asset reads back after reopen"), AfterRollback.IsSet()))
	{
		return false;
	}
	TestEqual(TEXT("the attempt counter survives a close/reopen"), AfterRollback->DeepIndexAttempts, 1);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
