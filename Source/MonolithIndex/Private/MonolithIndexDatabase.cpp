#include "MonolithIndexDatabase.h"
#include "GenericPlatform/GenericPlatformFile.h"
#include "SQLiteDatabase.h"
#include "Misc/Paths.h"
#include "HAL/PlatformFileManager.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"

DEFINE_LOG_CATEGORY(LogMonolithIndex);

// ============================================================
// Full table creation SQL
// ============================================================
static const TCHAR* GCreateTablesSQL = TEXT(R"SQL(

-- Core asset table: every indexed asset
CREATE TABLE IF NOT EXISTS assets (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    package_path TEXT NOT NULL UNIQUE,
    asset_name TEXT NOT NULL,
    asset_class TEXT NOT NULL,
    module_name TEXT DEFAULT '',
    description TEXT DEFAULT '',
    file_size_bytes INTEGER DEFAULT 0,
    last_modified TEXT DEFAULT '',
    saved_hash TEXT DEFAULT '',
    indexed_at TEXT DEFAULT (datetime('now')),
    deep_indexed_hash TEXT DEFAULT '',
    deep_index_attempts INTEGER DEFAULT 0
);
CREATE INDEX IF NOT EXISTS idx_assets_class ON assets(asset_class);
CREATE INDEX IF NOT EXISTS idx_assets_name ON assets(asset_name);

-- Graph nodes (Blueprint nodes, Material expressions, Niagara modules, etc.)
CREATE TABLE IF NOT EXISTS nodes (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    asset_id INTEGER NOT NULL REFERENCES assets(id) ON DELETE CASCADE,
    node_type TEXT NOT NULL,
    node_name TEXT NOT NULL,
    node_class TEXT DEFAULT '',
    properties TEXT DEFAULT '{}',
    pos_x INTEGER DEFAULT 0,
    pos_y INTEGER DEFAULT 0
);
CREATE INDEX IF NOT EXISTS idx_nodes_asset ON nodes(asset_id);
CREATE INDEX IF NOT EXISTS idx_nodes_class ON nodes(node_class);

-- Pin connections between nodes
CREATE TABLE IF NOT EXISTS connections (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    source_node_id INTEGER NOT NULL REFERENCES nodes(id) ON DELETE CASCADE,
    source_pin TEXT NOT NULL,
    target_node_id INTEGER NOT NULL REFERENCES nodes(id) ON DELETE CASCADE,
    target_pin TEXT NOT NULL,
    pin_type TEXT DEFAULT ''
);
CREATE INDEX IF NOT EXISTS idx_conn_source ON connections(source_node_id);
CREATE INDEX IF NOT EXISTS idx_conn_target ON connections(target_node_id);

-- Variables (Blueprint variables, material parameters, niagara parameters)
CREATE TABLE IF NOT EXISTS variables (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    asset_id INTEGER NOT NULL REFERENCES assets(id) ON DELETE CASCADE,
    var_name TEXT NOT NULL,
    var_type TEXT NOT NULL,
    category TEXT DEFAULT '',
    default_value TEXT DEFAULT '',
    is_exposed INTEGER DEFAULT 0,
    is_replicated INTEGER DEFAULT 0
);
CREATE INDEX IF NOT EXISTS idx_vars_asset ON variables(asset_id);

-- Parameters (Material params, Niagara params, etc.)
CREATE TABLE IF NOT EXISTS parameters (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    asset_id INTEGER NOT NULL REFERENCES assets(id) ON DELETE CASCADE,
    param_name TEXT NOT NULL,
    param_type TEXT NOT NULL,
    param_group TEXT DEFAULT '',
    default_value TEXT DEFAULT '',
    source TEXT DEFAULT ''
);
CREATE INDEX IF NOT EXISTS idx_params_asset ON parameters(asset_id);

-- Asset dependency graph
CREATE TABLE IF NOT EXISTS dependencies (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    source_asset_id INTEGER NOT NULL REFERENCES assets(id) ON DELETE CASCADE,
    target_asset_id INTEGER NOT NULL REFERENCES assets(id) ON DELETE CASCADE,
    dependency_type TEXT DEFAULT 'Hard'
);
CREATE INDEX IF NOT EXISTS idx_dep_source ON dependencies(source_asset_id);
CREATE INDEX IF NOT EXISTS idx_dep_target ON dependencies(target_asset_id);

-- Level actors
CREATE TABLE IF NOT EXISTS actors (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    asset_id INTEGER NOT NULL REFERENCES assets(id) ON DELETE CASCADE,
    actor_name TEXT NOT NULL,
    actor_class TEXT NOT NULL,
    actor_label TEXT DEFAULT '',
    transform TEXT DEFAULT '{}',
    components TEXT DEFAULT '[]'
);
CREATE INDEX IF NOT EXISTS idx_actors_asset ON actors(asset_id);
CREATE INDEX IF NOT EXISTS idx_actors_class ON actors(actor_class);

-- Gameplay tags
CREATE TABLE IF NOT EXISTS tags (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    tag_name TEXT NOT NULL UNIQUE,
    parent_tag TEXT DEFAULT '',
    reference_count INTEGER DEFAULT 0
);

-- Tag references (which assets use which tags)
CREATE TABLE IF NOT EXISTS tag_references (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    tag_id INTEGER NOT NULL REFERENCES tags(id) ON DELETE CASCADE,
    asset_id INTEGER NOT NULL REFERENCES assets(id) ON DELETE CASCADE,
    context TEXT DEFAULT ''
);
CREATE INDEX IF NOT EXISTS idx_tagref_tag ON tag_references(tag_id);
CREATE INDEX IF NOT EXISTS idx_tagref_asset ON tag_references(asset_id);

-- Config/INI entries
CREATE TABLE IF NOT EXISTS configs (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    file_path TEXT NOT NULL,
    section TEXT NOT NULL,
    key TEXT NOT NULL,
    value TEXT DEFAULT ''
);
CREATE INDEX IF NOT EXISTS idx_configs_file ON configs(file_path);

-- C++ symbols (from tree-sitter via MonolithSource)
CREATE TABLE IF NOT EXISTS cpp_symbols (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    file_path TEXT NOT NULL,
    symbol_name TEXT NOT NULL,
    symbol_type TEXT NOT NULL,
    signature TEXT DEFAULT '',
    line_number INTEGER DEFAULT 0,
    parent_symbol TEXT DEFAULT ''
);
CREATE INDEX IF NOT EXISTS idx_cpp_file ON cpp_symbols(file_path);
CREATE INDEX IF NOT EXISTS idx_cpp_name ON cpp_symbols(symbol_name);

-- Data table rows
CREATE TABLE IF NOT EXISTS datatable_rows (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    asset_id INTEGER NOT NULL REFERENCES assets(id) ON DELETE CASCADE,
    row_name TEXT NOT NULL,
    row_data TEXT DEFAULT '{}'
);
CREATE INDEX IF NOT EXISTS idx_dt_asset ON datatable_rows(asset_id);

-- FTS5 index over assets (name, class, description, path, module)
CREATE VIRTUAL TABLE IF NOT EXISTS fts_assets USING fts5(
    asset_name,
    asset_class,
    description,
    package_path,
    module_name,
    content=assets,
    content_rowid=id,
    tokenize='porter unicode61'
);

-- FTS5 triggers to keep index in sync
CREATE TRIGGER IF NOT EXISTS fts_assets_ai AFTER INSERT ON assets BEGIN
    INSERT INTO fts_assets(rowid, asset_name, asset_class, description, package_path, module_name)
    VALUES (new.id, new.asset_name, new.asset_class, new.description, new.package_path, new.module_name);
END;
CREATE TRIGGER IF NOT EXISTS fts_assets_ad AFTER DELETE ON assets BEGIN
    INSERT INTO fts_assets(fts_assets, rowid, asset_name, asset_class, description, package_path, module_name)
    VALUES ('delete', old.id, old.asset_name, old.asset_class, old.description, old.package_path, old.module_name);
END;
CREATE TRIGGER IF NOT EXISTS fts_assets_au AFTER UPDATE ON assets BEGIN
    INSERT INTO fts_assets(fts_assets, rowid, asset_name, asset_class, description, package_path, module_name)
    VALUES ('delete', old.id, old.asset_name, old.asset_class, old.description, old.package_path, old.module_name);
    INSERT INTO fts_assets(rowid, asset_name, asset_class, description, package_path, module_name)
    VALUES (new.id, new.asset_name, new.asset_class, new.description, new.package_path, new.module_name);
END;

-- FTS5 index over nodes (name, class, type)
CREATE VIRTUAL TABLE IF NOT EXISTS fts_nodes USING fts5(
    node_name,
    node_class,
    node_type,
    content=nodes,
    content_rowid=id,
    tokenize='porter unicode61'
);

CREATE TRIGGER IF NOT EXISTS fts_nodes_ai AFTER INSERT ON nodes BEGIN
    INSERT INTO fts_nodes(rowid, node_name, node_class, node_type)
    VALUES (new.id, new.node_name, new.node_class, new.node_type);
END;
CREATE TRIGGER IF NOT EXISTS fts_nodes_ad AFTER DELETE ON nodes BEGIN
    INSERT INTO fts_nodes(fts_nodes, rowid, node_name, node_class, node_type)
    VALUES ('delete', old.id, old.node_name, old.node_class, old.node_type);
END;
CREATE TRIGGER IF NOT EXISTS fts_nodes_au AFTER UPDATE ON nodes BEGIN
    INSERT INTO fts_nodes(fts_nodes, rowid, node_name, node_class, node_type)
    VALUES ('delete', old.id, old.node_name, old.node_class, old.node_type);
    INSERT INTO fts_nodes(rowid, node_name, node_class, node_type)
    VALUES (new.id, new.node_name, new.node_class, new.node_type);
END;

-- Metadata table for tracking index state
CREATE TABLE IF NOT EXISTS meta (
    key TEXT PRIMARY KEY,
    value TEXT DEFAULT ''
);

)SQL");

