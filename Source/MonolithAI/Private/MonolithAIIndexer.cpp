#include "MonolithAIIndexer.h"
#include "MonolithSettings.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Engine/Blueprint.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/BehaviorTreeTypes.h"
#include "BehaviorTree/BTCompositeNode.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BehaviorTree/BTDecorator.h"
#include "AIController.h"
#include "Perception/AIPerceptionComponent.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"

DEFINE_LOG_CATEGORY_STATIC(LogAIIndexer, Log, All);

// ─────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────

int32 FAIIndexer::CountBTNodes(const UBTCompositeNode* RootNode)
{
	if (!RootNode) return 0;

	int32 Count = 1; // the root itself
	for (int32 i = 0; i < RootNode->Children.Num(); ++i)
	{
		const FBTCompositeChild& Child = RootNode->Children[i];
		if (Child.ChildComposite)
		{
			Count += CountBTNodes(Child.ChildComposite);
		}
		else if (Child.ChildTask)
		{
			Count += 1;
		}
		// Count decorators on this child
		Count += Child.Decorators.Num();
	}
	// Decorators are per-child (FBTCompositeChild::Decorators), already counted above
	// Count services
	Count += RootNode->Services.Num();
	return Count;
}

void FAIIndexer::EnsureTablesExist(FMonolithIndexDatabase& DB)
{
	if (bTablesCreated) return;

	FSQLiteDatabase* RawDB = DB.GetRawDatabase();
	if (!RawDB) return;

	// ai_assets: core record for each indexed AI asset
	RawDB->Execute(TEXT(
		"CREATE TABLE IF NOT EXISTS ai_assets ("
		"  id INTEGER PRIMARY KEY AUTOINCREMENT,"
		"  path TEXT NOT NULL UNIQUE,"
		"  type TEXT NOT NULL,"  // BehaviorTree, BlackboardData, AIController
		"  name TEXT NOT NULL"
		")"
	));

	// ai_cross_refs: relationships between AI assets (BT->BB, Controller->BT, etc.)
	RawDB->Execute(TEXT(
		"CREATE TABLE IF NOT EXISTS ai_cross_refs ("
		"  id INTEGER PRIMARY KEY AUTOINCREMENT,"
		"  source_id INTEGER NOT NULL,"
		"  target_id INTEGER NOT NULL,"
		"  ref_type TEXT NOT NULL,"  // uses_blackboard, runs_behavior_tree, etc.
		"  FOREIGN KEY (source_id) REFERENCES ai_assets(id),"
		"  FOREIGN KEY (target_id) REFERENCES ai_assets(id)"
		")"
	));

	// ai_bb_keys: blackboard key definitions
	RawDB->Execute(TEXT(
		"CREATE TABLE IF NOT EXISTS ai_bb_keys ("
		"  id INTEGER PRIMARY KEY AUTOINCREMENT,"
		"  bb_id INTEGER NOT NULL,"
		"  key_name TEXT NOT NULL,"
		"  key_type TEXT NOT NULL,"
		"  FOREIGN KEY (bb_id) REFERENCES ai_assets(id)"
		")"
	));

	// Indices for common queries
	RawDB->Execute(TEXT("CREATE INDEX IF NOT EXISTS idx_ai_assets_type ON ai_assets(type)"));
	RawDB->Execute(TEXT("CREATE INDEX IF NOT EXISTS idx_ai_cross_refs_source ON ai_cross_refs(source_id)"));
	RawDB->Execute(TEXT("CREATE INDEX IF NOT EXISTS idx_ai_cross_refs_target ON ai_cross_refs(target_id)"));
	RawDB->Execute(TEXT("CREATE INDEX IF NOT EXISTS idx_ai_bb_keys_bb ON ai_bb_keys(bb_id)"));

	bTablesCreated = true;
}

// ─────────────────────────────────────────────────────────────
// Helper: insert into ai_assets, return row id
// ─────────────────────────────────────────────────────────────

