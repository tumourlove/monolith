#include "MonolithSourceDatabase.h"
#include "GenericPlatform/GenericPlatformFile.h"
#include "MonolithSourceSchema.h"
#include "SQLiteDatabase.h"
#include "Misc/Paths.h"
#include "HAL/PlatformFileManager.h"

DEFINE_LOG_CATEGORY(LogMonolithSource);

// ============================================================
// Helper: execute a multi-statement SQL string statement-by-statement.
// FSQLiteDatabase::Execute() only runs the first statement when given
// a semicolon-separated multi-statement string, so we must split manually.
//
// Splits on ';' at BEGIN/END nesting depth 0, so trigger bodies like
//   BEGIN INSERT INTO ...; END;
// are kept intact as a single statement.
// ============================================================
static bool ExecuteMulti(FSQLiteDatabase& DB, const TCHAR* SQL)
{
	const FString Source(SQL);
	const int32 Len = Source.Len();

	int32 Depth = 0;   // BEGIN...END nesting depth
	FString Current;

	auto FlushStatement = [&]() -> bool
	{
		FString Stmt = Current.TrimStartAndEnd();
		Current.Empty();
		if (Stmt.IsEmpty())
		{
			return true;
		}
		return DB.Execute(*Stmt);
	};

	int32 i = 0;
	while (i < Len)
	{
		const TCHAR Ch = Source[i];

		// Detect SQL keywords (BEGIN / END) at word boundaries.
		// String literals are not present in our DDL so we skip quote handling.
		if (FChar::IsAlpha(Ch) || Ch == TEXT('_'))
		{
			const int32 WordStart = i;
			while (i < Len && (FChar::IsAlnum(Source[i]) || Source[i] == TEXT('_')))
			{
				++i;
			}
			const FString Word = Source.Mid(WordStart, i - WordStart).ToUpper();
			Current += Source.Mid(WordStart, i - WordStart);

			if (Word == TEXT("BEGIN"))
			{
				++Depth;
			}
			else if (Word == TEXT("END") && Depth > 0)
			{
				--Depth;
			}
			continue;
		}

		if (Ch == TEXT(';') && Depth == 0)
		{
			++i;
			if (!FlushStatement())
			{
				return false;
			}
			continue;
		}

		Current += Ch;
		++i;
	}

	// Flush any trailing statement (no trailing semicolon)
	return FlushStatement();
}

// ============================================================
// Constructor / Destructor
// ============================================================

FMonolithSourceDatabase::FMonolithSourceDatabase()
{
}

FMonolithSourceDatabase::~FMonolithSourceDatabase()
{
	Close();
}

bool FMonolithSourceDatabase::Open(const FString& DbPath)
{
	FScopeLock Lock(&DbLock);

	if (Database)
	{
		Close();
	}

	CachedDbPath = DbPath;

	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	if (!PlatformFile.FileExists(*DbPath))
	{
		UE_LOG(LogMonolithSource, Warning, TEXT("Engine source DB not found: %s"), *DbPath);
		return false;
	}

	Database = new FSQLiteDatabase();
	if (!Database->Open(*DbPath, ESQLiteDatabaseOpenMode::ReadWrite))
	{
		UE_LOG(LogMonolithSource, Error, TEXT("Failed to open engine source DB: %s"), *DbPath);
		delete Database;
		Database = nullptr;
		return false;
	}

	// Force DELETE journal mode on EVERY open. This keeps the file free of
	// -wal/-shm sidecars at rest — tidy, and defensive idempotency in case a
	// prior session crashed mid-WAL or another tool flipped it back.
	//
	// NOTE (corrected 2026-05-29): an earlier theory blamed Reflection
	// Intelligence's "disk I/O error" on WAL+ReadOnly cross-process replay. That
	// was WRONG — this DB is in DELETE mode (no WAL involved) and the failure was
	// SAME-process, not cross-process. The real cause was a second open of this
	// same file by MonolithReflectionIntel: UE 5.7's custom `unreal-fs` SQLite VFS
	// permits only ONE open of a file per process and reserves a write handle even
	// for a "ReadOnly" open, so the second open returned SQLITE_IOERR. The fix is
	// to STOP the second open — RI now borrows this handle via GetRawHandle().
	// The journal_mode flip remains for hygiene only; it is no longer load-bearing
	// for RI access.
	if (!Database->Execute(TEXT("PRAGMA journal_mode=DELETE;")))
	{
		UE_LOG(LogMonolithSource, Warning,
			TEXT("Open: PRAGMA journal_mode=DELETE failed on '%s' (continuing — hygiene only)"),
			*DbPath);
	}

	UE_LOG(LogMonolithSource, Log, TEXT("Engine source DB opened: %s"), *DbPath);
	return true;
}

void FMonolithSourceDatabase::Close()
{
	FScopeLock Lock(&DbLock);
	if (Database)
	{
		Database->Close();
		delete Database;
		Database = nullptr;
	}
}

bool FMonolithSourceDatabase::IsOpen() const
{
	FScopeLock Lock(&DbLock);
	return Database != nullptr && Database->IsValid();
}

FSQLiteDatabase* FMonolithSourceDatabase::GetRawHandle() const
{
	// Deliberately NOT locked here: the borrower (MonolithReflectionIntel) holds
	// GetLock() across the whole fetch-prepare-step sequence, so taking DbLock
	// here too would either deadlock (non-recursive FCriticalSection) or give a
	// false sense of safety for a pointer that outlives this call. Return the
	// raw pointer only when the handle is genuinely open; the borrower locks.
	return (Database != nullptr && Database->IsValid()) ? Database : nullptr;
}

// ============================================================
// FTS escape — mirrors Python _escape_fts()
// ============================================================

FString FMonolithSourceDatabase::EscapeFTS(const FString& Query)
{
	// Replace :: with space
	FString Q = Query.Replace(TEXT("::"), TEXT(" "));

	// Strip non-alphanumeric/non-space
	FString Cleaned;
	for (TCHAR Ch : Q)
	{
		if (FChar::IsAlnum(Ch) || Ch == TEXT('_') || Ch == TEXT(' '))
		{
			Cleaned += Ch;
		}
	}

	// Split into tokens, wrap each with quotes and trailing *
	TArray<FString> Tokens;
	Cleaned.ParseIntoArray(Tokens, TEXT(" "), true);

	if (Tokens.Num() == 0)
	{
		return TEXT("\"\"");
	}

	FString Result;
	for (int32 i = 0; i < Tokens.Num(); ++i)
	{
		if (i > 0) Result += TEXT(" ");
		Result += FString::Printf(TEXT("\"%s\"*"), *Tokens[i]);
	}
	return Result;
}

// ============================================================
// Row readers
// ============================================================

