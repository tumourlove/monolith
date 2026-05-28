# Monolith — MonolithAudio Module

**Parent:** [SPEC_CORE.md](../SPEC_CORE.md)
**Engine:** Unreal Engine 5.7+
**Version:** 0.14.10 (Beta) — [Unreleased] adds 12 MetaSound document introspection actions

---

## MonolithAudio

**Dependencies:** Core, CoreUObject, Engine, MonolithCore, AudioMixer, AudioEditor, AssetTools, Json, JsonUtilities, Slate, SlateCore, UnrealEd
**Namespace:** `audio` | **Tool:** `audio_query(action, params)` | **Actions:** 98 (Phase J F18: +`create_test_wave`; v0.14.10 [Unreleased]: +12 MetaSound document introspection actions from PR #18 by @alakangas)
**Conditional:** MetaSound features wrapped in `#if WITH_METASOUND`. When MetaSound is absent, MetaSound Builder + introspection actions are inert but all other actions (Sound Cue, CRUD, batch, query, perception) function normally. Build.cs auto-detects MetaSound at `Engine/Plugins/Runtime/Metasound`. Companion deep indexer `FMetaSoundIndexer` lives in MonolithIndex (also v0.14.10 [Unreleased] from PR #18).
**Settings toggle:** `bEnableAudio` (default: True)

MonolithAudio provides MCP coverage of audio asset creation, inspection, batch management, Sound Cue graph building, MetaSound graph building, and AI Perception sound binding. It covers the 5 configurable audio asset types (SoundAttenuation, SoundClass, SoundMix, SoundConcurrency, SoundSubmix), read-only SoundWave inspection, Sound Cue node graph construction, MetaSound Builder API integration, and `UAssetUserData`-based perception stimulus authoring.

**No overlap with Resonance** — Resonance owns runtime footstep/surface/movement audio playback. MonolithAudio owns editor-time asset creation, management, and inspection.

### Action Categories

| Category | Actions | Source file | Description |
|----------|---------|-------------|-------------|
| Asset CRUD | 16 | `MonolithAudioAssetActions.cpp` | Create/get/set triads for SoundAttenuation, SoundClass, SoundMix, SoundConcurrency, SoundSubmix + `create_test_wave` (Phase J F18) |
| Query & Search | 10 | `MonolithAudioQueryActions.cpp` | List/search audio assets, hierarchy inspection, reference queries, stats, audio health checks (missing class, no attenuation, unused) |
| Batch Operations | 10 | `MonolithAudioBatchActions.cpp` | Batch assign sound class/attenuation/submix/concurrency/compression/looping/virtualization, batch rename, batch set properties, apply audio template |
| Sound Cue Graph | 21 | `MonolithAudioSoundCueActions.cpp` | Sound Cue CRUD, node add/remove/connect, graph read, node property editing, `build_sound_cue_from_spec` (power action), 5 template cues (random, layered, looping, crossfade, switch), validate, preview, delete |
| MetaSound Graph (Builder API, write-side) | 25 | `MonolithAudioMetaSoundActions.cpp` | MetaSound Source/Patch creation, node add/remove/connect/disconnect, graph inputs/outputs, interface management, graph read (live builder), node discovery, `build_metasound_from_spec` (power action), 4 template MetaSounds (oneshot, ambient, synth, interactive), preset, variables, layout |
| MetaSound Document Introspection (read-side, v0.14.10 [Unreleased]) | 12 | `MonolithAudioMetaSoundIntrospectionActions.cpp` | Read-only walk of `IMetaSoundDocumentInterface::GetConstDocument()` → `FMetasoundFrontendDocument`. Project-wide enumeration (`list_metasounds`), per-asset page list (`list_metasound_documents`), full-document walk (`get_metasound_document`), summary counts (`get_metasound_summary`), per-node instance inspection (`inspect_metasound_node_instance`), edge enumeration (`get_metasound_document_connections`), variables (`get_metasound_document_variables`), user-facing inputs/outputs (`get_metasound_user_parameters`), node search (`search_metasound_document_nodes`), asset metadata (`get_metasound_info`), external class deps (`get_metasound_dependencies`), validator (`validate_metasound`). PR #18 by @alakangas, refactored into `audio_query` namespace. Distinct from the Builder API actions above — those read live builder state, these read on-disk doc state. |
| AI Perception Binding | 4 | `MonolithAudioPerceptionActions.cpp` | `bind_sound_to_perception`, `unbind_sound_from_perception`, `get_sound_perception_binding`, `list_perception_bound_sounds`. Authored via `UMonolithSoundPerceptionUserData` + runtime subsystem. **F11** strict input validation (loudness/max_range floors, tag length cap, sense_class allowlist) |

**Total:** 16 + 10 + 10 + 21 + 25 + 12 + 4 = **98**.

### Phase J fixes touching this module

- **F11 (2026-04-26)** — `audio::bind_sound_to_perception` now rejects four silent-accept input seams (`loudness < 0`, `max_range < 0`, `tag.Len() > 255`, unknown `sense_class`). New strict `ParseSenseClass` allowlist replaces the buggy `TObjectIterator` walk; v1 supports `Hearing` only, future senses return distinct `"deferred to v2"` error. Investigation: `Docs/research/2026-04-26-j3-audio-validation-findings.md`.
- **F18 (2026-04-26)** — New `audio::create_test_wave` action procedurally synthesizes a 16-bit mono sine-tone `USoundWave` for test fixtures (no asset deps). Validates `frequency_hz`, `duration_seconds`, `sample_rate`, `amplitude`. UE 5.7 `FEditorAudioBulkData::UpdatePayload(FSharedBuffer, Owner)` payload write.

See [SPEC_CORE.md §11 Recent Fixes](../SPEC_CORE.md#recent-fixes-phase-j--shipped-in-0147) for the long-form descriptions.

### MetaSound Document Introspection (v0.14.10 [Unreleased])

Read-only inspection of MetaSound asset state via `IMetaSoundDocumentInterface::GetConstDocument()`. Distinct from the existing 25 Builder API actions which read **live builder state** during graph mutation; the introspection actions read **on-disk document state** for arbitrary assets without requiring an active builder session.

Source-of-truth: PR [#18](https://github.com/tumourlove/monolith/pull/18) by **@alakangas**. The PR originally proposed a separate `MonolithMetaSound` module + `metasound_query` namespace; landed refactored into the existing `MonolithAudio` module + `audio_query` namespace per maintainer architectural preference. All 12 PR action handlers ported with name disambiguation; helper functions consolidated into a `MonolithAudioMetaSoundIntrospectionActions.cpp` anonymous namespace with `Introspection_*` prefix.

**12 actions** — all conditional on `WITH_METASOUND`:

| Action | Purpose | Returns |
|---|---|---|
| `list_metasounds` | Project-wide enumeration of `UMetaSoundSource` + `UMetaSoundPatch` assets | `{ assets[{name,path,object_path,type}], count, filter, type_filter }` |
| `list_metasound_documents` | List the pages of one MetaSound document | `{ asset_path, pages[{page_id,node_count,edge_count,variable_count}], page_count, subgraph_count, dependency_count }` |
| `get_metasound_document` | Full per-page document walk: nodes + edges + variables | `{ asset_path, page_id, nodes[full+pins+literals], edges[+vertex names], variables }` |
| `get_metasound_summary` | Lightweight summary version of the above | `{ asset_path, page_id, nodes[summary only], node_count, edge_count }` |
| `inspect_metasound_node_instance` | Full node serialize for one instance | `{ node_id, name, class_id, class_name, input_count, output_count, inputs, outputs, input_literals, incoming_edges, outgoing_edges }` |
| `get_metasound_document_connections` | Edge enumeration with optional node filter | `{ asset_path, edges[+optional node_id filter], edge_count }` |
| `get_metasound_document_variables` | Graph variables with types + defaults | `{ asset_path, variables[name,type,id,literal], variable_count }` |
| `get_metasound_user_parameters` | Public inputs + outputs (the user-facing parameter surface) | `{ asset_path, inputs[+default_value], outputs, input_count, output_count }` |
| `search_metasound_document_nodes` | Substring node-name search | `{ asset_path, query, matches[node summary], match_count }` |
| `get_metasound_info` | Asset-level metadata + class info | `{ asset_path, asset_class, type, root_graph_class, version{major,minor}, default-page counts }` |
| `get_metasound_dependencies` | External class deps + subgraphs | `{ asset_path, dependencies[id,class_name,type,version], subgraphs[id,class_name,node_count], counts }` |
| `validate_metasound` | Lint pass: duplicate node IDs, dangling edges, unconnected inputs | `{ asset_path, errors[duplicate_node_id, dangling_edge], warnings[unconnected_inputs], counts, valid }` |

Naming disambiguates from the Builder-side actions: PR's `get_graph_data` → `get_metasound_document` (vs Builder-side `get_metasound_graph`); PR's `get_node_details` → `inspect_metasound_node_instance` (vs Builder-side `get_metasound_node_info`); PR's `get_connections` → `get_metasound_document_connections` (vs Builder-side `list_metasound_connections`); PR's `get_user_parameters` → `get_metasound_user_parameters` (vs Builder-side `get_metasound_input_names`).

Companion deep indexer: `FMetaSoundIndexer` lives in `MonolithIndex/Private/Indexers/MetaSoundIndexer.cpp`, also from PR #18 by @alakangas. Sentinel-class registration mirroring `FNiagaraIndexer`. Walks all project MetaSounds at reindex time and records nodes / edges / variables / dependencies into `ProjectIndex.db` for cross-asset query via `project_query`. Throttled via `FMonolithMemoryHelper`. Toggle: `bIndexMetaSounds` (default true) under Editor Preferences → Plugins → Monolith → Indexing → Deep Indexers.

### Key Actions

> **`build_sound_cue_from_spec` (power action).** Creates a complete Sound Cue graph from a JSON specification in a single call. The spec defines nodes (with type and properties), connections (from/to with child_index), and the first node. Handles node creation via `ConstructSoundNode`, property setting via reflection, connection wiring via `ChildNodes[]`, `LinkGraphNodesFromSoundNodes()`, and `CacheAggregateValues()`.
>
> **`build_metasound_from_spec` (power action).** Creates a complete MetaSound from a JSON specification in a single call. The spec defines type (Source/Patch), format, interfaces, graph inputs/outputs, nodes, connections, and interface wiring. Uses `UMetaSoundBuilderSubsystem::CreateSourceBuilder()`, `AddNodeByClassName()`, `ConnectNodes()`, and `BuildToAsset()`.
>
> **`apply_audio_template`.** Applies a combined settings template (sound class, attenuation, compression, submix, concurrency, looping, virtualization) to multiple assets in one call. The most efficient way to standardize audio pipeline configuration.
>
> **Template cues and MetaSounds.** Pre-built audio patterns: `create_random_sound_cue` (randomized selection with weights), `create_layered_sound_cue` (simultaneous playback), `create_looping_ambient_cue`, `create_distance_crossfade_cue`, `create_switch_sound_cue`, `create_oneshot_sfx`, `create_looping_ambient_metasound`, `create_synthesized_tone`, `create_interactive_metasound`.

### Bulk Fill & Describe Surface (2026-05-11)

`MonolithAudioBulkFillAdapter` registers under `FMonolithBulkFillRegistry` for the `audio` namespace, exposed via the framework-level `bulk_fill_query("apply", ...)` and `describe_query("schema", ...)` dispatchers. Phase 5 of the MCP ergonomics rollout (design spec `Docs/plans/2026-05-11-monolith-mcp-ergonomics-design.md`).

**Surface summary.** `bulk_fill_query("apply", target_namespace="audio", target="<asset_path>", tree={...})` walks JSON against an attenuation / concurrency / MetaSound asset and commits atomically. `describe_query("schema", target_namespace="audio", target="<asset_path>")` returns the writable field tree — units, enum values, defaults, the 50-field USoundAttenuation surface, MetaSound input polymorphic dispatch hints.

**fill_kind catalogue (3 — enumerated against `MonolithAudioBulkFillAdapter.cpp`):**

| `fill_kind` | Target shape | Walks | Gate |
|---|---|---|---|
| `Attenuation` | `USoundAttenuation` | `properties:{}` against the ~50-field attenuation tree — falloff, focus, occlusion, reverb send, submix send | gate-free |
| `Concurrency` | `USoundConcurrency` | `properties:{}` against MaxCount / ResolutionRule / VolumeScale / EvictionPriority etc. | gate-free |
| `MetaSoundInputs` | `UMetaSoundSource` / `UMetaSoundPatch` | `inputs:{}` written to the user-facing input surface via Builder API | **`#if WITH_METASOUND`** |

**Sample tree (Attenuation):**

```json
{
  "target": "/Game/Audio/Attenuations/SA_Footsteps",
  "tree": {
    "fill_kind": "Attenuation",
    "properties": {
      "FalloffDistance": 2000.0,
      "AttenuationShape": "Sphere",
      "bEnableOcclusion": true,
      "OcclusionLowPassFilterFrequency": 800.0,
      "SubmixSends": [{"Submix": "/Game/Audio/Submixes/SM_Combat", "SendLevel": 0.6}]
    }
  },
  "dry_run": true
}
```

**Adapter-specific quirks.**

- **`#if WITH_METASOUND` gate is INSIDE the adapter — M6 invariant.** Vanilla paths (`Attenuation`, `Concurrency`) compile and dispatch regardless of MetaSound presence. The `MetaSoundInputs` fill_kind compiles inert when `WITH_METASOUND=0` and returns a clean error `"audio adapter: MetaSoundInputs fill_kind requires MetaSound (WITH_METASOUND=0 in this build). Vanilla paths (Attenuation, Concurrency) remain available."`. Schema descriptor surfaces the MetaSoundInputs row with `(unavailable — WITH_METASOUND=0)` in release builds so `discover` parity stays symmetrical across configurations.
- **MetaSound builder lifecycle.** Builder handles die on editor restart. For `MetaSoundInputs` fills, the adapter calls `FindOrBeginBuilding()` per transaction — if a prior in-flight builder handle has expired, the adapter rebuilds the handle within the same transaction (matches the existing `build_metasound_from_spec` recommendation).
- **Private USoundWave fields are reflection-only.** `CompressionQuality`, `SoundAssetCompressionType` and similar private UPROPERTY fields are walkable via reflection but not via direct getter. Schema marks them `private_reflection_only: true`. Bulk_fill currently DOES NOT target USoundWave — this is a `(WISHLIST)` follow-up since SoundWaves are imported, not authored.
- **MetaSound input polymorphic dispatch.** Inputs are typed (`Float`, `Int32`, `Bool`, `String`, `Trigger`, `Object`) — the adapter's MetaSoundInputs handler dispatches per type. Mismatched types (e.g. number JSON passed to a `String` input) surface as `field_writes:[{path:"inputs.X", ok:false, reason:"type mismatch: input is String, got number"}]` in the dry-run report.
- **MCP authoring inside PIE.** Dispatcher gates all audio writes out of PIE per existing cross-cutting quirk; dry-run still works in PIE for read-only schema inspection.

**Limitations / v1.1 follow-ups.**

- USoundNode property opacity across 22 node types — `(WISHLIST v1.1)` — Sound Cue node-property fill_kind not implemented this phase.
- USoundClass / USoundMix / USoundSubmix bulk_fill — `(WISHLIST v1.1)` — extend `apply_audio_template` pattern.
- MetaSound graph-edge bulk_fill — `(WISHLIST)` — node/edge graph writes still through `build_metasound_from_spec`.
- CSV ingest — `(WISHLIST v1.1)` per Q2.

### Notes

> **Sound Cue connection semantics.** `from` is the child (data source), `to` is the parent (consumer). This matches the `ChildNodes[]` model where the parent holds references to its inputs.
>
> **MetaSound Builder lifecycle.** For multi-step operations, the builder is cached via `FindOrBeginBuilding()`. If the editor restarts, the builder is lost and individual mutation actions return `METASOUND_BUILDER_LOST`. The recommended workflow is `build_metasound_from_spec` for full graph creation in one call.
>
> **SoundWave is read-only.** MonolithAudio does not create SoundWaves (they are imported assets). `get_sound_wave_info` reads properties; `batch_set_sound_wave_properties` can modify UPROPERTY fields via reflection.
>
> **Future phases (not yet implemented).** Phase 3-6 planned (~69 additional actions): Audio Scene & Environment (~18), Audio Modulation & Quartz (~18), Analysis & Automation (~20), Middleware Bridges (~13). See `Docs/specs/2026-04-08-monolith-audio-phase3-6-design.md`.

---
