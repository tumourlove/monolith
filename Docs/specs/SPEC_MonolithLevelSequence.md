# Monolith — MonolithLevelSequence Module

**Parent:** [SPEC_CORE.md](../SPEC_CORE.md)
**Engine:** Unreal Engine 5.7+
**Version:** 0.25.0 (Beta)

---

## MonolithLevelSequence

**Dependencies:** Core, CoreUObject, Engine, MonolithCore, MonolithIndex, SQLiteCore, UnrealEd, AssetRegistry, MovieScene, MovieSceneTracks, LevelSequence, BlueprintGraph, Kismet, EditorSubsystem, Json, JsonUtilities; optional LevelSequenceEditor (`WITH_LEVELSEQUENCEEDITOR`)
**Namespace:** `level_sequence` | **Tool:** `level_sequence_query(action, params)` | **Actions:** 21 (8 index-backed including `ping`, 13 live-asset)
**Settings toggles:** `bEnableLevelSequence` (registers actions, default True) and `bIndexLevelSequences` (registers indexer, default True) — both under `[/Script/MonolithCore.MonolithSettings]`

MonolithLevelSequence reads and edits a sequence's contents (tracks, sections, keys, marked frames, ranges, retime) from the asset itself, and introspects `ULevelSequence` assets through the index: their **bindings** (legacy Possessables / Spawnables and the UE 5.7 `UMovieSceneCustomBinding` family — `MovieSceneSpawnableActorBinding`, `MovieSceneReplaceableActorBinding`, etc.), their **event-track wiring** (which sections fire which Director functions), and their **Director Blueprint** when one is present (own functions, variables, synthetic Sequencer entrypoints). It complements `MonolithBlueprint` (which already covers Director Blueprints as ordinary `UBlueprint` assets via `subobject:` paths) by adding the Sequencer-specific binding and event context.

### Action Categories

