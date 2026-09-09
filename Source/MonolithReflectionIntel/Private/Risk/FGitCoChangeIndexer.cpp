// SPDX-License-Identifier: MIT
// Plan: Plugins/Monolith/Docs/plans/2026-05-28-reflection-intelligence.md (Phase 2 — v0.17.0).
//
// FGitCoChangeIndexer — implementation. Co-change mining via `git log` subprocess
// per nested-git repo. Pattern lifted directly from
// `Engine/Source/Editor/UnrealEd/Private/Commandlets/DiffAssetRegistriesCommandlet.cpp:1459-1496`
// (UDiffAssetRegistriesCommandlet::LaunchP4) — pipe + CreateProc + IsProcRunning
// + ReadPipe loop + ClosePipe + ParseIntoArrayLines. We mirror it exactly.
//
// Memory feedback cited (Phase 2 review enforcement):
//   - `.claude/rules/scoped/cpp-code.md` § Module Dependencies — no new module
//     deps; `Core` already covers FPlatformProcess + FRegex.
//   - `.claude/rules/scoped/cpp-code.md` § Known Pitfalls — caller MUST have
//     enforced `PRAGMA journal_mode=DELETE` on `DB` before invocation. The
//     module-level RunRiskIndexersOnce wrapper handles this.
//
// Phase 2 code-quality items enforced:
//   1. No static raw pointers / new+delete — DB handle is passed in by ref.
//   2. No function-static latches — module owns `bRiskBootstrapAttempted`.
//   3. PRAGMA journal_mode=DELETE — caller's job; documented above.
//   4. (No FRegex hoisting required here — only one regex use, the commit
//      header pattern, and we use string-prefix matching instead per the
//      `COMMIT <sha> <ts>` line shape.)
//   5. `ensure(IsInGameThread())` at top of Run() — see entry.
//   6. BEGIN TRANSACTION / COMMIT for batch inserts.

#include "Risk/FGitCoChangeIndexer.h"
#include "HAL/PlatformTime.h"
#include "Risk/RiskSchema.h"
#include "MonolithReflectionIntelModule.h"
#include "MonolithRIMetaTable.h"

#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformProcess.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "SQLiteDatabase.h"

namespace
{
	/** Watch ProcHandle and pull ReadPipe content until process exits OR timeout. */
	bool DrainPipeUntilExit(
		FProcHandle& Proc,
		void* ReadPipe,
		int32 TimeoutSeconds,
		FString& OutStdout,
		FString& OutErr)
	{
		const double StartSeconds = FPlatformTime::Seconds();
		while (FPlatformProcess::IsProcRunning(Proc))
		{
			OutStdout += FPlatformProcess::ReadPipe(ReadPipe);

			const double Elapsed = FPlatformTime::Seconds() - StartSeconds;
			if (Elapsed > static_cast<double>(TimeoutSeconds))
			{
				// Terminate to avoid hanging the editor. Caller will discard
				// partial output and surface the timeout error.
				FPlatformProcess::TerminateProc(Proc, /*KillTree=*/false);
				OutErr = FString::Printf(
					TEXT("git log timeout after %ds (terminated)"), TimeoutSeconds);
				return false;
			}
			// Match the LaunchP4 pattern — no Sleep; if a tight pipe-poll
			// proves costly on Live Coding rebuilds we can re-enable a tiny
			// FPlatformProcess::Sleep(0.001f). Default is to spin to completion
			// because git log on the Monolith plugin completes in well under
			// 1s in normal use.
		}

		// Final tail drain after process exit.
		OutStdout += FPlatformProcess::ReadPipe(ReadPipe);
		return true;
	}

	/**
	 * True if `RepoRoot/.git` exists. Probed as a directory OR a file: a
	 * submodule or `git worktree` checkout writes a `.git` FILE holding a
	 * `gitdir:` pointer, and `git -C <root> log` works against either form.
	 * A working tree under any other version-control system has neither.
	 */
	bool IsGitRepoRoot(const FString& RepoRoot)
	{
		IPlatformFile& Pf = FPlatformFileManager::Get().GetPlatformFile();
		const FString GitDir = RepoRoot / TEXT(".git");
		return Pf.DirectoryExists(*GitDir) || Pf.FileExists(*GitDir);
	}

