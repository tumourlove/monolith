# Monolith — MonolithIndex Module

**Parent:** [SPEC_CORE.md](../SPEC_CORE.md)
**Engine:** Unreal Engine 5.7+
**Version:** 0.23.0 (Beta)

---

## MonolithIndex

**Dependencies:** Core, CoreUObject, Engine, MonolithCore, UnrealEd, AssetRegistry, Json, JsonUtilities, SQLiteCore, Slate, SlateCore, BlueprintGraph, KismetCompiler, EditorSubsystem

### Classes

| Class | Responsibility |
|-------|---------------|
| `FMonolithIndexModule` | Registers 12 project actions (7 baseline + 1 v0.17.0 cross-module `audit_orphan_assets` + 3 test/profiling harness Wave 1 + 1 (2026-06-10) `export_asset_text`, Gap 11) |
| `FMonolithIndexDatabase` | RAII SQLite wrapper. 13 tables + 2 FTS5 + 6 triggers + 1 meta. DELETE journal mode, 64MB cache. Schema v2: `saved_hash` column (Blake3 `FIoHash` hex), `schema_version` meta key. Schema v3: `deep_indexed_hash` + `deep_index_attempts` columns on `assets` (full-index resume — see **Full-Index Resume**) |
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
| `search` | `query` (required, max 4096 chars), `limit` (50, clamped 1-1000), `asset_class` (string or array, optional) | FTS5 full-text search over the `fts_assets` and `fts_nodes` columns. **Not** variables or parameters — those are structured rows, reachable via `get_asset_details`. See **Search Error Contract** and **`asset_class` filter** below |
| `find_references` | `asset_path` (required) | Bidirectional dependency lookup |
| `find_by_type` | `asset_type` (required), `limit` (100), `offset` (0) | Filter assets by class with pagination |
| `get_stats` | none | Row counts for all 13 tables + asset class breakdown (top 20) + `skipped_assets` / `skipped_asset_paths` (assets the poison-pill rule dropped from deep indexing — see **Full-Index Resume**) |
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
- `fts_assets` — content=assets, tokenize='porter unicode61', columns: asset_name, asset_class, description, package_path, module_name
- `fts_nodes` — content=nodes, tokenize='porter unicode61', columns: node_name, node_class, node_type

**DB Location:** `Plugins/Monolith/Saved/ProjectIndex.db`

### `asset_class` filter (v0.23.0)

`search` accepts an optional `asset_class`: a bare string (`"Blueprint"`) or an array (`["Blueprint","WidgetBlueprint"]`). Matching is case-insensitive, the list is de-duplicated, and it is capped at 32 entries. Absent means unfiltered. Responses echo the applied filter as `asset_class_filter`.

**The predicate is applied inside the SQL**, as `AND a.asset_class COLLATE NOCASE IN (?,?,...)`, not as a pass over the returned array. That distinction is the whole point: **`limit` counts *matching* rows** rather than returning the top N of everything and then culling. Searching a common token in a project with a large third-party content folder previously buried the target — a real `E_` search returned 16 `UserDefinedEnum` rows underneath 21 `Texture2D`, 10 `NiagaraEmitter` and 3 `StaticMesh`.

Both FTS statements already `JOIN assets`, so a **graph-node text hit inside a Blueprint still survives a `Blueprint` filter** — node hits are filtered by their owning asset's class, which is the only class a node has.

**Injection surface.** Only the placeholder count is interpolated into the statement; every class name stays a bound parameter.

**Validation.** `asset_class` is type-checked against `EJson` rather than read through `TryGetStringField`, because that coerces: a numeric `7` would otherwise become a filter for a class of that name — zero results dressed up as a valid search — instead of a `-32602`. Whitespace-only input and over-cap lists are likewise `-32602`. A class no asset uses is a **successful empty result**, not an error.

**Offline parity.** Mirrored into `monolith_query.exe` and `monolith_offline.py`; both accept `--asset-class A,B` and the Python tool also accepts a repeated flag. Note that `Scripts/verify_offline_parity.py` has **no `project` cases**, so this parity is maintained by hand — see `Docs/MISSING_FEATURES.md`.

