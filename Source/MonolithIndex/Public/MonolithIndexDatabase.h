#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "IO/IoHash.h"
#include "SQLiteDatabase.h"
#include "MonolithIndexLog.h"

struct FIndexedAsset
{
	int64 Id = 0;
	FString PackagePath;
	FString AssetName;
	FString AssetClass;
	FString ModuleName;
	FString Description;
	int64 FileSizeBytes = 0;
	FString LastModified;
	FString SavedHash;
	FString IndexedAt;

	/**
	 * Schema v3. The value `SavedHash` held when this asset last completed deep
	 * indexing, written in the SAME transaction as the child rows it produced.
	 * Empty means "never deep-indexed". Read back by the full-index queue filter
	 * so an interrupted run resumes instead of redoing finished work.
	 */
	FString DeepIndexedHash;

	/**
	 * Schema v3. How many times a full-index run has STARTED deep indexing this
	 * asset without finishing it. Bumped in its own committed transaction before
	 * the work begins and cleared on success, so an asset that kills the editor
	 * is counted even though its work transaction rolls back.
	 */
	int32 DeepIndexAttempts = 0;
};

struct FIndexedNode
{
	int64 Id = 0;
	int64 AssetId = 0;
	FString NodeType;
	FString NodeName;
	FString NodeClass;
	FString Properties; // JSON blob
	int32 PosX = 0;
	int32 PosY = 0;
};

struct FIndexedConnection
{
	int64 Id = 0;
	int64 SourceNodeId = 0;
	FString SourcePin;
	int64 TargetNodeId = 0;
	FString TargetPin;
	FString PinType;
};

struct FIndexedVariable
{
	int64 Id = 0;
	int64 AssetId = 0;
	FString VarName;
	FString VarType;
	FString Category;
	FString DefaultValue;
	bool bIsExposed = false;
	bool bIsReplicated = false;
};

struct FIndexedParameter
{
	int64 Id = 0;
	int64 AssetId = 0;
	FString ParamName;
	FString ParamType;
	FString ParamGroup;
	FString DefaultValue;
	FString Source; // "Material", "Niagara", etc.
};

struct FIndexedDependency
{
	int64 Id = 0;
	int64 SourceAssetId = 0;
	int64 TargetAssetId = 0;
	FString DependencyType; // "Hard", "Soft", "Searchable"
};

struct FIndexedActor
{
	int64 Id = 0;
	int64 AssetId = 0; // Level asset
	FString ActorName;
	FString ActorClass;
	FString ActorLabel;
	FString Transform; // JSON
	FString Components; // JSON array
};

struct FIndexedTag
{
	int64 Id = 0;
	FString TagName;
	FString ParentTag;
	int32 ReferenceCount = 0;
};

struct FIndexedTagReference
{
	int64 Id = 0;
	int64 TagId = 0;
	int64 AssetId = 0;
	FString Context; // "Variable", "Node", "Component", etc.
};

struct FIndexedConfig
{
	int64 Id = 0;
	FString FilePath;
	FString Section;
	FString Key;
	FString Value;
};

struct FIndexedCppSymbol
{
	int64 Id = 0;
	FString FilePath;
	FString SymbolName;
	FString SymbolType; // "Class", "Function", "Enum", "Struct", "Delegate"
	FString Signature;
	int32 LineNumber = 0;
	FString ParentSymbol;
};

struct FIndexedDataTableRow
{
	int64 Id = 0;
	int64 AssetId = 0;
	FString RowName;
	FString RowData; // JSON blob
};

struct FSearchResult
{
	FString AssetPath;
	FString AssetName;
	FString AssetClass;
	FString ModuleName;
	FString MatchContext; // snippet around the match
	float Rank = 0.0f;
};

/** Outcome of a project full-text search, so caller mistakes stay distinct from index failures. */
enum class EMonolithProjectSearchStatus : uint8
{
	/** Search completed, including the valid zero-result case. */
	Succeeded,

	/** The caller supplied malformed FTS5 syntax, or a column no FTS table exposes. */
	InvalidQuery,

	/** The project index could not execute an otherwise valid query. */
	InternalError
};

/** What the full-index deep queue filter decides for a single asset. */
enum class EMonolithDeepIndexQueueDecision : uint8
{
	/** Deep-index it: never done, or its content changed since it last was. */
	Queue,

