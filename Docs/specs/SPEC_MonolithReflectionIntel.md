# Monolith — MonolithReflectionIntel Module

**Parent:** [SPEC_CORE.md](../SPEC_CORE.md)
**Engine:** Unreal Engine 5.7+
**Version:** 0.21.3 (Beta) — actions shipped across Phases 1–4a (decision + risk + a source-namespace module-dep audit + cppreflect + network + pipeline + audit actions on existing namespaces: material/niagara/blueprint/project), plus the `cppreflect_query("list_class_specifiers")` and `reflect_query("rebuild_reflection_index")` (new `reflect` namespace, WRITE/maintenance) follow-ups. Counts are approximate — query `monolith_discover()` for the live figure. The network-completeness workstream also makes `list_replicated_classes` capture bare `UPROPERTY(Replicated)` (now WORKS, verified E2E), switches `list_rpc_functions` to specifier-based detection, and widens the indexer scan scope from the game module alone to an `IPluginManager`-driven ladder — game module + project plugins by default, marketplace plugins gated, Epic engine built-ins excluded (§5.2). With project plugins in scope, `list_rpc_functions` now returns the project's actual RPCs (the project's project-plugin Server RPCs, verified E2E) — the prior "empty due to game-module-only scan scope" limitation is resolved.

---

## 1. Purpose

`MonolithReflectionIntel` is a deterministic, $0-LLM intelligence layer that mines high-signal facts out of the project's own artefacts (markdown, git history, C++, AssetRegistry) and exposes them as MCP query actions. It exists to give AI agents structured answers to questions the project itself already knows the answer to — without spending tokens re-deriving them from raw source.

Phases 1, 2, 3a, and 4a fold into the same v0.17.0 release. Phase 1 ships the **Decision Intelligence** slice — architectural decision records mined from the project's markdown corpora (specs, plans, CHANGELOG, `.claude/rules/`) and served through the `decision_query` namespace (5 actions). Phase 2 ships the **Risk Intelligence** slice — git-log mining + conditional-gate inventory served through a new `risk_query` namespace (5 actions), plus a **Module-Dep Reality Audit** that registers a single audit action onto the existing `source_query` namespace. Phase 3a ships the **CppReflect Intelligence** slice — UE 5.7 reflection-edge queries served through a new `cppreflect_query` namespace (5 actions at v0.17.0 ship; 6 with the [Unreleased] `list_class_specifiers` follow-up), driven by direct reads of UHT artefacts (`Intermediate/Build/.../UHT/*.gen.cpp`) cross-joined with `IAssetRegistry`. Phase 4a ships the **Network Intelligence** slice — replication inspection served through a new `network_query` namespace (4 actions), the **Pipeline Composers** slice — PR-review + release-readiness composers served through a new `pipeline_query` namespace (2 actions), and 4 read-only audit actions registered onto the existing `material` / `niagara` / `blueprint` / `project` namespaces. The [Unreleased] **network-completeness workstream** adds a new `reflect_query` namespace (1 WRITE/maintenance action — `rebuild_reflection_index`, see §6b), makes `list_replicated_classes` capture bare `UPROPERTY(Replicated)` (now works, verified E2E), switches `list_rpc_functions` to specifier-based detection, and widens the indexer scan scope via an `IPluginManager`-driven ladder — game module + project plugins by default (default-on `bIndexProjectPluginReflection`), enabled marketplace plugins gated behind `bIndexMarketplacePluginReflection`, Epic engine built-ins excluded — so `list_rpc_functions` now returns project-plugin RPCs (the project's project-plugin Server RPCs, verified E2E) instead of coming back empty (see §5.2 + §6.5). Phase 3b (tree-sitter integration for native gameplay-tag declaration tracking) and Phase 4b (gas_query tag-graph audits + animation_query thread-safety audit) are deferred — both depend on tree-sitter substrate landing.

### Roadmap

