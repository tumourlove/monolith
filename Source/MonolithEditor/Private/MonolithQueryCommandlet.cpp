#include "MonolithQueryCommandlet.h"
#include "MonolithSourceDatabase.h"
#include "MonolithIndexDatabase.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Interfaces/IPluginManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogMonolithQuery, Log, All);

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

UMonolithQueryCommandlet::UMonolithQueryCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
	ShowErrorCount = false;
}

// ---------------------------------------------------------------------------
// Main entry
// ---------------------------------------------------------------------------

int32 UMonolithQueryCommandlet::Main(const FString& Params)
{
	TArray<FString> Tokens;
	Params.ParseIntoArrayWS(Tokens);

	if (Tokens.Num() < 2)
	{
		PrintUsage();
		return 1;
	}

	FString Namespace = Tokens[0].ToLower();
	TArray<FString> RemainingArgs(Tokens.GetData() + 1, Tokens.Num() - 1);

	TMap<FString, FString> Options;
	TArray<FString> PositionalArgs = ParseOptions(RemainingArgs, Options);

	if (Namespace == TEXT("source"))
	{
		return HandleSource(PositionalArgs, Options);
	}
	else if (Namespace == TEXT("project"))
	{
		return HandleProject(PositionalArgs, Options);
	}

	UE_LOG(LogMonolithQuery, Error, TEXT("Unknown namespace '%s'. Expected 'source' or 'project'."), *Namespace);
	PrintUsage();
	return 1;
}

// ---------------------------------------------------------------------------
// Option parsing
// ---------------------------------------------------------------------------

TArray<FString> UMonolithQueryCommandlet::ParseOptions(const TArray<FString>& RawArgs, TMap<FString, FString>& OutOptions)
{
	TArray<FString> Positional;
	for (const FString& Arg : RawArgs)
	{
		if (Arg.StartsWith(TEXT("--")))
		{
			FString Key, Value;
			if (Arg.Split(TEXT("="), &Key, &Value))
			{
				OutOptions.Add(Key.Mid(2), Value);
			}
			else
			{
				// Flag-style: --flag treated as --flag=true
				OutOptions.Add(Arg.Mid(2), TEXT("true"));
			}
		}
		else
		{
			Positional.Add(Arg);
		}
	}
	return Positional;
}

// ---------------------------------------------------------------------------
// DB paths
// ---------------------------------------------------------------------------

FString UMonolithQueryCommandlet::GetSourceDbPath() const
{
	TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("Monolith"));
	if (Plugin.IsValid())
	{
		return Plugin->GetBaseDir() / TEXT("Saved") / TEXT("EngineSource.db");
	}
	return FPaths::ProjectPluginsDir() / TEXT("Monolith") / TEXT("Saved") / TEXT("EngineSource.db");
}

FString UMonolithQueryCommandlet::GetProjectDbPath() const
{
	TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("Monolith"));
	if (Plugin.IsValid())
	{
		return Plugin->GetBaseDir() / TEXT("Saved") / TEXT("ProjectIndex.db");
	}
	return FPaths::ProjectPluginsDir() / TEXT("Monolith") / TEXT("Saved") / TEXT("ProjectIndex.db");
}

// ---------------------------------------------------------------------------
// Source handler
// ---------------------------------------------------------------------------