FMonolithSourceSymbol FMonolithSourceDatabase::ReadSymbolFromStatement(FSQLitePreparedStatement& Stmt)
{
	FMonolithSourceSymbol Sym;
	Stmt.GetColumnValueByIndex(0, Sym.Id);
	Stmt.GetColumnValueByIndex(1, Sym.Name);
	Stmt.GetColumnValueByIndex(2, Sym.QualifiedName);
	Stmt.GetColumnValueByIndex(3, Sym.Kind);
	Stmt.GetColumnValueByIndex(4, Sym.FileId);
	int32 LineStart = 0, LineEnd = 0;
	Stmt.GetColumnValueByIndex(5, LineStart);
	Stmt.GetColumnValueByIndex(6, LineEnd);
	Sym.LineStart = LineStart;
	Sym.LineEnd = LineEnd;
	// parent_symbol_id at index 7 — skip
	Stmt.GetColumnValueByIndex(8, Sym.Access);
	Stmt.GetColumnValueByIndex(9, Sym.Signature);
	Stmt.GetColumnValueByIndex(10, Sym.Docstring);
	int32 IsUEMacro = 0;
	Stmt.GetColumnValueByIndex(11, IsUEMacro);
	Sym.bIsUEMacro = IsUEMacro != 0;
	return Sym;
}

FMonolithSourceReference FMonolithSourceDatabase::ReadReferenceFromStatement(FSQLitePreparedStatement& Stmt, bool bIsRefTo)
{
	FMonolithSourceReference Ref;
	Stmt.GetColumnValueByIndex(0, Ref.Id);
	Stmt.GetColumnValueByIndex(1, Ref.FromSymbolId);
	Stmt.GetColumnValueByIndex(2, Ref.ToSymbolId);
	Stmt.GetColumnValueByIndex(3, Ref.RefKind);
	Stmt.GetColumnValueByIndex(4, Ref.FileId);
	int32 Line = 0;
	Stmt.GetColumnValueByIndex(5, Line);
	Ref.Line = Line;
	if (bIsRefTo)
	{
		Stmt.GetColumnValueByIndex(6, Ref.FromName);
	}
	else
	{
		Stmt.GetColumnValueByIndex(6, Ref.ToName);
	}
	Stmt.GetColumnValueByIndex(7, Ref.Path);
	return Ref;
}

// ============================================================
// Symbol queries
// ============================================================

TArray<FMonolithSourceSymbol> FMonolithSourceDatabase::GetSymbolsByName(const FString& Name, const FString& Kind)
{
	FScopeLock Lock(&DbLock);
	TArray<FMonolithSourceSymbol> Result;
	if (!Database || !Database->IsValid()) return Result;

	FSQLitePreparedStatement Stmt;
	if (Kind.IsEmpty())
	{
		Stmt.Create(*Database, TEXT("SELECT id, name, qualified_name, kind, file_id, line_start, line_end, parent_symbol_id, access, signature, docstring, is_ue_macro FROM symbols WHERE name = ? ORDER BY (line_end > line_start) DESC;"));
		Stmt.SetBindingValueByIndex(1, Name);
	}
	else
	{
		Stmt.Create(*Database, TEXT("SELECT id, name, qualified_name, kind, file_id, line_start, line_end, parent_symbol_id, access, signature, docstring, is_ue_macro FROM symbols WHERE name = ? AND kind = ? ORDER BY (line_end > line_start) DESC;"));
		Stmt.SetBindingValueByIndex(1, Name);
		Stmt.SetBindingValueByIndex(2, Kind);
	}

	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		Result.Add(ReadSymbolFromStatement(Stmt));
	}
	return Result;
}

TArray<FMonolithSourceSymbol> FMonolithSourceDatabase::SearchSymbolsFTS(const FString& Query, int32 Limit)
{
	FScopeLock Lock(&DbLock);
	TArray<FMonolithSourceSymbol> Result;
	if (!Database || !Database->IsValid()) return Result;

	FString FTSQuery = EscapeFTS(Query);

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("SELECT s.id, s.name, s.qualified_name, s.kind, s.file_id, s.line_start, s.line_end, s.parent_symbol_id, s.access, s.signature, s.docstring, s.is_ue_macro FROM symbols_fts f JOIN symbols s ON s.id = f.rowid WHERE symbols_fts MATCH ? ORDER BY bm25(symbols_fts) LIMIT ?;"));
	Stmt.SetBindingValueByIndex(1, FTSQuery);
	Stmt.SetBindingValueByIndex(2, static_cast<int64>(Limit));

	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		Result.Add(ReadSymbolFromStatement(Stmt));
	}
	return Result;
}

TOptional<FMonolithSourceSymbol> FMonolithSourceDatabase::GetSymbolById(int64 Id)
{
	FScopeLock Lock(&DbLock);
	if (!Database || !Database->IsValid()) return {};

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("SELECT id, name, qualified_name, kind, file_id, line_start, line_end, parent_symbol_id, access, signature, docstring, is_ue_macro FROM symbols WHERE id = ?;"));
	Stmt.SetBindingValueByIndex(1, Id);

	if (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		return ReadSymbolFromStatement(Stmt);
	}
	return {};
}

// ============================================================
// File queries
// ============================================================

FString FMonolithSourceDatabase::GetFilePath(int64 FileId)
{
	FScopeLock Lock(&DbLock);
	if (!Database || !Database->IsValid()) return TEXT("<unknown>");

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("SELECT path FROM files WHERE id = ?;"));
	Stmt.SetBindingValueByIndex(1, FileId);

	if (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FString Path;
		Stmt.GetColumnValueByIndex(0, Path);
		return Path;
	}
	return TEXT("<unknown>");
}

TOptional<FMonolithSourceFile> FMonolithSourceDatabase::FindFileBySuffix(const FString& Suffix)
{
	FScopeLock Lock(&DbLock);
	if (!Database || !Database->IsValid()) return {};

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("SELECT id, path, module_id, file_type, line_count FROM files WHERE path LIKE ? LIMIT 1;"));
	Stmt.SetBindingValueByIndex(1, FString::Printf(TEXT("%%%s"), *Suffix));

	if (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FMonolithSourceFile File;
		Stmt.GetColumnValueByIndex(0, File.Id);
		Stmt.GetColumnValueByIndex(1, File.Path);
		Stmt.GetColumnValueByIndex(2, File.ModuleId);
		Stmt.GetColumnValueByIndex(3, File.FileType);
		int32 LC = 0;
		Stmt.GetColumnValueByIndex(4, LC);
		File.LineCount = LC;
		return File;
	}
	return {};
}

bool FMonolithSourceDatabase::GetFileModuleInfo(int64 FileId, FString& OutModuleName, FString& OutBuildCsPath)
{
	FScopeLock Lock(&DbLock);
	OutModuleName.Empty();
	OutBuildCsPath.Empty();
	if (!Database || !Database->IsValid()) return false;

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database,
		TEXT("SELECT m.name, m.build_cs_path FROM files f JOIN modules m ON m.id = f.module_id WHERE f.id = ?;"));
	Stmt.SetBindingValueByIndex(1, FileId);

	if (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		Stmt.GetColumnValueByIndex(0, OutModuleName);
		Stmt.GetColumnValueByIndex(1, OutBuildCsPath);
		return true;
	}
	return false;
}

TOptional<FMonolithSourceFile> FMonolithSourceDatabase::FindFileByPath(const FString& Path)
{
	FScopeLock Lock(&DbLock);
	if (!Database || !Database->IsValid()) return {};

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("SELECT id, path, module_id, file_type, line_count FROM files WHERE path = ?;"));
	Stmt.SetBindingValueByIndex(1, Path);

	if (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FMonolithSourceFile File;
		Stmt.GetColumnValueByIndex(0, File.Id);
		Stmt.GetColumnValueByIndex(1, File.Path);
		Stmt.GetColumnValueByIndex(2, File.ModuleId);
		Stmt.GetColumnValueByIndex(3, File.FileType);
		int32 LC = 0;
		Stmt.GetColumnValueByIndex(4, LC);
		File.LineCount = LC;
		return File;
	}
	return {};
}

