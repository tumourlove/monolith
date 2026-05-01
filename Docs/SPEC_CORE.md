# Monolith — Technical Specification

**Version:** 0.14.7 (Beta)
**Wiki:** https://github.com/tumourlove/monolith/wiki
**Engine:** Unreal Engine 5.7+
**Platform:** Windows, macOS, Linux
**License:** MIT
**Author:** tumourlove
**Repository:** https://github.com/tumourlove/monolith

---

## 1. Overview

Monolith is a unified Unreal Engine editor plugin that consolidates 9 separate MCP (Model Context Protocol) servers and 4 C++ plugins into a single plugin with an embedded HTTP MCP server. It reduces ~220 individual tools down to 20 MCP tools (1271 total registrations across 16 in-tree namespaces, 1267 distinct handlers; 1271 active by default — 45 experimental town gen actions disabled), cutting AI assistant context consumption by ~95%. The CommonUI action pack (51 actions, conditional on `WITH_COMMONUI`) shipped M0.5, v0.14.0 (2026-04-19), tested M0.5.1 (2026-04-25). Editor +2 (`run_automation_tests`, `list_automation_tests`) landed in [Unreleased] — PR by @MaxenceEpitech.

### What It Replaces

| Original Server/Plugin | Actions | Replaced By |
|------------------------|---------|-------------|
| unreal-blueprint-mcp + BlueprintReader | 46 | MonolithBlueprint |
| unreal-material-mcp + MaterialMCPReader | 46 | MonolithMaterial |
| unreal-animation-mcp + AnimationMCPReader | 62 | MonolithAnimation (62 actions) |
| unreal-niagara-mcp + NiagaraMCPBridge | 70 | MonolithNiagara |
| unreal-editor-mcp | 11 | MonolithEditor |
| unreal-config-mcp | 6 | MonolithConfig |
| unreal-project-mcp | 17 | MonolithIndex |
| unreal-source-mcp (concept from Codeturion) | 9 | MonolithSource |
| unreal-api-mcp | — | MonolithSource |

---

## 2. Architecture

```
Monolith.uplugin
  MonolithCore          — HTTP server (bind retry with port probe, Restart()), tool registry, discovery, settings, auto-updater
  MonolithBlueprint     — Blueprint inspection, variable/component/graph CRUD, node operations, compile, spawn (89 actions)
  MonolithMaterial      — Material inspection + graph editing + CRUD + function suite + tiling quality + texture preview (63 actions)
  MonolithAnimation     — Animation sequences, montages, ABPs, curves, notifies, skeletons, PoseSearch, ABP/ControlRig write, layout (118 actions)
  MonolithNiagara       — Niagara particle systems, HLSL module/function creation, DI config, event handlers, sim stages, NPC, effect types, scalability, layout (109 actions)
  MonolithEditor        — Build triggers, live compile, log capture, compile output, crash context, scene capture, texture import, flipbook stitching, asset deletion, viewport info, blank-map factory + module status (Phase J F8), automation test list/run (24 actions — +2 from [Unreleased] PR by @MaxenceEpitech)
  MonolithConfig        — Config/INI resolution and search (6 actions)
  MonolithIndex         — SQLite FTS5 deep project indexer, 14 internal indexers (7 MCP actions)
  MonolithSource        — Engine source + API lookup, auto-reindex on hot-reload (Phase J F17) (11 actions)
  MonolithUI            — Widget blueprint CRUD + slot/template/styling, animation v1 (deprecated) + v2 (hoisted), settings scaffolding, accessibility, UISpec build/dump/schema, EffectSurface sub-bag actions (reflective optional-provider probe, decoupled 2026-04-27), CommonUI categories A–I, Type Registry diagnostic, Style Service diagnostic. (66 always-on + 51 CommonUI = 117 module-owned + 4 GAS UI binding aliases registered cross-namespace). CommonUI actions conditional on #if WITH_COMMONUI. EffectSurface actions return -32010 ErrOptionalDepUnavailable when the optional EffectSurface provider is absent (see specs/SPEC_MonolithUI.md § "Error Contract"). Architecture expansion Phase A–L landed 2026-04-26 (Spec Builder, Type Registry, EffectSurface, Style Service, hoisted Design Import verbs)
  MonolithMesh          — Mesh inspection, scene manipulation, spatial queries, level blockout, GeometryScript ops, horror/accessibility, lighting, audio/acoustics, performance, decals, level design, tech art, context props, procedural geometry (sweep walls, auto-collision, proc mesh caching, blueprint prefabs), genre presets, encounter design, accessibility reports (195 core actions) + EXPERIMENTAL procedural town generator (45 actions, disabled by default via bEnableProceduralTownGen) = 240 total
  MonolithGAS           — Gameplay Ability System integration: abilities, attributes, effects, ASC, tags, cues, targets, input, inspection, scaffolding, UI attribute binding (135 actions: 131 GAS-namespace + 4 also aliased into `ui` namespace). Conditional on #if WITH_GBA
  MonolithComboGraph    — ComboGraph plugin integration: combo graph CRUD, node/edge management, effects, cues, ability scaffolding (13 actions). Conditional on #if WITH_COMBOGRAPH
  MonolithAI            — AI asset manipulation: Behavior Trees, Blackboards, State Trees, EQS, Smart Objects, AI Controllers, Perception, Navigation, Runtime/PIE, Scaffolds, Discovery, Advanced (221 actions, +2 from Phase J F8). Conditional on #if WITH_STATETREE, #if WITH_SMARTOBJECTS (required); #if WITH_MASSENTITY, #if WITH_ZONEGRAPH (optional)
  MonolithLogicDriver   — Logic Driver Pro integration: SM CRUD, graph read/write, node config, runtime/PIE, JSON spec, scaffolding, discovery, components, text graph (66 actions). Conditional on #if WITH_LOGICDRIVER
  MonolithAudio         — Audio asset creation, inspection, batch management, Sound Cue graph building, MetaSound graph building via Builder API, AI Perception sound binding, sine-tone test wave (Phase J F18) (86 actions). MetaSound conditional on #if WITH_METASOUND
  MonolithAudioRuntime  — Runtime sub-module (Type: Runtime) holding `UMonolithSoundPerceptionUserData` + `UMonolithAudioPerceptionSubsystem` consumed by `audio::bind_sound_to_perception`. **Editor-only ship caveat** — Monolith does not currently ship to cooked game builds, so the runtime sub-module is not present at runtime in shipped Steam builds. See `COOKED_BUILD_TODO.md`. (0 MCP actions — provides runtime classes only)
  MonolithBABridge      — Optional IModularFeatures bridge for Blueprint Assist integration. Exposes IMonolithGraphFormatter; enables BA-powered auto_layout across blueprint, material, animation, and niagara modules when Blueprint Assist is present (0 MCP actions — integration only)
```

**Custom sibling plugins (not inside core Monolith; source + per-module specs are private to their respective repos):**
Additional project-specific sibling plugins may register their own namespaces outside this repository. They are intentionally excluded from public Monolith action counts and release packages; their source, action rosters, and module specs belong in their own repos.

