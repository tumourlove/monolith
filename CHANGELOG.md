# Changelog

All notable changes to Monolith will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

## [0.14.4] - 2026-04-24

### Fixed

- **Build error: missing `MonolithPackagePathValidator.h`** ([#35](https://github.com/tumourlove/monolith/issues/35)) — Header was added to working tree but not tracked by git when v0.14.3 was cut. Three modules (`MonolithAI`, `MonolithGAS`, `MonolithUI`) included it, causing `C1083` on clean builds. Now properly tracked. Reported by **@krojew**.

Full diff: [v0.14.3...v0.14.4](https://github.com/tumourlove/monolith/compare/v0.14.3...v0.14.4)

## [0.14.3] - 2026-04-24

### Added

- **HTTP bind retry with port probe** ([#33](https://github.com/tumourlove/monolith/pull/33)) — `Start()` now retries up to 5 times with exponential backoff when the port is held by a zombie editor instance. A TCP socket probe verifies the bind actually took, instead of trusting `StartAllListeners()` which can fail silently. New `Monolith.Restart` console command for manual recovery without restarting the editor. PR by **@MaxenceEpitech**.

- **Animation IK and bone control nodes** ([#34](https://github.com/tumourlove/monolith/pull/34)) — `add_anim_graph_node` now supports `TwoBoneIK`, `ModifyBone`, `LocalToComponentSpace`, and `ComponentToLocalSpace` node types. TwoBoneIK auto-exposes `EffectorLocation`, `JointTargetLocation`, and `Alpha` as input pins. New `expose_pins` parameter for manual pin control on any node type. PR by **@MaxenceEpitech**.

- **`add_variable_get` action** ([#34](https://github.com/tumourlove/monolith/pull/34)) — Places a `K2Node_VariableGet` in an ABP anim graph for reading AnimInstance member variables. Validates the variable exists on the skeleton class before spawning. Animation action count: 115 → 116. PR by **@MaxenceEpitech**.

### Fixed

- **Nested struct/array cross-package TObjectPtr serialization** ([#29](https://github.com/tumourlove/monolith/issues/29)) — `set_cdo_property` now fires recursive `PreEditChange`/`PostEditChangeChainProperty` on every nested sub-property containing object references, matching the Details panel's full edit cradle. Previously only the outer property got the notification, so inner `TObjectPtr` fields in structs and arrays would serialize as null on save. Also wired the cradle into `create_data_asset` and `create_blueprint` to fix creation-side `FOverridableManager` poisoning. Reported by **@danielandric**.

### Credits

- **@MaxenceEpitech** — PRs [#33](https://github.com/tumourlove/monolith/pull/33), [#34](https://github.com/tumourlove/monolith/pull/34) (HTTP retry + animation IK nodes). Two solid contributions in the same day.
- **@danielandric** — Issue [#29](https://github.com/tumourlove/monolith/issues/29) (nested property cradle). Thorough repro with the IMC DefaultKeyMappings case — made the fix straightforward.

Full diff: [v0.14.2...v0.14.3](https://github.com/tumourlove/monolith/compare/v0.14.2...v0.14.3)

## [0.14.0] - 2026-04-20

### Added

- **macOS (Apple Silicon) support** ([#24](https://github.com/tumourlove/monolith/pull/24)) — Monolith now builds and runs on macOS 15 / Apple Silicon under UE 5.7. Uses the existing Python proxy as the stdio↔HTTP bridge (the native C++ proxy remains Windows-only for now).
  - New `Scripts/monolith_proxy.sh` shell launcher with `python3`/`python` auto-detection and 3.8+ version gate (parity with `monolith_proxy.bat`).
  - `Scripts/monolith_proxy.py` now declares `from __future__ import annotations` so PEP 604 type syntax (`str | None`) works on Python 3.8+ — macOS ships 3.9 by default.
  - `MonolithNiagaraActions.cpp`: renamed local `NO` → `NodeObj` to dodge the `<objc/objc.h>` `#define NO __objc_no` macro leak that transitively reaches `ApplePlatformProcess.h` and broke compilation.
  - `Monolith.uplugin`: dropped ghost `MonolithISX` module reference (source lives outside master; release pipeline still strips ISX + SteamBridge on its own).
  - README + CONTRIBUTING updated to document macOS/Linux support and `.sh` launcher.
  - PR by **@MaxenceEpitech**.
  - **Note for macOS users:** this release ships Windows binaries only. Please clone the repo and build from source per `CONTRIBUTING.md` — the macOS build path is proven (all 17 Monolith dylibs compile on UE 5.7 / Apple Silicon). Prebuilt macOS dylibs will follow once a GitHub Actions macOS runner is wired up.

### Fixed

- **Editor crash on indexer pass with WorldPartition-enabled persistent level** ([#20](https://github.com/tumourlove/monolith/issues/20), fix [#21](https://github.com/tumourlove/monolith/pull/21)) — `LevelIndexer::IndexAsset` loaded level packages via `LoadPackage` to enumerate actors, which initializes `UWorldPartition` for WP-enabled levels (UE 5.4+ default). Because `LoadPackage` skips the editor's open-level flow, nothing tore down the subsystem, and when the batch loop marked the package for unload and GC eventually ran, `UWorldPartitionSubsystem::Deinitialize` asserted at `WorldPartitionSubsystem.cpp:507`. Fix uninitializes WorldPartition after `IndexActorsInLevel` and before `TryUnloadPackage(World)`. Affected every UE 5.4+ project with a WP-enabled persistent level and the default `bIndexLevels` setting. Reported and fixed by **@danielandric**.
- **Full Monolith rebuild on every UBT invocation after ZIP install** ([#22](https://github.com/tumourlove/monolith/issues/22), fix [#23](https://github.com/tumourlove/monolith/pull/23)) — PowerShell's `Compress-Archive` writes only DOS time (no NTFS or Unix extended timestamp), and DOS time is naked wall-clock with no timezone tag. `Expand-Archive` reinterprets the stored bytes as the user's local time, so a UTC+10-packaged ZIP extracted on UTC+0 landed with file mtimes ~10 hours in the user's future. UBT's `TargetMakefile.IsValidForSourceFiles` compares `ExternalDependency.LastWriteTimeUtc` against `Makefile.CreateTimeUtc`, so a future mtime on `Monolith.uplugin` tripped the check on every build and forced a full Monolith rebuild until the user's wall clock caught up. Affected every C++ user with auto-update on (default) and every C++ user installing from the ZIP manually. Fix mirrors POSIX tar's `--touch`: the auto-updater swap scripts (Windows + macOS/Linux) touch installed files post-xcopy, and `MonolithCoreModule::StartupModule` runs an idempotent self-heal that walks the plugin tree if `Monolith.uplugin` shows a future mtime (covers manual-install users). Microsoft acknowledged the underlying ZIP design flaw in [PowerShell/Microsoft.PowerShell.Archive#133](https://github.com/PowerShell/Microsoft.PowerShell.Archive/issues/133); their fix has not shipped. Reported and fixed by **@danielandric**.

### Changed

- **Release builds now run non-unity** — `Scripts/make_release.ps1` passes `-DisableUnity` to UBT so missing includes and unity-only symbol collisions get caught before they reach a public release.

### Credits

- **@danielandric** — PR [#21](https://github.com/tumourlove/monolith/pull/21) + issue [#20](https://github.com/tumourlove/monolith/issues/20) (WorldPartition indexer crash), PR [#23](https://github.com/tumourlove/monolith/pull/23) + issue [#22](https://github.com/tumourlove/monolith/issues/22) (ZIP mtime normalization). Thank you for two clean, well-diagnosed fixes in a single day.
- **@MaxenceEpitech** — PR [#24](https://github.com/tumourlove/monolith/pull/24) (macOS support — shell launcher, Python compat, Objective-C macro dodge, ghost module cleanup). Thanks for putting in the proof-of-work end-to-end build verification on Apple Silicon.

Full diff: [v0.13.2...v0.14.0](https://github.com/tumourlove/monolith/compare/v0.13.2...v0.14.0)

## [0.13.2] - 2026-04-19

### Hotfix

- **Pulled v0.13.1 — it accidentally shipped with some work-in-progress CommonUI stuff in `MonolithUI` that I forgot was sitting in my working tree.** Same #19 fix as 0.13.1, just rebuilt clean from a committed tree. Grab this one instead. The release script now refuses to run with a dirty working tree so this doesn't happen again.

## [0.13.1] - 2026-04-19 — DO NOT USE

Withdrawn. Use [v0.13.2](#0132---2026-04-19) — same fix, built from a clean tree. 0.13.1's release zip contained uncommitted WIP for unrelated `MonolithUI` work.

### Fixed

- **Indexer fatal crash: "Calling FinishCompilation is not allowed during PostCompilation"** ([#19](https://github.com/tumourlove/monolith/issues/19)) — sorry about this one, the fix I shipped for [#16](https://github.com/tumourlove/monolith/issues/16) in 0.13.0 caused the regression. I was calling `FAssetCompilingManager::FinishAllCompilation()` from inside `AsyncTask(ENamedThreads::GameThread, ...)` lambdas to avoid the reentrant compile crash, but those lambdas can land on the game thread while UE is already mid-`FTextureCompilingManager::PostCompilation`, and the engine fatals on that reentrance (`TextureCompiler.cpp:454`). Epic's own comment on the line above says workers should use `ExecuteOnGameThread` or tick-scheduled dispatch instead of `AsyncTask(GT)`. Done and done.
  - New `FMonolithCompilerSafeDispatch::RunOnGameThreadWhenCompilerIdle` helper — schedules work via `FTSTicker` (main tick loop, not task graph) and only fires when `FAssetCompilingManager::GetNumRemainingAssets() == 0`, with a 120s timeout safeguard.
  - All 8 asset-loading `AsyncTask(GT)` sites in `MonolithIndexSubsystem.cpp` rerouted through the helper: deep-index batch, dependency, level, data table, animation, gameplay tag, niagara, mesh catalog indexers.
  - All 5 `FinishAllCompilation()` calls inside indexer payloads deleted — the helper's idle-precondition is now the single point of compiler synchronization.
  - Reported by **@asafdubaaa**.

### Credits

- **@asafdubaaa** — issue [#19](https://github.com/tumourlove/monolith/issues/19) (caught the regression fast, thanks for the stack traces)

Full diff: [v0.13.0...v0.13.2](https://github.com/tumourlove/monolith/compare/v0.13.0...v0.13.2)

## [0.13.0] - 2026-04-18

### Added

- **MonolithAudio module shipped** — 81 actions across Phases 0-2: Sound asset CRUD (15), query/search (10), batch operations (10), Sound Cue graph building (21), MetaSound Builder API integration (25). Includes three power actions: `build_sound_cue_from_spec`, `build_metasound_from_spec`, `apply_audio_template`. MetaSound features gated on `WITH_METASOUND`. Phases 0-2 fully tested (28/28 PASS, 5 bugs fixed during test pass). Module had been completed + tested on 2026-04-08 but was not yet public.
- **Indexer RAM tier auto-detect** — `FMonolithMemoryHelper` now picks a memory budget and batch sizes based on installed RAM: 64+ GB → 32768 MB / deep=8 / post=4; 32+ GB → 16384 MB / deep=8 / post=4; 16 GB → 6144 MB / deep=4 / post=2; <16 GB → 3072 MB / deep=2 / post=1. Settings defaults changed to `0` (auto-detect sentinel) for `MemoryBudgetMB`, `DeepIndexBatchSize`, `PostPassBatchSize`. Users can still override via Project Settings > Monolith > Indexing > Performance. Tier logged once per editor session on first index run.

### Fixed

- **Indexer OOM + reentrant texture compiler crash on large projects** ([#16](https://github.com/tumourlove/monolith/issues/16)) — deep-index batches could exhaust physical RAM or re-enter `FTextureCompilingManager::ProcessAsyncTasks`, crashing the editor on large projects (>20 GB content). Fix introduces `FAssetCompilingManager::FinishAllCompilation()` guards before each batch, forced GC between batches, Slate-safe yields, emergency pause when available memory drops below 2 GB, and honors the async notification Cancel button. Shipped as PR [#17](https://github.com/tumourlove/monolith/pull/17) from **@alakangas**. Reported by **@MAYLYBY**.

### Changed

- **`bLogMemoryStats` default flipped to `false`** — opt in when debugging indexer memory behavior. Keeps shipped-project logs quiet.

### Credits

- **@alakangas** — PR [#17](https://github.com/tumourlove/monolith/pull/17) (indexer OOM + reentrant compiler crash fix)
- **@MAYLYBY** — issue [#16](https://github.com/tumourlove/monolith/issues/16) (detailed crash report that drove the fix and uncovered the low-spec regression we addressed with the RAM tier auto-detect)

Full diff: [v0.12.1...v0.13.0](https://github.com/tumourlove/monolith/compare/v0.12.1...v0.13.0)

## [0.12.1] - 2026-04-03

### Fixed

- **UE 5.7 compatibility** — resolved deprecated API usages that caused C2220 (warning-as-error) failures in non-unity builds ([#12](https://github.com/tumourlove/monolith/issues/12))
  - NavMesh: direct property access → `GetCellSize`/`SetCellSize`/`GetCellHeight`/`SetCellHeight`/`GetAgentMaxStepHeight`/`SetAgentMaxStepHeight` with `ENavigationDataResolution`
  - GAS: `EGameplayAbilityInstancingPolicy::NonInstanced` removed → legacy value cast
  - GAS: `StackingType` made private → reflection-based getter/setter
  - `FPackageName::DoesPackageExist` signature change (removed nullable param)
  - `FCollisionQueryParams::ClearIgnoredActors` → `ClearIgnoredSourceObjects`
  - `GetUsedTextures` simplified signature
  - `UMovieScene::GetBindings` const correctness, `FMovieSceneBinding::GetName` removal
- **Non-unity build** — fixed symbol collisions across 8 files (`VecToArr`, `ParseVectorArray`, `ParseStringArray`, `GetAssetPath` renamed to module-prefixed variants)
- **ComboGraph log category** — eliminated duplicate `DEFINE_LOG_CATEGORY_STATIC` across `#if`/`#else` branches; proper extern declaration in header
- **Uninitialized variables** — `FVector` locals now zero-initialized to suppress C4700/C6001

### Improved

- **StateTree schema resolution** — `ResolveStateTreeSchemaClass` searches multiple candidate paths including `/Script/GameplayStateTreeModule.*`
- **UI animation system** — `FindOrCreateWidgetAnimationBinding`, `FindOrCreateFloatTrack` helpers; transform/color component keyframes; proper `WidgetVariableNameToGuidMap` bookkeeping
- **UI param handling** — new `TryGetRequiredString`, `GetOptionalString`, `GetOptionalBool` helpers; duplicate-asset guard on widget creation

## [0.12.0] - 2026-04-01

Biggest release yet: +310 actions (815 to 1125). Two new domain modules (MonolithAI, MonolithLogicDriver), ComboGraph expansion. Python-to-C++ port of standalone tools eliminates Python as a runtime dependency. 14 skills (up from 12).

### Added

**MonolithAI (229 actions) — AI Asset Manipulation**

The most comprehensive AI tooling available through any MCP server. 229 actions across 15 categories, 24K lines C++, 30 files. Covers Behavior Trees, Blackboards, State Trees, EQS, Smart Objects, AI Controllers, AI Perception, Navigation, Runtime debugging, Scaffolds, Discovery, and Advanced categories.

- Crown jewels: `build_behavior_tree_from_spec` and `build_state_tree_from_spec` — create complete AI assets from a JSON specification
- BT nodes: tasks, decorators, services, composites — full CRUD with property configuration
- Blackboard: key CRUD with type configuration (bool, int, float, string, name, vector, rotator, enum, object, class)
- State Trees: states, transitions, conditions, tasks, evaluators — full structural editing
- EQS: queries, generators, tests — complete template management
- Smart Objects: definitions, slots, behaviors, tags, claim policies
- AI Controllers: Blueprint creation, BT/BB assignment, perception, pathfinding config
- Perception: sight, hearing, damage, touch — sense parameters, stimulus sources
- Navigation: mesh queries, path filtering, invokers, query filters, avoidance
- Runtime/PIE: start/stop BTs, query active nodes, inspect BB values, debug perception
- Scaffolding: patrol AI, combat AI, companion AI, guard AI — complete setup generation
- Conditional on `#if WITH_STATETREE`, `#if WITH_SMARTOBJECTS` (required). Optional: `#if WITH_MASSENTITY`, `#if WITH_ZONEGRAPH`

**MonolithLogicDriver (66 actions) — Logic Driver Pro State Machines**

Full integration with Logic Driver Pro marketplace plugin. Reflection-only (no direct C++ API linkage, version-agnostic). Conditional on `#if WITH_LOGICDRIVER` with 3-location Build.cs detection.

- SM CRUD: create, inspect, compile, delete, list, duplicate, validate
- Graph read/write: add states, transitions, configure properties, set transition rules
- Node config: state nodes, conduits, transition events, property editing via reflection
- Runtime/PIE: start, stop, query active states, trigger transitions, inspect variables
- `build_sm_from_spec`: one-shot state machine creation from JSON specification
- JSON spec: export, import, validate, diff for templating and version control
- Scaffolding: door controller, health system, AI patrol, dialogue, elevator, puzzle, inventory
- Components: add/configure Logic Driver components on actors
- Text graph: text-based visualization for debugging
- Discovery: list node classes, state types, templates

**MonolithComboGraph expanded (12 → 13 actions)**

- Added `auto_layout` action for combo graph node arrangement

**Standalone C++ Tools (Python-to-C++ port)**

Two standalone C++ executables replace the Python scripts. Zero Python dependency at runtime, zero UE dependency, instant startup.

- **`monolith_proxy.exe`** (473KB) — MCP stdio-to-HTTP proxy, replaces `Scripts/monolith_proxy.py`. Full feature parity: JSON-RPC, health poll, tool dedup, editor query splitting, action allowlist/denylist. Built with WinHTTP + nlohmann/json. Source: `Tools/MonolithProxy/monolith_proxy.cpp` (775 lines).
- **`monolith_query.exe`** (1.8MB) — Offline DB query tool, replaces `monolith_offline.py` AND `MonolithQueryCommandlet`. 14 actions: 9 source (search_source, read_source, find_callers, find_callees, find_references, get_class_hierarchy, get_module_info, get_symbol_context, read_file) + 5 project (search, find_by_type, find_references, get_stats, get_asset_details). Built with sqlite3 amalgamation. Source: `Tools/MonolithQuery/monolith_query.cpp` (1080 lines).
- Python scripts remain as deprecated fallbacks for environments without the exe
- `MonolithQueryCommandlet` deleted — standalone exe is faster (instant startup vs 6+ second UE engine load)
- 2 new Claude Code skills: `unreal-logicdriver`, `unreal-combograph`

### Changed

- Total: 815 → 1125 actions across 15 modules (was 13), exposed through 18 MCP tools (was 15)
- Blueprint: 86 → 88 actions
- ComboGraph: 12 → 13 actions
- Skills: 12 → 14 bundled with plugin
- `.mcp.json` recommended proxy config changed from Python script to `monolith_proxy.exe`
- Python is no longer required for any core functionality (only for optional project C++ source indexing via `Scripts/index_project.py`)

### Removed

- `MonolithQueryCommandlet` — replaced by standalone `monolith_query.exe`

---

## [0.11.0] - 2026-03-30

Massive expansion: +372 actions (443 → 815). Three new modules (MonolithMesh, MonolithGAS, MonolithUI) plus MonolithBABridge integration. MCP auto-reconnect proxy for Claude Code. Optional module system for third-party plugin integrations. Automated release builds with MONOLITH_RELEASE_BUILD env var. 12 skills (up from 9).

### Added

**MCP Auto-Reconnect Proxy**

Claude Code has a known issue where HTTP MCP sessions die permanently when the Unreal Editor restarts — forcing you to restart Claude Code every time you recompile, crash, or close the editor. Monolith now ships with a **stdio-to-HTTP proxy** (`Scripts/monolith_proxy.py`) that eliminates this entirely.

**Who it's for:** Claude Code users. Cursor and Cline handle reconnection natively and don't need this.

**What it does:**
- Keeps your MCP session alive across editor restarts — zero manual intervention
- Background health poll auto-detects when the editor comes up or goes down
- Sends `notifications/tools/list_changed` so Claude Code refreshes its tool list automatically
- When the editor is down, tool calls return graceful errors instead of killing the session
- When the editor comes back, the next tool call just works

**How to use it:** Update your `.mcp.json` to use the proxy instead of direct HTTP:

```json
{
  "mcpServers": {
    "monolith": {
      "command": "Plugins/Monolith/Scripts/monolith_proxy.bat",
      "args": []
    }
  }
}
```

Requires Python 3.8+ (stdlib only, no pip install). The `.bat` launcher auto-finds Python. No Python? The direct HTTP config still works — you'll just need to restart Claude Code after editor restarts.

- `Scripts/monolith_proxy.py` — stdio-to-HTTP proxy (pure Python, zero dependencies)
- `Scripts/monolith_proxy.bat` — Windows launcher that auto-detects Python
- `Templates/.mcp.json.proxy.example` — ready-to-copy config template

**MonolithMesh (242 actions — 197 core + 45 experimental town gen)**

The biggest module in Monolith. 22 capability tiers covering the entire spatial side of Unreal level design:

- **Core (197 actions):** Mesh inspection and comparison. Full actor CRUD with scene manipulation. Physics-based spatial queries (raycasts, sweeps, overlaps, navmesh) that work in-editor without PIE. Level blockout workflow with auto-matching and atomic replacement. GeometryScript mesh operations (boolean, simplify, remesh, LOD gen, UV projection, mirroring). Horror spatial analysis — sightlines, hiding spots, ambush points, choke points, zone tension, pacing curves, dead-end detection. Accessibility validation with A-F grading. Lighting analysis with dark corner detection and mood-based placement suggestions. Audio/acoustics with Sabine RT60 reverb estimation, stealth maps, quiet path routing, AI hearing simulation. Performance budgeting with region analysis, overdraw detection, shadow cost analysis. Decal placement with Catmull-Rom paths and 5 horror storytelling presets. Level design tools for lights, volumes, sublevels, prefabs, splines, HISM instancing. Tech art pipeline for mesh import, LOD gen, texel density, collision authoring, lightmap analysis. Context-aware prop scatter on any surface (floors, shelves, tables, walls, ceilings) with room disturbance levels. Procedural geometry — parametric furniture (15 types), horror props (7 types), architectural structures, multi-story buildings, mazes (3 algorithms), pipes, mesh fragmentation, terrain patches. Genre preset system (storytelling patterns, acoustic profiles, tension profiles) for any game type. Encounter design with patrol routes, safe room evaluation, scare sequence generation, and accessibility intensity validation. Quality and polish tools including naming conventions, batch rename, proxy mesh generation, HLOD, texture budgets, composition framing, and monster reveal scoring.
- **Town Gen (45 actions) — Work-in-progress:** Procedural floor plans, building facades, roofs, city blocks, spatial registry, auto-volumes, terrain/foundations, architectural features, room furnishing, debug views. Disabled by default (`bEnableProceduralTownGen` in Editor Preferences). Known geometry issues — wall misalignment, room separation. Very much a WIP; unless you're willing to dig in and help improve it, it's best left alone for now.

**MonolithGAS (130 actions)**

Complete Gameplay Ability System integration across 10 categories. All 4 implementation phases complete, 53/53 tests PASS.

- **Abilities (14):** Create, grant, revoke, activate, inspect, list, configure activation policies, cooldowns, costs, tags
- **Attributes (16):** AttributeSet CRUD for both C++ and Blueprint-based sets (Blueprint mode requires optional GBA plugin). Define attributes with min/max clamping, initialize from DataTables
- **Effects (18):** Gameplay Effect authoring with modifiers, duration policies, stacking, period, conditional application, granted abilities, immunity
- **ASC (12):** Ability System Component management — grant/revoke abilities, apply/remove effects, query active abilities and effects, initialize from DataTables
- **Tags (10):** Gameplay Tag utilities — query, add, remove, check tag containers, hierarchical queries
- **Cues (10):** Gameplay Cue management — create, trigger, inspect cues for audio/visual feedback
- **Targeting (12):** Target data generation, targeting tasks, trace-based and actor-based targeting
- **Input (8):** Input binding for ability activation via Enhanced Input
- **Inspect (16):** Runtime debugging — query active effects, ability states, attribute values, cooldown status, blocking tags
- **Scaffold (14):** Generate complete GAS setups from templates — character ability sets, attribute configurations, effect libraries, common gameplay patterns. Accessibility-focused infinite-duration GEs for reduced difficulty modes

**MonolithUI (42 actions)**

Widget Blueprint CRUD with a template system that scaffolds complete, functional UI. Pre-built templates for HUDs, main menus, settings screens, dialog boxes, loading screens, inventory grids, save/load menus, and toast notifications. Styling system for consistent theming. Widget animation support. Game-level scaffolding for settings, save systems, audio managers, input rebinding, and accessibility configurations. Built-in accessibility audit with colorblind modes and text scaling.

**Optional Module System**

- **MonolithBABridge** — Blueprint Assist integration via IModularFeatures bridge pattern. `auto_layout` action optionally delegates to BA's superior formatter when the marketplace plugin is installed. Compiles as empty shell when BA is absent — zero impact on users who don't own it.
- **Conditional GBA support** — MonolithGAS detects the GBA (Blueprint Attributes) plugin at compile time. Blueprint AttributeSet creation requires GBA; all other 130 GAS actions work without it.
- **`Build.cs` probe pattern** — Optional dependencies detected via `Directory.Exists()` at build time. `WITH_FOO=1` or `=0` preprocessor defines. No DLL load failures, no error dialogs. Full tutorial on [[Optional Modules]] wiki page.

**Skills (+3)**

- `unreal-mesh` — Mesh inspection, spatial queries, blockout, procedural geometry, horror/accessibility workflows
- `unreal-ui` — Widget Blueprint CRUD, templates, styling, accessibility workflows
- `unreal-gas` — Gameplay Ability System workflows: abilities, effects, attributes, ASC, tags, cues

### Changed

- **Total** — Action count 443 -> 815 (across 13 modules, exposed through 15 MCP tools)
- **Skills** — 9 -> 12 Claude Code skills bundled with plugin
- **Modules** — 10 -> 13 (added MonolithMesh, MonolithGAS, MonolithBABridge)

### Fixed

- SQLite multi-statement DDL was silently only executing the first statement, leaving source index schema incomplete (thanks @fp12 / ArcstoneGames)
- Plugin path resolution now uses `IPluginManager` instead of hardcoded `Plugins/Monolith/` — works for nested install paths like `Plugins/Marketplace/Monolith/` (thanks @fp12 / ArcstoneGames)

### Contributors

Thanks to **@fp12** (ArcstoneGames) for two solid PRs this release — real bugs caught and clean fixes shipped.

## [0.10.0] - 2026-03-25

Massive expansion across all modules: +153 actions (290 to 443). Niagara nearly doubles with 31 new actions and 10 bug fixes. Blueprint and Animation get major expansions. Material function suite rounds out the material pipeline.

### Added

**Niagara (+31, 65 -> 96)**

- `add_dynamic_input` / `remove_dynamic_input` / `set_dynamic_input_value` / `get_dynamic_input_info` / `search_dynamic_inputs` -- full dynamic input CRUD
- `add_event_handler` / `remove_event_handler` / `list_event_handlers` -- event handler management
- `add_simulation_stage` / `remove_simulation_stage` / `list_simulation_stages` -- simulation stage CRUD
- `create_npc_system` / `add_npc_behavior` / `get_npc_info` / `set_npc_property` / `list_npc_templates` -- NPC particle system support
- `create_effect_type` / `get_effect_type_info` / `set_effect_type_property` -- effect type CRUD
- `list_available_renderers` / `set_renderer_mesh` / `configure_ribbon` / `configure_subuv` -- renderer helpers
- `diff_systems` -- diff two Niagara systems side-by-side
- `save_emitter_as_template` -- save an emitter as a reusable template
- `clone_module_overrides` -- clone module overrides between emitters
- `preview_system` -- trigger a system preview in the editor
- `get_available_parameters` / `get_module_output_parameters` -- parameter introspection
- `rename_emitter` -- rename an emitter within a system
- `get_emitter_property` -- read a single emitter property
- `export_system_spec` expanded -- now includes event handlers, sim stages, static switches, and dynamic inputs

**Blueprint (+20, 66 -> 86)**

- `auto_layout` -- Modified Sugiyama graph layout algorithm for automatic node arrangement
- 22 new actions including expanded node types, resolve improvements, DataTable field resolution
- `batch_execute` improvements for bulk operations

**Animation (+41, 74 -> 115)**

- 41 new actions covering expanded montage editing, blend space manipulation, skeletal mesh queries, and animation asset management

**Material (+9, 48 -> 57)**

- `create_material_function` / `build_function_graph` / `get_function_info` -- material function full suite
- `batch_set_material_property` / `batch_recompile` -- batch operations
- `import_texture` -- image file import as UTexture2D
- `list_material_instances` / `replace_expression` / `rename_expression` -- additional utilities

**Project (+2, 5 -> 7)**

- 2 new project index actions for deeper asset discovery

### Fixed

**Niagara (10 fixes)**

- `batch_execute` reads now return data correctly instead of silently succeeding
- Type validation on module inputs catches mismatched types before crash
- GUID collision fix when duplicating emitters with shared module references
- ShapeLocation race condition on freshly-created emitters with shape DIs
- Color curve fan-out when multiple emitters share the same curve keys
- NPC namespace routing fixed for NPC-specific actions
- `move_module` now preserves parameter overrides during reorder
- 3 test-driven fixes from Phase 1-6 testing

**Material (6 fixes)**

- `AssetTagsFinalized` renamed to match UE 5.7 API change
- 5 missing includes that caused compile failures on clean builds

**Blueprint (5 fixes)**

- DataTable UDS field resolution -- match by display name
- `resolve_node` expanded -- Self, MacroInstance, Return, generic fallback
- K2Node generic fallback -- strip U prefix for UObject name lookup
- Simplified templates -- removed broken function refs
- Code review cleanup -- dead code, magic numbers, perf, correctness

### Changed

- **Niagara** -- Action count 65 -> 96
- **Blueprint** -- Action count 66 -> 86
- **Animation** -- Action count 74 -> 115
- **Material** -- Action count 48 -> 57
- **Project** -- Action count 5 -> 7
- **Total** -- Action count 290 -> 443 (across 10 modules)

## [0.9.0] - 2026-03-19

Major feature expansion: +69 actions across Blueprint, Material, Niagara, and Animation. IKRig, IK Retargeter, Control Rig, and AnimBP structural write support. Full Material instance CRUD. Niagara dynamic inputs, event handlers, and simulation stages. 60 bug fixes. 220 → 290 actions total.

### Added

**Blueprint (+20, 47 → 67)**

- `batch_execute` — dispatch multiple Blueprint operations in a single call
- `resolve_node` — resolve a node reference to its target (function, variable, etc.)
- `search_functions` — search functions and events by name across a Blueprint
- `get_node_details` — full detail dump for a single node (pins, defaults, metadata)
- `add_nodes_bulk` — add multiple nodes to a graph in one call
- `connect_pins_bulk` — connect multiple pin pairs in one call
- `set_pin_defaults_bulk` — set multiple pin default values in one call
- `scaffold_interface_implementation` — auto-generate stub event nodes for an unimplemented interface
- `add_timeline` — add a Timeline node to a graph
- `add_event_node` — add a named event node to a graph
- `add_comment_node` — add a comment box to a graph
- `get_function_signature` — return param list and return type for a Blueprint function
- `get_blueprint_info` — comprehensive Blueprint summary (class, interfaces, components, variable count)
- `get_event_dispatcher_details` — full detail for a single event dispatcher
- `remove_event_dispatcher` — remove an event dispatcher from a Blueprint
- `set_event_dispatcher_params` — change the parameter signature of an event dispatcher
- `validate_blueprint` (enhanced) — now detects unimplemented interfaces and duplicate events
- `promote_pin_to_variable` — promote a pin's value to a Blueprint variable
- `add_replicated_variable` — add a replicated variable with configurable RepNotify
- `add_node` (extended) — now supports cast node creation (`CastTo<ClassName>`)

**Material (+22, 25 → 47)**

- `auto_layout` — auto-arrange expression nodes in the material graph
- `duplicate_expression` — duplicate an expression node in-place
- `list_expression_classes` — list all available material expression class names
- `get_expression_connections` — return all connections into/out of an expression
- `move_expression` — move an expression node to a new graph position
- `get_material_properties` — return material-level properties (blend mode, shading model, etc.)
- `get_instance_parameters` — list all parameter overrides on a material instance
- `set_instance_parameters` — set multiple parameters on a material instance in one call
- `set_instance_parent` — reparent a material instance to a different material
- `clear_instance_parameter` — clear a parameter override on a material instance (revert to parent)
- `save_material` — explicitly save a material asset (bypass auto-save)
- `update_custom_hlsl_node` — update the HLSL code or description on a CustomHLSL expression
- `replace_expression` — swap an expression node for a different type, preserving connections
- `get_expression_pin_info` — return pin names, types, and connection state for an expression
- `rename_expression` — rename an expression node's parameter name
- `list_material_instances` — find all material instances derived from a material
- `create_material_function` — create a new UMaterialFunction asset
- `build_function_graph` — build a material function's node graph from a declarative spec
- `get_function_info` — return inputs, outputs, and description of a material function
- `batch_set_material_property` — set a property on multiple materials in one call
- `batch_recompile` — recompile multiple materials in one call
- `import_texture` — import an image file as a UTexture2D asset

**Niagara (+17, 47 → 64)**

- `get_system_summary` — high-level system overview (emitter count, renderer count, param count)
- `get_emitter_summary` — high-level emitter overview (module count, renderer count, sim target)
- `list_emitter_properties` — list all editable UPROPERTY fields on an emitter asset
- `get_module_input_value` — read the current value of a single module input
- `configure_curve_keys` — set the full key list on a curve data interface in one call
- `configure_data_interface` — set multiple properties on a data interface in one call
- `duplicate_system` — duplicate a Niagara system asset to a new path
- `set_fixed_bounds` — set fixed world-space bounds on a Niagara system
- `set_effect_type` — assign an effect type asset to a Niagara system
- `create_emitter` — create a standalone Niagara emitter asset from scratch
- `export_system_spec` — export a system's full spec as JSON (reverse of `create_system_from_spec`)
- `add_dynamic_input` — add a dynamic input module to a module's input slot
- `set_dynamic_input_value` — set an input value on a dynamic input module
- `search_dynamic_inputs` — search available dynamic input scripts by keyword
- `add_event_handler` — add an event handler stage to an emitter
- `validate_system` — validate system for GPU+Light renderer conflicts, missing materials, and bounds warnings
- `add_simulation_stage` — add a simulation stage to a GPU emitter

**Animation (+12, 62 → 74)**

- `get_ikrig_info` — return IKRig asset info: chains, goals, solvers, retarget root
- `add_ik_solver` — add a solver (PBIK, TwoBone, etc.) to an IKRig
- `get_retargeter_info` — return IK Retargeter asset info: source/target rigs, chain mappings
- `set_retarget_chain_mapping` — set or update a chain mapping on an IK Retargeter
- `get_control_rig_info` — return Control Rig hierarchy: bones, controls, nulls, curves
- `get_control_rig_variables` — list variables on a Control Rig Blueprint
- `add_control_rig_element` — add a bone, control, or null to a Control Rig hierarchy
- `get_abp_variables` — list variables defined in an Animation Blueprint
- `get_abp_linked_assets` — list assets linked to an Animation Blueprint (skeletons, rigs, etc.)
- `add_state_to_machine` — add a new state to an AnimBP state machine
- `add_transition` — add a transition between two states in a state machine
- `set_transition_rule` — set the condition expression on a state machine transition

### Fixed

**Blueprint (21 fixes)**

- 5 crash fixes: null graph reference, invalid pin access on removed nodes, blueprint-not-compiled guard, interface scaffold on abstract classes, cast node creation with missing target class
- 7 logic bugs: `get_functions` missing latent function flags, `find_nodes_by_class` incorrect prefix handling, `connect_pins` direction mismatch silent failure, `remove_node` orphaned connections, `get_event_dispatchers` missing param types, `validate_blueprint` false-positive on native interfaces, `get_graph_data` stale node references after compile
- 9 UX improvements: clearer error messages for invalid pin names, node class alias expansion in `add_node`, bulk op partial-success reporting, better param validation messages, schema enrichment for all 20 new actions

**Material (11 fixes)**

- 3 bugs: `build_function_graph` node class resolution for function-context expressions, `connect_expressions` direction detection when both nodes have same-named pins, `get_material_parameters` missing static switch params on instanced materials
- 3 UX: `list_material_instances` now recurses through instance chains, `get_compilation_stats` includes VS/PS instruction counts, `set_instance_parameter` accepts both short and full parameter names
- 5 minor: null-safety guards in expression walker, consistent use of `PostEditChangeProperty` across all write actions, `save_material` marks package dirty before save, `import_texture` sets sRGB correctly for normal maps, `batch_recompile` returns per-asset results

**Niagara (16 fixes)**

- 2 crash fixes: `configure_data_interface` null DI reference on freshly-created emitters, `add_event_handler` accessing uninitialized event receiver
- 5 bugs: `get_module_input_value` mismatched override vs default value for bound inputs, `set_dynamic_input_value` namespace aliasing for dynamic input params, `validate_system` false-positive on CPU+Light (only GPU+Light is invalid), `export_system_spec` missing user parameter defaults, `add_simulation_stage` not calling `RebuildEmitterNodes` after add
- 9 UX: `get_system_summary` includes compile status, `list_emitter_properties` groups by category, `configure_curve_keys` validates key ordering, `duplicate_system` deep-copies override table, consistent emitter param naming across all actions, `set_fixed_bounds` validates axis order, `search_dynamic_inputs` supports multi-word queries, `get_emitter_summary` includes module names, `add_event_handler` returns handler index

**Animation (12 fixes)**

- 1 crash fix: `add_ik_solver` null pointer when IKRig asset has no chain defined yet
- 6 bugs: `get_ikrig_info` missing retarget root bone, `set_retarget_chain_mapping` overwrote existing mappings instead of merging, `get_control_rig_info` excluded null-type elements, `get_abp_linked_assets` missed indirect skeleton links via pose asset references, `add_state_to_machine` duplicate state name collision not detected, `set_transition_rule` lost existing conditions on complex rule expressions
- 5 UX: `get_ikrig_info` now includes goal offsets and weight settings, `get_retargeter_info` includes auto-map status per chain, `add_transition` accepts both state names and state indices, `get_abp_variables` includes type info and default values, `add_control_rig_element` returns new element's full path

### Changed

- **Blueprint** — Action count 47 → 67
- **Material** — Action count 25 → 47
- **Niagara** — Action count 47 → 64
- **Animation** — Action count 62 → 74
- **Total** — Action count 220 → 290

## [0.8.0] - 2026-03-15

Native C++ source indexer, marketplace plugin content indexing, CDO property reader, and project C++ source indexing. Community PRs from NRG-Nad. 219 → 220 actions total.

### Added

**Source — Native C++ indexer (replaces Python/tree-sitter)**

- **MonolithSource** — Completely rewrote the source indexer in native C++ (4,119 lines). Eliminates the Python/tree-sitter dependency entirely — engine source indexing now works out of the box with no Python install. Two indexing modes: full (entire engine source tree) and incremental (project C++ source only, much faster).
- **MonolithSource** — New `MonolithQueryCommandlet` for offline source queries from the command line, without launching the full editor.
- **MonolithSource** — New `trigger_project_reindex` action: triggers an incremental re-index of project C++ source from within an MCP session. **220 total actions.**

**Index — Marketplace plugin content**

- **MonolithIndex** — Auto-discovers installed marketplace and Fab plugins via `IPluginManager` and indexes their content alongside project assets. Opt out per-plugin or globally with the new `bIndexMarketplacePlugins` toggle in plugin settings.

**Index — Configurable content paths**

- **MonolithIndex** — `AdditionalContentPaths` setting: add arbitrary content paths (e.g. external asset packs, shared libraries) to the project index. `GetIndexedContentPaths()` and `IsIndexedContentPath()` helpers available for tools that need path-aware filtering.

**Blueprint — CDO property reader (#5)**

- **MonolithBlueprint** — New `get_cdo_properties` action: reads `UPROPERTY` defaults from any Blueprint CDO or `UObject` asset. Works on any class with a valid CDO. Credit: **NRG-Nad** ([#5](https://github.com/tumourlove/monolith/pull/5)).
- **MonolithIndex** — New `FDataAssetIndexer`: deep-indexes DataAsset subclasses. 15 registered indexers total (up from 14). `bIndexDataAssets` toggle in plugin settings. Credit: **NRG-Nad** ([#5](https://github.com/tumourlove/monolith/pull/5)).

**Source — Project C++ source indexing (#6)**

- **MonolithSource** — `Scripts/index_project.py`: indexes project C++ source into `EngineSource.db` alongside engine symbols, enabling `find_callers`/`find_callees`/`get_class_hierarchy` across project code. Incremental pipeline with `_finalize()` and `load_existing_symbols()` — only changed files are reprocessed. Source DB grows from ~1.8 GB (engine only) to ~3.4 GB with a full project. Credit: **NRG-Nad** ([#6](https://github.com/tumourlove/monolith/pull/6)).

### Fixed

- **MonolithSource** — Improved error handling and recovery throughout the source indexer pipeline.
- **MonolithNiagara** — Resolved 5 bugs in DI handling, static switch inputs, SimTarget changes, and renderer class naming.

### Changed

- **MonolithSource** — Source indexer no longer requires Python. The C++ indexer runs natively inside the editor on startup. Python (`index_project.py`) is still available for project C++ source indexing as a separate optional step.
- **MonolithBlueprint** — Action count 46 → 47 (`get_cdo_properties`).
- **Total** — Action count 219 → 220 (`trigger_project_reindex`).

## [0.7.3] - 2026-03-15

Blueprint module fully realized (6 → 46 actions). Niagara HLSL module creation implemented. Major Niagara, Material, and MCP reliability fixes across all modules. 217 → 218 actions total.

### Added

**Blueprint — Write Actions (40 new)**

- **Blueprint — Variable CRUD (7):** `add_variable`, `remove_variable`, `set_variable_default`, `set_variable_type`, `set_variable_flags` (exposed, editable, replicated, transient), `rename_variable`, `get_variable_details`
- **Blueprint — Component CRUD (6):** `add_component`, `remove_component`, `set_component_property`, `get_components`, `get_component_details`, `reparent_component`
- **Blueprint — Graph Management (9):** `add_function_graph`, `remove_function_graph`, `add_macro_graph`, `remove_macro_graph`, `add_event_graph`, `remove_event_graph`, `get_functions`, `get_event_dispatchers`, `get_construction_script`
- **Blueprint — Node & Pin Operations (6):** `add_node`, `remove_node`, `connect_pins`, `disconnect_pins`, `get_pin_info`, `find_nodes_by_class`
- **Blueprint — Compile & Create (5):** `compile_blueprint`, `create_blueprint`, `reparent_blueprint`, `add_interface`, `remove_interface`
- **Blueprint — Read Actions expanded (4 new):** `get_parent_class`, `get_interfaces`, `get_construction_script` (graph data), `get_component_details`

**Blueprint — `add_node` usability:**
- Common node class aliases (`CallFunction`, `VariableGet`, `VariableSet`, `Branch`, `Sequence`, `ForEach`) resolve without full K2Node_ prefix
- Automatic `K2_` prefix fallback when a bare function name doesn't resolve

**Niagara — HLSL module authoring (2 new):**

- **Niagara** — `create_module_from_hlsl`: creates a standalone `UNiagaraScript` asset (module usage) with a CustomHlsl node, typed ParameterMap I/O pins, and user-defined input/output pin declarations. Supports CPU and GPU sim targets. Inputs are exposed as overridable parameters on the FunctionCall — `get_module_inputs` and `set_module_input_value` work on the result.
- **Niagara** — `create_function_from_hlsl`: same as above in function usage context — for reusable HLSL logic called from other modules. Direct typed pin wiring (no ParameterMap wrapper).
- **Niagara** — Dot validation for I/O pin names: dotted names (e.g. `Module.Color`) are now rejected with a clear error at creation time, with usage-specific guidance (modules: write via ParameterMap in HLSL body; functions: use bare names). Prevents cryptic HLSL compiler errors downstream.

**Niagara — System controls (2 new):**

- **Niagara** — `set_system_property`: sets a system-level property (e.g. `WarmupTime`, `bDeterminism`) via UE reflection. No hardcoded property list — any `UPROPERTY` on `UNiagaraSystem` is settable.
- **Niagara** — `set_static_switch_value`: sets a static switch input value on a Niagara module. Static switches control compile-time code paths in the Niagara module stack.

**Niagara — Discovery (2 new):**

- **Niagara** — `list_module_scripts`: searches available Niagara module scripts by keyword. Returns matching asset paths — useful for finding engine modules to add via `add_module`.
- **Niagara** — `list_renderer_properties`: lists editable UPROPERTY fields on a renderer via reflection. Params: `asset_path`, `emitter`, `renderer`. Returns property names, types, and current values.

**Niagara — Diagnostics (1 new):**

- **Niagara** — `get_system_diagnostics`: returns compile errors, warnings, renderer/SimTarget incompatibility flags, GPU + dynamic bounds warnings, and per-script stats (op count, register count, compile status). Also exposed `CalculateBoundsMode` in `set_emitter_property`.

**MCP — Client usability:**

- **MCP** — `tools/list` now embeds per-action param schemas for all actions at session start. AI clients see full param documentation (names, types, required/optional) without calling `monolith_discover()` first.
- **MCP** — Registry-level required param validation: missing required params return a clear error listing which params were provided vs which are required, before the handler is even called.

**Offline CLI:**

- **Core** — `Saved/monolith_offline.py`: pure Python (stdlib, zero deps) read-only CLI that queries `EngineSource.db` and `ProjectIndex.db` directly when the editor is not running. 14 actions across 2 namespaces: `source` (9 actions, mirrors `source_query`) and `project` (5 actions, mirrors `project_query`). Fallback for when MCP/editor is unavailable.

### Fixed

**Niagara — Emitter lifecycle:**

- **Niagara** — `create_system` + `add_emitter`: emitters added via `add_emitter` did not persist in the saved asset. Fixed by replacing raw `System->AddEmitterHandle()` with `FNiagaraEditorUtilities::AddEmitterToSystem()`, which calls `RebuildEmitterNodes()` + `SynchronizeOverviewGraphWithSystem()`. `SavePackage` now called in both `HandleCreateSystem` and `HandleAddEmitter`.
- **Niagara** — `create_system_from_spec`: was failing with `failed_steps:1` on any spec with modules. Fixed by adding synchronous `RequestCompile(true)` + `WaitForCompilationComplete()` after each emitter add in the spec flow, before module operations begin. Failed sub-operations now report in an `"errors"` array instead of silently incrementing a counter.
- **Niagara** — `set_emitter_property` SimTarget change caused "Data missing please force a recompile" in the editor. Raw field assignment on `SimTarget` bypassed `MarkNotSynchronized`, so `RequestCompile(false)` saw an unchanged hash and skipped compilation. Fixed: now calls `PostEditChangeVersionedProperty` + `RebuildEmitterNodes` + `SynchronizeOverviewGraphWithSystem` after `SimTarget`, `bLocalSpace`, and `bDeterminism` changes.
- **Niagara** — `list_emitters` was missing the emitter GUID in its output. Added `"id": Handle.GetId().ToString()` — provides a stable round-trip token for subsequent operations.

**Niagara — Parameter namespace correctness:**

- **Niagara** — `set_module_input_value` and `set_module_input_binding` were passing the stripped short name to `FNiagaraParameterHandle::CreateAliasedModuleParameterHandle` instead of the full `Module.`-prefixed name from `In.GetName()`. This caused namespace warnings on every subsequent Niagara compile. Both actions now use the full name.
- **Niagara** — `FindEmitterHandleIndex` accepted numeric string indices (`"0"`, `"1"`) as a last-resort fallback. `list_emitters` returns `"index"` for each emitter — this lets you pass that index directly instead of having to remember the emitter name.

**Niagara — Module input coverage:**

- **Niagara** — `get_module_inputs` and `set_module_input_value` now work with CustomHlsl modules. When `GetStackFunctionInputs` returns empty (no `Module.`-prefixed map entries, as is the case for CustomHlsl scripts), both actions fall back to reading typed pins directly from the FunctionCall node.
- **Niagara** — `get_module_inputs` now returns actual `FRichCurve` key data for DataInterface curve inputs, instead of just the DI class name.
- **Niagara** — `get_module_inputs` now correctly deserializes `LinearColor` and vector default values from their string-serialized JSON fallback. Previously returned zeroed values for these types.
- **Niagara** — `set_module_input_di` and `get_di_functions` now auto-resolve DI class names — both `NiagaraDataInterfaceCurve` and `UNiagaraDataInterfaceCurve` are accepted (U prefix stripped/added as needed).

**Niagara — Renderer:**

- **Niagara** — `list_renderers` now returns the short renderer class name in the `type` field (e.g. `SpriteRenderer`) instead of the full UClass path.

**Material — Editor integration:**

- **Material** — `set_expression_property` was calling `PostEditChange()` with no arguments, which didn't trigger `MaterialGraph->RebuildGraph()`. Now calls `PostEditChangeProperty(FPropertyChangedEvent(Prop))` with the actual property — changes reflect in the editor display without a manual recompile.
- **Material** — `build_material_graph` now auto-recompiles on success. Response includes `"recompiled": true`.
- **Material** — `delete_expression`, `connect_expressions`, and `disconnect_expression` now wrap operations in `PreEditChange`/`PostEditChange` for correct undo history and editor update.
- **Material** — `set_material_property`, `create_material`, `delete_expression`, and `connect_expressions` now call `Mat->PreEditChange(nullptr)` + `Mat->PostEditChange()` to push changes through the material graph system.
- **Material** — `disconnect_expression`: added optional `input_name`/`output_name` params for targeted disconnection. Previously always disconnected all connections on the expression — now supports disconnecting a specific pin pair while leaving others intact.

**Blueprint:**

- **Blueprint** — `add_node` now resolves common node class aliases (`CallFunction`, `VariableGet`, `VariableSet`, `Branch`, `Sequence`, `ForEach`) and automatically tries the `K2_` prefix for function call nodes when the bare name doesn't resolve. Previously failed with a class-not-found error on all common node types.

**Core — Asset loading:**

- **Core** — `LoadAssetByPath` now queries `IAssetRegistry::GetAssetByObjectPath()` + `FAssetData::GetAsset()` first, falling back to `StaticLoadObject` only if the Asset Registry has no record. Prevents stale `RF_Standalone` ghost objects from shadowing assets that were deleted and recreated in the same editor session.

### Changed

- **Blueprint** — Action count 6 → 46. Module refactored from one file into six focused source files: Actions (core read), Variables, Components, Graph, Nodes, Compile.
- **Niagara** — Action count 41 → 47. Added `set_system_property`, `set_static_switch_value`, `list_module_scripts`, `list_renderer_properties`, `get_system_diagnostics`, `create_module_from_hlsl`, `create_function_from_hlsl`. Param aliases added (`module_node`/`module_name`/`module`, `input`/`input_name`, `property`/`property_name`, `class`/`renderer_class`/`renderer_type`).
- **Total** — Action count 177 → 218

## [0.7.2] - 2026-03-13

### Fixed

- **Niagara** — `set_module_input_value`, `set_module_input_binding`, and `set_curve_value` silently defaulted to `GetFloatDef()` when input name didn't match any module input, creating orphaned override entries in the parameter map that cannot be removed. Now returns an error with the list of valid input names. Common trigger: CamelCase names vs spaced names (e.g. `LifetimeMin` vs `Lifetime Min`). (Thanks [@playtabegg](https://github.com/playtabegg) — [#2](https://github.com/tumourlove/monolith/pull/2))

## [0.7.1] - 2026-03-11

Niagara write testing: all 41 actions verified. 12 bugs found and fixed, plus a major improvement to `get_module_inputs`.

### Fixed

- **CRASH: Niagara** — `GetAssetPath` infinite recursion: fallback called itself instead of reading `system_path`. Crashed `create_system_from_spec` and any action using `system_path` param
- **CRASH: Niagara** — `HandleCreateSystem` used raw `NewObject<UNiagaraSystem>` without initialization. `AddEmitterHandle` crashed with array-out-of-bounds on the uninitialized system. Fixed: calls `UNiagaraSystemFactoryNew::InitializeSystem()` after creation
- **CRASH: Niagara** — `HandleAddEmitter` crashed when emitter asset had no versions. Added version count guard before `AddEmitterHandle`
- **CRASH: Niagara** — `set_module_input_di` crashed with assertion `OverridePin.LinkedTo.Num() == 0` when pin already had links. Added `BreakAllPinLinks()` guard before `SetDataInterfaceValueForFunctionInput`
- **Niagara** — `set_module_input_di` accepted nonexistent input names silently. Now validates input exists using full engine input enumeration
- **Niagara** — `set_module_input_di` accepted non-DataInterface types (e.g. setting a curve DI on a Vector3f input). Now validates `IsDataInterface()` on the input type
- **Niagara** — `set_module_input_di` `config` param was parsed as string, not JSON object. Now accepts both JSON object (correct) and string (legacy)
- **Niagara** — `get_module_inputs` only returned static switch pins from the FunctionCall node. Now uses `FNiagaraStackGraphUtilities::GetStackFunctionInputs` with `FCompileConstantResolver` to return ALL inputs (floats, vectors, colors, DIs, enums, bools, positions, quaternions)
- **Niagara** — `GetStackFunctionInputOverridePin` helper only searched FunctionCall node pins. Now also walks upstream to ParameterMapSet override node (mirrors engine logic), so `has_override` correctly detects data input overrides
- **Niagara** — `get_module_inputs` returned `Module.`-prefixed names (e.g. `Module.Gravity`). Now strips prefix for consistency with write actions. Write actions accept both short and prefixed names
- **Niagara** — `batch_execute` dispatch table was missing 8 write ops: `remove_user_parameter`, `set_parameter_default`, `set_module_input_di`, `set_curve_value`, `reorder_emitters`, `duplicate_emitter`, `set_renderer_binding`, `request_compile`
- **Niagara** — `FindEmitterHandleIndex` auto-selected the only emitter even when a specific non-matching name was passed. Now only auto-selects when caller passes an empty string
- **Niagara** — `set_module_input_value` and `set_curve_value` didn't break existing pin links before setting literal values. Added `BreakAllPinLinks()` guard so literal values take effect when overriding a previous binding

## [0.7.0] - 2026-03-10

Animation Wave 2: 44 new actions across animation and PoseSearch, bringing the module from 23 to 67 actions and the plugin total to 177.

### Added

- **Animation — Curve Operations (7):** `get_curves`, `add_curve`, `remove_curve`, `set_curve_keys`, `get_curve_keys`, `rename_curve`, `get_curve_data`
- **Animation — Bone Track Inspection (3):** `get_bone_tracks`, `get_bone_track_data`, `get_animation_statistics`
- **Animation — Sync Markers (3):** `get_sync_markers`, `add_sync_marker`, `remove_sync_marker`
- **Animation — Root Motion (2):** `get_root_motion_info`, `extract_root_motion`
- **Animation — Compression (2):** `get_compression_settings`, `apply_compression`
- **Animation — BlendSpace Operations (5):** `get_blendspace_info`, `add_blendspace_sample`, `remove_blendspace_sample`, `set_blendspace_axis`, `get_blendspace_samples`
- **Animation — AnimBP Inspection (5):** `get_anim_blueprint_info`, `get_state_machines`, `get_state_info`, `get_transitions`, `get_anim_graph_nodes`
- **Animation — Montage Operations (5):** `get_montage_info`, `add_montage_section`, `delete_montage_section`, `set_montage_section_link`, `get_montage_slots`
- **Animation — Skeleton Operations (5):** `get_skeleton_info`, `add_virtual_bone`, `remove_virtual_bones`, `get_socket_info`, `add_socket`
- **Animation — Batch & Modifiers (2):** `batch_get_animation_info`, `run_animation_modifier`
- **Animation — PoseSearch (5):** `get_pose_search_schema`, `get_pose_search_database`, `add_database_sequence`, `remove_database_sequence`, `get_database_stats`

### Fixed

- **Animation** — `get_transitions` cast fix: uses `UAnimStateNodeBase` with conduit support, adds `from_type`/`to_type`
- **Animation** — State machine names stripped of `\n` suffix
- **Animation** — `get_state_info` now validates required params (`machine_name`, `state_name`)
- **Animation** — State machine matching changed from fuzzy `Contains()` to exact match
- **Animation** — `get_nodes` now accepts optional `graph_name` filter

### Changed

- **Animation** — Action count 23 → 67 (62 animation + 5 PoseSearch)
- **Total** — Action count 133 → 177

## [0.6.1] - 2026-03-10

MCP tool discovery fix — tools now register natively in Claude Code's ToolSearch.

### Fixed

- **MCP** — Tool names changed from dot notation (`material.query`) to underscore (`material_query`). Dots in tool names broke Claude Code's `mcp__server__tool` name mapping, causing silent registration failure. Legacy `.query` names still accepted for backwards compatibility via curl.
- **MCP** — Protocol version negotiation: server now echoes back the client's requested version (`2024-11-05` or `2025-03-26`) instead of always returning `2025-03-26`.

### Changed

- **Docs** — All documentation, skills, wiki, templates, and CLAUDE.md updated to use underscore tool naming.

## [0.6.0] - 2026-03-10

Material Wave 2: Full material CRUD coverage with 11 new write actions. Critical updater fix.

### Added

- **Material** — `create_material` action: create UMaterial at path with configurable defaults (Opaque/DefaultLit/Surface)
- **Material** — `create_material_instance` action: create UMaterialInstanceConstant from parent with parameter overrides
- **Material** — `set_material_property` action: set blend_mode, shading_model, two_sided, etc. via UMaterialEditingLibrary
- **Material** — `delete_expression` action: delete expression node by name from material graph
- **Material** — `get_material_parameters` action: list scalar/vector/texture/static_switch params with values (works on UMaterial and MIC)
- **Material** — `set_instance_parameter` action: set parameters on material instances (scalar, vector, texture, static switch)
- **Material** — `recompile_material` action: force material recompile
- **Material** — `duplicate_material` action: duplicate material to new asset path
- **Material** — `get_compilation_stats` action: sampler count, texture estimates, UV scalars, blend mode, expression count
- **Material** — `set_expression_property` action: set properties on expression nodes (e.g., DefaultValue)
- **Material** — `connect_expressions` action: wire expression outputs to inputs or material property inputs

### Fixed

- **Material** — `build_material_graph` class lookup: `FindObject<UClass>` → `FindFirstObject<UClass>` with U-prefix fallback. Short names like "Constant" now resolve correctly
- **Material** — `disconnect_expression` now disconnects material output pins (was only checking expr→expr, missing expr→material property)
- **CRITICAL: Auto-Updater** — Hot-swap script was deleting `Saved/` directory (containing EngineSource.db 1.8GB and ProjectIndex.db). Fixed: swap script and C++ template now preserve `Saved/` alongside `.git`

### Changed

- **Material** — Action count 14 → 25
- **Total** — Action count 122 → 133

## [0.5.2] - 2026-03-09

Wave 2: Blueprint expansion, Material export controls, Niagara HLSL auto-compile, and discover param schemas.

### Added

- **Blueprint** — `get_graph_summary` action: lightweight graph overview (id/class/title + exec connections only, ~10KB vs 172KB)
- **Blueprint** — `get_graph_data` now accepts optional `node_class_filter` param
- **Material** — `export_material_graph` now accepts `include_properties` (bool) and `include_positions` (bool) params
- **Material** — `get_thumbnail` now accepts `save_to_file` (bool) param
- **All** — Per-action param schemas in `monolith_discover()` output — all 122 actions now self-document their params

### Fixed

- **Blueprint** — `get_variables` now reads default values from CDO (was always empty)
- **Blueprint** — BlueprintIndexer CDO fix — same default value extraction applied to indexer
- **Niagara** — `get_compiled_gpu_hlsl` auto-compiles system if HLSL not available
- **Niagara** — `User.` prefix now stripped in `get_parameter_value`, `trace_parameter_binding`, `remove_user_parameter`, `set_parameter_default`

### Changed

- **Blueprint** — Action count 5 -> 6
- **Total** — Action count 121 -> 122

## [0.5.1] - 2026-03-09

Indexer reliability, Niagara usability, and Animation accuracy fixes.

### Fixed

- **Indexer** — Auto-index deferred to `IAssetRegistry::OnFilesLoaded()` — was running too early, only indexing 193 of 9560 assets
- **Indexer** — Sanity check: if fewer than 500 assets indexed, skip writing `last_full_index` so next launch retries
- **Indexer** — `bIsIndexing` reset in `Deinitialize()` to prevent stuck flag across editor sessions
- **Indexer** — Index DB changed from WAL to DELETE journal mode
- **Niagara** — `trace_parameter_binding` missing OR fallback for `User.` prefix
- **Niagara** — `get_di_functions` reversed class name pattern — now tries `UNiagaraDataInterface<Name>`
- **Niagara** — `batch_execute` had 3 op name mismatches — old names kept as aliases
- **Animation** — State machine names stripped of `\n` suffix (clean names like "InAir" instead of "InAir\nState Machine")
- **Animation** — `get_state_info` now validates required params (`machine_name`, `state_name`)
- **Animation** — State machine matching changed from fuzzy `Contains()` to exact match

### Added

- **Niagara** — `list_emitters` action: returns emitter names, index, enabled, sim_target, renderer_count
- **Niagara** — `list_renderers` action: returns renderer class, index, enabled, material
- **Niagara** — All actions now accept `asset_path` (preferred) with `system_path` as backward-compat alias
- **Niagara** — `duplicate_emitter` accepts `emitter` as alias for `source_emitter`
- **Niagara** — `set_curve_value` accepts `module_node` as alias for `module`
- **Animation** — `get_nodes` now accepts optional `graph_name` filter (makes `get_blend_nodes` redundant for filtered queries)

### Changed

- **Niagara** — Action count 39 → 41
- **Total** — Action count 119 → 121

## [0.5.0] - 2026-03-08

Auto-updater rewrite — fixes all swap script failures on Windows.

### Fixed

- **Auto-Updater** — Swap script now polls `tasklist` for `UnrealEditor.exe` instead of a cosmetic 10-second countdown (was launching before editor fully exited)
- **Auto-Updater** — `errorlevel` check after retry rename was unreachable due to cmd.exe resetting `%ERRORLEVEL%` on closing `)` — replaced with `goto` pattern
- **Auto-Updater** — Launcher script now uses outer-double-quote trick for `cmd /c` paths with spaces (`D:\Unreal Projects\...`)
- **Auto-Updater** — Switched from `ren` (bare filename only) to `move` (full path support) for plugin folder rename
- **Auto-Updater** — Retry now cleans stale backup before re-attempting rename
- **Auto-Updater** — Rollback on failed xcopy now removes partial destination before restoring backup
- **Auto-Updater** — Added `/h` flag to primary xcopy to include hidden-attribute files
- **Auto-Updater** — Enabled `DelayedExpansion` for correct variable expansion inside `if` blocks

## [0.2.0] - 2026-03-08

Source indexer overhaul and auto-updater improvements.

### Fixed

- **Source Indexer** — UE macros (UCLASS, ENGINE_API, GENERATED_BODY) now stripped before tree-sitter parsing, fixing class hierarchy and inheritance resolution
- **Source Indexer** — Class definitions increased from ~0 to 62,059; inheritance links from ~0 to 37,010
- **Source Indexer** — `read_source members_only` now returns class members correctly
- **Source Indexer** — `get_class_hierarchy` ancestor traversal now works
- **MonolithSource** — `get_class_hierarchy` accepts both `symbol` and `class_name` params (was inconsistent)

### Added

- **Source Indexer** — UE macro preprocessor (`ue_preprocessor.py`) with balanced-paren stripping for UCLASS/USTRUCT/UENUM/UINTERFACE
- **Source Indexer** — `--clean` flag for `__main__.py` to delete DB before reindexing
- **Source Indexer** — Diagnostic output after indexing (definitions, forward decls, inheritance stats)
- **Auto-Updater** — Release notes now shown in update notification toast and logged to Output Log

### Changed

- **Source Indexer** — `reference_builder.py` now preprocesses source before tree-sitter parsing

### Important

- **You MUST delete your existing source database and reindex** after updating to 0.2.0. The old database has empty class hierarchy data. Delete the `.db` file in your Saved/Monolith/ directory and run the indexer with `--clean`.

## [0.1.0] - 2026-03-07

Initial beta release. One plugin, 9 domains, 119 actions.

### Added

- **MonolithCore** — Embedded Streamable HTTP MCP server with JSON-RPC 2.0 dispatch
- **MonolithCore** — Central tool registry with discovery/dispatch pattern (~14 namespace tools instead of ~117 individual tools)
- **MonolithCore** — Plugin settings via UDeveloperSettings (port, auto-update, module toggles, DB paths)
- **MonolithCore** — Auto-updater via GitHub Releases (download, stage, notify)
- **MonolithCore** — Asset loading with 4-tier fallback (StaticLoadObject -> PackageName.ObjectName -> FindObject+_C -> ForEachObjectWithPackage)
- **MonolithBlueprint** — 6 actions: graph topology, graph summary, variables, execution flow tracing, node search
- **MonolithMaterial** — 14 actions: inspection, graph editing, build/export/import, validation, preview rendering, Custom HLSL nodes
- **MonolithAnimation** — 23 actions: montage sections, blend space samples, ABP graph reading, notify editing, bone tracks, skeleton info
- **MonolithNiagara** — 39 actions: system/emitter management, module stack operations, parameters, renderers, batch execute, declarative system builder
- **MonolithNiagara** — 6 reimplemented NiagaraEditor helpers (Epic APIs not exported)
- **MonolithEditor** — 13 actions: Live Coding build triggers, compile output capture, log ring buffer (10K entries), crash context
- **MonolithConfig** — 6 actions: INI resolution, explain (multi-layer), diff from default, search, section read
- **MonolithIndex** — SQLite FTS5 project indexer with 4 indexers (Blueprint, Material, Generic, Dependency)
- **MonolithIndex** — 5 actions: full-text search, reference tracing, type filtering, stats, asset deep inspection
- **MonolithSource** — Python tree-sitter engine source indexer (C++ and shader parsing)
- **MonolithSource** — 10 actions: source reading, call graphs, class hierarchy, symbol context, module info
- **9 Claude Code skills** — Domain-specific workflow guides for animation, blueprints, build decisions, C++, debugging, materials, Niagara, performance, project search
- **Templates** — `.mcp.json.example` and `CLAUDE.md.example` for quick project setup
- All 9 modules compiling clean on UE 5.7
- **MonolithEditor** — `get_compile_output` action for Live Coding compile result capture with time-windowed error filtering
- **MonolithEditor** — Auto hot-swap on editor exit (stages update, swaps on close)
- **MonolithEditor** — Re-index buttons in Project Settings UI
- **MonolithEditor** — Improved Live Coding integration with compile output capture, time-windowed errors, category filtering
- **unreal-build skill** — Smart build decision-making guide (Live Coding vs full rebuild)
- **Logging** — 80% reduction in Log-level noise across all modules (kept Warnings/Errors, demoted routine logs to Verbose)
- **README** — Complete rewrite with Installation for Dummies walkthrough

### Fixed

- HTTP body null-termination causing malformed JSON-RPC responses
- Niagara graph traversal crash when accessing emitter shared graphs
- Niagara emitter lookup failures — added case-insensitive matching with fallbacks
- Source DB WAL journal mode causing lock contention — switched to DELETE mode
- SQL schema creation with nested BEGIN/END depth tracking for triggers
- Reindex dispatch — switched from `FindFunctionByName` to `StartFullIndex` with UFUNCTION
- Asset loading crash from `FastGetAsset` on background thread — removed unsafe call
- Animation `remove_bone_track` — now uses `RemoveBoneCurve(FName)` per bone with child traversal
- MonolithIndex `last_full_index` — added `WriteMeta()` call, guarded with `!bShouldStop`
- Niagara `move_module` — rewires stack-flow pins only, preserves override inputs
- Editor `get_build_errors` — uses `ELogVerbosity` enum instead of substring matching
- MonolithIndex SQL injection — all 13 insert methods converted to `FSQLitePreparedStatement`
- Animation modules using `LogTemp` instead of `LogMonolith`
- Editor `CachedLogCapture` dangling pointer — added `ClearCachedLogCapture()` in `ShutdownModule`
- MonolithSource vestigial outer module — flattened structure, deleted stub
- Session expiry / reconnection issues — removed session tracking entirely (server is stateless)
- Claude tools failing on first invocation — fixed transport type in `.mcp.json` (`"http"` -> `"streamableHttp"`) and fixed MonolithSource stub not registering actions