// ============================================================
// Constructor / Destructor
// ============================================================

FMonolithIndexDatabase::FMonolithIndexDatabase()
{
}

FMonolithIndexDatabase::~FMonolithIndexDatabase()
{
	Close();
}

bool FMonolithIndexDatabase::Open(const FString& InDbPath)
{
	if (Database)
	{
		Close();
	}

	DbPath = InDbPath;

	// Ensure directory exists
	FString Dir = FPaths::GetPath(DbPath);
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	if (!PlatformFile.DirectoryExists(*Dir))
	{
		PlatformFile.CreateDirectoryTree(*Dir);
	}

	Database = new FSQLiteDatabase();
	if (!Database->Open(*DbPath, ESQLiteDatabaseOpenMode::ReadWriteCreate))
	{
		UE_LOG(LogMonolithIndex, Error, TEXT("Failed to open index database: %s"), *DbPath);
		delete Database;
		Database = nullptr;
		return false;
	}

	// Force DELETE journal mode — WAL + ReadOnly on Windows silently returns 0 rows.
	// Belt-and-suspenders: force DELETE here regardless of what the DB was created with.
	ExecuteSQL(TEXT("PRAGMA journal_mode=DELETE;"));
	ExecuteSQL(TEXT("PRAGMA synchronous=NORMAL;"));
	ExecuteSQL(TEXT("PRAGMA foreign_keys=ON;"));
	ExecuteSQL(TEXT("PRAGMA cache_size=-64000;")); // 64MB cache

	if (!CreateTables())
	{
		UE_LOG(LogMonolithIndex, Error, TEXT("Failed to create index tables"));
		Close();
		return false;
	}

	// Schema migration: v1 -> v2
	{
		FString SchemaVersion = ReadMeta(TEXT("schema_version"));
		if (SchemaVersion.IsEmpty() || FCString::Atoi(*SchemaVersion) < 2)
		{
			// Check if saved_hash column already exists (fresh DBs have it from CreateTables)
			bool bHasSavedHash = false;
			FSQLitePreparedStatement PragmaStmt;
			if (PragmaStmt.Create(*Database, TEXT("PRAGMA table_info(assets);"), ESQLitePreparedStatementFlags::Persistent))
			{
				while (PragmaStmt.Step() == ESQLitePreparedStatementStepResult::Row)
				{
					FString ColName;
					PragmaStmt.GetColumnValueByIndex(1, ColName);
					if (ColName == TEXT("saved_hash"))
					{
						bHasSavedHash = true;
						break;
					}
				}
			}

			if (!bHasSavedHash)
			{
				ExecuteSQL(TEXT("ALTER TABLE assets ADD COLUMN saved_hash TEXT DEFAULT '';"));
				ExecuteSQL(TEXT("CREATE INDEX IF NOT EXISTS idx_assets_hash ON assets(saved_hash);"));
			}
			WriteMeta(TEXT("schema_version"), TEXT("2"));
		}
	}

	// Schema migration: -> v3 (full-index resume). Additive only: two columns on
	// `assets`, both also in the CREATE TABLE literal so fresh DBs skip the ALTER.
	// Runs UNCONDITIONALLY after the v1->v2 block: a fresh DB passes through that
	// block first (which stamps "2"), so only an unconditional `< 3` gate here
	// gets it to "3".
	//
	// A failed migration must NOT close the database. Leaving the version at 2
	// keeps `project_query` answering for the whole session; the resume path
	// gates on SupportsIndexResume() and degrades to a full reset instead.
	{
		if (FCString::Atoi(*ReadMeta(TEXT("schema_version"))) < 3)
		{
			bool bHasDeepHash = false;
			bool bHasAttempts = false;
			FSQLitePreparedStatement PragmaStmt;
			if (PragmaStmt.Create(*Database, TEXT("PRAGMA table_info(assets);"), ESQLitePreparedStatementFlags::Persistent))
			{
				while (PragmaStmt.Step() == ESQLitePreparedStatementStepResult::Row)
				{
					FString ColName;
					PragmaStmt.GetColumnValueByIndex(1, ColName);
					if (ColName == TEXT("deep_indexed_hash"))
					{
						bHasDeepHash = true;
					}
					else if (ColName == TEXT("deep_index_attempts"))
					{
						bHasAttempts = true;
					}
				}
			}

			bool bMigrated = true;
			if (!bHasDeepHash)
			{
				bMigrated &= ExecuteSQL(TEXT("ALTER TABLE assets ADD COLUMN deep_indexed_hash TEXT DEFAULT '';"));
			}
			if (!bHasAttempts)
			{
				bMigrated &= ExecuteSQL(TEXT("ALTER TABLE assets ADD COLUMN deep_index_attempts INTEGER DEFAULT 0;"));
			}

			if (bMigrated)
			{
				WriteMeta(TEXT("schema_version"), TEXT("3"));
			}
			else
			{
				UE_LOG(LogMonolithIndex, Error,
					TEXT("Index schema migration to v3 failed — index resume is unavailable this session, the index itself is unaffected"));
			}
		}
	}

	// Ensure hash index exists (safe for both fresh and migrated DBs)
	ExecuteSQL(TEXT("CREATE INDEX IF NOT EXISTS idx_assets_hash ON assets(saved_hash);"));

	UE_LOG(LogMonolithIndex, Log, TEXT("Index database opened: %s"), *DbPath);
	return true;
}

void FMonolithIndexDatabase::Close()
{
	if (Database)
	{
		Database->Close();
		delete Database;
		Database = nullptr;
	}
}

bool FMonolithIndexDatabase::IsOpen() const
{
	return Database != nullptr && Database->IsValid();
}

bool FMonolithIndexDatabase::ResetDatabase()
{
	if (!IsOpen()) return false;

	// Drop all tables and recreate — order matters for foreign keys
	ExecuteSQL(TEXT("DROP TRIGGER IF EXISTS fts_assets_ai;"));
	ExecuteSQL(TEXT("DROP TRIGGER IF EXISTS fts_assets_ad;"));
	ExecuteSQL(TEXT("DROP TRIGGER IF EXISTS fts_assets_au;"));
	ExecuteSQL(TEXT("DROP TRIGGER IF EXISTS fts_nodes_ai;"));
	ExecuteSQL(TEXT("DROP TRIGGER IF EXISTS fts_nodes_ad;"));
	ExecuteSQL(TEXT("DROP TRIGGER IF EXISTS fts_nodes_au;"));
	ExecuteSQL(TEXT("DROP TABLE IF EXISTS fts_assets;"));
	ExecuteSQL(TEXT("DROP TABLE IF EXISTS fts_nodes;"));
	ExecuteSQL(TEXT("DROP TABLE IF EXISTS tag_references;"));
	ExecuteSQL(TEXT("DROP TABLE IF EXISTS tags;"));
	ExecuteSQL(TEXT("DROP TABLE IF EXISTS connections;"));
	ExecuteSQL(TEXT("DROP TABLE IF EXISTS nodes;"));
	ExecuteSQL(TEXT("DROP TABLE IF EXISTS variables;"));
	ExecuteSQL(TEXT("DROP TABLE IF EXISTS parameters;"));
	ExecuteSQL(TEXT("DROP TABLE IF EXISTS dependencies;"));
	ExecuteSQL(TEXT("DROP TABLE IF EXISTS actors;"));
	ExecuteSQL(TEXT("DROP TABLE IF EXISTS configs;"));
	ExecuteSQL(TEXT("DROP TABLE IF EXISTS cpp_symbols;"));
	ExecuteSQL(TEXT("DROP TABLE IF EXISTS datatable_rows;"));
	ExecuteSQL(TEXT("DROP TABLE IF EXISTS meta;"));
	ExecuteSQL(TEXT("DROP TABLE IF EXISTS assets;"));

	if (!CreateTables())
	{
		return false;
	}

	// `meta` was just dropped, so the version stamp went with it. Recreated tables
	// carry the current shape, so restate it here — otherwise the DB reports "no
	// schema version" until the next Open(), and every version-gated path
	// (incremental indexing, resume) silently degrades for the rest of the session.
	ExecuteSQL(TEXT("CREATE INDEX IF NOT EXISTS idx_assets_hash ON assets(saved_hash);"));
	return WriteMeta(TEXT("schema_version"), TEXT("3"));
}

// ============================================================
// Transaction helpers
// ============================================================

bool FMonolithIndexDatabase::BeginTransaction()
{
	return ExecuteSQL(TEXT("BEGIN TRANSACTION;"));
}

bool FMonolithIndexDatabase::CommitTransaction()
{
	return ExecuteSQL(TEXT("COMMIT;"));
}

bool FMonolithIndexDatabase::RollbackTransaction()
{
	return ExecuteSQL(TEXT("ROLLBACK;"));
}

// ============================================================
// Asset CRUD
// ============================================================