int32 UMonolithQueryCommandlet::HandleSource(const TArray<FString>& Args, const TMap<FString, FString>& Options)
{
	if (Args.Num() < 1)
	{
		UE_LOG(LogMonolithQuery, Error, TEXT("source: missing action"));
		return 1;
	}

	FString Action = Args[0].ToLower();

	FMonolithSourceDatabase DB;
	FString DbPath = GetSourceDbPath();
	if (!DB.Open(DbPath))
	{
		UE_LOG(LogMonolithQuery, Error, TEXT("Failed to open source DB: %s"), *DbPath);
		return 1;
	}

	int32 Limit = FCString::Atoi(*Options.FindRef(TEXT("limit")));
	if (Limit <= 0) Limit = 20;

	// -----------------------------------------------------------------------
	// search_source
	// -----------------------------------------------------------------------
	if (Action == TEXT("search_source"))
	{
		if (Args.Num() < 2)
		{
			UE_LOG(LogMonolithQuery, Error, TEXT("search_source: requires a query string"));
			return 1;
		}
		const FString Query = Args[1];

		// Symbol FTS
		TArray<FMonolithSourceSymbol> Symbols = DB.SearchSymbolsFTS(Query, Limit);
		UE_LOG(LogMonolithQuery, Display, TEXT("=== Symbol matches (%d) ==="), Symbols.Num());
		for (const FMonolithSourceSymbol& S : Symbols)
		{
			UE_LOG(LogMonolithQuery, Display, TEXT("  [%s] %s  (%s:%d-%d)"),
				*S.Kind, *S.QualifiedName, *DB.GetFilePath(S.FileId), S.LineStart, S.LineEnd);
			if (!S.Signature.IsEmpty())
			{
				UE_LOG(LogMonolithQuery, Display, TEXT("    sig: %s"), *S.Signature);
			}
		}

		// Source FTS
		TArray<FMonolithSourceChunk> Chunks = DB.SearchSourceFTS(Query, TEXT("all"), Limit);
		UE_LOG(LogMonolithQuery, Display, TEXT(""));
		UE_LOG(LogMonolithQuery, Display, TEXT("=== Source line matches (%d) ==="), Chunks.Num());
		for (const FMonolithSourceChunk& C : Chunks)
		{
			UE_LOG(LogMonolithQuery, Display, TEXT("  [%s:%d] %s"),
				*DB.GetFilePath(C.FileId), C.LineNumber, *C.Text.Left(200));
		}
	}
	// -----------------------------------------------------------------------
	// read_source
	// -----------------------------------------------------------------------
	else if (Action == TEXT("read_source"))
	{
		if (Args.Num() < 2)
		{
			UE_LOG(LogMonolithQuery, Error, TEXT("read_source: requires a symbol name"));
			return 1;
		}

		TArray<FMonolithSourceSymbol> Symbols = DB.GetSymbolsByName(Args[1]);
		if (Symbols.Num() == 0)
		{
			// Fallback to FTS
			Symbols = DB.SearchSymbolsFTS(Args[1], 1);
		}
		if (Symbols.Num() == 0)
		{
			UE_LOG(LogMonolithQuery, Error, TEXT("read_source: symbol not found: %s"), *Args[1]);
			return 1;
		}

		const FMonolithSourceSymbol& Sym = Symbols[0];
		FString FilePath = DB.GetFilePath(Sym.FileId);

		UE_LOG(LogMonolithQuery, Display, TEXT("[%s] %s"), *Sym.Kind, *Sym.QualifiedName);
		UE_LOG(LogMonolithQuery, Display, TEXT("  File: %s:%d-%d"), *FilePath, Sym.LineStart, Sym.LineEnd);
		if (!Sym.Signature.IsEmpty()) { UE_LOG(LogMonolithQuery, Display, TEXT("  Sig:  %s"), *Sym.Signature); }
		if (!Sym.Docstring.IsEmpty()) { UE_LOG(LogMonolithQuery, Display, TEXT("  Doc:  %s"), *Sym.Docstring); }

		int32 MaxLines = FCString::Atoi(*Options.FindRef(TEXT("max-lines")));
		if (MaxLines <= 0) MaxLines = 100;

		TArray<FString> Lines;
		if (FFileHelper::LoadFileToStringArray(Lines, *FilePath))
		{
			int32 Start = FMath::Max(0, Sym.LineStart - 1);
			int32 End   = FMath::Min(Lines.Num(), Sym.LineEnd > Sym.LineStart ? Sym.LineEnd : Start + MaxLines);
			UE_LOG(LogMonolithQuery, Display, TEXT(""));
			UE_LOG(LogMonolithQuery, Display, TEXT("--- Source (%s:%d-%d) ---"),
				*FPaths::GetCleanFilename(FilePath), Start + 1, End);
			for (int32 i = Start; i < End; ++i)
			{
				UE_LOG(LogMonolithQuery, Display, TEXT("%5d | %s"), i + 1, *Lines[i]);
			}
		}
		else
		{
			UE_LOG(LogMonolithQuery, Warning, TEXT("Could not read file from disk: %s"), *FilePath);
		}
	}
	// -----------------------------------------------------------------------
	// find_callers / find_references
	// -----------------------------------------------------------------------
	else if (Action == TEXT("find_callers") || Action == TEXT("find_references"))
	{
		if (Args.Num() < 2)
		{
			UE_LOG(LogMonolithQuery, Error, TEXT("%s: requires a symbol name"), *Action);
			return 1;
		}

		TArray<FMonolithSourceSymbol> Symbols = DB.GetSymbolsByName(Args[1]);
		if (Symbols.Num() == 0)
		{
			UE_LOG(LogMonolithQuery, Error, TEXT("%s: symbol not found: %s"), *Action, *Args[1]);
			return 1;
		}

		const FString RefKind = (Action == TEXT("find_callers")) ? TEXT("call") : TEXT("");
		TArray<FMonolithSourceReference> Refs = DB.GetReferencesTo(Symbols[0].Id, RefKind, Limit);
		UE_LOG(LogMonolithQuery, Display, TEXT("=== %s for '%s' (%d results) ==="), *Action, *Args[1], Refs.Num());
		for (const FMonolithSourceReference& R : Refs)
		{
			UE_LOG(LogMonolithQuery, Display, TEXT("  [%s] %s  at %s:%d"),
				*R.RefKind, *R.FromName, *R.Path, R.Line);
		}
	}
	// -----------------------------------------------------------------------
	// find_callees
	// -----------------------------------------------------------------------
	else if (Action == TEXT("find_callees"))
	{
		if (Args.Num() < 2)
		{
			UE_LOG(LogMonolithQuery, Error, TEXT("find_callees: requires a symbol name"));
			return 1;
		}

		TArray<FMonolithSourceSymbol> Symbols = DB.GetSymbolsByName(Args[1]);
		if (Symbols.Num() == 0)
		{
			UE_LOG(LogMonolithQuery, Error, TEXT("find_callees: symbol not found: %s"), *Args[1]);
			return 1;
		}

		TArray<FMonolithSourceReference> Refs = DB.GetReferencesFrom(Symbols[0].Id, TEXT("call"), Limit);
		UE_LOG(LogMonolithQuery, Display, TEXT("=== Callees of '%s' (%d results) ==="), *Args[1], Refs.Num());
		for (const FMonolithSourceReference& R : Refs)
		{
			UE_LOG(LogMonolithQuery, Display, TEXT("  [%s] %s  at %s:%d"),
				*R.RefKind, *R.ToName, *R.Path, R.Line);
		}
	}
	// -----------------------------------------------------------------------
	// get_class_hierarchy
	// -----------------------------------------------------------------------
	else if (Action == TEXT("get_class_hierarchy"))
	{
		if (Args.Num() < 2)
		{
			UE_LOG(LogMonolithQuery, Error, TEXT("get_class_hierarchy: requires a symbol name"));
			return 1;
		}

		TArray<FMonolithSourceSymbol> Symbols = DB.GetSymbolsByName(Args[1]);
		if (Symbols.Num() == 0)
		{
			UE_LOG(LogMonolithQuery, Error, TEXT("get_class_hierarchy: symbol not found: %s"), *Args[1]);
			return 1;
		}

		TArray<FMonolithSourceInheritance> Parents = DB.GetParents(Symbols[0].Id);
		UE_LOG(LogMonolithQuery, Display, TEXT("=== Parents of '%s' (%d) ==="), *Args[1], Parents.Num());
		for (const FMonolithSourceInheritance& P : Parents)
		{
			UE_LOG(LogMonolithQuery, Display, TEXT("  %s  (%s)"), *P.Name, *P.Kind);
		}

		TArray<FMonolithSourceInheritance> Children = DB.GetChildren(Symbols[0].Id);
		UE_LOG(LogMonolithQuery, Display, TEXT("=== Children of '%s' (%d) ==="), *Args[1], Children.Num());
		for (const FMonolithSourceInheritance& C : Children)
		{
			UE_LOG(LogMonolithQuery, Display, TEXT("  %s  (%s)"), *C.Name, *C.Kind);
		}
	}
	// -----------------------------------------------------------------------
	// get_module_info
	// -----------------------------------------------------------------------
	else if (Action == TEXT("get_module_info"))
	{
		if (Args.Num() < 2)
		{
			UE_LOG(LogMonolithQuery, Error, TEXT("get_module_info: requires a module name"));
			return 1;
		}

		TOptional<FMonolithSourceModuleStats> Stats = DB.GetModuleStats(Args[1]);
		if (!Stats.IsSet())
		{
			UE_LOG(LogMonolithQuery, Error, TEXT("get_module_info: module not found: %s"), *Args[1]);
			return 1;
		}

		UE_LOG(LogMonolithQuery, Display, TEXT("Module: %s  (%s)"), *Stats->Name, *Stats->ModuleType);
		UE_LOG(LogMonolithQuery, Display, TEXT("  Path:  %s"), *Stats->Path);
		UE_LOG(LogMonolithQuery, Display, TEXT("  Files: %d"), Stats->FileCount);
		for (const auto& Pair : Stats->SymbolCounts)
		{
			UE_LOG(LogMonolithQuery, Display, TEXT("  %-20s %d"), *Pair.Key, Pair.Value);
		}
	}
	// -----------------------------------------------------------------------
	// get_symbol_context
	// -----------------------------------------------------------------------
	else if (Action == TEXT("get_symbol_context"))
	{
		if (Args.Num() < 2)
		{
			UE_LOG(LogMonolithQuery, Error, TEXT("get_symbol_context: requires a symbol name"));
			return 1;
		}

		TArray<FMonolithSourceSymbol> Symbols = DB.GetSymbolsByName(Args[1]);
		if (Symbols.Num() == 0)
		{
			UE_LOG(LogMonolithQuery, Error, TEXT("get_symbol_context: symbol not found: %s"), *Args[1]);
			return 1;
		}

		const FMonolithSourceSymbol& Sym = Symbols[0];
		FString FilePath = DB.GetFilePath(Sym.FileId);
		int32 ContextLines = FCString::Atoi(*Options.FindRef(TEXT("context-lines")));
		if (ContextLines <= 0) ContextLines = 10;

		TArray<FString> FileLines;
		if (FFileHelper::LoadFileToStringArray(FileLines, *FilePath))
		{
			int32 Start = FMath::Max(0, Sym.LineStart - 1 - ContextLines);
			int32 End   = FMath::Min(FileLines.Num(), Sym.LineEnd + ContextLines);
			UE_LOG(LogMonolithQuery, Display, TEXT("[%s] %s  (%s:%d-%d)"),
				*Sym.Kind, *Sym.QualifiedName, *FPaths::GetCleanFilename(FilePath), Sym.LineStart, Sym.LineEnd);
			UE_LOG(LogMonolithQuery, Display, TEXT(""));
			for (int32 i = Start; i < End; ++i)
			{
				UE_LOG(LogMonolithQuery, Display, TEXT("%5d | %s"), i + 1, *FileLines[i]);
			}
		}
		else
		{
			UE_LOG(LogMonolithQuery, Warning, TEXT("Could not read file from disk: %s"), *FilePath);
		}
	}
	// -----------------------------------------------------------------------
	// read_file
	// -----------------------------------------------------------------------
	else if (Action == TEXT("read_file"))
	{
		if (Args.Num() < 2)
		{
			UE_LOG(LogMonolithQuery, Error, TEXT("read_file: requires a file path or suffix"));
			return 1;
		}

		FString FilePath = Args[1];

		// If not an absolute path that exists, try DB lookup
		if (!FPaths::FileExists(FilePath))
		{
			TOptional<FMonolithSourceFile> File = DB.FindFileByPath(FilePath);
			if (!File.IsSet())
			{
				File = DB.FindFileBySuffix(FilePath);
			}
			if (File.IsSet())
			{
				FilePath = File->Path;
			}
		}

		int32 StartLine = FCString::Atoi(*Options.FindRef(TEXT("start")));
		int32 EndLine   = FCString::Atoi(*Options.FindRef(TEXT("end")));
		if (StartLine <= 0) StartLine = 1;
		if (EndLine   <= 0) EndLine   = StartLine + 200;

		TArray<FString> FileLines;
		if (FFileHelper::LoadFileToStringArray(FileLines, *FilePath))
		{
			int32 ActualEnd = FMath::Min(EndLine, FileLines.Num());
			UE_LOG(LogMonolithQuery, Display, TEXT("--- %s (lines %d-%d of %d) ---"),
				*FPaths::GetCleanFilename(FilePath), StartLine, ActualEnd, FileLines.Num());
			for (int32 i = StartLine - 1; i < ActualEnd; ++i)
			{
				UE_LOG(LogMonolithQuery, Display, TEXT("%5d | %s"), i + 1, *FileLines[i]);
			}
		}
		else
		{
			UE_LOG(LogMonolithQuery, Error, TEXT("read_file: could not read: %s"), *FilePath);
			return 1;
		}
	}
	else
	{
		UE_LOG(LogMonolithQuery, Error, TEXT("Unknown source action: '%s'"), *Action);
		UE_LOG(LogMonolithQuery, Display, TEXT("Available: search_source, read_source, find_callers, find_callees, find_references, get_class_hierarchy, get_module_info, get_symbol_context, read_file"));
		return 1;
	}

	return 0;
}