| Phase | Status | Surface | Substrate |
|-------|--------|---------|-----------|
| 1 — Decision Intelligence | **shipped v0.17.0** | `decision_query` (5 actions) | Markdown heuristic harvest |
| 2 — Risk Intelligence | **shipped v0.17.0** | `risk_query` (5 actions) + `source_query("audit_module_dep_reality")` (1 audit action) | Git log subprocess + LOC sweep + regex over `#if WITH_*` / `bHas*` + Build.cs parsing against `EngineSource.db` symbol resolution |
| 3a — CppReflect Intelligence | **shipped v0.17.0** (6 actions incl. [Unreleased] `list_class_specifiers`) | `cppreflect_query` (6 actions) + cpp↔asset edges | UHT artefact regex sweep over `Intermediate/Build/.../UHT/*.gen.cpp` + `IAssetRegistry` asset-graph joiner — NO tree-sitter dependency |
| 3b — Native Tag Tracking | `(WISHLIST)` | `cppreflect_query("list_native_tags")` (1 action) + 2 tag tables | tree-sitter-unreal-cpp on `.cpp` / `.h` for native `UE_DEFINE_GAMEPLAY_TAG_*` / `extern FGameplayTag` mining |
| 4a — Network Intelligence + Audits + Pipelines | **shipped v0.17.0** | `network_query` (4 actions) + `pipeline_query` (2 actions) + `material_query("audit_orphan_materials")` + `niagara_query("audit_cross_asset_refs")` + `blueprint_query("audit_cdo_drift")` + `project_query("audit_orphan_assets")` + `reflect_replicated_properties` SQLite table | Second UHT-artefact sweep (independent of Phase 3a's reader) for per-property `MetaData` blocks carrying `ReplicatedUsing` tags; composed reads against Phases 1/2/3a tables + `IAssetRegistry` for the 4 cross-namespace audits; composer reads-only |
| Network completeness | **[Unreleased]** | new `reflect_query("rebuild_reflection_index")` (1 WRITE/maintenance action — §6b) + `list_replicated_classes` bare-`Replicated` capture (WORKS, verified E2E) + `list_rpc_functions` specifier-based detection (WORKS — covers project plugins by default, returns the project's project-plugin RPCs) + `IPluginManager`-driven scan-scope ladder (game module + project plugins default-on; marketplace gated; Epic built-ins excluded — §5.2) | Project-plugin-aware force-rebuild of the RI reflection tables; `CPF_Net` property-flag sweep for bare-`Replicated`; `reflect_ufunctions.specifiers` from `EFunctionFlags` for RPC kind |
| 4b — Tag-graph + thread-safety audits | `(WISHLIST)` | `gas_query("find_tag_consumers" / "find_grant_paths" / "find_revoke_paths")` + `animation_query("audit_thread_safety")` | Both need Phase 3b's tree-sitter substrate: gas tag-graph queries depend on native-tag tracking; animation thread-safety audit depends on Phase 3b specifier population. (Bare-Replicated detection LANDED in the [Unreleased] network-completeness workstream — no longer a 4b item.) |

The phases are independent (Phase 2 does not depend on Phase 1; Phase 3a does not depend on Phase 2; Phase 4a depends on Phase 3a reflection-edge tables for `network_query` and on the Phase 1+2+3a substrate for the pipeline composers). Phases 1 + 2 + 3a + 4a co-shipped in v0.17.0. Phases 3b and 4b are deferred — both depend on tree-sitter substrate; rationales in §5b and §9 below.

---

## 2. Module Architecture

**Type:** `Editor`
**Loading phase:** `Default`
**Public namespaces owned by this module:** `decision` (5 actions, Phase 1) + `risk` (5 actions, Phase 2) + `cppreflect` (6 actions — 5 Phase 3a + 1 [Unreleased] `list_class_specifiers`) + `network` (4 actions, Phase 4a) + `pipeline` (2 actions, Phase 4a) + `reflect` (1 action, [Unreleased] — `rebuild_reflection_index`, the network-completeness maintenance verb; see §6b). Phase 2 additionally registers one audit action onto the **existing** `source` namespace owned by `MonolithSource` (`source_query("audit_module_dep_reality")`). Phase 4a additionally registers four audit actions onto **existing** host namespaces — `material_query("audit_orphan_materials")`, `niagara_query("audit_cross_asset_refs")`, `blueprint_query("audit_cdo_drift")`, `project_query("audit_orphan_assets")`. All cross-namespace audit handlers live in `MonolithReflectionIntel` but register against their host dispatchers for caller ergonomics — agents already discover `material_query` / `niagara_query` / `blueprint_query` / `project_query` / `source_query` first.

`MonolithReflectionIntel` is a self-contained editor module. Phase 1 owns one indexer worker (`FDecisionRecordIndexer`), one query adapter (`FDecisionQueryAdapter`), one settings UCLASS (`UMonolithReflectionIntelSettings`), and a SQLite schema fragment (`MonolithDecisionSchema` namespace). Phase 2 adds three indexer workers (`FGitChurnIndexer`, `FGitCoChangeIndexer`, `FConditionalGateIndexer`), two query adapters (`FRiskQueryAdapter`, `FModuleDepRealityAdapter`), and a second SQLite schema fragment (`MonolithRiskSchema` namespace) sharing `EngineSource.db`. Phase 3a adds one indexer worker (`FCppReflectIndexer` — UHT-artefact regex sweep + `IAssetRegistry` asset-graph joiner), one query adapter (`FCppReflectQueryAdapter`), and a third SQLite schema fragment (`MonolithCppReflectSchema` namespace) sharing the same `EngineSource.db`. Phase 4a adds one indexer worker (`FNetworkIndexer` — second UHT-artefact sweep over per-property `MetaData` blocks), two query adapters (`FNetworkQueryAdapter`, `FPipelineQueryAdapter`), four cross-namespace audit handlers registered against `material` / `niagara` / `blueprint` / `project` host adapters, and a fourth SQLite schema fragment (`MonolithNetworkSchema` namespace) sharing the same `EngineSource.db`.

### Lazy bootstrap

The module does NOT eagerly run the indexer on `StartupModule`. Two reasons:

1. UE 5.7's SQLite is built with `SQLITE_OS_OTHER=1` and a custom `unreal-fs` VFS that permits only ONE open of a given file per process — a second open of `EngineSource.db` returns `SQLITE_IOERR`. RI therefore borrows `UMonolithSourceSubsystem`'s already-open handle rather than opening its own; that handle must exist before RI can read, which the lazy path guarantees.
2. The decision corpus is small (~50–500 records at typical project scale) and tolerates lazy first-call cost.

Bootstrap fires on two events:

- **First `decision_query` call** — `FDecisionQueryAdapter` checks for the `decision_records` table via the borrowed shared handle (`FMonolithSourceDatabase::GetRawHandle()`); if missing, it calls `FMonolithReflectionIntelModule::RunDecisionIndexerOnce` under `FScopeLock(&SharedDb->GetLock())`, which ensures schema and writes rows on the shared handle. (A brief private ReadWrite open is used only in the narrow window where the subsystem's handle is closed during a reindex.) Subsequent calls read directly through the borrowed handle.
- **`FCoreUObjectDelegates::ReloadCompleteDelegate`** — bound at `StartupModule`. After Live Coding or UBT-driven hot-reload, the corpus refreshes automatically so agents see decisions added to spec files in the current session without manually triggering a re-index.

The `RunDecisionIndexerOnce` entry point is idempotent — calling it repeatedly is cheap (one wipe-and-rewrite per call) and safe.

### Shutdown

`ShutdownModule` unbinds the reload delegate and calls `FMonolithToolRegistry::UnregisterNamespace("decision")` so dispatcher state stays clean on editor exit.

### Shared internal helpers (unity-safe)

The cursor-pagination codec and project-relative path helper were duplicated per-adapter across the six query adapters (each `.cpp` carried its own anonymous-namespace copy), which collided under full unity builds (same-module `.cpp`s concatenate into one translation unit). They are now consolidated into module-internal shared units — `Private/Shared/RICursorCodec.{h,cpp}` (`FRICursorState` + `Encode`/`DecodeRICursor` + `RIComputeFilterHash` + `RIInvalidCursorError`) and `Private/Shared/RIPathUtils.{h,cpp}` (`RIToProjectRelative`) — landing the "Phase 5+" codec consolidation the per-adapter source comments had flagged. Internal linkage only; no namespace or action-surface change.

---

## 3. Decision Intelligence (Phase 1 — SHIPPED v0.17.0)

### 3.1 Markdown corpus harvest scope

The indexer walks `*.md` files recursively under each configured markdown root via `IFileManager::IterateDirectoryRecursively` (visitor pattern — sidesteps the `FindFilesRecursive` 6th-param `bClearFileNames=true` trap documented in `.claude/rules/scoped/cpp-code.md`).

Default roots (used when `UMonolithReflectionIntelSettings::DecisionMarkdownRoots` is empty):

- `Docs/` — project-level specs and plans
- `Plugins/Monolith/Docs/` — Monolith specs, plans, CHANGELOG, guides
- `.claude/rules/` — agent rules

Each root is resolved relative to `FPaths::ProjectDir()` unless absolute. Non-existent roots are skipped with a `Verbose` log line — never an error.

Files are read in full via `FFileHelper::LoadFileToString` and tokenised into lines via `FString::ParseIntoArrayLines` for header walk.

### 3.2 Heuristic detection

The indexer emits at most one row per markdown header (or one per file in the frontmatter-decision path). Three detection tiers with distinct confidence floors:

| Tier | Trigger | Confidence | Status default |
|------|---------|------------|----------------|
| **YAML frontmatter** | Leading `---` block with `decision: true` OR any `status:` key | `0.90` | from `status:` value (lowercased), else `accepted` |
| **ADR-style header** | Line matches `(?i)^#+\s*(?:ADR[-\s]?\d+|Architectural\s+Decision)\b` | `0.85` | `open` |
| **Header + rationale marker** | Markdown header (H2–H6 only — H1 skipped unless ADR-style) followed within 8 lines by a paragraph containing `because` / `rationale` / `evidence` / `decision:` | `0.65` | `open` |

Files matching neither tier contribute zero rows. Headers without rationale markers and without ADR shape are skipped — the indexer is conservative by design.

The `UMonolithReflectionIntelSettings::DecisionMinConfidence` floor (default `0.6`) is applied at **query time** by `decision_query("list_decisions")`, not at extraction time — every detected record is stored, then filtered on read so callers can override the floor per call.

### 3.3 SQLite schema

Tables live inside the shared `EngineSource.db` file under the `decision_` prefix so they coexist with the source-indexer's tables without name collision.

```sql
CREATE TABLE IF NOT EXISTS decision_records (
    decision_id     TEXT PRIMARY KEY,
    title           TEXT NOT NULL,
    status          TEXT NOT NULL DEFAULT 'open',
    source_path     TEXT NOT NULL,
    source_line     INTEGER NOT NULL DEFAULT 0,
    confidence      REAL NOT NULL DEFAULT 0.0,
    rationale       TEXT,
    source_mtime    INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE IF NOT EXISTS decision_supersedes (
    from_decision_id TEXT NOT NULL,
    to_decision_id   TEXT NOT NULL,
    PRIMARY KEY (from_decision_id, to_decision_id)
);

CREATE INDEX IF NOT EXISTS idx_decision_records_status
    ON decision_records(status);
CREATE INDEX IF NOT EXISTS idx_decision_records_source_path
    ON decision_records(source_path);
CREATE INDEX IF NOT EXISTS idx_decision_supersedes_to
    ON decision_supersedes(to_decision_id);
```

All schema statements use `CREATE ... IF NOT EXISTS` so first-run bootstrap and subsequent re-runs are both safe. Index creation failure is non-fatal (logged at `Warning`); the base tables MUST succeed or the indexer aborts.

**`decision_id` shape:** `<forward-slashed-project-relative-path>#<header-anchor>`, where the anchor is a slug derived from header text (lowercased, alphanumeric + hyphens, trailing hyphens trimmed). Frontmatter-decision rows use `#frontmatter` as the anchor. The ID is stable across reindex runs as long as the path and header text are stable.

**Wipe-and-rewrite semantics:** every `Run()` call wipes both tables and rewrites from scratch. The corpus is small enough that incremental delta-detection isn't justified; full rewrite makes "decision removed from markdown" reflect immediately. Writes occur inside a single `BEGIN TRANSACTION ... COMMIT` block with a reused prepared statement per `MeshCatalogIndexer.cpp` pattern.

### 3.4 Action surface

5 actions register under `decision` from `FDecisionQueryAdapter::RegisterActions`. All five carry `readOnlyHint: true`, `destructiveHint: false`, `idempotentHint: true` on the dispatcher annotations (v0.17.0 `tools/list` surface). All five participate in v0.17.0 universal response shaping (`_fields` / `_omit` / `_compact_json`) for free.

#### `decision_query("list_decisions", params)`

List architectural decisions filtered by source-path substring and minimum heuristic confidence. Cursor-paginated.

| Param | Type | EMonolithParamKind | Required | Default | Notes |
|-------|------|---------------------|----------|---------|-------|
| `path_filter` | string | `DiskPath` | no | `""` | Substring match against `source_path` (project-relative). `\` → `/` rewritten by dispatcher with surfaced warning. |
| `min_confidence` | number | `Other` | no | `0.6` | Floor in `[0, 1]`. Settings default is also `0.6`; per-call override wins. |
| `status` | string | `Other` | no | `""` | Exact match — typical values: `open`, `accepted`, `superseded`, `deprecated`, `draft`. |
| `limit` | integer | `Other` | no | `50` | Page size. Hard cap `200`. |
| `cursor` | string | `Other` | no | `""` | Opaque base64+JSON cursor from a prior `next_cursor`. Restart pagination by omitting. |

**Response:**

```json
{
  "decisions": [
    {
      "decision_id": "Plugins/Monolith/Docs/SPEC_CORE.md#some-anchor",
      "title": "Some Architectural Decision",
      "status": "open",
      "source_path": "Plugins/Monolith/Docs/SPEC_CORE.md",
      "source_line": 142,
      "confidence": 0.85,
      "rationale": "Rationale paragraph if one was mined.",
      "source_mtime": 1717094400
    }
  ],
  "total_estimate": 47,
  "next_cursor": "<opaque>"
}
```

`total_estimate` is emitted on page 0 only (one `COUNT(*)` per filter set). Subsequent pages carry the cached count inside the cursor. `next_cursor` is omitted on the last page.

#### `decision_query("get_decision", params)`

Fetch one record by stable id.

| Param | Type | Required |
|-------|------|----------|
| `decision_id` | string | yes |

**Response:** `{ "decision": <row-or-null> }` — `decision` is `null` when the id is unknown.

#### `decision_query("list_stale", params)`

List decisions whose source markdown has not been modified within `max_age_days` days. Useful for spec-drift detection.

| Param | Type | EMonolithParamKind | Required | Default | Notes |
|-------|------|---------------------|----------|---------|-------|
| `max_age_days` | integer | `Other` | yes | — | Positive only. Compared against source-file mtime in UTC. |
| `path_filter` | string | `DiskPath` | no | `""` | Substring match. |
| `limit` | integer | `Other` | no | `50` | Hard cap `200`. |
| `cursor` | string | `Other` | no | `""` | Opaque cursor. |

**Response:**

```json
{
  "stale_decisions": [ /* row objects */ ],
  "cutoff_unix": 1714502400,
  "next_cursor": "<opaque>"
}
```

Rows are ordered by `source_mtime ASC` (oldest first). Records with `source_mtime = 0` (mtime unavailable) are excluded — they cannot be honestly classified.

#### `decision_query("find_supersession_chain", params)`

Walk supersedes edges outward from a starting decision. Returns the ordered chain of decisions the start id transitively supersedes.

| Param | Type | Required | Default | Notes |
|-------|------|----------|---------|-------|
| `decision_id` | string | yes | — | Start of the walk. |
| `depth` | integer | no | `10` | Maximum traversal depth. Hard cap `50`. |

**Response:**

```json
{
  "start": "<decision_id>",
  "chain": [
    { "from": "<id>", "to": "<id>", "depth": 1 },
    { "from": "<id>", "to": "<id>", "depth": 2 }
  ],
  "truncated": false
}
```

`truncated: true` indicates the walk hit `depth` while frontier nodes remained — call again with higher `depth` if needed. Cycle protection via a visited set.

#### `decision_query("find_referent_decisions", params)`

Inverse of `find_supersession_chain` — list decisions that explicitly supersede the given id (the records that replaced it).

| Param | Type | Required |
|-------|------|----------|
| `decision_id` | string | yes |

**Response:**

```json
{
  "decision_id": "<id>",
  "referent_decisions": [ /* full row objects */ ]
}
```

Rows ordered by `source_path, source_line`.

### 3.5 Borrowing the shared `EngineSource.db` handle

`FDecisionQueryAdapter` borrows `UMonolithSourceSubsystem`'s already-open `EngineSource.db` handle rather than opening its own. It MUST: UE 5.7's SQLite is built with `SQLITE_OS_OTHER=1` and a custom `unreal-fs` VFS that permits only ONE open of a given file per process; a second open of `EngineSource.db` returns `SQLITE_IOERR`. `FMonolithSourceDatabase` exposes that handle via `GetRawHandle()` (the open `FSQLiteDatabase*`) and `GetLock()` (`FCriticalSection&`).

The borrow contract is **game-thread-only** — read-path adapters call `ensure(IsInGameThread())`. This serialises reads against the subsystem's handle close without a per-read lock: the subsystem's handle close runs on the game thread (its reindex trigger is game-thread-dispatched), and its async indexer uses a SEPARATE worker handle, so a game-thread read can never observe a half-closed handle.

- Read-path adapters borrow `GetRawHandle()` and read directly; no separate ReadOnly open, no concurrent-reader coexistence to reason about.
- Write-path bootstrap (`RunDecisionIndexerOnce`) runs under `FScopeLock(&SharedDb->GetLock())`. A brief private ReadWrite open is used only in the narrow window where the subsystem's handle is closed during a reindex.

The accessor on `FMonolithSourceDatabase` is named `GetRawHandle()` — NOT `GetRawDatabase()` (that name belongs to the unrelated `FMonolithIndexDatabase`). `MonolithReflectionIntel.Build.cs` depends on `MonolithSource` (plus `UnrealEd` + `EditorSubsystem`) to reach the subsystem; the dependency is one-way (RI → MonolithSource; MonolithSource never references RI).

### 3.6 Staleness detection

`source_mtime` is captured via `IFileManager::Get().GetTimeStamp(path)` and stored as a UTC Unix timestamp in the `decision_records` table. `FDateTime::MinValue()` returns sentinel `0` — those rows are excluded from `list_stale` so a filesystem error doesn't masquerade as fresh data.

`list_stale` computes the cutoff at query time: `cutoff = utc_now - max_age_days * 86400`. The SQL is `WHERE source_mtime > 0 AND source_mtime < ?`.

### 3.7 Test coverage

Four automation tests under `Monolith.ReflectionIntel.Decision.*` (`EditorContext | EngineFilter` flags):

| Test | Asserts |
|------|---------|
| `SchemaBootstrap` | Empty-corpus `Run()` succeeds; `decision_records` and `decision_supersedes` tables exist after the call. |
| `HeuristicAccuracy` | ≥4 rows ingested from the 5-file fixture corpus; `03_non_decision.md` contributes zero rows. |
| `SupersessionChain` | ≥1 edge in `decision_supersedes` after indexing the fixture corpus (file `02` carries two `Supersedes:` lines). |
| `StalenessFlag` | A fixture file copied to `FPaths::AutomationTransientDir()` and aged by 60 days via `IFileManager::SetTimeStamp` shows up in a 30-day cutoff query. |

Fixture corpus under `Source/MonolithReflectionIntel/Private/Tests/Fixtures/DecisionCorpus/` (5 markdown files: `01_decision_with_rationale.md`, `02_decision_with_supersedes.md`, `03_non_decision.md`, `04_yaml_frontmatter.md`, `05_adr_style.md`).

Disposable test DBs are created at `FPaths::AutomationTransientDir() / "decision-test-<guid>.db"` via `ESQLiteDatabaseOpenMode::ReadWriteCreate` and deleted at test teardown — the real `EngineSource.db` is never touched.

Run via `editor_query("run_automation_tests", "Monolith.ReflectionIntel.Decision")`.

---

## 4. Risk Intelligence (Phase 2 — SHIPPED v0.17.0)

### 4.1 Substrate scope

The risk slice is deterministic — no LLM calls, no embeddings, no scoring heuristics that aren't traceable to a single line of git log or a single LOC count. Three substrates feed four indexers and five `risk_query` actions:

| Substrate | Mining method | Output |
|-----------|---------------|--------|
| Git log (per-repo) | `FPlatformProcess::CreateProc` invoking `git log --name-only --pretty=format:%H|%at|%an` against each tracked repo's `.git/` | Per-file churn (commit count, line delta) + co-change pairs (files appearing in the same commit window) |
| Source-file LOC | `IFileManager::IterateDirectoryRecursively` walk of `.cpp` / `.h` under each repo's source root + line counting via `FFileHelper::LoadFileToStringArray` | LOC count per file as a coarse complexity proxy. **No** AST parsing, **no** McCabe-style cyclomatic measure — those land in Phase 3. |
| Build.cs + `.cpp` / `.h` conditional gates | Regex sweep for `#if WITH_*` blocks, `bHas*` 3-location probe blocks in `.Build.cs`, `MONOLITH_RELEASE_BUILD` bypasses | Conditional-gate inventory keyed by module |

The mining is read-only against the working tree and the `.git/` directory — no `git checkout`, no `git reset`, no index touches.

### 4.2 Git repo enumeration

Phase 2 ships with a hardcoded list of nested git repositories (defined in `MonolithReflectionIntelModule.cpp:291-297`):

- The Monolith plugin repo itself (primary tracked repo)
- The configured sibling-plugin repos beside it (each a separate repo)

Each path is probed for `<path>/.git/`; missing `.git/` directories are skipped silently (`Verbose` log only — not an error). The hardcoded list is a known limitation; **Phase 3 follow-up** is to make the probe directory-walk-based so future sibling plugins are picked up without a code change.

If the host project root uses a non-git VCS, it has no `.git/`; the risk indexer correctly skips it.

### 4.3 Co-change pair detection algorithm

A co-change pair `(A, B)` means files `A` and `B` appeared in the **same git commit** within a configurable commit window. The default window is the entire history of the repo at index time; `UMonolithReflectionIntelSettings::MaxCoChangeWindowCommits` (default `50`, range `[10, 500]`) caps the window so very long histories don't dominate.

For each commit observed:

1. Parse the commit's changed-file list from `git log --name-only` output.
2. Apply the `GitMiningNoiseFilter` blacklist (file-pattern globs — `Saved/`, `Intermediate/`, `Binaries/*.dll`, etc.).
3. Apply `MaxCommitFileCount` cap (default `50`) — commits touching more files than the cap (typical for tree-wide refactors or initial imports) contribute zero pairs; they would otherwise dominate co-change scores. The cap suppresses the "monster commit" noise floor.
4. Emit every unordered pair `(A, B)` with `A < B` lexicographically — symmetric storage avoided.
5. Aggregate `(A, B)` → commit count across the window.

The pair scoring is **count-based, not normalised**. Two files appearing together in 12 commits beat two files appearing together in 3 commits. Phase 3 may layer in tf-idf-style normalisation; v0.17.0 ships the raw count.

### 4.4 Hotspot score formula

The hotspot score for a file is a deterministic blend of normalised churn and normalised complexity proxy:

```
hotspot_score(file) = 0.6 * normalised_churn(file) + 0.4 * normalised_loc(file)

normalised_churn(file) = file.commit_count / max(repo.commit_count_across_files)
normalised_loc(file)   = file.loc / max(repo.loc_across_files)
```

Both normalisers are per-repo (a busy Monolith file isn't penalised against a quiet file in another repo). Score range `[0.0, 1.0]`. Score `> 0.7` is the documented "hotspot" threshold for `get_release_window_hotspots` filtering.

The weight split (0.6 churn / 0.4 LOC) is hardcoded in v0.17.0. Configurable weighting is a Phase 3 enhancement once the LOC proxy is replaced by a real complexity measure.

### 4.5 Conditional gate sweep

The conditional-gate inventory is built by regex sweep against three patterns:

| Pattern | Where | What it captures |
|---------|-------|------------------|
| `#if\s+WITH_(\w+)` | `.cpp` / `.h` under each module's source root | Compile-time feature gates the module honours (e.g. `#if WITH_GBA`, `#if WITH_COMBOGRAPH`) |
| `bool\s+bHas(\w+)\s*=` | `.Build.cs` | 3-location detection probe variables (e.g. `bHasGameplayAbilities`, `bHasCommonUI`) |
| `MONOLITH_RELEASE_BUILD` | `.Build.cs` | Release-build bypass branches |

For each match the indexer records the module, the gate name, the file path, the source line, and (for `bHas*` probes) the surrounding probe block's classification (3-location, 4-location, or release-bypass). The output table `reflect_conditional_gates` is the substrate for `risk_query("list_conditional_gates")` and is also consumed by the Phase 4a pipeline composer's release-readiness audit.

Regex-based detection is intentionally cheap. Phase 3 may swap to tree-sitter for higher fidelity (catching commented-out `#if WITH_*` blocks, multi-line conditions, etc.); v0.17.0 accepts the false-positive rate for the indexer-runtime budget.

### 4.6 SQLite schema

Four new tables live in the shared `EngineSource.db` file under the `git_*`, `risk_*`, and `reflect_*` prefixes so they coexist with the source-indexer's tables and the Phase 1 `decision_*` tables.

```sql
CREATE TABLE IF NOT EXISTS git_file_churn (
    file_path       TEXT NOT NULL,
    repo_path       TEXT NOT NULL,
    commit_count    INTEGER NOT NULL DEFAULT 0,
    lines_added     INTEGER NOT NULL DEFAULT 0,
    lines_deleted   INTEGER NOT NULL DEFAULT 0,
    first_commit_ts INTEGER NOT NULL DEFAULT 0,
    last_commit_ts  INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (repo_path, file_path)
);

CREATE TABLE IF NOT EXISTS git_cochange_pairs (
    file_a       TEXT NOT NULL,
    file_b       TEXT NOT NULL,
    repo_path    TEXT NOT NULL,
    commit_count INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (repo_path, file_a, file_b),
    CHECK (file_a < file_b)
);

CREATE TABLE IF NOT EXISTS risk_hotspot_scores (
    file_path       TEXT NOT NULL,
    repo_path       TEXT NOT NULL,
    score           REAL NOT NULL DEFAULT 0.0,
    normalised_churn REAL NOT NULL DEFAULT 0.0,
    normalised_loc   REAL NOT NULL DEFAULT 0.0,
    loc              INTEGER NOT NULL DEFAULT 0,
    indexed_at_ts    INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (repo_path, file_path)
);

CREATE TABLE IF NOT EXISTS reflect_conditional_gates (
    module_name   TEXT NOT NULL,
    gate_name     TEXT NOT NULL,
    gate_kind     TEXT NOT NULL,      -- 'with_macro' | 'bhas_probe' | 'release_bypass'
    source_path   TEXT NOT NULL,
    source_line   INTEGER NOT NULL DEFAULT 0,
    probe_arity   INTEGER NOT NULL DEFAULT 0,  -- 3 or 4 for bHas* probes; 0 otherwise
    PRIMARY KEY (module_name, gate_name, source_path, source_line)
);

CREATE INDEX IF NOT EXISTS idx_git_file_churn_count
    ON git_file_churn(commit_count DESC);
CREATE INDEX IF NOT EXISTS idx_git_cochange_count
    ON git_cochange_pairs(commit_count DESC);
CREATE INDEX IF NOT EXISTS idx_risk_hotspot_score
    ON risk_hotspot_scores(score DESC);
CREATE INDEX IF NOT EXISTS idx_reflect_gates_module
    ON reflect_conditional_gates(module_name);
```

All four tables follow the wipe-and-rewrite semantics from Phase 1 — `Run()` truncates and rewrites in a single `BEGIN TRANSACTION ... COMMIT` block per indexer.

### 4.7 Action surface

Five actions register under `risk` from `FRiskQueryAdapter::RegisterActions`. All five carry `readOnlyHint: true`, `destructiveHint: false`, `idempotentHint: true` on the dispatcher annotations. All five participate in v0.17.0 universal response shaping (`_fields` / `_omit` / `_compact_json`) for free.

#### `risk_query("get_hotspot_score", params)`

Fetch the hotspot score for a single file path.

| Param | Type | EMonolithParamKind | Required | Default | Notes |
|-------|------|---------------------|----------|---------|-------|
| `file_path` | string | `DiskPath` | yes | — | Project-relative or repo-relative path. `\` → `/` rewritten by dispatcher with surfaced warning. |
| `repo_path` | string | `DiskPath` | no | `""` | When omitted, searches across all indexed repos and returns the first match. |

**Response:** `{ "score": <number-or-null>, "normalised_churn": <number>, "normalised_loc": <number>, "loc": <int>, "repo_path": <string> }` — `score` is `null` when the file is not in the index.

#### `risk_query("get_cochange_pairs", params)`

List files that frequently change in the same commits as the given file. Cursor-paginated.

| Param | Type | EMonolithParamKind | Required | Default | Notes |
|-------|------|---------------------|----------|---------|-------|
| `file_path` | string | `DiskPath` | yes | — | Anchor file. |
| `repo_path` | string | `DiskPath` | no | `""` | Optional repo scope. |
| `min_commits` | integer | `Other` | no | `2` | Lower bound on `commit_count` per pair. Pairs with `commit_count == 1` are filtered to suppress one-off co-touches. |
| `limit` | integer | `Other` | no | `50` | Hard cap `200`. |
| `cursor` | string | `Other` | no | `""` | Opaque base64+JSON cursor. |

**Response:**

```json
{
  "anchor": "path/to/file.cpp",
  "pairs": [
    { "partner": "path/to/other.cpp", "commit_count": 12 }
  ],
  "total_estimate": 47,
  "next_cursor": "<opaque>"
}
```

#### `risk_query("get_file_churn", params)`

Per-file churn record — commit count and line-delta totals.

| Param | Type | EMonolithParamKind | Required |
|-------|------|---------------------|----------|
| `file_path` | string | `DiskPath` | yes |
| `repo_path` | string | `DiskPath` | no |

**Response:** `{ "churn": <row-or-null> }` — row includes `commit_count`, `lines_added`, `lines_deleted`, `first_commit_ts`, `last_commit_ts`.

#### `risk_query("get_release_window_hotspots", params)`

List files whose hotspot score exceeds a threshold, ordered descending. Designed for release-readiness queries — "which files are most likely to bite us before tagging?" Cursor-paginated.

| Param | Type | EMonolithParamKind | Required | Default | Notes |
|-------|------|---------------------|----------|---------|-------|
| `threshold` | number | `Other` | no | `0.7` | Floor in `[0, 1]`. |
| `repo_path` | string | `DiskPath` | no | `""` | Optional repo scope. |
| `limit` | integer | `Other` | no | `50` | Hard cap `200`. |
| `cursor` | string | `Other` | no | `""` | Opaque cursor. |

**Response:** `{ "hotspots": [ { "file_path": ..., "score": ..., "normalised_churn": ..., "normalised_loc": ..., "loc": ..., "repo_path": ... } ], "total_estimate": 12, "next_cursor": "<opaque>" }`.

#### `risk_query("list_conditional_gates", params)`

List `#if WITH_*` macros, `bHas*` 3-location probe variables, and `MONOLITH_RELEASE_BUILD` bypass branches across the project. Cursor-paginated.

| Param | Type | EMonolithParamKind | Required | Default | Notes |
|-------|------|---------------------|----------|---------|-------|
| `module_filter` | string | `Other` | no | `""` | Substring match against module name. |
| `gate_kind` | string | `Other` | no | `""` | Exact match — `with_macro`, `bhas_probe`, `release_bypass`. |
| `limit` | integer | `Other` | no | `100` | Hard cap `500`. |
| `cursor` | string | `Other` | no | `""` | Opaque cursor. |

**Response:** `{ "gates": [ { "module_name": ..., "gate_name": ..., "gate_kind": ..., "source_path": ..., "source_line": ..., "probe_arity": ... } ], "total_estimate": 87, "next_cursor": "<opaque>" }`.

### 4.8 Test coverage

Automation tests under `Monolith.ReflectionIntel.Risk.*` and `Monolith.ReflectionIntel.ModuleDepReality.*` (`EditorContext | EngineFilter` flags). Disposable test DBs at `FPaths::AutomationTransientDir()`; the real `EngineSource.db` is never touched.

| Test | Asserts |
|------|---------|
| `RiskSchemaBootstrap` | Empty-corpus `Run()` succeeds; all 4 Phase 2 tables exist after the call. |
| `ChurnAggregation` | Fixture mini-repo with 5 known commits produces correct per-file `commit_count` rows. |
| `CoChangePairSymmetry` | Pair `(A, B)` is stored with `A < B`; reverse lookup returns the same row. |
| `HotspotScoreFormula` | Hand-computed expected score for a fixture file matches `0.6 * churn + 0.4 * loc` blend within `1e-6`. |
| `ConditionalGateSweep` | Fixture `.cpp` / `.Build.cs` corpus produces expected `with_macro` + `bhas_probe` + `release_bypass` rows. |
| `MonsterCommitSuppression` | A fixture commit with `MaxCommitFileCount+1` files contributes zero co-change pairs. |

Fixture corpus under `Source/MonolithReflectionIntel/Private/Tests/Fixtures/RiskCorpus/` — synthetic mini-repos and `.cpp` / `.Build.cs` snippets covering each detection path.

Run via `editor_query("run_automation_tests", "Monolith.ReflectionIntel.Risk")` and `editor_query("run_automation_tests", "Monolith.ReflectionIntel.ModuleDepReality")`.

---

## 4b. Module-Dep Reality Audit (Phase 2 — SHIPPED v0.17.0)

### 4b.1 Purpose

The audit catches a specific bug class: a UPROPERTY (or other reflection-touching declaration) references a symbol from a module that the owning module's `Build.cs` does not list in `PrivateDependencyModuleNames` (or `PublicDependencyModuleNames`). UHT generates `Z_Construct_*_NoRegister` reflection code that calls into the foreign module's API macro at link time — if the dep is missing, the failure surfaces as a downstream LNK2019 with a confusing-looking unresolved external referring to a symbol the developer didn't expect to be a link-time dep.

The canonical example of this bug class is `TSoftObjectPtr<UWidgetBase>` UPROPERTY in a non-UMG module, where `UMG` is missing from `Build.cs`. The same class also covers FGameplayTag UPROPERTY without `GameplayTags`, FNiagaraSystemAsset without `Niagara`, etc.

### 4b.2 Algorithm

The audit is a four-pass scan against the project's source tree:

1. **Parse every `*.Build.cs` under `Source/`** — regex-extract `PublicDependencyModuleNames.AddRange({...})` and `PrivateDependencyModuleNames.AddRange({...})` array contents. Build a `module → declared_deps` map.
2. **Parse every `*.h` / `*.cpp` under each module's source root** — regex-extract type-bearing reflection declarations (`UPROPERTY(...)\s+\w+`, `UFUNCTION(...)\s+\w+`, function signatures). Extract the type names used (including template arguments — `TSoftObjectPtr<UMyClass>` extracts `UMyClass`).
3. **Resolve each extracted type name against `EngineSource.db`** — locate the symbol's owning module via the existing source-indexer tables. Unknown / external-to-Unreal types are skipped silently.
4. **Emit a violation** for each `(declaring_module, used_type, used_type_owning_module)` triple where `used_type_owning_module` is NOT in `declaring_module`'s declared deps AND NOT in the implicit-deps whitelist (see §4b.3).

The audit is heuristic — it catches the common case (direct UCLASS / USTRUCT references in UPROPERTY) but does not currently chase typedef aliases or template-argument metaclasses with full UHT fidelity. That fidelity lands in Phase 3 when the UHT-artefact parser ships.

### 4b.3 Implicit-deps whitelist

Six modules are treated as transitively-available and never reported as missing:

- `Core`, `CoreUObject`, `Engine`, `Projects`, `RHI`, `RenderCore`

These are near-universal — virtually every UE module already lists them or inherits them transitively. Flagging them as "missing deps" would drown real violations in noise. The list is hardcoded in Phase 2; making it configurable per-project is a deferred enhancement.

### 4b.4 Action surface

One action registers onto the existing `source` namespace from `FModuleDepRealityAdapter::RegisterActions`. The audit handler lives in `MonolithReflectionIntel` but is registered against `source_query` for caller ergonomics — agents searching for source-related tooling already find `source_query` first.

#### `source_query("audit_module_dep_reality", params)`

Scan the project for UPROPERTY / API-symbol usages whose owning module is missing from the declaring module's `Build.cs` deps. Cursor-paginated.

| Param | Type | EMonolithParamKind | Required | Default | Notes |
|-------|------|---------------------|----------|---------|-------|
| `module_filter` | string | `Other` | no | `""` | Substring match against the **declaring** module's name. Empty scans all. |
| `include_whitelist` | bool | `Other` | no | `false` | When `true`, also reports references to whitelisted implicit-dep modules (debug aid). |
| `limit` | integer | `Other` | no | `100` | Hard cap `500`. |
| `cursor` | string | `Other` | no | `""` | Opaque base64+JSON cursor. |

**Response:**

```json
{
  "violations": [
    {
      "declaring_module": "MonolithMesh",
      "source_path": "Source/MonolithMesh/Private/Foo.cpp",
      "source_line": 142,
      "used_type": "UNiagaraSystem",
      "missing_dep": "Niagara"
    }
  ],
  "scanned_modules": 17,
  "scanned_declarations": 4823,
  "next_cursor": "<opaque>"
}
```

Annotations: `readOnlyHint: true`, `destructiveHint: false`, `idempotentHint: true`. Carries `EMonolithParamKind::Other` on all params (no path normalisation applies — `module_filter` is a name substring, not a path).

**False-positive mitigation.** The audit returns violations sorted by `(declaring_module, source_path, source_line)` so duplicates clump together — typical scan reports group related findings, making batch review tractable. Callers MAY treat `include_whitelist=true` results as advisory only.

### 4b.5 Known limitations

- **Typedef-cleared cases caught poorly.** When a UPROPERTY uses `FMyAlias` and `FMyAlias = TSoftObjectPtr<UFoo>` is declared in a header, the audit currently resolves `FMyAlias` to whatever module declares the typedef rather than chasing the underlying type. Phase 3 (UHT-artefact parser) addresses this by reading the canonicalised type from the `*.generated.h` output instead of the source declaration.
- **Template-argument metaclasses partial.** Single-argument templates (`TSoftObjectPtr<X>`, `TSubclassOf<X>`, `TArray<X>`) are handled. Multi-argument templates (`TMap<K, V>`, custom 3+ arg templates) extract only the first argument in v0.17.0.
- **Macro-hidden types invisible.** Types behind `#if WITH_*` blocks that are gated false at audit time produce false negatives. This is intentional — the audit reflects the build-as-configured, not all theoretical configurations. Pair with `risk_query("list_conditional_gates")` to spot-check gated regions.
- **No deduplication across `module_filter` paginations.** A violation appearing in two distinct source lines surfaces as two rows. This is correct — distinct call sites are distinct evidence — but callers writing release-gate reports should dedupe on `(declaring_module, missing_dep)` before presenting summary counts.

### 4b.6 Forward direction — `source_query("suggest_build_cs_deps")` (Phase 2, LLM C++ ergonomics)

A second action on `FModuleDepRealityAdapter`, registered onto the `source` namespace alongside `audit_module_dep_reality`. Where the audit scans the whole project for missing-dep *violations*, this is the **forward direction**: given ONE file (or symbol list), report the modules it requires and which are absent from the declaring module's `Build.cs`. Same four-pass machinery (Build.cs parse + UE-ident regex + symbol→module SQL resolve + implicit-whitelist diff), inverted.

| Param | Type | EMonolithParamKind | Required | Notes |
|-------|------|---------------------|----------|-------|
| `file_path` | string | `Other` | one of file_path/symbols | A `.h`/`.cpp` whose declaring module's Build.cs deps to audit. **Intentionally `Other`, NOT `DiskPath`** — see kind note below. |
| `symbols` | array | `Other` | one of file_path/symbols | Explicit UE type names (used instead of / together with file extraction). |

**`file_path` is `Other`, not `DiskPath`, by design.** The `DiskPath` kind warns "paths in this index are stored with forward slashes, so a query for '<x>' will likely return zero results" on backslashes — correct for actions that key the SQLite index BY PATH, but FALSE here: this handler uses `file_path` only for on-disk reads (the file + its `<Module>.Build.cs`, both backslash-tolerant on Windows) and path-string module derivation. The DB is queried by TYPE NAME, never by path, so a backslash path resolves fine and the warning was spurious. `Other` passes the value through untouched.

**Declaring-module resolution is path-first** — parse `Source/<Module>/...` or `Plugins/<X>/Source/<Module>/...` out of `file_path` (LAST `/Source/` wins → innermost module), then read `<Module>.Build.cs` from disk. This works on **uncommitted/unindexed files** (the common case: a header the agent just wrote). The DB is used ONLY to resolve the OWNING modules of the *used types*, never the declaring module.

**Build.cs dep parse strips C# comments first.** `ParseBuildCs` (shared with `audit_module_dep_reality`) runs the dependency-array regex over a comment-stripped copy of the file. Without this, a `//` or `/* */` comment containing a `)` *inside* the `AddRange(new string[]{ ... })` body (e.g. `// (AIMODULE_API). ...`) truncated the non-greedy `(...)` site capture at the first `)`, silently dropping every dependency declared after it (observed: `EnhancedInput` / `UMG` / `MaterialEditor` reported missing though declared). Comment stripping is string-literal aware. This hardening benefits both `suggest_build_cs_deps` and `audit_module_dep_reality`.

**Response:** `{ declaring_module, build_cs (or build_cs_note if none on disk), required_modules[], missing[] }` (+ a `content[].text` rendering). `required_modules` excludes self + the Core/CoreUObject/Engine/Projects/RHI/RenderCore implicit whitelist; `missing` = required minus declared.

Annotations: `readOnlyHint: true`, `destructiveHint: false`, `idempotentHint: true`, game-thread only (`ensure(IsInGameThread())`). Borrows the shared `FMonolithSourceDatabase` via `GetSharedSourceDb()` and holds `FScopeLock(&SharedDb->GetLock())` for the full statement borrow (prepare→step→`Destroy()`), per the `GetRawHandle` contract.

**NOT offline-served.** Like `audit_module_dep_reality`, this action is RI/live-only — it needs the cached query DB plus an on-disk Build.cs/source walk that the standalone `monolith_query.exe` / `monolith_offline.py` do not implement. It is absent from the offline tools and from `verify_offline_parity.py`.

---

## 5. CppReflect Intelligence (Phase 3a — SHIPPED v0.17.0)

### 5.1 Purpose

The CppReflect slice answers the two highest-frequency reflection-edge questions an agent asks while navigating an unfamiliar UE codebase:

- **"What UPROPERTY / UFUNCTION / interface surface does this UCLASS declare?"** — Phase 3a serves this from canonical UHT artefacts, not from the live UClass reflection (`UClass*` reflection is fine for a running editor but unhelpful when an agent is reading a class definition for the first time and wants the *as-declared* surface including specifiers).
- **"Which Blueprint / asset implementations exist for this UINTERFACE, and what assets reference this C++ class?"** — Phase 3a serves this by joining the UHT-derived class graph against `IAssetRegistry`'s asset-dependency table, surfacing a single deterministic table (`cpp_asset_edges`) that bridges the two graphs.

All six actions are read-only (five shipped in Phase 3a; `list_class_specifiers` added [Unreleased]). No tree-sitter dependency, no vendored ThirdParty, no parser.c blob in the release zip. Phase 3a is a deterministic regex sweep over UHT-generated `.gen.cpp` files plus a single `IAssetRegistry` walk.

### 5.2 Substrate

Two substrates, joined at index time:

| Substrate | Mining method | Output |
|-----------|---------------|--------|
| UHT artefacts | Regex sweep over `Intermediate/Build/Win64/.../Inc/<Module>/UHT/*.gen.cpp` via `IFileManager::IterateDirectoryRecursively` | Reflected UCLASS / UPROPERTY / UFUNCTION / UINTERFACE inventory + interface-implementation edges |
| IAssetRegistry | `IAssetRegistry::Get().GetDependencies` for every `/Script/<Module>` package the registry knows about | Asset → C++ class edges, joined by class name |

The UHT artefact root resolves through `UMonolithReflectionIntelSettings::UHTArtefactRoot` (default: auto-discover via `FPaths::ProjectIntermediateDir() / TEXT("Build")`).

#### Scan-scope ladder ([Unreleased])

As of the [Unreleased] workstream the indexers no longer scan the project game module alone. Scope is driven by `IPluginManager::Get().GetEnabledPlugins()` and walks the matching plugins' own `Intermediate/Build/Win64/UnrealEditor` UHT artefact trees in addition to the game module. The ladder, widest-to-narrowest:

| Tier | What it scans | Setting | Default |
|------|---------------|---------|---------|
| Game module | The project's own game-module UHT artefacts under `FPaths::ProjectIntermediateDir() / Build` | always on | — |
| Project plugins | Every enabled plugin with `LoadedFrom == EPluginLoadedFrom::Project` — their `Intermediate/Build/Win64/UnrealEditor` UHT artefacts | `bIndexProjectPluginReflection` | **`true`** |
| Marketplace plugins | ALSO every enabled engine-installed marketplace plugin (`LoadedFrom == EPluginLoadedFrom::Engine` whose base dir sits under `/Plugins/Marketplace/`) | `bIndexMarketplacePluginReflection` | **`false`** |
| Epic engine built-ins | Epic's built-in engine plugins (the rest of `LoadedFrom == Engine`) | `bIndexEnginePluginReflection` | `false` |

Epic engine built-ins stay excluded by default — engine-side surface area dwarfs the project's at ~6 orders of magnitude and floods queries with low-signal hits. The marketplace tier is a deliberate middle ground: enabled marketplace plugins are usually relevant to gameplay code, but they're still off by default so a fresh install gets the project + project-plugin signal without the marketplace noise unless you opt in.

**E2E evidence (project-only force-reindex via `reflect_query("rebuild_reflection_index")`):** game module alone ~30 artefacts → with project plugins on **337 artefacts** (project-plugin Server RPCs + replicated classes) → with the marketplace flag also on **927 artefacts / 745 UClasses** (marketplace-plugin classes indexed). No engine source-symbol reindex is triggered by any of this — the ladder only widens the UHT-artefact scan, it does not touch `source_query`'s engine index.

### 5.3 SQLite schema

Six new tables live inside the shared `EngineSource.db` file under the `reflect_` / `cpp_asset_` prefixes so they coexist with the Phase 1 `decision_*` and Phase 2 `git_*` / `risk_*` / `reflect_conditional_gates` tables.

```sql
CREATE TABLE IF NOT EXISTS reflect_uclasses (
    class_name       TEXT NOT NULL,
    module_name      TEXT NOT NULL,
    parent_class     TEXT,
    class_specifiers TEXT,                  -- raw specifier list (e.g. "BlueprintType,Blueprintable,Abstract")
    source_path      TEXT NOT NULL,         -- UHT ModuleRelativePath (see §5.7)
    source_line      INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (class_name, module_name)
);

CREATE TABLE IF NOT EXISTS reflect_uproperties (
    class_name           TEXT NOT NULL,
    module_name          TEXT NOT NULL,
    property_name        TEXT NOT NULL,
    property_type        TEXT NOT NULL,
    blueprint_visibility TEXT,              -- 'BlueprintReadWrite' | 'BlueprintReadOnly' | '' — Phase 3b populates fully
    specifiers           TEXT,              -- raw specifier list — Phase 3b populates fully
    source_path          TEXT NOT NULL,
    source_line          INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (class_name, module_name, property_name)
);

CREATE TABLE IF NOT EXISTS reflect_ufunctions (
    class_name      TEXT NOT NULL,
    module_name     TEXT NOT NULL,
    function_name   TEXT NOT NULL,
    function_flags  INTEGER NOT NULL DEFAULT 0,  -- raw EFunctionFlags bitfield from UHT
    return_type     TEXT,
    params_json     TEXT,                          -- compact JSON: [{name,type}, ...]
    source_path     TEXT NOT NULL,
    source_line     INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (class_name, module_name, function_name)
);

CREATE TABLE IF NOT EXISTS reflect_uinterfaces (
    interface_name   TEXT NOT NULL,
    module_name      TEXT NOT NULL,
    source_path      TEXT NOT NULL,
    source_line      INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (interface_name, module_name)
);

CREATE TABLE IF NOT EXISTS reflect_uinterface_impls (
    interface_name   TEXT NOT NULL,
    impl_class_name  TEXT NOT NULL,
    impl_module_name TEXT NOT NULL,
    PRIMARY KEY (interface_name, impl_class_name, impl_module_name)
);

CREATE TABLE IF NOT EXISTS cpp_asset_edges (
    cpp_class       TEXT NOT NULL,
    cpp_module      TEXT NOT NULL,
    asset_path      TEXT NOT NULL,
    edge_kind       TEXT NOT NULL,          -- 'package_dep' (Phase 3a coarse) — Phase 3b may add 'subclass_of' / 'native_default'
    PRIMARY KEY (cpp_class, asset_path)
);

CREATE INDEX IF NOT EXISTS idx_reflect_uclasses_parent       ON reflect_uclasses(parent_class);
CREATE INDEX IF NOT EXISTS idx_reflect_uclasses_module       ON reflect_uclasses(module_name);
CREATE INDEX IF NOT EXISTS idx_reflect_uproperties_class     ON reflect_uproperties(class_name, module_name);
CREATE INDEX IF NOT EXISTS idx_reflect_ufunctions_class      ON reflect_ufunctions(class_name, module_name);
CREATE INDEX IF NOT EXISTS idx_reflect_uinterface_impls_face ON reflect_uinterface_impls(interface_name);
CREATE INDEX IF NOT EXISTS idx_cpp_asset_edges_class         ON cpp_asset_edges(cpp_class);
CREATE INDEX IF NOT EXISTS idx_cpp_asset_edges_asset         ON cpp_asset_edges(asset_path);
```

All tables follow Phase 1 wipe-and-rewrite semantics — `Run()` truncates and rewrites each in a single `BEGIN TRANSACTION ... COMMIT` block. DDL canonical source: `Plugins/Monolith/Source/MonolithReflectionIntel/Private/CppReflectSchema.cpp`.

### 5.4 UHT regex patterns

The artefact reader is driven by eight regex patterns derived from real-world `.gen.cpp` inspection. Naming below matches the C++ pattern constants:

| Pattern | Captures | Purpose |
|---------|----------|---------|
| `BeginClassPattern` | class name | Anchors the per-class block — every `Z_Construct_UClass_<Name>` call boundary |
| `ClassParentPattern` | parent class | Pulls the `SuperStruct` / `ClassWithin` assignment line |
| `ClassSpecifiersPattern` | raw specifier list | Captures the per-class `MetaDataMap` entries used to surface BlueprintType / Blueprintable / Abstract |
| `PropInfoPattern` | property name, property type, owning class | Walks the `FPropertyParamsBase`-derived emission |
| `PropMetaPattern` | property name, metadata key, value | Pulls the per-property metadata block (BlueprintVisibility / Category / etc.) — Phase 3a stores the empty string when no match; Phase 3b populates fully |
| `FuncInfoPattern` | three-capture validator (function name, owning class, params-block start) | Anchors per-function emission; the three-capture form rejects false-positive matches inside templated lambdas |
| `FuncParamPattern` | param name, param type | Walks the per-function parameter block following a `FuncInfoPattern` match |
| `InterfaceImplPattern` | interface name, implementing class | Pulls `IMPLEMENTS_INTERFACE` / `UClass::ImplementsInterface` emission lines |

Patterns are constexpr `FRegexPattern` instances; the reader iterates files via `FFileHelper::LoadFileToStringArray` so per-file memory stays bounded.

### 5.5 Action surface

Six actions register under `cppreflect` from `FCppReflectQueryAdapter::RegisterActions` (five shipped in v0.17.0 Phase 3a; `list_class_specifiers` added [Unreleased]). All six carry `readOnlyHint: true`, `destructiveHint: false`, `idempotentHint: true` on the dispatcher annotations. All six participate in v0.17.0 universal response shaping (`_fields` / `_omit` / `_compact_json`).

#### `cppreflect_query("get_uclass", params)`

Fetch the UHT-derived UCLASS record — parent class, specifiers, source path, line.

| Param | Type | EMonolithParamKind | Required | Default | Notes |
|-------|------|---------------------|----------|---------|-------|
| `class_name` | string | `Other` | yes | — | Bare class name. Module ambiguity resolved via the optional `module_name` filter. |
| `module_name` | string | `Other` | no | `""` | Disambiguates classes that share a name across modules. |

**Response:** `{ "uclass": <row-or-null> }` — row includes `class_name`, `module_name`, `parent_class`, `class_specifiers`, `source_path`, `source_line`.

#### `cppreflect_query("list_uproperties", params)`

Enumerate the UPROPERTY surface for a UCLASS. Cursor-paginated.

| Param | Type | EMonolithParamKind | Required | Default | Notes |
|-------|------|---------------------|----------|---------|-------|
| `class_name` | string | `Other` | yes | — | Bare class name. |
| `module_name` | string | `Other` | no | `""` | Optional disambiguator. |
| `limit` | integer | `Other` | no | `50` | Hard cap `200`. |
| `cursor` | string | `Other` | no | `""` | Opaque base64+JSON cursor. |

**Response:** `{ "properties": [ { "property_name", "property_type", "blueprint_visibility", "specifiers", "source_path", "source_line" } ], "total_estimate": N, "next_cursor": "<opaque>" }`. `blueprint_visibility` and `specifiers` are empty strings in Phase 3a — see §5.7.

#### `cppreflect_query("list_ufunctions", params)`

Enumerate the UFUNCTION surface for a UCLASS. Cursor-paginated.

| Param | Type | EMonolithParamKind | Required | Default | Notes |
|-------|------|---------------------|----------|---------|-------|
| `class_name` | string | `Other` | yes | — | Bare class name. |
| `module_name` | string | `Other` | no | `""` | Optional disambiguator. |
| `limit` | integer | `Other` | no | `50` | Hard cap `200`. |
| `cursor` | string | `Other` | no | `""` | Opaque base64+JSON cursor. |

**Response:** `{ "functions": [ { "function_name", "function_flags", "return_type", "params_json", "source_path", "source_line" } ], "total_estimate": N, "next_cursor": "<opaque>" }`. `function_flags` is the raw `EFunctionFlags` bitfield as emitted by UHT — callers translate to symbolic names if needed.

#### `cppreflect_query("find_interface_impls", params)`

List every UCLASS that implements the given UINTERFACE.

| Param | Type | EMonolithParamKind | Required | Default |
|-------|------|---------------------|----------|---------|
| `interface_name` | string | `Other` | yes | — |
| `limit` | integer | `Other` | no | `100` |
| `cursor` | string | `Other` | no | `""` |

**Response:** `{ "interface_name": "<name>", "implementations": [ { "impl_class_name", "impl_module_name" } ], "total_estimate": N, "next_cursor": "<opaque>" }`. Blueprint implementations are NOT in this set — `reflect_uinterface_impls` is C++ only. Use the asset graph (`cpp_asset_edges`) for the BP side.

#### `cppreflect_query("find_class_specifier", params)`

Find every UCLASS carrying a given specifier — substring match against the `flags` column of `reflect_uclasses`. Cursor-paginated.

The `flags` column stores UHT metadata keys (`IsBlueprintBase`, `BlueprintType`, `Abstract`, etc.), NOT raw C++ UCLASS specifiers, so the query is more forgiving than a literal token compare ([Unreleased] enhancements):

- **Alias map.** Well-known C++ specifiers are translated to the token UHT actually stores — `Blueprintable` -> `IsBlueprintBase`. The response surfaces the translated `effective_token` so callers understand what was queried.
- **Honest not-captured note.** Specifiers UHT drops entirely (`MinimalAPI` is a pure export-macro hint; `NotBlueprintable` is encoded as the *absence* of `IsBlueprintBase`) are never stored, so a search for them can only ever return zero rows. The action returns an explicit not-captured note instead of a silent empty result, and points callers at `list_class_specifiers` to see what *is* queryable.
- **Case-insensitive matching.** SQLite `LIKE` is ASCII-case-insensitive, so `blueprinttype` and `BlueprintType` match the same rows.

| Param | Type | EMonolithParamKind | Required | Default | Notes |
|-------|------|---------------------|----------|---------|-------|
| `specifier` | string | `Other` | yes | — | Substring match against the stored token, case-insensitive — `"BlueprintType"` matches both `BlueprintType` and `IsBlueprintBase:BlueprintType,...`. Well-known C++ specifiers (`Blueprintable`) are alias-mapped to the stored token. |
| `module_filter` | string | `Other` | no | `""` | Optional module-name substring. |
| `limit` | integer | `Other` | no | `100` | Hard cap `500`. |
| `cursor` | string | `Other` | no | `""` | Opaque base64+JSON cursor. |

**Response:** `{ "classes": [ { "class_name", "module_name", "parent_class", "class_specifiers", "source_path", "source_line" } ], "effective_token": "<translated-token>", "total_estimate": N, "next_cursor": "<opaque>" }`. When the requested specifier is one UHT drops (`MinimalAPI` / `NotBlueprintable`), the response carries a not-captured note explaining it is not stored and refers the caller to `list_class_specifiers`.

#### `cppreflect_query("list_class_specifiers")`

**[Unreleased].** Return the DISTINCT universe of tokens stored in the `flags` column of `reflect_uclasses`, each with a per-token class count. The `flags` column stores UHT metadata keys (e.g. `IsBlueprintBase`, `BlueprintType`, `Abstract`), NOT raw C++ UCLASS specifiers. Use this to discover what `find_class_specifier` can actually match. No params.

This is the discovery companion to `find_class_specifier`: rather than guessing whether `Blueprintable` / `BlueprintType` / `EditInlineNew` is a real stored token, an agent lists the actual token universe first, then queries with a value it knows is present.

| Param | Type | EMonolithParamKind | Required | Default | Notes |
|-------|------|---------------------|----------|---------|-------|
| _(none)_ | — | — | — | — | The action takes no params. |

**Response:** `{ "specifiers": [ { "token": "IsBlueprintBase", "class_count": 142 }, { "token": "BlueprintType", "class_count": 138 } ], "total_estimate": N }`. Tokens are the distinct values walked out of the `flags` column (split on `:`), ordered by `class_count` descending.

### 5.6 Bootstrap pattern

Lazy on first call — `FCppReflectQueryAdapter` checks for `reflect_uclasses` via the borrowed shared handle (`FMonolithSourceDatabase::GetRawHandle()`) and triggers `FMonolithReflectionIntelModule::RunCppReflectIndexerOnce` on absence. The write-path indexer ensures the six tables and writes rows under `FScopeLock(&SharedDb->GetLock())` on the shared handle. `FCoreUObjectDelegates::ReloadCompleteDelegate` is bound at `StartupModule` (shared with the Phase 1 + Phase 2 hot-reload refresh) so UBT-driven hot-reload re-runs the indexer automatically.

Read-path queries borrow the subsystem's single open handle directly under the game-thread-only contract (`ensure(IsInGameThread())`); there is no second open to coexist with — UE 5.7's `unreal-fs` VFS would reject a second open of `EngineSource.db` with `SQLITE_IOERR`. The shared handle's `PRAGMA journal_mode=DELETE` (the WAL-silent-fail trap from `Docs/references/UE57Gotchas.md`) applies as set by the subsystem.

### 5.7 Known limitations

These shape the caller contract — agents querying the surface must treat the four fields below as Phase 3a-coarse:

- **`source_path` is UHT's ModuleRelativePath**, not the canonical project-relative path the Phase 1+2 tables use. UHT emits paths as `<Module>/<Relative>` with forward slashes; mapping to `Source/<Module>/<Relative>` is the caller's job for now. The mapping is unambiguous within a single module but the schema stores UHT's form verbatim for traceability.
- **`source_line` is `0` everywhere.** UHT's `.gen.cpp` output does NOT carry source line numbers — UHT discards the original-header line during code generation. Anyone needing per-line precision pairs Phase 3a with a `source_query("search_source", "<symbol>")` round-trip; the source-indexer carries the real line.
- **`cpp_asset_edges.edge_kind` is coarse.** Phase 3a populates one edge per `(asset_path, cpp_class)` pair where the asset's own class belongs to a `/Script/<Module>` package the asset depends on. The edge is `'package_dep'` — coarser than "this asset's parent class is `cpp_class`" or "this asset's CDO references `cpp_class`". Phase 3b may add `'subclass_of'` / `'native_default'` once tree-sitter lands.
- **`reflect_uproperties.blueprint_visibility` and `.specifiers` are empty strings.** Phase 3a's `PropMetaPattern` reads the per-property metadata block but the canonicalisation into BlueprintReadWrite / BlueprintReadOnly / specifier lists is deferred to Phase 3b. Callers needing exact per-property visibility must round-trip through `blueprint_query("get_cdo_properties")` against a live UClass.

### 5.8 Test coverage

Four automation tests under `Monolith.ReflectionIntel.CppReflect.*` (`EditorContext | EngineFilter` flags):

| Test | Asserts |
|------|---------|
| `CppReflectSchemaBootstrap` | Empty-artefact-root `Run()` succeeds; all 6 Phase 3a tables exist after the call. |
| `UClassFixtureExtraction` | `sample.gen.cpp.fixture` produces expected `reflect_uclasses` row count + parent-class linkage. |
| `InterfaceImplResolution` | `IMPLEMENTS_INTERFACE` lines in fixture produce expected `reflect_uinterface_impls` rows; `find_interface_impls` round-trips them. |
| `AssetGraphJoin` | Fixture asset-registry stub with two `/Script/<Module>` package dependencies produces 2 `cpp_asset_edges` rows. |

Fixture corpus under `Source/MonolithReflectionIntel/Private/Tests/Fixtures/CppReflectCorpus/` (`sample.gen.cpp.fixture` + a minimal asset-registry stub). Disposable test DBs at `FPaths::AutomationTransientDir()`; the real `EngineSource.db` is never touched.

Run via `editor_query("run_automation_tests", "Monolith.ReflectionIntel.CppReflect")`.

---

## 5b. CppReflect Intelligence Phase 3b — Native Tag Tracking (WISHLIST)

Phase 3b would add native gameplay-tag declaration tracking on top of the Phase 3a substrate:

- **2 new SQLite tables:**
  - `reflect_native_tag_decls` — every `UE_DEFINE_GAMEPLAY_TAG_*` / `UE_DEFINE_GAMEPLAY_TAG_COMMENT` declaration site (tag name, file, line, comment).
  - `reflect_native_tag_externs` — every `UE_DECLARE_GAMEPLAY_TAG_EXTERN` consumption site (tag name, file, line).
- **1 new action:** `cppreflect_query("list_native_tags", params)` — query the cross-reference, filter by tag-name substring, paginated.
- **Phase 3b would also backfill** `reflect_uproperties.blueprint_visibility` / `.specifiers` (today empty in Phase 3a) and upgrade `cpp_asset_edges.edge_kind` to differentiate `subclass_of` / `native_default` from the current coarse `package_dep`.

**Deferral rationale.** Tree-sitter integration on Windows requires vendoring `tree-sitter-cpp` (parser.c is ~1.1M lines after grammar generation) into `Plugins/Monolith/Source/ThirdParty/`. The release-zip footprint cost is ~50MB — material against the current Monolith release-zip baseline (~12MB) and a non-trivial fraction of a Steam-build .pak budget. Phase 3a's UHT + IAssetRegistry substrate already serves "reflected surface" and "asset ↔ C++ class" queries without parser.c; native-tag tracking is the one workflow that genuinely needs source-level parsing (UHT does not emit native-tag metadata). The cost / benefit ratio favours deferring until either (a) a separate workflow justifies the parser dependency or (b) UE 5.8+ exposes native-tag declaration sites through a reflection-friendly API.

---

## 6. Network Intelligence (Phase 4a — SHIPPED v0.17.0)

### 6.1 Purpose

The network slice answers the four highest-frequency replication-edge questions an agent asks while reviewing UE 5.7 multiplayer code:

- **"Which UCLASSes declare replicated state?"** — `list_replicated_classes` enumerates every UCLASS that carries at least one replicated `UPROPERTY`, sortable by replicated-property count. **As of the [Unreleased] network-completeness workstream this now captures bare `UPROPERTY(Replicated)` + `DOREPLIFETIME` (via the `CPF_Net` flag), not just `ReplicatedUsing` declarations** — verified end-to-end (the query returned `Athe host projectCharacterBase` + `Uthe host projectVitalsSet`).
- **"Which UFUNCTIONs are RPCs of a given kind?"** — `list_rpc_functions` filters `reflect_ufunctions` by **replication specifier** (`reflect_ufunctions.specifiers` parsed from `EFunctionFlags` — `FUNC_NetServer` / `FUNC_NetClient` / `FUNC_NetMulticast`), as of the [Unreleased] workstream. **This now covers project plugins by default** (the scan-scope ladder in §5.2 walks `LoadedFrom == Project` plugins), so the project's actual RPCs — which live in project plugins — are in scope. E2E returned 28 RPCs including the project's project-plugin Server RPCs. The prior "empty because the scan is the game module only" limitation is resolved.
- **"Which OnRep handlers exist?"** — `list_onrep_handlers` returns every UFUNCTION named `OnRep_*` paired with the property it covers (resolved via name-suffix match against `reflect_replicated_properties.rep_notify_func`).
- **"Which `ReplicatedUsing=` declarations point at OnRep handlers that don't exist?"** — `audit_unbalanced_onreps` is the consistency check that catches typos and rename drift between the property declaration and the handler definition.

All four actions are read-only. The substrate is a second UHT-artefact regex sweep (independent of Phase 3a's `FUHTArtefactReader` for separation of concerns) focused on per-property `MetaData` blocks and (as of the [Unreleased] network-completeness workstream) the `CPF_Net` property-flag emission for bare-`Replicated` capture. As of the [Unreleased] workstream the sweep follows the §5.2 scan-scope ladder — game module + project plugins by default (marketplace plugins when enabled) — so replicated state and RPCs declared in project plugins (your project plugins, etc.) are in scope, not just the game module. Cross-joins are against `reflect_ufunctions` from Phase 3a, so Phase 4a depends on Phase 3a's reflection-edge tables being populated.

### 6.2 Substrate

| Substrate | Mining method | Output |
|-----------|---------------|--------|
| UHT artefacts (replication sweep) | Second regex sweep over the same `Intermediate/Build/Win64/.../Inc/<Module>/UHT/*.gen.cpp` files Phase 3a reads, focused on per-property `MetaData` blocks carrying `ReplicatedUsing` tags | Per-property replication record: declaring class, property, `rep_kind` ('rep_notify' in Phase 4a), `rep_notify_func` |

The reader is intentionally a second sweep rather than an extension of Phase 3a's `FUHTArtefactReader` — the per-property metadata-block parse path is distinct from the property-declaration parse path Phase 3a walks, and the separation keeps the two indexers individually testable and individually swappable when the UE 5.x UHT emission format drifts.

### 6.3 SQLite schema

One new table lives inside the shared `EngineSource.db` file under the `reflect_` prefix so it coexists with the Phase 3a `reflect_*` tables.

```sql
CREATE TABLE IF NOT EXISTS reflect_replicated_properties (
    class_name       TEXT NOT NULL,
    module_name      TEXT NOT NULL,
    property_name    TEXT NOT NULL,
    rep_kind         TEXT NOT NULL,            -- 'rep_notify' (Phase 4a) — 'bare_replicated' deferred to Phase 4b
    rep_notify_func  TEXT,                     -- the OnRep_* handler name from ReplicatedUsing=
    source_path      TEXT NOT NULL,            -- UHT ModuleRelativePath, same convention as Phase 3a
    source_line      INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (class_name, module_name, property_name)
);

CREATE INDEX IF NOT EXISTS idx_reflect_replicated_properties_class
    ON reflect_replicated_properties(class_name, module_name);
CREATE INDEX IF NOT EXISTS idx_reflect_replicated_properties_repnotify
    ON reflect_replicated_properties(rep_notify_func);
```

Follows Phase 1 wipe-and-rewrite semantics — `Run()` truncates and rewrites inside a single `BEGIN TRANSACTION ... COMMIT` block. DDL canonical source: `Plugins/Monolith/Source/MonolithReflectionIntel/Private/NetworkSchema.cpp`.

### 6.4 Action surface

Four actions register under `network` from `FNetworkQueryAdapter::RegisterActions`. All four carry `readOnlyHint: true`, `destructiveHint: false`, `idempotentHint: true` on the dispatcher annotations. All four participate in v0.17.0 universal response shaping (`_fields` / `_omit` / `_compact_json`).

#### `network_query("list_replicated_classes", params)`

Enumerate UCLASSes carrying at least one replicated property. **As of the [Unreleased] network-completeness workstream this captures bare `UPROPERTY(Replicated)` + `DOREPLIFETIME` (via the `CPF_Net` property flag) in addition to `ReplicatedUsing` declarations** — verified E2E (returned `Athe host projectCharacterBase` + `Uthe host projectVitalsSet`). It also covers project-plugin classes by default per the §5.2 scan-scope ladder (15 replicated classes on the E2E run with project plugins on). Cursor-paginated.

| Param | Type | EMonolithParamKind | Required | Default | Notes |
|-------|------|---------------------|----------|---------|-------|
| `module_filter` | string | `Other` | no | `""` | Substring match against `module_name`. |
| `limit` | integer | `Other` | no | `50` | Hard cap `200`. |
| `cursor` | string | `Other` | no | `""` | Opaque base64+JSON cursor. |

**Response:** `{ "classes": [ { "class_name", "module_name", "replicated_property_count" } ], "total_estimate": N, "next_cursor": "<opaque>" }`. Sorted by `replicated_property_count DESC, class_name ASC`.

#### `network_query("list_rpc_functions", params)`

Filter `reflect_ufunctions` by **replication specifier** (`reflect_ufunctions.specifiers` parsed from the `EFunctionFlags` bitfield — `FUNC_NetServer` / `FUNC_NetClient` / `FUNC_NetMulticast`) to surface the project's RPC surface. As of the [Unreleased] network-completeness workstream this is **specifier-based**, not name-prefix-based, and the scan covers project plugins by default per the §5.2 scan-scope ladder (marketplace plugins when enabled). The project's actual RPCs — which live in project project plugins — are therefore in scope. E2E returned 28 RPCs including the project's project-plugin Server RPCs. Cursor-paginated.

| Param | Type | EMonolithParamKind | Required | Default | Notes |
|-------|------|---------------------|----------|---------|-------|
| `rpc_kind` | string | `Other` | no | `""` | Exact match — `server`, `client`, `multicast`, `netmulticast`. Empty returns all four. |
| `class_name` | string | `Other` | no | `""` | Optional UCLASS filter. |
| `module_filter` | string | `Other` | no | `""` | Substring match against module name. |
| `limit` | integer | `Other` | no | `100` | Hard cap `500`. |
| `cursor` | string | `Other` | no | `""` | Opaque cursor. |

**Response:** `{ "rpcs": [ { "class_name", "module_name", "function_name", "rpc_kind", "function_flags", "return_type", "source_path", "source_line" } ], "total_estimate": N, "next_cursor": "<opaque>" }`. `rpc_kind` is derived from the replication specifier (`reflect_ufunctions.specifiers` parsed from `EFunctionFlags`) at query time. With project plugins in scope by default (§5.2) the array populates from project-plugin RPCs — e.g. the a project plugin Server RPCs on the E2E run.

#### `network_query("list_onrep_handlers", params)`

List every `OnRep_*` UFUNCTION paired with the property it covers. Cursor-paginated.

| Param | Type | EMonolithParamKind | Required | Default | Notes |
|-------|------|---------------------|----------|---------|-------|
| `class_name` | string | `Other` | no | `""` | Optional UCLASS filter. |
| `module_filter` | string | `Other` | no | `""` | Substring match. |
| `limit` | integer | `Other` | no | `100` | Hard cap `500`. |
| `cursor` | string | `Other` | no | `""` | Opaque cursor. |

**Response:** `{ "handlers": [ { "class_name", "module_name", "function_name", "covered_property", "source_path", "source_line" } ], "total_estimate": N, "next_cursor": "<opaque>" }`. `covered_property` is resolved by joining `reflect_replicated_properties.rep_notify_func == function_name`; if no match is found the field is empty (handler is orphaned — the inverse of `audit_unbalanced_onreps`).

#### `network_query("audit_unbalanced_onreps", params)`

Find `ReplicatedUsing=OnRep_X` declarations whose `OnRep_X` function does NOT exist in the same class's reflected UFUNCTION surface. Cursor-paginated.

| Param | Type | EMonolithParamKind | Required | Default | Notes |
|-------|------|---------------------|----------|---------|-------|
| `module_filter` | string | `Other` | no | `""` | Substring match. |
| `limit` | integer | `Other` | no | `100` | Hard cap `500`. |
| `cursor` | string | `Other` | no | `""` | Opaque cursor. |

**Response:** `{ "violations": [ { "class_name", "module_name", "property_name", "missing_handler" } ], "total_estimate": N, "next_cursor": "<opaque>" }`. Sorted by `(module_name, class_name, property_name)` so multi-row violations within a single class clump together.

### 6.5 Known limitations

- **Bare `UPROPERTY(Replicated)` capture: WORKS as of the [Unreleased] network-completeness workstream.** `list_replicated_classes` now picks up bare `UPROPERTY(Replicated)` + `DOREPLIFETIME` declarations via the `CPF_Net` property flag, in addition to `ReplicatedUsing`. Verified end-to-end — the query returned `Athe host projectCharacterBase` + `Uthe host projectVitalsSet`. (This was the Phase 4b WISHLIST item that has now landed for replicated-class detection; the prior "not detected" limitation is removed.)
- **`list_rpc_functions` covers project plugins by default ([Unreleased]) — the prior "empty because game-module-only" limitation is RESOLVED.** Detection is specifier-based (`reflect_ufunctions.specifiers` parsed from `EFunctionFlags` — `FUNC_NetServer` / `FUNC_NetClient` / `FUNC_NetMulticast`) rather than function-name-prefix, and the scan-scope ladder (§5.2) now walks `LoadedFrom == Project` plugins by default (and marketplace plugins when `bIndexMarketplacePluginReflection` is on). This project's actual RPCs — which live in project project plugins — are therefore in scope: E2E returned 28 RPCs including the project's project-plugin Server RPCs. The remaining narrow caveat is that Epic engine built-in plugins stay excluded by default (`bIndexEnginePluginReflection` off), so RPCs declared in engine-built-in code are out of scope unless you opt in.
- **Multi-condition replication (`COND_*`) not surfaced.** The `rep_kind` column carries `'rep_notify'` (and, post-workstream, `'bare_replicated'` for `CPF_Net`-only properties). Replication-condition gating (`COND_OwnerOnly`, `COND_SkipOwner`, etc.) is parsed by UHT but not yet read by the sweep.

---

## 6b. Reflect Namespace — Index Maintenance ([Unreleased])

The `reflect` namespace ships one WRITE/maintenance action as part of the [Unreleased] network-completeness workstream. It exists for a specific operational gap: after you change an RI indexer's code, there is no clean way to repopulate the reflection tables. The lazy bootstrap only fires when a table is **absent**, the `OnReloadComplete` refresh only fires on **Live Coding**, and `source_query("trigger_reindex")` is the **full engine** reindex (heavy, and the wrong scope). `reflect_query("rebuild_reflection_index")` fills that gap with a fast, project-only force-rebuild.

### 6b.1 `reflect_query("rebuild_reflection_index")`

Force-rebuild the RI reflection tables from PROJECT UHT artefacts.

*No parameters.*

**What it rebuilds:** `reflect_uclasses`, `reflect_uproperties`, `reflect_ufunctions`, `reflect_uinterfaces`, `reflect_uinterface_impls`, `cpp_asset_edges` (the Phase 3a tables) plus `reflect_replicated_properties` (the Phase 4a table). It re-runs the RI indexers (`FCppReflectIndexer` + `FNetworkIndexer`) over the project's on-disk UHT artefacts.

**Scope:** PROJECT only — Epic engine built-ins excluded. This is deliberate: engine-side reflection surface dwarfs the project's and floods queries with low-signal hits (the same rationale as `bIndexEnginePluginReflection` in §5.2). As of the [Unreleased] scan-scope ladder (§5.2), "project" here means the game module **plus** enabled `LoadedFrom == Project` plugins by default (and marketplace plugins when `bIndexMarketplacePluginReflection` is on) — so a rebuild repopulates project-plugin reflection, which is why `list_rpc_functions` returns the project's project-plugin RPCs after a rebuild rather than coming back empty.

**Semantics:**

- **WRITE / maintenance — NOT read-only.** It mutates the shared `EngineSource.db` tables. Annotations: `readOnlyHint: false`, `destructiveHint: false`, `idempotentHint: true`.
- **Idempotent.** Each rebuild wipes-and-rewrites the affected tables inside a single `BEGIN TRANSACTION ... COMMIT` per indexer (the same semantics as the lazy-bootstrap path). Calling it twice in a row leaves the tables in the same state as calling it once.
- **Non-destructive.** It regenerates the tables deterministically from the on-disk UHT artefacts. It does not delete artefacts, touch source, or alter any non-RI table. If a rebuild is interrupted, re-running it recovers cleanly.

**Response:** a per-table row-count summary plus an overall `ok` flag — e.g. `{ "ok": true, "rebuilt": { "reflect_uclasses": N, "reflect_uproperties": N, "reflect_ufunctions": N, "reflect_uinterfaces": N, "reflect_uinterface_impls": N, "cpp_asset_edges": N, "reflect_replicated_properties": N } }`.

**When to call it:** after an RI indexer code change (the canonical case), or any time you suspect the reflection tables are stale relative to the on-disk UHT artefacts and you don't want to pay for a full engine reindex.

---

## 7. Audit Actions on Existing Namespaces (Phase 4a — SHIPPED v0.17.0)

Four read-only audit actions register against existing host namespaces. Each is registered through the host namespace's adapter (so the existing namespace-level dispatcher annotations apply) but the handler lives inside `MonolithReflectionIntel`.

### 7.1 `material_query("audit_orphan_materials")`

Identify materials with zero inbound references in the asset graph — typical orphans from deleted Blueprint owners or refactored material chains. Cursor-paginated.

| Param | Type | EMonolithParamKind | Required | Default | Notes |
|-------|------|---------------------|----------|---------|-------|
| `path_prefix` | string | `AssetPath` | no | `"/Game/"` | `/Game/...` path prefix to scope the scan. Dispatcher rewrites `\` → `/` with a surfaced warning. |
| `limit` | integer | `Other` | no | `100` | Hard cap `500`. |
| `cursor` | string | `Other` | no | `""` | Opaque cursor. |

**Algorithm:** walk every `UMaterial` / `UMaterialInstance` under `path_prefix` via `IAssetRegistry::GetAssetsByPath`; for each, query `IAssetRegistry::GetReferencers` and emit a row when the returned referencer set is empty. Skip the engine and project transient packages.

**Response:** `{ "orphans": [ { "asset_path", "asset_class" } ], "total_estimate": N, "next_cursor": "<opaque>" }`.

Dispatcher annotations: inherits `material_query`'s namespace annotations. The action itself is read-only + idempotent.

### 7.2 `niagara_query("audit_cross_asset_refs")`

Find broken or stale asset references inside Niagara systems / emitters — referenced assets that no longer exist (deleted, renamed, moved out of `/Game/`) or whose class has shifted. Cursor-paginated.

| Param | Type | EMonolithParamKind | Required | Default | Notes |
|-------|------|---------------------|----------|---------|-------|
| `path_prefix` | string | `AssetPath` | no | `"/Game/"` | `/Game/...` path prefix to scope the scan. |
| `limit` | integer | `Other` | no | `100` | Hard cap `500`. |
| `cursor` | string | `Other` | no | `""` | Opaque cursor. |

**Algorithm:** for every `UNiagaraSystem` / `UNiagaraEmitter` under `path_prefix`, walk Phase 3a's `cpp_asset_edges` table joined against `IAssetRegistry::GetAssetByObjectPath`; emit a row for each `asset_path` whose target asset is not loadable or whose registry record's class no longer matches the stored `cpp_class`.

**Response:** `{ "broken_refs": [ { "owning_asset", "missing_or_stale_ref", "expected_class", "actual_class_or_null" } ], "total_estimate": N, "next_cursor": "<opaque>" }`.

Dispatcher annotations: inherits `niagara_query`'s namespace annotations.

### 7.3 `blueprint_query("audit_cdo_drift")`

Detect Blueprint child classes whose CDO has overridden a native C++ parent's default value — useful when a native default changes upstream and BP children silently keep the stale override. Cursor-paginated.

| Param | Type | EMonolithParamKind | Required | Default | Notes |
|-------|------|---------------------|----------|---------|-------|
| `path_prefix` | string | `AssetPath` | no | `"/Game/"` | `/Game/...` path prefix to scope the scan. |
| `class_filter` | string | `Other` | no | `""` | Substring match against the parent C++ class name. |
| `limit` | integer | `Other` | no | `100` | Hard cap `500`. |
| `cursor` | string | `Other` | no | `""` | Opaque cursor. |

**Algorithm:** for every `UBlueprint` under `path_prefix` whose `ParentClass` is a native (non-BP) UCLASS, walk the CDO's `UProperty` set comparing the BP CDO's current value to the parent native CDO's stored default; emit a row per drifted property.

**Response:** `{ "drifts": [ { "bp_asset_path", "parent_class", "property_name", "parent_default", "bp_override" } ], "total_estimate": N, "next_cursor": "<opaque>" }`. Sorted by `(bp_asset_path, property_name)` so multi-drift BPs clump together.

Dispatcher annotations: inherits `blueprint_query`'s namespace annotations.

### 7.4 `project_query("audit_orphan_assets")`

Project-wide zero-reference scan across all asset classes — `material_query("audit_orphan_materials")` is the type-scoped sibling; this is the general form. Cursor-paginated.

| Param | Type | EMonolithParamKind | Required | Default | Notes |
|-------|------|---------------------|----------|---------|-------|
| `path_prefix` | string | `AssetPath` | no | `"/Game/"` | `/Game/...` path prefix to scope the scan. |
| `asset_class_filter` | string | `Other` | no | `""` | Substring match against asset class name. |
| `limit` | integer | `Other` | no | `100` | Hard cap `500`. |
| `cursor` | string | `Other` | no | `""` | Opaque cursor. |

**Algorithm:** `IAssetRegistry::GetAssetsByPath` walk + `GetReferencers` per asset (cross-validated against Phase 3a's `cpp_asset_edges` to surface assets referenced only from C++ but not from BP/asset graph). An asset is orphaned only when both reference sets are empty.

**Response:** `{ "orphans": [ { "asset_path", "asset_class" } ], "total_estimate": N, "next_cursor": "<opaque>" }`.

Dispatcher annotations: inherits `project_query`'s namespace annotations.

---

## 8. Pipeline Composers (Phase 4a — SHIPPED v0.17.0)

Two read-only composer actions register under a new `pipeline` namespace from `FPipelineQueryAdapter::RegisterActions`. Both carry `readOnlyHint: true`, `destructiveHint: false`, `idempotentHint: true` on the dispatcher annotations. Both participate in v0.17.0 universal response shaping.

Composers fan out other registered actions serially on the game thread and assemble the results into a single JSON payload. They never mutate state — every action they invoke is itself `readOnlyHint: true`.

### 8.1 `pipeline_query("pr_review", params)`

Bundle the most common PR-review reads into a single call against a list of changed files. Inputs are project-relative paths (typically the output of `git diff --name-only`).

| Param | Type | EMonolithParamKind | Required | Default | Notes |
|-------|------|---------------------|----------|---------|-------|
| `changed_files` | `string[]` | `DiskPath` | yes | — | Array of project-relative paths. Hard cap 100 paths per call. |
| `include_drift` | bool | `Other` | no | `true` | Include CDO drift check (Phase 4a action 7.3). |

**Algorithm:** for each path in `changed_files` the composer issues `risk_query("get_hotspot_score")`, `risk_query("get_cochange_pairs")`, `decision_query("list_decisions", path_filter=path)`, `source_query("audit_module_dep_reality", module_filter=<derived>)`, and (when `include_drift` is true) `blueprint_query("audit_cdo_drift", path_prefix=<derived>)`. Results are aggregated per-path.

**Response:**

```json
{
  "files": [
    {
      "path": "Plugins/Monolith/Source/.../Foo.cpp",
      "hotspot_score": 0.81,
      "cochange_partners": [ /* top-5 partners */ ],
      "decisions": [ /* matching decision rows */ ],
      "module_dep_violations": [ /* if any */ ],
      "cdo_drifts": [ /* if include_drift true and BPs derive from this module */ ]
    }
  ],
  "summary": {
    "files_above_hotspot_threshold": 3,
    "total_decisions_touched": 7,
    "violation_count": 0
  }
}
```

### 8.2 `pipeline_query("release_readiness", params)`

Release-gate composer — bundles the signals a release-readiness check needs into a single payload. No required params.

| Param | Type | EMonolithParamKind | Required | Default | Notes |
|-------|------|---------------------|----------|---------|-------|
| `stale_decision_days` | integer | `Other` | no | `90` | Forwarded to `decision_query("list_stale")`. |
| `hotspot_threshold` | number | `Other` | no | `0.7` | Forwarded to `risk_query("get_release_window_hotspots")`. |

**Algorithm:** composer issues `monolith_status()`, `decision_query("list_stale")`, `risk_query("get_release_window_hotspots")`, and (in-process Monolith-only signals) the sentinel-list audit + CHANGELOG completeness audit specced in `.claude/rules/scoped/monolith-release.md`. Read-only end-to-end.

**Response:**

```json
{
  "status": { /* monolith_status payload */ },
  "stale_decisions": [ /* list_stale rows */ ],
  "release_window_hotspots": [ /* hotspot rows */ ],
  "sentinel_audit": { /* sentinel-list check result */ },
  "changelog_completeness": { /* commit-vs-CHANGELOG diff summary */ }
}
```

---

## 9. Phase 4b — Tag Graph + Thread-Safety Audits (WISHLIST)

Phase 4b would add two audit families on top of the Phase 4a + Phase 3b substrate:

- **`gas_query("find_tag_consumers" / "find_grant_paths" / "find_revoke_paths")`** — full GameplayTag dependency graph: every UFUNCTION / UPROPERTY that consumes a tag, every code path that grants the tag via `GiveAbility` / `AddLooseGameplayTag`, every revoke path. Requires Phase 3b's native-tag declaration tracking — without `reflect_native_tag_decls` populated, the audit cannot resolve native-side tag references.
- **`animation_query("audit_thread_safety")`** — for each AnimBP, validate that every math node touched from `BlueprintThreadSafeUpdateAnimation` carries the `BlueprintThreadSafe` meta tag. Requires Phase 3b's `reflect_uproperties.specifiers` population — Phase 4a-coarse empty-string fields make the check impossible.

**Bare `UPROPERTY(Replicated)` detection has LANDED** (no longer a Phase 4b item). The [Unreleased] network-completeness workstream made `list_replicated_classes` capture bare `Replicated` + `DOREPLIFETIME` properties via the `CPF_Net` property flag — verified E2E (returned `Athe host projectCharacterBase` + `Uthe host projectVitalsSet`). See §6.5.

**Deferral rationale.** Both remaining items are blocked on Phase 3b's tree-sitter substrate landing — see §5b for the tree-sitter vendoring cost / benefit analysis. The combined value of "tag dependency graph + thread-safety audit" is real but not blocking the Steam-build readiness work Phase 4a directly addresses.

---

## 11. Dependencies

`Plugins/Monolith/Source/MonolithReflectionIntel/MonolithReflectionIntel.Build.cs`:

| Deps | Type |
|------|------|
| `Core`, `CoreUObject`, `Engine` | `PublicDependencyModuleNames` |
| `MonolithCore`, `MonolithSource`, `SQLiteCore`, `DeveloperSettings`, `Json`, `JsonUtilities`, `Projects`, `AssetRegistry`, `UnrealEd`, `EditorSubsystem` | `PrivateDependencyModuleNames` |

`DeveloperSettings` is its own module (NOT part of `Engine`) — required for the `UDeveloperSettings`-derived `UMonolithReflectionIntelSettings`. Documented in `.claude/rules/scoped/cpp-code.md` § Module Dependencies; LNK2019 trap if omitted.

**Build.cs unchanged from Phase 1 → Phase 2.** Phase 2 adds git-log subprocess invocation (which uses `FPlatformProcess::CreateProc` from `Core`, already linked) and regex sweeps (`FRegexPattern` from `Core`). The module-dep audit's resolver reuses the existing `Core`/`CoreUObject` reflection surface. No new module deps required for Phase 2.

**Phase 3a adds one dep: `AssetRegistry`.** The Phase 3a cppreflect indexer joins the UHT class graph against `IAssetRegistry::Get().GetDependencies` to populate `cpp_asset_edges`. The UHT artefact reader itself uses only `Core` (`FRegexPattern`, `FFileHelper`, `IFileManager`) — no new dep needed there. `AssetRegistry` is the only Phase 3a addition to `PrivateDependencyModuleNames`.

**Phase 4a adds no new module deps.** The network indexer's second UHT-artefact sweep reuses `Core` (`FRegexPattern`, `FFileHelper`, `IFileManager`) — the same surface Phase 3a uses. The four cross-namespace audit handlers use `IAssetRegistry::GetReferencers` / `GetAssetsByPath` (already linked via Phase 3a's `AssetRegistry` dep). The two pipeline composers fan out other registered actions via `FMonolithToolRegistry::ExecuteAction` — already available via `MonolithCore`. **Build.cs unchanged from Phase 3a → Phase 4a.**

**Depends on `MonolithSource` (+ `UnrealEd` + `EditorSubsystem`)** — all adapters borrow `UMonolithSourceSubsystem`'s already-open `EngineSource.db` handle via `FMonolithSourceDatabase::GetRawHandle()` / `GetLock()` rather than opening their own. They MUST: UE 5.7's SQLite (`SQLITE_OS_OTHER=1` + `unreal-fs` VFS) permits only one open of a file per process, so a second open returns `SQLITE_IOERR`. The dependency is one-way (RI → MonolithSource; MonolithSource never references RI) and therefore non-circular. The Phase 2 module-dep audit and Phase 3a / Phase 4a indexers read the source-indexer's existing symbol tables through the same borrowed handle. The accessor is `GetRawHandle()`, NOT `GetRawDatabase()` (that name belongs to the unrelated `FMonolithIndexDatabase`).

No conditional-gate `WITH_*` macros — the module loads unconditionally and contributes 28 actions (5 `decision` + 5 `risk` + 1 `source` audit + 6 `cppreflect` — incl. the [Unreleased] `list_class_specifiers` — + 4 `network` + 2 `pipeline` + 1 `reflect` ([Unreleased] `rebuild_reflection_index`) + 4 audit actions across `material` / `niagara` / `blueprint` / `project`) to every install.

---

## 12. Configuration

**Editor location:** Editor Preferences → Plugins → "Monolith Reflection Intel"
**INI file:** `Config/MonolithSettings.ini`
**Section:** `[/Script/MonolithReflectionIntel.MonolithReflectionIntelSettings]`

| Setting | Default | Category | Description |
|---------|---------|----------|-------------|
| `bEnableDecisionMining` | `true` | Decision | Mine decision records from markdown corpora during indexing. When `false`, `RunDecisionIndexerOnce` skips with a status string. |
| `DecisionMinConfidence` | `0.6` | Decision | Floor in `[0, 1]` applied at query time by `list_decisions`. Per-call `min_confidence` parameter overrides this. |
| `DecisionMarkdownRoots` | `[]` | Decision | Project-relative directory paths to scan. Empty array uses defaults: `Docs/`, `Plugins/Monolith/Docs/`, `.claude/rules/`. |
| `bEnableGitCoChangeMining` | `true` | Risk | Toggle git-log mining for the risk indexers. Setting `false` short-circuits all three Phase 2 indexers (`FGitChurnIndexer`, `FGitCoChangeIndexer`, `FConditionalGateIndexer`) at `Run()` entry. |
| `MaxCoChangeWindowCommits` | `50` | Risk | Maximum commit history window per repo to walk for co-change pair detection. Clamped `[10, 500]`. Larger windows produce more pair density at the cost of indexer runtime. |
| `MaxCommitFileCount` | `50` | Risk | Per-commit file-touch cap. Commits touching more than this many files contribute zero co-change pairs (suppresses tree-wide refactor / initial-import noise). Clamped `[5, 500]`. |
| `GitMiningNoiseFilter` | `["Saved/*", "Intermediate/*", "Binaries/*", "*.uasset", "*.umap"]` | Risk | File-pattern blacklist applied to git-log output before pair / churn aggregation. Patterns are glob-style; an entry matches any file whose project-relative path matches the glob. |
| `bIndexProjectPluginReflection` | `true` | CppReflect | ([Unreleased]) Scan every enabled `LoadedFrom == Project` plugin's UHT artefacts (`Intermediate/Build/Win64/UnrealEditor`) in addition to the game module — your project plugins. Default `true`; this is what brings project-plugin replicated classes and RPCs into scope (the a project plugin Server RPCs). Set `false` to fall back to game-module-only scanning. |
| `bIndexMarketplacePluginReflection` | `false` | CppReflect | ([Unreleased]) ALSO scan enabled engine-installed marketplace plugins (`LoadedFrom == Engine` whose base dir is under `/Plugins/Marketplace/`) — e.g. installed marketplace plugins. Default `false`. Setting `true` widens the sweep to marketplace surface (E2E: ~337 → ~927 artefacts, marketplace-plugin classes). Epic engine built-in plugins remain governed separately by `bIndexEnginePluginReflection`. |
| `bIndexEnginePluginReflection` | `false` | CppReflect | Include Epic engine built-in plugin UHT artefacts in the Phase 3a sweep. Default `false` keeps the sweep scoped to the game module + project plugins (and marketplace plugins when enabled) — engine-side surface area floods low-signal hits. Setting `true` walks every UHT artefact directory under the engine, multiplying index time and DB size. |
| `UHTArtefactRoot` | `""` (auto-discover) | CppReflect | Override the UHT artefact root. Empty string resolves to `FPaths::ProjectIntermediateDir() / TEXT("Build")` and walks every `Win64/.../Inc/<Module>/UHT/` subtree. Set explicitly only when running against a non-standard intermediate layout (CI mirrors, packaged-only builds with a separated `Intermediate/`). |
| `bEnableNetworkReplicationAudit` | `true` | Network | Toggle the Phase 4a network replication indexer + the four `network_query` actions. When `false`, `FNetworkIndexer::Run` short-circuits at entry and `network_query` returns a status string per call. The cross-namespace audit actions on `material` / `niagara` / `blueprint` / `project` are not gated by this flag — they continue to function. |
| `bEnablePipelineComposers` | `true` | Pipeline | Toggle the Phase 4a `pipeline_query("pr_review")` + `pipeline_query("release_readiness")` composers. When `false`, both actions return a status string instead of fanning out. Useful for benchmarking the individual underlying actions without composer overhead. |

`UMonolithReflectionIntelSettings::Get()` returns the cached CDO — cheap, allocation-free.

`UDeveloperSettings::GetCategoryName()` returns `"Plugins"` so the panel groups with other Monolith settings.

---

## 13. Threading Model

- **Phase 1 indexer (`FDecisionRecordIndexer::Run`)** runs on whatever thread invoked `FMonolithReflectionIntelModule::RunDecisionIndexerOnce`. In practice that is the game thread (first-call adapter path) or whichever thread fired `FCoreUObjectDelegates::ReloadCompleteDelegate` (Live Coding fires this on the game thread). The indexer is single-threaded by construction; SQLite ops use a single `FSQLiteDatabase` handle that lives only for the duration of `Run`.
- **Phase 2 indexers (`FGitChurnIndexer`, `FGitCoChangeIndexer`, `FConditionalGateIndexer`)** are scheduled on background threads via `FRunnableThread` after first-call detection — `MonolithReflectionIntelModule.cpp` posts the work to a background runnable and the calling action returns immediately if the table is missing. **However:** the lazy-bootstrap subprocess that fires `git log` during first-ever-call indexing currently runs on the game thread inline. This is a documented trade-off — first-call latency on a fresh install (~200ms on the host project-scale repos) is acceptable for the simpler control flow. Subsequent reindex invocations run fully on the background thread.
- **Phase 3a indexer (`FCppReflectIndexer::Run`)** runs on a background thread during the indexer pass — UHT artefact discovery + regex sweep + `IAssetRegistry::GetDependencies` join all happen off the game thread. The SQLite wipe-and-rewrite of the six Phase 3a tables runs under `FScopeLock(&SharedDb->GetLock())` on the borrowed shared handle and follows the same `BEGIN TRANSACTION ... COMMIT` discipline as Phases 1 + 2. `IAssetRegistry::Get()` is thread-safe for the read API used (`GetDependencies`); module loading is verified done before the indexer kicks off.
- **Phase 4a network indexer (`FNetworkIndexer::Run`)** runs on a background thread — same pattern as Phase 3a's `FCppReflectIndexer`. The second UHT-artefact sweep + the `reflect_replicated_properties` wipe-and-rewrite happen off the game thread under `FScopeLock(&SharedDb->GetLock())` inside a single `BEGIN TRANSACTION ... COMMIT`. Phase 4a's four cross-namespace audit handlers (`material_query("audit_orphan_materials")`, `niagara_query("audit_cross_asset_refs")`, `blueprint_query("audit_cdo_drift")`, `project_query("audit_orphan_assets")`) run on the game thread under `FMonolithToolRegistry::ExecuteAction` — `IAssetRegistry::GetReferencers` / `GetAssetsByPath` are thread-safe but the per-asset Blueprint CDO walk in `audit_cdo_drift` requires the game thread for `UClass::GetDefaultObject` access.
- **Phase 4a pipeline composers** run on the game thread and fan out other registered actions serially via `FMonolithToolRegistry::ExecuteAction`. No `ParallelFor`, no async dispatch. The composer holds no SQLite handles directly — every read goes through the underlying action's own handle.
- **Adapter handlers (`FDecisionQueryAdapter::*`, `FRiskQueryAdapter::*`, `FModuleDepRealityAdapter::*`, `FCppReflectQueryAdapter::*`, `FNetworkQueryAdapter::*`, `FPipelineQueryAdapter::*`)** run on the game thread under `FMonolithToolRegistry::ExecuteAction`. All 26 handlers are pure read paths against the borrowed shared `EngineSource.db` handle — no mutation, no async work, no `ParallelFor`.
- Read-path adapters borrow the subsystem's single open handle via `FMonolithSourceDatabase::GetRawHandle()` and enforce a game-thread-only contract with `ensure(IsInGameThread())`. This is what makes the lock-free read safe: the subsystem's handle close runs on the game thread (its reindex trigger is game-thread-dispatched) and its async indexer uses a SEPARATE worker handle, so a game-thread read serialises against the close without a per-read lock. If the adapter surface ever fans out to background threads, reads must take `FScopeLock(&SharedDb->GetLock())` like the write path already does.
- No render-thread work. No `UPROPERTY(Replicated)`. No `Server`/`Client`/`NetMulticast` UFUNCTIONs. Editor-only by design.

---

## 14. Cross-References

- **Parent spec:** [`SPEC_CORE.md`](../SPEC_CORE.md) — see §3 Module Reference and §12 Action Count Summary
- **MCP reference:** `Docs/references/MCP.md` — `decision_query` row + `risk_query` row + `cppreflect_query` row + `network_query` row + `pipeline_query` row + `source_query("audit_module_dep_reality")` entry + the 4 Phase 4a audit actions on existing namespaces
- **C++ conventions:** `.claude/rules/scoped/cpp-code.md` — module dep gotchas (`DeveloperSettings`, `FindFilesRecursive` 6th-param, SQLite WAL trap)
- **API verification log:** `Docs/references/UE57Gotchas.md`
- **Bug class motivating the module-dep audit:** the `UPROPERTY` referencing a foreign-module type without that module being in `Build.cs` — surfaces as a confusing LNK2019 against UHT-generated `Z_Construct_*_NoRegister` symbols.
- **Release-readiness composer reference:** `.claude/rules/scoped/monolith-release.md` — the sentinel-list audit + CHANGELOG completeness audit `pipeline_query("release_readiness")` invokes.