**Optional widget runtime providers** (not bundled with the public Monolith release zip): MonolithUI can expose EffectSurface action handlers through a reflective UClass probe when an external provider supplies the expected widget classes. MonolithUI has zero compile-time dependency on that provider. When the provider is absent, EffectSurface actions return `-32010 ErrOptionalDepUnavailable` (see [`specs/SPEC_MonolithUI.md` § "Error Contract"](specs/SPEC_MonolithUI.md#error-contract--optional-effectsurface-provider-absence--32010)); the rest of `ui::` is fully functional. The `make_release.ps1` `$LeakSentinels` list defends against accidental optional-provider symbol leakage into public release DLLs.

For the architectural pattern that lets you write your own sibling plugin and register actions into Monolith's MCP registry from outside the core repo, see [`SIBLING_PLUGIN_GUIDE.md`](SIBLING_PLUGIN_GUIDE.md).

> **Live editor `monolith_status` will report a higher count than the in-tree total.** When sibling plugins are loaded the editor reports the union of in-tree and sibling actions. This is expected. The numbers in §12 below are the **in-tree** ground truth; sibling totals are specced in their own repos.

### Discovery/Dispatch Pattern

All domain modules register actions with `FMonolithToolRegistry` (central singleton). Each domain exposes a single `{namespace}_query(action, params)` MCP tool. The 4 core tools (`monolith_discover`, `monolith_status`, `monolith_reindex`, `monolith_update`) are standalone. Conditional modules gate registration on compile-time defines: MonolithGAS (`#if WITH_GBA`), MonolithComboGraph (`#if WITH_COMBOGRAPH`), MonolithLogicDriver (`#if WITH_LOGICDRIVER`), MonolithUI CommonUI actions (`#if WITH_COMMONUI`), MonolithAI (`#if WITH_STATETREE` + `#if WITH_SMARTOBJECTS` required; `#if WITH_MASSENTITY` + `#if WITH_ZONEGRAPH` optional), MonolithAudio (MetaSound actions conditional on `#if WITH_METASOUND`).

### MCP Protocol

- **Protocol version:** Echoes client's requested version; supports both `2024-11-05` and `2025-03-26` (defaults to `2025-03-26`)
- **Transport:** HTTP with JSON-RPC 2.0 (POST for requests, GET for SSE stub, OPTIONS for CORS). Transport type in `.mcp.json` varies by client: `"http"` for Claude Code, `"streamableHttp"` for Cursor/Cline
- **Endpoint:** `http://localhost:{port}/mcp` (default port 9316)
- **Bind retry:** `FMonolithHttpServer::Start()` attempts up to 5 binds with exponential backoff and TCP port probe before failing. `Restart()` method available for runtime recovery. Console command: `Monolith.Restart`
- **Batch support:** Yes (JSON-RPC arrays)
- **Session management:** None — server is fully stateless (session tracking removed; no per-session state was ever stored)
- **CORS:** `Access-Control-Allow-Origin: *`

#### JSON-RPC error catalogue

Standard codes mirror the JSON-RPC 2.0 spec. Monolith server-defined codes live in the `-32000..-32099` range. Constants: `Plugins/Monolith/Source/MonolithCore/Public/MonolithJsonUtils.h`.

| Constant | Code | Meaning |
|----------|------|---------|
| `ErrParseError` | `-32700` | JSON parse failure on the request body. |
| `ErrInvalidRequest` | `-32600` | Request shape doesn't match JSON-RPC 2.0. |
| `ErrMethodNotFound` | `-32601` | Tool / action name not registered. |
| `ErrInvalidParams` | `-32602` | Action found, params invalid (missing required field, bad enum, etc.). |
| `ErrInternalError` | `-32603` | The server choked. Default for unspecified failures. |
| `ErrOptionalDepUnavailable` | `-32010` | An optional sibling/marketplace plugin the action depends on is not present. The action exists in the registry; the call cannot be served. First consumer: the 10 EffectSurface action handlers when the optional EffectSurface provider is absent (see [`specs/SPEC_MonolithUI.md` § "Error Contract — Optional EffectSurface Provider Absence (-32010)"](specs/SPEC_MonolithUI.md#error-contract--optional-effectsurface-provider-absence--32010)). Reserved range `-32011..-32019` left open for future "optional dep" codes. |

### Module Loading

| Module | Loading Phase | Type |
|--------|--------------|------|
| MonolithCore | PostEngineInit | Editor |
| All others (16 Editor + 1 Runtime) | Default | Editor (MonolithBABridge is optional — empty shell when Blueprint Assist absent). MonolithAudioRuntime is Type: Runtime — provides `UMonolithSoundPerceptionUserData` + `UMonolithAudioPerceptionSubsystem` for `audio::bind_sound_to_perception` |

### Plugin Dependencies

- Niagara
- SQLiteCore
- EnhancedInput
- EditorScriptingUtilities
- PoseSearch
- IKRig
- ControlRig
- RigVM
- Sockets
- Networking
- GeometryScripting (optional — enables Tier 5 mesh operations)
- GameplayAbilities (optional — enables MonolithGAS module; `#if WITH_GBA` compile guard)

### Optional-Dependency Detection Matrix

Modules that probe for optional plugins follow a unified Build.cs convention: 3-location detection (project Plugins/, engine Plugins/Marketplace/, engine Plugins/Runtime/) with `MONOLITH_RELEASE_BUILD=1` env-var escape hatch. When the env var is `"1"`, detection short-circuits and the corresponding `WITH_*` define is forced off — the released DLL drops the hard import and Blueprint-only users without the dep don't hit `GetLastError=126` at module load.

| Module | Optional dep | Compile guard | Build.cs file | Hotfix landed |
|--------|--------------|---------------|---------------|---------------|
| MonolithBABridge | BlueprintAssist | `WITH_BLUEPRINT_ASSIST` | `MonolithBABridge.Build.cs` | (canonical) |
| MonolithMesh | GeometryScripting | `WITH_GEOMETRYSCRIPT` | `MonolithMesh.Build.cs` | **v0.14.1** (#26 / #30) |
| MonolithGAS | GameplayAbilities | `WITH_GBA` | `MonolithGAS.Build.cs` | (existing) |
| MonolithComboGraph | ComboGraph | `WITH_COMBOGRAPH` | `MonolithComboGraph.Build.cs` | (existing) |
| MonolithLogicDriver | Logic Driver Pro | `WITH_LOGICDRIVER` | `MonolithLogicDriver.Build.cs` | (existing) |
| MonolithAudio | MetaSound | `WITH_METASOUND` | `MonolithAudio.Build.cs` | (existing) |
| MonolithUI | CommonUI | `WITH_COMMONUI` | `MonolithUI.Build.cs` | **v0.14.0** (M0.5) |
| MonolithAI | StateTree, SmartObjects | `WITH_STATETREE`, `WITH_SMARTOBJECTS` (required); `WITH_MASSENTITY`, `WITH_ZONEGRAPH` (optional) | `MonolithAI.Build.cs` | (existing) |

---

## 3. Module Reference

Each module has its own spec file under `specs/`. The table below is the index.

| # | Module | Spec | Summary |
|---|--------|------|---------|
| 3.1 | MonolithCore | [specs/SPEC_MonolithCore.md](specs/SPEC_MonolithCore.md) | HTTP server (bind retry, Restart(), `Monolith.Restart` console cmd), tool registry, discovery, settings, auto-updater |
| 3.2 | MonolithBlueprint | [specs/SPEC_MonolithBlueprint.md](specs/SPEC_MonolithBlueprint.md) | Blueprint inspection, variable/component/graph CRUD, node ops, compile, spawn (89 actions) |
| 3.3 | MonolithMaterial | [specs/SPEC_MonolithMaterial.md](specs/SPEC_MonolithMaterial.md) | Material inspection + graph editing + CRUD + function suite (63 actions) |
| 3.4 | MonolithAnimation | [specs/SPEC_MonolithAnimation.md](specs/SPEC_MonolithAnimation.md) | Animation sequences, montages, ABPs, curves, notifies, skeletons, PoseSearch, ABP write, ControlRig (118 actions) |
| 3.5 | MonolithNiagara | [specs/SPEC_MonolithNiagara.md](specs/SPEC_MonolithNiagara.md) | Niagara particle systems, HLSL module/function, DI config, event handlers, sim stages, layout (109 actions) |
| 3.6 | MonolithEditor | [specs/SPEC_MonolithEditor.md](specs/SPEC_MonolithEditor.md) | Build triggers, live compile, log capture, crash context, scene capture (20 actions) |
| 3.7 | MonolithConfig | [specs/SPEC_MonolithConfig.md](specs/SPEC_MonolithConfig.md) | Config/INI resolution and search (6 actions) |
| 3.8 | MonolithIndex | [specs/SPEC_MonolithIndex.md](specs/SPEC_MonolithIndex.md) | SQLite FTS5 deep project indexer (7 MCP actions, 14 internal indexers) |
| 3.9 | MonolithSource | [specs/SPEC_MonolithSource.md](specs/SPEC_MonolithSource.md) | Engine source + API lookup (11 actions) |
| 3.10 | MonolithUI | [specs/SPEC_MonolithUI.md](specs/SPEC_MonolithUI.md) | Widget blueprint CRUD, slot/template/styling, animation v1+v2, bindings, settings/accessibility scaffolds, **Spec Builder + Type Registry + EffectSurface + Style Service** (Phase A–L expansion 2026-04-26), CommonUI categories A–I. **117 module-owned actions** (66 always-on + 51 CommonUI under `WITH_COMMONUI`) + 4 GAS UI binding aliases |
| 3.11 | MonolithMesh | [specs/SPEC_MonolithMesh.md](specs/SPEC_MonolithMesh.md) | Mesh/scene/spatial/blockout/GeometryScript/procedural (195 core + 45 experimental town gen = 240 actions) |
| 3.12 | MonolithBABridge | [specs/SPEC_MonolithBABridge.md](specs/SPEC_MonolithBABridge.md) | IModularFeatures bridge for Blueprint Assist (0 MCP actions, integration only) |
| 3.13 | MonolithGAS | [specs/SPEC_MonolithGAS.md](specs/SPEC_MonolithGAS.md) | Gameplay Ability System integration (135 actions: 131 GAS + 4 UI binding aliased into `ui::`, WITH_GBA) |
| 3.14 | MonolithComboGraph | [specs/SPEC_MonolithComboGraph.md](specs/SPEC_MonolithComboGraph.md) | ComboGraph integration (13 actions, WITH_COMBOGRAPH) |
| 3.15 | MonolithLogicDriver | [specs/SPEC_MonolithLogicDriver.md](specs/SPEC_MonolithLogicDriver.md) | Logic Driver Pro integration (66 actions, WITH_LOGICDRIVER) |
| 3.16 | MonolithAI | [specs/SPEC_MonolithAI.md](specs/SPEC_MonolithAI.md) | Behavior Trees, Blackboards, State Trees, EQS, Smart Objects, Perception, Nav (221 actions, +2 in Phase J F8) |
| 3.17 | MonolithAudio | [specs/SPEC_MonolithAudio.md](specs/SPEC_MonolithAudio.md) | Sound Cues, MetaSounds, batch audio ops, AI Perception bind (86 actions, MetaSound WITH_METASOUND) |

---

## 4. Source Indexer

### 4.1 C++ Indexer (current)

The engine source indexer is a native C++ implementation within `MonolithSource`. `UMonolithSourceSubsystem` builds and maintains `EngineSource.db` in-process. Indexing is triggered via:

- **`trigger_reindex`** — full engine source re-index
- **`trigger_project_reindex`** — incremental project-only C++ re-index (faster; only updates project symbols)

### 4.2 Python Source Indexer (legacy)

> **LEGACY:** The Python tree-sitter indexer in `Scripts/source_indexer/` has been superseded by the native C++ indexer. It is no longer invoked by MonolithSource and is retained only for reference.

**Location:** `Scripts/source_indexer/`
**Entry point:** `python -m source_indexer --source PATH --db PATH [--shaders PATH]`
**Dependencies:** tree-sitter>=0.21.0, tree-sitter-cpp>=0.21.0, Python 3.10+

#### Pipeline (IndexingPipeline)

1. **Module Discovery** — Walks Runtime, Editor, Developer, Programs under Engine/Source + Engine/Plugins. Optionally Engine/Shaders
2. **File Processing** — C++ files -> CppParser (tree-sitter AST) -> symbols, includes. Shader files -> ShaderParser (regex) -> symbols, includes
3. **Source Line FTS** — Chunks source in batches of 10 lines into source_fts table
4. **Finalization** — Resolves inheritance, runs ReferenceBuilder for call/type cross-references

#### Parsers

| Parser | Technology | Handles |
|--------|-----------|---------|
| CppParser | tree-sitter-cpp | Classes, structs, enums, functions, variables, macros, typedefs. UE macro awareness (UCLASS, USTRUCT, UENUM, UFUNCTION, UPROPERTY). 3 fallback strategies |
| ShaderParser | Regex | #include, #define, struct, function declarations in .usf/.ush |
| ReferenceBuilder | tree-sitter-cpp (2nd pass) | Call references, type references, local variable type resolution |

### Source DB Schema

| Table | Purpose |
|-------|---------|
| `modules` | id, name, path, module_type, build_cs_path |
| `files` | id, path, module_id, file_type, line_count, last_modified |
| `symbols` | id, name, qualified_name, kind, file_id, line_start, line_end, parent_symbol_id, access, signature, docstring, is_ue_macro |
| `inheritance` | id, child_id, parent_id |
| `references` | id, from_symbol_id, to_symbol_id, ref_kind, file_id, line |
| `includes` | id, file_id, included_path, line |
| `symbols_fts` | FTS5 on name, qualified_name, docstring |
| `source_fts` | FTS5 on text (file_id, line_number UNINDEXED) |
| `meta` | key, value |

---

## 5. Offline CLI

Two options for offline access (no full editor session required):

### 5.1 monolith_query.exe (preferred)

**Binary:** `Plugins/Monolith/Binaries/monolith_query.exe`
**Source:** `Tools/MonolithQuery/` — build via `build.bat`
**Run via:**
```
'Plugins/Monolith/Binaries/monolith_query.exe' <namespace> <action> [args...]
```

Standalone C++ executable. No UE runtime, no Python, instant startup. Queries `EngineSource.db` and `ProjectIndex.db` directly. Replaces the previous `MonolithQueryCommandlet` (removed) and supersedes `monolith_offline.py` as the primary offline access path.

### 5.2 monolith_offline.py (legacy)

> **LEGACY:** `monolith_offline.py` is superseded by `monolith_query.exe`. It remains functional as a zero-dependency fallback requiring only Python stdlib and no UE installation.

**Location:** `Saved/monolith_offline.py`
**Dependencies:** Python stdlib only (sqlite3, argparse, json, re, pathlib) — no pip installs required
**Python version:** 3.8+

A companion CLI that queries `EngineSource.db` and `ProjectIndex.db` directly without the Unreal Editor running. Intended as a fallback when MCP is unavailable (editor down, CI environments, quick terminal lookups).

**Scope:** Read/query operations only. Write operations require the editor and MCP.

### Usage

```
python Saved/monolith_offline.py <namespace> <action> [args...]
```

### Namespaces and Actions

**Source (9 actions)** — mirrors `source_query` MCP tool:

| Action | Positional | Key Options | Description |
|--------|-----------|-------------|-------------|
| `search_source` | `query` | `--limit`, `--module`, `--kind` | FTS across symbols + source lines, BM25 ranked |
| `read_source` | `symbol` | `--max-lines`, `--members-only`, `--no-header` | Source for a class/function/struct; FTS fallback on no exact match |
| `find_references` | `symbol` | `--ref-kind`, `--limit` | All usage sites |
| `find_callers` | `symbol` | `--limit` | Functions that call the given function |
| `find_callees` | `symbol` | `--limit` | Functions called by the given function |
| `get_class_hierarchy` | `symbol` | `--direction up\|down\|both`, `--depth` | Inheritance tree traversal |
| `get_module_info` | `module_name` | — | File count, symbol counts by kind, key classes |
| `get_symbol_context` | `symbol` | `--context-lines` | Definition with surrounding context |
| `read_file` | `file_path` | `--start`, `--end` | Read source lines; resolves via absolute path → DB exact → DB suffix match |

**Project (5 actions)** — mirrors `project_query` MCP tool:

| Action | Positional | Key Options | Description |
|--------|-----------|-------------|-------------|
| `search` | `query` | `--limit` | FTS across assets FTS + nodes FTS, BM25 ranked |
| `find_by_type` | `asset_class` | `--limit`, `--offset` | Filter assets by class with pagination |
| `find_references` | `asset_path` | — | Bidirectional: depends_on + referenced_by |
| `get_stats` | — | — | Row counts for all tables + top 20 asset class breakdown |
| `get_asset_details` | `asset_path` | — | Nodes, variables, parameters for one asset |

### Implementation Notes

- Opens DBs with `PRAGMA query_only=ON` + `PRAGMA journal_mode=DELETE`. The DELETE journal mode override is mandatory — WAL mode silently returns 0 rows on Windows when opened in any read-only mode (same bug that affected the C++ module; see CLAUDE.md Key Lessons).
- FTS escaping mirrors `EscapeFTS()` in C++: `::` replaced with space, non-word chars stripped, each token wrapped as `"token"*` for prefix match.
- `read_source` defaults to `--header` (includes `.h` declarations). Pass `--no-header` to skip header files.
- `read_file` with `--end 0` (default) reads 200 lines from `--start`.
- Source output is plain text. Project output is JSON.

---

## 6. Skills (11 bundled)

| Skill | Trigger Words | Entry Point | Actions |
|-------|--------------|-------------|---------|
| unreal-animation | animation, montage, ABP, blend space, notify, curves, compression, PoseSearch | `animation_query()` | 116 |
| unreal-audio | audio, sound, SoundCue, MetaSound, attenuation, submix, mixing | `audio_query()` | 81 |
| unreal-blueprints | Blueprint, BP, event graph, node, variable | `blueprint_query()` | 86 |
| unreal-build | build, compile, Live Coding, hot reload, rebuild | `editor_query()` | 19 |
| unreal-cpp | C++, header, include, UCLASS, Build.cs, linker error | `source_query()` + `config_query()` | 11+6 |
| unreal-debugging | build error, crash, log, debug, stack trace | `editor_query()` | 19 |
| unreal-materials | material, shader, PBR, texture, material graph | `material_query()` | 57 |
| unreal-niagara | Niagara, particle, VFX, emitter | `niagara_query()` | 96 |
| unreal-performance | performance, optimization, FPS, frame time | Cross-domain | config + material + niagara |
| unreal-project-search | find asset, search project, dependencies | `project_query()` | 7 |
| unreal-ui | UI, HUD, widget, menu, settings, save game, accessibility, CommonUI, activatable, button style, input glyph, focus, **spec builder, EffectSurface, type registry, style service** | `ui_query()` | 117 |

All skills follow a common structure: YAML frontmatter, Discovery section, Asset Path Conventions table, action tables, workflow examples, and rules.

---

## 7. Configuration

**Settings location:** Editor Preferences > Plugins > Monolith
**Config file:** `Config/MonolithSettings.ini` section `[/Script/MonolithCore.MonolithSettings]`

Setting names below match the actual `UMonolithSettings` UPROPERTY identifiers in `Source/MonolithCore/Public/MonolithSettings.h` (verified 2026-04-26 audit). The convention is `bEnable<Module>` for module toggles, `bEnableProceduralTownGen` for experimental sub-features.

| Setting | Default | Description |
|---------|---------|-------------|
| ServerPort | 9316 | MCP HTTP server port |
| bAutoUpdateEnabled | True | GitHub Releases auto-check on startup |
| DatabasePathOverride | (empty) | Override default DB path (Plugins/Monolith/Saved/) |
| EngineSourceDBPathOverride | (empty) | Override engine source DB path |
| EngineSourcePath | (empty) | Override engine source directory |
| bEnableBlueprint | True | Enable Blueprint module |
| bEnableMaterial | True | Enable Material module |
| bEnableAnimation | True | Enable Animation module |
| bEnableNiagara | True | Enable Niagara module |
| bEnableEditor | True | Enable Editor module |
| bEnableConfig | True | Enable Config module |
| bEnableIndex | True | Enable Index module |
| bEnableSource | True | Enable Source module |
| bEnableUI | True | Enable UI module |
| bEnableMesh | True | Enable Mesh module (core actions) |
| bEnableGAS | True | Enable GAS module (requires GameplayAbilities plugin; no-op if `WITH_GBA=0`) |
| bEnableComboGraph | True | Enable ComboGraph module (no-op if `WITH_COMBOGRAPH=0`) |
| bEnableLogicDriver | True | Enable Logic Driver Pro module (no-op if `WITH_LOGICDRIVER=0`) |
| bEnableAI | True | Enable AI module (Behavior Trees, Blackboards, State Trees, EQS, Smart Objects, Perception, Navigation) |
| bEnableAudio | True | Enable Audio module (Sound Cues, MetaSounds, batch ops, AI Perception bind) |
| bEnableExternalInventoryModule | True | Allow an external sibling plugin to register `inventory_query` actions |
| bEnableProceduralTownGen | **False** | Enable Procedural Town Generator actions (45 actions). Requires `bEnableMesh`. **Work-in-progress** — known geometry issues, disabled by default. Unless you're willing to dig in and help improve it, best left alone |
| bEnableBlueprintAssist | True | Allow MonolithBABridge to register IMonolithGraphFormatter when Blueprint Assist is present. Set false to force built-in layout for all auto_layout calls |
| bDeferFirstTimeIndex | False | If true, first-time indexing won't run automatically. Use `Monolith.StartIndex` console command to trigger |
| bLogMemoryStats | False | Log memory usage during indexing for debugging. Default off — enable when investigating memory pressure |
| LogVerbosity | 3 (Log) | 0=Silent, 1=Error, 2=Warning, 3=Log, 4=Verbose |

**Note:** Module enable toggles are functional — each module checks its toggle at registration time and skips action registration if disabled.

---

## 8. Templates

| File | Purpose |
|------|---------|
| `Templates/.mcp.json.example` | Minimal MCP config. Transport type varies by client: `"http"` for Claude Code, `"streamableHttp"` for Cursor/Cline. URL: `http://localhost:9316/mcp` |
| `Templates/.mcp.json.proxy.example` | MCP config variant for clients that need the stdio↔HTTP proxy bridge (Cursor / Cline / Continue). Spawns `Scripts/monolith_proxy.py`. |

**Note on AI project-instructions files:** As of v0.14.7 we no longer ship a `CLAUDE.md.example` template. Conventions across AI assistants (`CLAUDE.md`, `AGENTS.md`, `.cursorrules`, `.github/copilot-instructions.md`, etc.) drift faster than a static template can track — installers are pointed at their own assistant for the right shape. See README §Step 5.

---

## 9. File Structure

```
YourProject/Plugins/Monolith/
  Monolith.uplugin
  README.md
  LICENSE                          (MIT)
  ATTRIBUTION.md                   (Credits: Codeturion concept, tumourlove originals)
  .gitignore
  Config/
    MonolithSettings.ini
  Docs/
    plans/
      2026-03-06-monolith-design.md
      2026-03-06-monolith-implementation-plan.md
      phase-3-animation-niagara.md
  Plans/
    Phase6_Skills_Templates_Polish.md
  Skills/
    unreal-animation/unreal-animation.md
    unreal-blueprints/unreal-blueprints.md
    unreal-build/unreal-build.md
    unreal-cpp/unreal-cpp.md
    unreal-debugging/unreal-debugging.md
    unreal-materials/unreal-materials.md
    unreal-niagara/unreal-niagara.md
    unreal-performance/unreal-performance.md
    unreal-project-search/unreal-project-search.md
    unreal-ui/unreal-ui.md
  Templates/
    .mcp.json.example
    .mcp.json.proxy.example
  Scripts/
    source_indexer/                (LEGACY: Python tree-sitter indexer — superseded by C++ indexer in MonolithSource)
      db/schema.py
      ...
  MCP/
    pyproject.toml                 (Package scaffold — CLI is unimplemented stub)
    src/monolith_source/
  Source/
    MonolithCore/                  (8 source files)
    MonolithBlueprint/             (4 source files)
    MonolithMaterial/              (4 source files)
    MonolithAnimation/             (6 source files — includes PoseSearch)
    MonolithNiagara/               (4 source files)
    MonolithEditor/                (4 source files)
    MonolithConfig/                (4 source files)
    MonolithIndex/                 (12+ source files)
    MonolithSource/                (8 source files)
    MonolithUI/                    (17+ source files — UMG baseline + CommonUI categories A-I, conditional on WITH_COMMONUI)
    MonolithGAS/                   (conditional on WITH_GBA — abilities, attributes, effects, ASC, tags, cues, targets, input, inspect, scaffold)
    MonolithComboGraph/            (conditional on WITH_COMBOGRAPH — combo graph CRUD, nodes, edges, effects, cues, ability scaffolding)
    MonolithAI/                    (conditional on WITH_STATETREE + WITH_SMARTOBJECTS — BT, BB, ST, EQS, SO, Controllers, Perception, Navigation, Runtime, Scaffolds)
    MonolithLogicDriver/           (conditional on WITH_LOGICDRIVER — SM CRUD, graph read/write, node config, runtime/PIE, JSON spec, scaffolding, discovery, components, text graph)
  Tools/
    MonolithProxy/                   (MCP stdio-to-HTTP proxy source + build.bat)
    MonolithQuery/                   (Offline query tool source + build.bat)
  Binaries/
    monolith_proxy.exe               (Compiled MCP proxy — replaces Python proxy)
    monolith_query.exe               (Compiled offline query tool — replaces MonolithQueryCommandlet)
  Saved/
    .gitkeep
    monolith_offline.py              (Legacy offline CLI — superseded by monolith_query.exe)
    EngineSource.db                  (Engine source index, ~1.8GB — not in git)
    ProjectIndex.db                  (Project asset index — not in git)
```

---

## 10. Deployment

### Development & Release Workflow

Everything lives in one place: `YourProject/Plugins/Monolith/`

This folder is both the working copy and the git repo (`git@github.com:tumourlove/monolith.git`). Edit, build, commit, push, and release all happen here — no file copying.

#### Publishing a release

1. Bump version in `Source/MonolithCore/Public/MonolithCoreModule.h` (`MONOLITH_VERSION`) and `Monolith.uplugin` (`VersionName`)
2. Update `CHANGELOG.md`
3. UBT build (bakes version into DLLs)
4. `git add -A && git commit && git push origin master`
5. Create zip: `powershell -ExecutionPolicy Bypass -File Scripts/make_release.ps1 -Version "X.Y.Z"` (excludes Intermediate/Saved/.git, sets `"Installed": true` for BP-only users)
6. `gh release create vX.Y.Z "../Monolith-vX.Y.Z.zip" --title "..." --notes "..."`

**Important:** Release zips MUST include pre-compiled DLLs (`Binaries/Win64/*.dll`) so Blueprint-only users can use the plugin without rebuilding. The `make_release.ps1` script sets `"Installed": true` in the zip's `.uplugin` to suppress rebuild prompts. The local dev copy keeps `"Installed": false`.

#### Auto-updater flow

1. On editor startup (5s delay), checks `api.github.com/repos/tumourlove/monolith/releases/latest`
2. Compares `tag_name` semver against compiled `MONOLITH_VERSION`
3. If newer: shows a dialog window with full release notes + "Install Update" / "Remind Me Later"
4. Download stages to `Saved/Monolith/Staging/` (NOT Plugins/ — would cause UBT conflicts)
5. On editor exit, a detached swap script runs:
   - Polls `tasklist` for `UnrealEditor.exe` until it's gone (120s timeout)
   - Asks for user confirmation (Y/N)
   - `move` command with retry loop (10 attempts × 3s) to handle Defender/Indexer file locks
   - `xcopy /h` copies new version, preserves `.git/`, `.gitignore`, `.github/`
   - Rollback on failure: removes partial copy, restores backup
   - Shows conditional message: C++ users rebuild, BP-only users launch immediately

### Installation (for other projects)

1. Clone to `YourProject/Plugins/Monolith`
2. Copy `Templates/.mcp.json.example` to project root as `.mcp.json`
3. Launch editor — Monolith auto-starts and indexes
4. Optionally copy `Skills/*` to `~/.claude/skills/`

---

## 11. Known Issues & Workarounds

See `TODO.md` for the full list. Key architectural constraints:

- **6 reimplemented NiagaraEditor helpers** — NiagaraEditor APIs not exported by Epic; Monolith reimplements them locally
- **SSE is stub-only** — GET endpoint returns single event and close, not full streaming
- **MaterialExpressionNoise fails on Lumen card passes** — Compiles for base pass but errors on Lumen card capture shaders ("function signature unavailable"). Engine limitation, not a Monolith bug. Workaround: use custom HLSL noise or pre-baked noise textures instead.
- **MaterialExpressionRadialGradientExponential does not exist** — Despite appearing in some community references, this expression class is not in UE 5.7. Use a Custom HLSL node with `pow(1.0 - saturate(length(UV - 0.5) * 2.0), Exponent)` instead.

### Recent Fixes (Phase J — shipped in 0.14.7)

- **F1 (2026-04-26) — BT crash hardening (`MonolithAIBehaviorTreeActions.cpp`).** Five `ai::add_bt_*` actions and `build_behavior_tree_from_spec` now reject Task-under-Root parenting at the API entry point via `ValidateParentForChildTask` helper and a schema-checked `ConnectParentChild`. Root cause: `UBehaviorTreeGraphNode_Root::NodeInstance` is `nullptr` by engine design; wiring a Task there produced a malformed graph that crashed `UBehaviorTreeGraph::UpdateAsset()` at `BehaviorTreeGraph.cpp:517` (`RootNode->Children.Reset()` on null `BTAsset->RootNode`). Hardened sites: `add_bt_node` (Task class), `add_bt_run_eqs_task`, `add_bt_smart_object_task`, `add_bt_use_ability_task`, `move_bt_node` (Task target), `build_behavior_tree_from_spec` (Task root). `reorder_bt_children` audited, structurally safe (permutation of existing children only). Investigation: `Docs/research/2026-04-26-bt-ability-task-crash-investigation.md`. Plan: `Docs/plans/2026-04-26-monolith-j-phase-fix-plan.md` §F1. Tests: `Plugins/Monolith/Docs/testing/2026-04-26-j2-bt-gas-ability-task-test.md` Failure Modes §Phase F1 (J2-AB-Crash-01..09).
- **F2 (2026-04-26) — `gas::bind_widget_to_attribute` rejects unknown `owner_resolver` (`MonolithGASUIBindingActions.cpp`).** `ParseOwner` was a `EMonolithAttrBindOwner`-returning function whose fall-through silently coerced any unrecognized string (e.g. `owner_resolver="banana"`) to `OwningPlayerPawn`. Refactored to `bool ParseOwner(S, OutOwner, OutSocketTag, OutError)`. Empty input still defaults to `OwningPlayerPawn` (back-compat); any non-empty string that matches none of `[owning_player_pawn, owning_player_state, owning_player_controller, self_actor, named_socket:<tag>]` now returns an `FMonolithActionResult::Error` with the full valid-list enumeration. Socket-tag extraction collapsed into the validator (single source of truth — call site no longer re-splits `OwnerStr` on `:`). Investigation: `Docs/research/2026-04-26-j1-gas-binding-findings.md` §A.1. Plan: `Docs/plans/2026-04-26-monolith-j-phase-fix-plan.md` §F2.
- **F3 (2026-04-26) — `gas::bind_widget_to_attribute` rejects `format=format_string` templates missing required slots (`MonolithGASUIBindingActions.cpp`).** New helper `ValidateFormatStringPayload(Payload, bHasMaxAttribute, OutError)` enforces that `{0}` is present whenever `format=format_string` is selected, and additionally `{1}` whenever `max_attribute` is bound. Both bare slot (`{0}`) and typed-slot (`{0:int}`, `{1:int}`) forms are accepted. Two guard sites in `HandleBindWidgetToAttribute`: (1) immediately after `ParseFormat` (catches the user-supplied `format=format_string:NoSlots` case); (2) after `ValidateWidgetProperty` (catches `format=auto` auto-promoted to `FormatString` for Text widgets with `max_attribute` bound but no template — distinct error message instructing the caller to pass an explicit `format_string:<template>`). Previously such inputs persisted silently and produced constant-string runtime values that never reflected the attribute. Investigation: `Docs/research/2026-04-26-j1-gas-binding-findings.md` §A.2. Plan: `Docs/plans/2026-04-26-monolith-j-phase-fix-plan.md` §F3.
- **F4 (2026-04-26) — `ULeviathanVitalsSet` shipped as production C++ (`Source/Leviathan/Public/GAS/LeviathanVitalsSet.h` + `.cpp`).** Canonical vitals AttributeSet for the Leviathan project: `Health` / `MaxHealth`, `Sanity` / `MaxSanity`, `Stamina` / `MaxStamina` (six `FGameplayAttributeData`, all default 100, all `BlueprintReadOnly`, replicated `REPNOTIFY_Always`). Standard GAS pattern via `ATTRIBUTE_ACCESSORS` macro. `PreAttributeChange` clamps current values into `[0, Max]`, Max attributes floor at 1. `PostGameplayEffectExecute` re-clamps base values defensively after instant executes and re-clamps `Current <= Max` when a Max attribute changes downward. Build.cs gained `GameplayAbilities` + `GameplayTasks` public deps (`GameplayTags` was already present). Resolves the J1 test-spec prerequisite that previously demanded a disposable BP fallback at `/Game/Tests/Monolith/AS_TestVitals` — bind targets `ULeviathanVitalsSet.<Attr>` are now first-class. Eldritch resistance attributes (`BleedResistance`, `PossessionResistance`, `RotResistance`) DEFERRED to horror-system spec per project scope decision. New spec: `Docs/specs/SPEC_Vitals.md`. Plan: `Docs/plans/2026-04-26-monolith-j-phase-fix-plan.md` §F4.
- **F5 (2026-04-26) — J1 GAS UI binding response-shape & error-message drift cleanup (`MonolithGASUIBindingActions.cpp`).** Six impl changes that bring response shapes and error text into alignment with the J1 test spec: (1) `SerializeBindingRow` field `index` renamed to `binding_index` (matches the bind-response field name; previously the only inconsistency between bind and list outputs); (2) composite `attribute` and `max_attribute` strings (`"<ClassShortName>.<PropertyName>"`, derived via `FPackageName::ObjectPathToObjectName`) added alongside the existing split `attribute_set_class` + `attribute_name` (and `max_attribute_*`) fields, giving callers round-trip parity with the bind-input contract while keeping split fields for back-compat; (3) `widget_class` field added to each list-row by looking up the widget in the WBP tree (parity with the bind response); (4) `removed_binding_index` field added to the unbind response, captured pre-removal via `IndexOfBinding` (no `RemoveBinding` refactor needed); (5) two error sites enriched with valid-options enumerations — the "Widget '...' not found" site now appends `Available: [...]` listing widget-tree variable names (sorted, capped at 20 with a SPEC pointer beyond the cap) via new helper `BuildAvailableWidgetsClause`, and the "Unsupported (widget=...)" site rewritten as `"Property '...' invalid for <class>. Valid: [...]"` via new helper `BuildValidPropertiesClause` mirroring `ValidateWidgetProperty`'s accept branches; (6) `LoadWBP` split into raw-load + Cast so callers can distinguish "asset path doesn't exist" (`"Widget Blueprint asset not found: <path>"`) from "asset is the wrong UClass" (`"Asset at <path> is not a Widget Blueprint (got <UClassName>)"`) — the type-checked `LoadAssetByPath` overload was conflating both. Investigation: `Docs/research/2026-04-26-j1-gas-binding-findings.md` §B. Plan: `Docs/plans/2026-04-26-monolith-j-phase-fix-plan.md` §F5.
- **F8 (2026-04-26) — five new MCP scaffolding actions for J-phase test specs (+5 → 1282 total).** Net adds across three modules:
  - `editor::create_empty_map` — new file `MonolithEditorMapActions.cpp/.h`. Creates a fully blank `UWorld` asset on disk via `UWorldFactory` + `IAssetTools::CreateAsset`, then `UPackage::SavePackage`. v1 supports `map_template="blank"` only — `vr_basic` / `thirdperson_basic` reserved (UE 5.7 templates are populated by editor-only template files, not factory-creatable). Resolves J3 §Setup #5 ("disposable test scene") which previously required manual File > New Level.
  - `editor::get_module_status` — same new file. Wraps `IPluginManager::GetDiscoveredPlugins` (one-pass module-name → plugin reverse-index) + `FModuleManager::IsModuleLoaded`. Returns `{ module_name, plugin_name, enabled, loaded, is_runtime, version? }` per row. Empty input list returns rows for all Monolith modules. Unknown module names return `enabled=false / loaded=false / plugin_name=""` without error so callers can probe optional modules. Resolves J3 §Setup #3 reference to a previously non-existent action.
  - `gas::grant_ability_to_pawn` — extended existing `MonolithGASScaffoldActions.cpp/.h`. Locates a pawn BP's `UAbilitySystemComponent` SCS node (or native ASC on the parent CDO), reflects over the ASC class for any `TArray<TSubclassOf<UGameplayAbility>>` UPROPERTY whose name contains "Ability" (matches the conventional `StartupAbilities` / `DefaultAbilities` pattern across most projects), appends the resolved class via `FScriptArrayHelper`, marks BP structurally modified, then `FKismetEditorUtilities::CompileBlueprint`. Stock `UAbilitySystemComponent` has NO startup-abilities array; the action returns a clear "subclass it and add the array" error in that case (project-agnostic — no `ULeviathanASC` assumption). Skips duplicates idempotently.
  - `ai::add_perception_to_actor` — new file `MonolithAIPerceptionScaffoldActions.cpp/.h`. The existing `ai::add_perception_component` is restricted to AIController BPs and accepts only a single `dominant_sense`; this F8 variant accepts ANY actor BP via `actor_bp_path` and a `senses` array (Sight, Hearing, Damage). Adds `UAIPerceptionComponent` via SCS if absent (matches `MonolithAIPerceptionActions:421-429` pattern), then for each sense uses `UAIPerceptionComponent::ConfigureSense` to register a `UAISenseConfig_<Sense>`. Optional `sight_radius` (default 1500, with 1.1× LoseSightRadius margin) and `hearing_range` (default 3000). Marks BP modified + compiles. Touch/Team/Prediction reserved for v2 — return clear error with supported list. Resolves J3 §Setup #5 "1 listener AI pawn" which previously required hand-authoring.
  - `ai::get_bt_graph` — extended existing `MonolithAIBehaviorTreeActions.cpp`. Distinct from `get_behavior_tree` (which returns a recursive nested tree). Walks `BT->BTGraph->Nodes` (the editor's `UEdGraph` node list) and emits a flat `{ node_id, node_class, node_name, parent_id, children[] }` array suitable for GUID-based single-node lookup. Root identified by `UBehaviorTreeGraphNode_Root::StaticClass()` IsA. Returns `root_id` for convenience. Same `WITH_EDITORONLY_DATA` + missing-graph fallback as `get_behavior_tree`. Resolves J2 §TC2.18 reference to the previously non-existent action.
  Module action-count delta: editor 20 → 22, gas 130 → 131, ai 229 → 231. All five actions self-contained, project-agnostic, editor-only (Monolith is editor-only). Plan: `Docs/plans/2026-04-26-monolith-j-phase-fix-plan.md` §F8. Investigation: `Docs/research/2026-04-26-j-spec-environment-findings.md` §C.
- **F9 (2026-04-25) — CDO save pipeline cradle/walker fixes (PR #39 by @danielandric, squash-merge `509d6dc` against `master @ fb05228`).** Four-mechanism fix in `MonolithBlueprintEditCradle.{h,cpp}` + call-site in `MonolithBlueprintCDOActions.cpp` (`HandleSetCDOProperty`):
  1. **Transient-outer reparent.** New `MonolithEditCradle::ReparentTransientInstancedSubobjects` walks the property tree and renames any `CPF_InstancedReference | CPF_PersistentInstance` leaf currently outered to `GetTransientPackage()` under `TargetObject` (`REN_DontCreateRedirectors | REN_NonTransactional`). Routes around `FJsonObjectConverter.cpp:964` defaulting `Outer = GetTransientPackage()` when its immediate container is a USTRUCT — the case that nulls `InputModifierSwizzleAxis` / `InputTriggerChordAction` slots inside `FEnhancedActionKeyMapping.Modifiers` / `.Triggers`. Called between the JSON write and `FireFullCradle` so the cradle's Pre/Post fires on correctly-outered subobjects. Closes the inline-subobject sub-case left after #29 (v0.14.3's recursive cradle).
  2. **Walker unification.** `FireCradleRecursive` and the new reparent walker collapse into a single `WalkObjectRefLeaves(Prop, ContainerPtr, Chain, TFunctionRef Visitor)` over struct / array / map / set property trees. `FireCradleRecursive` and `MayContainObjectRef` demoted out of the public header — no external callers.
  3. **`FMapProperty::ValueProp` double-offset fix.** `FMapProperty::LinkInternal` (`PropertyMap.cpp:226`) calls `ValueProp->SetOffset_Internal(MapLayout.ValueOffset)`, so the pre-refactor `ContainerPtrToValuePtr(GetValuePtr(i))` produced `PairPtr + 2*ValueOffset` (past the value slot). Unified walker passes `GetPairPtr(i)` as the shared container — `ContainerPtrToValuePtr` resolves Key to `PairPtr+0` (KeyProp keeps offset 0) and Value to `PairPtr+ValueOffset` correctly. Latent bug; not exercised by any shipping engine `UDataAsset` (no `TMap<X, FStructWithInstancedRef>` field), validated correct-by-construction against UE source. Synthetic test fixture is open follow-up.
  4. **Sparse iteration fix.** `FScriptMapHelper` / `FScriptSetHelper` use `TSparseArray`-backed storage; map / set walkers switched from `Helper.Num()` → `Helper.GetMaxIndex()` + `IsValidIndex(i)` per UE's `TScriptContainerIterator` contract (`UnrealType.h:4577` docs, `:4654` canonical advance). `Num()` silently skipped any valid entry whose internal index was past `Num` when holes existed.

  Net `+123 / −116` lines across 4 files in `MonolithBlueprint`. No new MCP actions; no public API surface change. **Caveat (REN_NonTransactional):** if the enclosing `FScopedTransaction` is undone, the JSON-written value reverts but the freshly-created subobjects remain outered to `TargetObject` as orphans (GC reclaims). Validated by author via two cold-restart round-trips (canonical `IMC_ReparentClean` with `InputModifierSwizzleAxis` + `InputTriggerChordAction`; fresh `IMC_TestRun_Fresh` with `InputModifierNegate` + `InputTriggerHold`) plus 10 pre-existing repaired `UInputAction` round-trips with no regressions. Local UBT clean (target up-to-date, 0.66s). **Follow-ups deferred:** synthetic map-walker test fixture (`UDataAsset` subclass with `TMap<FName, FStructWithInstancedRef>` field), poisoned-asset scan/repair tooling rework, `editor.delete_assets` `LoadAssetByPath` swap, project-wide `Helper.Num()` sparse-iteration sweep.
- **F6 (2026-04-26) — J1 UI-binding test spec aligned with as-shipped impl (`Plugins/Monolith/Docs/testing/2026-04-26-j1-ui-gas-binding-test.md`).** Three drift items where the impl is canonical and the spec was relaxed: (a) `warnings` field documented as OMITTED-when-empty (TC1.11 sample updated, comment added; previously demanded an always-present empty array — both shapes are valid JSON and the omit-when-empty pattern is a smaller payload); (b) the "Available sets: [...]" enumeration in the AttributeSet-class-not-found error dropped from spec — enumerating all `UAttributeSet` subclasses requires a full `TObjectIterator<UClass>` scan that grows with project size and yields a list too long to scan visually, so the terser `"AttributeSet class not found: <name>"` message is now canonical; (c) the Levenshtein "Did you mean: ?" suggestion on attribute-property typo replaced with documentation that the impl returns the FULL valid-property list, which handles ambiguous typos better and avoids per-miss string-distance compute. Plus the previously-undocumented `replaced: bool` field in the `bind_widget_to_attribute` response shape was added to TC1.11's sample with a note explaining it is set whenever the bind succeeds — `true` when an existing binding for the same `(widget_name, target_property)` pair was overwritten via `replace_existing=true` (default), `false` on first author. The TC1.12 list-response sample was also updated to show the split `attribute_set_class` / `attribute_name` / `max_attribute_*` fields the impl emits alongside the composite and to surface the `count` and `note` envelope fields. Plan: `Docs/plans/2026-04-26-monolith-j-phase-fix-plan.md` §F6.
- **F11 (2026-04-26) — `audio::bind_sound_to_perception` rejects four silent-accept input-validation seams (`MonolithAudioPerceptionActions.cpp`).** New anonymous-namespace pre-flight `ValidateBindingParams(Params, OutError)` runs at action entry BEFORE asset load and BEFORE any UserData mutation, mirroring the F2/F3 "Parse + Validate, THEN mutate" idiom: rejects `loudness < 0` (`"loudness must be >= 0"`), `max_range < 0` (`"max_range must be >= 0 (use 0 for listener default)"`), `tag.Len() > 255` (`"tag exceeds 255 characters"` — project soft-cap, not engine `NAME_SIZE=1024`), and unknown `sense_class` strings. The previous `ResolveSenseClass` walked `TObjectIterator<UClass>` with case-insensitive name match — but `"AISense_Sight".Equals("Sight", IgnoreCase)` is FALSE, so unknown-and-future inputs silently fell through to a Hearing default. Replaced with strict `ParseSenseClass(SenseStr, OutClass, OutError)` allowlist: `"Hearing"` / `"AISense_Hearing"` (case-insensitive) → Hearing; `"Sight"` / `"Damage"` / `"Touch"` / `"Team"` / `"Prediction"` → distinct `"sense_class '<X>' deferred to v2"` error so callers distinguish capability gaps from typos; everything else → `"Unsupported sense_class '<X>'. v1 supports: [Hearing]"`. `TObjectIterator` walk dropped entirely (was dead code given v1-Hearing-only scope and the silent-fallback bug it produced). Empty inputs preserved as back-compat defaults on both `tag` (NAME_None) and `sense_class` (Hearing). `.cpp`-only mutation, no header touched, Live Coding compatible. Investigation: `Docs/research/2026-04-26-j3-audio-validation-findings.md`. Tests: `Plugins/Monolith/Docs/testing/2026-04-26-j3-audio-ai-stimulus-test.md` Failure Modes §Phase F11 (J3-Validate-* — 12 rows: 10 fix-coverage + 2 back-compat).
- **F9 — Phase J logging unification (2026-04-26) — GAS UI-binding observability + log-category retirement (`MonolithGASUIBindingActions.cpp`, `MonolithGASUIBindingBlueprintExtension.cpp`, `MonolithGASAttributeBindingClassExtension.cpp` + `.h`).** (Note: distinct from the unrelated PR #39 "F9 — CDO save pipeline cradle/walker fixes" entry above.) Two changes: (1) **Category retirement.** The two file-static log categories `LogMonolithGASUIBinding` (in the runtime extension) and `LogMonolithGASUIBindingExt` (in the editor-side blueprint extension) were defensive over-design — they fragment grep visibility across the GAS module. Both `DEFINE_LOG_CATEGORY_STATIC` lines deleted; all 7 pre-existing UE_LOG sites in those two files now route to the parent `LogMonolithGAS` category (declared in `MonolithGASInternal.h:13`, defined in `MonolithGASModule.cpp:16`). Single-category convention now matches every other file in the module. (2) **Observability adds (8 new statements).** Five action-handler entry/exit logs in `MonolithGASUIBindingActions.cpp`: `BindWidget` success at `Log` (line 739, augmented with `replaced=<bool>` so a single grep distinguishes overwrite from first-author), `UnbindWidget` success at `Log` (line 794, includes `removed_index`), `ListBindings` at `Verbose` (line 836 — read-only and frequently called, demoted to keep shipping logs clean), `ClearBindings` success at `Log` (line 878). Three runtime-side logs in `MonolithGASAttributeBindingClassExtension.cpp`: per-fire `ApplyValue` trace at `Verbose` (line 362, includes raw_value/max/ratio), owner-resolution Verbose-deferring branch and Warning-escalation branch in `SubscribeRow` ASC-not-found path (lines 268 and 275). The Warning is gated by a new per-row 1-second grace window (`FActiveSub::FirstSubscribeAttemptTime` + `bGraceEscalated` latch added to the private struct in `MonolithGASAttributeBindingClassExtension.h`) so misconfigured rows surface in shipping logs exactly once after the owner-spawn race window closes. Note: the `FActiveSub` field additions are header changes — Live Coding alone is insufficient; orchestrator must run a full UBT build. Plan: `Docs/plans/2026-04-26-monolith-j-phase-fix-plan.md` §F9. Investigation: `Docs/research/2026-04-26-j1-gas-binding-findings.md` §C.
- **F14+F16 (2026-04-26) — J2 spec relaxed to match omit-when-empty handler shape; combat tag refs corrected to existing `Ability.Combat.Melee.Light/Heavy` registry (`Plugins/Monolith/Docs/testing/2026-04-26-j2-bt-gas-ability-task-test.md`, `2026-04-26-j2-results.md`).** F14: TC2.16/TC2.17 sample responses rewritten to document `ability_class`/`ability_tags` as mutually-exclusive (exactly one present), and `event_tag`/`node_name` as OMITTED when not supplied — matches the as-shipped `HandleAddBTUseAbilityTask` serialization (`MonolithAIBehaviorTreeActions.cpp:3367-3392`) per the F5/F6 ADR pattern (relax spec to match impl, not the other way around). F16: J2 spec swept of `Ability.Combat.Punch` and `Ability.Combat.Kick` references — both unregistered. Replaced with `Ability.Combat.Melee.Light` / `Ability.Combat.Melee.Heavy` (verified registered at `Config/DefaultGameplayTags.ini:26-27`); fixture abilities renamed `GA_Test_Punch`/`GA_Test_Kick` → `GA_Test_MeleeLight`/`GA_Test_MeleeHeavy`; rationale ("uses existing Melee.Light/Heavy registry tags; Punch/Kick deliberately not in tree per survival-horror curation") inlined into TC2.9/TC2.11/§Setup. J2 results doc historical Punch/Kick mentions annotated as superseded-by-F16 rather than rewritten (preserves test-execution record). Plan: `Docs/plans/2026-04-26-monolith-j-phase-fix-plan-v2.md` §F14 + §F16. Investigation: `Docs/research/2026-04-26-j-misc-drift-findings.md` §A.1 + §B.2.
- **F15 (2026-04-26) — invalid-GUID vs unknown-GUID error messages now distinct across 16 BT action call sites (`MonolithAIBehaviorTreeActions.cpp`).** Hoisted the open-coded `FindGraphNodeByGuid` + null-check pair into a new anonymous-namespace helper `RequireBtNodeByGuid(Graph, GuidStr, ParamName, BTName, OutNode, OutError) -> bool` (declared after the legacy `FindGraphNodeByGuid` at line 210). The legacy helper collapsed both `FGuid::Parse` failures and unknown-GUID lookup failures into the same `nullptr`, so 16 sibling sites all emitted the same opaque `"...not found"` message regardless of whether the caller typed garbage or a valid-but-unmatched GUID. New behavior: parse failure returns `"<ParamName> '<GuidStr>' is not a valid GUID"` (e.g. `"parent_id 'abc' is not a valid GUID"`); lookup failure returns `"No node with GUID '<GuidStr>' in BT '<BTName>'"`. Sites swept: `add_bt_node` (parent_id), `remove_bt_node`, `move_bt_node` (both node_id + new_parent_id), `add_bt_decorator`, `remove_bt_decorator`, `add_bt_service`, `remove_bt_service`, `set_bt_node_property`, `get_bt_node_properties`, `reorder_bt_children`, `add_bt_run_eqs_task`, `add_bt_smart_object_task`, `add_bt_use_ability_task`, `clone_bt_subtree` (both source `node_id` and `dest_parent_id`). All post-lookup validation (`ValidateParentForChildTask`, `Cast<UBTNode>` instance check, `Cast<UBehaviorTreeGraphNode_Root>` removal guard, etc.) preserved verbatim — the helper only replaces the parse + base-lookup steps. Empty-or-resolve sites (`add_bt_node`, `add_bt_run_eqs_task`, `add_bt_smart_object_task`, `add_bt_use_ability_task`) now also emit a clearer `"Root node not found in BT graph"` when the BT lacks a Root edge node. .cpp-only mutation, no header touched, Live Coding compatible. Note: research doc prose said "17 sites" but the actual line list and source-file grep both confirm **16** sites — drift between summary text and the line enumeration. Plan: `Docs/plans/2026-04-26-monolith-j-phase-fix-plan-v2.md` §F15. Investigation: `Docs/research/2026-04-26-j-misc-drift-findings.md` §A.2. Tests: `Plugins/Monolith/Docs/testing/2026-04-26-j2-bt-gas-ability-task-test.md` Failure Modes (`parent_id` not a valid GUID / valid GUID but not in this BT rows now satisfied verbatim).
- **F18 (2026-04-26) — new `audio::create_test_wave` action procedurally generates a sine-tone USoundWave for test fixtures (no asset deps) (+1 → 1283 total; MonolithAudio 81 → 82).** New action handler `FMonolithAudioAssetActions::CreateTestWave` in `MonolithAudioAssetActions.{h,cpp}`. Inputs: `path` (required, must be under `/Game/`), `frequency_hz` (default 440.0, range [20.0, 20000.0]), `duration_seconds` (default 0.5, range [0.05, 5.0]), `sample_rate` (default 44100, allowlist {22050, 44100, 48000}), `amplitude` (default 0.5, range (0.0, 1.0]). Recipe: validate path + numeric params at action entry (parse-then-mutate idiom); generate `int16` mono PCM samples filled with `amplitude * 32767 * sin(2π·f·t/SR)`; apply 256-sample linear fade-in/fade-out to suppress click; build canonical 44-byte RIFF/WAVE header + PCM payload in memory; `NewObject<USoundWave>` in destination package; set `NumChannels=1`, `SetSampleRate` (`WITH_EDITOR`), `Duration`, `TotalSamples`, `SoundGroup=SOUNDGROUP_Default`; write the WAV blob into `Wave->RawData` via the canonical `Lock(LOCK_READ_WRITE) → Realloc(Size) → FMemory::Memcpy → Unlock` pattern (mirrors `Engine/Source/Editor/AudioEditor/Private/Factories/SoundFactory.cpp::FactoryCreateBinary`); `InvalidateCompressedData(true, false)` so the cooker re-cooks; `FAssetRegistryModule::AssetCreated` + `UPackage::SavePackage`. Returns `{ asset_path, samples_written, duration_actual_seconds, frequency_hz, sample_rate, amplitude }`. Unblocks J3 TC3.19 (USoundWave direct binding) plus any future test that needs a disposable wave fixture (perception, attenuation, submix routing) — fully deterministic, reproducible across runs, project-agnostic. Note: `CreateTestWave` declaration added to private section of `MonolithAudioAssetActions.h` — header change requires full UBT build, Live Coding alone is insufficient. Build deps unchanged (`Engine` already provides `USoundWave`, `AudioEditor` already linked). Plan: `Docs/plans/2026-04-26-monolith-j-phase-fix-plan-v2.md` §F18. Investigation: `Docs/research/2026-04-26-j-misc-drift-findings.md` §B.3.
- **F22 (2026-04-26) — `MonolithAI.Build.cs` retrofit: SmartObjects + StateTree gated via 3-location detection (no longer hard-deps).** Lines 17-32 of the prior Build.cs hard-added `StateTreeModule`, `StateTreeEditorModule`, `GameplayStateTreeModule`, `PropertyBindingUtils`, `StructUtils`, `SmartObjectsModule`, `SmartObjectsEditorModule` to `PrivateDependencyModuleNames` and force-defined `WITH_STATETREE=1` + `WITH_SMARTOBJECTS=1`. The five backing engine plugins (StateTree, GameplayStateTree, PropertyBindingUtils, StructUtils, SmartObjects) all carry `EnabledByDefault: false` in their `.uplugin` manifests — end users on a fresh project install hit C1083 (missing headers) and LNK2019 (missing module exports) when loading the Monolith plugin without first enabling these engine plugins via the .uproject Plugins panel. Same shape as GitHub issue #30 where `MonolithMesh.dll` hard-linked `GeometryScriptingCore.dll`. Fix: two new conditional probe blocks (`bHasStateTree` + `bHasSmartObjects`) modeled on the existing `bHasGameplayAbilities` / GBA / CommonUI patterns. Each probes 3 locations (engine `Plugins/Runtime/<Plugin>/`, engine `Plugins/AI/<Plugin>/`, project `Plugins/<Plugin>/`) and honours `MONOLITH_RELEASE_BUILD=1` to force OFF for binary releases. Engine paths confirmed via direct disk inspection at `C:/Program Files (x86)/UE_5.7/Engine/Plugins/Runtime/{StateTree,SmartObjects,GameplayStateTree,PropertyBindingUtils}` and `Plugins/Experimental/StructUtils`. `bHasStateTree` adds all 5 StateTree-family modules together (StateTree's own `.uplugin` lists PropertyBindingUtils as a required dep, GameplayStateTree's `.uplugin` requires StateTree, StructUtils is mandatory for `FInstancedStruct` throughout StateTree's task/condition instance data — the four travel as a unit). `bHasSmartObjects` adds the 2 SmartObjects modules together. .cpp action sites already guarded — `#if WITH_STATETREE` / `#if WITH_SMARTOBJECTS` blocks present at: `MonolithAIRuntimeActions.cpp` (5 sites with `#else` stubs at lines 1172, 1234, 1335), `MonolithAIDiscoveryActions.cpp` (11 sites including a `#else` stub at line 1188 for `lint_state_tree`), `MonolithAIScaffoldActions.cpp` (6 sites with `#else` stub at line 1962 for `create_st_from_template`), full-file wrap at `MonolithAIStateTreeActions.{h,cpp}` and `MonolithAISmartObjectActions.{h,cpp}` — `RegisterActions` becomes empty when the macro is 0, so the 60+ StateTree + 16 SmartObjects actions simply do not register (dispatcher returns its own "unknown action" message). All `#if`/`#endif` pairing audited via grep, structurally sound. Build.cs change only — no .cpp modifications. Plan: `Docs/plans/2026-04-26-monolith-j-phase-fix-plan-v2.md` §F22. Reference: GitHub issue #30 (precedent fix for the same pattern in MonolithMesh). (StructUtils subsequently dropped from the gated set in the post-F22 deprecation cleanup, 2026-04-26 — `FInstancedStruct` relocated into CoreUObject in 5.5; the existing CoreUObject Public dep in `MonolithAI.Build.cs` covers the include resolution.)
- **F17 (2026-04-26) — `MonolithSource` auto-reindex on hot-reload (`MonolithSourceSubsystem.h` + `.cpp`).** `UMonolithSourceSubsystem::Initialize` now binds `FCoreUObjectDelegates::ReloadCompleteDelegate` and kicks `TriggerProjectReindex()` on every Live Coding patch and post-UBT hot-reload. Without this hook agents saw stale `source_query` results until someone called `source.trigger_project_reindex` manually — and since `monolith_reindex` is the **asset** indexer (not the source DB), the canonical recovery message had been confusing in spec docs. The new handler `OnReloadComplete(EReloadCompleteReason)` has three guards: (1) `bIsIndexing` re-entrancy guard — UBT can fire one signal per reloaded module in quick succession; (2) 5-second cooldown via new `LastReindexTimeSeconds` member (`FPlatformTime::Seconds()`); (3) bootstrap-skip if `EngineSource.db` doesn't yet exist — incremental reindex requires the engine symbols already in place, so the very-first-install case stays silent and waits for a manual `source.trigger_reindex`. `Deinitialize` unbinds via the new `ReloadCompleteHandle` member BEFORE indexer teardown so a late-firing reload signal can't re-enter into a half-destroyed subsystem. Build.cs unchanged — `CoreUObject` is already a Public dep, no `HotReload` module needed (the `FCoreUObjectDelegates` route is the project-precedent pattern, used in `Plugins/CarnageFX/Docs/plans/2026-04-16-engine-hacks-ranked.md` and `MonolithUI` plan §B for hot-reloaded UClass discovery). Note: the `OnReloadComplete` declaration + `ReloadCompleteHandle` + `LastReindexTimeSeconds` field additions are header changes — Live Coding alone is insufficient; orchestrator must run a full UBT build. SPEC update: `Plugins/Monolith/Docs/specs/SPEC_MonolithSource.md` (new "Auto-Reindex on Hot-Reload (F17)" section + class-table annotation). Plan: `Docs/plans/2026-04-26-monolith-j-phase-fix-plan-v2.md` §F17. Investigation: `Docs/research/2026-04-26-j-misc-drift-findings.md` §C.Layer1.

---

## 12. Action Count Summary

Counts below were re-verified against the live `monolith_discover()` registry on 2026-04-30 (pre-release audit). Where a module's spec previously claimed a different number, those per-module SPEC files were corrected. The Phase J F8 deltas (`editor` +2, `gas` +1, `ai` +2, `audio` +1), the Phase A–L MonolithUI deltas (+21 net: hoisted Design Import 5, Animation v2 core+events 5, EffectSurface 10, Spec Builder/Schema/Serializer 3, Type Registry diagnostic 1, Style Service diagnostic 1 conditional, plus 2 v1 actions tagged `[DEPRECATED]` but still registered), and the [Unreleased] editor `run_automation_tests` + `list_automation_tests` (+2) are reflected.

| Module | Namespace | Actions | Source-of-truth notes |
|--------|-----------|---------|------------------------|
| MonolithCore | monolith | 4 | discover, status, update, reindex |
| MonolithBlueprint | blueprint | 89 | |
| MonolithMaterial | material | 63 | |
| MonolithAnimation | animation | 118 | Includes 5 ABP write actions (`add_anim_graph_node`, `connect_anim_graph_pins`, `set_state_animation`, `add_variable_get`, `set_anim_graph_node_property`), 3 ControlRig write, 1 layout, plus 96 baseline + 13 PoseSearch |
| MonolithNiagara | niagara | 109 | 108 baseline + 1 layout (`auto_layout`) |
| MonolithMesh | mesh | 239 (194 core + 45 experimental town gen) | Town gen registered only when `bEnableProceduralTownGen=true` (default false) |
| MonolithEditor | editor | 24 | 20 base + 2 from `MonolithEditorMapActions` (Phase J F8: `create_empty_map`, `get_module_status`) + 2 from `MonolithEditorAutomationActions` ([Unreleased] PR #?? by @MaxenceEpitech: `list_automation_tests`, `run_automation_tests`) |
| MonolithConfig | config | 6 | |
| MonolithIndex | project | 7 | |
| MonolithSource | source | 11 | |
| MonolithUI | ui | 117 module-owned (66 always-on + 51 CommonUI under `WITH_COMMONUI`) + 4 GAS UI binding aliases (registered from `MonolithGAS`, conditional on `WITH_GBA`) = **121** distinct registrations into `ui::` in the full-stack configuration | Architecture expansion Phase A–L landed 2026-04-26: hoisted Design Import (5), Animation v2 core/events (5), EffectSurface actions (10), Spec Builder/Schema/Serializer (3), Type Registry diagnostic (1), Style Service diagnostic (1, `WITH_COMMONUI`). Optional EffectSurface provider decouple landed 2026-04-27 (Wave 1/2 + Final.1) — count unchanged; the 10 EffectSurface actions now return `-32010 ErrOptionalDepUnavailable` instead of crashing/erroring when the provider is absent. 2 v1 actions (`create_animation`, `add_animation_keyframe`) tagged `[DEPRECATED]` Phase L — still registered, scheduled for removal one major release out. Per-category roll-up in [`specs/SPEC_MonolithUI.md`](specs/SPEC_MonolithUI.md). The 4 GAS aliases (`bind_widget_to_attribute`, `unbind_widget_attribute`, `list_attribute_bindings`, `clear_widget_attribute_bindings`) come from `MonolithGAS/Private/MonolithGASUIBindingActions.cpp` and dispatch to the same handlers as their canonical `gas::` versions |
| MonolithGAS | gas | 135 | 131 documented in the Action Categories table + 4 UI binding actions (`bind_widget_to_attribute`, `unbind_widget_attribute`, `list_attribute_bindings`, `clear_widget_attribute_bindings`) — the latter four are also aliased into `ui` |
| MonolithComboGraph | combograph | 13 | |
| MonolithAI | ai | 221 | Phase J F8 added `add_perception_to_actor` and `get_bt_graph` — pre-J baseline was 219, not the previously documented 229 (per-category `~N` estimates in SPEC_MonolithAI were aspirational, not literal) |
| MonolithLogicDriver | logicdriver | 66 | |
| MonolithAudio | audio | 86 | 82 documented in the Action Categories table + 4 perception bind actions (`bind_sound_to_perception`, `unbind_sound_from_perception`, `get_sound_perception_binding`, `list_perception_bound_sounds`) |
| MonolithBABridge | — | 0 (integration only) | |
| **Total** | | **1275** registrations across 16 namespaces + 4 `monolith_*` meta = **1271 in-tree namespace actions** active by default (45 town-gen experimental disabled). With town gen registered: 1320. Without `WITH_COMMONUI`: 1220 (and 1265 with town gen); without `WITH_GBA`: 1140 (1185 w/ town gen); without both: 1089 (1134 w/ town gen) | The `ui` namespace double-counts 4 aliased GAS actions; **distinct** action handlers = **1267** in default-active configuration, **1316** with town gen registered |

**Note:** MonolithMesh includes 194 core actions (always registered) plus 45 experimental Procedural Town Generator actions (registered only when `bEnableProceduralTownGen = true`, default: false — known geometry issues). MonolithGAS is conditional on `#if WITH_GBA` — projects without GameplayAbilities register 0 GAS actions. MonolithComboGraph is conditional on `#if WITH_COMBOGRAPH` — projects without the ComboGraph plugin register 0 combograph actions. MonolithAI is conditional on `#if WITH_STATETREE` + `#if WITH_SMARTOBJECTS` — projects without these register 0 AI actions. MonolithLogicDriver is conditional on `#if WITH_LOGICDRIVER` — projects without Logic Driver Pro register 0 logicdriver actions. MonolithAudio MetaSound actions are conditional on `#if WITH_METASOUND` — projects without MetaSound get Sound Cue + CRUD + batch actions but no MetaSound graph building. MonolithUI includes 66 always-on actions (Widget CRUD + Slot + Templates + Styling + v1 Animation + v2 hoisted Animation + Bindings + Settings + Accessibility + Hoisted Design Import + EffectSurface + Spec Builder + Type Registry diagnostic) plus 51 CommonUI actions (50 in `Source/MonolithUI/Private/CommonUI/*.cpp` + 1 inline `dump_style_cache_stats` lambda in `MonolithUIModule.cpp`, all registered only when `WITH_COMMONUI=1`). Projects without CommonUI register 66 `ui::` actions; the full-stack configuration registers 117. The Phase A–L architecture expansion (2026-04-26) added the Spec System (`build_ui_from_spec` / `dump_ui_spec_schema` / `dump_ui_spec`), Type Registry + per-type property allowlist, EffectSurface widget + sub-bag setters, and the dedup-driven Style Service. See [`specs/SPEC_MonolithUI.md`](specs/SPEC_MonolithUI.md) for the full breakdown. MonolithBABridge registers no MCP actions — it only provides the `IMonolithGraphFormatter` IModularFeatures bridge consumed by `auto_layout` in the blueprint, material, animation, and niagara modules. The original Python server had higher tool counts (~231 tools) due to fragmented action design — Monolith consolidates these into 19 MCP tools with namespaced actions.

---

<a id="pipelines"></a>

## 13. Pipelines — Cross-Module Workflow Chains

Authoring complex assets typically requires calling a sequence of actions in order. The chains below are the canonical flows for each asset type. Where a "build from spec" shortcut exists, prefer it — spec-based builders are transactional and handle validation, connection resolution, and rollback in a single call.

### 13.1 Materials

Build a material from an expression spec (create asset → populate graph → wire inputs → compile).

```
create_material → build_material_graph → connect_expressions → recompile_material
```

**Shortcut:** `build_material_graph` accepts a `graph_spec` that can populate the entire graph (expressions + connections) in a single call. Follow with `recompile_material`.

### 13.2 State Machines (LogicDriver)

Author a Logic Driver Pro state machine asset (create SM → add states and transitions → compile).

```
create_state_machine → add_state (×N) → add_transition (×N) → compile_state_machine
```

**Shortcut:** `build_sm_from_spec` accepts a JSON spec describing all states, transitions, and node config, then compiles — one call instead of N.

### 13.3 Sound Cues

Author a Sound Cue graph (create asset → add nodes → connect → designate root).

```
create_sound_cue → add_sound_cue_node (×N) → connect_sound_cue_nodes (×N) → set_sound_cue_first_node
```

**Shortcut:** `build_sound_cue_from_spec` accepts a full graph spec (nodes + connections + root) and emits the finished Sound Cue in one call.

### 13.4 MetaSounds

Author a MetaSound Source graph via the Builder API (create asset → add nodes → connect).

```
create_metasound_source → add_metasound_node (×N) → connect_metasound_nodes (×N)
```

**Shortcut:** `build_metasound_from_spec` accepts a full graph spec and produces a compiled MetaSound Source in one call. Conditional on `#if WITH_METASOUND`.

### 13.5 Town Generation

**EXPERIMENTAL** — off by default via `bEnableProceduralTownGen`. Fundamental geometry issues remain (wall misalignment, room separation per MEMORY.md). Actions are not registered unless the setting is explicitly enabled. Use core mesh actions (sweep walls, blueprint prefabs, collision-aware scatter) for production work.

Per-building authoring (floor plan → building shell → facade → roof → register into city):

```
generate_floor_plan → create_building_from_grid → generate_facade → generate_roof → register_building
```

**Shortcut:** `create_city_block` wraps the full per-building chain across multiple buildings within a block boundary.

### Deep-linking

External docs can deep-link to this section via `[SPEC_CORE §Pipelines](Plugins/Monolith/Docs/SPEC_CORE.md#pipelines)`.