**Coverage.** `Monolith.ProjectSearch.AssetClassFilter` uses a lopsided 10-noise/3-wanted fixture that a post-hoc filter cannot pass by luck; `Monolith.ProjectSearch.AssetClassValidation` covers the rejection cases.

### Search Error Contract

`FMonolithIndexDatabase::FullTextSearch` has a 4-argument overload returning
`EMonolithProjectSearchStatus` (`Succeeded` / `InvalidQuery` / `InternalError`); the
2-argument overload delegates to it and logs any failure. `OutResults` is empty for
every non-`Succeeded` outcome. `project search` maps the status to a JSON-RPC code:

| Status | Code | Cause |
|--------|------|-------|
| `Succeeded` | — | Completed, **including the valid zero-result case** |
| `InvalidQuery` | `-32602` | Malformed FTS5 syntax, or a column no FTS table exposes |
| `InternalError` | `-32603` | Prepare/bind failure, a closed database, corruption |

Classification uses the SQLite diagnostic, not a hand-rolled grammar: the query is
bound into `MATCH ?` and SQLite's own bounded parser is the authority.
`MonolithProjectSearchDetail::IsFts5QuerySyntaxError` matches the five messages the
MATCH expression parser emits (`fts5: syntax error`, `unterminated string`,
`malformed MATCH`, `unknown special query`, `expected integer, got`).

Before this contract existed, `while (Step() == Row)` treated a query error as
end-of-results, so every failure surfaced as a successful empty result set.

#### Per-table applicability — deliberate, not incidental

`search` runs the same query against **two** FTS tables that carry **different**
columns, so a column filter valid for one is `no such column` for the other. The
rule:

1. A table reporting `no such column:` is treated as **not applicable** to this
   query and **skipped**. Its verdict alone is not a failure.
2. The status is `InvalidQuery` **only when both tables reject the query** that way.
3. Any other step failure is classified immediately by the table that hit it —
   applicability is a special case for unknown columns, not a general "keep going".

This is what makes `node_name:Branch` and `asset_name:BP_Enemy` valid searches:
each is answerable by exactly one table, and the other one's refusal is expected.
A filter that spans both (`asset_name:X OR node_name:Y`) is answerable by neither,
so it is correctly refused — and `MonolithProjectSearchDetail::DescribeUnknownColumn`
names the offending column and lists the valid ones, because a typo is the common
case.

> **Do not "tidy" this into a fail-on-first-error loop.** Column-qualified search
> worked before this contract only *by accident*: the asset query errored, the old
> `while (Step() == Row)` loop silently swallowed that as end-of-results, and the
> node query then answered. Classifying every per-table error as a hard failure —
> the obvious reading of "stop errors masquerading as zero results" — turns a
> working feature into a `-32602`. PR #113 avoided this only because it filtered
> column names per-table *before* reaching SQL, in a 1449-line hand-written FTS5
> grammar parser that was rejected on separate grounds (unguarded recursive AST
> teardown; editor death at roughly 32 KB of chained terms, on the game thread).
> Letting SQLite report applicability gives the same per-table filtering with no
> grammar of our own. `Monolith.ProjectSearch.ColumnQualified` locks this.

**Validation** happens in `ProjectSearchAction` before the database is touched:
`query` must be a string, is trimmed, must be non-empty, and is capped at
`MonolithProjectSearchActionDetail::MaxQueryLength` (4096); `limit` must be an
integer and is clamped to 1-1000. All four failures are `-32602`. The cap bounds
the parse work one MCP call can hand the game thread — `FullTextSearch` runs there.

The offline `monolith_query.exe` and `monolith_offline.py` mirror this
classification and these limits. Note `verify_offline_parity.py` has **no
`project.*` cases**, so nothing gates that mirroring — keep the three in step by
hand when editing any of them.

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

An index run owns the database exclusively, so the callbacks are detached for its duration and re-armed in `OnIndexingFinished` — on **every** outcome, success, failure or cancel. Re-arming only on success (the pre-0.22.0 behaviour) left the subsystem reporting itself active while silently dropping every subsequent asset change until a successful reindex or an editor restart. `UnregisterLiveCallbacks` resets the four `FDelegateHandle`s, and `RegisterLiveCallbacks` is a no-op while indexing, while the database is closed, or when any handle is already bound — so a double register cannot leave a first copy permanently attached.