// ---------------------------------------------------------------------------
// Project handler
// ---------------------------------------------------------------------------

int32 UMonolithQueryCommandlet::HandleProject(const TArray<FString>& Args, const TMap<FString, FString>& Options)
{
	if (Args.Num() < 1)
	{
		UE_LOG(LogMonolithQuery, Error, TEXT("project: missing action"));
		return 1;
	}

	FString Action = Args[0].ToLower();

	FMonolithIndexDatabase DB;
	FString DbPath = GetProjectDbPath();
	if (!DB.Open(DbPath))
	{
		UE_LOG(LogMonolithQuery, Error, TEXT("Failed to open project DB: %s"), *DbPath);
		return 1;
	}

	int32 Limit = FCString::Atoi(*Options.FindRef(TEXT("limit")));
	if (Limit <= 0) Limit = 50;

	// -----------------------------------------------------------------------
	// search
	// -----------------------------------------------------------------------
	if (Action == TEXT("search"))
	{
		if (Args.Num() < 2)
		{
			UE_LOG(LogMonolithQuery, Error, TEXT("project search: requires a query string"));
			return 1;
		}

		TArray<FSearchResult> Results = DB.FullTextSearch(Args[1], Limit);
		UE_LOG(LogMonolithQuery, Display, TEXT("=== Project search: '%s' (%d results) ==="), *Args[1], Results.Num());
		for (const FSearchResult& R : Results)
		{
			UE_LOG(LogMonolithQuery, Display, TEXT("  [%s] %s"), *R.AssetClass, *R.AssetPath);
			if (!R.MatchContext.IsEmpty())
			{
				UE_LOG(LogMonolithQuery, Display, TEXT("    > %s"), *R.MatchContext.Left(200));
			}
		}
	}
	// -----------------------------------------------------------------------
	// find_by_type
	// -----------------------------------------------------------------------
	else if (Action == TEXT("find_by_type"))
	{
		if (Args.Num() < 2)
		{
			UE_LOG(LogMonolithQuery, Error, TEXT("project find_by_type: requires an asset class name"));
			return 1;
		}

		int32 Offset = FCString::Atoi(*Options.FindRef(TEXT("offset")));
		TArray<FIndexedAsset> Assets = DB.FindByType(Args[1], Limit, Offset);
		UE_LOG(LogMonolithQuery, Display, TEXT("=== Assets of type '%s' (%d results) ==="), *Args[1], Assets.Num());
		for (const FIndexedAsset& A : Assets)
		{
			UE_LOG(LogMonolithQuery, Display, TEXT("  %s/%s  [%s]"), *A.PackagePath, *A.AssetName, *A.AssetClass);
		}
	}
	// -----------------------------------------------------------------------
	// find_references
	// -----------------------------------------------------------------------
	else if (Action == TEXT("find_references"))
	{
		if (Args.Num() < 2)
		{
			UE_LOG(LogMonolithQuery, Error, TEXT("project find_references: requires an asset path"));
			return 1;
		}

		TSharedPtr<FJsonObject> Result = DB.FindReferences(Args[1]);
		if (!Result.IsValid())
		{
			UE_LOG(LogMonolithQuery, Error, TEXT("project find_references: no data for '%s'"), *Args[1]);
			return 1;
		}

		FString OutputStr;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputStr);
		FJsonSerializer::Serialize(Result.ToSharedRef(), Writer);
		UE_LOG(LogMonolithQuery, Display, TEXT("=== References for '%s' ==="), *Args[1]);
		UE_LOG(LogMonolithQuery, Display, TEXT("%s"), *OutputStr);
	}
	// -----------------------------------------------------------------------
	// get_stats
	// -----------------------------------------------------------------------
	else if (Action == TEXT("get_stats"))
	{
		TSharedPtr<FJsonObject> Stats = DB.GetStats();
		if (!Stats.IsValid())
		{
			UE_LOG(LogMonolithQuery, Error, TEXT("project get_stats: failed to retrieve stats"));
			return 1;
		}

		UE_LOG(LogMonolithQuery, Display, TEXT("=== Project Index Stats ==="));
		FString OutputStr;
		TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&OutputStr);
		FJsonSerializer::Serialize(Stats.ToSharedRef(), Writer);
		UE_LOG(LogMonolithQuery, Display, TEXT("%s"), *OutputStr);
	}
	// -----------------------------------------------------------------------
	// get_asset_details
	// -----------------------------------------------------------------------
	else if (Action == TEXT("get_asset_details"))
	{
		if (Args.Num() < 2)
		{
			UE_LOG(LogMonolithQuery, Error, TEXT("project get_asset_details: requires an asset path"));
			return 1;
		}

		TSharedPtr<FJsonObject> Details = DB.GetAssetDetails(Args[1]);
		if (!Details.IsValid())
		{
			UE_LOG(LogMonolithQuery, Error, TEXT("project get_asset_details: not found: %s"), *Args[1]);
			return 1;
		}

		FString OutputStr;
		TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&OutputStr);
		FJsonSerializer::Serialize(Details.ToSharedRef(), Writer);
		UE_LOG(LogMonolithQuery, Display, TEXT("%s"), *OutputStr);
	}
	else
	{
		UE_LOG(LogMonolithQuery, Error, TEXT("Unknown project action: '%s'"), *Action);
		UE_LOG(LogMonolithQuery, Display, TEXT("Available: search, find_by_type, find_references, get_stats, get_asset_details"));
		return 1;
	}

	return 0;
}

