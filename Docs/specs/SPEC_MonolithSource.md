# Monolith — MonolithSource Module

**Parent:** [SPEC_CORE.md](../SPEC_CORE.md)
**Engine:** Unreal Engine 5.7+
**Version:** 0.21.3 (Beta)

---

## MonolithSource

**Dependencies:** Core, CoreUObject, Engine, MonolithCore, SQLiteCore, EditorSubsystem, UnrealEd, Json, JsonUtilities, Slate, SlateCore

**Note:** Module structure was flattened — the vestigial outer stub has been removed. MonolithSource registers ~19+ actions (plus `suggest_build_cs_deps`, registered onto `source` from MonolithReflectionIntel). The engine source indexer is a native C++ implementation (`UMonolithSourceSubsystem` builds `EngineSource.db` in-process). The legacy Python tree-sitter indexer (`Scripts/source_indexer/`) is no longer used.

### Classes

| Class | Responsibility |
|-------|---------------|
| `FMonolithSourceModule` | Registers ~19+ source actions |
| `UMonolithSourceSubsystem` | UEditorSubsystem. Owns engine source DB. Runs native C++ source indexer. Exposes `TriggerReindex()` (full engine re-index) and `TriggerProjectReindex()` (project C++ only, incremental). **F17 (2026-04-26):** Auto-binds `FCoreUObjectDelegates::ReloadCompleteDelegate` at `Initialize` to kick incremental project reindex on Live Coding / hot-reload completion (5s cooldown + `bIsIndexing` re-entrancy guard + bootstrap-DB-missing skip). Unbinds at `Deinitialize`. |
| `FMonolithSourceDatabase` | Read-only SQLite wrapper. Thread-safe via FCriticalSection. FTS queries with prefix matching |
| `FMonolithSourceActions` | ~14+ handlers. Helpers: IsForwardDeclaration (regex), ExtractMembers (smart class outline), DeriveIncludePath (Phase 1) |
| ~~`UMonolithQueryCommandlet`~~ | **Removed.** Replaced by standalone `monolith_query.exe` (see Section 5.1). The exe has no UE runtime dependency and starts instantly |

### Auto-Reindex on Hot-Reload (F17)

**Important:** `monolith_reindex` is the **asset/project** indexer (Blueprints, materials, textures via `MonolithIndex`). It does NOT update the C++ source DB. Source-symbol freshness is owned by this module via:

1. `source.trigger_reindex` — full clean rebuild (engine + shaders + project).
2. `source.trigger_project_reindex` — incremental, project C++ only.
3. **F17 auto-hook (2026-04-26):** `UMonolithSourceSubsystem` listens on `FCoreUObjectDelegates::ReloadCompleteDelegate`. After every Live Coding patch and after every UBT-driven editor restart that fires hot-reload, the subsystem auto-kicks `TriggerProjectReindex()` (async). Guarded by a 5-second cooldown and an in-flight `bIsIndexing` flag so multi-module reload bursts don't storm. Skips silently if `EngineSource.db` doesn't yet exist (first-install bootstrap requires a manual `source.trigger_reindex`).

After F17, agents do not need to invoke any source-reindex action manually in the common dev loop — just run UBT or Live Coding and `source_query` reflects the new symbols within ~1 second.

### Actions (~20 — namespace: "source")

