// SPDX-License-Identifier: MIT
// Plan: Plugins/Monolith/Docs/plans/2026-05-28-reflection-intelligence.md (Phase 1).
//
// FDecisionRecordIndexer — implementation. Markdown heuristic surface lifted
// straight from MONOLITH_GUIDE.md Phase 1 § Heuristic Rules:
//   - "## ADR-NNN" / "## Architectural Decision" headers     → high confidence
//   - YAML frontmatter `decision: true` (or `status: ...`)   → high confidence
//   - markdown header followed within N lines, and before the next header,
//     by a paragraph carrying a rationale marker              → medium confidence
//
// The rationale-marker rule is the loose one, so it is the one that decides how
// much ordinary prose gets misfiled as a decision. See LineHasRationaleMarker
// below for the exact marker shapes it accepts and why bare "rationale" /
// "evidence" nouns are not among them.
//
// API verifications (per Iron Law 1):
//   - FSQLitePreparedStatement Create/Execute/Bind pattern: VERIFIED at
//     MonolithIndex/Private/Indexers/MeshCatalogIndexer.cpp:36-43. We use the
//     same `Create + Execute` / `Reset + ClearBindings + BindParam + Step`
//     idiom — see MeshCatalogIndexer.cpp:88-94 for reference.
//   - IFileManager::IterateDirectoryRecursively: VERIFIED at
//     Engine/Plugins/FastBuildController/.../FastBuildUtilities.cpp:171
//     (visitor-pattern usage; sidesteps FindFilesRecursive 6th-param trap).
//   - IFileManager::GetTimeStamp: VERIFIED at
//     MonolithCore/Private/MonolithCoreModule.cpp:141 (same Monolith codebase
//     already uses this pattern).
//   - FFileHelper::LoadFileToString: existing pattern across MonolithIndex.
//   - FRegexPattern/FRegexMatcher: VERIFIED at
//     MonolithSource/Private/MonolithSourceActions.cpp:196-198.

#include "Decision/FDecisionRecordIndexer.h"
#include "Decision/DecisionSchema.h"
#include "MonolithReflectionIntelModule.h"
#include "MonolithRIMetaTable.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Internationalization/Regex.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "SQLiteDatabase.h"

namespace
{
	// ---------------------------------------------------------------------
	// Small helpers — kept anonymous to avoid leaking into module surface.
	// ---------------------------------------------------------------------

	/** True if Line looks like a markdown ATX header (`# `, `## `, …). */
	bool IsMarkdownHeader(const FString& Line, FString& OutHeaderText, int32& OutLevel)
	{
		const FString Trimmed = Line.TrimStartAndEnd();
		if (Trimmed.IsEmpty() || Trimmed[0] != TEXT('#')) { return false; }

		int32 Hashes = 0;
		while (Hashes < Trimmed.Len() && Trimmed[Hashes] == TEXT('#')) { ++Hashes; }
		if (Hashes == 0 || Hashes > 6) { return false; }
		if (Hashes < Trimmed.Len() && Trimmed[Hashes] != TEXT(' ')) { return false; }

		OutLevel = Hashes;
		OutHeaderText = Trimmed.Mid(Hashes + 1).TrimStartAndEnd();
		return !OutHeaderText.IsEmpty();
	}

	/** Slugify header text for stable anchor id. ASCII-only, lower, hyphenated. */
	FString HeaderAnchor(const FString& HeaderText)
	{
		FString Out;
		Out.Reserve(HeaderText.Len());
		bool bLastWasSep = false;
		for (TCHAR Ch : HeaderText)
		{
			if (FChar::IsAlnum(Ch))
			{
				Out += FChar::ToLower(Ch);
				bLastWasSep = false;
			}
			else if (!bLastWasSep && !Out.IsEmpty())
			{
				Out += TEXT('-');
				bLastWasSep = true;
			}
		}
		while (!Out.IsEmpty() && Out[Out.Len() - 1] == TEXT('-'))
		{
			Out.LeftChopInline(1, EAllowShrinking::No);
		}
		return Out;
	}

	/** True if Ch can be part of a word — used for whole-word marker matching. */
	bool IsWordChar(TCHAR Ch)
	{
		return FChar::IsAlnum(Ch) || Ch == TEXT('_');
	}