// ============================================================
// Reference queries
// ============================================================

TArray<FMonolithSourceReference> FMonolithSourceDatabase::GetReferencesTo(int64 SymbolId, const FString& RefKind, int32 Limit)
{
	FScopeLock Lock(&DbLock);
	TArray<FMonolithSourceReference> Result;
	if (!Database || !Database->IsValid()) return Result;

	FSQLitePreparedStatement Stmt;
	if (RefKind.IsEmpty())
	{
		Stmt.Create(*Database, TEXT("SELECT r.id, r.from_symbol_id, r.to_symbol_id, r.ref_kind, r.file_id, r.line, s.name, f.path FROM \"references\" r JOIN symbols s ON s.id = r.from_symbol_id JOIN files f ON f.id = r.file_id WHERE r.to_symbol_id = ? LIMIT ?;"));
		Stmt.SetBindingValueByIndex(1, SymbolId);
		Stmt.SetBindingValueByIndex(2, static_cast<int64>(Limit));
	}
	else
	{
		Stmt.Create(*Database, TEXT("SELECT r.id, r.from_symbol_id, r.to_symbol_id, r.ref_kind, r.file_id, r.line, s.name, f.path FROM \"references\" r JOIN symbols s ON s.id = r.from_symbol_id JOIN files f ON f.id = r.file_id WHERE r.to_symbol_id = ? AND r.ref_kind = ? LIMIT ?;"));
		Stmt.SetBindingValueByIndex(1, SymbolId);
		Stmt.SetBindingValueByIndex(2, RefKind);
		Stmt.SetBindingValueByIndex(3, static_cast<int64>(Limit));
	}

	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		Result.Add(ReadReferenceFromStatement(Stmt, true));
	}
	return Result;
}

TArray<FMonolithSourceReference> FMonolithSourceDatabase::GetReferencesFrom(int64 SymbolId, const FString& RefKind, int32 Limit)
{
	FScopeLock Lock(&DbLock);
	TArray<FMonolithSourceReference> Result;
	if (!Database || !Database->IsValid()) return Result;

	FSQLitePreparedStatement Stmt;
	if (RefKind.IsEmpty())
	{
		Stmt.Create(*Database, TEXT("SELECT r.id, r.from_symbol_id, r.to_symbol_id, r.ref_kind, r.file_id, r.line, s.name, f.path FROM \"references\" r JOIN symbols s ON s.id = r.to_symbol_id JOIN files f ON f.id = r.file_id WHERE r.from_symbol_id = ? LIMIT ?;"));
		Stmt.SetBindingValueByIndex(1, SymbolId);
		Stmt.SetBindingValueByIndex(2, static_cast<int64>(Limit));
	}
	else
	{
		Stmt.Create(*Database, TEXT("SELECT r.id, r.from_symbol_id, r.to_symbol_id, r.ref_kind, r.file_id, r.line, s.name, f.path FROM \"references\" r JOIN symbols s ON s.id = r.to_symbol_id JOIN files f ON f.id = r.file_id WHERE r.from_symbol_id = ? AND r.ref_kind = ? LIMIT ?;"));
		Stmt.SetBindingValueByIndex(1, SymbolId);
		Stmt.SetBindingValueByIndex(2, RefKind);
		Stmt.SetBindingValueByIndex(3, static_cast<int64>(Limit));
	}

	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		Result.Add(ReadReferenceFromStatement(Stmt, false));
	}
	return Result;
}

// ============================================================
// Inheritance queries
// ============================================================

TArray<FMonolithSourceInheritance> FMonolithSourceDatabase::GetParents(int64 SymbolId)
{
	FScopeLock Lock(&DbLock);
	TArray<FMonolithSourceInheritance> Result;
	if (!Database || !Database->IsValid()) return Result;

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("SELECT s.id, s.name, s.qualified_name, s.kind, s.file_id, s.line_start, s.line_end FROM inheritance i JOIN symbols s ON s.id = i.parent_id WHERE i.child_id = ?;"));
	Stmt.SetBindingValueByIndex(1, SymbolId);

	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FMonolithSourceInheritance Inh;
		Stmt.GetColumnValueByIndex(0, Inh.Id);
		Stmt.GetColumnValueByIndex(1, Inh.Name);
		Stmt.GetColumnValueByIndex(2, Inh.QualifiedName);
		Stmt.GetColumnValueByIndex(3, Inh.Kind);
		Stmt.GetColumnValueByIndex(4, Inh.FileId);
		int32 LS = 0, LE = 0;
		Stmt.GetColumnValueByIndex(5, LS);
		Stmt.GetColumnValueByIndex(6, LE);
		Inh.LineStart = LS;
		Inh.LineEnd = LE;
		Result.Add(MoveTemp(Inh));
	}
	return Result;
}

TArray<FMonolithSourceInheritance> FMonolithSourceDatabase::GetChildren(int64 SymbolId)
{
	FScopeLock Lock(&DbLock);
	TArray<FMonolithSourceInheritance> Result;
	if (!Database || !Database->IsValid()) return Result;

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("SELECT s.id, s.name, s.qualified_name, s.kind, s.file_id, s.line_start, s.line_end FROM inheritance i JOIN symbols s ON s.id = i.child_id WHERE i.parent_id = ?;"));
	Stmt.SetBindingValueByIndex(1, SymbolId);

	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FMonolithSourceInheritance Inh;
		Stmt.GetColumnValueByIndex(0, Inh.Id);
		Stmt.GetColumnValueByIndex(1, Inh.Name);
		Stmt.GetColumnValueByIndex(2, Inh.QualifiedName);
		Stmt.GetColumnValueByIndex(3, Inh.Kind);
		Stmt.GetColumnValueByIndex(4, Inh.FileId);
		int32 LS = 0, LE = 0;
		Stmt.GetColumnValueByIndex(5, LS);
		Stmt.GetColumnValueByIndex(6, LE);
		Inh.LineStart = LS;
		Inh.LineEnd = LE;
		Result.Add(MoveTemp(Inh));
	}
	return Result;
}

// ============================================================
// Module queries
// ============================================================

TOptional<FMonolithSourceModuleStats> FMonolithSourceDatabase::GetModuleStats(const FString& ModuleName)
{
	FScopeLock Lock(&DbLock);
	if (!Database || !Database->IsValid()) return {};

	// Get module info
	FSQLitePreparedStatement ModStmt;
	ModStmt.Create(*Database, TEXT("SELECT id, name, path, module_type FROM modules WHERE name = ?;"));
	ModStmt.SetBindingValueByIndex(1, ModuleName);

	if (ModStmt.Step() != ESQLitePreparedStatementStepResult::Row)
	{
		return {};
	}

	FMonolithSourceModuleStats Stats;
	int64 ModId = 0;
	ModStmt.GetColumnValueByIndex(0, ModId);
	ModStmt.GetColumnValueByIndex(1, Stats.Name);
	ModStmt.GetColumnValueByIndex(2, Stats.Path);
	ModStmt.GetColumnValueByIndex(3, Stats.ModuleType);

	// File count
	FSQLitePreparedStatement FileStmt;
	FileStmt.Create(*Database, TEXT("SELECT COUNT(*) FROM files WHERE module_id = ?;"));
	FileStmt.SetBindingValueByIndex(1, ModId);
	if (FileStmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		int64 Count = 0;
		FileStmt.GetColumnValueByIndex(0, Count);
		Stats.FileCount = static_cast<int32>(Count);
	}

	// Symbol counts by kind
	FSQLitePreparedStatement KindStmt;
	KindStmt.Create(*Database, TEXT("SELECT s.kind, COUNT(*) as cnt FROM symbols s JOIN files f ON f.id = s.file_id WHERE f.module_id = ? GROUP BY s.kind;"));
	KindStmt.SetBindingValueByIndex(1, ModId);
	while (KindStmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FString Kind;
		int64 Count = 0;
		KindStmt.GetColumnValueByIndex(0, Kind);
		KindStmt.GetColumnValueByIndex(1, Count);
		Stats.SymbolCounts.Add(Kind, static_cast<int32>(Count));
	}

	return Stats;
}