	/** Normalise to forward-slashed path. */
	FString ToForwardSlashes(const FString& In)
	{
		FString Out = In;
		Out.ReplaceInline(TEXT("\\"), TEXT("/"));
		return Out;
	}

	/** Derive a repo_tag for the repo-key column — basename of the root path. */
	FString RepoTagFor(const FString& RepoRoot)
	{
		const FString Norm = ToForwardSlashes(RepoRoot);
		FString Trimmed = Norm;
		while (!Trimmed.IsEmpty() && Trimmed[Trimmed.Len() - 1] == TEXT('/'))
		{
			Trimmed.LeftChopInline(1, EAllowShrinking::No);
		}
		int32 SlashIdx = INDEX_NONE;
		Trimmed.FindLastChar(TEXT('/'), SlashIdx);
		return (SlashIdx == INDEX_NONE) ? Trimmed : Trimmed.Mid(SlashIdx + 1);
	}
}

// ============================================================================
// Public entry
// ============================================================================

bool FGitCoChangeIndexer::Run(
	FSQLiteDatabase& DB,
	const TArray<FString>& GitRepoRoots,
	int32 MaxCommitWindow,
	const TArray<FString>& NoiseFilter,
	int32 MaxCommitFileCount,
	FString& OutStatus)
{
	// Code-quality non-negotiable item 5 — game-thread enforcement. The lazy
	// bootstrap path invokes this from the action handler, which runs on the
	// game thread. If somehow called off-thread, fail loud.
	ensure(IsInGameThread());

	if (!EnsureSchema(DB))
	{
		OutStatus = TEXT("FGitCoChangeIndexer: schema bootstrap failed");
		UE_LOG(LogMonolithReflectionIntel, Error, TEXT("%s"), *OutStatus);
		return false;
	}

	// Handover doc item #1 — ensure the stale-detection meta table exists.
	// GitCoChange is one of three risk sub-indexers; we stamp the unified "risk"
	// subsystem version after THIS indexer's writes succeed (the runner only
	// returns true when all three subindexers report OK, but stamping here is
	// sufficient because the version check is just "did the parsing code shift"
	// and the runner re-fires all three on rebuild).
	MonolithRIMeta::EnsureMetaTable(DB);

	// Wipe-and-rewrite — keep semantics simple. Per-repo data dominates so the
	// repo_tag column is the natural delete key. Wipe everything; the loop
	// below re-populates per repo.
	{
		FSQLitePreparedStatement Del1;
		Del1.Create(DB, TEXT("DELETE FROM git_cochange_pairs;"));
		Del1.Execute();
		FSQLitePreparedStatement Del2;
		Del2.Create(DB, TEXT("DELETE FROM git_file_churn;"));
		Del2.Execute();
	}

	const FString ProjectRoot =
		ToForwardSlashes(FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()));

	int32 ReposScanned = 0;
	int32 ReposSkipped = 0;
	int32 TotalPairs = 0;
	int32 TotalChurnRows = 0;
	int32 ErrorRepoCount = 0;

	// Timeout — keep modest. The Monolith plugin's git log emits ~MB in <500ms
	// in normal operation; older / bigger repos may need more. Hard cap inside
	// the function rather than a settings field to avoid runaway pathologies.
	constexpr int32 GitLogTimeoutSeconds = 30;

	for (const FString& RawRoot : GitRepoRoots)
	{
		FString Root = RawRoot;
		if (FPaths::IsRelative(Root))
		{
			Root = FPaths::ConvertRelativePathToFull(ProjectRoot / Root);
		}
		else
		{
			Root = FPaths::ConvertRelativePathToFull(Root);
		}
		Root = ToForwardSlashes(Root);

		IPlatformFile& Pf = FPlatformFileManager::Get().GetPlatformFile();
		if (!Pf.DirectoryExists(*Root))
		{
			UE_LOG(LogMonolithReflectionIntel, Verbose,
				TEXT("GitCoChangeIndexer: skipping non-existent root '%s'"), *Root);
			++ReposSkipped;
			continue;
		}
		if (!IsGitRepoRoot(Root))
		{
			UE_LOG(LogMonolithReflectionIntel, Verbose,
				TEXT("GitCoChangeIndexer: skipping root '%s' "
					 "(no `.git` entry — not a git repository root)"), *Root);
			++ReposSkipped;
			continue;
		}

		TArray<FGitCommitFileTouches> Commits;
		FString Err;
		if (!RunGitLog(Root, MaxCommitWindow, GitLogTimeoutSeconds, Commits, Err))
		{
			UE_LOG(LogMonolithReflectionIntel, Warning,
				TEXT("GitCoChangeIndexer: git log failed in '%s' — %s"),
				*Root, *Err);
			++ErrorRepoCount;
			continue;
		}

		// Rebase this repository's repo-relative paths into the project-relative
		// key space BEFORE tallying, so churn keys, pair keys and the noise
		// filter all operate on the same shape FHotspotScorer joins against.
		// Doing it here rather than at write time keeps pair ordering correct:
		// a shared prefix preserves lexicographic order, but dropped rows must
		// not leave dangling pair halves behind.
		FString PrefixToAdd, PrefixToStrip;
		ComputeChurnPathRebase(Root, ProjectRoot, PrefixToAdd, PrefixToStrip);
		if (!PrefixToAdd.IsEmpty() || !PrefixToStrip.IsEmpty())
		{
			int32 DroppedOutsideProject = 0;
			for (FGitCommitFileTouches& Commit : Commits)
			{
				TArray<FString> Rebased;
				Rebased.Reserve(Commit.Files.Num());
				for (const FString& File : Commit.Files)
				{
					FString Mapped = File;
					if (!PrefixToStrip.IsEmpty())
					{
						if (!Mapped.StartsWith(PrefixToStrip, ESearchCase::IgnoreCase))
						{
							// Tracked by the enclosing repository but outside the
							// project subtree — nothing in this project can join
							// against it.
							++DroppedOutsideProject;
							continue;
						}
						Mapped.RightChopInline(PrefixToStrip.Len(), EAllowShrinking::No);
					}
					if (!PrefixToAdd.IsEmpty())
					{
						Mapped = PrefixToAdd + Mapped;
					}
					Rebased.Add(MoveTemp(Mapped));
				}
				Commit.Files = MoveTemp(Rebased);
			}
			if (DroppedOutsideProject > 0)
			{
				UE_LOG(LogMonolithReflectionIntel, Verbose,
					TEXT("GitCoChangeIndexer: '%s' — dropped %d file touches outside the project subtree"),
					*Root, DroppedOutsideProject);
			}
		}

		TMap<TPair<FString, FString>, int32> Pairs;
		TMap<FString, int32> Churn;
		TMap<FString, int64> LastTouched;
		TallyCoChangePairs(Commits, NoiseFilter, MaxCommitFileCount,
			Pairs, Churn, LastTouched);

		const FString Tag = RepoTagFor(Root);
		if (!WritePairs(DB, Tag, Pairs, Churn, LastTouched))
		{
			UE_LOG(LogMonolithReflectionIntel, Warning,
				TEXT("GitCoChangeIndexer: write failed for repo '%s'"), *Tag);
			++ErrorRepoCount;
			continue;
		}

		TotalPairs += Pairs.Num();
		TotalChurnRows += Churn.Num();
		++ReposScanned;
	}

	OutStatus = FString::Printf(
		TEXT("GitCoChangeIndexer: %d repos scanned (%d skipped, %d errors), "
			 "%d co-change pairs, %d churn rows"),
		ReposScanned, ReposSkipped, ErrorRepoCount, TotalPairs, TotalChurnRows);

	// "Mined nothing, and it was not because a repo errored" is the whole of
	// issue #119's symptom, and it used to be reported only at Verbose — which
	// is why the reporter had to read source to find out why `risk_query`
	// returned empty. Say it at Warning, with the fix attached.
	const bool bNothingToMine =
		(ReposScanned == 0) && (ReposSkipped > 0 || GitRepoRoots.Num() == 0);
	if (bNothingToMine)
	{
		OutStatus += TEXT(" | ");
		OutStatus += FMonolithReflectionIntelModule::GetRiskNoReposHint();
		UE_LOG(LogMonolithReflectionIntel, Warning, TEXT("%s"), *OutStatus);
	}
	else
	{
		UE_LOG(LogMonolithReflectionIntel, Log, TEXT("%s"), *OutStatus);
	}

	// Handover doc item #1 — stamp the risk code-version on success.
	MonolithRIMeta::WriteStoredVersion(DB, TEXT("risk"),
		MonolithRIMeta::GetIndexerCodeVersion(TEXT("risk")));

	// Stamp the CONFIGURATION fingerprint beside it (issue #119). Computed from
	// the INPUT array, unmodified — FRiskQueryAdapter::GetRawDB computes the
	// same value from ResolveGitRepoRoots' output, so writer and reader are
	// hashing byte-identical inputs through one shared function.
	MonolithRIMeta::WriteStoredVersion(DB, MonolithRIMeta::GetRiskConfigSubsystemKey(),
		MonolithRIMeta::ComputeRiskConfigFingerprint(GitRepoRoots));
	return true;
}