**Layer 3 — Forced Full Reindex (fallback)**

`monolith_reindex()` defaults to incremental mode (Layer 1 logic). Passing `force=true` triggers a full wipe-and-rebuild: drops all table data, re-enumerates, and re-indexes every asset. Used when the DB is suspected corrupt or after schema migrations.

**Reindex result contract (0.22.0)**

`StartFullIndex()` and `StartIncrementalIndex()` return `bool`: `true` only when a run actually started. They decline (and return `false`) when a run is already in flight or the database is not open, and `StartFullIndex` also declines when the worker thread cannot be created — a case that previously latched `bIsIndexing` for the rest of the session, so every later request was refused with "Indexing already in progress" until the editor restarted. `CanAcceptIndexRequest()` exposes the same precondition (`!bIsIndexing && Database.IsValid() && Database->IsOpen()`) and drives the enabled state of the Project Settings re-index button.

Both are `UFUNCTION`s: MonolithCore reaches this subsystem only through reflection, so `monolith.reindex` checks the return property is an `FBoolProperty` before reading it and falls back to the old unconditional `reindex_started` response if a future build drops the bool. It reports `reindex_not_started` when the subsystem declines.

**Background task shutdown invariant**

`FIndexingTask::Run` hands progress and completion back to the game thread with fire-and-forget `AsyncTask` calls. `Deinitialize` does `Stop()` → `WaitForCompletion()` → `IndexingTaskPtr.Reset()`, and **joining the worker does not drain the game thread's task queue** — an already-queued lambda still runs after the task object is gone. Every such lambda therefore captures a `TWeakObjectPtr<UMonolithIndexSubsystem>` and null-checks it, never `this`, and any counter it reports is copied into a local **before** the lambda is constructed rather than loaded through the task inside it. This applies to all four sites: the DB-open failure exit, both per-batch `OnProgress` broadcasts, and the final completion. The progress broadcasts fire once per batch, so they are the likeliest to be in flight when the editor closes mid-index. The Asset Registry enumeration hop is exempt — the worker blocks on its event, so the task is guaranteed alive.

**Schema v2 Migration**

Schema v2 adds:
- `saved_hash TEXT` column on the `assets` table (stores Blake3 `FIoHash` as hex string)
- `schema_version` key in the `meta` table
- Index on `saved_hash` for fast lookup

Migration is automatic: on startup, `PRAGMA table_info(assets)` checks for the `saved_hash` column. If missing, `ALTER TABLE assets ADD COLUMN saved_hash TEXT` runs followed by index creation.

**Schema v3 Migration**

Schema v3 adds two columns to `assets` — **the table count is unchanged at 13**:
- `deep_indexed_hash TEXT DEFAULT ''` — the value `saved_hash` held when the asset last *completed* deep indexing. Written in the same transaction as the child rows it vouches for.
- `deep_index_attempts INTEGER DEFAULT 0` — how many full-index runs began deep indexing this asset without finishing.

Migration is automatic and additive, using the same `PRAGMA table_info(assets)` probe as v2. The v3 block runs unconditionally after the v2 block (a fresh database passes through v2 first, which stamps `"2"`). A **failed** migration does not close the database: the version stays at 2, `project_query` keeps answering for the session, and the resume path degrades to a full reset. `ResetDatabase()` drops `meta`, so it restates `schema_version = 3` before returning.

**Downgrade is safe.** A v3 database opened by v0.21.3 reads `schema_version = "3"`, skips the only migration it knows (`< 2`), and ignores the new columns — every `assets` statement uses an explicit column list and there is no `SELECT *`. The one caveat: an older build updates `saved_hash` without maintaining `deep_indexed_hash`, so a stale checkpoint can survive a downgrade/upgrade round trip. That is why the queue filter's **only** "already done" gate is `deep_indexed_hash == the hash the Asset Registry reports now` — a stale checkpoint on a changed asset simply re-queues.

