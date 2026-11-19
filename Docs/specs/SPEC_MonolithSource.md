# Monolith — MonolithSource Module

**Parent:** [SPEC_CORE.md](../SPEC_CORE.md)
**Engine:** Unreal Engine 5.7+
**Version:** 0.25.0 (Beta)

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
2. `source.trigger_project_reindex` — incremental, project C++ only. Purges every previously indexed project file first, then re-indexes (see *Incremental Project Reindex* below).
3. **F17 auto-hook (2026-04-26):** `UMonolithSourceSubsystem` listens on `FCoreUObjectDelegates::ReloadCompleteDelegate`. After every Live Coding patch and after every UBT-driven editor restart that fires hot-reload, the subsystem auto-kicks `TriggerProjectReindex()` (async). Guarded by a 5-second cooldown and an in-flight `bIsIndexing` flag so multi-module reload bursts don't storm. Skips silently if `EngineSource.db` doesn't yet exist (first-install bootstrap requires a manual `source.trigger_reindex`).

After F17, agents do not need to invoke any source-reindex action manually in the common dev loop — just run UBT or Live Coding and `source_query` reflects the new symbols within ~1 second.

The auto-hook stamps its 5-second cooldown only when `TriggerProjectReindex()` reports that a run actually started. Stamping unconditionally (the pre-0.22.0 behaviour) suppressed the next five seconds of auto-kicks on behalf of a reindex that never ran.

### Reindex Result Contract (0.22.0)

`TriggerReindex()` and `TriggerProjectReindex()` return `bool`: `true` only when a run actually started. They return `false` when a reindex is already in flight, when the worker thread cannot be created, and — for `TriggerProjectReindex` — when `EngineSource.db` does not exist yet. `source.trigger_reindex` / `source.trigger_project_reindex` surface that as an **error** naming the cause; earlier builds always answered with a success message regardless.

Every exit from `FMonolithSourceIndexer` now routes through `CompleteRun()`, which clears the running flag and broadcasts `OnComplete`. Two exits used to return silently — a failed `FRunnableThread::Create` in `StartAsync`, and a failed `OpenForWriting` in `Run`. Because the subsystem clears `bIsIndexing` from that callback, either one latched the flag and every later request was refused with "Indexing already in progress" until the editor restarted. `Source/MonolithSource/Private/Tests/MonolithSourceIndexerFailureTest.cpp` covers the writer-open exit.

The completion callback hops to the game thread and cannot be cancelled once queued, so it holds a `TWeakObjectPtr` to the subsystem (never `this`) and re-checks a shutdown flag before reopening the database. `Deinitialize` clears the indexer's delegates before stopping and deleting it, so a worker broadcasting on its way out cannot re-enter a subsystem that is already tearing down.

### Incremental Project Reindex (schema v3)

`TriggerProjectReindex()` (and the F17 auto-kick) runs `FMonolithSourceIndexer` with `SetCleanBuild(false)`, no engine source path, project phase only. It keeps the engine rows and **replaces** the project rows:

1. **Purge.** Before `LoadExistingSymbols`, `FMonolithSourceDatabase::PurgeFilesUnderPaths({<Project>/Source, <Project>/Plugins})` deletes every `files` row whose path lies under either root, plus everything derived from those files, in one transaction: `references` (by `file_id`, and by `from_symbol_id` / `to_symbol_id` in the purged symbol set — type references carry `from_symbol_id = 0` and are reachable only through `file_id`), `inheritance` (child or parent purged), `symbol_deprecations` (`file_id` or `symbol_id` purged), `includes`, `source_fts` chunks, `symbols` (the `symbols_ad` trigger keeps `symbols_fts` in sync), and the `files` rows. Surviving symbols whose `parent_symbol_id` pointed into the purged set are set to NULL, and `modules` rows at or under the roots that no longer own a file are removed. Id sets are staged in connection-private TEMP tables so each delete is a single set-based statement. Paths are matched by prefix with `\` folded to `/` and ASCII case folded on both sides.
2. **Re-index.** Project modules are rediscovered and indexed from scratch, so files deleted from disk, symbols removed from a file, and modules or plugins deleted entirely all disappear, and nothing is duplicated.

Purging by path prefix rather than by the discovered module set is what catches modules and plugins that no longer exist on disk. The Monolith plugin itself lives under `<Project>/Plugins`, so it is purged and re-indexed like any other project plugin. A full `source.trigger_reindex` is unaffected — it drops and recreates every table via `ResetDatabase()`.

**Cost.** `source_fts.file_id` is `UNINDEXED` and `references` / `includes` have no `file_id` index, so the purge scans those tables once per run. On a full engine index (~2.3M source chunks, ~4.7M references) that is about a second with a warm page cache and noticeably longer cold.

**Id resolution.** `InsertModule` / `InsertFile` look the row up first and only INSERT when it is absent. They previously ran `INSERT OR IGNORE` and read `GetLastInsertRowId()`, which SQLite does not reset when an insert is ignored — an existing file or module resolved to the rowid of an unrelated earlier insert (typically a symbol). An existing row now has its metadata refreshed (`files`: module_id, file_type, line_count, last_modified; `modules`: module_type, build_cs_path).

**Schema v3.** `symbol_deprecations` gained a nullable `file_id` column (below) so the purge can reach rows whose `symbol_id` is NULL. `CreateTablesIfNeeded()` adds the column in place (`PRAGMA table_info` check, then `ALTER TABLE ... ADD COLUMN`) on a pre-v3 database, and the indexer now stamps `meta.schema_version` with the schema constant rather than a literal `1`. **Legacy rows** written before v3 keep `file_id = NULL`; those that also have `symbol_id = NULL` cannot be attributed to a file, so a stale project deprecation of that kind survives incremental passes until one full `source.trigger_reindex`.

### Actions (~20 — namespace: "source")

| Action | Params | Description |
|--------|--------|-------------|
| `read_source` | `symbol`, `include_header`, `max_lines`, `members_only` | Get source code for a class/function/struct. FTS fallback if exact match fails |
| `find_references` | `symbol`, `ref_kind`, `limit` | Find all usage sites. `symbol` may be `Class::Method` (see Qualified Names) |
| `find_callers` | `symbol`, `limit` | All functions that call the given function. Accepts `Class::Method` |
| `find_callees` | `symbol`, `limit` | All functions called by the given function. Accepts `Class::Method` |
| `search_source` | `query`, `scope`, `limit`, `mode`, `module`, `path_filter`, `symbol_kind` | Dual search: symbol FTS + source line FTS |
| `get_class_hierarchy` | `class_name`, `direction`, `depth` | Inheritance tree (both/ancestors/descendants, max 80 shown) |
| `get_module_info` | `module_name` | Module stats: file count, symbol counts, key classes |
| `get_symbol_context` | `symbol`, `context_lines` | Definition with surrounding context |
| `read_file` | `file_path`, `start_line`, `end_line` | Read source lines by path. Order: existing absolute path -> forward-slash exact -> forward-slash suffix anchored at `/` -> backslash exact -> backslash suffix -> unindexed `Engine/...` joined onto the engine root of an indexed engine file. Same order as the offline `monolith_query.exe read_file` |
| `trigger_reindex` | none | Trigger full C++ engine source re-index (replaces entire EngineSource.db) |
| `trigger_project_reindex` | none | Trigger incremental project-only C++ source re-index: purges all previously indexed project rows, then re-indexes project source, without touching engine rows |
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

**`check_deprecations`** — accepts `symbols[]`, reads `symbol_deprecations`, returns per-symbol `{deprecated, found, version, message, kind}`. `symbol_name` holds bare identifiers only, so a `Class::Method` input with no exact row is retried with its trailing identifier (#155); qualified inputs then carry `match: "qualified"` (the linked `symbols.qualified_name` matches the requested owner) or `match: "name"` (bare-name hit, owner not verified), plus `matched_name` when the tail was used. `match` is set on every `found: true` verdict, deprecated or not (bare-name inputs are always `"name"`; for a non-deprecated qualified input the owner is checked against `symbols` rows), and is omitted when `found: false`. `version` / `message` / `kind` appear only when `deprecated: true`. The text output (`content[].text`, compared by the offline parity gate) is unchanged. `found: false` means no `symbols` row has that name (or its tail) at all, so `deprecated: false` is not a verdict there; `found: true` + `deprecated: false` is a real "not deprecated". When the table has zero rows (schema v2 landed but no reindex yet) it returns `{ index_state: "empty", hint: "run source.trigger_reindex" }` and OMITS per-symbol verdicts — never a false "no symbol is deprecated" clean bill.

### Phase 2 Action Contracts (Round-Trip Compression)

Three actions that collapse multiple lookups into a single round-trip. `verify_symbols` + `find_example_usage` live in `MonolithSource` and are offline-served (`monolith_query.exe` / `monolith_offline.py`); `suggest_build_cs_deps` lives in `MonolithReflectionIntel` and is **NOT offline-served** (it needs the live cached query DB + on-disk Build.cs walk — same as `audit_module_dep_reality`).

**`verify_symbols`** — accepts `symbols[]`; per symbol composes the Phase-1 logic via shared C++ helpers (NOT by re-parsing the JSON handlers) into one record: `exists`, `include` / `includable` / `module` / `warning`, `signature` / `signature_source`, and `deprecated` / `deprecation_version` / `deprecation_message` / `deprecation_kind`. **`exists` for a `Class::Method` is decided by the owning class row + a source-line declaration hit (`Name(`), NEVER `symbols`-table presence** — indexes built before parser v2 have no member rows for reflected types (#156), so a symbols-only check would false-negative the bulk of the engine API on them. A missing symbol reports `exists: false` with no error and no further fields. When the deprecation index is empty (Decision 3) the record carries `deprecation_index: "empty"` instead of a verdict.

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
| `symbol_id` | **NULLABLE** — stored NULL when the name does not resolve to a `symbols` row (parser v2 prefers the row the same file declares at that line). Lookups key on `symbol_name`, not this column. |
| `symbol_name` | NOT NULL — parsed identifier before `(` in the declaration |
| `version` | e.g. `"5.4"` — may be empty |
| `message` | the deprecation message string |
| `kind` | `UE_DEPRECATED` \| `UE_DEPRECATED_FORENGINE` \| `UE_DEPRECATED_FORGAME` \| `DeprecatedFunction` |
| `file_id` | **NULLABLE**, schema v3 — the file the row was extracted from. Used only by the incremental purge; read paths ignore it. NULL on rows written before v3. |

**Bootstrap requirement:** the table is created via `CREATE TABLE IF NOT EXISTS`, so it appears empty on the next index pass after schema v2 lands. **Engine deprecation rows require one FULL `source.trigger_reindex`** — the F17 project-only reindex (and `trigger_project_reindex`) covers project symbols only, not engine deprecations. Until that full pass runs, `check_deprecations` returns the `index_state: "empty"` state above.

**`build_cs_path` backfill:** the same Phase 1 indexer change backfills the `modules.build_cs_path` column (previously inserted empty). `DiscoverProjectModules` / `DiscoverEngineModules` derive each module's `<Module>.Build.cs` path from its `Source/<Module>/` dir. `get_include_path`'s `build_cs_note` reads this populated column. Existing DBs pick up the value on the same full `trigger_reindex` bootstrap.

### Qualified Names (`Class::Method`)

`symbols.name` and `symbol_deprecations.symbol_name` store bare identifiers; `symbols.qualified_name` stores `Parent::Name` with only the innermost parent scope. `find_references` / `find_callers` / `find_callees` resolve a qualified request by exact name first, then by the trailing identifier narrowed to rows whose `qualified_name` equals the request or is a `::`-suffix of it (so `UE::Foo::Bar::Method` matches a stored `Bar::Method`). If no row has that owner, every trailing-identifier match is used and the text output starts with a note saying so.

### Class/Struct Indexer Coverage (allman + K&R)

`FMonolithCppParser::ExtractClassesAndStructs` extracts class/struct rows in two passes:

- **Phase A** — UCLASS/USTRUCT/UINTERFACE-anchored reflected types. Allman-tolerant (looks ahead ≤5 lines for the declaration, ≤2 more for the brace). Until parser v2 Phase A passed the `{` line itself as the first body line to `ExtractMembers`, so the member brace depth never returned to 0 and **no member of any reflected type was indexed** (#156 — `UCommonUserWidget`, every `UCLASS` method). Phase A now starts on the line after the brace, like Phase B.
- **Phase B** — plain (non-reflected) classes/structs. Previously required the opening `{` **on the declaration line**, so the entire allman-style plain surface (Epic's coding standard — brace on the next line) was structurally excluded: `>99%` of exported non-reflected engine types (e.g. `FCollisionShape`, `FSceneView`, `FPrimitiveSceneProxy`) had no `symbols` row and no extracted members. Phase B is now allman-tolerant: the pattern's trailing token is a `{`-or-end-of-line alternation (so allman decls AND today's same-line/inline one-liners both match), and a ≤3-clean-line lookahead confirms the opening brace OPENS a following line. Behaviour:
  - **Forward declarations stay excluded** — `class FFoo;` fails the regex, and any match with no opening brace found in the lookahead window is DROPPED (Phase B has no UE-macro anchor proving a definition follows, unlike Phase A).
  - **Template-parameter guard** — a multi-line template parameter list can place `class T` alone on a line ahead of the real class's `{`; if the nearest preceding non-empty clean line ends with `<` or `,`, the match is treated as a template parameter and skipped (the SUBSEQUENT real declaration still indexes).

**Bootstrap requirement:** the parser change is indexer-side, no schema change. Existing `EngineSource.db` files enrich with the previously-missing plain-class rows and their members on the next full `source.trigger_reindex` (the project-only F17/`trigger_project_reindex` pass enriches project symbols only). No offline-tool change — `monolith_query.exe` / `monolith_offline.py` read the same DB and return the richer results automatically after reindex.

### Parser v2 (#156)

`MonolithSourceSchema::ParserVersion` = 2, stamped as `meta.parser_version` by a full (clean) index. The subsystem logs a warning on DB open when the stamp is older; table layout is unchanged (`SchemaVersion` stays 3). Changes:

- **Reflected-type members** — the Phase A off-by-one above.
- **Member extractor** — skips a line only when it starts inside a nested/inline body (a line that opens one, e.g. `int32 Get() const {`, is still read); folds multi-line parameter lists into one declaration (signature joined, `LineEnd == LineStart` kept for declarations); consumes multi-line `UFUNCTION(...)`/`UPROPERTY(...)` specifier lists so the flag reaches the declaration; ignores `typedef`/`using` lines.
- **Out-of-line definitions** (`ExtractFreeFunctions`) — matches the head (`[specifiers] Ret Name(` or an `A::A(` / `A::~A(` constructor/destructor), walks the parameter list with balanced parens (≤15 lines), then requires the `{` on the same or a following line with only qualifiers between (`const`, `noexcept`, `override`, `final`, ref-qualifiers, `-> T`), or a constructor initializer list. A `;` first rejects (prototypes, `TAutoConsoleVariable` globals). Bodies are skipped after a match. Previously only same-line-`{` definitions matched, so allman `.cpp` files produced no symbols.
- **Call references** (`MonolithReferenceBuilder`) — a definition header ending its line followed by a line opening with `{` (or a constructor initializer list) now starts a caller body; previously only same-line-`{` bodies produced `call` references.
- **Deprecation linking** — `symbol_id` prefers the symbol this file declares between the macro line and the parsed declaration line, so `qualified_name` through the link names the real owner.
- **Duplicate files** — a path reachable from two discovered module roots (a nested `Source` dir) is parsed once per run; before, its rows were inserted twice and `meta.total_files` exceeded `COUNT(*) FROM files`.

Not handled: template out-of-line definitions (`TFoo<T>::Bar`), operator definitions, caller bodies whose header spans several lines in the reference pass. Existing databases need one full `source.trigger_reindex`.

**Measured enrichment (full reindex 2026-06-11):** `symbols` rows `301,590 → 967,491` — plain classes `1,584 → 40,454`, plain structs `2,754 → 36,832`, functions `77,451 → 411,303` (member extraction of the newly indexed classes). DB file size unchanged at ~7.0 GB (SQLite page reuse); full-reindex wall time ~28.5 min, comparable to prior full passes. The fix also hardened the member extractor against two latent crash classes it exposed (an unterminated-brace-at-EOF out-of-bounds read and a use-after-free when the symbol array reallocates mid-extraction — both previously reachable from Phase A as well).

---