| Action | Params | Description |
|--------|--------|-------------|
| `read_source` | `symbol`, `include_header`, `max_lines`, `members_only` | Get source code for a class/function/struct. FTS fallback if exact match fails |
| `find_references` | `symbol`, `ref_kind`, `limit` | Find all usage sites |
| `find_callers` | `symbol`, `limit` | All functions that call the given function |
| `find_callees` | `symbol`, `limit` | All functions called by the given function |
| `search_source` | `query`, `scope`, `limit`, `mode`, `module`, `path_filter`, `symbol_kind` | Dual search: symbol FTS + source line FTS |
| `get_class_hierarchy` | `class_name`, `direction`, `depth` | Inheritance tree (both/ancestors/descendants, max 80 shown) |
| `get_module_info` | `module_name` | Module stats: file count, symbol counts, key classes |
| `get_symbol_context` | `symbol`, `context_lines` | Definition with surrounding context |
| `read_file` | `file_path`, `start_line`, `end_line` | Read source lines by path (absolute -> DB exact -> DB suffix match) |
| `trigger_reindex` | none | Trigger full C++ engine source re-index (replaces entire EngineSource.db) |
| `trigger_project_reindex` | none | Trigger incremental project-only C++ source re-index (updates project symbols in EngineSource.db without a full rebuild) |
| `get_include_path` | `symbol` | Canonical `#include` form for a symbol, derived from its indexed file path (Phase 1). See contract below. |
| `get_signature` | `symbol` | Exact overload signature(s) for a function/method. Declaration-read primary (Phase 1). See contract below. |
| `check_deprecations` | `symbols[]` | Batch deprecation status for a symbol list, read from the `symbol_deprecations` index (Phase 1). See contract below. |
| `verify_symbols` | `symbols[]` | Batch pre-flight verdict: per symbol composes existence + include + signature + deprecation into one record (Phase 2). See contract below. |
| `find_example_usage` | `symbol`, `prefer`, `limit`, `cursor` | Ranked real call-site examples via source-line FTS (`SymbolName(`), ±3 context lines, cursor-paginated (Phase 2). See contract below. |
| `suggest_build_cs_deps` | `file_path`, `symbols[]` | Required Build.cs modules + missing deps for a file/symbol-list (Phase 2). Registered onto `source` from `MonolithReflectionIntel` — see `SPEC_MonolithReflectionIntel.md` §4b-sibling. |
| `lint_header` | `file_path` | Regex-level UHT-gotcha lint of a single header (Phase 3). Works on UNINDEXED files. Structured findings `{rule_id, line, message, severity}`; clean header → zero findings. See contract below. |
| `generate_class_stub` | `parent`, `class_name`, `module` | UCLASS-derived `.h`/`.cpp` stub pair returned as TEXT — never writes to disk (Phase 3, Decision 1). See contract below. |

**DB Location:** `Plugins/Monolith/Saved/EngineSource.db`

### Phase 1 Action Contracts (LLM C++ Ergonomics)

Three demand-proven lookups added so an agent can resolve an include, a signature, or a deprecation status in a single round-trip without reading raw source. All three are read-only / idempotent and offline-served by `monolith_query.exe` / `monolith_offline.py`.

**`get_include_path`** — `DeriveIncludePath` derives the canonical include from the symbol's indexed file path, keyed on the prefix after the module `Source/<Module>/` root:

- `Public/`, `Classes/`, `Internal/` → strip the prefix, return the includable cross-module form (`includable: true`).
- `Private/` → NOT includable from another module: `includable: false` + a `warning` ("Private header — not includable outside `<Module>`; same-module include shown") and the same-module relative form is returned (not presented as a canonical cross-module include).
- No recognised prefix (engine headers) → basename fallback.
- Always forward-slashed. Emits the owning module + a `build_cs_note` (read from the backfilled `modules.build_cs_path`).
- For a `Class::Method` input, the include resolves via the OWNING CLASS row — the method itself need not be a symbol, the file is the class's header regardless.