int64 FMonolithIndexDatabase::InsertAsset(const FIndexedAsset& Asset)
{
	if (!IsOpen()) return -1;

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("INSERT INTO assets (package_path, asset_name, asset_class, module_name, description, file_size_bytes, last_modified, saved_hash) VALUES (?, ?, ?, ?, ?, ?, ?, ?);"));
	Stmt.SetBindingValueByIndex(1, Asset.PackagePath);
	Stmt.SetBindingValueByIndex(2, Asset.AssetName);
	Stmt.SetBindingValueByIndex(3, Asset.AssetClass);
	Stmt.SetBindingValueByIndex(4, Asset.ModuleName);
	Stmt.SetBindingValueByIndex(5, Asset.Description);
	Stmt.SetBindingValueByIndex(6, Asset.FileSizeBytes);
	Stmt.SetBindingValueByIndex(7, Asset.LastModified);
	Stmt.SetBindingValueByIndex(8, Asset.SavedHash);

	if (!Stmt.Execute()) return -1;
	return Database->GetLastInsertRowId();
}

TOptional<FIndexedAsset> FMonolithIndexDatabase::GetAssetByPath(const FString& PackagePath)
{
	if (!IsOpen()) return {};

	// The two v3 columns ride along on the SELECT the full-index queue filter
	// already makes, so resume costs zero extra statements per asset.
	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("SELECT id, package_path, asset_name, asset_class, module_name, description, file_size_bytes, last_modified, saved_hash, indexed_at, deep_indexed_hash, deep_index_attempts FROM assets WHERE package_path = ?;"));
	Stmt.SetBindingValueByIndex(1, PackagePath);

	if (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FIndexedAsset Asset;
		Stmt.GetColumnValueByIndex(0, Asset.Id);
		Stmt.GetColumnValueByIndex(1, Asset.PackagePath);
		Stmt.GetColumnValueByIndex(2, Asset.AssetName);
		Stmt.GetColumnValueByIndex(3, Asset.AssetClass);
		Stmt.GetColumnValueByIndex(4, Asset.ModuleName);
		Stmt.GetColumnValueByIndex(5, Asset.Description);
		Stmt.GetColumnValueByIndex(6, Asset.FileSizeBytes);
		Stmt.GetColumnValueByIndex(7, Asset.LastModified);
		Stmt.GetColumnValueByIndex(8, Asset.SavedHash);
		Stmt.GetColumnValueByIndex(9, Asset.IndexedAt);
		Stmt.GetColumnValueByIndex(10, Asset.DeepIndexedHash);
		Stmt.GetColumnValueByIndex(11, Asset.DeepIndexAttempts);
		return Asset;
	}
	return {};
}

int64 FMonolithIndexDatabase::GetAssetId(const FString& PackagePath)
{
	if (!IsOpen()) return -1;

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("SELECT id FROM assets WHERE package_path = ?;"));
	Stmt.SetBindingValueByIndex(1, PackagePath);

	if (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		int64 Id = 0;
		Stmt.GetColumnValueByIndex(0, Id);
		return Id;
	}
	return -1;
}

bool FMonolithIndexDatabase::DeleteAssetAndRelated(int64 AssetId)
{
	// CASCADE handles child rows
	return ExecuteSQL(FString::Printf(TEXT("DELETE FROM assets WHERE id = %lld;"), AssetId));
}

// ============================================================
// Node CRUD
// ============================================================

int64 FMonolithIndexDatabase::InsertNode(const FIndexedNode& Node)
{
	if (!IsOpen()) return -1;

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("INSERT INTO nodes (asset_id, node_type, node_name, node_class, properties, pos_x, pos_y) VALUES (?, ?, ?, ?, ?, ?, ?);"));
	Stmt.SetBindingValueByIndex(1, Node.AssetId);
	Stmt.SetBindingValueByIndex(2, Node.NodeType);
	Stmt.SetBindingValueByIndex(3, Node.NodeName);
	Stmt.SetBindingValueByIndex(4, Node.NodeClass);
	Stmt.SetBindingValueByIndex(5, Node.Properties);
	Stmt.SetBindingValueByIndex(6, static_cast<int64>(Node.PosX));
	Stmt.SetBindingValueByIndex(7, static_cast<int64>(Node.PosY));

	if (!Stmt.Execute()) return -1;
	return Database->GetLastInsertRowId();
}

TArray<FIndexedNode> FMonolithIndexDatabase::GetNodesForAsset(int64 AssetId)
{
	TArray<FIndexedNode> Result;
	if (!IsOpen()) return Result;

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("SELECT id, asset_id, node_type, node_name, node_class, properties, pos_x, pos_y FROM nodes WHERE asset_id = ?;"));
	Stmt.SetBindingValueByIndex(1, AssetId);

	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FIndexedNode Node;
		Stmt.GetColumnValueByIndex(0, Node.Id);
		Stmt.GetColumnValueByIndex(1, Node.AssetId);
		Stmt.GetColumnValueByIndex(2, Node.NodeType);
		Stmt.GetColumnValueByIndex(3, Node.NodeName);
		Stmt.GetColumnValueByIndex(4, Node.NodeClass);
		Stmt.GetColumnValueByIndex(5, Node.Properties);
		Stmt.GetColumnValueByIndex(6, Node.PosX);
		Stmt.GetColumnValueByIndex(7, Node.PosY);
		Result.Add(MoveTemp(Node));
	}
	return Result;
}

// ============================================================
// Connection CRUD
// ============================================================

int64 FMonolithIndexDatabase::InsertConnection(const FIndexedConnection& Conn)
{
	if (!IsOpen()) return -1;

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("INSERT INTO connections (source_node_id, source_pin, target_node_id, target_pin, pin_type) VALUES (?, ?, ?, ?, ?);"));
	Stmt.SetBindingValueByIndex(1, Conn.SourceNodeId);
	Stmt.SetBindingValueByIndex(2, Conn.SourcePin);
	Stmt.SetBindingValueByIndex(3, Conn.TargetNodeId);
	Stmt.SetBindingValueByIndex(4, Conn.TargetPin);
	Stmt.SetBindingValueByIndex(5, Conn.PinType);

	if (!Stmt.Execute()) return -1;
	return Database->GetLastInsertRowId();
}

TArray<FIndexedConnection> FMonolithIndexDatabase::GetConnectionsForAsset(int64 AssetId)
{
	TArray<FIndexedConnection> Result;
	if (!IsOpen()) return Result;

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("SELECT c.id, c.source_node_id, c.source_pin, c.target_node_id, c.target_pin, c.pin_type FROM connections c INNER JOIN nodes n ON c.source_node_id = n.id WHERE n.asset_id = ?;"));
	Stmt.SetBindingValueByIndex(1, AssetId);

	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FIndexedConnection Conn;
		Stmt.GetColumnValueByIndex(0, Conn.Id);
		Stmt.GetColumnValueByIndex(1, Conn.SourceNodeId);
		Stmt.GetColumnValueByIndex(2, Conn.SourcePin);
		Stmt.GetColumnValueByIndex(3, Conn.TargetNodeId);
		Stmt.GetColumnValueByIndex(4, Conn.TargetPin);
		Stmt.GetColumnValueByIndex(5, Conn.PinType);
		Result.Add(MoveTemp(Conn));
	}
	return Result;
}

// ============================================================
// Variable CRUD
// ============================================================

int64 FMonolithIndexDatabase::InsertVariable(const FIndexedVariable& Var)
{
	if (!IsOpen()) return -1;

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("INSERT INTO variables (asset_id, var_name, var_type, category, default_value, is_exposed, is_replicated) VALUES (?, ?, ?, ?, ?, ?, ?);"));
	Stmt.SetBindingValueByIndex(1, Var.AssetId);
	Stmt.SetBindingValueByIndex(2, Var.VarName);
	Stmt.SetBindingValueByIndex(3, Var.VarType);
	Stmt.SetBindingValueByIndex(4, Var.Category);
	Stmt.SetBindingValueByIndex(5, Var.DefaultValue);
	Stmt.SetBindingValueByIndex(6, static_cast<int64>(Var.bIsExposed ? 1 : 0));
	Stmt.SetBindingValueByIndex(7, static_cast<int64>(Var.bIsReplicated ? 1 : 0));

	if (!Stmt.Execute()) return -1;
	return Database->GetLastInsertRowId();
}

TArray<FIndexedVariable> FMonolithIndexDatabase::GetVariablesForAsset(int64 AssetId)
{
	TArray<FIndexedVariable> Result;
	if (!IsOpen()) return Result;

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("SELECT id, asset_id, var_name, var_type, category, default_value, is_exposed, is_replicated FROM variables WHERE asset_id = ?;"));
	Stmt.SetBindingValueByIndex(1, AssetId);

	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FIndexedVariable Var;
		Stmt.GetColumnValueByIndex(0, Var.Id);
		Stmt.GetColumnValueByIndex(1, Var.AssetId);
		Stmt.GetColumnValueByIndex(2, Var.VarName);
		Stmt.GetColumnValueByIndex(3, Var.VarType);
		Stmt.GetColumnValueByIndex(4, Var.Category);
		Stmt.GetColumnValueByIndex(5, Var.DefaultValue);
		int32 Exposed = 0, Replicated = 0;
		Stmt.GetColumnValueByIndex(6, Exposed);
		Stmt.GetColumnValueByIndex(7, Replicated);
		Var.bIsExposed = Exposed != 0;
		Var.bIsReplicated = Replicated != 0;
		Result.Add(MoveTemp(Var));
	}
	return Result;
}

// ============================================================
// Parameter CRUD
// ============================================================

