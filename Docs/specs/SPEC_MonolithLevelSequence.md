# Monolith — MonolithLevelSequence Module

**Parent:** [SPEC_CORE.md](../SPEC_CORE.md)
**Engine:** Unreal Engine 5.7+
**Version:** 0.14.7 (Beta)

---

## MonolithLevelSequence

**Dependencies:** Core, CoreUObject, Engine, MonolithCore, MonolithIndex, SQLiteCore, UnrealEd, MovieScene, MovieSceneTracks, LevelSequence, BlueprintGraph, Kismet, EditorSubsystem, Json, JsonUtilities
**Namespace:** `level_sequence` | **Tool:** `level_sequence_query(action, params)` | **Actions:** 8 (7 production + `ping` smoke)
**Settings toggles:** `bEnableLevelSequence` (registers actions, default True) and `bIndexLevelSequences` (registers indexer, default True) — both under `[/Script/MonolithCore.MonolithSettings]`

MonolithLevelSequence introspects `ULevelSequence` assets end-to-end: their **bindings** (legacy Possessables / Spawnables and the UE 5.7 `UMovieSceneCustomBinding` family — `MovieSceneSpawnableActorBinding`, `MovieSceneReplaceableActorBinding`, etc.), their **event-track wiring** (which sections fire which Director functions), and their **Director Blueprint** when one is present (own functions, variables, synthetic Sequencer entrypoints). It complements `MonolithBlueprint` (which already covers Director Blueprints as ordinary `UBlueprint` assets via `subobject:` paths) by adding the Sequencer-specific binding and event context.

### Action Categories

All 7 actions live in `Source/MonolithLevelSequence/Private/MonolithLevelSequenceActions.cpp`. Counts below are the literal registrations.

| Category | Actions | Description |
|----------|---------|-------------|
| Smoke | 1 | `ping` — module liveness check, returns `{status:"ok", module:"MonolithLevelSequence"}` |
| Bindings | 1 | `list_bindings` (every binding inside one LS regardless of event tracks; reports kind, bound class, exact `UMovieSceneCustomBinding` subclass when present, and a `kind_counts` breakdown — works for sequences with no Director) |
| Director discover | 2 | `list_directors` (all LSes with a Director, optional `asset_path_filter` glob), `get_director_info` (one-LS summary: counts grouped by kind, event-binding totals, sample functions) |
| Director inspect | 3 | `list_director_functions` (with `kind` filter: `user` / `custom_event` / `sequencer_endpoint` / `event` alias / `all`), `list_director_variables` (name + K2-formatted type, declaration order), `list_event_bindings` (event-tracks grouped by binding GUID, each with sections + resolved Director function) |
| Reverse-lookup | 1 | `find_director_function_callers` — given a function name, every event-track section across the project that fires it (with binding context) |

**Total:** **8** registered (`ping` + `list_bindings` + `list_directors` + `get_director_info` + `list_director_functions` + `list_director_variables` + `list_event_bindings` + `find_director_function_callers`).

### Indexer

`FLevelSequenceIndexer` registers itself into `UMonolithIndexSubsystem` on `OnPostEngineInit` (when `bIndexLevelSequences` is true). Implements `IMonolithIndexer::IndexAsset` for `LevelSequence` assets and writes 5 tables:

| Table | Rows per | Purpose |
|-------|---------|---------|
| `level_sequence_bindings` | one per `(Guid, BindingIndex)` in the LS | Full per-LS binding inventory: name, kind (`possessable` / `spawnable` / `replaceable` / `custom`), bound class, `custom_binding_class` (e.g. `MovieSceneSpawnableActorBinding`), `custom_binding_pretty`, track count. Recorded for every binding, not just event-bound ones; runs before the Director early-return so cinematics with no Director still get indexed |
| `level_sequence_directors` | one per LS with a Director BP | Director name + total function/variable counts; LSes with no Director are skipped at this table only |
| `level_sequence_director_functions` | one per Director's own function | `kind` ∈ {`user`, `custom_event`, `sequencer_endpoint`}; signature stored as JSON array of `{name, type}` |
| `level_sequence_director_variables` | one per Director's own variable | Name + K2-schema-formatted type |
| `level_sequence_event_bindings` | one per `FMovieSceneEvent` (trigger entry or repeater) | Binding GUID + name + kind (now propagated through the same custom-binding classifier as `level_sequence_bindings`) + bound class + section kind + the Director function it fires (`fires_function_id` resolved via per-asset SQL UPDATE post-pass) |

