# Monolith — MonolithIndex Module

**Parent:** [SPEC_CORE.md](../SPEC_CORE.md)
**Engine:** Unreal Engine 5.7+
**Version:** 0.21.3 (Beta)

---

## MonolithIndex

**Dependencies:** Core, CoreUObject, Engine, MonolithCore, UnrealEd, AssetRegistry, Json, JsonUtilities, SQLiteCore, Slate, SlateCore, BlueprintGraph, KismetCompiler, EditorSubsystem

### Classes

| Class | Responsibility |
|-------|---------------|
| `FMonolithIndexModule` | Registers 12 project actions (7 baseline + 1 v0.17.0 cross-module `audit_orphan_assets` + 3 test/profiling harness Wave 1 + 1 (2026-06-10) `export_asset_text`, Gap 11) |
| `FMonolithIndexDatabase` | RAII SQLite wrapper. 13 tables + 2 FTS5 + 6 triggers + 1 meta. DELETE journal mode, 64MB cache. Schema v2: `saved_hash` column (Blake3 `FIoHash` hex), `schema_version` meta key |
| `UMonolithIndexSubsystem` | UEditorSubsystem. 3-layer indexing (startup delta, live AR callbacks, full fallback). Hash-based startup catch-up. Live batched AR delegates on 2s timer. Deep asset indexing with game-thread batching. Batches every 100 assets. Progress notifications |
| `IMonolithIndexer` | Pure virtual interface: GetSupportedClasses(), IndexAsset(), GetName(), IsSentinel(), SupportsIncrementalIndex(), IndexScoped() |
| `FBlueprintIndexer` | Blueprint, WidgetBlueprint, AnimBlueprint — graphs, nodes, variables |
| `FMaterialIndexer` | Material, MaterialInstanceConstant, MaterialFunction — expressions, params, connections |
| `FAnimationIndexer` | AnimSequence, AnimMontage, BlendSpace, AnimBlueprint — tracks, notifies, slots, state machines |
| `FNiagaraIndexer` | NiagaraSystem, NiagaraEmitter — emitters, modules, parameters, renderers |
| `FDataTableIndexer` | DataTable — row names, struct type, column info |
| `FLevelIndexer` | World/MapBuildData — actors, components, sublevel references. **Editor-world skip invariant (v0.14.1, PR #28):** `IndexAsset` skips WorldPartition `Uninitialize` + `TryUnloadPackage` when the asset being indexed is the world currently open in the editor (`GEditor->GetEditorWorldContext().World()`). Prevents the indexer from tearing down the live editor WP world mid-session (fixes #20/#27). **Landscape-world safe-teardown invariant (issue #67):** worlds loaded purely to enumerate placed actors are torn down + unloaded after enumeration. For a world carrying a `ULandscapeSubsystem`, the indexer first unregisters every landscape proxy's components (`AActor::UnregisterAllComponents`, world-wide) — nulling the grass-builder state so the subsystem's `Deinitialize` no longer dereferences a null render scene — then drives `UWorld::CleanupWorld`, which clears the world subsystem collection's `bInitialized` (no GC ensure) and tears the world down normally, with no residency cost. Non-landscape worlds use the existing WorldPartition-uninit + unload path. A bare `CleanupWorld` without the unregister-first step is unsafe (it deinitializes the landscape subsystem while a grass-builder still references the null render scene, crashing) — hence the ordering. Runtime-verified: a full reindex with level indexing enabled completes with zero ensures, zero crashes, and all landscape worlds torn down (no residency) on landscape-heavy projects. |
| `FGameplayTagIndexer` | GameplayTag containers — tag hierarchies and references |
| `FConfigIndexer` | Config/INI files — sections, keys, values across config hierarchy |
| `FCppIndexer` | C++ source files — classes, functions, includes (project-level source) |
| `FGenericAssetIndexer` | StaticMesh, SkeletalMesh, Texture2D, SoundWave, etc. — metadata nodes |
| `FDependencyIndexer` | Hard + Soft package dependencies (runs after all other indexers) |
| `FMonolithIndexNotification` | Slate notification bar with throbber + percentage |

> **Shared read-side serializer (2026-06-07).** The DataAsset indexer's `PropertyToJsonValue` field serializer was deduplicated into the new `FMonolithReflectionReader` helper in `MonolithCore` (see [`SPEC_MonolithCore.md`](SPEC_MonolithCore.md)). The indexer now calls the shared reader instead of carrying its own copy — the same single implementation the Blueprint CDO actions (`get_cdo_properties`) and `seed_data_asset`'s `read_back_values` use, so indexed DataAsset field JSON and live verify-after-write JSON are produced by one code path.

> **`FUserDefinedStructIndexer` `<unresolved>` field guard (issue #70).** `FUserDefinedStructIndexer::IndexAsset` (`Indexers/UserDefinedStructIndexer.cpp`) previously called `FProperty::GetCPPType()` unconditionally while indexing UDS fields. Several property subclasses dereference their inner type pointer inside `GetCPPType()` with no null guard, so a field whose type can no longer resolve — e.g. a `TSubclassOf<X>` pointing at a deleted Blueprint, leaving `MetaClass` null — asserted (`check(MetaClass)`, `PropertyClass.cpp:160`) and took the editor down mid deep-index. A file-local `SafeGetCPPType` helper now returns `GetCPPType()` for every well-formed property and a `<unresolved>` placeholder only when the inner pointer the assert would dereference is null. It covers the verified asserting paths: `FObjectProperty`/`FSoftObjectProperty` (`PropertyClass`), `FClassProperty`/`FSoftClassProperty` (`MetaClass`/`PropertyClass`), `FStructProperty` (`Struct`), and `FEnumProperty` (`GetEnum()`); `FByteProperty` already null-guards internally. Both the JSON `type` field and the indexed-variable `VarType` route through the helper, so a broken field indexes as `<unresolved>` instead of crashing. Behavior is identical for all well-formed properties.

### Actions (12 — namespace: "project")

| Action | Params | Description |
|--------|--------|-------------|
| `search` | `query` (required), `limit` (50) | FTS5 full-text search across all indexed assets, nodes, variables, parameters |
| `find_references` | `asset_path` (required) | Bidirectional dependency lookup |
| `find_by_type` | `asset_type` (required), `limit` (100), `offset` (0) | Filter assets by class with pagination |
| `get_stats` | none | Row counts for all 13 tables + asset class breakdown (top 20) |
| `get_asset_details` | `asset_path` (required) | Deep inspection: nodes, variables, references for a single asset |
| `list_gameplay_tags` | `prefix` (optional) | List indexed gameplay tags, optionally filtered by prefix |
| `search_gameplay_tags` | `query` (required) | Search gameplay tags and return referencing assets |
| `audit_orphan_assets` | `asset_class_filter` (optional), `limit` (50, cap 200), `cursor` (optional) | **v0.17.0 (cross-module from `MonolithReflectionIntel`).** List `/Game/.../*.uasset` assets with ZERO `IAssetRegistry` referencers AND zero entries in `cpp_asset_edges`. Strictest orphan signal for pre-release cleanup. Excludes `/Engine/*` + `/Memory/*`. Read-only, cursor-paginated |
| `export_asset_text` | `asset_path` (required), `object_filter` (optional), `grep_pattern` (optional), `max_bytes` (default 262144) | **(2026-06-10, Gap 11) — `ProjectExportAssetTextAction.cpp`.** Export an asset to its native T3D text dump (via `UExporter::ExportToOutputDevice` into an `FStringOutputDevice`) and return the text (or grepped excerpts) directly. The **universal escape hatch** for surfaces no typed read exposes — **prefer the typed actions first** (`get_node_details` for Blueprint/AnimGraph nodes, `inspect_chooser` for chooser tables, `list_graphs` for graph structure); reach for this only when no typed action covers what you need. `object_filter` (name/class substring, case-insensitive) scopes the export to a single matching sub-object; `grep_pattern` (case-insensitive substring) returns only matching lines plus surrounding context. `max_bytes` caps the returned payload — a payload over budget **hard-errors** (with advice to narrow via `grep_pattern`/`object_filter`) rather than truncating silently mid-T3D; asking past the internal ceiling is also rejected. No Build.cs change (`Engine` + `UnrealEd` deps already present). |

**Test/Profiling Harness — Wave 1 (3 — post-save freshness / disk state / sandboxed cleanup)**

| Action | Params | Description |
|--------|--------|-------------|
| `refresh_assets` | `asset_paths[]` (required), `wait_for_asset_registry` (default true), `wait_for_disk` (default false) | Force a synchronous asset-registry rescan of the requested `/Game/...` package or directory paths (post-save freshness). `wait_for_asset_registry` drains pending registry work so subsequent queries see fresh state; `wait_for_disk` bounded-polls until each package's backing file exists with size > 0 |
| `get_saved_asset_state` | `asset_path` (required) | Return disk-backed state for an asset — class, package, disk path, file size, mtime, dependencies, and referencers |
| `cleanup_generated_assets` | `paths[]` (required), `dry_run` (default true), `require_no_referencers` (default true), `remove_empty_folders` (default false) | Safely delete generated throwaway assets with reference checks. **HARD allowlist guard:** refuses any path outside `/Game/Tests/Monolith/`. Dry-run by default (reports what would be deleted without touching disk); `require_no_referencers` skips any asset still referenced from outside the request set; `remove_empty_folders` prunes now-empty folders under the allowlist |

### Database Schema

**13 Tables:** assets, nodes, connections, variables, parameters, dependencies, actors, tags, tag_references, configs, cpp_symbols, datatable_rows, meta

**2 FTS5 Virtual Tables:**
- `fts_assets` — content=assets, tokenize='porter unicode61', columns: asset_name, asset_class, description, package_path
- `fts_nodes` — content=nodes, tokenize='porter unicode61', columns: node_name, node_class, node_type

**DB Location:** `Plugins/Monolith/Saved/ProjectIndex.db`

### Incremental Indexing

The project indexer uses a 3-layer architecture to keep `ProjectIndex.db` in sync without costly full rebuilds:

**Layer 1 — Startup Catch-Up (hash-based delta)**

On editor startup, `UMonolithIndexSubsystem` runs a fast delta engine:
1. `EnumerateAllPackages()` collects all discoverable `.uasset` packages with their `FIoHash` (Blake3).
2. Hash comparison against the `saved_hash` column in the `assets` table identifies added, removed, and changed assets. Move detection uses a `TMultiMap<FIoHash, FString>` to match removed→added pairs with identical hashes.
3. Delta application (inserts, updates, deletes, renames) executes in a single SQLite transaction.
4. Hash updates are deferred until after commit for crash recovery — if the editor crashes mid-index, the next startup re-detects the delta.

Performance: ~14K assets compared in ~20ms. <1s total startup time with no changes.

**Layer 2 — Live Asset Registry Callbacks**

Four AR delegates are registered at startup:
- `OnAssetsAdded` — new assets
- `OnAssetsRemoved` — deleted assets
- `OnAssetRenamed` — moved/renamed assets
- `OnAssetsUpdatedOnDisk` — externally modified assets

Events are batched into a pending queue and drained on a 2-second timer tick. The drain deduplicates entries (same asset touched multiple times within the window) and applies changes in a single transaction.

**Layer 3 — Forced Full Reindex (fallback)**

`monolith_reindex()` defaults to incremental mode (Layer 1 logic). Passing `force=true` triggers a full wipe-and-rebuild: drops all table data, re-enumerates, and re-indexes every asset. Used when the DB is suspected corrupt or after schema migrations.

**Schema v2 Migration**

Schema v2 adds:
- `saved_hash TEXT` column on the `assets` table (stores Blake3 `FIoHash` as hex string)
- `schema_version` key in the `meta` table
- Index on `saved_hash` for fast lookup

Migration is automatic: on startup, `PRAGMA table_info(assets)` checks for the `saved_hash` column. If missing, `ALTER TABLE assets ADD COLUMN saved_hash TEXT` runs followed by index creation.

**IMonolithIndexer Interface Additions**

| Method | Purpose |
|--------|---------|
| `IsSentinel()` | Returns true if this indexer acts as a sentinel for a specific asset type (used by incremental path to decide which indexers to invoke) |
| `SupportsIncrementalIndex()` | Returns true if the indexer can process individual asset changes without a full rebuild |
| `IndexScoped()` | Index a specific set of assets (subset of full index). Default implementation falls back to `IndexAsset()` per asset |

**Plugin Content Scope Fix**

The `bInstalled` filter on plugin content paths was replaced with explicit path enumeration. This fixes discovery of project-local plugins (e.g., DrawCallReducer, NiagaraDestructionDriver) that previously reported `bInstalled=false` and were excluded from indexing. The `MeshCatalogIndexer` paths were also corrected to use the new enumeration.

---