	/** Quote / code-span delimiters, straight and typographic. */
	bool IsQuoteChar(TCHAR Ch)
	{
		return Ch == TEXT('"') || Ch == TEXT('\'') || Ch == TEXT('`')
			|| Ch == static_cast<TCHAR>(0x2018)   // left single quote
			|| Ch == static_cast<TCHAR>(0x2019)   // right single quote
			|| Ch == static_cast<TCHAR>(0x201C)   // left double quote
			|| Ch == static_cast<TCHAR>(0x201D);  // right double quote
	}

	/**
	 * True if Line carries a rationale marker. Two marker shapes, deliberately
	 * different:
	 *
	 *   - "because" is a CONNECTIVE. It counts wherever it appears as a whole
	 *     word, because that is how rationale is actually written in prose
	 *     ("we chose X over Y because ...").
	 *   - "rationale" / "evidence" / "decision" count only in LABEL form
	 *     ("Rationale:", "Evidence:", "Decision:"). This is the same colon
	 *     discipline the marker list already applied to "decision:", now applied
	 *     consistently. As bare nouns these words are unremarkable prose ("no
	 *     rationale paragraphs", "the evidence is thin", "a design decision we
	 *     revisited") and they classify nothing while matching a great deal.
	 *
	 * In both shapes a marker immediately hugged by a quote or a backtick is
	 * being MENTIONED rather than used — documentation that describes this very
	 * heuristic quotes its own marker words — so it does not count.
	 */
	bool LineHasRationaleMarker(const FString& Line)
	{
		struct FRationaleMarker
		{
			const TCHAR* Word;
			bool bRequiresColon;
		};
		static const FRationaleMarker Markers[] =
		{
			{ TEXT("because"),   false },
			{ TEXT("rationale"), true  },
			{ TEXT("evidence"),  true  },
			{ TEXT("decision"),  true  },
		};

		const int32 LineLen = Line.Len();
		for (const FRationaleMarker& Marker : Markers)
		{
			const int32 WordLen = FCString::Strlen(Marker.Word);
			int32 At = Line.Find(Marker.Word, ESearchCase::IgnoreCase, ESearchDir::FromStart, 0);
			while (At != INDEX_NONE)
			{
				const int32 WordEnd = At + WordLen;

				// Treat the line edges as separators, not as word characters.
				const TCHAR Before = (At > 0)           ? Line[At - 1]  : TEXT('\n');
				const TCHAR After  = (WordEnd < LineLen) ? Line[WordEnd] : TEXT('\n');

				const bool bWholeWord = !IsWordChar(Before) && !IsWordChar(After);
				const bool bMentioned = IsQuoteChar(Before) || IsQuoteChar(After);
				const bool bLabelled  = !Marker.bRequiresColon || After == TEXT(':');

				if (bWholeWord && !bMentioned && bLabelled)
				{
					return true;
				}

				// WordLen >= 1, so WordEnd > At: the scan always advances.
				// FString::Find clamps StartPosition to Len()-1, so stop at the
				// end rather than re-scanning the final character forever.
				if (WordEnd >= LineLen)
				{
					break;
				}
				At = Line.Find(Marker.Word, ESearchCase::IgnoreCase, ESearchDir::FromStart, WordEnd);
			}
		}
		return false;
	}

	/**
	 * Look ahead from HeaderLineIdx for rationale markers within WindowLines.
	 *
	 * With bStopAtNextHeader the scan also stops at the next markdown header:
	 * rationale that lives under a LATER header is that section's rationale, not
	 * this one's. Unbounded, a rationale-free section is credited with its
	 * neighbour's rationale purely because the neighbour starts inside the
	 * window, which promotes the heading directly above every decision.
	 */
	bool FindRationaleWithin(const TArray<FString>& Lines, int32 HeaderLineIdx,
		int32 WindowLines, bool bStopAtNextHeader, FString& OutRationale)
	{
		const int32 End = FMath::Min(Lines.Num(), HeaderLineIdx + 1 + WindowLines);
		for (int32 i = HeaderLineIdx + 1; i < End; ++i)
		{
			if (bStopAtNextHeader)
			{
				FString NextHeaderText;
				int32 NextHeaderLevel = 0;
				if (IsMarkdownHeader(Lines[i], NextHeaderText, NextHeaderLevel))
				{
					break;
				}
			}

			if (LineHasRationaleMarker(Lines[i]))
			{
				OutRationale = Lines[i].TrimStartAndEnd();
				return true;
			}
		}
		return false;
	}

