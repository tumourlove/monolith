// SPDX-License-Identifier: MIT
//
// FScopedTestDb — RAII holder for the transient SQLite file the ReflectionIntel
// automation tests index into.
//
// The hazard it removes is fatal, not cosmetic. FSQLiteDatabase::Close() calls
// sqlite3_close(), which returns SQLITE_BUSY while any FSQLitePreparedStatement built
// on the handle is still alive. Close() then leaves the handle open, so the temp-file
// delete fails with a sharing violation (ERROR_SHARING_VIOLATION, code 32) and
// ~FSQLiteDatabase's `checkf(!Database, "...Did you forget to call Close?")` takes the
// whole editor down mid-suite. A hand-placed Close() at the end of a test body has the
// second failure mode: every early return above it skips the close entirely.
//
// Declare this holder BEFORE any prepared statement in a test. Locals are destroyed in
// reverse declaration order, so the statements are finalised first and the close then
// succeeds — on every exit path, including failures and early returns.

#pragma once

#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "SQLiteDatabase.h"

namespace MonolithReflectionIntelTests
{
	class FScopedTestDb
	{
	public:
		FScopedTestDb() = default;
		FScopedTestDb(const FScopedTestDb&) = delete;
		FScopedTestDb& operator=(const FScopedTestDb&) = delete;

		~FScopedTestDb()
		{
			Db.Close();
			if (!Path.IsEmpty())
			{
				IFileManager::Get().Delete(*Path, /*bRequireExists=*/false, /*bEvenReadOnly=*/true);
			}
		}

		/**
		 * Opens a fresh <AutomationTransientDir>/<Prefix>-<guid>.db, so a test never
		 * touches the real EngineSource.db. The path is recorded before the open attempt
		 * so a partially created file is still deleted when the open fails.
		 */
		bool Open(const TCHAR* Prefix)
		{
			const FString Dir = FPaths::AutomationTransientDir();
			FPlatformFileManager::Get().GetPlatformFile().CreateDirectoryTree(*Dir);
			Path = Dir / FString::Printf(TEXT("%s-%s.db"), Prefix, *FGuid::NewGuid().ToString());

			// Pre-delete in case a prior aborted run left a stale file.
			IFileManager::Get().Delete(*Path, /*bRequireExists=*/false, /*bEvenReadOnly=*/true);
			return Db.Open(*Path, ESQLiteDatabaseOpenMode::ReadWriteCreate);
		}

		FSQLiteDatabase& Get() { return Db; }
		const FString& GetPath() const { return Path; }

	private:
		FSQLiteDatabase Db;
		FString Path;
	};
}