void FGitCoChangeIndexer::ComputeChurnPathRebase(
	const FString& AbsRepoRoot,
	const FString& AbsProjectRoot,
	FString& OutPrefixToAdd,
	FString& OutPrefixToStrip)
{
	OutPrefixToAdd.Reset();
	OutPrefixToStrip.Reset();

	auto TrimTrailingSlash = [](const FString& In)
	{
		FString Out = In;
		while (Out.Len() > 1
			&& Out.EndsWith(TEXT("/"), ESearchCase::CaseSensitive)
			&& !Out.EndsWith(TEXT(":/"), ESearchCase::CaseSensitive))
		{
			Out.LeftChopInline(1, EAllowShrinking::No);
		}
		return Out;
	};

	const FString Repo = TrimTrailingSlash(AbsRepoRoot);
	const FString Project = TrimTrailingSlash(AbsProjectRoot);

	// Repository IS the project root — `git log` paths are already
	// project-relative. Nothing to do.
	if (Repo.Equals(Project, ESearchCase::IgnoreCase))
	{
		return;
	}

	// Repository sits UNDER the project — prepend its offset, e.g. a repo at
	// <Project>/Plugins/Monolith turns `Source/X.cpp` into
	// `Plugins/Monolith/Source/X.cpp`.
	const FString ProjectWithSlash = Project + TEXT("/");
	if (Repo.StartsWith(ProjectWithSlash, ESearchCase::IgnoreCase))
	{
		OutPrefixToAdd = Repo.Mid(ProjectWithSlash.Len()) + TEXT("/");
		return;
	}

	// Repository is an ANCESTOR of the project — its paths carry the project's
	// own folder offset, e.g. `MyProject/Plugins/X/Source/Y.cpp`. Strip that
	// offset; rows that lack it are outside the project subtree and the caller
	// drops them.
	const FString RepoWithSlash = Repo + TEXT("/");
	if (Project.StartsWith(RepoWithSlash, ESearchCase::IgnoreCase))
	{
		OutPrefixToStrip = Project.Mid(RepoWithSlash.Len()) + TEXT("/");
		return;
	}

	// Unrelated absolute root — there is no project-relative form, so the paths
	// are stored exactly as the repository reports them.
}

