# Monolith — TODO

Last updated: 2026-03-30

---

### MonolithGAS Module — 130 Actions, Phases 1-4 COMPLETE (2026-03-29)

- [x] Phase 1 — Abilities (28 actions): CRUD, grant, activate, cancel, spec handles, instancing, tags, costs, cooldowns.
- [x] Phase 2 — Attributes (20) + Effects (26): Attribute sets, get/set values, derived attributes, init/clamping/replication. GE authoring, duration, modifiers, executions, stacking, period, conditional application.
- [x] Phase 3 — ASC (14) + Tags (10) + Cues (10): ASC inspection/config, granted abilities, active effects, owned tags, replication mode. Tag hierarchy, matching, loose tags, containers, queries. Cue notify CRUD (static + actor), cue params, handler lookup.
- [x] Phase 4 — Targets (5) + Input (5) + Inspect (6) + Scaffold (6): Target data handles, actor selection, confirmation. Enhanced Input binding, input tags. Runtime inspection (PIE-only). Common pattern scaffolding.
- [x] Conditional compilation — `#if WITH_GBA` wraps entire module. Compiles clean with WITH_GBA=1 and WITH_GBA=0.
- [x] Settings toggle — `bEnableGAS` in UMonolithSettings.

#### MonolithGAS — Testing Complete (2026-03-30)

- [x] **Functional testing** — 53/53 tests PASS, 0 FAIL. 12 bugs found and fixed during testing. 8 git commits (32c86d7 through 5639dda). PIE runtime tests all passing. Key fixes: IGameplayTagsEditorModule API for tags, three-tier EnsureAssetPathFree guard, BS_BeingCreated suppression for ASC SCS, AR pre-filter on all GetAsset calls, GAS deep indexer in MonolithIndex.
- [x] **GAS deep indexer** — GASIndexer added to MonolithIndex. `project_query("search")` surfaces GAS assets with rich metadata.
- [x] **Skill file** — `unreal-gas` skill created in `.claude/skills/` and `Plugins/Monolith/Skills/`. 130 actions, 10 categories, workflow examples.

#### MonolithGAS — Remaining Work

- [ ] **Template gaps** — `init_player_stats` and `init_enemy_stats` scaffold templates not yet implemented. Should generate attribute sets + initialization GEs for common game archetypes.
- [ ] **Helper deduplication** — Several helper functions are duplicated across action classes (tag container utilities, effect spec builders). Consolidate into shared `MonolithGASHelpers`.
- [ ] **Type-safe reflection** — Attribute set property access uses string-based reflection. Investigate `FGameplayAttribute` direct property pointer for safer access.
- [ ] **Discover enhancement** — `monolith_discover("gas")` should include GAS-specific workflow hints and category groupings in the response metadata.

---

### MonolithMesh Module — 242 Actions (197 core + 45 experimental town gen), ALL 22 PHASES COMPLETE + Proc Geo Overhaul + Procedural Town Generator + Fix Plans v2-v5 (2026-03-30)

- [x] Phase 0-12 — Original 111 actions compiled and tested.
- [x] Phase 13-22 — Expansion 76 actions compiled (lights, volumes, horror intel, tech art, sublevels, context props, proc geo, presets, encounters, polish).
- [x] Proc Geo Overhaul — 5 new actions (list_cached_meshes, clear_cache, validate_cache, get_cache_stats, create_blueprint_prefab). Sweep-based thin walls, auto-collision, collision-aware prop placement, proc mesh caching, blueprint prefabs, door trim frames, floor-aware spawning, human-scale defaults. 187 → 192 actions.
- [x] Procedural Town Generator — 45 new actions across 11 sub-projects (SP1-SP10). Grid-based buildings, floor plans, facades, roofs, city blocks, spatial registry, auto-volumes, terrain adaptation, architectural features, debug views, room furnishing. 196 → 241 mesh actions, 639 → 684 total.
- [x] Fix Plan v2 — 20 fixes across 3 phases (geometry, floor plan, integration). Stair angles, switchbacks, corridor/door widths, per-floor assignment, aspect ratios, footprint validation, exterior entrance guarantee, building_context/wall_openings on arch features, validate_building action. 241 → 242 mesh actions, 684 → 685 total.
- [x] Fix Plan v5 — 7 fixes (2026-03-30): facade reorder, boolean isolation, wall alignment, door clamp, window density, template variety, furniture placement. Town gen still has fundamental geometry issues (wall misalignment, room separation).
- [x] **Town gen marked EXPERIMENTAL** (2026-03-30) — `bEnableProceduralTownGen = false` by default in UMonolithSettings. 45 town gen actions only registered when enabled. Core mesh actions (197) always available.

#### Procedural Town Generator — Fix Plan v2 Follow-Up (2026-03-28)

- [ ] **Door mesh placement** — Place closed door meshes in doorways for horror pacing (doors block sightlines, create tension before opening)
- [ ] **Interior lighting policy** — Define per-room-type lighting defaults for generated buildings (e.g., flickering fluorescent in corridors, warm lamp in bedrooms, no light in storage). Wire into `furnish_room` or separate `light_building` action
- [ ] **Per-building retry in orchestrator** — When `generate_floor_plan` fails for one building in a `create_city_block` call, retry that building with adjusted params instead of failing the entire block
- [ ] **Multiple entrances for large buildings** — Buildings above a size threshold should have 2+ exterior entrances (front + back door, or front + fire exit). Currently only one entrance guaranteed
- [ ] **Batch validation action** — `validate_block` to run `validate_building` across all buildings in a block and return aggregate results

#### Procedural Town Generator — Follow-Up (2026-03-28)

- [ ] **Integration test: full city block** — Generate a complete city block with `create_city_block({buildings: 4, genre: "horror", seed: 42})` and verify all SPs work together end-to-end (SP1 grid → SP2 floor plan → SP3 facade → SP4 roof → SP5 orchestrator → SP6 registry → SP7 volumes → SP10 furnishing)
- [ ] **Performance profiling on 4-8 building blocks** — Profile generation time, mesh actor count, volume count, navmesh build time against the performance budget (target: <30s for 4 buildings, <50 mesh actors, <100 volumes)
- [ ] **Additional building archetypes** — mansion, warehouse, church, school. Add to `Saved/Monolith/BuildingArchetypes/`
- [ ] **Additional facade styles** — Art Deco, Industrial, Modern. Add to `Saved/Monolith/FacadeStyles/`
- [ ] **SP11: Street network generation** — Connecting multiple blocks into a coherent street grid. Road hierarchy (main street, side street, alley), intersections, traffic flow patterns. Would enable full neighborhood-scale generation

#### MonolithMesh — Release / Wiki TODO

