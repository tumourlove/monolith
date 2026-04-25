#include "MonolithSourceSubsystem.h"
#include "MonolithSourceIndexer.h"
#include "MonolithSettings.h"
#include "Misc/Paths.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformTime.h"
#include "Async/Async.h"
#include "Interfaces/IPluginManager.h"
#include "UObject/UObjectGlobals.h" // F17: FCoreUObjectDelegates::ReloadCompleteDelegate
#include "UObject/Object.h"

UMonolithSourceSubsystem::~UMonolithSourceSubsystem()
{
	delete Indexer;
	Indexer = nullptr;
}

void UMonolithSourceSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// Commandlet mode (cook/compile): skip DB open. The running editor holds a WAL lock on
	// EngineSource.db; a second open surfaces as "disk I/O error" → UAT ExitCode=1.
	if (IsRunningCommandlet())
	{
		return;
	}

	Database = MakeUnique<FMonolithSourceDatabase>();
	FString DbPath = GetDatabasePath();

	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	if (PlatformFile.FileExists(*DbPath))
	{
		if (Database->Open(DbPath))
		{
			UE_LOG(LogMonolithSource, Log, TEXT("Engine source DB loaded from %s"), *DbPath);
		}
	}
	else
	{
		UE_LOG(LogMonolithSource, Log, TEXT("Engine source DB not found at %s — run source.trigger_reindex to create it"), *DbPath);
	}

	// F17 (2026-04-26): Auto-reindex on hot-reload / Live Coding completion.
	// Without this, agents see stale source_query results until a manual `source.trigger_project_reindex` call.
	// `FCoreUObjectDelegates::ReloadCompleteDelegate` fires on both Live Coding patches AND full UBT-restart
	// hot-reloads (precedent: unreal-code-reviewer agent memory ref_ue57_ui_plan_review_findings.md).
	// Editor-only by construction: this whole subsystem is a UEditorSubsystem and the commandlet branch
	// above already short-circuits cook/compile; no extra WITH_EDITOR gate needed here.
	ReloadCompleteHandle = FCoreUObjectDelegates::ReloadCompleteDelegate.AddUObject(
		this, &UMonolithSourceSubsystem::OnReloadComplete);
}

void UMonolithSourceSubsystem::Deinitialize()
{
	// F17: Unbind the hot-reload hook BEFORE we tear down anything else, so a late-firing
	// reload signal during shutdown can't re-enter into a half-destroyed subsystem.
	if (ReloadCompleteHandle.IsValid())
	{
		FCoreUObjectDelegates::ReloadCompleteDelegate.Remove(ReloadCompleteHandle);
		ReloadCompleteHandle.Reset();
	}

	// Stop any running indexer
	if (Indexer)
	{
		Indexer->RequestStop();
		delete Indexer;
		Indexer = nullptr;
	}

	if (Database.IsValid())
	{
		Database->Close();
	}
	Super::Deinitialize();
}

// ============================================================
// F17: Hot-reload auto-reindex hook
// ============================================================

void UMonolithSourceSubsystem::OnReloadComplete(EReloadCompleteReason Reason)
{
	// Idempotency guard #1: a reindex is already running. UBT can fire multiple
	// ReloadCompleteDelegate signals (one per loaded module) in quick succession —
	// without this, every additional fire would log a "Indexing already in progress"
	// warning from TriggerProjectReindex(). Cheap early-out.
	if (bIsIndexing)
	{
		UE_LOG(LogMonolithSource, Verbose,
			TEXT("[F17] OnReloadComplete: indexing already in progress — skipping auto-kick"));
		return;
	}

	// Idempotency guard #2: cooldown. Even if indexing isn't currently running, a
	// freshly-completed reindex within the last 5s almost certainly already covers
	// the symbols this signal is reporting. 5s is comfortably longer than the
	// typical multi-module reload burst (~50–200 ms) but short enough that a real
	// second-edit kicks promptly.
	const double Now = FPlatformTime::Seconds();
	const double CooldownSeconds = 5.0;
	if (LastReindexTimeSeconds > 0.0 && (Now - LastReindexTimeSeconds) < CooldownSeconds)
	{
		UE_LOG(LogMonolithSource, Verbose,
			TEXT("[F17] OnReloadComplete: cooldown active (%.2fs since last) — skipping auto-kick"),
			Now - LastReindexTimeSeconds);
		return;
	}

	// Sanity guard: the project DB must exist (TriggerProjectReindex is incremental and
	// errors out if the engine symbols aren't already in place). On the very first run
	// after install there is no DB — fall through silently and let the user run
	// `source.trigger_reindex` once to bootstrap. Don't surface a noisy warning here.
	const FString DbPath = GetDatabasePath();
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	if (!PlatformFile.FileExists(*DbPath))
	{
		UE_LOG(LogMonolithSource, Verbose,
			TEXT("[F17] OnReloadComplete: EngineSource.db not present — bootstrap first via source.trigger_reindex"));
		return;
	}

	UE_LOG(LogMonolithSource, Log,
		TEXT("[F17] Hot-reload detected — kicking incremental project reindex (auto)"));

	LastReindexTimeSeconds = Now;
	TriggerProjectReindex(); // Already async via Indexer->StartAsync(); returns immediately.
}

// ============================================================
// Full reindex: engine + shaders + project (clean build)
// ============================================================