int64 FMonolithIndexDatabase::InsertParameter(const FIndexedParameter& Param)
{
	if (!IsOpen()) return -1;

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("INSERT INTO parameters (asset_id, param_name, param_type, param_group, default_value, source) VALUES (?, ?, ?, ?, ?, ?);"));
	Stmt.SetBindingValueByIndex(1, Param.AssetId);
	Stmt.SetBindingValueByIndex(2, Param.ParamName);
	Stmt.SetBindingValueByIndex(3, Param.ParamType);
	Stmt.SetBindingValueByIndex(4, Param.ParamGroup);
	Stmt.SetBindingValueByIndex(5, Param.DefaultValue);
	Stmt.SetBindingValueByIndex(6, Param.Source);

	if (!Stmt.Execute()) return -1;
	return Database->GetLastInsertRowId();
}

// ============================================================
// Dependency CRUD
// ============================================================

int64 FMonolithIndexDatabase::InsertDependency(const FIndexedDependency& Dep)
{
	if (!IsOpen()) return -1;

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("INSERT INTO dependencies (source_asset_id, target_asset_id, dependency_type) VALUES (?, ?, ?);"));
	Stmt.SetBindingValueByIndex(1, Dep.SourceAssetId);
	Stmt.SetBindingValueByIndex(2, Dep.TargetAssetId);
	Stmt.SetBindingValueByIndex(3, Dep.DependencyType);

	if (!Stmt.Execute()) return -1;
	return Database->GetLastInsertRowId();
}

TArray<FIndexedDependency> FMonolithIndexDatabase::GetDependenciesForAsset(int64 AssetId)
{
	TArray<FIndexedDependency> Result;
	if (!IsOpen()) return Result;

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("SELECT id, source_asset_id, target_asset_id, dependency_type FROM dependencies WHERE source_asset_id = ?;"));
	Stmt.SetBindingValueByIndex(1, AssetId);

	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FIndexedDependency Dep;
		Stmt.GetColumnValueByIndex(0, Dep.Id);
		Stmt.GetColumnValueByIndex(1, Dep.SourceAssetId);
		Stmt.GetColumnValueByIndex(2, Dep.TargetAssetId);
		Stmt.GetColumnValueByIndex(3, Dep.DependencyType);
		Result.Add(MoveTemp(Dep));
	}
	return Result;
}

TArray<FIndexedDependency> FMonolithIndexDatabase::GetReferencersOfAsset(int64 AssetId)
{
	TArray<FIndexedDependency> Result;
	if (!IsOpen()) return Result;

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("SELECT id, source_asset_id, target_asset_id, dependency_type FROM dependencies WHERE target_asset_id = ?;"));
	Stmt.SetBindingValueByIndex(1, AssetId);

	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FIndexedDependency Dep;
		Stmt.GetColumnValueByIndex(0, Dep.Id);
		Stmt.GetColumnValueByIndex(1, Dep.SourceAssetId);
		Stmt.GetColumnValueByIndex(2, Dep.TargetAssetId);
		Stmt.GetColumnValueByIndex(3, Dep.DependencyType);
		Result.Add(MoveTemp(Dep));
	}
	return Result;
}

// ============================================================
// Actor CRUD
// ============================================================

int64 FMonolithIndexDatabase::InsertActor(const FIndexedActor& Actor)
{
	if (!IsOpen()) return -1;

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("INSERT INTO actors (asset_id, actor_name, actor_class, actor_label, transform, components) VALUES (?, ?, ?, ?, ?, ?);"));
	Stmt.SetBindingValueByIndex(1, Actor.AssetId);
	Stmt.SetBindingValueByIndex(2, Actor.ActorName);
	Stmt.SetBindingValueByIndex(3, Actor.ActorClass);
	Stmt.SetBindingValueByIndex(4, Actor.ActorLabel);
	Stmt.SetBindingValueByIndex(5, Actor.Transform);
	Stmt.SetBindingValueByIndex(6, Actor.Components);

	if (!Stmt.Execute()) return -1;
	return Database->GetLastInsertRowId();
}

// ============================================================
// Tag CRUD
// ============================================================

int64 FMonolithIndexDatabase::InsertTag(const FIndexedTag& Tag)
{
	if (!IsOpen()) return -1;

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("INSERT OR IGNORE INTO tags (tag_name, parent_tag, reference_count) VALUES (?, ?, ?);"));
	Stmt.SetBindingValueByIndex(1, Tag.TagName);
	Stmt.SetBindingValueByIndex(2, Tag.ParentTag);
	Stmt.SetBindingValueByIndex(3, static_cast<int64>(Tag.ReferenceCount));

	if (!Stmt.Execute()) return -1;
	return GetOrCreateTag(Tag.TagName, Tag.ParentTag);
}

int64 FMonolithIndexDatabase::GetOrCreateTag(const FString& TagName, const FString& ParentTag)
{
	if (!IsOpen()) return -1;

	// Try to get existing
	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("SELECT id FROM tags WHERE tag_name = ?;"));
	Stmt.SetBindingValueByIndex(1, TagName);

	if (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		int64 Id = 0;
		Stmt.GetColumnValueByIndex(0, Id);
		return Id;
	}

	// Insert new
	FSQLitePreparedStatement InsertStmt;
	InsertStmt.Create(*Database, TEXT("INSERT INTO tags (tag_name, parent_tag) VALUES (?, ?);"));
	InsertStmt.SetBindingValueByIndex(1, TagName);
	InsertStmt.SetBindingValueByIndex(2, ParentTag);
	InsertStmt.Execute();
	return Database->GetLastInsertRowId();
}

int64 FMonolithIndexDatabase::InsertTagReference(const FIndexedTagReference& Ref)
{
	if (!IsOpen()) return -1;

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("INSERT INTO tag_references (tag_id, asset_id, context) VALUES (?, ?, ?);"));
	Stmt.SetBindingValueByIndex(1, Ref.TagId);
	Stmt.SetBindingValueByIndex(2, Ref.AssetId);
	Stmt.SetBindingValueByIndex(3, Ref.Context);

	if (!Stmt.Execute()) return -1;

	// Update reference count
	FSQLitePreparedStatement UpdateStmt;
	UpdateStmt.Create(*Database, TEXT("UPDATE tags SET reference_count = (SELECT COUNT(*) FROM tag_references WHERE tag_id = ?) WHERE id = ?;"));
	UpdateStmt.SetBindingValueByIndex(1, Ref.TagId);
	UpdateStmt.SetBindingValueByIndex(2, Ref.TagId);
	UpdateStmt.Execute();

	return Database->GetLastInsertRowId();
}

// ============================================================
// Config CRUD
// ============================================================

int64 FMonolithIndexDatabase::InsertConfig(const FIndexedConfig& Config)
{
	if (!IsOpen()) return -1;

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("INSERT INTO configs (file_path, section, key, value) VALUES (?, ?, ?, ?);"));
	Stmt.SetBindingValueByIndex(1, Config.FilePath);
	Stmt.SetBindingValueByIndex(2, Config.Section);
	Stmt.SetBindingValueByIndex(3, Config.Key);
	Stmt.SetBindingValueByIndex(4, Config.Value);

	if (!Stmt.Execute()) return -1;
	return Database->GetLastInsertRowId();
}

// ============================================================
// C++ Symbol CRUD
// ============================================================

int64 FMonolithIndexDatabase::InsertCppSymbol(const FIndexedCppSymbol& Symbol)
{
	if (!IsOpen()) return -1;

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("INSERT INTO cpp_symbols (file_path, symbol_name, symbol_type, signature, line_number, parent_symbol) VALUES (?, ?, ?, ?, ?, ?);"));
	Stmt.SetBindingValueByIndex(1, Symbol.FilePath);
	Stmt.SetBindingValueByIndex(2, Symbol.SymbolName);
	Stmt.SetBindingValueByIndex(3, Symbol.SymbolType);
	Stmt.SetBindingValueByIndex(4, Symbol.Signature);
	Stmt.SetBindingValueByIndex(5, static_cast<int64>(Symbol.LineNumber));
	Stmt.SetBindingValueByIndex(6, Symbol.ParentSymbol);

	if (!Stmt.Execute()) return -1;
	return Database->GetLastInsertRowId();
}

// ============================================================
// DataTable Row CRUD
// ============================================================

int64 FMonolithIndexDatabase::InsertDataTableRow(const FIndexedDataTableRow& Row)
{
	if (!IsOpen()) return -1;

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("INSERT INTO datatable_rows (asset_id, row_name, row_data) VALUES (?, ?, ?);"));
	Stmt.SetBindingValueByIndex(1, Row.AssetId);
	Stmt.SetBindingValueByIndex(2, Row.RowName);
	Stmt.SetBindingValueByIndex(3, Row.RowData);

	if (!Stmt.Execute()) return -1;
	return Database->GetLastInsertRowId();
}

// ============================================================
// Meta
// ============================================================

bool FMonolithIndexDatabase::WriteMeta(const FString& Key, const FString& Value)
{
	if (!IsOpen()) return false;

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("INSERT OR REPLACE INTO meta (key, value) VALUES (?, ?);"));
	Stmt.SetBindingValueByIndex(1, Key);
	Stmt.SetBindingValueByIndex(2, Value);
	return Stmt.Execute();
}

FString FMonolithIndexDatabase::ReadMeta(const FString& Key) const
{
	if (!Database || !Database->IsValid()) return FString();

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("SELECT value FROM meta WHERE key = ?;"));
	Stmt.SetBindingValueByIndex(1, Key);

	if (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FString Value;
		Stmt.GetColumnValueByIndex(0, Value);
		return Value;
	}
	return FString();
}

bool FMonolithIndexDatabase::DeleteMeta(const FString& Key)
{
	if (!IsOpen()) return false;

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("DELETE FROM meta WHERE key = ?;"));
	Stmt.SetBindingValueByIndex(1, Key);
	return Stmt.Execute();
}

