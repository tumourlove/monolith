# Monolith — MonolithAudio Module

**Parent:** [SPEC_CORE.md](../SPEC_CORE.md)
**Engine:** Unreal Engine 5.7+
**Version:** 0.14.1 (Beta)

---

## MonolithAudio

**Dependencies:** Core, CoreUObject, Engine, MonolithCore, AudioMixer, AudioEditor, AssetTools, Json, JsonUtilities, Slate, SlateCore, UnrealEd
**Namespace:** `audio` | **Tool:** `audio_query(action, params)` | **Actions:** 81
**Conditional:** MetaSound features wrapped in `#if WITH_METASOUND`. When MetaSound is absent, MetaSound graph actions return `METASOUND_NOT_AVAILABLE` but all other actions (Sound Cue, CRUD, batch, query) function normally. Build.cs auto-detects MetaSound at `Engine/Plugins/Runtime/Metasound`.
**Settings toggle:** `bEnableAudio` (default: True)

MonolithAudio provides MCP coverage of audio asset creation, inspection, batch management, Sound Cue graph building, and MetaSound graph building. It covers the 5 configurable audio asset types (SoundAttenuation, SoundClass, SoundMix, SoundConcurrency, SoundSubmix), read-only SoundWave inspection, Sound Cue node graph construction, and MetaSound Builder API integration.

**No overlap with Resonance** — Resonance owns runtime footstep/surface/movement audio playback. MonolithAudio owns editor-time asset creation, management, and inspection.

### Action Categories

| Category | Actions | Description |
|----------|---------|-------------|
| Asset CRUD | 15 | Create/get/set triads for SoundAttenuation, SoundClass, SoundMix, SoundConcurrency, SoundSubmix |
| Query & Search | 10 | List/search audio assets, hierarchy inspection, reference queries, stats, audio health checks (missing class, no attenuation, unused) |
| Batch Operations | 10 | Batch assign sound class/attenuation/submix/concurrency/compression/looping/virtualization, batch rename, batch set properties, apply audio template |
| Sound Cue Graph | 21 | Sound Cue CRUD, node add/remove/connect, graph read, node property editing, `build_sound_cue_from_spec` (power action), 5 template cues (random, layered, looping, crossfade, switch), validate, preview, delete |
| MetaSound Graph | 25 | MetaSound Source/Patch creation, node add/remove/connect/disconnect, graph inputs/outputs, interface management, graph read, node discovery, `build_metasound_from_spec` (power action), 4 template MetaSounds (oneshot, ambient, synth, interactive), preset, variables, layout |

### Key Actions

> **`build_sound_cue_from_spec` (power action).** Creates a complete Sound Cue graph from a JSON specification in a single call. The spec defines nodes (with type and properties), connections (from/to with child_index), and the first node. Handles node creation via `ConstructSoundNode`, property setting via reflection, connection wiring via `ChildNodes[]`, `LinkGraphNodesFromSoundNodes()`, and `CacheAggregateValues()`.
>
> **`build_metasound_from_spec` (power action).** Creates a complete MetaSound from a JSON specification in a single call. The spec defines type (Source/Patch), format, interfaces, graph inputs/outputs, nodes, connections, and interface wiring. Uses `UMetaSoundBuilderSubsystem::CreateSourceBuilder()`, `AddNodeByClassName()`, `ConnectNodes()`, and `BuildToAsset()`.
>
> **`apply_audio_template`.** Applies a combined settings template (sound class, attenuation, compression, submix, concurrency, looping, virtualization) to multiple assets in one call. The most efficient way to standardize audio pipeline configuration.
>
> **Template cues and MetaSounds.** Pre-built audio patterns: `create_random_sound_cue` (randomized selection with weights), `create_layered_sound_cue` (simultaneous playback), `create_looping_ambient_cue`, `create_distance_crossfade_cue`, `create_switch_sound_cue`, `create_oneshot_sfx`, `create_looping_ambient_metasound`, `create_synthesized_tone`, `create_interactive_metasound`.

### Notes

> **Sound Cue connection semantics.** `from` is the child (data source), `to` is the parent (consumer). This matches the `ChildNodes[]` model where the parent holds references to its inputs.
>
> **MetaSound Builder lifecycle.** For multi-step operations, the builder is cached via `FindOrBeginBuilding()`. If the editor restarts, the builder is lost and individual mutation actions return `METASOUND_BUILDER_LOST`. The recommended workflow is `build_metasound_from_spec` for full graph creation in one call.
>
> **SoundWave is read-only.** MonolithAudio does not create SoundWaves (they are imported assets). `get_sound_wave_info` reads properties; `batch_set_sound_wave_properties` can modify UPROPERTY fields via reflection.
>
> **Future phases (not yet implemented).** Phase 3-6 planned (~69 additional actions): Audio Scene & Environment (~18), Audio Modulation & Quartz (~18), Analysis & Automation (~20), Middleware Bridges (~13). See `Docs/specs/2026-04-08-monolith-audio-phase3-6-design.md`.

---