void UMonolithSourceSubsystem::TriggerReindex()
{
	if (bIsIndexing)
	{
		UE_LOG(LogMonolithSource, Warning, TEXT("Indexing already in progress"));
		return;
	}

	FString DbPath = GetDatabasePath();

	// Ensure Saved dir exists
	FString SavedDir = FPaths::GetPath(DbPath);
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	if (!PlatformFile.DirectoryExists(*SavedDir))
	{
		PlatformFile.CreateDirectoryTree(*SavedDir);
	}

	// Close DB during reindex
	if (Database.IsValid() && Database->IsOpen())
	{
		Database->Close();
	}

	bIsIndexing = true;

		delete Indexer;
	Indexer = new FMonolithSourceIndexer();
	Indexer->SetSourcePath(GetEngineSourcePath());
	Indexer->SetShaderPath(GetEngineShaderPath());
	Indexer->SetProjectPath(GetProjectPath());
	Indexer->SetDatabasePath(DbPath);
	Indexer->SetCleanBuild(true);
	Indexer->SetIndexProjectSource(true);

	Indexer->OnComplete.AddLambda([this, DbPath](int32 Files, int32 Symbols, int32 Errors)
	{
		AsyncTask(ENamedThreads::GameThread, [this, DbPath, Files, Symbols, Errors]()
		{
			bIsIndexing = false;
			UE_LOG(LogMonolithSource, Log, TEXT("C++ source indexing complete: %d files, %d symbols, %d errors"), Files, Symbols, Errors);
			ReopenDatabase(DbPath);
		});
	});

	UE_LOG(LogMonolithSource, Log, TEXT("Starting full source indexing (engine + project) via C++ indexer"));
	Indexer->StartAsync();
}

// ============================================================
// Incremental project-only reindex
// ============================================================

void UMonolithSourceSubsystem::TriggerProjectReindex()
{
	if (bIsIndexing)
	{
		UE_LOG(LogMonolithSource, Warning, TEXT("Indexing already in progress"));
		return;
	}

	FString DbPath = GetDatabasePath();

	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	if (!PlatformFile.FileExists(*DbPath))
	{
		UE_LOG(LogMonolithSource, Error, TEXT("EngineSource.db not found at %s — run full TriggerReindex() first"), *DbPath);
		return;
	}

	// Close DB during reindex
	if (Database.IsValid() && Database->IsOpen())
	{
		Database->Close();
	}

	bIsIndexing = true;

		delete Indexer;
	Indexer = new FMonolithSourceIndexer();
	// No engine source path — project only
	Indexer->SetProjectPath(GetProjectPath());
	Indexer->SetDatabasePath(DbPath);
	Indexer->SetCleanBuild(false);   // Incremental — keep existing engine symbols
	Indexer->SetIndexProjectSource(true);

	Indexer->OnComplete.AddLambda([this, DbPath](int32 Files, int32 Symbols, int32 Errors)
	{
		AsyncTask(ENamedThreads::GameThread, [this, DbPath, Files, Symbols, Errors]()
		{
			bIsIndexing = false;
			UE_LOG(LogMonolithSource, Log, TEXT("Project source indexing complete: %d files, %d symbols, %d errors"), Files, Symbols, Errors);
			ReopenDatabase(DbPath);
		});
	});

	UE_LOG(LogMonolithSource, Log, TEXT("Starting project source indexing (incremental) via C++ indexer"));
	Indexer->StartAsync();
}

// ============================================================
// Helpers
// ============================================================

void UMonolithSourceSubsystem::ReopenDatabase(const FString& DbPath)
{
	if (Database.IsValid())
	{
		IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
		if (PlatformFile.FileExists(*DbPath))
		{
			if (Database->Open(DbPath))
			{
				UE_LOG(LogMonolithSource, Log, TEXT("Engine source DB reopened: %s"), *DbPath);
			}
		}
	}
}

FString UMonolithSourceSubsystem::GetDatabasePath() const
{
	const UMonolithSettings* Settings = UMonolithSettings::Get();
	if (Settings && !Settings->EngineSourceDBPathOverride.Path.IsEmpty())
	{
		return Settings->EngineSourceDBPathOverride.Path / TEXT("EngineSource.db");
	}

	// Use the actual plugin directory so the DB lands next to the plugin regardless
	// of where it is installed.
	TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("Monolith"));
	if (Plugin.IsValid())
	{
		return Plugin->GetBaseDir() / TEXT("Saved") / TEXT("EngineSource.db");
	}

	// Fallback — should not be reached when running inside the plugin itself
	return FPaths::ProjectPluginsDir() / TEXT("Monolith") / TEXT("Saved") / TEXT("EngineSource.db");
}

FString UMonolithSourceSubsystem::GetEngineSourcePath() const
{
	const UMonolithSettings* Settings = UMonolithSettings::Get();
	if (Settings && !Settings->EngineSourcePath.Path.IsEmpty())
	{
		return Settings->EngineSourcePath.Path;
	}
	return FPaths::ConvertRelativePathToFull(FPaths::EngineDir() / TEXT("Source"));
}

FString UMonolithSourceSubsystem::GetEngineShaderPath() const
{
	return FPaths::ConvertRelativePathToFull(FPaths::EngineDir() / TEXT("Shaders"));
}

FString UMonolithSourceSubsystem::GetProjectPath() const
{
	return FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
}
