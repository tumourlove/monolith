---
name: unreal-level-sequences
description: Use when reading or editing Unreal Level Sequences via Monolith MCP — dumping a sequence's tracks, sections and keys, inserting or removing time (retime), adding or retiming animation sections, keying transforms, blending animations with easing, fixing camera cuts, marked frames; listing every binding inside a sequence (legacy Possessables/Spawnables and UE 5.7 UMovieSceneCustomBinding subclasses like MovieSceneSpawnableActorBinding), reading Director Blueprint functions and variables when a Director is present, walking event-track bindings, and reverse-looking-up which sections fire a given director function across the project. Triggers on level sequence, sequencer, cinematic, retime, keyframe, camera cut, animation section, possessable, spawnable, custom binding, MovieSceneSpawnableActorBinding, BindingReferences, director blueprint, event track, FMovieSceneEvent.
---

# Unreal Level Sequence Workflows

Via `level_sequence_query()`. Discover with `monolith_discover({ namespace: "level_sequence" })`. Two families:

- **Index-backed inspection** (bindings, Director, event wiring): the tables below.
- **Live-asset contents and edits** (tracks, sections, keys, retime, markers, ranges): see [Reading and editing a sequence](#reading-and-editing-a-sequence).

Indexes **every** Level Sequence — including those with no Director. Each binding is classified by inspecting both the legacy `UMovieScene` structures (`FMovieScenePossessable` / `FMovieSceneSpawnable`) and the UE 5.7 `UMovieSceneSequence::GetBindingReferences()` chain, so modern custom bindings (`MovieSceneSpawnableActorBinding`, `MovieSceneReplaceableActorBinding`, etc.) report as `spawnable` / `replaceable` rather than the upgrade-stub `possessable`. Director Blueprint introspection layers on top: own functions (including synthetic `SequenceEvent__ENTRYPOINT<DirBP>_N` UFunctions UE generates for "Quick Bind" event entries), variables, and event-track wiring.

## Key Parameters

- `asset_path` — full Level Sequence object path (e.g. `/Game/Cinematics/LS_Intro.LS_Intro`); matches `ULevelSequence::GetPathName()`
- `asset_path_filter` — optional glob (`*` and `?`) for `list_directors` and `find_director_function_callers`; converted to SQL `LIKE`
- `kind` (on `list_director_functions`) — `user` / `custom_event` / `sequencer_endpoint` / `event` (alias for the latter two) / `all`
- `kind` (on `list_bindings`) — `possessable` / `spawnable` / `replaceable` / `custom` / `all`
- `function_name` — exact, case-sensitive name for `find_director_function_callers`

## Action Reference

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `list_bindings` | `asset_path`, `kind`? | Every binding inside one LS (one row per `Guid×BindingIndex`), regardless of event-track presence. Each row carries `name`, `kind`, `bound_class`, `custom_binding_class` (e.g. `MovieSceneSpawnableActorBinding`) and `custom_binding_pretty` when a `UMovieSceneCustomBinding` is attached, plus `track_count` and a `kind_counts` breakdown |
| `list_directors` | `asset_path_filter`? | All LSes with a Director BP, ordered by path. Each row: `ls_path`, `director_bp_name`, `function_count`, `variable_count` |
| `get_director_info` | `asset_path` | One Director's summary: `function_breakdown` grouped by kind, `variable_count`, `event_bindings.{total,resolved}`, sample of up to 10 functions ordered user → custom_event → sequencer_endpoint |
| `list_director_functions` | `asset_path`, `kind`? | Director's own functions with parsed signatures. Inherited base-class methods and compiler-generated `ExecuteUbergraph*` are not indexed (own-only, matches `blueprint_query` convention) |
| `list_director_variables` | `asset_path` | Director's own variables (name + K2-formatted type) in declaration order |
| `list_event_bindings` | `asset_path` | All event-track entries grouped by binding GUID. Each binding shows kind (now correctly `spawnable` / `replaceable` for UE 5.7 custom bindings), bound class, and an array of sections (`trigger` / `repeater`) with the Director function each fires (resolved name + kind + signature when matched) |
| `find_director_function_callers` | `function_name`, `asset_path_filter`? | Cross-sequence reverse lookup. Returns every event-track section across the project that fires the named function, with LS path and binding context (binding GUID/name/kind/bound class, section kind, resolved bool) |

## Common Workflows

### "What's inside this Level Sequence?"
```
level_sequence_query({ action: "list_bindings", params: { asset_path: "/Game/Cinematics/LS_Intro.LS_Intro" } })
```
Spot modern UE 5.7 spawnables by their `custom_binding_class` (`MovieSceneSpawnableActorBinding` etc.) and the `kind_counts` breakdown.

### "Just the spawnables, please"
```
level_sequence_query({ action: "list_bindings", params: { asset_path: "/Game/Cinematics/LS_Intro.LS_Intro", kind: "spawnable" } })
```

### "Which sequences in module X have a Director?"
```
level_sequence_query({ action: "list_directors", params: { asset_path_filter: "/Game/Cinematics/*" } })
```

### "What does this LS Director do?"
```
level_sequence_query({ action: "get_director_info", params: { asset_path: "/Game/Cinematics/LS_Intro.LS_Intro" } })
level_sequence_query({ action: "list_director_functions", params: { asset_path: "/Game/Cinematics/LS_Intro.LS_Intro", kind: "user" } })
level_sequence_query({ action: "list_director_variables", params: { asset_path: "/Game/Cinematics/LS_Intro.LS_Intro" } })
```

### "Which actor in the level fires which Director function in this LS?"
```
level_sequence_query({ action: "list_event_bindings", params: { asset_path: "/Game/Cinematics/LS_Intro.LS_Intro" } })
```
Returns event-bound bindings grouped by GUID — for each Possessable / Spawnable / master track, see all event-track sections and the Director function each fires.

### "Where else is this Director function called from?"
```
level_sequence_query({ action: "find_director_function_callers", params: { function_name: "OnPlayerEntered" } })
level_sequence_query({ action: "find_director_function_callers", params: { function_name: "SequenceEvent__ENTRYPOINTLS_Intro_DirectorBP_0", asset_path_filter: "/Game/Cinematics/*" } })
```

### Combine with `blueprint_query` for full graph introspection
The Director Blueprint is also a regular `UBlueprint` accessible via `blueprint_query` using `subobject:` syntax:
```
blueprint_query({ action: "get_blueprint_info", params: { asset_path: "/Game/Cinematics/LS_Intro.LS_Intro:LS_Intro_DirectorBP" } })
blueprint_query({ action: "get_graph_summary", params: { asset_path: "...:LS_Intro_DirectorBP", graph_name: "Sequencer Events" } })
```
`level_sequence_query` covers Sequencer-side metadata (bindings, event-track structure); `blueprint_query` covers BP graph-level details.

## Reading and editing a sequence

These actions load the asset (always current, unlike the index). **Frames are display frames** (what Sequencer shows); keys also take an optional `sub_frame` in [0, 1). Every response carries `display_rate` and `tick_resolution`.

| Action | Key params | Purpose |
|--------|-----------|---------|
| `get_sequence_contents` | `asset_path`, `binding?`, `track_class?`, `include_keys?`, `max_keys?` (40, 0 = all) | Rates, playback / view / work ranges, marked frames, sequence tracks, then each binding (kind, template class, skeletal mesh, children nested) with tracks, sections (range, row, active, easing, animation + offsets, camera binding, subsequence) and channel keys (frame, value, interp, tangents) or default |
| `retime` | `at`, `frames` (N > 0 inserts, N < 0 removes the frames before `at`), `move_whole_classes?`, `dry_run?` (default **true**) | Insert or remove time across the whole sequence: keys, sections, marked frames, ranges. Reports every move, deletion and leftover |
| `add_track` / `remove_track` | `binding?`, `track_class`, `track_index?` | Add or remove a track on a binding or on the sequence (camera cut track: one per sequence) |
| `add_section` / `set_section` / `remove_section` | track target + `section_index?`, `start`, `end`, `row`, `active`, `ease_in` / `ease_out` (+ `_type`), typed fields | Typed fields: skeletal animation `animation`, `first_loop_start_offset`, `start_offset`, `end_offset`, `play_rate`, `slot`; camera cut `camera_binding`; subsequence `sequence`. `set_section` `move_by` moves the section **with its keys**; `start` / `end` change the range only |
| `set_keys` / `remove_keys` / `set_channel_default` | section target + `channel` (`Location.X`, `Weight`, or `#index`) | Upsert keys by frame (float, double, bool, integer, byte/enum); remove keys at frames; set or clear the no-key default |
| `set_marked_frames` | `op` add / set / remove / clear | Timeline markers, kept sorted by frame |
| `set_playback_range` | `start?`, `end?`, `view_*?`, `work_*?` | Playback, view and working ranges |
| `refresh_current_sequence` | — | Refresh the open Sequencer after writes; returns the open sequence's path or null |

**Targeting.** `binding` is a GUID (hyphenated or 32 hex digits) or display name (an ambiguous name errors with the candidate GUIDs); omit it for sequence tracks (camera cuts, audio, subsequences). A track is `track_class` (exact class, e.g. `MovieSceneSkeletalAnimationTrack`, `MovieScene3DTransformTrack`) + `track_index` among tracks of that class; a section is `section_index` in its track. A section's only channel, when the engine leaves it unnamed (spawn, visibility), is named after its track. Other section types (audio, Wwise, plugin sections) list what they play under `references`. `get_sequence_contents` prints all of these: read it first.

**Writes** run in a transaction (Ctrl+Z in the editor) and save unless `save: false`. In an open editor where the user reviews in Sequencer, pass `save: false`, then call `refresh_current_sequence`. Writes don't reindex: the index-backed actions catch up on the next index pass.

### Workflows

- **Cut a pause:** `retime` with `at` = the frame after the pause and `frames` = −length; read the dry-run report (keys left in the removed range, sections ending inside it), then rerun with `dry_run: false`. Keep a sound in sync with what follows with `move_whole_classes: ["MovieSceneAudioSection"]` (or the project's audio section class).
- **Blend two animations:** put the incoming section on another `row`, overlapping the outgoing one, and give the overlap as `ease_in` on the incoming and `ease_out` on the outgoing section.
- **Where two motions meet** (e.g. lifted, then walking): an auto tangent carries the first motion's speed into the second. Flatten that key: `set_keys` with `interp: "cubic"`, `tangent_mode: "user"`, `arrive: 0`, `leave: 0`. A hold until the next key is `interp: "constant"`.
- **Rotation keys are Euler** (`Rotation.X` roll, `.Y` pitch, `.Z` yaw) and interpolate per channel: pick the equivalent form that interpolates simply from the previous key.
- **Time a reaction to another character's hit:** sample the attacker's hand bone per frame with `animation_query get_animated_bone_transform`; the impact is where the hand stops right after its speed peak. Animation length is `animation_length` in the dump (frames).

## Rules

- **Path format strict.** Pass the full object path returned by `ULevelSequence::GetPathName()` (e.g. `/Module/.../File.File`), not just `/Module/.../File`. Errors include a hint.
- **`list_bindings` ≠ `list_event_bindings`.** `list_bindings` is the full binding inventory of one LS regardless of event tracks (use it to see scene composition and spot modern custom bindings). `list_event_bindings` filters to event-bound rows and joins them to the Director functions they fire.
- **UE 5.7 binding kinds.** Modern custom bindings classify by their UCLASS hierarchy: `UMovieSceneSpawnableBindingBase` → `spawnable`, `UMovieSceneReplaceableBindingBase` → `replaceable`, anything else inheriting `UMovieSceneCustomBinding` → `custom`. Plain possessables remain `possessable`. The `custom_binding_class` field carries the exact UCLASS name when present.
- **`function_name` matching is exact and case-sensitive.** `Start` will not match `start_event`.
- **Glob conversion.** `*` → `%`, `?` → `_`. Single quotes are escaped automatically.
- **`kind` filter values for functions.** Use `event` as an alias when you want both `custom_event` (graph-defined `K2Node_CustomEvent`) and `sequencer_endpoint` (UE-generated `SequenceEvent__ENTRYPOINT*`). Use the precise values when you need to distinguish.
- **`fires_function_id` may be NULL** even when `fires_function_name` is set — this happens for empty trigger sections (no key-frames yet) and the rare cross-Director call. Check the `resolved` boolean in `find_director_function_callers` output.
- **Function counts include synthetic functions.** `function_count` on a Director includes user functions + graph CustomEvents + Sequencer-generated endpoints. Use `list_director_functions` with `kind` to slice.
- **Inherited base-class API is intentionally not indexed.** `ULevelSequenceDirector::OnCreated`, `GetBoundActor`, `GetCurrentTime`, etc. won't show up — this matches `blueprint_query.get_functions` convention.