TArray<FMonolithSourceSymbol> FMonolithSourceDatabase::GetSymbolsInModule(const FString& ModuleName, const FString& Kind, int32 Limit)
{
	FScopeLock Lock(&DbLock);
	TArray<FMonolithSourceSymbol> Result;
	if (!Database || !Database->IsValid()) return Result;

	FSQLitePreparedStatement Stmt;
	if (Kind.IsEmpty())
	{
		Stmt.Create(*Database, TEXT("SELECT s.id, s.name, s.qualified_name, s.kind, s.file_id, s.line_start, s.line_end, s.parent_symbol_id, s.access, s.signature, s.docstring, s.is_ue_macro FROM symbols s JOIN files f ON f.id = s.file_id JOIN modules m ON m.id = f.module_id WHERE m.name = ? LIMIT ?;"));
		Stmt.SetBindingValueByIndex(1, ModuleName);
		Stmt.SetBindingValueByIndex(2, static_cast<int64>(Limit));
	}
	else
	{
		Stmt.Create(*Database, TEXT("SELECT s.id, s.name, s.qualified_name, s.kind, s.file_id, s.line_start, s.line_end, s.parent_symbol_id, s.access, s.signature, s.docstring, s.is_ue_macro FROM symbols s JOIN files f ON f.id = s.file_id JOIN modules m ON m.id = f.module_id WHERE m.name = ? AND s.kind = ? LIMIT ?;"));
		Stmt.SetBindingValueByIndex(1, ModuleName);
		Stmt.SetBindingValueByIndex(2, Kind);
		Stmt.SetBindingValueByIndex(3, static_cast<int64>(Limit));
	}

	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		Result.Add(ReadSymbolFromStatement(Stmt));
	}
	return Result;
}

// ============================================================
// Source FTS
// ============================================================

TArray<FMonolithSourceChunk> FMonolithSourceDatabase::SearchSourceFTS(const FString& Query, const FString& Scope, int32 Limit)
{
	FScopeLock Lock(&DbLock);
	TArray<FMonolithSourceChunk> Result;
	if (!Database || !Database->IsValid()) return Result;

	FString FTSQuery = EscapeFTS(Query);

	FSQLitePreparedStatement Stmt;
	if (Scope == TEXT("all"))
	{
		Stmt.Create(*Database, TEXT("SELECT f.file_id, f.line_number, f.text FROM source_fts f WHERE source_fts MATCH ? ORDER BY bm25(source_fts) LIMIT ?;"));
		Stmt.SetBindingValueByIndex(1, FTSQuery);
		Stmt.SetBindingValueByIndex(2, static_cast<int64>(Limit));
	}
	else
	{
		Stmt.Create(*Database, TEXT("SELECT sf.file_id, sf.line_number, sf.text FROM source_fts sf JOIN files fi ON fi.id = sf.file_id WHERE source_fts MATCH ? AND fi.file_type = ? ORDER BY bm25(source_fts) LIMIT ?;"));
		Stmt.SetBindingValueByIndex(1, FTSQuery);
		Stmt.SetBindingValueByIndex(2, Scope);
		Stmt.SetBindingValueByIndex(3, static_cast<int64>(Limit));
	}

	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FMonolithSourceChunk Chunk;
		Stmt.GetColumnValueByIndex(0, Chunk.FileId);
		int32 LN = 0;
		Stmt.GetColumnValueByIndex(1, LN);
		Chunk.LineNumber = LN;
		Stmt.GetColumnValueByIndex(2, Chunk.Text);
		Result.Add(MoveTemp(Chunk));
	}
	return Result;
}

TArray<FMonolithSourceChunk> FMonolithSourceDatabase::SearchSourceFTSFiltered(const FString& Query, const FString& Scope, const FString& Module, const FString& PathFilter, int32 Limit)
{
	FScopeLock Lock(&DbLock);
	TArray<FMonolithSourceChunk> Result;
	if (!Database || !Database->IsValid()) return Result;

	if (Scope == TEXT("all") && Module.IsEmpty() && PathFilter.IsEmpty())
	{
		return SearchSourceFTS(Query, Scope, Limit);
	}

	FString FTSQuery = EscapeFTS(Query);

	FString SQL = TEXT("SELECT sf.file_id, sf.line_number, sf.text FROM source_fts sf JOIN files fi ON fi.id = sf.file_id ");
	TArray<FString> Conditions;
	Conditions.Add(TEXT("source_fts MATCH ?"));

	if (!Module.IsEmpty())
	{
		SQL += TEXT("JOIN modules m ON m.id = fi.module_id ");
		Conditions.Add(TEXT("m.name = ?"));
	}
	if (Scope != TEXT("all"))
	{
		Conditions.Add(TEXT("fi.file_type = ?"));
	}
	if (!PathFilter.IsEmpty())
	{
		Conditions.Add(TEXT("fi.path LIKE ?"));
	}

	SQL += TEXT("WHERE ") + FString::Join(Conditions, TEXT(" AND "));
	SQL += FString::Printf(TEXT(" ORDER BY bm25(source_fts) LIMIT %d;"), Limit);

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, *SQL);

	int32 BindIdx = 1;
	Stmt.SetBindingValueByIndex(BindIdx++, FTSQuery);
	if (!Module.IsEmpty())
	{
		Stmt.SetBindingValueByIndex(BindIdx++, Module);
	}
	if (Scope != TEXT("all"))
	{
		Stmt.SetBindingValueByIndex(BindIdx++, Scope);
	}
	if (!PathFilter.IsEmpty())
	{
		Stmt.SetBindingValueByIndex(BindIdx++, FString::Printf(TEXT("%%%s%%"), *PathFilter));
	}

	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FMonolithSourceChunk Chunk;
		Stmt.GetColumnValueByIndex(0, Chunk.FileId);
		int32 LN = 0;
		Stmt.GetColumnValueByIndex(1, LN);
		Chunk.LineNumber = LN;
		Stmt.GetColumnValueByIndex(2, Chunk.Text);
		Result.Add(MoveTemp(Chunk));
	}
	return Result;
}

