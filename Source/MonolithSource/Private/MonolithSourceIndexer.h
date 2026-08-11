#pragma once

#include "CoreMinimal.h"
#include "HAL/ThreadSafeBool.h"
#include "HAL/Runnable.h"

class FMonolithSourceDatabase;

DECLARE_MULTICAST_DELEGATE_FiveParams(FOnSourceIndexProgress,
	const FString& /* ModuleName */, int32 /* Current */, int32 /* Total */,
	int32 /* FilesProcessed */, int32 /* SymbolsExtracted */);

DECLARE_MULTICAST_DELEGATE_ThreeParams(FOnSourceIndexComplete,
	int32 /* FilesProcessed */, int32 /* SymbolsExtracted */, int32 /* Errors */);

struct FSourceIndexDiagnostics
{
	int32 ForwardDecls = 0;
	int32 Definitions = 0;
	int32 WithBaseClasses = 0;
	int32 InheritanceResolved = 0;
	int32 InheritanceFailed = 0;
};

class FMonolithSourceIndexer : public FRunnable
{
public:
	FMonolithSourceIndexer();
	~FMonolithSourceIndexer();

	// Configuration — set before calling StartAsync/RunSynchronous
	void SetSourcePath(const FString& InPath);
	void SetShaderPath(const FString& InPath);
	void SetProjectPath(const FString& InPath);
	void SetDatabasePath(const FString& InPath);
	void SetCleanBuild(bool bClean);
	void SetIndexProjectSource(bool bIndex);

	/** Start indexing on a background thread. Non-blocking. */
	bool StartAsync();

	/** Run synchronously (blocks calling thread). For commandlet use. */
	bool RunSynchronous();

	/** Request cancellation. */
	void RequestStop();

	bool IsRunning() const;

	/** Thread-safe — locks DiagLock before reading. */
	FSourceIndexDiagnostics GetDiagnostics() const;

	FOnSourceIndexProgress OnProgress;
	FOnSourceIndexComplete OnComplete;

	// FRunnable
	virtual bool Init() override;
	virtual uint32 Run() override;
	virtual void Stop() override;

private:
	struct FModuleEntry
	{
		FString Path;
		FString Name;
		FString Type; // Runtime, Editor, Developer, Programs, Plugin, Shaders, Project, GameFeature
		FString BuildCsPath; // Decision 2: derived <Module>.Build.cs path (empty for Shaders / when absent)
	};

	void DiscoverEngineModules(TArray<FModuleEntry>& OutModules);
	void DiscoverProjectModules(TArray<FModuleEntry>& OutModules);
	void IndexModule(const FModuleEntry& Module, FMonolithSourceDatabase& DB);
	int32 IndexCppFile(const FString& FilePath, int64 ModuleId, FMonolithSourceDatabase& DB);
	int32 IndexShaderFile(const FString& FilePath, int64 ModuleId, FMonolithSourceDatabase& DB);
	void Finalize(FMonolithSourceDatabase& DB);

	/**
	 * Single exit point for a run: clears the running flag and broadcasts
	 * OnComplete. Pass a reason to record a failed run (logged as an error and
	 * counted), or nullptr for a normal completion. Every exit must go through
	 * here — an exit that skips the broadcast leaves the owning subsystem's
	 * bIsIndexing latched, and it then refuses every later request until the
	 * editor restarts.
	 */
	void CompleteRun(const TCHAR* FailureReason);

	// Symbol tracking (accumulated during indexing)
	void UpdateSymbolMap(const FString& Name, int64 SymId, int32 LineStart, int32 LineEnd);
	void UpdateClassMap(const FString& Name, int64 SymId, int32 LineStart, int32 LineEnd);

	// Config
	FString SourcePath;
	FString ShaderPath;
	FString ProjectPath;
	FString DbPath;
	bool bCleanBuild = true;
	bool bIndexProjectSource = false;

	// Thread state
	FThreadSafeBool bShouldStop;
	FThreadSafeBool bIsRunning;
	FRunnableThread* Thread = nullptr;

	// Symbol maps
	TMap<FString, int64> SymbolNameToId;
	TMap<FString, TPair<int32, int32>> SymbolSpans;
	TMap<FString, int64> ClassNameToId;
	TMap<FString, TPair<int32, int32>> ClassSpans;
	TMap<FString, TArray<FString>> PendingBaseClasses; // className -> [baseClassName, ...]
	TSet<int64> NewFileIds;

	// Stats
	mutable FCriticalSection DiagLock;
	FSourceIndexDiagnostics Diagnostics;
	TAtomic<int32> TotalFilesProcessed{0};
	TAtomic<int32> TotalSymbolsExtracted{0};
	TAtomic<int32> TotalErrors{0};
};
