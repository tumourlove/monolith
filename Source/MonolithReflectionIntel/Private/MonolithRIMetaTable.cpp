// SPDX-License-Identifier: MIT
//
// MonolithRIMetaTable — implementation. See header for the stale-detection
// contract. Plain SQLite ops; caller is expected to hold the shared DB lock
// (the RI module's borrow path always holds the FMonolithSourceDatabase lock
// for the full borrow window).

#include "MonolithRIMetaTable.h"
#include "MonolithReflectionIntelModule.h"
#include "MonolithReflectionIntelSettings.h"

#include "Misc/Crc.h"
#include "Misc/DateTime.h"
#include "SQLiteDatabase.h"

namespace MonolithRIMeta
{
	int32 GetIndexerCodeVersion(const FString& Subsystem)
	{
		// Bump the literal for a subsystem when you change its parser logic or
		// row shape. Auto-rebuild kicks in on the next adapter call. Start every
		// subsystem at integer 1 (handover doc spec).
		if (Subsystem.Equals(TEXT("cppreflect"), ESearchCase::CaseSensitive)) { return 1; }
		if (Subsystem.Equals(TEXT("network"),    ESearchCase::CaseSensitive)) { return 1; }
		// risk 1 -> 2: churn keys changed shape. They used to be REPO-relative
		// (`Source/...` for a nested plugin repo) while the hotspot join keys are
		// project-relative (`Plugins/Monolith/Source/...`), so the two maps never
		// met and every scored file landed with one signal zeroed. Rows written
		// by version 1 are unusable against the version-2 join.
		if (Subsystem.Equals(TEXT("risk"),       ESearchCase::CaseSensitive)) { return 2; }
		// decision 1 -> 2: the rationale-marker heuristic tightened. Version 1
		// accepted a bare "rationale"/"evidence" noun anywhere in an 8-line
		// window that ran past the next header, so ordinary prose sections were
		// indexed as decisions. Version-1 rows are those false positives.
		if (Subsystem.Equals(TEXT("decision"),   ESearchCase::CaseSensitive)) { return 2; }
		return 0;
	}

	const TCHAR* GetRiskConfigSubsystemKey()
	{
		return TEXT("risk.config");
	}

	int32 ComputeRiskConfigFingerprint(const TArray<FString>& ResolvedGitRepoRoots)
	{
		// Chained FCrc::StrCrc32<TCHAR>: each call seeds from the previous CRC,
		// so the result is order-sensitive across the whole input sequence.
		// ResolveGitRepoRoots emits a deterministic order (override order, then
		// project root, then sorted plugin folders), so an unchanged
		// configuration always produces an unchanged fingerprint.
		uint32 Crc = 0;
		for (const FString& Root : ResolvedGitRepoRoots)
		{
			Crc = FCrc::StrCrc32<TCHAR>(*Root, Crc);
			// Separator so ["ab","c"] and ["a","bc"] cannot collide.
			Crc = FCrc::StrCrc32<TCHAR>(TEXT("\x1f"), Crc);
		}

		if (const UMonolithReflectionIntelSettings* Settings = UMonolithReflectionIntelSettings::Get())
		{
			Crc = FCrc::StrCrc32<TCHAR>(*FString::FromInt(Settings->MaxCoChangeWindowCommits), Crc);
			Crc = FCrc::StrCrc32<TCHAR>(TEXT("\x1f"), Crc);
			Crc = FCrc::StrCrc32<TCHAR>(*FString::FromInt(Settings->MaxCommitFileCount), Crc);
			Crc = FCrc::StrCrc32<TCHAR>(TEXT("\x1f"), Crc);
			for (const FString& Fragment : Settings->GitMiningNoiseFilter)
			{
				Crc = FCrc::StrCrc32<TCHAR>(*Fragment, Crc);
				Crc = FCrc::StrCrc32<TCHAR>(TEXT("\x1f"), Crc);
			}
		}

		// See the header: the int32 cast lives here and ONLY here, so the writer
		// and the reader cannot disagree about the stored width.
		return static_cast<int32>(Crc);
	}

	bool EnsureMetaTable(FSQLiteDatabase& DB)
	{
		FSQLitePreparedStatement Stmt;
		if (!Stmt.Create(DB, TEXT(
			"CREATE TABLE IF NOT EXISTS monolith_ri_meta ("
			"    subsystem    TEXT PRIMARY KEY,"
			"    code_version INTEGER NOT NULL,"
			"    stamped_at   TEXT NOT NULL"
			");")))
		{
			UE_LOG(LogMonolithReflectionIntel, Warning,
				TEXT("MonolithRIMeta: CREATE TABLE prepare failed for monolith_ri_meta"));
			return false;
		}
		return Stmt.Execute();
	}

	bool ReadStoredVersion(FSQLiteDatabase& DB, const FString& Subsystem, int32& OutVersion)
	{
		// Tolerate a missing table — that is exactly the "stale / never stamped"
		// signal the caller wants treated as "rebuild".
		FSQLitePreparedStatement Stmt;
		if (!Stmt.Create(DB, TEXT(
			"SELECT code_version FROM monolith_ri_meta WHERE subsystem = ? LIMIT 1;")))
		{
			return false;
		}
		Stmt.SetBindingValueByIndex(1, Subsystem);
		if (Stmt.Step() != ESQLitePreparedStatementStepResult::Row)
		{
			return false;
		}
		int32 Version = 0;
		Stmt.GetColumnValueByIndex(0, Version);
		OutVersion = Version;
		return true;
	}

	bool WriteStoredVersion(FSQLiteDatabase& DB, const FString& Subsystem, int32 Version)
	{
		if (!EnsureMetaTable(DB))
		{
			return false;
		}

		// ISO-8601 UTC timestamp keeps `stamped_at` human-readable + sort-stable.
		const FString Now = FDateTime::UtcNow().ToIso8601();

		// INSERT OR REPLACE keeps PK semantics correct (single row per subsystem)
		// and matches the dominant idiom used by the Phase 3a/4a writers (see
		// FUHTArtefactReader::WriteBatch). UPSERT also works on UE 5.7's bundled
		// SQLite 3.24+, but the OR-REPLACE form is the project's house style.
		FSQLitePreparedStatement Stmt;
		if (!Stmt.Create(DB, TEXT(
			"INSERT OR REPLACE INTO monolith_ri_meta "
			"(subsystem, code_version, stamped_at) VALUES (?, ?, ?);")))
		{
			UE_LOG(LogMonolithReflectionIntel, Warning,
				TEXT("MonolithRIMeta: UPSERT prepare failed for subsystem '%s'"), *Subsystem);
			return false;
		}
		Stmt.SetBindingValueByIndex(1, Subsystem);
		Stmt.SetBindingValueByIndex(2, Version);
		Stmt.SetBindingValueByIndex(3, Now);
		return Stmt.Execute();
	}
}
