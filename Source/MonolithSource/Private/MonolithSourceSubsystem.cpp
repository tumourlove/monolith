#include "MonolithSourceSubsystem.h"
#include "MonolithSettings.h"
#include "Misc/Paths.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformProcess.h"
#include "Async/Async.h"

void UMonolithSourceSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

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
	if (Database.IsValid())
	{
		Database->Close();
	}
	Super::Deinitialize();
}

void UMonolithSourceSubsystem::TriggerReindex()
{
	if (bIsIndexing)
	{
		UE_LOG(LogMonolithSource, Warning, TEXT("Indexing already in progress"));
		return;
	}

	FString PythonExe = FindPython();
	if (PythonExe.IsEmpty())
	{
		UE_LOG(LogMonolithSource, Error, TEXT("Could not find python/python3 in PATH"));
		return;
	}

	FString ScriptsDir = FPaths::ProjectPluginsDir() / TEXT("Monolith") / TEXT("Scripts");
	FString DbPath = GetDatabasePath();
	FString SourcePath = GetEngineSourcePath();
	FString ShaderPath = GetEngineShaderPath();

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

	FString Args = FString::Printf(
		TEXT("-m source_indexer --source \"%s\" --db \"%s\""),
		*SourcePath, *DbPath
	);
	if (!ShaderPath.IsEmpty())
	{
		Args += FString::Printf(TEXT(" --shaders \"%s\""), *ShaderPath);
	}

	UE_LOG(LogMonolithSource, Log, TEXT("Starting engine source indexing: %s %s"), *PythonExe, *Args);

	// Run in background thread
	AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [this, PythonExe, Args, ScriptsDir, DbPath]()
	{
		FString StdOut, StdErr;
		int32 ReturnCode = -1;

		// Run from Scripts dir so "python -m source_indexer" works
		FPlatformProcess::ExecProcess(
			*PythonExe,
			*Args,
			&ReturnCode,
			&StdOut,
			&StdErr,
			*ScriptsDir
		);

		AsyncTask(ENamedThreads::GameThread, [this, ReturnCode, StdOut, StdErr, DbPath]()
		{
			bIsIndexing = false;

			if (ReturnCode == 0)
			{
				UE_LOG(LogMonolithSource, Log, TEXT("Engine source indexing complete: %s"), *StdOut);
			}
			else
			{
				IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
				if (PF.FileExists(*DbPath))
				{
					UE_LOG(LogMonolithSource, Warning, TEXT("Engine source indexing exited with code %d (DB exists, built externally): %s"), ReturnCode, *StdErr);
				}
				else
				{
					UE_LOG(LogMonolithSource, Error, TEXT("Engine source indexing failed with code %d and no DB was produced: %s"), ReturnCode, *StdErr);
				}
			}

			// Always reopen the DB — it may have been built/updated externally (e.g. index_project.py)
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
		});
	});
}

FString UMonolithSourceSubsystem::GetDatabasePath() const
{
	const UMonolithSettings* Settings = UMonolithSettings::Get();
	if (Settings && !Settings->EngineSourceDBPathOverride.Path.IsEmpty())
	{
		return Settings->EngineSourceDBPathOverride.Path / TEXT("EngineSource.db");
	}
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

FString UMonolithSourceSubsystem::FindPython() const
{
	// Try python3 first, then python — just use the name directly,
	// FPlatformProcess::ExecProcess resolves from PATH
	TArray<FString> Candidates = { TEXT("python3"), TEXT("python") };
	for (const FString& Candidate : Candidates)
	{
		FString StdOut, StdErr;
		int32 ReturnCode = -1;
		FPlatformProcess::ExecProcess(*Candidate, TEXT("--version"), &ReturnCode, &StdOut, &StdErr);
		if (ReturnCode == 0)
		{
			return Candidate;
		}
	}
	return TEXT("python");
}