// ============================================================================
// Schema bootstrap
// ============================================================================

bool FGitCoChangeIndexer::EnsureSchema(FSQLiteDatabase& DB)
{
	auto Exec = [&DB](const TCHAR* Sql) -> bool
	{
		FSQLitePreparedStatement Stmt;
		if (!Stmt.Create(DB, Sql))
		{
			UE_LOG(LogMonolithReflectionIntel, Error,
				TEXT("GitCoChangeIndexer DDL prepare failed: %s"), Sql);
			return false;
		}
		return Stmt.Execute();
	};

	if (!Exec(MonolithRiskSchema::GetCreateCoChangePairsTableSQL())) { return false; }
	if (!Exec(MonolithRiskSchema::GetCreateFileChurnTableSQL())) { return false; }
	// Indices are nice-to-have; failure non-fatal.
	Exec(MonolithRiskSchema::GetCreateCoChangePairsIndexFileASQL());
	Exec(MonolithRiskSchema::GetCreateCoChangePairsIndexFileBSQL());
	Exec(MonolithRiskSchema::GetCreateFileChurnIndexPathSQL());
	return true;
}

// ============================================================================
// Spawn git log
// ============================================================================

bool FGitCoChangeIndexer::RunGitLog(
	const FString& RepoRoot,
	int32 MaxCommits,
	int32 TimeoutSeconds,
	TArray<FGitCommitFileTouches>& OutCommits,
	FString& OutErr)
{
	// We can't pass --git-dir + working-tree easily via CreateProc's
	// OptionalWorkingDirectory because git's behaviour is more reliable when
	// run with -C <repo> as the first arg. That keeps the command simple
	// and portable across Windows + Linux. `--name-only` emits filenames
	// one per line; `--pretty=format:"COMMIT %H %at"` prefixes each commit
	// with a header we parse.
	const FString CommandLine = FString::Printf(
		TEXT("-C \"%s\" log --name-only --pretty=format:\"COMMIT %%H %%at\" --max-count=%d"),
		*RepoRoot, FMath::Max(MaxCommits, 1));

	void* PipeRead = nullptr;
	void* PipeWrite = nullptr;
	if (!FPlatformProcess::CreatePipe(PipeRead, PipeWrite))
	{
		OutErr = TEXT("FPlatformProcess::CreatePipe failed");
		return false;
	}

	// bLaunchDetached=false, bLaunchHidden=true, bLaunchReallyHidden=true —
	// Phase 2 §8 gotcha: Windows CreateProc pops a console window otherwise.
	// Mirrors `DiffAssetRegistriesCommandlet.cpp:1469` exactly.
	FProcHandle Proc = FPlatformProcess::CreateProc(
		TEXT("git"),
		*CommandLine,
		/*bLaunchDetached=*/false,
		/*bLaunchHidden=*/true,
		/*bLaunchReallyHidden=*/true,
		/*OutProcessID=*/nullptr,
		/*PriorityModifier=*/0,
		/*OptionalWorkingDirectory=*/nullptr,
		PipeWrite,
		/*PipeReadChild=*/nullptr);

	if (!Proc.IsValid())
	{
		FPlatformProcess::ClosePipe(PipeRead, PipeWrite);
		OutErr = TEXT("FPlatformProcess::CreateProc returned invalid handle "
			"(git not on PATH?)");
		return false;
	}

	FString Stdout;
	FString PipeErr;
	const bool bDrained = DrainPipeUntilExit(Proc, PipeRead, TimeoutSeconds, Stdout, PipeErr);

	int32 ExitCode = -1;
	FPlatformProcess::GetProcReturnCode(Proc, &ExitCode);

	FPlatformProcess::CloseProc(Proc);
	FPlatformProcess::ClosePipe(PipeRead, PipeWrite);

	if (!bDrained)
	{
		OutErr = PipeErr;
		return false;
	}
	if (ExitCode != 0)
	{
		OutErr = FString::Printf(TEXT("git log exited %d"), ExitCode);
		return false;
	}

	ParseGitLog(Stdout, OutCommits);
	return true;
}