	/** Parse a leading YAML frontmatter block. Returns the (key→value) map or empty. */
	TMap<FString, FString> ParseFrontmatter(const TArray<FString>& Lines)
	{
		TMap<FString, FString> Out;
		if (Lines.Num() < 2 || Lines[0].TrimStartAndEnd() != TEXT("---")) { return Out; }

		for (int32 i = 1; i < Lines.Num(); ++i)
		{
			const FString T = Lines[i].TrimStartAndEnd();
			if (T == TEXT("---")) { break; }

			int32 ColonIdx = INDEX_NONE;
			if (T.FindChar(TEXT(':'), ColonIdx) && ColonIdx > 0)
			{
				FString Key = T.Left(ColonIdx).TrimStartAndEnd().ToLower();
				FString Val = T.Mid(ColonIdx + 1).TrimStartAndEnd();
				if (Val.StartsWith(TEXT("\"")) && Val.EndsWith(TEXT("\"")) && Val.Len() >= 2)
				{
					Val = Val.Mid(1, Val.Len() - 2);
				}
				Out.Add(Key, Val);
			}
		}
		return Out;
	}

	/** Find "supersedes" references in rationale or headers. Returns referenced ids. */
	void FindSupersedesReferences(const TArray<FString>& Lines, int32 FromIdx, int32 WindowLines,
		TArray<FString>& OutRefs)
	{
		const int32 End = FMath::Min(Lines.Num(), FromIdx + 1 + WindowLines);
		// Pattern: `supersedes: <id>` OR `supersedes [decision-id]` OR `→ supersedes <id>`
		// Tolerant — case insensitive; capture token-like substring after the keyword.
		const FRegexPattern Pattern(TEXT("(?i)supersedes[:\\s]+([\\w\\-./]+)"));

		for (int32 i = FromIdx; i < End; ++i)
		{
			FRegexMatcher Matcher(Pattern, Lines[i]);
			while (Matcher.FindNext())
			{
				const FString Captured = Matcher.GetCaptureGroup(1).TrimStartAndEnd();
				if (!Captured.IsEmpty() && Captured.Len() < 256)
				{
					OutRefs.AddUnique(Captured);
				}
			}
		}
	}
}

// ============================================================================
// Public entry
// ============================================================================

bool FDecisionRecordIndexer::Run(FSQLiteDatabase& DB, const TArray<FString>& MarkdownRoots,
	FString& OutStatus)
{
	if (!EnsureSchema(DB))
	{
		OutStatus = TEXT("FDecisionRecordIndexer: schema bootstrap failed");
		UE_LOG(LogMonolithReflectionIntel, Error, TEXT("%s"), *OutStatus);
		return false;
	}

	// Handover doc item #1 — ensure the stale-detection meta table exists.
	MonolithRIMeta::EnsureMetaTable(DB);

	const FString ProjectRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());

	// Wipe-and-rewrite — keep the table semantics simple. The corpus is small
	// (~50-500 records expected at Leviathan scale per plan §0); INSERT OR
	// REPLACE per-row would also work but the wipe makes "record removed from
	// markdown" reflect immediately.
	{
		FSQLitePreparedStatement DelRec;
		DelRec.Create(DB, TEXT("DELETE FROM decision_records;"));
		DelRec.Execute();

		FSQLitePreparedStatement DelEdge;
		DelEdge.Create(DB, TEXT("DELETE FROM decision_supersedes;"));
		DelEdge.Execute();
	}

	TArray<FDecisionRecordRow> AllRows;
	int32 FilesScanned = 0;
	int32 FileErrors = 0;

	for (const FString& RawRoot : MarkdownRoots)
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

		IPlatformFile& Pf = FPlatformFileManager::Get().GetPlatformFile();
		if (!Pf.DirectoryExists(*Root))
		{
			UE_LOG(LogMonolithReflectionIntel, Verbose,
				TEXT("DecisionIndexer: skipping non-existent root '%s'"), *Root);
			continue;
		}

		TArray<FString> Files;
		WalkMarkdownRoot(Root, Files);

		for (const FString& File : Files)
		{
			++FilesScanned;
			TArray<FDecisionRecordRow> Rows;
			if (ExtractRecordsFromFile(File, ProjectRoot, Rows))
			{
				AllRows.Append(MoveTemp(Rows));
			}
			else
			{
				++FileErrors;
			}
		}
	}

	if (!WriteRecords(DB, AllRows))
	{
		OutStatus = FString::Printf(TEXT("FDecisionRecordIndexer: write failed after %d files"),
			FilesScanned);
		return false;
	}

	if (!ResolveSupersessionEdges(DB, AllRows))
	{
		// Non-fatal — records still written.
		UE_LOG(LogMonolithReflectionIntel, Warning,
			TEXT("DecisionIndexer: supersession-edge resolution had errors"));
	}

	OutStatus = FString::Printf(
		TEXT("DecisionIndexer: %d records from %d files (%d errors)"),
		AllRows.Num(), FilesScanned, FileErrors);
	UE_LOG(LogMonolithReflectionIntel, Log, TEXT("%s"), *OutStatus);

	// Handover doc item #1 — stamp the decision code-version on success.
	MonolithRIMeta::WriteStoredVersion(DB, TEXT("decision"),
		MonolithRIMeta::GetIndexerCodeVersion(TEXT("decision")));
	return true;
}