namespace
{
	int64 InsertAIAsset(FSQLiteDatabase* RawDB, const FString& Path, const FString& Type, const FString& Name)
	{
		if (!RawDB) return -1;

		FString SQL = FString::Printf(
			TEXT("INSERT OR REPLACE INTO ai_assets (path, type, name) VALUES ('%s', '%s', '%s')"),
			*Path.Replace(TEXT("'"), TEXT("''")),
			*Type,
			*Name.Replace(TEXT("'"), TEXT("''")));

		if (!RawDB->Execute(*SQL))
		{
			return -1;
		}
		return RawDB->GetLastInsertRowId();
	}

	void InsertCrossRef(FSQLiteDatabase* RawDB, int64 SourceId, int64 TargetId, const FString& RefType)
	{
		if (!RawDB || SourceId < 0 || TargetId < 0) return;

		FString SQL = FString::Printf(
			TEXT("INSERT INTO ai_cross_refs (source_id, target_id, ref_type) VALUES (%lld, %lld, '%s')"),
			SourceId, TargetId, *RefType);

		RawDB->Execute(*SQL);
	}

	void InsertBBKey(FSQLiteDatabase* RawDB, int64 BBId, const FString& KeyName, const FString& KeyType)
	{
		if (!RawDB || BBId < 0) return;

		FString SQL = FString::Printf(
			TEXT("INSERT INTO ai_bb_keys (bb_id, key_name, key_type) VALUES (%lld, '%s', '%s')"),
			BBId,
			*KeyName.Replace(TEXT("'"), TEXT("''")),
			*KeyType.Replace(TEXT("'"), TEXT("''")));

		RawDB->Execute(*SQL);
	}
}

// ─────────────────────────────────────────────────────────────
// Main sentinel entry point
// ─────────────────────────────────────────────────────────────

bool FAIIndexer::IndexAsset(const FAssetData& AssetData, UObject* LoadedAsset, FMonolithIndexDatabase& DB, int64 AssetId)
{
	EnsureTablesExist(DB);

	FSQLiteDatabase* RawDB = DB.GetRawDatabase();
	if (!RawDB) return false;

	// Clear previous AI index data
	RawDB->Execute(TEXT("DELETE FROM ai_bb_keys"));
	RawDB->Execute(TEXT("DELETE FROM ai_cross_refs"));
	RawDB->Execute(TEXT("DELETE FROM ai_assets"));

	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

	// Build content path filter
	FARFilter Filter;
	for (const FName& ContentPath : UMonolithSettings::GetIndexedContentPaths())
	{
		Filter.PackagePaths.Add(ContentPath);
	}
	Filter.bRecursivePaths = true;

	int32 BTCount = 0, BBCount = 0, ControllerCount = 0;

	// ── 1. BlackboardData ──
	// Index these first so BTs can cross-reference them
	{
		FARFilter BBFilter = Filter;
		BBFilter.ClassPaths.Add(UBlackboardData::StaticClass()->GetClassPathName());

		TArray<FAssetData> BBAssets;
		Registry.GetAssets(BBFilter, BBAssets);

		for (const FAssetData& Asset : BBAssets)
		{
			UBlackboardData* BB = Cast<UBlackboardData>(Asset.GetAsset());
			if (!BB) continue;

			IndexBlackboard(BB, Asset.PackageName.ToString(), DB);
			++BBCount;
		}
	}

	// ── 2. BehaviorTrees ──
	{
		FARFilter BTFilter = Filter;
		BTFilter.ClassPaths.Add(UBehaviorTree::StaticClass()->GetClassPathName());

		TArray<FAssetData> BTAssets;
		Registry.GetAssets(BTFilter, BTAssets);

		for (const FAssetData& Asset : BTAssets)
		{
			UBehaviorTree* BT = Cast<UBehaviorTree>(Asset.GetAsset());
			if (!BT) continue;

			IndexBehaviorTree(BT, Asset.PackageName.ToString(), DB);
			++BTCount;
		}
	}

	// ── 3. AIController Blueprints ──
	{
		FARFilter BPFilter = Filter;
		BPFilter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());

		TArray<FAssetData> BPAssets;
		Registry.GetAssets(BPFilter, BPAssets);

		for (const FAssetData& Asset : BPAssets)
		{
			UBlueprint* BP = Cast<UBlueprint>(Asset.GetAsset());
			if (!BP || !BP->GeneratedClass) continue;
			if (!BP->GeneratedClass->IsChildOf(AAIController::StaticClass())) continue;

			IndexAIController(BP, Asset.PackageName.ToString(), DB);
			++ControllerCount;
		}
	}

	UE_LOG(LogAIIndexer, Log, TEXT("AIIndexer: indexed %d AI assets (%d BTs, %d BBs, %d Controllers)"),
		BTCount + BBCount + ControllerCount, BTCount, BBCount, ControllerCount);

	return true;
}