// ============================================================================
// Parser
// ============================================================================

void FGitCoChangeIndexer::ParseGitLog(
	const FString& StdoutText,
	TArray<FGitCommitFileTouches>& OutCommits)
{
	TArray<FString> Lines;
	StdoutText.ParseIntoArrayLines(Lines, /*InCullEmpty=*/false);

	FGitCommitFileTouches Current;
	bool bInCommit = false;

	auto FlushCurrent = [&]()
	{
		if (bInCommit && !Current.CommitHash.IsEmpty())
		{
			OutCommits.Add(MoveTemp(Current));
		}
		Current = FGitCommitFileTouches();
		bInCommit = false;
	};

	for (const FString& RawLine : Lines)
	{
		const FString Line = RawLine.TrimEnd();
		if (Line.IsEmpty())
		{
			// blank line separates commit header block from file list /
			// next commit. Keep collecting files until we see a new COMMIT
			// header — blanks are harmless.
			continue;
		}

		if (Line.StartsWith(TEXT("COMMIT "), ESearchCase::CaseSensitive))
		{
			FlushCurrent();
			// Tokenise: "COMMIT <hash> <unix-ts>"
			TArray<FString> Tokens;
			Line.ParseIntoArray(Tokens, TEXT(" "), /*bCullEmpty=*/true);
			if (Tokens.Num() >= 3)
			{
				Current.CommitHash = Tokens[1];
				Current.CommitTimestamp = FCString::Atoi64(*Tokens[2]);
				bInCommit = true;
			}
			continue;
		}

		// Non-header non-blank line under an active commit = a touched file.
		if (bInCommit)
		{
			Current.Files.Add(ToForwardSlashes(Line));
		}
	}
	FlushCurrent();
}