### Full-Index Resume (0.22.0, issue #117)

Before 0.22.0, `StartFullIndex()` unconditionally called `ResetDatabase()` and `last_full_index` was written only after the entire run finished. Any interruption — crash, kill, power loss — left no completion marker, so the next launch started another full index, which immediately wiped every batch the previous run had committed. On a large project a single crash cost hours; repeated crashes meant the index never completed at all.

**Lifecycle.** `StartFullIndexInternal(bool bForceReset)` is the shared body of two entry points:

| Entry point | Behaviour |
|---|---|
| `StartFullIndex()` (`UFUNCTION`, and `monolith_reindex force=true`) | **Always wipes.** The explicit start-over request. |
| `ResumeFullIndex()` (startup auto-index, `OnAssetRegistryFilesLoaded`, `Monolith.StartIndex`) | Continues an interrupted run when one is recoverable; otherwise wipes and starts fresh. |

`Monolith.StartIndex` resumes; `Monolith.StartIndex force` wipes.

**Markers.** `BeginFullIndex()` writes `full_index_state = in_progress` and clears `last_full_index` in one transaction, *after* `ResetDatabase()` (which drops `meta`). `CompleteFullIndex(UtcNow)` writes `last_full_index` and clears `full_index_state` in one transaction, so a crash between the two cannot leave a completed index looking interrupted. `IsFullIndexInProgress()` is the sole resume trigger, and a resume additionally requires `schema_version >= 3` — `full_index_state` is a `meta` row and would survive a failed *column* migration, which would otherwise resume onto a schema with nowhere to store checkpoints.

**Checkpointing.** The metadata pass UPSERTs (`GetAssetByPath` → `UpdateAssetMetadata` reusing the id, else `InsertAsset`) instead of blind-inserting, so a resume does not fight the `package_path` UNIQUE constraint. Each successfully deep-indexed asset writes `deep_indexed_hash` **inside the transaction that carries its child rows** — the two are atomic, so a rollback can never leave a checkpoint pointing at data that is not there. On a resume, an asset whose checkpoint equals its current hash is not re-queued; one whose hash changed has its child rows cleared and is re-queued.

**Poison pill.** A resume re-queues the asset that killed the editor, so without attempt counting a crash becomes a crash-every-launch loop — strictly worse than the bug being fixed. Before each deep batch, `BumpDeepIndexAttempts` increments the batch's counters in **its own transaction, committed before the batch work transaction opens**. This ordering is the whole mechanism: a write inside an open transaction is not durable, and under `journal_mode=DELETE` a process death mid-batch rolls that transaction back on next open — taking the marker with it, so the counter would read 0 and the pill could never fire in the exact case it exists for. The frame-budget commit inside the batch does not help either: it only fires after at least one asset has been processed, so it never covers a batch's *first* asset, which is where a resumed queue puts the poison one. The cost is one extra commit **per batch** (not per asset).

At two interrupted attempts an asset is dropped from the deep queue and stamped with its current hash so it leaves permanently. Its attempt counter is deliberately **not** cleared, so it is held out on both gates — an asset whose Asset Registry hash is empty would otherwise stamp an empty checkpoint, fail the hash comparison on the next run, re-enter the queue and resume crashing. Because the marker is batch-granular (`GetResolvedDeepIndexBatchSize()` = 8/4/2 by RAM tier), **one poison asset takes its whole batch out of deep indexing** — a data-completeness loss, so it is never silent: every skipped path is logged at **Error**, persisted to the `full_index_skipped_assets` meta row, and counted in `project get_stats` as `skipped_assets`. Recovery is `Monolith.StartIndex force` (or `monolith_reindex force=true`), which wipes and clears the counters.