// ============================================================================
// Schema bootstrap
// ============================================================================

bool FDecisionRecordIndexer::EnsureSchema(FSQLiteDatabase& DB)
{
	// Execute each DDL statement individually. SQLiteDatabase::Execute() runs a
	// single statement; same pattern used across MeshCatalogIndexer.cpp.
	auto Exec = [&DB](const TCHAR* Sql) -> bool
	{
		FSQLitePreparedStatement Stmt;
		if (!Stmt.Create(DB, Sql))
		{
			UE_LOG(LogMonolithReflectionIntel, Error,
				TEXT("DDL prepare failed: %s"), Sql);
			return false;
		}
		return Stmt.Execute();
	};

	if (!Exec(MonolithDecisionSchema::GetCreateRecordsTableSQL())) { return false; }
	if (!Exec(MonolithDecisionSchema::GetCreateSupersedesTableSQL())) { return false; }
	// Indices are nice-to-have; failure here is non-fatal but unlikely.
	Exec(MonolithDecisionSchema::GetCreateRecordsIndexStatusSQL());
	Exec(MonolithDecisionSchema::GetCreateRecordsIndexPathSQL());
	Exec(MonolithDecisionSchema::GetCreateSupersedesIndexToSQL());
	return true;
}

// ============================================================================
// Walk markdown roots — visitor pattern (avoids FindFilesRecursive 6th-param trap)
// ============================================================================

void FDecisionRecordIndexer::WalkMarkdownRoot(const FString& Root, TArray<FString>& OutFiles)
{
	class FMdVisitor : public IPlatformFile::FDirectoryVisitor
	{
	public:
		TArray<FString>& Out;
		explicit FMdVisitor(TArray<FString>& InOut) : Out(InOut) {}
		virtual bool Visit(const TCHAR* FilenameOrDirectory, bool bIsDirectory) override
		{
			if (!bIsDirectory)
			{
				const FString Path(FilenameOrDirectory);
				if (Path.EndsWith(TEXT(".md"), ESearchCase::IgnoreCase))
				{
					Out.Add(Path);
				}
			}
			return true;
		}
	};

	FMdVisitor Visitor(OutFiles);
	IFileManager::Get().IterateDirectoryRecursively(*Root, Visitor);
}

// ============================================================================
// Per-file extraction
// ============================================================================