// ============================================================================
// Tally
// ============================================================================

void FGitCoChangeIndexer::TallyCoChangePairs(
	const TArray<FGitCommitFileTouches>& Commits,
	const TArray<FString>& NoiseFilter,
	int32 MaxFiles,
	TMap<TPair<FString, FString>, int32>& OutPairs,
	TMap<FString, int32>& OutChurn,
	TMap<FString, int64>& OutLastTouched)
{
	for (const FGitCommitFileTouches& Commit : Commits)
	{
		// Skip mass commits (release, rename, bulk format) — they otherwise
		// poison co-change weights. Design spec Q6 + plan §3.
		if (MaxFiles > 0 && Commit.Files.Num() > MaxFiles)
		{
			continue;
		}

		// Filter noise files first so pair generation is on the clean subset.
		TArray<FString> CleanFiles;
		CleanFiles.Reserve(Commit.Files.Num());
		for (const FString& F : Commit.Files)
		{
			if (!IsNoise(F, NoiseFilter))
			{
				CleanFiles.Add(F);
			}
		}

		// Bump churn + last_touched.
		for (const FString& F : CleanFiles)
		{
			int32& C = OutChurn.FindOrAdd(F, 0);
			++C;
			int64& T = OutLastTouched.FindOrAdd(F, 0);
			if (Commit.CommitTimestamp > T) { T = Commit.CommitTimestamp; }
		}

		// Generate undirected pairs (file_a < file_b lexicographically) so the
		// (repo_tag, file_a, file_b) PK acts as a deterministic set key. O(N^2)
		// over a single commit's file list — already capped at MaxFiles.
		for (int32 i = 0; i < CleanFiles.Num(); ++i)
		{
			for (int32 j = i + 1; j < CleanFiles.Num(); ++j)
			{
				const FString* A = &CleanFiles[i];
				const FString* B = &CleanFiles[j];
				if (*B < *A) { Swap(A, B); }
				int32& K = OutPairs.FindOrAdd(TPair<FString, FString>(*A, *B), 0);
				++K;
			}
		}
	}
}

// ============================================================================
// Writes
// ============================================================================