TArray<FMonolithSourceSymbol> FMonolithSourceDatabase::SearchSymbolsFTSFiltered(const FString& Query, const FString& Kind, const FString& Module, const FString& PathFilter, int32 Limit)
{
	FScopeLock Lock(&DbLock);
	TArray<FMonolithSourceSymbol> Result;
	if (!Database || !Database->IsValid()) return Result;

	FString FTSQuery = EscapeFTS(Query);

	FString SQL = TEXT("SELECT s.id, s.name, s.qualified_name, s.kind, s.file_id, s.line_start, s.line_end, s.parent_symbol_id, s.access, s.signature, s.docstring, s.is_ue_macro FROM symbols_fts f JOIN symbols s ON s.id = f.rowid ");
	TArray<FString> Conditions;
	Conditions.Add(TEXT("symbols_fts MATCH ?"));

	if (!Module.IsEmpty() || !PathFilter.IsEmpty())
	{
		SQL += TEXT("JOIN files fi ON fi.id = s.file_id ");
	}
	if (!Module.IsEmpty())
	{
		SQL += TEXT("JOIN modules m ON m.id = fi.module_id ");
		Conditions.Add(TEXT("m.name = ?"));
	}
	if (!Kind.IsEmpty())
	{
		Conditions.Add(TEXT("s.kind = ?"));
	}
	if (!PathFilter.IsEmpty())
	{
		Conditions.Add(TEXT("fi.path LIKE ?"));
	}

	SQL += TEXT("WHERE ") + FString::Join(Conditions, TEXT(" AND "));
	SQL += FString::Printf(TEXT(" ORDER BY bm25(symbols_fts) LIMIT %d;"), Limit);

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, *SQL);

	int32 BindIdx = 1;
	Stmt.SetBindingValueByIndex(BindIdx++, FTSQuery);
	if (!Module.IsEmpty())
	{
		Stmt.SetBindingValueByIndex(BindIdx++, Module);
	}
	if (!Kind.IsEmpty())
	{
		Stmt.SetBindingValueByIndex(BindIdx++, Kind);
	}
	if (!PathFilter.IsEmpty())
	{
		Stmt.SetBindingValueByIndex(BindIdx++, FString::Printf(TEXT("%%%s%%"), *PathFilter));
	}

	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		Result.Add(ReadSymbolFromStatement(Stmt));
	}
	return Result;
}

// ============================================================
// FTS COUNT(*) helpers (Survivor E — plan §3.E)
//
// Issued ONLY on page 0 of cursor-paginated search_source. Mirrors the
// JOIN / WHERE clauses of SearchSymbolsFTSFiltered / SearchSourceFTSFiltered
// so the COUNT matches what the paged result would surface across the full
// rerun. ORDER BY / LIMIT are intentionally omitted — COUNT short-circuits.
// ============================================================

int32 FMonolithSourceDatabase::CountSymbolsFTSFiltered(const FString& Query, const FString& Kind, const FString& Module, const FString& PathFilter)
{
	FScopeLock Lock(&DbLock);
	if (!Database || !Database->IsValid()) return 0;

	const FString FTSQuery = EscapeFTS(Query);

	FString SQL = TEXT("SELECT COUNT(*) FROM symbols_fts f JOIN symbols s ON s.id = f.rowid ");
	TArray<FString> Conditions;
	Conditions.Add(TEXT("symbols_fts MATCH ?"));

	if (!Module.IsEmpty() || !PathFilter.IsEmpty())
	{
		SQL += TEXT("JOIN files fi ON fi.id = s.file_id ");
	}
	if (!Module.IsEmpty())
	{
		SQL += TEXT("JOIN modules m ON m.id = fi.module_id ");
		Conditions.Add(TEXT("m.name = ?"));
	}
	if (!Kind.IsEmpty())
	{
		Conditions.Add(TEXT("s.kind = ?"));
	}
	if (!PathFilter.IsEmpty())
	{
		Conditions.Add(TEXT("fi.path LIKE ?"));
	}

	SQL += TEXT("WHERE ") + FString::Join(Conditions, TEXT(" AND "));
	SQL += TEXT(";");

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, *SQL);

	int32 BindIdx = 1;
	Stmt.SetBindingValueByIndex(BindIdx++, FTSQuery);
	if (!Module.IsEmpty())
	{
		Stmt.SetBindingValueByIndex(BindIdx++, Module);
	}
	if (!Kind.IsEmpty())
	{
		Stmt.SetBindingValueByIndex(BindIdx++, Kind);
	}
	if (!PathFilter.IsEmpty())
	{
		Stmt.SetBindingValueByIndex(BindIdx++, FString::Printf(TEXT("%%%s%%"), *PathFilter));
	}

	int32 Count = 0;
	if (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		int64 C64 = 0;
		Stmt.GetColumnValueByIndex(0, C64);
		Count = static_cast<int32>(C64);
	}
	return Count;
}

int32 FMonolithSourceDatabase::CountSourceFTSFiltered(const FString& Query, const FString& Scope, const FString& Module, const FString& PathFilter)
{
	FScopeLock Lock(&DbLock);
	if (!Database || !Database->IsValid()) return 0;

	const FString FTSQuery = EscapeFTS(Query);

	// Fast path: no joins needed when neither module nor path filter is set
	// AND scope is "all" — single FTS COUNT(*).
	if (Scope == TEXT("all") && Module.IsEmpty() && PathFilter.IsEmpty())
	{
		FSQLitePreparedStatement Stmt;
		Stmt.Create(*Database, TEXT("SELECT COUNT(*) FROM source_fts WHERE source_fts MATCH ?;"));
		Stmt.SetBindingValueByIndex(1, FTSQuery);

		if (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
		{
			int64 C64 = 0;
			Stmt.GetColumnValueByIndex(0, C64);
			return static_cast<int32>(C64);
		}
		return 0;
	}

	FString SQL = TEXT("SELECT COUNT(*) FROM source_fts sf JOIN files fi ON fi.id = sf.file_id ");
	TArray<FString> Conditions;
	Conditions.Add(TEXT("source_fts MATCH ?"));

	if (!Module.IsEmpty())
	{
		SQL += TEXT("JOIN modules m ON m.id = fi.module_id ");
		Conditions.Add(TEXT("m.name = ?"));
	}
	if (Scope != TEXT("all"))
	{
		Conditions.Add(TEXT("fi.file_type = ?"));
	}
	if (!PathFilter.IsEmpty())
	{
		Conditions.Add(TEXT("fi.path LIKE ?"));
	}

	SQL += TEXT("WHERE ") + FString::Join(Conditions, TEXT(" AND "));
	SQL += TEXT(";");

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, *SQL);

	int32 BindIdx = 1;
	Stmt.SetBindingValueByIndex(BindIdx++, FTSQuery);
	if (!Module.IsEmpty())
	{
		Stmt.SetBindingValueByIndex(BindIdx++, Module);
	}
	if (Scope != TEXT("all"))
	{
		Stmt.SetBindingValueByIndex(BindIdx++, Scope);
	}
	if (!PathFilter.IsEmpty())
	{
		Stmt.SetBindingValueByIndex(BindIdx++, FString::Printf(TEXT("%%%s%%"), *PathFilter));
	}

	int32 Count = 0;
	if (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		int64 C64 = 0;
		Stmt.GetColumnValueByIndex(0, C64);
		Count = static_cast<int32>(C64);
	}
	return Count;
}

// ============================================================
// Write API — OpenForWriting
// ============================================================