- [ ] **Wiki major update** — All 192 mesh actions need wiki documentation. Current wiki covers original modules only.
- [ ] **Genre Preset Authoring Guide** — Dedicated wiki page (or `PRESET_AUTHORING.md`) explaining how LLMs/users create presets for OTHER genres (fantasy, sci-fi, detective, cozy). Include:
  - Room template JSON format with examples
  - Storytelling pattern format (element types, radial distribution)
  - Acoustic profile format (surface absorption/transmission/loudness)
  - Tension profile format (factor weights, threshold mappings)
  - Prop kit format (items, relative positions, spawn_chance)
  - Step-by-step: "How to create a Fantasy Dungeon preset pack"
  - How to test presets via MCP before distributing
  - How to export/import/share preset packs
- [ ] **SPEC.md MonolithMesh section** — Full 241-action reference with param schemas (currently has Phase 1-4 section + proc geo overhaul + Procedural Town Generator SP1-SP10 + validate_building)
- [ ] **MCP.md mesh_query docs** — Tool reference for mesh_query namespace
- [ ] **README update** — Feature highlight for MonolithMesh in the plugin README

#### MonolithMesh — Known Issues / Polish

- [x] **Placement overlap warnings:** DONE — `create_primitive`, `_batch`, `import_layout`, `scatter_props` warn when overlapping existing actors
- [ ] **BP_MonolithBlockoutVolume** — Construction Script for per-RoomType wireframe colors (cosmetic polish)
- [ ] **Discover workflow hints** — Add workflow metadata to `monolith_discover("mesh")` response
- [ ] **Tier-based discover filtering** — `monolith_discover("mesh", {"tier": "audio"})` to reduce token usage

#### MonolithMesh — Context-Aware Prop Placement (NEW)

- [ ] **Surface-aware scatter (`scatter_on_surface`)** — Place props ON specific surfaces, not just floors. Detect shelf tops, table tops, cabinet interiors, wall surfaces via downward/directional traces from the surface actor's bounds. "Place 5 books on this shelf" should work.
- [ ] **Disturbance levels (`set_room_disturbance`)** — Apply a disturbance level to placed props in a volume:
  - `"orderly"` — aligned, evenly spaced, upright
  - `"slightly_messy"` — small random offsets, some tilted 5-15 degrees
  - `"ransacked"` — large random offsets, many tipped over (60-90 degree rotations), some on floor
  - `"abandoned"` — like ransacked + props pushed to edges (simulating years of settling)
  Implementation: iterate placed actors in volume, apply progressive random transforms based on disturbance level. Single undo transaction.