	/** Already deep-indexed at this exact content hash — a resume must not redo it. */
	SkipAlreadyIndexed,

	/**
	 * Two full-index runs began deep indexing this asset and neither finished.
	 * Treat it as the asset that killed the editor and take it out of the queue,
	 * otherwise every resume re-attempts it and the crash loops forever.
	 */
	SkipPoisonAsset
};

/**
 * Pure decision function behind the resume queue filter. Shared by the indexing
 * worker and its automation test so the rule is asserted where it is defined
 * rather than re-derived at the call site.
 *
 * @param StoredDeepHash    `deep_indexed_hash` from the existing row ('' if none).
 * @param StoredAttempts    `deep_index_attempts` from the existing row.
 * @param CurrentSavedHash  the asset's hash as the Asset Registry reports it now.
 */
MONOLITHINDEX_API EMonolithDeepIndexQueueDecision MonolithDecideDeepIndexQueueEntry(
	const FString& StoredDeepHash,
	int32 StoredAttempts,
	const FString& CurrentSavedHash);

/** Attempts at which an asset is treated as poisonous and dropped from the queue. */
inline constexpr int32 MonolithMaxDeepIndexAttempts = 2;

/**
 * RAII wrapper around FSQLiteDatabase for the Monolith project index.
 * Creates all tables on first open, provides typed insert/query helpers.
 * Thread-safe for reads; writes should be serialized by the caller.
 */
class MONOLITHINDEX_API FMonolithIndexDatabase
{
public:
	FMonolithIndexDatabase();
	~FMonolithIndexDatabase();

	/** Open (or create) the database at the given path */
	bool Open(const FString& InDbPath);

	/** Close the database */
	void Close();

	/** Is the database currently open? */
	bool IsOpen() const;

	/** Wipe all data and recreate tables (for full re-index) */
	bool ResetDatabase();

	/** Direct access to the underlying SQLite database */
	FSQLiteDatabase* GetRawDatabase() const { return Database; }

	// --- Transaction helpers ---
	bool BeginTransaction();
	bool CommitTransaction();
	bool RollbackTransaction();

	// --- Asset CRUD ---
	int64 InsertAsset(const FIndexedAsset& Asset);
	TOptional<FIndexedAsset> GetAssetByPath(const FString& PackagePath);
	int64 GetAssetId(const FString& PackagePath);
	bool DeleteAssetAndRelated(int64 AssetId);

	// --- Node CRUD ---
	int64 InsertNode(const FIndexedNode& Node);
	TArray<FIndexedNode> GetNodesForAsset(int64 AssetId);

	// --- Connection CRUD ---
	int64 InsertConnection(const FIndexedConnection& Conn);
	TArray<FIndexedConnection> GetConnectionsForAsset(int64 AssetId);

	// --- Variable CRUD ---
	int64 InsertVariable(const FIndexedVariable& Var);
	TArray<FIndexedVariable> GetVariablesForAsset(int64 AssetId);

	// --- Parameter CRUD ---
	int64 InsertParameter(const FIndexedParameter& Param);

	// --- Dependency CRUD ---
	int64 InsertDependency(const FIndexedDependency& Dep);
	TArray<FIndexedDependency> GetDependenciesForAsset(int64 AssetId);
	TArray<FIndexedDependency> GetReferencersOfAsset(int64 AssetId);

	// --- Actor CRUD ---
	int64 InsertActor(const FIndexedActor& Actor);

	// --- Tag CRUD ---
	int64 InsertTag(const FIndexedTag& Tag);
	int64 GetOrCreateTag(const FString& TagName, const FString& ParentTag);
	int64 InsertTagReference(const FIndexedTagReference& Ref);

	// --- Meta ---
	bool WriteMeta(const FString& Key, const FString& Value);
	FString ReadMeta(const FString& Key) const;
	bool DeleteMeta(const FString& Key);

	// --- Full-index lifecycle (schema v3) ---

	/**
	 * True when the two v3 resume columns are known to exist. `full_index_state`
	 * is a `meta` row and survives a failed COLUMN migration, so the resume path
	 * must gate on this as well or it would resume onto a schema that cannot
	 * store checkpoints.
	 */
	bool SupportsIndexResume() const;