bool FMonolithSourceDatabase::OpenForWriting(const FString& DbPath)
{
	FScopeLock Lock(&DbLock);

	if (Database)
	{
		Database->Close();
		delete Database;
		Database = nullptr;
	}

	CachedDbPath = DbPath;

	Database = new FSQLiteDatabase();
	if (!Database->Open(*DbPath, ESQLiteDatabaseOpenMode::ReadWriteCreate))
	{
		UE_LOG(LogMonolithSource, Error, TEXT("OpenForWriting: failed to open/create DB: %s"), *DbPath);
		delete Database;
		Database = nullptr;
		return false;
	}

	// Force DELETE journal mode on the writer handle so the DB persists in DELETE
	// mode at rest (no -wal/-shm sidecars). NOTE (corrected 2026-05-29): this is
	// hygiene, NOT the fix for Reflection Intelligence's "disk I/O error" x25 —
	// that was a SAME-process double-open rejected by UE 5.7's single-open
	// `unreal-fs` VFS, now resolved by RI borrowing the subsystem's open handle
	// (FMonolithSourceDatabase::GetRawHandle) rather than opening its own. We keep
	// the return-value check because a journal-mode failure here is still worth a
	// visible warning.
	if (!Database->Execute(TEXT("PRAGMA journal_mode=DELETE;")))
	{
		UE_LOG(LogMonolithSource, Warning,
			TEXT("OpenForWriting: PRAGMA journal_mode=DELETE failed on '%s' (continuing — hygiene only)"),
			*DbPath);
	}
	Database->Execute(TEXT("PRAGMA synchronous=NORMAL;"));
	Database->Execute(TEXT("PRAGMA cache_size=-64000;"));   // 64 MB page cache

	UE_LOG(LogMonolithSource, Log, TEXT("Engine source DB opened for writing: %s"), *DbPath);
	return true;
}

// ============================================================
// Schema management
// ============================================================

bool FMonolithSourceDatabase::CreateTablesIfNeeded()
{
	FScopeLock Lock(&DbLock);
	if (!Database || !Database->IsValid())
	{
		UE_LOG(LogMonolithSource, Error, TEXT("CreateTablesIfNeeded: DB not open"));
		return false;
	}

	if (!ExecuteMulti(*Database, MonolithSourceSchema::DDL_Tables))
	{
		UE_LOG(LogMonolithSource, Error, TEXT("CreateTablesIfNeeded: DDL_Tables failed — %s"), *Database->GetLastError());
		return false;
	}
	if (!ExecuteMulti(*Database, MonolithSourceSchema::DDL_FTS))
	{
		UE_LOG(LogMonolithSource, Error, TEXT("CreateTablesIfNeeded: DDL_FTS failed — %s"), *Database->GetLastError());
		return false;
	}
	if (!ExecuteMulti(*Database, MonolithSourceSchema::DDL_Triggers))
	{
		UE_LOG(LogMonolithSource, Error, TEXT("CreateTablesIfNeeded: DDL_Triggers failed — %s"), *Database->GetLastError());
		return false;
	}

	// Stamp the schema version into meta
	FSQLitePreparedStatement MetaStmt;
	MetaStmt.Create(*Database, TEXT("INSERT OR REPLACE INTO meta (key, value) VALUES (?, ?);"));
	MetaStmt.SetBindingValueByIndex(1, FString(TEXT("schema_version")));
	MetaStmt.SetBindingValueByIndex(2, FString::FromInt(MonolithSourceSchema::SchemaVersion));
	MetaStmt.Step();

	UE_LOG(LogMonolithSource, Log, TEXT("Schema created/verified (version %d)"), MonolithSourceSchema::SchemaVersion);
	return true;
}

bool FMonolithSourceDatabase::ResetDatabase()
{
	FScopeLock Lock(&DbLock);
	if (!Database || !Database->IsValid())
	{
		UE_LOG(LogMonolithSource, Error, TEXT("ResetDatabase: DB not open"));
		return false;
	}

	if (!ExecuteMulti(*Database, MonolithSourceSchema::DDL_Drop))
	{
		UE_LOG(LogMonolithSource, Error, TEXT("ResetDatabase: drop failed — %s"), *Database->GetLastError());
		return false;
	}

	UE_LOG(LogMonolithSource, Log, TEXT("ResetDatabase: all tables dropped, recreating schema"));

	// Execute DDL inline (we're already holding DbLock, can't call CreateTablesIfNeeded)
	if (!ExecuteMulti(*Database, MonolithSourceSchema::DDL_Tables))
	{
		UE_LOG(LogMonolithSource, Error, TEXT("ResetDatabase: DDL_Tables failed — %s"), *Database->GetLastError());
		return false;
	}
	if (!ExecuteMulti(*Database, MonolithSourceSchema::DDL_FTS))
	{
		UE_LOG(LogMonolithSource, Error, TEXT("ResetDatabase: DDL_FTS failed — %s"), *Database->GetLastError());
		return false;
	}
	if (!ExecuteMulti(*Database, MonolithSourceSchema::DDL_Triggers))
	{
		UE_LOG(LogMonolithSource, Error, TEXT("ResetDatabase: DDL_Triggers failed — %s"), *Database->GetLastError());
		return false;
	}

	FSQLitePreparedStatement MetaStmt;
	MetaStmt.Create(*Database, TEXT("INSERT OR REPLACE INTO meta (key, value) VALUES (?, ?);"));
	MetaStmt.SetBindingValueByIndex(1, FString(TEXT("schema_version")));
	MetaStmt.SetBindingValueByIndex(2, FString::FromInt(MonolithSourceSchema::SchemaVersion));
	MetaStmt.Step();

	UE_LOG(LogMonolithSource, Log, TEXT("ResetDatabase: schema recreated successfully"));
	return true;
}

// ============================================================
// Transaction control
// ============================================================

bool FMonolithSourceDatabase::BeginTransaction()
{
	FScopeLock Lock(&DbLock);
	if (!Database || !Database->IsValid()) return false;
	return Database->Execute(TEXT("BEGIN;"));
}

bool FMonolithSourceDatabase::CommitTransaction()
{
	FScopeLock Lock(&DbLock);
	if (!Database || !Database->IsValid()) return false;
	return Database->Execute(TEXT("COMMIT;"));
}

bool FMonolithSourceDatabase::RollbackTransaction()
{
	FScopeLock Lock(&DbLock);
	if (!Database || !Database->IsValid()) return false;
	return Database->Execute(TEXT("ROLLBACK;"));
}

// ============================================================
// Insert helpers
// ============================================================

int64 FMonolithSourceDatabase::InsertModule(const FString& Name, const FString& Path, const FString& ModuleType, const FString& BuildCsPath)
{
	FScopeLock Lock(&DbLock);
	if (!Database || !Database->IsValid()) return 0;

	// INSERT OR IGNORE — if UNIQUE(name,path) already exists, this is a no-op
	FSQLitePreparedStatement InsStmt;
	InsStmt.Create(*Database, TEXT("INSERT OR IGNORE INTO modules (name, path, module_type, build_cs_path) VALUES (?, ?, ?, ?);"));
	InsStmt.SetBindingValueByIndex(1, Name);
	InsStmt.SetBindingValueByIndex(2, Path);
	InsStmt.SetBindingValueByIndex(3, ModuleType);
	InsStmt.SetBindingValueByIndex(4, BuildCsPath);
	InsStmt.Step();

	int64 RowId = Database->GetLastInsertRowId();
	if (RowId != 0)
	{
		return RowId;
	}

	// Already existed — fetch its id
	FSQLitePreparedStatement SelStmt;
	SelStmt.Create(*Database, TEXT("SELECT id FROM modules WHERE name = ? AND path = ?;"));
	SelStmt.SetBindingValueByIndex(1, Name);
	SelStmt.SetBindingValueByIndex(2, Path);
	if (SelStmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		int64 ExistingId = 0;
		SelStmt.GetColumnValueByIndex(0, ExistingId);
		return ExistingId;
	}

	UE_LOG(LogMonolithSource, Warning, TEXT("InsertModule: could not retrieve id for '%s'"), *Name);
	return 0;
}