// ============================================================
// Full-index lifecycle (schema v3)
// ============================================================

// Named (not anonymous) namespace, and prefixed: file-local duplicates of a
// common name are what collide once the release build forces full unity.
namespace MonolithIndexMetaKeys
{
	static const TCHAR* const FullIndexState = TEXT("full_index_state");
	static const TCHAR* const FullIndexInProgress = TEXT("in_progress");
	static const TCHAR* const LastFullIndex = TEXT("last_full_index");
	static const TCHAR* const SkippedAssets = TEXT("full_index_skipped_assets");
}

EMonolithDeepIndexQueueDecision MonolithDecideDeepIndexQueueEntry(
	const FString& StoredDeepHash,
	int32 StoredAttempts,
	const FString& CurrentSavedHash)
{
	// Hash equality is the ONLY "already done" gate. A build that predates schema
	// v3 updates `saved_hash` without maintaining `deep_indexed_hash`, so after a
	// downgrade-then-upgrade the checkpoint can be stale; comparing it against the
	// hash the Asset Registry reports right now re-queues those assets naturally.
	// An empty hash on either side never counts as a match.
	if (!StoredDeepHash.IsEmpty() && !CurrentSavedHash.IsEmpty() && StoredDeepHash == CurrentSavedHash)
	{
		return EMonolithDeepIndexQueueDecision::SkipAlreadyIndexed;
	}

	if (StoredAttempts >= MonolithMaxDeepIndexAttempts)
	{
		return EMonolithDeepIndexQueueDecision::SkipPoisonAsset;
	}

	return EMonolithDeepIndexQueueDecision::Queue;
}

bool FMonolithIndexDatabase::SupportsIndexResume() const
{
	if (!Database || !Database->IsValid()) return false;
	return FCString::Atoi(*ReadMeta(TEXT("schema_version"))) >= 3;
}

bool FMonolithIndexDatabase::BeginFullIndex()
{
	if (!IsOpen()) return false;

	// One transaction: a death between the two writes would otherwise leave both
	// markers set, which reads as "indexed AND interrupted" on the next launch.
	if (!BeginTransaction()) return false;

	if (!WriteMeta(MonolithIndexMetaKeys::FullIndexState, MonolithIndexMetaKeys::FullIndexInProgress) || !DeleteMeta(MonolithIndexMetaKeys::LastFullIndex))
	{
		RollbackTransaction();
		return false;
	}

	return CommitTransaction();
}

bool FMonolithIndexDatabase::IsFullIndexInProgress() const
{
	return ReadMeta(MonolithIndexMetaKeys::FullIndexState) == MonolithIndexMetaKeys::FullIndexInProgress;
}

bool FMonolithIndexDatabase::CompleteFullIndex(const FString& UtcNow)
{
	if (!IsOpen()) return false;

	if (!BeginTransaction()) return false;

	if (!WriteMeta(MonolithIndexMetaKeys::LastFullIndex, UtcNow) || !DeleteMeta(MonolithIndexMetaKeys::FullIndexState))
	{
		RollbackTransaction();
		return false;
	}

	return CommitTransaction();
}

bool FMonolithIndexDatabase::SetDeepIndexedHash(int64 AssetId, const FString& Hash)
{
	if (!IsOpen()) return false;

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("UPDATE assets SET deep_indexed_hash = ? WHERE id = ?;"));
	Stmt.SetBindingValueByIndex(1, Hash);
	Stmt.SetBindingValueByIndex(2, AssetId);
	return Stmt.Execute();
}

bool FMonolithIndexDatabase::BumpDeepIndexAttempts(const TArray<int64>& AssetIds)
{
	if (!IsOpen()) return false;
	if (AssetIds.Num() == 0) return true;

	FSQLitePreparedStatement Stmt;
	if (!Stmt.Create(*Database, TEXT("UPDATE assets SET deep_index_attempts = deep_index_attempts + 1 WHERE id = ?;"),
		ESQLitePreparedStatementFlags::Persistent))
	{
		return false;
	}

	// Execute() resets the statement itself, so rebinding index 1 each time is
	// all that is needed to reuse it across the batch.
	bool bSuccess = true;
	for (const int64 AssetId : AssetIds)
	{
		Stmt.SetBindingValueByIndex(1, AssetId);
		bSuccess &= Stmt.Execute();
	}
	return bSuccess;
}

bool FMonolithIndexDatabase::ClearDeepIndexAttempts(int64 AssetId)
{
	if (!IsOpen()) return false;

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("UPDATE assets SET deep_index_attempts = 0 WHERE id = ?;"));
	Stmt.SetBindingValueByIndex(1, AssetId);
	return Stmt.Execute();
}

TArray<FString> FMonolithIndexDatabase::GetSkippedAssetPaths() const
{
	TArray<FString> Paths;
	const FString Raw = ReadMeta(MonolithIndexMetaKeys::SkippedAssets);
	if (!Raw.IsEmpty())
	{
		Raw.ParseIntoArray(Paths, TEXT("\n"), /*InCullEmpty=*/true);
	}
	return Paths;
}

bool FMonolithIndexDatabase::RecordSkippedAssetPaths(const TArray<FString>& Paths)
{
	if (!IsOpen()) return false;
	if (Paths.Num() == 0) return true;

	// Accumulate rather than replace: once an asset is skipped its hash is written
	// so it leaves the queue, and a later run would otherwise silently drop it
	// from the record while the data is still missing.
	TArray<FString> Merged = GetSkippedAssetPaths();
	for (const FString& Path : Paths)
	{
		Merged.AddUnique(Path);
	}

	return WriteMeta(MonolithIndexMetaKeys::SkippedAssets, FString::Join(Merged, TEXT("\n")));
}

// ============================================================
// Incremental indexing helpers
// ============================================================

TArray<FString> FMonolithIndexDatabase::GetAllIndexedPaths()
{
	TArray<FString> Result;
	if (!IsOpen()) return Result;

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("SELECT package_path FROM assets;"));

	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FString Path;
		Stmt.GetColumnValueByIndex(0, Path);
		Result.Add(MoveTemp(Path));
	}
	return Result;
}

FString FMonolithIndexDatabase::GetSavedHash(const FString& PackagePath)
{
	if (!IsOpen()) return FString();

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("SELECT saved_hash FROM assets WHERE package_path = ?;"));
	Stmt.SetBindingValueByIndex(1, PackagePath);

	if (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FString Hash;
		Stmt.GetColumnValueByIndex(0, Hash);
		return Hash;
	}
	return FString();
}

TMap<FString, FString> FMonolithIndexDatabase::GetAllPathsAndHashes()
{
	TMap<FString, FString> Result;
	if (!IsOpen()) return Result;

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("SELECT package_path, saved_hash FROM assets;"));

	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FString Path, Hash;
		Stmt.GetColumnValueByIndex(0, Path);
		Stmt.GetColumnValueByIndex(1, Hash);
		Result.Add(MoveTemp(Path), MoveTemp(Hash));
	}
	return Result;
}

bool FMonolithIndexDatabase::DeleteAssetByPath(const FString& PackagePath)
{
	if (!IsOpen()) return false;

	int64 AssetId = GetAssetId(PackagePath);
	if (AssetId < 0) return false;

	return DeleteAssetAndRelated(AssetId);
}

bool FMonolithIndexDatabase::UpdateAssetPath(const FString& OldPath, const FString& NewPath, const FString& NewAssetName)
{
	if (!IsOpen()) return false;

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("UPDATE assets SET package_path = ?, asset_name = COALESCE(NULLIF(?, ''), asset_name) WHERE package_path = ?;"));
	Stmt.SetBindingValueByIndex(1, NewPath);
	Stmt.SetBindingValueByIndex(2, NewAssetName);
	Stmt.SetBindingValueByIndex(3, OldPath);

	if (!Stmt.Execute()) return false;

	// Check if a row was actually updated
	// GetLastInsertRowId isn't useful for UPDATE; use changes count via a follow-up query
	FSQLitePreparedStatement ChangesStmt;
	ChangesStmt.Create(*Database, TEXT("SELECT changes();"));
	if (ChangesStmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		int64 Changes = 0;
		ChangesStmt.GetColumnValueByIndex(0, Changes);
		return Changes > 0;
	}
	return false;
}

bool FMonolithIndexDatabase::UpdateAssetMetadata(const FIndexedAsset& Asset)
{
	if (!IsOpen()) return false;

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("UPDATE assets SET asset_name = ?, asset_class = ?, module_name = ?, description = ?, file_size_bytes = ?, last_modified = ?, saved_hash = ?, indexed_at = datetime('now') WHERE package_path = ?;"));
	Stmt.SetBindingValueByIndex(1, Asset.AssetName);
	Stmt.SetBindingValueByIndex(2, Asset.AssetClass);
	Stmt.SetBindingValueByIndex(3, Asset.ModuleName);
	Stmt.SetBindingValueByIndex(4, Asset.Description);
	Stmt.SetBindingValueByIndex(5, Asset.FileSizeBytes);
	Stmt.SetBindingValueByIndex(6, Asset.LastModified);
	Stmt.SetBindingValueByIndex(7, Asset.SavedHash);
	Stmt.SetBindingValueByIndex(8, Asset.PackagePath);

	if (!Stmt.Execute()) return false;

	FSQLitePreparedStatement ChangesStmt;
	ChangesStmt.Create(*Database, TEXT("SELECT changes();"));
	if (ChangesStmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		int64 Changes = 0;
		ChangesStmt.GetColumnValueByIndex(0, Changes);
		return Changes > 0;
	}
	return false;
}