	/**
	 * Mark a full index as started: writes `full_index_state = in_progress` and
	 * clears `last_full_index`, in one transaction. Call AFTER `ResetDatabase()`
	 * — that drops the `meta` table.
	 */
	bool BeginFullIndex();

	/** The sole resume trigger: was a full index started and never completed? */
	bool IsFullIndexInProgress() const;

	/**
	 * Mark a full index as finished: writes `last_full_index` and clears
	 * `full_index_state` in ONE transaction, so a crash between the two cannot
	 * leave a completed index looking interrupted.
	 */
	bool CompleteFullIndex(const FString& UtcNow);

	/** Record that an asset finished deep indexing at the given content hash. */
	bool SetDeepIndexedHash(int64 AssetId, const FString& Hash);

	/**
	 * Increment `deep_index_attempts` for a whole batch. MUST be called in its
	 * own transaction, committed BEFORE the batch's work transaction opens — a
	 * write inside an open transaction is not durable, and under
	 * `journal_mode=DELETE` a process death mid-batch rolls the work transaction
	 * back on next open, which would take the marker with it.
	 */
	bool BumpDeepIndexAttempts(const TArray<int64>& AssetIds);

	/** Reset the attempt counter after a successful deep index. */
	bool ClearDeepIndexAttempts(int64 AssetId);

	/** Paths dropped from the deep queue by the poison-pill rule, oldest first. */
	TArray<FString> GetSkippedAssetPaths() const;

	/** Merge newly skipped paths into the persisted list (deduplicated). */
	bool RecordSkippedAssetPaths(const TArray<FString>& Paths);

	// --- Config CRUD ---
	int64 InsertConfig(const FIndexedConfig& Config);

	// --- C++ Symbol CRUD ---
	int64 InsertCppSymbol(const FIndexedCppSymbol& Symbol);

	// --- DataTable Row CRUD ---
	int64 InsertDataTableRow(const FIndexedDataTableRow& Row);

	// --- FTS5 Search ---
	TArray<FSearchResult> FullTextSearch(const FString& Query, int32 Limit = 50);

	/**
	 * Search both project FTS tables without conflating a caller's bad query with an
	 * index/storage failure. OutResults is left empty for every non-Succeeded outcome.
	 *
	 * AssetClassFilter, when non-empty, restricts results to those asset classes. It is
	 * applied INSIDE the SQL rather than to the returned array, because LIMIT is applied
	 * by the query: filtering afterwards would return fewer rows than the caller asked
	 * for -- often zero -- while matching assets sat below the cut. Matching is
	 * case-insensitive, so callers need not reproduce UClass casing exactly.
	 */
	EMonolithProjectSearchStatus FullTextSearch(
		const FString& Query,
		int32 Limit,
		TArray<FSearchResult>& OutResults,
		FString& OutError,
		const TArray<FString>& AssetClassFilter = TArray<FString>());

	// --- Stats ---
	TSharedPtr<FJsonObject> GetStats();

	// --- Asset details (all related data) ---
	TSharedPtr<FJsonObject> GetAssetDetails(const FString& PackagePath);

	// --- Find by type ---
	TArray<FIndexedAsset> FindByType(const FString& AssetClass, int32 Limit = 100, int32 Offset = 0);

	// --- Find references (bidirectional) ---
	TSharedPtr<FJsonObject> FindReferences(const FString& PackagePath);

	// --- Incremental indexing helpers ---
	TArray<FString> GetAllIndexedPaths();
	FString GetSavedHash(const FString& PackagePath);
	TMap<FString, FString> GetAllPathsAndHashes();
	bool DeleteAssetByPath(const FString& PackagePath);
	bool UpdateAssetPath(const FString& OldPath, const FString& NewPath, const FString& NewAssetName = FString());
	bool UpdateAssetMetadata(const FIndexedAsset& Asset);
	bool DeleteChildDataForAsset(int64 AssetId);
	bool UpdateSavedHash(const FString& PackagePath, const FString& HashHex);

private:
	bool CreateTables();
	bool ExecuteSQL(const FString& SQL);
	FSQLiteDatabase* Database = nullptr;
	FString DbPath;
};