int64 FMonolithSourceDatabase::InsertFile(const FString& FilePath, int64 ModuleId, const FString& FileType, int32 LineCount, double LastModified)
{
	FScopeLock Lock(&DbLock);
	if (!Database || !Database->IsValid()) return 0;

	FSQLitePreparedStatement InsStmt;
	InsStmt.Create(*Database, TEXT("INSERT OR IGNORE INTO files (path, module_id, file_type, line_count, last_modified) VALUES (?, ?, ?, ?, ?);"));
	InsStmt.SetBindingValueByIndex(1, FilePath);
	InsStmt.SetBindingValueByIndex(2, ModuleId);
	InsStmt.SetBindingValueByIndex(3, FileType);
	InsStmt.SetBindingValueByIndex(4, static_cast<int64>(LineCount));
	InsStmt.SetBindingValueByIndex(5, LastModified);
	InsStmt.Step();

	int64 RowId = Database->GetLastInsertRowId();
	if (RowId != 0)
	{
		return RowId;
	}

	// Already existed — fetch its id
	FSQLitePreparedStatement SelStmt;
	SelStmt.Create(*Database, TEXT("SELECT id FROM files WHERE path = ?;"));
	SelStmt.SetBindingValueByIndex(1, FilePath);
	if (SelStmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		int64 ExistingId = 0;
		SelStmt.GetColumnValueByIndex(0, ExistingId);
		return ExistingId;
	}

	UE_LOG(LogMonolithSource, Warning, TEXT("InsertFile: could not retrieve id for '%s'"), *FilePath);
	return 0;
}

int64 FMonolithSourceDatabase::InsertSymbol(
	const FString& Name, const FString& QualifiedName, const FString& Kind,
	int64 FileId, int32 LineStart, int32 LineEnd,
	int64 ParentSymbolId,
	const FString& Access, const FString& Signature, const FString& Docstring,
	bool bIsUEMacro)
{
	FScopeLock Lock(&DbLock);
	if (!Database || !Database->IsValid()) return 0;

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database,
		TEXT("INSERT INTO symbols (name, qualified_name, kind, file_id, line_start, line_end, parent_symbol_id, access, signature, docstring, is_ue_macro) ")
		TEXT("VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);"));

	Stmt.SetBindingValueByIndex(1, Name);
	Stmt.SetBindingValueByIndex(2, QualifiedName);
	Stmt.SetBindingValueByIndex(3, Kind);

	// file_id — bind NULL if 0
	if (FileId != 0)
	{
		Stmt.SetBindingValueByIndex(4, FileId);
	}
	// else: leave unbound — SQLite defaults to NULL

	Stmt.SetBindingValueByIndex(5, static_cast<int64>(LineStart));
	Stmt.SetBindingValueByIndex(6, static_cast<int64>(LineEnd));

	// parent_symbol_id — bind NULL if 0
	if (ParentSymbolId != 0)
	{
		Stmt.SetBindingValueByIndex(7, ParentSymbolId);
	}
	// else: leave unbound — SQLite defaults to NULL

	Stmt.SetBindingValueByIndex(8, Access);
	Stmt.SetBindingValueByIndex(9, Signature);
	Stmt.SetBindingValueByIndex(10, Docstring);
	Stmt.SetBindingValueByIndex(11, static_cast<int64>(bIsUEMacro ? 1 : 0));

	Stmt.Step();

	return Database->GetLastInsertRowId();
}

void FMonolithSourceDatabase::InsertInheritance(int64 ChildId, int64 ParentId)
{
	FScopeLock Lock(&DbLock);
	if (!Database || !Database->IsValid()) return;

	// OR IGNORE — silent on unique constraint violation, mirrors Python IntegrityError catch
	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("INSERT OR IGNORE INTO inheritance (child_id, parent_id) VALUES (?, ?);"));
	Stmt.SetBindingValueByIndex(1, ChildId);
	Stmt.SetBindingValueByIndex(2, ParentId);
	Stmt.Step();
}

void FMonolithSourceDatabase::InsertReference(int64 FromSymbolId, int64 ToSymbolId, const FString& RefKind, int64 FileId, int32 Line)
{
	FScopeLock Lock(&DbLock);
	if (!Database || !Database->IsValid()) return;

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database,
		TEXT("INSERT INTO \"references\" (from_symbol_id, to_symbol_id, ref_kind, file_id, line) ")
		TEXT("VALUES (?, ?, ?, ?, ?);"));
	Stmt.SetBindingValueByIndex(1, FromSymbolId);
	Stmt.SetBindingValueByIndex(2, ToSymbolId);
	Stmt.SetBindingValueByIndex(3, RefKind);

	if (FileId != 0)
	{
		Stmt.SetBindingValueByIndex(4, FileId);
	}

	Stmt.SetBindingValueByIndex(5, static_cast<int64>(Line));
	Stmt.Step();
}

void FMonolithSourceDatabase::InsertInclude(int64 FileId, const FString& IncludedPath, int32 Line)
{
	FScopeLock Lock(&DbLock);
	if (!Database || !Database->IsValid()) return;

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("INSERT INTO includes (file_id, included_path, line) VALUES (?, ?, ?);"));
	Stmt.SetBindingValueByIndex(1, FileId);
	Stmt.SetBindingValueByIndex(2, IncludedPath);
	Stmt.SetBindingValueByIndex(3, static_cast<int64>(Line));
	Stmt.Step();
}

void FMonolithSourceDatabase::InsertSourceChunks(int64 FileId, const TArray<FString>& Lines)
{
	FScopeLock Lock(&DbLock);
	if (!Database || !Database->IsValid()) return;
	if (Lines.Num() == 0) return;

	// Batch lines in groups of 10, matching Python _insert_source_lines()
	// Chunk's line_number is the 1-based index of the first line in that batch.
	static const int32 ChunkSize = 10;

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("INSERT INTO source_fts (file_id, line_number, text) VALUES (?, ?, ?);"));

	for (int32 BatchStart = 0; BatchStart < Lines.Num(); BatchStart += ChunkSize)
	{
		const int32 BatchEnd = FMath::Min(BatchStart + ChunkSize, Lines.Num());

		FString JoinedText;
		for (int32 i = BatchStart; i < BatchEnd; ++i)
		{
			if (i > BatchStart)
			{
				JoinedText += TEXT("\n");
			}
			JoinedText += Lines[i];
		}

		// 1-based line number of the first line in this batch
		const int64 ChunkLineNumber = static_cast<int64>(BatchStart + 1);

		Stmt.Reset();
		Stmt.SetBindingValueByIndex(1, FileId);
		Stmt.SetBindingValueByIndex(2, ChunkLineNumber);
		Stmt.SetBindingValueByIndex(3, JoinedText);
		Stmt.Step();
	}
}