// ─────────────────────────────────────────────────────────────
// Per-type indexing
// ─────────────────────────────────────────────────────────────

void FAIIndexer::IndexBehaviorTree(UBehaviorTree* BT, const FString& AssetPath, FMonolithIndexDatabase& DB)
{
	FSQLiteDatabase* RawDB = DB.GetRawDatabase();
	if (!RawDB || !BT) return;

	int64 BTId = InsertAIAsset(RawDB, AssetPath, TEXT("BehaviorTree"), BT->GetName());
	if (BTId < 0) return;

	// Also write to the main index as a node with metadata
	FIndexedNode Node;
	Node.AssetId = DB.GetAssetId(AssetPath);
	Node.NodeType = TEXT("BehaviorTree");
	Node.NodeName = BT->GetName();
	Node.NodeClass = TEXT("UBehaviorTree");

	// Build properties JSON
	TSharedPtr<FJsonObject> Props = MakeShared<FJsonObject>();
	Props->SetNumberField(TEXT("node_count"), CountBTNodes(BT->RootNode));

	if (BT->BlackboardAsset)
	{
		FString BBPath = BT->BlackboardAsset->GetPathName();
		Props->SetStringField(TEXT("blackboard"), BBPath);

		// Cross-reference: BT -> BB
		// Look up BB's ai_assets id
		FString LookupSQL = FString::Printf(
			TEXT("SELECT id FROM ai_assets WHERE path = '%s' AND type = 'BlackboardData'"),
			*BT->BlackboardAsset->GetPackage()->GetName().Replace(TEXT("'"), TEXT("''")));

		FSQLitePreparedStatement BTStmt;
		if (BTStmt.Create(*RawDB, *LookupSQL, ESQLitePreparedStatementFlags::Persistent))
		{
			if (BTStmt.Step() == ESQLitePreparedStatementStepResult::Row)
			{
				int64 BBId = 0;
				BTStmt.GetColumnValueByIndex(0, BBId);
				InsertCrossRef(RawDB, BTId, BBId, TEXT("uses_blackboard"));
			}
		}
	}

	FString PropsStr;
	auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&PropsStr);
	FJsonSerializer::Serialize(Props.ToSharedRef(), *Writer, true);
	Node.Properties = PropsStr;

	if (Node.AssetId > 0)
	{
		DB.InsertNode(Node);
	}
}