### Binding classification

UE 5.7 introduced `UMovieSceneCustomBinding` on `UMovieSceneSequence::GetBindingReferences()`, separate from the legacy `FMovieScenePossessable` / `FMovieSceneSpawnable` structs in `UMovieScene`. The migration path (`ULevelSequence::ConvertOldSpawnables`) registers modern Spawnables AS Possessables inside `UMovieScene` so their tracks survive, while attaching the real spawnable identity to `BindingReferences`. The indexer's `ClassifyBinding` consults both sources:

| `kind` | Trigger |
|--------|---------|
| `spawnable` | Legacy `FMovieSceneSpawnable` OR custom `UMovieSceneSpawnableBindingBase` subclass |
| `replaceable` | Custom `UMovieSceneReplaceableBindingBase` subclass |
| `custom` | Any other `UMovieSceneCustomBinding` subclass |
| `possessable` | Plain possessable, no custom binding |

When a `UMovieSceneCustomBinding` is present, `bound_class` is taken from `GetBoundObjectClass()` (more accurate than the upgrade-stub possessable class), and `custom_binding_class` / `custom_binding_pretty` carry the exact UCLASS name and editor label from `GetBindingTypePrettyName()`.

### Conventions notes

- **Own-functions only.** Inherited base-class methods (e.g. `OnCreated`, `GetBoundActor`) and compiler-generated `ExecuteUbergraph*` dispatchers are deliberately NOT indexed — same convention as `blueprint_query.get_functions`.
- **Synthetic Sequencer endpoints ARE indexed.** When the user clicks "Quick Bind" / "Create New Endpoint" in Sequencer, UE compiles a `SequenceEvent__ENTRYPOINT<DirBP>_N` UFunction with no graph node. We capture these via `TFieldIterator<UFunction>(GenClass)` after walking the graphs — they are the actual runtime targets of `FMovieSceneEvent::Ptrs::Function`, so `event_bindings.fires_function_id` would never resolve without them.
- **No FK on `assets(id)`.** All tables that reference `ls_asset_id` intentionally have no FK to the core `assets(id)` — `FMonolithIndexDatabase::ResetDatabase()` (called by `force=true` reindex) wipes built-in tables without knowing about our custom ones, and a FK would block the `DELETE FROM assets`. Pattern borrowed from `MonolithAI`'s `ai_assets`.
- **Cleanup keyed by `ls_path`.** `IndexAsset` looks up the previous director row by `ls_path` (stable across reindex) and also captures its previous `ls_asset_id` to wipe orphan rows in `level_sequence_event_bindings` and `level_sequence_bindings` — necessary because the core `assets` autoincrement restarts on full reindex.

### Notes

> **Glob filters.** Both `list_directors` and `find_director_function_callers` accept an `asset_path_filter` parameter; `*` and `?` are converted to SQL `LIKE` `%` and `_` before the query.
>
> **Path format.** All actions that take an `asset_path` expect the full object path (e.g. `/Module/.../File.File`), matching `ULevelSequence::GetPathName()`. A typo or missing path returns a clear error with a hint.
>
> **`list_bindings` vs `list_event_bindings`.** `list_bindings` returns the full binding inventory of one LS (every Guid×BindingIndex), useful for understanding scene composition and spotting modern custom bindings. `list_event_bindings` filters to bindings that have event-track sections and joins them to the Director functions they fire — useful for tracing event-driven logic.

---