// ============================================================
// Meta key/value
// ============================================================

void FMonolithSourceDatabase::SetMeta(const FString& Key, const FString& Value)
{
	FScopeLock Lock(&DbLock);
	if (!Database || !Database->IsValid()) return;

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("INSERT OR REPLACE INTO meta (key, value) VALUES (?, ?);"));
	Stmt.SetBindingValueByIndex(1, Key);
	Stmt.SetBindingValueByIndex(2, Value);
	Stmt.Step();
}

FString FMonolithSourceDatabase::GetMeta(const FString& Key)
{
	FScopeLock Lock(&DbLock);
	if (!Database || !Database->IsValid()) return TEXT("");

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("SELECT value FROM meta WHERE key = ?;"));
	Stmt.SetBindingValueByIndex(1, Key);

	if (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FString Value;
		Stmt.GetColumnValueByIndex(0, Value);
		return Value;
	}
	return TEXT("");
}

// ============================================================
// Deprecation queries (item 3)
// ============================================================

void FMonolithSourceDatabase::InsertDeprecation(int64 SymbolId, const FString& SymbolName, const FString& Version, const FString& Message, const FString& Kind)
{
	FScopeLock Lock(&DbLock);
	if (!Database || !Database->IsValid()) return;

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database,
		TEXT("INSERT INTO symbol_deprecations (symbol_id, symbol_name, version, message, kind) ")
		TEXT("VALUES (?, ?, ?, ?, ?);"));

	// symbol_id — bind NULL if 0 (class-body methods have no symbols row)
	if (SymbolId != 0)
	{
		Stmt.SetBindingValueByIndex(1, SymbolId);
	}
	// else: leave unbound — SQLite defaults to NULL

	Stmt.SetBindingValueByIndex(2, SymbolName);
	Stmt.SetBindingValueByIndex(3, Version);
	Stmt.SetBindingValueByIndex(4, Message);
	Stmt.SetBindingValueByIndex(5, Kind);
	Stmt.Step();
}

TOptional<FMonolithDeprecationRow> FMonolithSourceDatabase::GetDeprecation(const FString& SymbolName)
{
	FScopeLock Lock(&DbLock);
	if (!Database || !Database->IsValid()) return {};

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("SELECT version, message, kind FROM symbol_deprecations WHERE symbol_name = ? LIMIT 1;"));
	Stmt.SetBindingValueByIndex(1, SymbolName);

	if (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FMonolithDeprecationRow Row;
		Stmt.GetColumnValueByIndex(0, Row.Version);
		Stmt.GetColumnValueByIndex(1, Row.Message);
		Stmt.GetColumnValueByIndex(2, Row.Kind);
		return Row;
	}
	return {};
}

TMap<FString, FMonolithDeprecationRow> FMonolithSourceDatabase::GetDeprecationsBatch(const TArray<FString>& SymbolNames)
{
	FScopeLock Lock(&DbLock);
	TMap<FString, FMonolithDeprecationRow> Result;
	if (!Database || !Database->IsValid()) return Result;

	// One prepared statement reused per name — symbol counts are typically small.
	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("SELECT version, message, kind FROM symbol_deprecations WHERE symbol_name = ? LIMIT 1;"));

	for (const FString& Name : SymbolNames)
	{
		Stmt.Reset();
		Stmt.SetBindingValueByIndex(1, Name);
		if (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
		{
			FMonolithDeprecationRow Row;
			Stmt.GetColumnValueByIndex(0, Row.Version);
			Stmt.GetColumnValueByIndex(1, Row.Message);
			Stmt.GetColumnValueByIndex(2, Row.Kind);
			Result.Add(Name, Row);
		}
	}
	return Result;
}

int32 FMonolithSourceDatabase::GetDeprecationCount()
{
	FScopeLock Lock(&DbLock);
	if (!Database || !Database->IsValid()) return 0;

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("SELECT COUNT(*) FROM symbol_deprecations;"));
	if (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		int64 C64 = 0;
		Stmt.GetColumnValueByIndex(0, C64);
		return static_cast<int32>(C64);
	}
	return 0;
}

// ============================================================
// Incremental indexing support
// ============================================================

int32 FMonolithSourceDatabase::LoadExistingSymbols(
	TMap<FString, int64>& OutSymbolNameToId,
	TMap<FString, int64>& OutClassNameToId,
	TMap<FString, TPair<int32,int32>>& OutSymbolSpans,
	TMap<FString, TPair<int32,int32>>& OutClassSpans)
{
	FScopeLock Lock(&DbLock);
	OutSymbolNameToId.Empty();
	OutClassNameToId.Empty();
	OutSymbolSpans.Empty();
	OutClassSpans.Empty();

	if (!Database || !Database->IsValid()) return 0;

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database,
		TEXT("SELECT id, name, qualified_name, kind, line_start, line_end FROM symbols;"));

	int32 Count = 0;
	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		int64 Id = 0;
		FString Name, QualifiedName, Kind;
		int32 LineStart = 0, LineEnd = 0;

		Stmt.GetColumnValueByIndex(0, Id);
		Stmt.GetColumnValueByIndex(1, Name);
		Stmt.GetColumnValueByIndex(2, QualifiedName);
		Stmt.GetColumnValueByIndex(3, Kind);
		Stmt.GetColumnValueByIndex(4, LineStart);
		Stmt.GetColumnValueByIndex(5, LineEnd);

		// Populate name->id maps (name and qualified_name both point to same id)
		OutSymbolNameToId.Add(Name, Id);
		if (QualifiedName != Name && !QualifiedName.IsEmpty())
		{
			OutSymbolNameToId.Add(QualifiedName, Id);
		}

		// Span tracking — prefer definitions (line_end > line_start) over forward decls
		const bool bIsDefinition = (LineEnd > LineStart);
		const TPair<int32,int32> NewSpan(LineStart, LineEnd);

		if (!OutSymbolSpans.Contains(Name))
		{
			OutSymbolSpans.Add(Name, NewSpan);
		}
		else if (bIsDefinition && OutSymbolSpans[Name].Value <= OutSymbolSpans[Name].Key)
		{
			// Overwrite forward decl (line_end <= line_start) with definition
			OutSymbolSpans[Name] = NewSpan;
		}

		// Class/struct maps
		const bool bIsClassOrStruct = (Kind == TEXT("class") || Kind == TEXT("struct"));
		if (bIsClassOrStruct)
		{
			OutClassNameToId.Add(Name, Id);
			if (QualifiedName != Name && !QualifiedName.IsEmpty())
			{
				OutClassNameToId.Add(QualifiedName, Id);
			}

			if (!OutClassSpans.Contains(Name))
			{
				OutClassSpans.Add(Name, NewSpan);
			}
			else if (bIsDefinition && OutClassSpans[Name].Value <= OutClassSpans[Name].Key)
			{
				// Overwrite forward decl with definition
				OutClassSpans[Name] = NewSpan;
			}
		}

		++Count;
	}

	UE_LOG(LogMonolithSource, Log, TEXT("LoadExistingSymbols: loaded %d symbols (%d classes/structs)"),
		Count, OutClassNameToId.Num());

	return Count;
}
