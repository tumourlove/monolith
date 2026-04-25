#include "MonolithSourceSubsystem.h"
#include "MonolithSourceIndexer.h"
#include "MonolithSettings.h"
#include "Misc/Paths.h"
#include "HAL/PlatformFileManager.h"
#include "Async/Async.h"
#include "Interfaces/IPluginManager.h"

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
}

void UMonolithSourceSubsystem::Deinitialize()
{
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