bool FDecisionRecordIndexer::ExtractRecordsFromFile(const FString& AbsPath,
	const FString& ProjectRoot, TArray<FDecisionRecordRow>& OutRows)
{
	FString FileText;
	if (!FFileHelper::LoadFileToString(FileText, *AbsPath))
	{
		UE_LOG(LogMonolithReflectionIntel, Verbose,
			TEXT("DecisionIndexer: LoadFileToString failed: %s"), *AbsPath);
		return false;
	}

	TArray<FString> Lines;
	FileText.ParseIntoArrayLines(Lines, /*InCullEmpty=*/false);

	const FString RelPath = ToProjectRelative(AbsPath, ProjectRoot);
	const FDateTime Mtime = IFileManager::Get().GetTimeStamp(*AbsPath);
	const int64 MtimeUnix = (Mtime == FDateTime::MinValue()) ? 0 : Mtime.ToUnixTimestamp();

	const TMap<FString, FString> Frontmatter = ParseFrontmatter(Lines);
	const bool bFrontmatterDecision =
		(Frontmatter.Contains(TEXT("decision")) && Frontmatter[TEXT("decision")] == TEXT("true")) ||
		Frontmatter.Contains(TEXT("status"));

	// If the WHOLE FILE is a decision (frontmatter `decision: true`), emit
	// ONE record keyed on the file itself (line 1). Otherwise scan headers.
	if (bFrontmatterDecision)
	{
		FDecisionRecordRow Row;
		Row.Title = Frontmatter.Contains(TEXT("title"))
			? Frontmatter[TEXT("title")]
			: FPaths::GetBaseFilename(AbsPath);
		Row.Status = Frontmatter.Contains(TEXT("status"))
			? Frontmatter[TEXT("status")].ToLower()
			: TEXT("accepted");
		Row.SourcePath = RelPath;
		Row.SourceLine = 1;
		Row.Confidence = 0.9f;
		Row.SourceMtimeUnix = MtimeUnix;
		Row.DecisionId = MakeDecisionId(RelPath, TEXT("frontmatter"));

		// Look for inline rationale anywhere in the first 30 lines. The record
		// is already established by the frontmatter, so this is a best-effort
		// body sweep and deliberately reads across headers.
		FindRationaleWithin(Lines, 0, 30, /*bStopAtNextHeader=*/false, Row.Rationale);

		// Frontmatter `supersedes:` accepts a single id or a comma-separated list.
		if (Frontmatter.Contains(TEXT("supersedes")))
		{
			FString Csv = Frontmatter[TEXT("supersedes")];
			TArray<FString> Tokens;
			Csv.ParseIntoArray(Tokens, TEXT(","), /*bCullEmpty=*/true);
			for (FString& T : Tokens) { Row.SupersedesIds.AddUnique(T.TrimStartAndEnd()); }
		}
		// Body sweep — supports the alternate `Supersedes: foo` body line idiom.
		FindSupersedesReferences(Lines, 0, Lines.Num(), Row.SupersedesIds);

		OutRows.Add(MoveTemp(Row));
		return true;
	}

	// Header-walk path. ADR-style or rationale-followed-header.
	const FRegexPattern AdrHeaderPattern(TEXT("(?i)^#+\\s*(?:ADR[-\\s]?\\d+|Architectural\\s+Decision)\\b"));

	for (int32 i = 0; i < Lines.Num(); ++i)
	{
		FString HeaderText;
		int32 Level = 0;
		if (!IsMarkdownHeader(Lines[i], HeaderText, Level)) { continue; }

		// Skip H1 (typically the document title) unless ADR-style.
		FRegexMatcher AdrMatcher(AdrHeaderPattern, Lines[i]);
		const bool bIsAdrHeader = AdrMatcher.FindNext();
		if (Level == 1 && !bIsAdrHeader) { continue; }

		FString Rationale;
		const bool bFoundRationale = FindRationaleWithin(
			Lines, i, /*WindowLines=*/8, /*bStopAtNextHeader=*/true, Rationale);

		// Heuristic gate: emit a row only if ADR-header OR rationale-followed.
		if (!bIsAdrHeader && !bFoundRationale) { continue; }

		FDecisionRecordRow Row;
		Row.Title = HeaderText;
		Row.Status = TEXT("open");
		Row.SourcePath = RelPath;
		Row.SourceLine = i + 1;
		Row.Confidence = bIsAdrHeader ? 0.85f : 0.65f;
		Row.Rationale = Rationale;
		Row.SourceMtimeUnix = MtimeUnix;
		Row.DecisionId = MakeDecisionId(RelPath, HeaderAnchor(HeaderText));

		FindSupersedesReferences(Lines, i, /*WindowLines=*/12, Row.SupersedesIds);

		OutRows.Add(MoveTemp(Row));
	}

	return true;
}

// ============================================================================
// Writes
// ============================================================================

bool FDecisionRecordIndexer::WriteRecords(FSQLiteDatabase& DB,
	const TArray<FDecisionRecordRow>& Rows)
{
	// Single prepared statement reused across all rows — matches the
	// MeshCatalogIndexer pattern (Reset + ClearBindings + Bind + Step).
	FSQLitePreparedStatement Ins;
	if (!Ins.Create(DB, TEXT(
		"INSERT OR REPLACE INTO decision_records "
		"(decision_id, title, status, source_path, source_line, confidence, rationale, source_mtime) "
		"VALUES (?, ?, ?, ?, ?, ?, ?, ?);")))
	{
		UE_LOG(LogMonolithReflectionIntel, Error,
			TEXT("DecisionIndexer: INSERT prepare failed"));
		return false;
	}

	DB.Execute(TEXT("BEGIN TRANSACTION;"));
	int32 OkCount = 0;
	for (const FDecisionRecordRow& R : Rows)
	{
		Ins.Reset();
		Ins.ClearBindings();
		Ins.SetBindingValueByIndex(1, R.DecisionId);
		Ins.SetBindingValueByIndex(2, R.Title);
		Ins.SetBindingValueByIndex(3, R.Status);
		Ins.SetBindingValueByIndex(4, R.SourcePath);
		Ins.SetBindingValueByIndex(5, R.SourceLine);
		Ins.SetBindingValueByIndex(6, static_cast<double>(R.Confidence));
		Ins.SetBindingValueByIndex(7, R.Rationale);
		Ins.SetBindingValueByIndex(8, R.SourceMtimeUnix);
		if (Ins.Execute()) { ++OkCount; }
	}
	DB.Execute(TEXT("COMMIT;"));

	UE_LOG(LogMonolithReflectionIntel, Verbose,
		TEXT("DecisionIndexer: wrote %d / %d rows"), OkCount, Rows.Num());
	return OkCount == Rows.Num();
}

