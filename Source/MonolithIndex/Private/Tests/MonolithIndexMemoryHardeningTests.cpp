// SPDX-License-Identifier: MIT

#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "HAL/PlatformFileManager.h"
#include "SQLiteDatabase.h"
#include "MonolithIndexDatabase.h"
#include "MonolithMemoryHelper.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithMemoryPressureClassificationTest,
	"Monolith.Index.Memory.PressureClassification",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMemoryPressureClassificationTest::RunTest(const FString& /*Parameters*/)
{
	FMonolithMemorySnapshot Snapshot;
	Snapshot.TotalPhysicalMB = 32768;
	Snapshot.AvailablePhysicalMB = 8192;
	Snapshot.ProcessUsedPhysicalMB = 8000;

	TestEqual(TEXT("healthy memory has no pressure"),
		FMonolithMemoryHelper::ClassifyMemoryPressure(Snapshot, 16384),
		EMonolithMemoryPressure::None);

	Snapshot.ProcessUsedPhysicalMB = 17000;
	TestEqual(TEXT("process budget is a soft throttle"),
		FMonolithMemoryHelper::ClassifyMemoryPressure(Snapshot, 16384),
		EMonolithMemoryPressure::Soft);

	Snapshot.ProcessUsedPhysicalMB = 8000;
	Snapshot.AvailablePhysicalMB = 1500;
	TestEqual(TEXT("low system headroom is critical"),
		FMonolithMemoryHelper::ClassifyMemoryPressure(Snapshot, 16384),
		EMonolithMemoryPressure::Critical);

	Snapshot.AvailablePhysicalMB = 8192;
	Snapshot.bHasGPUStats = true;
	Snapshot.GPUBudgetMB = 8192;
	Snapshot.GPUUsedMB = 7000;
	TestEqual(TEXT("high GPU usage is a soft throttle while headroom remains"),
		FMonolithMemoryHelper::ClassifyMemoryPressure(Snapshot, 16384),
		EMonolithMemoryPressure::Soft);

	Snapshot.GPUUsedMB = 7600;
	TestEqual(TEXT("low GPU headroom is critical"),
		FMonolithMemoryHelper::ClassifyMemoryPressure(Snapshot, 16384),
		EMonolithMemoryPressure::Critical);

	Snapshot.bHasGPUStats = false;
	Snapshot.GPUUsedMB = 8192;
	TestEqual(TEXT("unavailable GPU telemetry is ignored"),
		FMonolithMemoryHelper::ClassifyMemoryPressure(Snapshot, 16384),
		EMonolithMemoryPressure::None);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithMemoryTierRoundingTest,
	"Monolith.Index.Memory.RamTierRounding",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMemoryTierRoundingTest::RunTest(const FString& /*Parameters*/)
{
	constexpr uint64 GiB = 1024ULL * 1024ULL * 1024ULL;
	TestEqual(TEXT("a hardware-reported 31.817 GiB enters the 32 GB tier"),
		FMonolithMemoryHelper::RoundPhysicalBytesToRamGB(34163589120ULL), 32);
	TestEqual(TEXT("15.75 GiB enters the 16 GB tier"),
		FMonolithMemoryHelper::RoundPhysicalBytesToRamGB(15ULL * GiB + 3ULL * GiB / 4ULL), 16);
	TestEqual(TEXT("15.25 GiB remains below the 16 GB tier"),
		FMonolithMemoryHelper::RoundPhysicalBytesToRamGB(15ULL * GiB + GiB / 4ULL), 15);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithLevelRowsReplaceTest,
	"Monolith.Index.Memory.LevelRowsReplaceInsteadOfAppend",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithLevelRowsReplaceTest::RunTest(const FString& /*Parameters*/)
{
	const FString DbPath = FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("MonolithTests"),
		FString::Printf(TEXT("LevelRows_%s.db"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));

	FMonolithIndexDatabase Database;
	if (!TestTrue(TEXT("fixture database opens"), Database.Open(DbPath)))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		Database.Close();
		FPlatformFileManager::Get().GetPlatformFile().DeleteFile(*DbPath);
	};

	FIndexedAsset Asset;
	Asset.PackagePath = TEXT("/Game/Tests/Monolith/LevelFixture");
	Asset.AssetName = TEXT("LevelFixture");
	Asset.AssetClass = TEXT("World");
	Asset.ModuleName = TEXT("Game");
	const int64 AssetId = Database.InsertAsset(Asset);
	if (!TestTrue(TEXT("fixture world inserts"), AssetId > 0))
	{
		return false;
	}

	FIndexedAsset OtherAsset = Asset;
	OtherAsset.PackagePath = TEXT("/Game/Tests/Monolith/OtherLevelFixture");
	OtherAsset.AssetName = TEXT("OtherLevelFixture");
	const int64 OtherAssetId = Database.InsertAsset(OtherAsset);
	if (!TestTrue(TEXT("second fixture world inserts"), OtherAssetId > 0))
	{
		return false;
	}

	FIndexedActor Actor;
	Actor.AssetId = AssetId;
	Actor.ActorName = TEXT("FixtureActor");
	Actor.ActorClass = TEXT("Actor");
	TestTrue(TEXT("fixture actor inserts"), Database.InsertActor(Actor) > 0);
	Actor.AssetId = OtherAssetId;
	Actor.ActorName = TEXT("UnprocessedFixtureActor");
	TestTrue(TEXT("unprocessed fixture actor inserts"), Database.InsertActor(Actor) > 0);
	TestTrue(TEXT("processed level rows clear before a resumed pass"), Database.ClearActorsForAsset(AssetId));

	FSQLitePreparedStatement CountStmt;
	if (!TestTrue(TEXT("actor count statement prepares"),
		CountStmt.Create(*Database.GetRawDatabase(), TEXT("SELECT COUNT(*) FROM actors;"))))
	{
		return false;
	}
	if (!TestEqual(TEXT("actor count query returns a row"), CountStmt.Step(), ESQLitePreparedStatementStepResult::Row))
	{
		return false;
	}
	int64 ActorCount = -1;
	CountStmt.GetColumnValueByIndex(0, ActorCount);
	TestEqual(TEXT("unprocessed level rows survive a degraded resumed pass"), ActorCount, static_cast<int64>(1));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