// Deletes per-asset child data that deep indexing repopulates (nodes, variables, parameters, datatable_rows).
// Does NOT delete: dependencies (DependencyIndexer sentinel), tag_references (GameplayTagIndexer sentinel),
// actors (LevelIndexer sentinel). Those are scoped separately.
bool FMonolithIndexDatabase::DeleteChildDataForAsset(int64 AssetId)
{
	if (!IsOpen()) return false;

	bool bSuccess = true;

	FSQLitePreparedStatement Stmt1;
	Stmt1.Create(*Database, TEXT("DELETE FROM nodes WHERE asset_id = ?;"));
	Stmt1.SetBindingValueByIndex(1, AssetId);
	bSuccess &= Stmt1.Execute();

	FSQLitePreparedStatement Stmt2;
	Stmt2.Create(*Database, TEXT("DELETE FROM variables WHERE asset_id = ?;"));
	Stmt2.SetBindingValueByIndex(1, AssetId);
	bSuccess &= Stmt2.Execute();

	FSQLitePreparedStatement Stmt3;
	Stmt3.Create(*Database, TEXT("DELETE FROM parameters WHERE asset_id = ?;"));
	Stmt3.SetBindingValueByIndex(1, AssetId);
	bSuccess &= Stmt3.Execute();

	FSQLitePreparedStatement Stmt4;
	Stmt4.Create(*Database, TEXT("DELETE FROM datatable_rows WHERE asset_id = ?;"));
	Stmt4.SetBindingValueByIndex(1, AssetId);
	bSuccess &= Stmt4.Execute();

	return bSuccess;
}

bool FMonolithIndexDatabase::UpdateSavedHash(const FString& PackagePath, const FString& HashHex)
{
	if (!IsOpen()) return false;

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("UPDATE assets SET saved_hash = ? WHERE package_path = ?;"));
	Stmt.SetBindingValueByIndex(1, HashHex);
	Stmt.SetBindingValueByIndex(2, PackagePath);

	if (!Stmt.Execute()) return false;

	FSQLitePreparedStatement ChangesStmt;
	ChangesStmt.Create(*Database, TEXT("SELECT changes();"));
	if (ChangesStmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		int64 Changes = 0;
		ChangesStmt.GetColumnValueByIndex(0, Changes);
		return Changes > 0;
	}
	return false;
}

// ============================================================
// FTS5 Full-text search
// ============================================================

// Named (never anonymous) so the forced-full-unity release pass cannot collide
// these helpers with same-named file-locals in a sibling translation unit.
namespace MonolithProjectSearchDetail
{
	/**
	 * Diagnostics emitted by the FTS5 MATCH expression parser rather than by
	 * storage/schema access. Verified against SQLite: unbalanced parentheses,
	 * a trailing operator, `NEAR/3` (not FTS5 syntax) and a bare `:` all report
	 * "fts5: syntax error near ..."; `"unterminated`, `*bogus` and a non-decimal
	 * NEAR distance report the other three. Everything else stays internal.
	 */
	static bool IsFts5QuerySyntaxError(const FString& Error)
	{
		return Error.Contains(TEXT("fts5: syntax error"), ESearchCase::IgnoreCase)
			|| Error.Contains(TEXT("unterminated string"), ESearchCase::IgnoreCase)
			|| Error.Contains(TEXT("malformed MATCH"), ESearchCase::IgnoreCase)
			|| Error.Contains(TEXT("unknown special query"), ESearchCase::IgnoreCase)
			|| Error.Contains(TEXT("expected integer, got"), ESearchCase::IgnoreCase);
	}

	/**
	 * A column filter naming a column this FTS table does not carry. The two
	 * project FTS tables expose different columns, so on its own this means
	 * "not answerable here", not "bad query" — `node_name:Branch` is a valid
	 * search that only fts_nodes can serve. Only a rejection by BOTH tables
	 * makes it a caller error.
	 */
	static bool IsFts5UnknownColumnError(const FString& Error)
	{
		return Error.Contains(TEXT("no such column"), ESearchCase::IgnoreCase);
	}

	// Mirrors the CREATE VIRTUAL TABLE column lists in the schema DDL above
	// (fts_assets / fts_nodes). Keep in step with them.
	static const TCHAR* const AssetFtsColumns = TEXT("asset_name, asset_class, description, package_path, module_name");
	static const TCHAR* const NodeFtsColumns = TEXT("node_name, node_class, node_type");

	/**
	 * Turn SQLite's bare "no such column: node_nme" into something a caller can act
	 * on. A typo'd column is the common case, so name it and list the valid ones —
	 * that is the difference between a five-second fix and a filed issue.
	 */
	static FString DescribeUnknownColumn(const FString& Error)
	{
		static const FString Marker(TEXT("no such column:"));
		const int32 MarkerIndex = Error.Find(Marker, ESearchCase::IgnoreCase, ESearchDir::FromStart);

		FString Subject = Error;
		if (MarkerIndex != INDEX_NONE)
		{
			FString ColumnName = Error.Mid(MarkerIndex + Marker.Len());
			ColumnName.TrimStartAndEndInline();
			if (!ColumnName.IsEmpty())
			{
				Subject = FString::Printf(TEXT("no such column '%s'"), *ColumnName);
			}
		}

		return FString::Printf(
			TEXT("%s. Valid columns are %s (assets) or %s (nodes); one filter cannot span both tables."),
			*Subject,
			AssetFtsColumns,
			NodeFtsColumns);
	}
}

TArray<FSearchResult> FMonolithIndexDatabase::FullTextSearch(const FString& Query, int32 Limit)
{
	TArray<FSearchResult> Results;
	FString SearchError;
	if (FullTextSearch(Query, Limit, Results, SearchError) != EMonolithProjectSearchStatus::Succeeded)
	{
		UE_LOG(LogMonolithIndex, Error, TEXT("Project search failed: %s"), *SearchError);
	}
	return Results;
}