bool FGitCoChangeIndexer::WritePairs(
	FSQLiteDatabase& DB,
	const FString& RepoTag,
	const TMap<TPair<FString, FString>, int32>& Pairs,
	const TMap<FString, int32>& Churn,
	const TMap<FString, int64>& LastTouched)
{
	// Batch inserts inside a transaction — code-quality item 6. Pair counts
	// can hit ~10K on the Monolith repo per plan §3, where transaction-less
	// commits would be 1000x slower.
	DB.Execute(TEXT("BEGIN TRANSACTION;"));

	int32 PairOkCount = 0;
	int32 ChurnOkCount = 0;
	bool bAllOk = true;

	{
		FSQLitePreparedStatement Ins;
		if (!Ins.Create(DB, TEXT(
			"INSERT OR REPLACE INTO git_cochange_pairs "
			"(repo_tag, file_a, file_b, count) VALUES (?, ?, ?, ?);")))
		{
			DB.Execute(TEXT("ROLLBACK;"));
			UE_LOG(LogMonolithReflectionIntel, Error,
				TEXT("GitCoChangeIndexer: pairs INSERT prepare failed"));
			return false;
		}
		for (const TPair<TPair<FString, FString>, int32>& E : Pairs)
		{
			Ins.Reset();
			Ins.ClearBindings();
			Ins.SetBindingValueByIndex(1, RepoTag);
			Ins.SetBindingValueByIndex(2, E.Key.Key);
			Ins.SetBindingValueByIndex(3, E.Key.Value);
			Ins.SetBindingValueByIndex(4, E.Value);
			if (Ins.Execute()) { ++PairOkCount; }
			else
			{
				bAllOk = false;
				UE_LOG(LogMonolithReflectionIntel, Verbose,
					TEXT("GitCoChangeIndexer: pair INSERT failed for (%s,%s)"),
					*E.Key.Key, *E.Key.Value);
			}
		}
	}

	{
		FSQLitePreparedStatement Ins;
		if (!Ins.Create(DB, TEXT(
			"INSERT OR REPLACE INTO git_file_churn "
			"(repo_tag, file_path, commit_count, last_touched) "
			"VALUES (?, ?, ?, ?);")))
		{
			DB.Execute(TEXT("ROLLBACK;"));
			UE_LOG(LogMonolithReflectionIntel, Error,
				TEXT("GitCoChangeIndexer: churn INSERT prepare failed"));
			return false;
		}
		for (const TPair<FString, int32>& E : Churn)
		{
			Ins.Reset();
			Ins.ClearBindings();
			Ins.SetBindingValueByIndex(1, RepoTag);
			Ins.SetBindingValueByIndex(2, E.Key);
			Ins.SetBindingValueByIndex(3, E.Value);
			const int64* TouchedPtr = LastTouched.Find(E.Key);
			Ins.SetBindingValueByIndex(4, TouchedPtr ? *TouchedPtr : int64(0));
			if (Ins.Execute()) { ++ChurnOkCount; }
			else
			{
				bAllOk = false;
				UE_LOG(LogMonolithReflectionIntel, Verbose,
					TEXT("GitCoChangeIndexer: churn INSERT failed for %s"), *E.Key);
			}
		}
	}

	DB.Execute(TEXT("COMMIT;"));

	UE_LOG(LogMonolithReflectionIntel, Verbose,
		TEXT("GitCoChangeIndexer: repo=%s pairs=%d/%d churn=%d/%d"),
		*RepoTag, PairOkCount, Pairs.Num(), ChurnOkCount, Churn.Num());

	return bAllOk;
}

bool FGitCoChangeIndexer::IsNoise(const FString& Path, const TArray<FString>& NoiseFilter)
{
	for (const FString& Frag : NoiseFilter)
	{
		if (Frag.IsEmpty()) { continue; }
		if (Path.Contains(Frag, ESearchCase::IgnoreCase))
		{
			return true;
		}
	}
	return false;
}