The 8 index-backed actions live in `Source/MonolithLevelSequence/Private/MonolithLevelSequenceActions.cpp`; the 13 live-asset actions in `MonolithLevelSequenceContentActions.cpp` and `MonolithLevelSequenceRetime.cpp` (see [Live-asset contents and edits](#live-asset-contents-and-edits)). Counts below are the literal registrations.

| Category | Actions | Description |
|----------|---------|-------------|
| Smoke | 1 | `ping` — module liveness check, returns `{status:"ok", module:"MonolithLevelSequence"}` |
| Bindings | 1 | `list_bindings` (every binding inside one LS regardless of event tracks; reports kind, bound class, exact `UMovieSceneCustomBinding` subclass when present, and a `kind_counts` breakdown — works for sequences with no Director) |
| Director discover | 2 | `list_directors` (all LSes with a Director, optional `asset_path_filter` glob), `get_director_info` (one-LS summary: counts grouped by kind, event-binding totals, sample functions) |
| Director inspect | 3 | `list_director_functions` (with `kind` filter: `user` / `custom_event` / `sequencer_endpoint` / `event` alias / `all`), `list_director_variables` (name + K2-formatted type, declaration order), `list_event_bindings` (event-tracks grouped by binding GUID, each with sections + resolved Director function) |
| Reverse-lookup | 1 | `find_director_function_callers` — given a function name, every event-track section across the project that fires it (with binding context) |
| Contents (live) | 2 | `get_sequence_contents`, `refresh_current_sequence` |
| Time (live) | 1 | `retime` |
| Tracks / sections (live) | 5 | `add_track`, `remove_track`, `add_section`, `set_section`, `remove_section` |
| Keys (live) | 3 | `set_keys`, `remove_keys`, `set_channel_default` |
| Sequence (live) | 2 | `set_marked_frames`, `set_playback_range` |

**Total:** **21** registered: 8 index-backed (`ping` + `list_bindings` + `list_directors` + `get_director_info` + `list_director_functions` + `list_director_variables` + `list_event_bindings` + `find_director_function_callers`) and 13 live-asset.

### Live-asset contents and edits

These actions load the sequence (`FMonolithAssetUtils::LoadAssetByPath<UMovieSceneSequence>`) instead of reading the index, so they always see the current asset, saved or not. Shared plumbing is in `MonolithLevelSequenceContentUtils.{h,cpp}`.

- **Time.** Every frame a caller sends or reads is a display-rate frame, as in UE's Sequencer scripting default (`EMovieSceneTimeUnit::DisplayRate`) and UE 5.8's AnimationAssistantToolset. Keys and marked frames also take a `sub_frame` in [0, 1); section ranges are whole frames (end exclusive). Data is stored in ticks; responses carry `display_rate`, `fps`, `tick_resolution` and `ticks_per_frame` (when whole). View and working ranges are stored in seconds and exposed in frames.
- **Targeting.** `binding` = GUID (any `FGuid::Parse` format) or display name, matched case-insensitively against the Sequencer label and the possessable / spawnable name; an ambiguous name is refused with the candidate GUIDs. No binding = the sequence scope: the camera cut track then `UMovieScene::GetTracks()`. A track is `track_class` (exact `UClass`, `U` prefix optional) + `track_index` among the tracks of that class in the scope; a section is `section_index` in `GetAllSections()`; a channel is its `FMovieSceneChannelMetaData::Name` (`Location.X`, `Weight`) or `#index` in channel-proxy order.
- **Bindings in the dump.** Classified by `MonolithLevelSequence::ClassifyBinding` (`MonolithLevelSequenceBindingUtils.{h,cpp}`, shared with the indexer). Child possessables (`FMovieScenePossessable::GetParent`) are nested under their parent. The template is the legacy spawnable's `GetObjectTemplate()` or the 5.7 `UMovieSceneSpawnableBindingBase::GetObjectTemplate()`; when it is an actor with a `USkeletalMeshComponent`, its mesh is reported.
- **Writes** parse and check every param before changing anything, run in an `FScopedTransaction` with `Modify()` on the movie scene / track / section, and save through `FMonolithAssetUtils::SaveAssetPackage` unless `save: false`. A sequence locked in Sequencer (`UMovieScene::IsReadOnly`) is refused. Writes don't notify an open Sequencer (like UE 5.8's toolset, which has a separate `refresh_sequence`): call `refresh_current_sequence`, which calls `ULevelSequenceEditorBlueprintLibrary::RefreshCurrentLevelSequence`. Writes don't reindex: the index-backed actions catch up on the next index pass.
- **Sections.** `add_section` creates the section with `UMovieSceneTrack::CreateNewSection()` (what `UMovieSceneTrackExtensions::AddSection` does), checks the typed fields against its real class, then adds it. `end` defaults to the animation length (minus offsets, divided by play rate) or the subsequence's playback length. `set_section` `move_by` calls `UMovieSceneSection::MoveSection`, which moves the keys too; `start` / `end` call `SetRange` and leave keys in place. `ease_in` / `ease_out` set manual easing (`FMovieSceneEasingSettings`), `"auto"` goes back to automatic; `ease_*_type` sets a `UMovieSceneBuiltInEasingFunction` type. Skeletal animation offsets are in frames (stored as tick `FFrameNumber`s); `play_rate` sets a fixed `FMovieSceneTimeWarpVariant`.
- **Keys.** `set_keys` updates the key at the exact tick or adds one (new float/double keys: cubic, auto tangent), then calls `AutoSetTangents`. Interpolation and tangents (raw stored values) apply to float and double channels. Byte channels with an enum accept enumerator names. `remove_keys` works on any channel type through the `FMovieSceneChannel` interface.
- **`retime`** walks every section of every track (sequence scope, then every binding) and works on any channel type through `FMovieSceneChannel::GetKeys` / `SetKeyTimes` / `DeleteKeys`. With F = `at` and N = `frames`: keys at F or later move by N; when removing, a key exactly at F+N is deleted if the channel has a key exactly at F, and other keys in [F+N, F) are reported and left. A section starting at F or later moves by N (range only, its keys follow the key rule); a section spanning F keeps its start and its end moves by N, or, for `move_whole_classes`, the whole section moves with all its keys; a section that would end before its start, or that ends inside the removed range, is reported and left. Marked frames at F or later move by N. The playback end and the view and working range ends move by N. `dry_run` (default true) runs the same walk without changing anything.

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
> **Path format.** The index-backed actions that take an `asset_path` expect the full object path (e.g. `/Module/.../File.File`), matching `ULevelSequence::GetPathName()`. A typo or missing path returns a clear error with a hint. The live-asset actions also accept the package path (`/Game/.../File`).
>
> **`list_bindings` vs `list_event_bindings`.** `list_bindings` returns the full binding inventory of one LS (every Guid×BindingIndex), useful for understanding scene composition and spotting modern custom bindings. `list_event_bindings` filters to bindings that have event-track sections and joins them to the Director functions they fire — useful for tracing event-driven logic.

---
