#pragma once

#include "CoreMinimal.h"
#include "EditorSubsystem.h"
#include "MonolithSourceDatabase.h"
#include "MonolithSourceSubsystem.generated.h"

class FMonolithSourceIndexer;
// UE 5.7: declared without underlying type at UObject/UObjectGlobals.h:3216 — match exactly.
enum class EReloadCompleteReason;

/**
 * Editor subsystem that owns the engine source DB and triggers C++ source indexing.
 */
UCLASS()
class MONOLITHSOURCE_API UMonolithSourceSubsystem : public UEditorSubsystem
{
	GENERATED_BODY()

public:
	virtual ~UMonolithSourceSubsystem();
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	/** Get the source database (read-only). May be null if DB doesn't exist. */
	FMonolithSourceDatabase* GetDatabase() { return Database.IsValid() ? Database.Get() : nullptr; }

	/** Full reindex: engine + shaders + project source (clean build). */
	void TriggerReindex();

	/** Incremental project-only reindex: loads existing engine symbols, indexes only project C++ source. */
	void TriggerProjectReindex();

	/** Is indexing currently running? */
	bool IsIndexing() const { return bIsIndexing; }

private:
	FString GetDatabasePath() const;
	FString GetEngineSourcePath() const;
	FString GetEngineShaderPath() const;
	FString GetProjectPath() const;
	void ReopenDatabase(const FString& DbPath);

	/**
	 * F17 (2026-04-26): Auto-reindex hook. Fires when Live Coding / hot-reload completes.
	 * Kicks an incremental project-only reindex so newly-shipped C++ symbols become
	 * queryable via source_query without requiring a manual `source.trigger_project_reindex` call.
	 *
	 * Cooldown-guarded (LastReindexTimeSeconds + 5s) and idempotency-guarded
	 * (bIsIndexing) to prevent storming when UBT fires multiple reload signals back-to-back.
	 */
	void OnReloadComplete(EReloadCompleteReason Reason);

	TUniquePtr<FMonolithSourceDatabase> Database;
	FMonolithSourceIndexer* Indexer = nullptr;
	TAtomic<bool> bIsIndexing{false};

	/** F17: Handle into FCoreUObjectDelegates::ReloadCompleteDelegate; cleared on Deinitialize. */
	FDelegateHandle ReloadCompleteHandle;

	/** F17: FPlatformTime::Seconds() at last successful auto-kick — used for the 5s cooldown. */
	double LastReindexTimeSeconds = 0.0;
};