// ---------------------------------------------------------------------------
// Usage
// ---------------------------------------------------------------------------

void UMonolithQueryCommandlet::PrintUsage()
{
	UE_LOG(LogMonolithQuery, Display, TEXT(""));
	UE_LOG(LogMonolithQuery, Display, TEXT("Monolith Offline Query Commandlet"));
	UE_LOG(LogMonolithQuery, Display, TEXT("=================================="));
	UE_LOG(LogMonolithQuery, Display, TEXT("Usage:"));
	UE_LOG(LogMonolithQuery, Display, TEXT("  UnrealEditor-Cmd.exe ProjectName -run=MonolithQuery <namespace> <action> [args] [--options]"));
	UE_LOG(LogMonolithQuery, Display, TEXT(""));
	UE_LOG(LogMonolithQuery, Display, TEXT("Namespace: source"));
	UE_LOG(LogMonolithQuery, Display, TEXT("  search_source   <query>              [--limit=N]"));
	UE_LOG(LogMonolithQuery, Display, TEXT("  read_source     <symbol>             [--max-lines=N]"));
	UE_LOG(LogMonolithQuery, Display, TEXT("  find_callers    <symbol>             [--limit=N]"));
	UE_LOG(LogMonolithQuery, Display, TEXT("  find_callees    <symbol>             [--limit=N]"));
	UE_LOG(LogMonolithQuery, Display, TEXT("  find_references <symbol>             [--limit=N]"));
	UE_LOG(LogMonolithQuery, Display, TEXT("  get_class_hierarchy <symbol>"));
	UE_LOG(LogMonolithQuery, Display, TEXT("  get_module_info <module_name>"));
	UE_LOG(LogMonolithQuery, Display, TEXT("  get_symbol_context <symbol>          [--context-lines=N]"));
	UE_LOG(LogMonolithQuery, Display, TEXT("  read_file       <path_or_suffix>     [--start=N] [--end=N]"));
	UE_LOG(LogMonolithQuery, Display, TEXT(""));
	UE_LOG(LogMonolithQuery, Display, TEXT("Namespace: project"));
	UE_LOG(LogMonolithQuery, Display, TEXT("  search          <query>              [--limit=N]"));
	UE_LOG(LogMonolithQuery, Display, TEXT("  find_by_type    <AssetClass>         [--limit=N] [--offset=N]"));
	UE_LOG(LogMonolithQuery, Display, TEXT("  find_references <asset_path>"));
	UE_LOG(LogMonolithQuery, Display, TEXT("  get_stats"));
	UE_LOG(LogMonolithQuery, Display, TEXT("  get_asset_details <asset_path>"));
	UE_LOG(LogMonolithQuery, Display, TEXT(""));
	UE_LOG(LogMonolithQuery, Display, TEXT("Examples:"));
	UE_LOG(LogMonolithQuery, Display, TEXT("  -run=MonolithQuery source search_source FCharacterMovementComponent"));
	UE_LOG(LogMonolithQuery, Display, TEXT("  -run=MonolithQuery source read_source ACharacter --max-lines=50"));
	UE_LOG(LogMonolithQuery, Display, TEXT("  -run=MonolithQuery source get_class_hierarchy ACharacter"));
	UE_LOG(LogMonolithQuery, Display, TEXT("  -run=MonolithQuery source find_callers BeginPlay --limit=10"));
	UE_LOG(LogMonolithQuery, Display, TEXT("  -run=MonolithQuery project search damage"));
	UE_LOG(LogMonolithQuery, Display, TEXT("  -run=MonolithQuery project find_by_type Blueprint --limit=20"));
	UE_LOG(LogMonolithQuery, Display, TEXT("  -run=MonolithQuery project get_stats"));
}