**`get_signature`** — declaration-read is the PRIMARY mechanism. The handler resolves the symbol via the owning class row + source-line FTS over `source_fts` (the same FTS fallback `read_source` uses), reads the declaration line(s) from the file (forward to the closing paren), and strips any trailing macro `\` and inline body. The indexed `symbols.signature` column is used as an opportunistic fast path ONLY when present AND body-free. The response reports which source was used in a `source` field: `"declaration_read"` | `"column"`. Overloads are returned as separate entries.
  - Rationale: `symbols.signature` is populated only for inline-defined functions and captures the body + macro continuations; class-body method declarations (`UGameplayStatics::ApplyDamage`, `UWorld::SpawnActor`, …) are not indexed as symbols at all, so column-only resolution would miss the bulk of the engine API.

**`check_deprecations`** — accepts `symbols[]`, batch-reads `symbol_deprecations`, returns per-symbol `{deprecated, version, message, kind}`. When the table has zero rows (schema v2 landed but no reindex yet) it returns `{ index_state: "empty", hint: "run source.trigger_reindex" }` and OMITS per-symbol verdicts — never a false "no symbol is deprecated" clean bill.

### Phase 2 Action Contracts (Round-Trip Compression)

Three actions that collapse multiple lookups into a single round-trip. `verify_symbols` + `find_example_usage` live in `MonolithSource` and are offline-served (`monolith_query.exe` / `monolith_offline.py`); `suggest_build_cs_deps` lives in `MonolithReflectionIntel` and is **NOT offline-served** (it needs the live cached query DB + on-disk Build.cs walk — same as `audit_module_dep_reality`).

**`verify_symbols`** — accepts `symbols[]`; per symbol composes the Phase-1 logic via shared C++ helpers (NOT by re-parsing the JSON handlers) into one record: `exists`, `include` / `includable` / `module` / `warning`, `signature` / `signature_source`, and `deprecated` / `deprecation_version` / `deprecation_message` / `deprecation_kind`. **`exists` for a `Class::Method` is decided by the owning class row + a source-line declaration hit (`Name(`), NEVER `symbols`-table presence** — engine class-body methods have no symbols row (Step-0 finding), so a symbols-only check would false-negative the bulk of the engine API. A missing symbol reports `exists: false` with no error and no further fields. When the deprecation index is empty (Decision 3) the record carries `deprecation_index: "empty"` instead of a verdict.

**`find_example_usage`** — substrate is source-line FTS over `source_fts` (NOT the `references` table, which is `to_symbol_id`-keyed and empty for engine API methods — Step-0). Runs an FTS query for the symbol, keeps hits whose line matches the call pattern `Name(`, reads ±3 context lines via `LoadFileToStringArray`, ranks per Decision 4, and cursor-paginates via `MonolithCursorCodec` + the rerun-slice scheme (the same moving-FTS-index rationale as `search_source`).
  - Ranking (`prefer`, default `"engine"`): `"engine"` → (0) engine `Runtime` modules, (1) other engine modules, (2) project; `"project"` → project first, then engine `Runtime`, then other engine. Tie-break by file path, then line. Engine-vs-project is decided by whether the file path is under the engine dir; the `Runtime` sub-rank by a `/Source/Runtime/` path segment.

**`suggest_build_cs_deps`** — forward direction of `audit_module_dep_reality` (see `SPEC_MonolithReflectionIntel.md` §4b-sibling). Resolves the declaring module **path-first** from `file_path`, reads its `<Module>.Build.cs` from disk (works on uncommitted/unindexed files), regex-extracts used UE types (and/or accepts `symbols[]`), resolves each type's owning module via the source index, diffs against the declared deps (Core/CoreUObject/Engine/Projects/RHI/RenderCore whitelist), and returns `required_modules[]` + `missing[]`.

### Phase 3 Action Contracts (Pre-Flight Lint + Stub Gen)

Two actions for header pre-flight + class scaffolding. Both are read-only / idempotent and offline-served by `monolith_query.exe` / `monolith_offline.py` (item 9 text mode is a pure DB read — Decision 5).

**`lint_header`** — `file_path` (required). Reads a single header via `FFileHelper::LoadFileToStringArray` and applies a deterministic regex rule table. **MUST work on UNINDEXED files** — the primary case is a header the agent just wrote, not yet in `EngineSource.db`. The `<MODULE>_API` macro check derives the module name PRIMARILY from the file path (`Source/<Module>/...` / `Plugins/<X>/Source/<Module>/...`); no DB read is required. Returns structured findings `{rule_id, line, message, severity}` (sorted by line then rule_id) + a `finding_count`; a clean header returns zero findings. `FRegexMatcher`/`FRegexPattern` are constructed as locals only (ICU init contract). Rules:

| `rule_id` | Severity | Fires when |
|-----------|----------|-----------|
| `missing_generated_body` | error | A reflected type (UCLASS/USTRUCT) declares no `GENERATED_BODY()`. |
| `generated_h_not_last` | error | The `*.generated.h` include is not the LAST `#include`. |
| `missing_generated_h_include` | error | A reflected type uses `GENERATED_BODY()` but has no `*.generated.h` include. |
| `generated_h_name_mismatch` | error | The `*.generated.h` base name ≠ the header file base name. |
| `missing_api_macro` | warning | A UCLASS-declared type lacks the expected `<MODULE>_API` export macro. |
| `reflected_member_in_non_reflected_type` | error | `UPROPERTY`/`UFUNCTION` in a file with no UCLASS/USTRUCT. |
| `invalid_specifier` | warning | A specifier token not in the cppreflect `list_class_specifiers` vocabulary. **Cross-check only — degrades gracefully (rule skipped) when RI is unavailable**, e.g. in the offline tools, which always skip it. |

**`generate_class_stub`** — `parent`, `class_name`, `module` (all required). Resolves the parent's header + owning module via the source DB and returns a `.h`/`.cpp` pair as TEXT (fields: `header`, `cpp`, `parent_include`, `api_macro`, `uses_object_initializer`). **TEXT-RETURN-ONLY — NEVER writes to disk** (Decision 1): no `write_to_disk` param, no `target_dir` param, no `written_path` in the response. v1 supports **UCLASS-derived parents ONLY** (no USTRUCT/UENUM/UINTERFACE — rejected cleanly). The `.h` emits `<MODULE>_API`, the parent header include FIRST, `"<Class>.generated.h"` LAST, and `GENERATED_BODY()`. The `.cpp` emits a plain default constructor unless the parent's indexed constructor signature requires `FObjectInitializer&` (detected by an FObjectInitializer-only ctor with no plain alternative).

### Deprecation Index (schema v2)

`check_deprecations` reads a `symbol_deprecations` table added to `EngineSource.db` in schema v2 (`SchemaVersion` 1→2). Populated by the indexer during the normal index pass: it regex-scans for `UE_DEPRECATED` / `UE_DEPRECATED_FORENGINE` / `UE_DEPRECATED_FORGAME` (and the `DeprecatedFunction` UFUNCTION specifier), parses the symbol NAME from the declaration text following the macro, and inserts a row.

| Column | Note |
|--------|------|
| `id` | INTEGER PK AUTOINCREMENT |
| `symbol_id` | **NULLABLE** — class-body methods have no `symbols` row; stored NULL when the name does not resolve. Lookups key on `symbol_name`, not this column. |
| `symbol_name` | NOT NULL — parsed identifier before `(` in the declaration |
| `version` | e.g. `"5.4"` — may be empty |
| `message` | the deprecation message string |
| `kind` | `UE_DEPRECATED` \| `UE_DEPRECATED_FORENGINE` \| `UE_DEPRECATED_FORGAME` \| `DeprecatedFunction` |

**Bootstrap requirement:** the table is created via `CREATE TABLE IF NOT EXISTS`, so it appears empty on the next index pass after schema v2 lands. **Engine deprecation rows require one FULL `source.trigger_reindex`** — the F17 project-only reindex (and `trigger_project_reindex`) covers project symbols only, not engine deprecations. Until that full pass runs, `check_deprecations` returns the `index_state: "empty"` state above.

**`build_cs_path` backfill:** the same Phase 1 indexer change backfills the `modules.build_cs_path` column (previously inserted empty). `DiscoverProjectModules` / `DiscoverEngineModules` derive each module's `<Module>.Build.cs` path from its `Source/<Module>/` dir. `get_include_path`'s `build_cs_note` reads this populated column. Existing DBs pick up the value on the same full `trigger_reindex` bootstrap.

### Class/Struct Indexer Coverage (allman + K&R)

`FMonolithCppParser::ExtractClassesAndStructs` extracts class/struct rows in two passes:

- **Phase A** — UCLASS/USTRUCT/UINTERFACE-anchored reflected types. Already allman-tolerant (looks ahead ≤5 lines for the declaration, ≤2 more for the brace), so reflected coverage is unaffected by the change below.
- **Phase B** — plain (non-reflected) classes/structs. Previously required the opening `{` **on the declaration line**, so the entire allman-style plain surface (Epic's coding standard — brace on the next line) was structurally excluded: `>99%` of exported non-reflected engine types (e.g. `FCollisionShape`, `FSceneView`, `FPrimitiveSceneProxy`) had no `symbols` row and no extracted members. Phase B is now allman-tolerant: the pattern's trailing token is a `{`-or-end-of-line alternation (so allman decls AND today's same-line/inline one-liners both match), and a ≤3-clean-line lookahead confirms the opening brace OPENS a following line. Behaviour:
  - **Forward declarations stay excluded** — `class FFoo;` fails the regex, and any match with no opening brace found in the lookahead window is DROPPED (Phase B has no UE-macro anchor proving a definition follows, unlike Phase A).
  - **Template-parameter guard** — a multi-line template parameter list can place `class T` alone on a line ahead of the real class's `{`; if the nearest preceding non-empty clean line ends with `<` or `,`, the match is treated as a template parameter and skipped (the SUBSEQUENT real declaration still indexes).

**Bootstrap requirement:** the parser change is indexer-side, no schema change. Existing `EngineSource.db` files enrich with the previously-missing plain-class rows and their members on the next full `source.trigger_reindex` (the project-only F17/`trigger_project_reindex` pass enriches project symbols only). No offline-tool change — `monolith_query.exe` / `monolith_offline.py` read the same DB and return the richer results automatically after reindex.

**Measured enrichment (full reindex 2026-06-11):** `symbols` rows `301,590 → 967,491` — plain classes `1,584 → 40,454`, plain structs `2,754 → 36,832`, functions `77,451 → 411,303` (member extraction of the newly indexed classes). DB file size unchanged at ~7.0 GB (SQLite page reuse); full-reindex wall time ~28.5 min, comparable to prior full passes. The fix also hardened the member extractor against two latent crash classes it exposed (an unterminated-brace-at-EOF out-of-bounds read and a use-after-free when the symbol array reallocates mid-extraction — both previously reachable from Phase A as well).

---