bool FDecisionRecordIndexer::ResolveSupersessionEdges(FSQLiteDatabase& DB,
	const TArray<FDecisionRecordRow>& Rows)
{
	FSQLitePreparedStatement Ins;
	if (!Ins.Create(DB, TEXT(
		"INSERT OR IGNORE INTO decision_supersedes (from_decision_id, to_decision_id) VALUES (?, ?);")))
	{
		return false;
	}

	// Build a lookup so a `supersedes: <anchor-or-id-or-title-slug>` reference
	// can resolve to a real decision_id in the corpus. Tolerant — accept the
	// full id, just the anchor (after the '#'), or the file basename suffix.
	TMap<FString, FString> AnchorToId;
	TMap<FString, FString> BasenameToId;
	for (const FDecisionRecordRow& R : Rows)
	{
		AnchorToId.Add(R.DecisionId, R.DecisionId);
		const int32 Hash = R.DecisionId.Find(TEXT("#"));
		if (Hash != INDEX_NONE)
		{
			AnchorToId.Add(R.DecisionId.Mid(Hash + 1), R.DecisionId);
		}
		const FString Base = FPaths::GetBaseFilename(R.SourcePath);
		if (!Base.IsEmpty()) { BasenameToId.Add(Base, R.DecisionId); }
	}

	DB.Execute(TEXT("BEGIN TRANSACTION;"));
	int32 Edges = 0;
	for (const FDecisionRecordRow& R : Rows)
	{
		for (const FString& RawRef : R.SupersedesIds)
		{
			FString ResolvedTo;
			if (const FString* Hit = AnchorToId.Find(RawRef)) { ResolvedTo = *Hit; }
			else if (const FString* Hit2 = BasenameToId.Find(RawRef)) { ResolvedTo = *Hit2; }
			else { ResolvedTo = RawRef; /* keep as opaque reference; may be external */ }

			Ins.Reset();
			Ins.ClearBindings();
			Ins.SetBindingValueByIndex(1, R.DecisionId);
			Ins.SetBindingValueByIndex(2, ResolvedTo);
			if (Ins.Execute()) { ++Edges; }
		}
	}
	DB.Execute(TEXT("COMMIT;"));

	UE_LOG(LogMonolithReflectionIntel, Verbose,
		TEXT("DecisionIndexer: %d supersession edges resolved"), Edges);
	return true;
}

// ============================================================================
// Helpers
// ============================================================================

FString FDecisionRecordIndexer::MakeDecisionId(const FString& RelPath, const FString& HeaderAnchorIn)
{
	// Format: <forward-slashed-rel-path>#<anchor>. Anchor empty → just the path.
	FString PathPart = RelPath;
	PathPart.ReplaceInline(TEXT("\\"), TEXT("/"));
	if (HeaderAnchorIn.IsEmpty()) { return PathPart; }
	return PathPart + TEXT("#") + HeaderAnchorIn;
}

FString FDecisionRecordIndexer::ToProjectRelative(const FString& AbsPath, const FString& ProjectRoot)
{
	FString Full = FPaths::ConvertRelativePathToFull(AbsPath);
	FString RootFull = FPaths::ConvertRelativePathToFull(ProjectRoot);
	Full.ReplaceInline(TEXT("\\"), TEXT("/"));
	RootFull.ReplaceInline(TEXT("\\"), TEXT("/"));

	if (Full.StartsWith(RootFull, ESearchCase::IgnoreCase))
	{
		FString Rel = Full.Mid(RootFull.Len());
		while (!Rel.IsEmpty() && (Rel[0] == TEXT('/') || Rel[0] == TEXT('\\'))) { Rel.RightChopInline(1); }
		return Rel;
	}
	return Full;
}
