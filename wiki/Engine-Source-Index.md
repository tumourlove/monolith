# Engine Source Index

Monolith includes a **native C++ source indexer** that runs automatically on editor startup. No Python, no external tools, no setup required. It indexes the entire UE engine source tree into a local SQLite database, enabling instant API lookups, call graphs, and class hierarchy queries.

## How It Works

1. **Automatic on startup** — The native indexer runs inside the editor when it first launches. No separate script to run.
2. **1M+ symbols indexed** — full class definitions, inheritance links, call graphs. As of the v0.19.0 allman-brace parser fix the engine `symbols` table tripled (~301K → ~967K rows) — see the note below.
3. **Database** — Stored in `Plugins/Monolith/Saved/EngineSource.db` (overridable via the `EngineSourceDBPathOverride` setting).
4. **Fully offline** — All queries run against the local SQLite database. No network access needed.

> **v0.19.0 — plain-class indexing gap fixed.** The non-reflected class/struct extractor used to require the opening `{` on the declaration line, but Epic's coding standard puts it on the next line (allman). That structurally excluded the vast majority of exported non-reflected engine types — `FCollisionShape`, `FSceneView`, `FPrimitiveSceneProxy`, `FScopeLock`, `FPaths`, `FRegexPattern` and ~40K others had no row and no extracted members, so `get_include_path`, `read_source`, `get_class_hierarchy`, `verify_symbols`, `get_signature`, and `find_example_usage` all degraded or failed on them. The parser now accepts the allman brace. Existing `EngineSource.db` files enrich on the next full `trigger_reindex` — the offline tools then return the richer results automatically.

## Available Actions (20)

| Action | Description |
|--------|-------------|
| `read_source` | Full source code for any class (or `members_only` mode for just the interface) |
| `find_references` | Type references across the engine codebase |
| `find_callers` | Who calls a given function |
| `find_callees` | What a function calls |
| `search_source` | Full-text search across all indexed source (cursor-paginated) |
| `get_class_hierarchy` | Ancestors and descendants with configurable depth |
| `get_module_info` | Module path, type, file count, key classes |
| `get_symbol_context` | Definition with surrounding context lines |
| `read_file` | Raw source file by relative path |
| `trigger_reindex` | Re-run the engine source indexer |
| `trigger_project_reindex` | Incremental re-index of project C++ source |
| `audit_module_dep_reality` | Find UE type references whose declaring module is missing from the file's Build.cs deps (the LNK2019 bug class) |

### LLM C++ authoring ergonomics (v0.19.0, 8 actions)

Read-only, offline-served lookups so an AI agent resolves an include, signature, deprecation, or Build.cs dep in one round-trip instead of reading raw source.

| Action | Description |
|--------|-------------|
| `get_include_path` | Canonical `#include` for a symbol (resolves via the owning class header). Public/Classes/Internal headers are includable cross-module; Private headers report `includable:false` with a same-module note |
| `get_signature` | Exact declaration signature(s) for a function or `Class::Method`, with inline bodies + macro line-continuations stripped. Overloads returned separately |
| `check_deprecations` | Batch `UE_DEPRECATED` status for a `symbols[]` list. Returns `index_state:"empty"` (never a false clean bill) if the deprecation index hasn't been built by a reindex yet |
| `verify_symbols` | Batch pre-flight verdict — existence + include + signature + deprecation composed per symbol. A missing symbol reports `exists:false`, no error |
| `find_example_usage` | Ranked real call-site examples via source-line FTS, ±3 lines of context, `prefer` engine/project ranking, cursor-paginated |
| `suggest_build_cs_deps` | Suggest Build.cs `PrivateDependencyModuleNames` for a file (or symbol list). Reads the declaring module's `Build.cs` from disk — works on uncommitted files |
| `lint_header` | Regex-level UHT-gotcha lint of a single header — **works on unindexed files you just wrote** |
| `generate_class_stub` | UCLASS-derived `.h`/`.cpp` stub pair returned as TEXT (never writes to disk) |

## Your Project's C++ Source

The native indexer also indexes your project's C++ source (everything under `Source/` and `Plugins/`) into `EngineSource.db` alongside the engine symbols, so `find_callers`/`find_callees`/`get_class_hierarchy` span your project code too. No Python or separate script is needed.

- **Automatic refresh** — after every Live Coding patch or hot reload, Monolith re-indexes your project source in the background (5-second cooldown). It is skipped until the engine index exists.
- **Manual refresh** — `source_query("trigger_project_reindex")` runs the same pass on demand.
- **What the pass does** — it removes every row previously indexed from your project's `Source/` and `Plugins/` trees, then indexes them again. Files, functions, classes, modules and whole plugins you deleted disappear from the index, and nothing is duplicated. Engine rows are never touched.
- **Full rebuild** — `source_query("trigger_reindex")` rebuilds the whole database (engine + shaders + project). Run it once after upgrading from a build older than source DB schema v3 to clear any stale project deprecation entries the incremental pass cannot attribute to a file.

## Offline Queries (Editor Not Running)

When the editor is down, use the standalone `monolith_query.exe` to query the same databases. Zero UE dependency, instant startup.

```bash
# Search engine source
Plugins/Monolith/Binaries/monolith_query.exe source search_source FCharacterMovementComponent --limit=5

# Read a class
Plugins/Monolith/Binaries/monolith_query.exe source read_source ACharacter --max-lines=50

# Class hierarchy
Plugins/Monolith/Binaries/monolith_query.exe source get_class_hierarchy ACharacter --depth=3 --direction=down

# Project asset search
Plugins/Monolith/Binaries/monolith_query.exe project search damage --limit=10
```

`monolith_offline.py` (`Scripts/monolith_offline.py`) is a maintained stdlib-only dev fallback for the exe, kept byte-identical to it. Both offline tools serve the full Reflection-Intelligence surface (cppreflect, network, decision, risk) byte-identical to the live MCP server. The exe auto-detects database paths relative to its own location.

## Usage Tips

- **Always use `source_query` when debugging** — never guess UE API signatures, include paths, or function names
- **`read_source` with `members_only: true`** — returns the class interface without function bodies, much more compact
- **`find_callers`/`find_callees`** — great for understanding how engine systems interact
- **`get_class_hierarchy`** — trace inheritance from any class up or down the tree