**Completion gate.** `bStructurallyComplete = !bShouldStop && !bTransactionFailure` — cancellation and transaction/DB failures only. Per-asset failures (`Errors` / `DeepErrors`) are **not** gates: they fire routinely on real projects (a class from a disabled plugin, a redirector stub, an animation asset that crashes on load — the animation indexer's SEH guard exists because those are expected), and a fail-closed gate would mean one bad asset re-runs a full wipe-and-rebuild on every launch, which is #117's symptom reached from the opposite direction. The old `Indexed < 500` guard is gone for the same reason: it made every project with fewer than 500 assets re-index from scratch on every launch.

**`bDeferFirstTimeIndex` is honoured on resume.** That flag is the documented escape hatch for the UE 5.7 incremental-GC worker-context crash class, which is exactly the crash class an interrupted index is evidence of. Bypassing it on the resume path would override the user's escape hatch at the moment they need it, leaving them to delete `ProjectIndex.db` by hand. The startup path logs that an interrupted index is pending and waits for `Monolith.StartIndex`; checkpoints are durable, so waiting costs nothing.

**Limits — what resume does NOT cover.**
- **Post-pass sentinels always re-run from the start.** `FAnimationIndexer` operates on the `__Animations__` sentinel rather than on `assets` rows with ids, so `deep_indexed_hash` / `deep_index_attempts` do not apply to it; the same holds for the dependency, level, DataTable, config, C++, gameplay-tag and Niagara passes. A resume therefore repeats whichever post-pass work the interrupted run had done, and those indexers append rather than replace — so an interruption **during the post-pass phase** (as opposed to the far longer deep pass, where crashes normally happen) leaves duplicate `dependencies` / `actors` / `configs` / `cpp_symbols` / `tag_references` rows and duplicate animation nodes. `monolith_reindex force=true` clears them. Per-indexer post-pass checkpointing is tracked in `MISSING_FEATURES.md`.
- The animation pass gets **per-batch transactions** (bounding an interruption's loss to one batch instead of the whole pass) but no checkpoint and no poison protection; its SEH guard already isolates per-asset crashes.
- `monolith_reindex()` with no arguments on an interrupted index still takes the full-reset path (`CanDoIncrementalIndex()` requires `last_full_index`, which an interrupted run does not have). Use `Monolith.StartIndex` to resume.

### Editor-exit shutdown (v0.23.0)

**Quitting the editor while a project index was running hung the process forever.** The indexing worker parked in an unbounded `FEvent::Wait()` at **fourteen** points, every one of them waiting on work that only the game thread can run — while `Deinitialize()` blocked the game thread joining that same worker. Neither side could move.

**The fix, and its shape.** Those waits now poll in 100 ms slices and abandon **only when the game thread has provably stopped serving them** — a shutdown join or engine exit. The provably-stopped condition is what keeps this from being a timeout: a slow game thread is not a stopped one, so a legitimately long dispatch still completes. A force-stopped index now exits in well under a second. **A user-initiated cancel is unaffected and still runs to a clean finish** — cancel and shutdown are different paths, and only the latter abandons work.

Two related corrections landed with it:

- **An abandoned game-thread dispatch is withdrawn through an abort token before the worker unwinds**, so its payload can never run against a retired stack frame. The asset-registry scan now owns its state outright instead of borrowing that frame.
- **`Deinitialize` completes the progress notification before destroying it**, so interrupting an index by closing the editor no longer trips `"AsyncTaskNotification was still pending when destroyed"`. That assert had been hiding behind the deadlock — the process never got far enough to fire it.

> **Note for future work in this file:** PR #123 (index memory-pressure hardening) overlaps this change and is deferred pending a combined design pass. See `Docs/MISSING_FEATURES.md`.

**IMonolithIndexer Interface Additions**

| Method | Purpose |
|--------|---------|
| `IsSentinel()` | Returns true if this indexer acts as a sentinel for a specific asset type (used by incremental path to decide which indexers to invoke) |
| `SupportsIncrementalIndex()` | Returns true if the indexer can process individual asset changes without a full rebuild |
| `IndexScoped()` | Index a specific set of assets (subset of full index). Default implementation falls back to `IndexAsset()` per asset |

**Plugin Content Scope Fix**

The `bInstalled` filter on plugin content paths was replaced with explicit path enumeration. This fixes discovery of project-local plugins (e.g., DrawCallReducer, NiagaraDestructionDriver) that previously reported `bInstalled=false` and were excluded from indexing. The `MeshCatalogIndexer` paths were also corrected to use the new enumeration.

---