EMonolithProjectSearchStatus FMonolithIndexDatabase::FullTextSearch(
	const FString& Query,
	int32 Limit,
	TArray<FSearchResult>& OutResults,
	FString& OutError,
	const TArray<FString>& AssetClassFilter)
{
	OutResults.Reset();
	OutError.Reset();

	if (!IsOpen())
	{
		OutError = TEXT("Project index database is not open");
		return EMonolithProjectSearchStatus::InternalError;
	}

	const int32 ClampedLimit = FMath::Clamp(Limit, 1, 1000);

	// Only the placeholder COUNT is interpolated; every class name is still bound as a
	// parameter, so this does not build SQL out of caller-supplied text. The count comes
	// from the array length, never from the strings themselves.
	FString ClassPredicate;
	if (AssetClassFilter.Num() > 0)
	{
		FString Placeholders;
		for (int32 Index = 0; Index < AssetClassFilter.Num(); ++Index)
		{
			Placeholders += (Index == 0) ? TEXT("?") : TEXT(",?");
		}
		// COLLATE binds tighter than IN, so this compares a.asset_class case-insensitively
		// against every bound name -- a caller passing "blueprint" matches "Blueprint".
		ClassPredicate = FString::Printf(TEXT(" AND a.asset_class COLLATE NOCASE IN (%s)"), *Placeholders);
	}

	// One table's verdict on the query. NotApplicable is not yet a failure: the
	// sibling table gets its turn before an unknown column becomes a caller error.
	enum class EAttempt : uint8
	{
		Completed,
		NotApplicable,
		InvalidQuery,
		InternalError
	};

	auto RunSearch = [this, &OutResults, &Query, ClampedLimit, &AssetClassFilter](
		const TCHAR* SQL,
		const TCHAR* TableName,
		FString& AttemptError) -> EAttempt
	{
		AttemptError.Reset();

		FSQLitePreparedStatement Stmt;
		if (!Stmt.Create(*Database, SQL))
		{
			AttemptError = FString::Printf(
				TEXT("Failed to prepare the %s FTS query: %s"), TableName, *Database->GetLastError());
			return EAttempt::InternalError;
		}
		if (!Stmt.SetBindingValueByIndex(1, Query))
		{
			AttemptError = FString::Printf(TEXT("Failed to bind the %s FTS query"), TableName);
			return EAttempt::InternalError;
		}

		// The class filters occupy 2..N+1, so the limit's index moves with the filter
		// count rather than staying hardcoded at 2.
		int32 BindIndex = 2;
		for (const FString& ClassName : AssetClassFilter)
		{
			if (!Stmt.SetBindingValueByIndex(BindIndex, ClassName))
			{
				AttemptError = FString::Printf(
					TEXT("Failed to bind the %s FTS asset_class filter"), TableName);
				return EAttempt::InternalError;
			}
			++BindIndex;
		}

		if (!Stmt.SetBindingValueByIndex(BindIndex, ClampedLimit))
		{
			AttemptError = FString::Printf(TEXT("Failed to bind the %s FTS result limit"), TableName);
			return EAttempt::InternalError;
		}

		// Gather into a local array so a mid-enumeration failure contributes
		// no partial rows to the caller's result set.
		TArray<FSearchResult> TableResults;
		for (;;)
		{
			const ESQLitePreparedStatementStepResult StepResult = Stmt.Step();
			if (StepResult == ESQLitePreparedStatementStepResult::Done)
			{
				break;
			}
			if (StepResult != ESQLitePreparedStatementStepResult::Row)
			{
				// Anything that is neither Row nor Done is a real failure. The old
				// `while (Step() == Row)` loop treated it as end-of-results, which is
				// how query errors used to masquerade as zero matches.
				const FString DatabaseError = Database->GetLastError();
				if (MonolithProjectSearchDetail::IsFts5UnknownColumnError(DatabaseError))
				{
					AttemptError = DatabaseError;
					return EAttempt::NotApplicable;
				}
				if (MonolithProjectSearchDetail::IsFts5QuerySyntaxError(DatabaseError))
				{
					AttemptError = DatabaseError;
					return EAttempt::InvalidQuery;
				}
				AttemptError = FString::Printf(
					TEXT("%s FTS query failed: %s"),
					TableName,
					DatabaseError.IsEmpty() ? TEXT("database operation failed") : *DatabaseError);
				return EAttempt::InternalError;
			}

			FSearchResult R;
			Stmt.GetColumnValueByIndex(0, R.AssetPath);
			Stmt.GetColumnValueByIndex(1, R.AssetName);
			Stmt.GetColumnValueByIndex(2, R.AssetClass);
			Stmt.GetColumnValueByIndex(3, R.ModuleName);
			Stmt.GetColumnValueByIndex(4, R.MatchContext);
			double RankD = 0.0;
			Stmt.GetColumnValueByIndex(5, RankD);
			R.Rank = static_cast<float>(RankD);
			TableResults.Add(MoveTemp(R));
		}

		OutResults.Append(MoveTemp(TableResults));
		return EAttempt::Completed;
	};

	auto ToStatus = [](EAttempt Attempt)
	{
		return Attempt == EAttempt::InvalidQuery
			? EMonolithProjectSearchStatus::InvalidQuery
			: EMonolithProjectSearchStatus::InternalError;
	};

	// Search assets FTS. The statement already JOINs assets, so the class predicate needs
	// no extra join -- it slots in beside the MATCH.
	const FString AssetSQL = FString::Printf(
		TEXT("SELECT a.package_path, a.asset_name, a.asset_class, a.module_name, snippet(fts_assets, 2, '>>>', '<<<', '...', 32) as ctx, rank FROM fts_assets f JOIN assets a ON a.id = f.rowid WHERE fts_assets MATCH ?%s ORDER BY rank LIMIT ?;"),
		*ClassPredicate);

	FString AssetError;
	const EAttempt AssetAttempt = RunSearch(*AssetSQL, TEXT("assets"), AssetError);
	if (AssetAttempt == EAttempt::InvalidQuery || AssetAttempt == EAttempt::InternalError)
	{
		OutResults.Reset();
		OutError = AssetError;
		return ToStatus(AssetAttempt);
	}

	// Also search nodes FTS. The class filter applies to the OWNING asset, so a node-text
	// hit inside a Blueprint still survives a "Blueprint" filter.
	const FString NodeSQL = FString::Printf(
		TEXT("SELECT a.package_path, a.asset_name, a.asset_class, a.module_name, snippet(fts_nodes, 0, '>>>', '<<<', '...', 32) as ctx, f.rank FROM fts_nodes f JOIN nodes n ON n.id = f.rowid JOIN assets a ON a.id = n.asset_id WHERE fts_nodes MATCH ?%s ORDER BY f.rank LIMIT ?;"),
		*ClassPredicate);

	FString NodeError;
	const EAttempt NodeAttempt = RunSearch(*NodeSQL, TEXT("nodes"), NodeError);
	if (NodeAttempt == EAttempt::InvalidQuery || NodeAttempt == EAttempt::InternalError)
	{
		OutResults.Reset();
		OutError = NodeError;
		return ToStatus(NodeAttempt);
	}

	// No FTS table exposes the requested column, so the caller named one that
	// does not exist anywhere in the index.
	if (AssetAttempt == EAttempt::NotApplicable && NodeAttempt == EAttempt::NotApplicable)
	{
		OutResults.Reset();
		OutError = MonolithProjectSearchDetail::DescribeUnknownColumn(AssetError);
		return EMonolithProjectSearchStatus::InvalidQuery;
	}

	// Sort combined results by rank (lower = better in FTS5)
	OutResults.Sort([](const FSearchResult& A, const FSearchResult& B) { return A.Rank < B.Rank; });

	if (OutResults.Num() > ClampedLimit)
	{
		OutResults.SetNum(ClampedLimit);
	}

	return EMonolithProjectSearchStatus::Succeeded;
}

// ============================================================
// Stats
// ============================================================

TSharedPtr<FJsonObject> FMonolithIndexDatabase::GetStats()
{
	auto Stats = MakeShared<FJsonObject>();
	if (!IsOpen()) return Stats;

	auto GetCount = [this](const TCHAR* Table) -> int64
	{
		FSQLitePreparedStatement Stmt;
		FString SQL = FString::Printf(TEXT("SELECT COUNT(*) FROM %s;"), Table);
		Stmt.Create(*Database, *SQL);
		if (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
		{
			int64 Count = 0;
			Stmt.GetColumnValueByIndex(0, Count);
			return Count;
		}
		return 0;
	};

	Stats->SetNumberField(TEXT("assets"), GetCount(TEXT("assets")));
	Stats->SetNumberField(TEXT("nodes"), GetCount(TEXT("nodes")));
	Stats->SetNumberField(TEXT("connections"), GetCount(TEXT("connections")));
	Stats->SetNumberField(TEXT("variables"), GetCount(TEXT("variables")));
	Stats->SetNumberField(TEXT("parameters"), GetCount(TEXT("parameters")));
	Stats->SetNumberField(TEXT("dependencies"), GetCount(TEXT("dependencies")));
	Stats->SetNumberField(TEXT("actors"), GetCount(TEXT("actors")));
	Stats->SetNumberField(TEXT("tags"), GetCount(TEXT("tags")));
	Stats->SetNumberField(TEXT("configs"), GetCount(TEXT("configs")));
	Stats->SetNumberField(TEXT("cpp_symbols"), GetCount(TEXT("cpp_symbols")));
	Stats->SetNumberField(TEXT("datatable_rows"), GetCount(TEXT("datatable_rows")));

	// Asset class breakdown
	auto ClassBreakdown = MakeShared<FJsonObject>();
	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("SELECT asset_class, COUNT(*) as cnt FROM assets GROUP BY asset_class ORDER BY cnt DESC LIMIT 20;"));
	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FString ClassName;
		int64 Count = 0;
		Stmt.GetColumnValueByIndex(0, ClassName);
		Stmt.GetColumnValueByIndex(1, Count);
		ClassBreakdown->SetNumberField(ClassName, Count);
	}
	Stats->SetObjectField(TEXT("asset_class_breakdown"), ClassBreakdown);

	// Module breakdown (which plugins have how many assets)
	auto ModuleBreakdown = MakeShared<FJsonObject>();
	FSQLitePreparedStatement ModStmt;
	ModStmt.Create(*Database, TEXT("SELECT CASE WHEN module_name = '' THEN 'Project' ELSE module_name END as mod, COUNT(*) as cnt FROM assets GROUP BY module_name ORDER BY cnt DESC;"));
	while (ModStmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FString ModName;
		int64 Count = 0;
		ModStmt.GetColumnValueByIndex(0, ModName);
		ModStmt.GetColumnValueByIndex(1, Count);
		ModuleBreakdown->SetNumberField(ModName, Count);
	}
	Stats->SetObjectField(TEXT("module_breakdown"), ModuleBreakdown);

	// Assets the poison-pill rule dropped from deep indexing. Surfaced here so an
	// agent can see the data-completeness gap without reading the editor log —
	// `monolith_reindex(force=true)` (or `Monolith.StartIndex force`) clears it.
	const TArray<FString> SkippedPaths = GetSkippedAssetPaths();
	Stats->SetNumberField(TEXT("skipped_assets"), SkippedPaths.Num());
	if (SkippedPaths.Num() > 0)
	{
		constexpr int32 MaxReportedSkips = 50;
		TArray<TSharedPtr<FJsonValue>> SkipValues;
		for (int32 i = 0; i < FMath::Min(SkippedPaths.Num(), MaxReportedSkips); ++i)
		{
			SkipValues.Add(MakeShared<FJsonValueString>(SkippedPaths[i]));
		}
		Stats->SetArrayField(TEXT("skipped_asset_paths"), SkipValues);
	}

	return Stats;
}

// ============================================================
// Asset details
// ============================================================