- [ ] **Physics prop sleep state (`configure_physics_props`)** — Set SimulatePhysics=true + bStartAwake=false on designated actors. They sit in their placed position until bumped by player or woken by gameplay event. Essential for interactive horror (knock over a stack of cans, send bottles rolling).
- [ ] **Gravity-settle placement (`settle_props`)** — For each prop: enable physics, simulate for N frames (or until velocity < threshold), capture settled transform, disable physics. Gives organic "someone dropped this here" look. UE5 has `UPhysicsSimulationComponent` or we can use `FPhysicsInterface::Simulate()`.
- [ ] **Themed prop kits** — JSON definitions like room templates but for prop sets: "office_desk_clutter" (papers, pens, mug, monitor, keyboard), "hospital_tray" (syringe, bandages, clipboard). Each kit defines items with relative positions to an anchor point. `place_prop_kit` action.
- [ ] **Wall/ceiling scatter** — Extend `scatter_props` with `surface` param: "floor" (current), "wall" (horizontal trace outward, align to wall normal), "ceiling" (upward trace), "shelf" (trace to named actor's top surface). Paintings, clocks, chains, cables.

#### MonolithMesh — Procedural Geometry (Overhaul COMPLETE 2026-03-28)

See `Docs/plans/2026-03-28-procedural-geometry-wishlist.md` for original wishlist.
- [x] `create_parametric_mesh` — chair, table, shelf, door_frame, stairs, etc. (~15 types). Human-scale defaults (stairs 90/28/18cm, doors 90cm, floor 3cm)
- [x] `create_structure` — room, corridor, L-corridor, T-junction, stairwell, vent. Sweep-based thin walls, door/window/vent trim frames
- [x] `create_building_shell` — multi-story from 2D footprint polygon
- [x] `create_maze` — recursive backtracker, Prim's, Eller's, binary tree
- [x] `create_pipe_network` — sweep circle along path with elbow joints
- [x] `create_fragments` — Voronoi fracture for destruction
- [x] `create_terrain_patch` — Perlin/simplex noise heightmap
- [x] `create_horror_prop` — barricade, debris pile, cage, coffin, broken wall
- [x] Proc mesh caching system — hash-based manifest, `use_cache`/`auto_save` on all proc gen actions, 4 cache management actions (list_cached_meshes, clear_cache, validate_cache, get_cache_stats)
- [x] `create_blueprint_prefab` — dialog-free Blueprint creation from placed actors (replaces create_prefab for MCP workflows)
- [x] Auto-collision on all saved meshes (collision param: auto/box/convex/complex_as_simple/none)
- [x] Collision-aware prop placement (collision_mode: none/warn/reject/adjust on scatter actions)
- [x] Floor-aware spawning (snap_to_floor param, SweepSingle box traces)

#### MonolithMesh — Unified Wishlist from 3 Perspectives (Level Design + Horror + Tech Art)

**P0 — Would use EVERY session (~15 actions, ~80 hours)**

Level Design:
- [ ] `place_light` / `set_light_properties` — spawn + modify lights directly. Lighting IS horror. `suggest_light_placement` advises but can't act
- [ ] `find_replace_mesh` — swap every instance of mesh X with mesh Y across level. Blockout→art pass essential
- [ ] `spawn_volume` — trigger/kill/pain/blocking/nav_modifier/audio/post_process volumes. Can't build a functional level without these
- [ ] `get_actor_properties` / `copy_actor_properties` — read arbitrary component properties, copy settings between actors

Horror Design:
- [ ] `predict_player_paths` — THE multiplier. Auto-generate weighted paths (shortest/safest/curious/cautious) so every horror action works without manual path input
- [ ] `evaluate_spawn_point` — composite score: visibility delay, audio cover, lighting, escape proximity, path commitment
- [ ] `suggest_scare_positions` — optimal locations for scripted events along a path. Scores anticipation, visibility, timing, player agency
- [ ] `evaluate_encounter_pacing` — check spacing/intensity across multiple encounters. Flag back-to-back with no breather

Tech Art:
- [ ] `set_actor_material` / `swap_material_in_level` — assign materials to placed actors. Bridges mesh + material systems
- [ ] `analyze_texel_density` / `compare_texel_density_in_region` — texels/cm consistency. #1 visual quality issue
- [ ] `find_instancing_candidates` / `convert_to_hism` — "SM_Pipe appears 47 times, convert to HISM, save 46 draw calls"
- [ ] `auto_generate_lods` + `set_lod_screen_sizes` — one-shot LOD pipeline for meshes

**P1 — High value, weekly use (~20 actions, ~100 hours)**

Level Design:
- [ ] `build_navmesh` — horror analysis depends on navmesh but we can't trigger a rebuild
- [ ] `manage_sublevel` — create/load/unload/move_actors_to. Horror streaming needs this
- [ ] `place_blueprint_actor` — spawn BP actors with exposed properties ("locked door needing ward key")
- [ ] `select_actors` — control editor selection, focus camera. AI↔human handoff
- [ ] `snap_to_surface` — drop actors onto geometry with normal alignment

Horror Design:
- [ ] `design_encounter` — compose spawn + patrol + exits + sightline breaks + audio zones in one call
- [ ] `suggest_patrol_route` — generate navmesh routes per AI archetype (stalker/patrol/ambusher)
- [ ] `analyze_ai_territory` — score region as AI territory: hiding spots, patrol options, ambush positions
- [ ] `evaluate_safe_room` — score a room: defensible entrance? good lighting? sound isolation?
- [ ] `analyze_level_pacing_structure` — macro tension→release→tension rhythm across entire level

Tech Art:
- [ ] `import_mesh` — FBX/glTF import with settings. Every mesh enters the project here
- [ ] `analyze_material_cost_in_region` — cross-module: mesh placement × material instruction counts
- [ ] `fix_mesh_quality` — auto-fix: remove degenerate tris, weld verts, fix normals (extends analyze_mesh_quality)
- [ ] `set_mesh_collision` / `auto_collision` — write collision back to assets
- [ ] `analyze_lightmap_density` — lightmap texel density + resolution management

**P2 — Workflow accelerators (~15 actions, ~60 hours)**

- [ ] `randomize_transforms` — variation pass on rotation/scale/offset for organic feel
- [ ] `place_spline` — mesh/cable splines for pipes, cables, railings
- [ ] `get_level_actors` — filtered enumeration (class, tag, sublevel, mesh wildcard)
- [ ] `measure_distance` — quick measurement between actors or points
- [x] `create_blueprint_prefab` — replaces `create_prefab` for MCP workflows (dialog-free via HarvestBlueprintFromActors). DONE 2026-03-28
- [ ] `generate_scare_sequence` — procedural scare events with variety + escalation
- [ ] `analyze_framing` — camera composition scoring (leading lines, focal points)
- [ ] `evaluate_monster_reveal` — score reveal quality: silhouette, backlight, distance, partial visibility
- [ ] `validate_horror_intensity` — cap tension for accessibility mode, remove jump scares
- [ ] `generate_hospice_report` — full level audit: intensity caps, rest spacing, cognitive load, one-handed playability

**P3 — Quality of life + future (~10 actions)**

- [ ] `validate_naming_conventions` / `batch_rename_assets`
- [ ] `generate_proxy_mesh` / `setup_hlod`
- [ ] `analyze_texture_budget` — texture streaming pool analysis
- [ ] GeometryScript expansions: `mesh_extrude`, `mesh_subdivide`, `mesh_combine`, `mesh_separate_by_material`, `compute_ao`
- [ ] Integration hooks: AI Director data feed, GAS tension effects, telemetry feedback loop

#### MonolithMesh — Genre Preset System (NEW — Extensibility)

The mesh module ships horror defaults (storytelling patterns, room templates, acoustic profiles, tension scoring). But the SYSTEMS are genre-agnostic. Other LLMs or users should be able to create their own presets for fantasy, sci-fi, detective noir, cozy sim, etc.

**Authoring actions:**
- [ ] `list_storytelling_patterns` — List all available patterns (built-in + user-created)
- [ ] `create_storytelling_pattern` — Author a new pattern JSON: element types, radial distribution, size ranges, spawn chance. Save to `Saved/Monolith/Patterns/`. Example: fantasy "tavern_brawl" (overturned chairs, spilled mead puddles, broken mug fragments)
- [ ] `list_acoustic_profiles` — List available surface acoustic profiles
- [ ] `create_acoustic_profile` — Author a new acoustic property set for a genre. Fantasy: stone_dungeon, wooden_tavern, crystal_cave. Sci-fi: metal_hull, glass_viewport, organic_hive
- [ ] `create_tension_profile` — Define what tension factors mean for a genre. Horror: short sightlines = dread. Fantasy: open vistas = wonder, narrow caves = claustrophobia. Different scoring weights per genre
- [ ] `list_prop_kits` — List available themed prop kits
- [ ] `create_prop_kit` — Author a themed prop kit JSON: items with relative positions, size ranges, spawn chances. "tavern_table_setting" (plates, mugs, candles, food), "sci-fi_console" (screens, buttons, cables)

**Preset packs (JSON bundles in Saved/Monolith/Presets/):**
- [ ] `export_genre_preset` — Bundle all templates + patterns + acoustic profiles + tension config + prop kits into a single distributable JSON/ZIP
- [ ] `import_genre_preset` — Load a genre preset pack, merging with existing presets
- [ ] Ship starter packs: `horror_default` (current), document format for community presets

**Documentation for preset authors:**
- [ ] Write a `PRESET_AUTHORING.md` guide explaining:
  - Room template JSON format (furniture entries, position_pct, size_range)
  - Storytelling pattern JSON format (elements, radial distribution, intensity scaling)
  - Acoustic profile JSON format (surface types, absorption, transmission, loudness)
  - Tension profile JSON format (factor weights, threshold mappings)
  - Prop kit JSON format (items, relative positions, spawn_chance)
  - How to test presets via MCP before distributing
  - Examples for fantasy, sci-fi, detective, cozy genres

**Why this matters:** Monolith becomes not just a tool but a PLATFORM. Horror is Leviathan's genre, but the open-source plugin serves everyone. LLMs working on fantasy games load a fantasy preset pack and immediately have genre-appropriate spatial awareness, environmental storytelling, and room templates. Community-driven expansion without touching C++.

---

### Niagara Expansion — 31 New Actions (2026-03-25)

- [x] **Dynamic Inputs (5):** `list_dynamic_inputs`, `get_dynamic_input_tree`, `remove_dynamic_input`, `get_dynamic_input_value`, `get_dynamic_input_inputs`
- [x] **Emitter Management (3):** `rename_emitter`, `get_emitter_property`, `list_available_renderers`
- [x] **Renderer Configuration (3):** `set_renderer_mesh`, `configure_ribbon`, `configure_subuv`
- [x] **Event Handlers (3):** `get_event_handlers`, `set_event_handler_property`, `remove_event_handler`
- [x] **Simulation Stages (3):** `get_simulation_stages`, `set_simulation_stage_property`, `remove_simulation_stage`
- [x] **Module Outputs (1):** `get_module_output_parameters`
- [x] **Niagara Parameter Collections (5):** `create_npc`, `get_npc`, `add_npc_parameter`, `remove_npc_parameter`, `set_npc_default`
- [x] **Effect Types (3):** `create_effect_type`, `get_effect_type`, `set_effect_type_property`
- [x] **Utilities (5):** `get_available_parameters`, `preview_system`, `diff_systems`, `save_emitter_as_template`, `clone_module_overrides`
- [x] **`move_module` rewrite** — Complete rewrite of move_module to preserve input overrides
- [x] **7 bug fixes** — type fallback warning, spawn shape duplicate, NPC namespace mismatch, + 4 others found during testing
- [x] **Full test pass** — 40/40 PASS, 0 FAIL, 3 SKIPPED. Total Niagara actions: 65 → 96.

---

### Material Function Full Suite (2026-03-25)

- [x] **export_function_graph** — Full graph export with connections, properties, switch details (#7)
- [x] **set_function_metadata** — Update description, categories, library exposure
- [x] **delete_function_expression** — Remove expression(s) from function
- [x] **update_material_function** — Recompile cascade to referencing materials
- [x] **create_function_instance** — Create MFI with parent + optional overrides
- [x] **set_function_instance_parameter** — Set param overrides on MFI
- [x] **get_function_instance_info** — Read MFI parent chain + overrides (11 param types)
- [x] **layout_function_expressions** — Auto-arrange function graph
- [x] **rename_function_parameter_group** — Rename param group
- [x] **create_material_function type param** — Support MaterialLayer/MaterialLayerBlend creation

---

### MonolithUI Module (2026-03-22)

- [x] **NEW: MonolithUI module** — IMPLEMENTED (2026-03-22). 42 actions across 8 action classes in the `ui` namespace (`ui_query` tool). Widget blueprint CRUD, slot layout, HUD/menu templates, styling, UMG animation CRUD, binding inspection, settings scaffolding, accessibility.
- [x] **MonolithUI — MCP testing** — DONE (2026-03-22). All 42 actions PASS. Full test results in TESTING.md.
- [ ] **MonolithUI — Phase 3 deprecation warnings** — `GetBindings` and `FMovieSceneBinding::GetName` may generate deprecation warnings in UE 5.7. Audit and update call sites.
- [x] **MonolithUI — agent skill file** — DONE (2026-03-22). `unreal-ui` skill created in `.claude/skills/` and `Plugins/Monolith/Skills/`, added to `interface-architect` agent definition.
- [x] **MonolithUI — bUIEnabled settings toggle** — DONE (2026-03-22). `UMonolithSettings::bUIEnabled` wired to registration gating (confirmed in SPEC.md settings table).

---

### Recently Fixed (2026-03-14)

- [x] **NEW: Niagara `set_system_property`** — ADDED (2026-03-14). Sets a system-level property (WarmupTime, bDeterminism, etc.) via reflection.
- [x] **NEW: Niagara `set_static_switch_value`** — ADDED (2026-03-14). Sets a static switch value on a module.
- [x] **NEW: Niagara `list_module_scripts`** — ADDED (2026-03-14). Searches available Niagara module scripts by keyword. Returns matching script asset paths.
- [x] **NEW: Niagara `list_renderer_properties`** — ADDED (2026-03-14). Lists editable properties on a renderer via reflection. Params: `asset_path`, `emitter`, `renderer`.
- [x] **Niagara DI class name auto-prefix** — FIXED (2026-03-14). `set_module_input_di` and `get_di_functions` now auto-resolve DI class names: both `NiagaraDataInterfaceCurve` and `UNiagaraDataInterfaceCurve` are accepted (U prefix stripped/added as needed).
- [x] **Niagara `get_module_inputs` returns DI curve data** — FIXED (2026-03-14). Now returns actual FRichCurve key data for DataInterface curve inputs, not just the DI class name.
- [x] **LinearColor/vector defaults deserialization** — FIXED (2026-03-14). `get_module_inputs` correctly deserializes LinearColor and vector default values from string-serialized JSON fallback, no longer returns zeroed values.
- [x] **Material `disconnect_expression` targeted disconnection** — FIXED (2026-03-14). Now supports disconnecting a specific connection via optional `input_name`/`output_name` params, instead of always disconnecting all connections on the expression.
- [x] **Blueprint `add_node` aliases and K2_ prefix fallback** — FIXED (2026-03-14). `add_node` now resolves common node class aliases (e.g. `CallFunction`, `VariableGet`) and automatically tries the `K2_` prefix for function call nodes when the bare name doesn't resolve.
- [x] **Niagara `list_renderers` returns `type` short name** — FIXED (2026-03-14). The `type` field now returns the short renderer class name (e.g. `SpriteRenderer`) instead of the full UClass path.

### Recently Fixed

- [x] **Niagara `set_emitter_property` SimTarget — "Data missing please force a recompile"** — FIXED (2026-03-13). Raw field assignment on `SimTarget` skipped `PostEditChangeVersionedProperty`, so `MarkNotSynchronized` was never called and `RequestCompile(false)` saw unchanged hash → skipped compilation. Fix: call `PostEditChangeVersionedProperty` + `RebuildEmitterNodes` + `SynchronizeOverviewGraphWithSystem` after SimTarget change. Same pattern applied to `bLocalSpace` and `bDeterminism`.
- [x] **Niagara `list_emitters` missing emitter GUID** — FIXED (2026-03-13). Added `"id": Handle.GetId().ToString()` to the emitter listing. Users now have a stable round-trip token.
- [x] **Niagara `get_system_diagnostics` — NEW ACTION** — Added (2026-03-13). Returns compile errors, warnings, renderer/SimTarget incompatibility, GPU+dynamic bounds warnings, and per-script stats (op count, registers, compile status). Also added `CalculateBoundsMode` to `set_emitter_property`.

- [x] **Niagara `create_system` + `add_emitter` — emitters don't persist** — FIXED (2026-03-13). Replaced raw `System->AddEmitterHandle()` with `FNiagaraEditorUtilities::AddEmitterToSystem()` which calls `RebuildEmitterNodes` + `SynchronizeOverviewGraphWithSystem` after adding the handle. Also added `SavePackage` call in both `HandleCreateSystem` and `HandleAddEmitter`. Custom emitter names applied post-add via `SetName()`.
- [x] **Niagara `create_system_from_spec` — fails with `failed_steps:1`** — FIXED (2026-03-13). Added synchronous `RequestCompile(true)` + `WaitForCompilationComplete()` after each emitter add in the spec flow, before modules are added. Removed redundant async `RequestCompile(false)` from `HandleAddEmitter`. Also added error message capture — failed sub-operations now report in an `"errors"` array instead of silent `FailCount++`.
- [x] **All MCP tools return stale in-memory objects after asset recreate** — FIXED (2026-03-13). `LoadAssetByPath` now queries `IAssetRegistry::GetAssetByObjectPath()` + `FAssetData::GetAsset()` first (reflects editor ground truth), falling back to `StaticLoadObject` only if the Asset Registry has no record. Prevents stale `RF_Standalone` ghosts from shadowing recreated assets.
- [x] **Niagara `set_module_input_value` — namespace warnings on compile** — FIXED (2026-03-13). `MatchedFullName` was assigned the stripped short name instead of the full `Module.`-prefixed name from `In.GetName()`. Same fix applied to `HandleSetModuleInputBinding`. Both now pass the full name to `FNiagaraParameterHandle::CreateAliasedModuleParameterHandle`.
- [x] **Registry-level required param validation** — ADDED (2026-03-13). `FMonolithToolRegistry::Execute()` now validates required params from schema before dispatching to the handler. Checks all schema keys marked `required: true`, skips `asset_path` (handled by `GetAssetPath()` with aliases). Returns error listing missing + provided keys.
- [x] **Niagara param name aliases** — ADDED (2026-03-13). All module write actions (`set_module_input_value`, `set_module_input_binding`, `set_module_input_di`, `set_curve_value`, `remove_module`, `move_module`, `set_module_enabled`, `get_module_inputs`, `get_module_graph`) now accept `module_name` and `module` as aliases for the canonical `module_node` param. `input_name` accepted as alias for `input`.
- [x] **`set_expression_property` PostEditChange fix** — FIXED (2026-03-13). Was calling `PostEditChange()` (no args), which didn't rebuild the material graph. Now calls `PostEditChangeProperty(FPropertyChangedEvent(Prop))` with the actual property so `MaterialGraph->RebuildGraph()` fires and the editor display updates correctly.
- [x] **Auto-recompile + PostEditChange on 4 material write actions** — FIXED (2026-03-13). `set_material_property`, `create_material`, `delete_expression`, and `connect_expressions` now all call `Mat->PreEditChange(nullptr)` + `Mat->PostEditChange()` to trigger recompile and push changes through the material graph system.
- [x] **`tools/list` embeds per-action param schemas** — IMPLEMENTED (2026-03-13). `FMonolithHttpServer::HandleToolsList()` now builds the `params` property description with per-action param documentation in `*name(type)` format (`*` = required). AI clients can see all param names and types from the MCP tool list without calling `monolith_discover` first.

### Minor

- [x] **Material `validate_material` false positive islands** — FIXED (2026-03-09). Added MP_MaterialAttributes + 6 missing properties to AllMaterialProperties, seeded BFS from UMaterialExpressionMaterialAttributeLayers. 0 false positives on standard materials. Layer-blend materials still have a known limitation (implicit layer system connections not traversable via pin graph).
- [x] **Blueprint `get_execution_flow` matches comments before events** — FIXED (2026-03-09). Two-pass FindEntryNode: Pass 1 checks events/functions (prefers K2Node_Event, K2Node_FunctionEntry), Pass 2 is fuzzy fallback that skips EdGraphNode_Comment.

---

## Unimplemented Features (stubs in code)

- [x] **Niagara `create_module_from_hlsl`** — DONE (2026-03-15). Creates standalone NiagaraScript with CustomHlsl node, typed I/O pins, ParameterMap flow. Bypasses unexported APIs via UPROPERTY reflection + Signature-driven pin creation.

- [x] **Niagara `create_function_from_hlsl`** — DONE (2026-03-15). Same path as module, `ENiagaraScriptUsage::Function` with direct typed pin wiring.

- [ ] **SSE streaming** — DEFERRED. `MonolithHttpServer.cpp` SSE endpoint returns a single event and closes. Comment: "Full SSE streaming will be implemented when we need server-initiated notifications."
  - **File:** `Source/MonolithCore/Private/MonolithHttpServer.cpp` (~line 232)

- [x] **C++ source indexer — native port complete** — DONE (2026-03-15). `MonolithSource` module now runs a native C++ indexer via `UMonolithSourceSubsystem`. The Python tree-sitter indexer (`Scripts/source_indexer/`) is legacy and no longer invoked. New action: `trigger_project_reindex` for incremental project-only C++ re-index. New commandlet: `UMonolithQueryCommandlet` (-run=MonolithQuery) replaces `monolith_offline.py` as the preferred offline access path.

- [x] **Python indexer: capture full class/struct definitions** — FIXED (2026-03-08). Added UE macro preprocessor that strips UCLASS/USTRUCT/UENUM/UINTERFACE, *_API, GENERATED_BODY() before tree-sitter parsing. 62,059 definitions now captured (was near-zero).

- [x] **Source index: ancestor traversal** — FIXED (2026-03-08). Inheritance table now has 37,010 entries across 34,444 classes. AActor→UObject, APawn→AActor, ACharacter→APawn all working.

---

## Feature Improvements

### Platform

- [ ] **Mac/Linux support** — DEFERRED (Windows-only project). All build-related actions are `#if PLATFORM_WINDOWS` guarded. Live Coding is Windows-only. Update system is Windows-only.

### MonolithIndex — Incremental Indexer Remaining Work

- [ ] **Implement IndexScoped() for individual sentinels** — DependencyIndexer, AnimationIndexer, and other sentinels currently fall back to per-asset `IndexAsset()`. Implement proper `IndexScoped()` for batch efficiency.
- [ ] **Batched frame-budget deep indexing for >10 assets** — When more than 10 assets need deep indexing, spread work across frames with a per-frame time budget to avoid hitches.
- [ ] **External file deletion detection (FDirectoryWatcherModule)** — AR callbacks don't fire for files deleted outside the editor. Hook `FDirectoryWatcherModule` to detect and reconcile external deletions.

### Niagara Module — Improvements

- [ ] **`FindEmitterHandleIndex` should accept numeric index** — `list_emitters` returns `"index"` for each emitter. Allow passing `"0"`, `"1"` etc. as emitter identifier for convenient fallback.
  - **File:** `Source/MonolithNiagara/Private/MonolithNiagaraActions.cpp` (~line 292)

### Animation Module — Wishlist

Priority features identified for future waves:

- [x] **Wave 1 — Read actions (EASY, ~8 actions):** DONE (2026-03-10)
- [x] **Wave 2 — Notify CRUD (EASY, ~4 actions):** DONE (2026-03-10)
- [x] **Wave 3 — Curve CRUD (EASY, ~5 actions):** DONE (2026-03-10)
- [x] **Wave 4 — Skeleton sockets (EASY, ~4 actions):** DONE (2026-03-10) — expanded to 6 actions (added set_blend_space_axis, set_root_motion_settings)
- [x] **Wave 5 — Creation + editing (MODERATE, ~6 actions):** DONE (2026-03-10)
- [x] **Wave 6 — PoseSearch/Motion Matching (MODERATE, ~5 actions):** DONE (2026-03-10)
- [x] **Wave 7 — Anim Modifiers + Composites (MODERATE, ~5 actions):** DONE (2026-03-10)
- [ ] **Wave 8 — IKRig + Control Rig (HARD, ~6 actions):** `get_ikrig_info`, `add_ik_solver`, `get_retargeter_info`, `get_control_rig_info` — requires IKRig/ControlRig module dependencies
- [ ] **Deferred — ABP write ops (HARD):** State machine structural writes (add state/transition) require Blueprint graph mutation, high complexity

---

## Documentation

- [ ] **CI pipeline** — Per Phase 6 plan

---

## Completed

- [x] Core infrastructure (HTTP server, registry, settings, JSON utils, asset utils)
- [x] All 11 domain modules compiling clean on UE 5.7
- [x] SQLite FTS5 project indexer with 14 indexers (Blueprint, Material, Generic, Dependency, Animation, Niagara, DataTable, Level, GameplayTag, Config, Cpp, UserDefinedEnum, UserDefinedStruct, InputAction)
- [x] Python tree-sitter engine source indexer
- [x] Auto-updater via GitHub Releases
- [x] 10 Claude Code skills (including unreal-build and unreal-ui)
- [x] Templates (.mcp.json, CLAUDE.md)
- [x] README, LICENSE, ATTRIBUTION
- [x] HTTP body null-termination fix
- [x] Niagara graph traversal fix (emitter shared graph)
- [x] Niagara emitter lookup hardening (case-insensitive + fallbacks)
- [x] Source DB WAL -> DELETE journal mode fix
- [x] Asset loading 4-tier fallback
- [x] SQL schema creation (BEGIN/END depth tracking for triggers)
- [x] Reindex dispatch fix (FindFunctionByName -> StartFullIndex + UFUNCTION)
- [x] Asset loading crash fix (removed FastGetAsset from background thread)
- [x] Animation `remove_bone_track` — now uses `RemoveBoneCurve(FName)` per bone + child traversal (2026-03-07)
- [x] MonolithIndex `last_full_index` — added `WriteMeta()` call, guarded with `!bShouldStop` (2026-03-07)
- [x] Niagara `move_module` — rewires stack-flow pins only, preserves override inputs (2026-03-07)
- [x] Editor `get_build_errors` — uses `ELogVerbosity` enum instead of substring matching (2026-03-07)
- [x] MonolithIndex SQL injection — all 13 insert methods converted to `FSQLitePreparedStatement` (2026-03-07)
- [x] Animation `LogTemp` -> `LogMonolith` (2026-03-07)
- [x] Editor `CachedLogCapture` dangling pointer — added `ClearCachedLogCapture()` in ShutdownModule (2026-03-07)
- [x] MonolithSource vestigial outer module — flattened structure, deleted stub (2026-03-07)
- [x] Session expiry / reconnection — Removed session tracking entirely. Sessions stored no per-session state and only caused bugs when server restarted. Server is now fully stateless. (2026-03-07)
- [x] Claude tools fail on first invocation — Fixed transport type mismatch in .mcp.json ("http" → "streamableHttp") and fixed MonolithSource stub that wasn't registering actions. (2026-03-07)
- [x] Module enable toggles — settings now checked before registering actions (2026-03-07)
- [x] MCP package CLI cleanup — removed abandoned scaffold (2026-03-07)
- [x] Material action count alignment — skill updated to match C++ reality (2026-03-07)
- [x] Animation action count alignment — skill updated to match C++ reality (2026-03-07)
- [x] Niagara action count alignment — skill updated to match C++ reality (2026-03-07)
- [x] Config action schema documentation — `explain_setting` convenience mode documented in skill (2026-03-07)
- [x] Niagara `reorder_emitters` safety — proper change notifications added (2026-03-07)
- [x] `diff_from_default` INI parsing — rewritten with GConfig/FConfigCacheIni (2026-03-07)
- [x] Config `diff_from_default` enhancement — now compares all 5 config layers (2026-03-07)
- [x] Live Coding trigger action (`editor.live_compile`) — fully implemented (2026-03-07)
- [x] Cross-platform update system — tar/unzip support added (2026-03-07)
- [x] Hot-swap plugin updates — delayed file swap mechanism implemented (2026-03-07)
- [x] Remove phase plan .md files from Source/ — moved to Docs/plans/ (2026-03-07)
- [x] AnimationIndexer — AnimSequence, AnimMontage, BlendSpace indexing (2026-03-07)
- [x] NiagaraIndexer — NiagaraSystem, NiagaraEmitter deep indexing (2026-03-07)
- [x] DataTableIndexer — DataTable row indexing (2026-03-07)
- [x] LevelIndexer — Level/World actor indexing (2026-03-07)
- [x] GameplayTagIndexer — Tag hierarchy indexing (2026-03-07)
- [x] ConfigIndexer — INI config indexing (2026-03-07)
- [x] CppIndexer — C++ symbol indexing (2026-03-07)
- [x] Deep asset indexing — safe game-thread loading strategy implemented (2026-03-07)
- [x] Incremental indexing — delta updates from file change detection (2026-03-07)
- [x] Asset change detection — hooked into Asset Registry callbacks (2026-03-07)
- [x] API reference page — auto-generated API_REFERENCE.md with 119 actions (2026-03-07)
- [x] Contribution guide — CONTRIBUTING.md created (2026-03-07)
- [x] Changelog — CHANGELOG.md created (2026-03-07)
- [x] Clean up MCP/ package — removed abandoned CLI scaffold (2026-03-07)
- [x] `find_callers` / `find_callees` param name fix — `"function"` → `"symbol"` (2026-03-07)
- [x] `read_file` param name fix — `"path"` → `"file_path"` (2026-03-07)
- [x] `read_file` path normalization — forward slash → backslash for DB suffix matching (2026-03-07)
- [x] `get_class_hierarchy` forward-declaration filtering — prefer real definitions over `class X;` (2026-03-07)
- [x] `ExtractMembers` rewrite — brace depth tracking for Allman-style UE code (2026-03-07)
- [x] `get_recent_logs` — accepts both `"max"` and `"count"` param names (2026-03-07)
- [x] `search_config` category filter — changed param read from `"file"` to `"category"` (2026-03-07)
- [x] `get_section` category name resolution — accepts `"Engine"` not just `"DefaultEngine"` (2026-03-07)
- [x] SQLite WAL → DELETE — belt-and-suspenders fix: C++ forces DELETE on open + Python indexer never sets WAL (2026-03-07)
- [x] Source DB ReadOnly → ReadWrite — WAL + ReadOnly silently returns 0 rows on Windows (2026-03-07)
- [x] Reindex absolute path — `FPaths::ConvertRelativePathToFull()` on engine source + shader paths (2026-03-07)
- [x] MonolithHttpServer top-level param merge — params alongside `action` were silently dropped, now merged (2026-03-07)
- [x] UE macro preprocessor — strips UCLASS/USTRUCT/UENUM/UINTERFACE, *_API, GENERATED_BODY() before tree-sitter parsing (2026-03-08)
- [x] Source indexer --clean flag — deletes existing DB before reindexing (2026-03-08)
- [x] Inheritance resolution — 37,010 links across 34,444 classes, full ancestor chains working (2026-03-08)
- [x] Diagnostic counters — definitions/forward_decls/with_base_classes/inheritance_resolved/failed printed after indexing (2026-03-08)
- [x] Preprocessor in ReferenceBuilder — consistent AST for cross-reference extraction (2026-03-08)
- [x] Auto-updater rewrite — tasklist polling, move retry loop 10x3s, errorlevel fix, cmd /c quoting, DelayedExpansion, xcopy /h, rollback rmdir. Windows end-to-end tested v0.4.0→v0.5.0 (2026-03-08)
- [x] Release script `Scripts/make_release.ps1` — sets `"Installed": true` in zip for BP-only users (2026-03-08)
- [x] BP-only support — release zips work without rebuild for Blueprint-only projects (2026-03-08)
- [x] GitHub Wiki — 11 pages: Installation, Tool Reference, Test Status, Auto-Updater, FAQ, Changelog, etc. (2026-03-08)
- [x] Indexer auto-index deferred to `IAssetRegistry::OnFilesLoaded()` — was running too early, only indexing 193/9560 assets (2026-03-09)
- [x] Indexer sanity check — if < 500 assets indexed, skip writing `last_full_index` so next launch retries (2026-03-09)
- [x] Indexer `bIsIndexing` reset in `Deinitialize()` to prevent stuck flag (2026-03-09)
- [x] Index DB changed from WAL to DELETE journal mode (2026-03-09)
- [x] Niagara `trace_parameter_binding` — fixed missing OR fallback for `User.` prefix (2026-03-09)
- [x] Niagara `get_di_functions` — fixed reversed class name pattern, now tries `UNiagaraDataInterface<Name>` (2026-03-09)
- [x] Niagara `batch_execute` — fixed 3 op name mismatches, old names kept as aliases (2026-03-09)
- [x] Niagara actions now accept `asset_path` (preferred) with `system_path` as backward-compat alias (2026-03-09)
- [x] Niagara `duplicate_emitter` accepts `emitter` as alias for `source_emitter` (2026-03-09)
- [x] Niagara `set_curve_value` accepts `module_node` as alias for `module` (2026-03-09)
- [x] NEW: Niagara `list_emitters` action — returns emitter names, index, enabled, sim_target, renderer_count (2026-03-09)
- [x] NEW: Niagara `list_renderers` action — returns renderer class, index, enabled, material (2026-03-09)
- [x] Animation state machine names stripped of `\n` — clean names like "InAir" not "InAir\nState Machine" (2026-03-09)
- [x] Animation `get_state_info` validates required params (machine_name, state_name) (2026-03-09)
- [x] Animation state machine matching changed from fuzzy Contains() to exact match (2026-03-09)
- [x] Animation `get_nodes` now accepts optional `graph_name` filter (2026-03-09)
- [x] NEW: Blueprint `get_graph_summary` — lightweight graph overview (id/class/title + exec connections only, ~10KB vs 172KB) (2026-03-09)
- [x] Blueprint `get_graph_data` now accepts optional `node_class_filter` param (2026-03-09)
- [x] Blueprint `get_variables` now reads default values from CDO (was always empty) (2026-03-09)
- [x] Blueprint indexer CDO fix — same default value extraction fix applied to BlueprintIndexer (2026-03-09)
- [x] Material `export_material_graph` now accepts `include_properties` and `include_positions` params (2026-03-09)
- [x] Material `get_thumbnail` now accepts `save_to_file` param (2026-03-09)
- [x] Niagara `get_compiled_gpu_hlsl` auto-compiles system if HLSL not available (2026-03-09)
- [x] Niagara `User.` prefix stripped in get_parameter_value, trace_parameter_binding, remove_user_parameter, set_parameter_default (2026-03-09)
- [x] Per-action param schemas in `monolith_discover()` output — all 122 actions now have param documentation (2026-03-09)
- [x] Niagara `get_module_inputs` — types now use `PinToTypeDefinition` instead of default Vector4f (2026-03-09)
- [x] Niagara `get_ordered_modules` — usage filter now works with shorthands ("spawn", "update"), returns error on invalid values, returns all stages when omitted (2026-03-09)
- [x] Niagara `get_renderer_bindings` — clean JSON output (name/bound_to/type) instead of raw UE struct dumps (2026-03-09)
- [x] Niagara `get_all_parameters` — added optional `emitter` and `scope` filters (2026-03-09)
- [x] Animation `get_transitions` — cast to `UAnimStateNodeBase*` instead of `UAnimStateNode*`, resolves conduit names. Added from_type/to_type fields (2026-03-09)
- [x] Material `validate_material` — seeds BFS from `UMaterialExpressionCustomOutput` subclasses + `UMaterialExpressionMaterialAttributeLayers`, added MP_MaterialAttributes + 6 missing properties. 0 false positives on standard materials (2026-03-09)
- [x] Blueprint `get_execution_flow` — two-pass FindEntryNode: Pass 1 prefers events/functions, Pass 2 fuzzy fallback skips comments (2026-03-09)
- [x] Blueprint `get_graph_summary` all-graphs mode — returns all graphs when graph_name empty, single graph when specified (2026-03-09)
- [x] **CRITICAL: Hot-swap updater deletes Saved/** — swap script moved entire plugin dir to backup, then only preserved .git/.github. EngineSource.db (1.8GB) and ProjectIndex.db were destroyed on cleanup. Fixed: both static .bat script and C++ template (Windows + Mac/Linux) now preserve Saved/ alongside .git (2026-03-10)
- [x] Material `build_material_graph` class lookup — `FindObject<UClass>(nullptr, ClassName)` always returned null. Changed to `FindFirstObject<UClass>(ClassName, NativeFirst)` with U-prefix fallback. Short names like "Constant", "VectorParameter" now resolve correctly (2026-03-10)
- [x] Material `disconnect_expression` missing material outputs — `disconnect_outputs=true` only iterated other expressions' inputs, never checked material output properties. Added `GetExpressionInputForProperty()` loop over `MaterialOutputEntries` (2026-03-10)
- [x] NEW: Material `create_material` — creates UMaterial asset at path with Opaque/DefaultLit/Surface defaults (2026-03-10)
- [x] NEW: Material `create_material_instance` — creates UMaterialInstanceConstant from parent material with parameter overrides (2026-03-10)
- [x] NEW: Material `set_material_property` — sets material properties (blend_mode, shading_model, etc.) via UMaterialEditingLibrary::SetMaterialUsage (2026-03-10)
- [x] NEW: Material `delete_expression` — deletes expression node by name from material graph (2026-03-10)
- [x] NEW: Material `get_material_parameters` — returns scalar/vector/texture/static_switch parameter arrays with values, works on UMaterial and UMaterialInstanceConstant (2026-03-10)
- [x] NEW: Material `set_instance_parameter` — sets scalar/vector/texture/static_switch parameters on MIC (2026-03-10)
- [x] NEW: Material `recompile_material` — forces material recompile via UMaterialEditingLibrary::RecompileMaterial (2026-03-10)
- [x] NEW: Material `duplicate_material` — duplicates material asset to new path via UEditorAssetLibrary::DuplicateAsset (2026-03-10)
- [x] NEW: Material `get_compilation_stats` — returns sampler count, texture estimates, UV scalars, blend mode, expression count. API corrected for UE 5.7 FMaterialResource (2026-03-10)
- [x] NEW: Material `set_expression_property` — sets properties on expression nodes (e.g., DefaultValue on scalar param) (2026-03-10)
- [x] NEW: Material `connect_expressions` — wires expression outputs to expression inputs or material property inputs. Supports expr-to-expr and expr-to-material-property (2026-03-10)
- [x] **Niagara `get_module_inputs` returns all input types** — IMPLEMENTED (2026-03-11). Now uses engine's `FNiagaraStackGraphUtilities::GetStackFunctionInputs` with `FCompileConstantResolver`. Returns floats, vectors, colors, data interfaces, enums, bools — not just static switch pins. Output uses short names (no `Module.` prefix).
- [x] **Niagara `batch_execute` missing write ops** — FIXED (2026-03-11). Dispatch table now covers all 23 write op types. Previously missing 8: `remove_user_parameter`, `set_parameter_default`, `set_module_input_di`, `set_curve_value`, `reorder_emitters`, `duplicate_emitter`, `set_renderer_binding`, `request_compile`.
- [x] **Niagara `set_module_input_di` validation** — FIXED (2026-03-11). Now validates input exists and is DataInterface type before applying. Rejects nonexistent inputs and non-DI type inputs with descriptive errors. `config` param now accepts JSON object (not just string).
- [x] **Niagara `create_system_from_spec` functional** — FIXED (2026-03-11). Was broken — now uses `UNiagaraSystemFactoryNew::InitializeSystem` for proper system creation.
- [x] **Niagara `FindEmitterHandleIndex` auto-select removed** — FIXED (2026-03-11). Was silently auto-selecting the single emitter when a specific non-matching name was passed. Now requires the name to match if one is provided, returning a clear error instead.
- [x] **Niagara write actions accept both short and prefixed input names** — IMPLEMENTED (2026-03-11). `set_module_input_value`, `set_module_input_binding`, `set_module_input_di`, `set_curve_value` all accept both `InputName` (short) and `Module.InputName` (prefixed) forms.
- [x] **CRASH: `add_virtual_bone` no bone validation** — FIXED (2026-03-10). Added `FReferenceSkeleton::FindBoneIndex()` validation for both source and target bones before calling `AddNewVirtualBone()`. Previously created bogus virtual bones with non-existent bones, causing array OOB crash on skeleton access.
- [x] **`set_notify_time` / `set_notify_duration` reject AnimMontage** — FIXED (2026-03-10). Changed `LoadAssetByPath<UAnimSequence>` to `LoadAssetByPath<UAnimSequenceBase>` so montages and composites are accepted. Also made `set_notify_duration` error message include `(total: N)` to match `set_notify_time`.
- [x] **`remove_virtual_bones` false success for non-existent bones** — FIXED (2026-03-10). Now validates each bone name against actual virtual bones before removal. Returns `not_found` array and errors if all names are invalid.
- [x] **`delete_montage_section` allows deleting last section** — FIXED (2026-03-10). Added guard: if montage has only 1 section remaining, returns error "Cannot delete the last remaining montage section".
- [x] **`add_blendspace_sample` generic error on skeleton mismatch** — FIXED (2026-03-10). Added skeleton comparison before adding sample, returns descriptive error naming both skeletons when they don't match.
- [x] **Animation Waves 1-7: 39 new actions** — IMPLEMENTED (2026-03-10). Total animation module: 62 actions + 5 PoseSearch = 67. Waves: 8 read actions, 4 notify CRUD, 5 curve CRUD, 6 skeleton+blendspace, 6 creation+montage, 5 PoseSearch, 5 modifiers+composites. Build errors fixed: BlendParameters private, GetTargetSkeleton removed, UMirrorDataTable forward-decl, GetBoneAnimationTracks deprecated, OpenBracket FText.
- [x] **Blueprint module upgrade: 6 → 46 actions** — IMPLEMENTED (2026-03-13). Added 40 new write actions across 5 categories: Variable CRUD (7), Component CRUD (6), Graph Management (9), Node & Pin Operations (6), Compile & Create (5). Also expanded Read Actions to 13 (added get_components, get_component_details, get_functions, get_event_dispatchers, get_parent_class, get_interfaces, get_construction_script). Total plugin actions: 177 → 217.
- [x] **Offline CLI (`monolith_offline.py`)** — IMPLEMENTED (2026-03-13). Pure Python (stdlib only) CLI that queries `EngineSource.db` and `ProjectIndex.db` directly without the editor running. 14 actions across 2 namespaces: `source` (9 actions, mirrors `source_query`) and `project` (5 actions, mirrors `project_query`). Read-only, zero footprint, zero dependencies. Fallback for when MCP/editor is unavailable. Location: `Saved/monolith_offline.py`.
- [x] **NEW: MonolithUI module** — IMPLEMENTED (2026-03-22). New module at `Source/MonolithUI/`. 42 actions in `ui` namespace (`ui_query` tool). 8 action classes: FMonolithUIActions (7), FMonolithUISlotActions (3), FMonolithUITemplateActions (8), FMonolithUIStylingActions (6), FMonolithUIAnimationActions (5), FMonolithUIBindingActions (4), FMonolithUISettingsActions (5), FMonolithUIAccessibilityActions (4).
- [x] **NEW: MonolithMesh module** — IMPLEMENTED (2026-03-27). New module at `Source/MonolithMesh/`. 46 actions in `mesh` namespace (`mesh_query` tool). 4 action classes: FMonolithMeshInspectionActions (12), FMonolithMeshSceneActions (8), FMonolithMeshSpatialActions (11), FMonolithMeshBlockoutActions (15). MeshCatalogIndexer added to MonolithIndex.
- [x] **Incremental indexer (3-layer architecture)** — IMPLEMENTED (2026-03-28). Startup hash-based delta engine, live AR callbacks on 2s timer, forced full reindex fallback. <1s startup with no changes, ~14K assets hashed in ~20ms.
- [x] **Schema v2 migration** — IMPLEMENTED (2026-03-28). Added `saved_hash` column (Blake3 FIoHash hex) to assets table, `schema_version` meta key. Auto-migrates via `PRAGMA table_info` check + `ALTER TABLE`.
- [x] **Live AR callbacks** — IMPLEMENTED (2026-03-28). Batched Asset Registry delegates (OnAssetsAdded, OnAssetsRemoved, OnAssetRenamed, OnAssetsUpdatedOnDisk) drained on 2s timer with dedup and transactional apply.
- [x] **Plugin content scope fix (bInstalled filter)** — FIXED (2026-03-28). Replaced `bInstalled` filter with explicit path enumeration. DrawCallReducer and NiagaraDestructionDriver now indexed. MeshCatalogIndexer paths corrected.
- [x] **MCP reindex action (incremental default + force param)** — IMPLEMENTED (2026-03-28). `monolith_reindex()` defaults to incremental mode; `force=true` triggers full wipe-and-rebuild.
- [x] **NEW: MonolithGAS module** — IMPLEMENTED (2026-03-29), TESTED (2026-03-30). 130 actions in `gas` namespace (`gas_query` tool). 53/53 tests PASS, 12 bugs fixed (8 commits: 32c86d7-5639dda). Key fixes: IGameplayTagsEditorModule API, EnsureAssetPathFree 3-tier guard, BS_BeingCreated suppression, AR pre-filter, GAS deep indexer. Conditional on `#if WITH_GBA`. Total plugin: 685 → 815 actions, 11 → 12 domains, 14 → 15 MCP tools.
