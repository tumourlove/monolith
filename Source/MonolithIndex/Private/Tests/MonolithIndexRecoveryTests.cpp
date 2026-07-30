// Copyright tumourlove. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "MonolithIndexDatabase.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"

namespace MonolithIndexRecoveryTests
{
	static FString MakeDatabasePath()
	{
		const FString Directory = FPaths::AutomationTransientDir();
		FPlatformFileManager::Get().GetPlatformFile().CreateDirectoryTree(*Directory);
		return Directory / FString::Printf(
			TEXT("monolith-index-recovery-%s.db"),
			*FGuid::NewGuid().ToString());
	}

	static void DeleteDatabase(const FString& DatabasePath)
	{
		IFileManager::Get().Delete(
			*DatabasePath,
			/*bRequireExists=*/false,
			/*bEvenReadOnly=*/true);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithIndexRecoveryLifecycleTest,
	"Monolith.Index.Recovery.Lifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithIndexRecoveryLifecycleTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithIndexRecoveryTests;

	const FString DatabasePath = MakeDatabasePath();
	DeleteDatabase(DatabasePath);

	{
		FMonolithIndexDatabase Database;
		if (!TestTrue(TEXT("Open fresh database"), Database.Open(DatabasePath)))
		{
			DeleteDatabase(DatabasePath);
			return false;
		}

		TestEqual(TEXT("Fresh database schema version"), Database.ReadMeta(TEXT("schema_version")), FString(TEXT("3")));
		TestFalse(TEXT("Fresh database has no interrupted run"), Database.IsFullIndexInProgress());
		TestTrue(TEXT("Begin full index"), Database.BeginFullIndex());
		TestTrue(TEXT("Full index state is durable"), Database.IsFullIndexInProgress());
		TestTrue(
			TEXT("Record completed deep-index asset"),
			Database.MarkFullIndexAssetComplete(
				TEXT("/Game/Test/BP_Recovery"),
				TEXT("hash-a"),
				TEXT("BlueprintIndexer")));
		TestTrue(
			TEXT("Record completed post-pass"),
			Database.MarkFullIndexPostPassComplete(TEXT("DependencyIndexer")));
		Database.Close();
	}

	{
		FMonolithIndexDatabase Database;
		if (!TestTrue(TEXT("Reopen interrupted database"), Database.Open(DatabasePath)))
		{
			DeleteDatabase(DatabasePath);
			return false;
		}

		TestTrue(TEXT("Interrupted state survives reopen"), Database.IsFullIndexInProgress());
		TestTrue(
			TEXT("Matching asset checkpoint survives reopen"),
			Database.IsFullIndexAssetComplete(
				TEXT("/Game/Test/BP_Recovery"),
				TEXT("hash-a"),
				TEXT("BlueprintIndexer")));
		TestFalse(
			TEXT("Changed asset hash invalidates checkpoint"),
			Database.IsFullIndexAssetComplete(
				TEXT("/Game/Test/BP_Recovery"),
				TEXT("hash-b"),
				TEXT("BlueprintIndexer")));
		TestFalse(
			TEXT("Checkpoint is scoped to its indexer"),
			Database.IsFullIndexAssetComplete(
				TEXT("/Game/Test/BP_Recovery"),
				TEXT("hash-a"),
				TEXT("MaterialIndexer")));
		TestTrue(
			TEXT("Post-pass checkpoint survives reopen"),
			Database.IsFullIndexPostPassComplete(TEXT("DependencyIndexer")));
		TestTrue(
			TEXT("Invalidate post-pass checkpoints after metadata changes"),
			Database.ClearFullIndexPostPassProgress());
		TestFalse(
			TEXT("Invalidated post-pass is pending again"),
			Database.IsFullIndexPostPassComplete(TEXT("DependencyIndexer")));
		TestTrue(
			TEXT("Re-record completed post-pass"),
			Database.MarkFullIndexPostPassComplete(TEXT("DependencyIndexer")));

		const FString CompletedAt = TEXT("2026-07-30T12:00:00Z");
		TestTrue(TEXT("Complete full index"), Database.CompleteFullIndex(CompletedAt));
		TestFalse(TEXT("Completed run is no longer resumable"), Database.IsFullIndexInProgress());
		TestEqual(TEXT("Completion timestamp is published"), Database.ReadMeta(TEXT("last_full_index")), CompletedAt);
		TestFalse(
			TEXT("Completion clears asset checkpoints"),
			Database.IsFullIndexAssetComplete(
				TEXT("/Game/Test/BP_Recovery"),
				TEXT("hash-a"),
				TEXT("BlueprintIndexer")));
		TestFalse(
			TEXT("Completion clears post-pass checkpoints"),
			Database.IsFullIndexPostPassComplete(TEXT("DependencyIndexer")));

		TestTrue(TEXT("Begin second full index"), Database.BeginFullIndex());
		TestTrue(
			TEXT("New full index unpublishes previous completion"),
			Database.ReadMeta(TEXT("last_full_index")).IsEmpty());
		TestTrue(
			TEXT("Record checkpoint before forced reset"),
			Database.MarkFullIndexAssetComplete(
				TEXT("/Game/Test/BP_Reset"),
				TEXT("hash-reset"),
				TEXT("BlueprintIndexer")));
		TestTrue(TEXT("Forced reset recreates schema"), Database.ResetDatabase());
		TestFalse(TEXT("Forced reset clears interrupted state"), Database.IsFullIndexInProgress());
		TestFalse(
			TEXT("Forced reset clears recovery checkpoints"),
			Database.IsFullIndexAssetComplete(
				TEXT("/Game/Test/BP_Reset"),
				TEXT("hash-reset"),
				TEXT("BlueprintIndexer")));
		Database.Close();
	}

	DeleteDatabase(DatabasePath);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithIndexRecoveryTransactionTest,
	"Monolith.Index.Recovery.CheckpointTransaction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithIndexRecoveryTransactionTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithIndexRecoveryTests;

	const FString DatabasePath = MakeDatabasePath();
	DeleteDatabase(DatabasePath);

	FMonolithIndexDatabase Database;
	if (!TestTrue(TEXT("Open database"), Database.Open(DatabasePath)))
	{
		DeleteDatabase(DatabasePath);
		return false;
	}

	TestTrue(TEXT("Begin full index"), Database.BeginFullIndex());
	TestTrue(TEXT("Begin asset transaction"), Database.BeginTransaction());
	TestTrue(
		TEXT("Write checkpoint inside transaction"),
		Database.MarkFullIndexAssetComplete(
			TEXT("/Game/Test/BP_Atomic"),
			TEXT("hash-atomic"),
			TEXT("BlueprintIndexer")));
	TestTrue(TEXT("Rollback asset transaction"), Database.RollbackTransaction());
	TestFalse(
		TEXT("Rolled-back checkpoint is not visible"),
		Database.IsFullIndexAssetComplete(
			TEXT("/Game/Test/BP_Atomic"),
			TEXT("hash-atomic"),
			TEXT("BlueprintIndexer")));

	TestTrue(TEXT("Begin committed asset transaction"), Database.BeginTransaction());
	TestTrue(
		TEXT("Write committed checkpoint"),
		Database.MarkFullIndexAssetComplete(
			TEXT("/Game/Test/BP_Atomic"),
			TEXT("hash-atomic"),
			TEXT("BlueprintIndexer")));
	TestTrue(TEXT("Commit asset transaction"), Database.CommitTransaction());
	TestTrue(
		TEXT("Committed checkpoint is visible"),
		Database.IsFullIndexAssetComplete(
			TEXT("/Game/Test/BP_Atomic"),
			TEXT("hash-atomic"),
			TEXT("BlueprintIndexer")));

	Database.Close();
	DeleteDatabase(DatabasePath);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithIndexRecoveryPostPassCleanupTest,
	"Monolith.Index.Recovery.PostPassCleanup",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithIndexRecoveryPostPassCleanupTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithIndexRecoveryTests;

	const FString DatabasePath = MakeDatabasePath();
	DeleteDatabase(DatabasePath);

	FMonolithIndexDatabase Database;
	if (!TestTrue(TEXT("Open database"), Database.Open(DatabasePath)))
	{
		DeleteDatabase(DatabasePath);
		return false;
	}

	FIndexedAsset BlueprintAsset;
	BlueprintAsset.PackagePath = TEXT("/Game/Test/BP_GAS");
	BlueprintAsset.AssetName = TEXT("BP_GAS");
	BlueprintAsset.AssetClass = TEXT("Blueprint");
	const int64 BlueprintAssetId = Database.InsertAsset(BlueprintAsset);
	TestTrue(TEXT("Insert Blueprint asset"), BlueprintAssetId > 0);

	FIndexedNode BlueprintGraphNode;
	BlueprintGraphNode.AssetId = BlueprintAssetId;
	BlueprintGraphNode.NodeType = TEXT("Function");
	BlueprintGraphNode.NodeName = TEXT("UnrelatedBlueprintNode");
	TestTrue(TEXT("Insert unrelated Blueprint node"), Database.InsertNode(BlueprintGraphNode) > 0);

	FIndexedNode GASNode;
	GASNode.AssetId = BlueprintAssetId;
	GASNode.NodeType = TEXT("GameplayAbility");
	GASNode.NodeName = TEXT("GA_Test");
	TestTrue(TEXT("Insert GAS node"), Database.InsertNode(GASNode) > 0);

	FIndexedAsset MetaSoundAsset;
	MetaSoundAsset.PackagePath = TEXT("/Game/Test/MS_Test");
	MetaSoundAsset.AssetName = TEXT("MS_Test");
	MetaSoundAsset.AssetClass = TEXT("MetaSoundSource");
	const int64 MetaSoundAssetId = Database.InsertAsset(MetaSoundAsset);
	TestTrue(TEXT("Insert MetaSound asset"), MetaSoundAssetId > 0);

	FIndexedNode MetaSoundNode;
	MetaSoundNode.AssetId = MetaSoundAssetId;
	MetaSoundNode.NodeType = TEXT("Node");
	MetaSoundNode.NodeName = TEXT("Oscillator");
	TestTrue(TEXT("Insert MetaSound node"), Database.InsertNode(MetaSoundNode) > 0);

	FIndexedVariable MetaSoundVariable;
	MetaSoundVariable.AssetId = MetaSoundAssetId;
	MetaSoundVariable.VarName = TEXT("Frequency");
	MetaSoundVariable.VarType = TEXT("Float");
	TestTrue(TEXT("Insert MetaSound variable"), Database.InsertVariable(MetaSoundVariable) > 0);

	TestTrue(TEXT("Clear GAS post-pass data"), Database.ClearFullIndexPostPassData(TEXT("GASIndexer")));
	const TArray<FIndexedNode> RemainingBlueprintNodes = Database.GetNodesForAsset(BlueprintAssetId);
	TestEqual(TEXT("GAS cleanup preserves unrelated Blueprint nodes"), RemainingBlueprintNodes.Num(), 1);
	if (RemainingBlueprintNodes.Num() == 1)
	{
		TestEqual(
			TEXT("Preserved Blueprint node is the unrelated row"),
			RemainingBlueprintNodes[0].NodeName,
			FString(TEXT("UnrelatedBlueprintNode")));
	}

	FIndexedNode AINode;
	AINode.AssetId = BlueprintAssetId;
	AINode.NodeType = TEXT("AIController");
	AINode.NodeName = TEXT("AIC_Test");
	TestTrue(TEXT("Insert AI node"), Database.InsertNode(AINode) > 0);
	TestTrue(TEXT("Clear AI post-pass data"), Database.ClearFullIndexPostPassData(TEXT("AI")));
	TestEqual(TEXT("AI cleanup preserves unrelated Blueprint nodes"), Database.GetNodesForAsset(BlueprintAssetId).Num(), 1);

	TestTrue(
		TEXT("Clear MetaSound post-pass data"),
		Database.ClearFullIndexPostPassData(TEXT("MetaSoundIndexer")));
	TestEqual(TEXT("MetaSound cleanup removes graph nodes"), Database.GetNodesForAsset(MetaSoundAssetId).Num(), 0);
	TestEqual(TEXT("MetaSound cleanup removes variables"), Database.GetVariablesForAsset(MetaSoundAssetId).Num(), 0);

	Database.Close();
	DeleteDatabase(DatabasePath);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