TSharedPtr<FJsonObject> FMonolithIndexDatabase::GetAssetDetails(const FString& PackagePath)
{
	auto Details = MakeShared<FJsonObject>();
	if (!IsOpen()) return Details;

	auto MaybeAsset = GetAssetByPath(PackagePath);
	if (!MaybeAsset.IsSet()) return Details;

	const FIndexedAsset& Asset = MaybeAsset.GetValue();
	Details->SetStringField(TEXT("package_path"), Asset.PackagePath);
	Details->SetStringField(TEXT("asset_name"), Asset.AssetName);
	Details->SetStringField(TEXT("asset_class"), Asset.AssetClass);
	Details->SetStringField(TEXT("module_name"), Asset.ModuleName);
	Details->SetStringField(TEXT("description"), Asset.Description);
	Details->SetNumberField(TEXT("file_size_bytes"), Asset.FileSizeBytes);
	Details->SetStringField(TEXT("last_modified"), Asset.LastModified);
	Details->SetStringField(TEXT("indexed_at"), Asset.IndexedAt);

	// Nodes
	TArray<TSharedPtr<FJsonValue>> NodesArr;
	for (const auto& Node : GetNodesForAsset(Asset.Id))
	{
		auto NodeObj = MakeShared<FJsonObject>();
		NodeObj->SetStringField(TEXT("node_type"), Node.NodeType);
		NodeObj->SetStringField(TEXT("node_name"), Node.NodeName);
		NodeObj->SetStringField(TEXT("node_class"), Node.NodeClass);

		// Include stored properties (type-specific metadata from indexers)
		if (!Node.Properties.IsEmpty() && Node.Properties != TEXT("{}"))
		{
			TSharedPtr<FJsonObject> PropsObj;
			auto Reader = TJsonReaderFactory<>::Create(Node.Properties);
			if (FJsonSerializer::Deserialize(Reader, PropsObj) && PropsObj.IsValid() && PropsObj->Values.Num() > 0)
			{
				NodeObj->SetObjectField(TEXT("properties"), PropsObj);
			}
		}

		NodesArr.Add(MakeShared<FJsonValueObject>(NodeObj));
	}
	Details->SetArrayField(TEXT("nodes"), NodesArr);

	// Variables
	TArray<TSharedPtr<FJsonValue>> VarsArr;
	for (const auto& Var : GetVariablesForAsset(Asset.Id))
	{
		auto VarObj = MakeShared<FJsonObject>();
		VarObj->SetStringField(TEXT("name"), Var.VarName);
		VarObj->SetStringField(TEXT("type"), Var.VarType);
		VarObj->SetStringField(TEXT("category"), Var.Category);
		VarObj->SetBoolField(TEXT("exposed"), Var.bIsExposed);
		VarsArr.Add(MakeShared<FJsonValueObject>(VarObj));
	}
	Details->SetArrayField(TEXT("variables"), VarsArr);

	// Dependencies — wrap in safety check to prevent invalid JSON propagation
	auto Refs = FindReferences(PackagePath);
	if (Refs.IsValid())
	{
		// Validate the references object can serialize cleanly
		FString SerializedRefs;
		auto Writer = TJsonWriterFactory<>::Create(&SerializedRefs);
		if (FJsonSerializer::Serialize(Refs.ToSharedRef(), Writer))
		{
			Details->SetObjectField(TEXT("references"), Refs);
		}
		else
		{
			// Fallback: provide empty references rather than invalid JSON
			auto EmptyRefs = MakeShared<FJsonObject>();
			EmptyRefs->SetArrayField(TEXT("depends_on"), TArray<TSharedPtr<FJsonValue>>());
			EmptyRefs->SetArrayField(TEXT("referenced_by"), TArray<TSharedPtr<FJsonValue>>());
			Details->SetObjectField(TEXT("references"), EmptyRefs);
		}
	}

	return Details;
}

// ============================================================
// Find by type
// ============================================================

TArray<FIndexedAsset> FMonolithIndexDatabase::FindByType(const FString& AssetClass, int32 Limit, int32 Offset)
{
	TArray<FIndexedAsset> Result;
	if (!IsOpen()) return Result;

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("SELECT id, package_path, asset_name, asset_class, module_name, description, file_size_bytes, last_modified, saved_hash, indexed_at FROM assets WHERE asset_class = ? LIMIT ? OFFSET ?;"));
	Stmt.SetBindingValueByIndex(1, AssetClass);
	Stmt.SetBindingValueByIndex(2, static_cast<int64>(Limit));
	Stmt.SetBindingValueByIndex(3, static_cast<int64>(Offset));

	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FIndexedAsset Asset;
		Stmt.GetColumnValueByIndex(0, Asset.Id);
		Stmt.GetColumnValueByIndex(1, Asset.PackagePath);
		Stmt.GetColumnValueByIndex(2, Asset.AssetName);
		Stmt.GetColumnValueByIndex(3, Asset.AssetClass);
		Stmt.GetColumnValueByIndex(4, Asset.ModuleName);
		Stmt.GetColumnValueByIndex(5, Asset.Description);
		Stmt.GetColumnValueByIndex(6, Asset.FileSizeBytes);
		Stmt.GetColumnValueByIndex(7, Asset.LastModified);
		Stmt.GetColumnValueByIndex(8, Asset.SavedHash);
		Stmt.GetColumnValueByIndex(9, Asset.IndexedAt);
		Result.Add(MoveTemp(Asset));
	}
	return Result;
}

// ============================================================
// Find references (bidirectional dependency lookup)
// ============================================================

TSharedPtr<FJsonObject> FMonolithIndexDatabase::FindReferences(const FString& PackagePath)
{
	auto Result = MakeShared<FJsonObject>();
	if (!IsOpen()) return Result;

	int64 AssetId = GetAssetId(PackagePath);
	if (AssetId < 0) return Result;

	// What this asset depends on
	TArray<TSharedPtr<FJsonValue>> DepsArr;
	for (const auto& Dep : GetDependenciesForAsset(AssetId))
	{
		FSQLitePreparedStatement Stmt;
		Stmt.Create(*Database, TEXT("SELECT package_path, asset_class FROM assets WHERE id = ?;"));
		Stmt.SetBindingValueByIndex(1, Dep.TargetAssetId);
		if (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
		{
			auto DepObj = MakeShared<FJsonObject>();
			FString Path, Class;
			Stmt.GetColumnValueByIndex(0, Path);
			Stmt.GetColumnValueByIndex(1, Class);
			DepObj->SetStringField(TEXT("path"), Path);
			DepObj->SetStringField(TEXT("class"), Class);
			DepObj->SetStringField(TEXT("type"), Dep.DependencyType);
			DepsArr.Add(MakeShared<FJsonValueObject>(DepObj));
		}
	}
	Result->SetArrayField(TEXT("depends_on"), DepsArr);

	// What references this asset
	TArray<TSharedPtr<FJsonValue>> RefsArr;
	for (const auto& Ref : GetReferencersOfAsset(AssetId))
	{
		FSQLitePreparedStatement Stmt;
		Stmt.Create(*Database, TEXT("SELECT package_path, asset_class FROM assets WHERE id = ?;"));
		Stmt.SetBindingValueByIndex(1, Ref.SourceAssetId);
		if (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
		{
			auto RefObj = MakeShared<FJsonObject>();
			FString Path, Class;
			Stmt.GetColumnValueByIndex(0, Path);
			Stmt.GetColumnValueByIndex(1, Class);
			RefObj->SetStringField(TEXT("path"), Path);
			RefObj->SetStringField(TEXT("class"), Class);
			RefObj->SetStringField(TEXT("type"), Ref.DependencyType);
			RefsArr.Add(MakeShared<FJsonValueObject>(RefObj));
		}
	}
	Result->SetArrayField(TEXT("referenced_by"), RefsArr);

	return Result;
}

// ============================================================
// Internal helpers
// ============================================================

bool FMonolithIndexDatabase::CreateTables()
{
	if (!Database || !Database->IsValid())
	{
		return false;
	}

	// GCreateTablesSQL contains multiple statements separated by semicolons.
	// FSQLiteDatabase::Execute() only handles one statement at a time,
	// so we split and execute each individually.
	FString FullSQL(GCreateTablesSQL);
	TArray<FString> Statements;

	// Split on semicolons, tracking BEGIN/END depth for trigger bodies
	int32 Start = 0;
	int32 Depth = 0;
	for (int32 i = 0; i < FullSQL.Len(); ++i)
	{
		// Check for BEGIN keyword (trigger body start)
		if (i + 5 <= FullSQL.Len())
		{
			FString Word = FullSQL.Mid(i, 5).ToUpper();
			if (Word == TEXT("BEGIN") && (i == 0 || FChar::IsWhitespace(FullSQL[i - 1]) || FullSQL[i - 1] == '\n'))
			{
				if (i + 5 >= FullSQL.Len() || FChar::IsWhitespace(FullSQL[i + 5]) || FullSQL[i + 5] == '\n')
				{
					Depth++;
				}
			}
		}
		// Check for END keyword (trigger body end)
		if (i + 3 <= FullSQL.Len())
		{
			FString Word = FullSQL.Mid(i, 3).ToUpper();
			if (Word == TEXT("END") && (i == 0 || FChar::IsWhitespace(FullSQL[i - 1]) || FullSQL[i - 1] == '\n'))
			{
				if (i + 3 >= FullSQL.Len() || FullSQL[i + 3] == ';' || FChar::IsWhitespace(FullSQL[i + 3]))
				{
					if (Depth > 0) Depth--;
				}
			}
		}

		if (FullSQL[i] == ';' && Depth == 0)
		{
			FString Stmt = FullSQL.Mid(Start, i - Start + 1).TrimStartAndEnd();
			if (!Stmt.IsEmpty() && Stmt != TEXT(";"))
			{
				Statements.Add(Stmt);
			}
			Start = i + 1;
		}
	}

	bool bAllSucceeded = true;
	for (const FString& Stmt : Statements)
	{
		if (!Database->Execute(*Stmt))
		{
			UE_LOG(LogMonolithIndex, Warning, TEXT("Schema statement failed: %s -- Error: %s"),
				*Stmt.Left(100), *Database->GetLastError());
			bAllSucceeded = false;
			// Don't stop -- try remaining statements (some may be IF NOT EXISTS)
		}
	}

	if (!bAllSucceeded)
	{
		UE_LOG(LogMonolithIndex, Warning, TEXT("Some schema statements failed -- FTS5 may not be available in this SQLite build"));
	}

	return true; // Return true even if FTS fails -- basic tables should work
}

bool FMonolithIndexDatabase::ExecuteSQL(const FString& SQL)
{
	if (!Database || !Database->IsValid())
	{
		UE_LOG(LogMonolithIndex, Error, TEXT("Cannot execute SQL -- database not open"));
		return false;
	}

	if (!Database->Execute(*SQL))
	{
		UE_LOG(LogMonolithIndex, Error, TEXT("SQL execution failed: %s"), *Database->GetLastError());
		return false;
	}
	return true;
}