void FAIIndexer::IndexBlackboard(UBlackboardData* BB, const FString& AssetPath, FMonolithIndexDatabase& DB)
{
	FSQLiteDatabase* RawDB = DB.GetRawDatabase();
	if (!RawDB || !BB) return;

	int64 BBId = InsertAIAsset(RawDB, AssetPath, TEXT("BlackboardData"), BB->GetName());
	if (BBId < 0) return;

	// Index own keys
	for (const FBlackboardEntry& Entry : BB->Keys)
	{
		FString KeyType = TEXT("Unknown");
		if (Entry.KeyType)
		{
			KeyType = Entry.KeyType->GetClass()->GetName();
		}
		InsertBBKey(RawDB, BBId, Entry.EntryName.ToString(), KeyType);
	}

	// Write to main index as a node
	FIndexedNode Node;
	Node.AssetId = DB.GetAssetId(AssetPath);
	Node.NodeType = TEXT("BlackboardData");
	Node.NodeName = BB->GetName();
	Node.NodeClass = TEXT("UBlackboardData");

	TSharedPtr<FJsonObject> Props = MakeShared<FJsonObject>();
	Props->SetNumberField(TEXT("key_count"), BB->Keys.Num());

	if (BB->Parent)
	{
		FString ParentPath = BB->Parent->GetPackage()->GetName();
		Props->SetStringField(TEXT("parent_blackboard"), ParentPath);

		// Cross-reference: BB -> parent BB
		FString LookupSQL = FString::Printf(
			TEXT("SELECT id FROM ai_assets WHERE path = '%s' AND type = 'BlackboardData'"),
			*ParentPath.Replace(TEXT("'"), TEXT("''")));

		FSQLitePreparedStatement BBStmt;
		if (BBStmt.Create(*RawDB, *LookupSQL, ESQLitePreparedStatementFlags::Persistent))
		{
			if (BBStmt.Step() == ESQLitePreparedStatementStepResult::Row)
			{
				int64 ParentId = 0;
				BBStmt.GetColumnValueByIndex(0, ParentId);
				InsertCrossRef(RawDB, BBId, ParentId, TEXT("inherits_blackboard"));
			}
		}
	}

	FString PropsStr;
	auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&PropsStr);
	FJsonSerializer::Serialize(Props.ToSharedRef(), *Writer, true);
	Node.Properties = PropsStr;

	if (Node.AssetId > 0)
	{
		DB.InsertNode(Node);
	}
}

void FAIIndexer::IndexAIController(UBlueprint* BP, const FString& AssetPath, FMonolithIndexDatabase& DB)
{
	FSQLiteDatabase* RawDB = DB.GetRawDatabase();
	if (!RawDB || !BP || !BP->GeneratedClass) return;

	int64 ControllerId = InsertAIAsset(RawDB, AssetPath, TEXT("AIController"), BP->GetName());
	if (ControllerId < 0) return;

	UObject* CDO = BP->GeneratedClass->GetDefaultObject(false);
	AAIController* AIC = CDO ? Cast<AAIController>(CDO) : nullptr;

	FIndexedNode Node;
	Node.AssetId = DB.GetAssetId(AssetPath);
	Node.NodeType = TEXT("AIController");
	Node.NodeName = BP->GetName();
	Node.NodeClass = BP->GeneratedClass->GetSuperClass()
		? BP->GeneratedClass->GetSuperClass()->GetName()
		: TEXT("AAIController");

	TSharedPtr<FJsonObject> Props = MakeShared<FJsonObject>();
	Props->SetStringField(TEXT("parent_class"), Node.NodeClass);

	if (AIC)
	{
		Props->SetBoolField(TEXT("has_perception"), AIC->PerceptionComponent != nullptr);
		Props->SetNumberField(TEXT("team_id"), static_cast<int32>(AIC->GetGenericTeamId().GetId()));
	}
	else
	{
		// CDO not available at index time — provide defaults
		Props->SetBoolField(TEXT("has_perception"), false);
		Props->SetNumberField(TEXT("team_id"), 0);
	}

	FString PropsStr;
	auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&PropsStr);
	FJsonSerializer::Serialize(Props.ToSharedRef(), *Writer, true);
	Node.Properties = PropsStr;

	if (Node.AssetId > 0)
	{
		DB.InsertNode(Node);
	}
}
