# Monolith — Technical Specification

**Version:** 0.14.3 (Beta)
**Wiki:** https://github.com/tumourlove/monolith/wiki
**Engine:** Unreal Engine 5.7+
**Platform:** Windows, macOS, Linux
**License:** MIT
**Author:** tumourlove
**Repository:** https://github.com/tumourlove/monolith

---

## 1. Overview

Monolith is a unified Unreal Engine editor plugin that consolidates 9 separate MCP (Model Context Protocol) servers and 4 C++ plugins into a single plugin with an embedded HTTP MCP server. It reduces ~220 individual tools down to 19 MCP tools (1227 total actions across 16 domains; 1182 active by default — 45 experimental town gen actions disabled), cutting AI assistant context consumption by ~95%. A CommonUI action pack (~50 actions, conditional on `WITH_COMMONUI`) is planned for a future release.

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
  MonolithAnimation     — Animation sequences, montages, ABPs, curves, notifies, skeletons, PoseSearch (116 actions)
  MonolithNiagara       — Niagara particle systems, HLSL module/function creation, DI config, event handlers, sim stages, NPC, effect types, scalability (108 actions)
  MonolithEditor        — Build triggers, live compile, log capture, compile output, crash context, scene capture, texture import, GIF capture (20 actions)
  MonolithConfig        — Config/INI resolution and search (6 actions)
  MonolithIndex         — SQLite FTS5 deep project indexer, 14 internal indexers (7 MCP actions)
  MonolithSource        — Engine source + API lookup (11 actions)
  MonolithUI            — Widget blueprint CRUD, templates, styling, animation, settings scaffolding, accessibility (42 UMG actions). A CommonUI action pack (activatable widgets, buttons+styling, input glyphs, focus/nav, lists/tabs, dialogs, audit/lint, accessibility) is planned for a future release.
  MonolithMesh          — Mesh inspection, scene manipulation, spatial queries, level blockout, GeometryScript ops, horror/accessibility, lighting, audio/acoustics, performance, decals, level design, tech art, context props, procedural geometry (sweep walls, auto-collision, proc mesh caching, blueprint prefabs), genre presets, encounter design, accessibility reports (197 core actions) + EXPERIMENTAL procedural town generator (45 actions, disabled by default via bEnableProceduralTownGen) = 242 total
  MonolithGAS           — Gameplay Ability System integration: abilities, attributes, effects, ASC, tags, cues, targets, input, inspection, scaffolding (130 actions). Conditional on #if WITH_GBA
  MonolithComboGraph    — ComboGraph plugin integration: combo graph CRUD, node/edge management, effects, cues, ability scaffolding (13 actions). Conditional on #if WITH_COMBOGRAPH
  MonolithAI            — AI asset manipulation: Behavior Trees, Blackboards, State Trees, EQS, Smart Objects, AI Controllers, Perception, Navigation, Runtime/PIE, Scaffolds, Discovery, Advanced (229 actions). Conditional on #if WITH_STATETREE, #if WITH_SMARTOBJECTS (required); #if WITH_MASSENTITY, #if WITH_ZONEGRAPH (optional)
  MonolithLogicDriver   — Logic Driver Pro integration: SM CRUD, graph read/write, node config, runtime/PIE, JSON spec, scaffolding, discovery, components, text graph (66 actions). Conditional on #if WITH_LOGICDRIVER
  MonolithAudio         — Audio asset creation, inspection, batch management, Sound Cue graph building, MetaSound graph building via Builder API (81 actions). MetaSound conditional on #if WITH_METASOUND
  MonolithBABridge      — Optional IModularFeatures bridge for Blueprint Assist integration. Exposes IMonolithGraphFormatter; enables BA-powered auto_layout across blueprint, material, animation, and niagara modules when Blueprint Assist is present (0 MCP actions — integration only)
```

**Custom sibling plugins (not inside core Monolith — source + per-module specs are private to their respective repos):**
- **MonolithISX** — InventorySystemX integration, 158 actions, `inventory` namespace, conditional on InventorySystemX. Extracted 2026-04-21 to `Plugins/MonolithISX/`. Totals above do NOT include these 158 actions.
- **MonolithSteamBridge** — Steam Integration Kit integration, 28 actions, `steam` namespace, conditional on SIK. Lives at `Plugins/MonolithSteamBridge/` (+ `Plugins/MonolithSteamBridgeLeaderboard/` for full-fidelity leaderboard fidelity). Totals above do NOT include these 28 actions.

For the architectural pattern that lets you write your own sibling plugin and register actions into Monolith's MCP registry from outside the core repo, see [`SIBLING_PLUGIN_GUIDE.md`](SIBLING_PLUGIN_GUIDE.md).

### Discovery/Dispatch Pattern

All domain modules register actions with `FMonolithToolRegistry` (central singleton). Each domain exposes a single `{namespace}_query(action, params)` MCP tool. The 4 core tools (`monolith_discover`, `monolith_status`, `monolith_reindex`, `monolith_update`) are standalone. Conditional modules gate registration on compile-time defines: MonolithGAS (`#if WITH_GBA`), MonolithComboGraph (`#if WITH_COMBOGRAPH`), MonolithLogicDriver (`#if WITH_LOGICDRIVER`), MonolithAI (`#if WITH_STATETREE` + `#if WITH_SMARTOBJECTS` required; `#if WITH_MASSENTITY` + `#if WITH_ZONEGRAPH` optional), MonolithAudio (MetaSound actions conditional on `#if WITH_METASOUND`).

### MCP Protocol

- **Protocol version:** Echoes client's requested version; supports both `2024-11-05` and `2025-03-26` (defaults to `2025-03-26`)
- **Transport:** HTTP with JSON-RPC 2.0 (POST for requests, GET for SSE stub, OPTIONS for CORS). Transport type in `.mcp.json` varies by client: `"http"` for Claude Code, `"streamableHttp"` for Cursor/Cline
- **Endpoint:** `http://localhost:{port}/mcp` (default port 9316)
- **Bind retry:** `FMonolithHttpServer::Start()` attempts up to 5 binds with exponential backoff and TCP port probe before failing. `Restart()` method available for runtime recovery. Console command: `Monolith.Restart`
- **Batch support:** Yes (JSON-RPC arrays)
- **Session management:** None — server is fully stateless (session tracking removed; no per-session state was ever stored)
- **CORS:** `Access-Control-Allow-Origin: *`

### Module Loading

| Module | Loading Phase | Type |
|--------|--------------|------|
| MonolithCore | PostEngineInit | Editor |
| All others (16) | Default | Editor (MonolithBABridge is optional — empty shell when Blueprint Assist absent) |

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
| MonolithAI | StateTree, SmartObjects | `WITH_STATETREE`, `WITH_SMARTOBJECTS` (required); `WITH_MASSENTITY`, `WITH_ZONEGRAPH` (optional) | `MonolithAI.Build.cs` | (existing) |

---

## 3. Module Reference

Each module has its own spec file under `specs/`. The table below is the index.

| # | Module | Spec | Summary |
|---|--------|------|---------|
| 3.1 | MonolithCore | [specs/SPEC_MonolithCore.md](specs/SPEC_MonolithCore.md) | HTTP server (bind retry, Restart(), `Monolith.Restart` console cmd), tool registry, discovery, settings, auto-updater |
| 3.2 | MonolithBlueprint | [specs/SPEC_MonolithBlueprint.md](specs/SPEC_MonolithBlueprint.md) | Blueprint inspection, variable/component/graph CRUD, node ops, compile, spawn (89 actions) |
| 3.3 | MonolithMaterial | [specs/SPEC_MonolithMaterial.md](specs/SPEC_MonolithMaterial.md) | Material inspection + graph editing + CRUD + function suite (63 actions) |
| 3.4 | MonolithAnimation | [specs/SPEC_MonolithAnimation.md](specs/SPEC_MonolithAnimation.md) | Animation sequences, montages, ABPs, curves, notifies, skeletons, PoseSearch (116 actions) |
| 3.5 | MonolithNiagara | [specs/SPEC_MonolithNiagara.md](specs/SPEC_MonolithNiagara.md) | Niagara particle systems, HLSL module/function, DI config, event handlers, sim stages (108 actions) |
| 3.6 | MonolithEditor | [specs/SPEC_MonolithEditor.md](specs/SPEC_MonolithEditor.md) | Build triggers, live compile, log capture, crash context, scene capture (20 actions) |
| 3.7 | MonolithConfig | [specs/SPEC_MonolithConfig.md](specs/SPEC_MonolithConfig.md) | Config/INI resolution and search (6 actions) |
| 3.8 | MonolithIndex | [specs/SPEC_MonolithIndex.md](specs/SPEC_MonolithIndex.md) | SQLite FTS5 deep project indexer (7 MCP actions, 14 internal indexers) |
| 3.9 | MonolithSource | [specs/SPEC_MonolithSource.md](specs/SPEC_MonolithSource.md) | Engine source + API lookup (11 actions) |
| 3.10 | MonolithUI | [specs/SPEC_MonolithUI.md](specs/SPEC_MonolithUI.md) | Widget blueprint CRUD, templates, styling, accessibility (42 UMG actions; CommonUI pack planned) |
| 3.11 | MonolithMesh | [specs/SPEC_MonolithMesh.md](specs/SPEC_MonolithMesh.md) | Mesh/scene/spatial/blockout/GeometryScript/procedural (197 core + 45 experimental town gen = 242 actions) |
| 3.12 | MonolithBABridge | [specs/SPEC_MonolithBABridge.md](specs/SPEC_MonolithBABridge.md) | IModularFeatures bridge for Blueprint Assist (0 MCP actions, integration only) |
| 3.13 | MonolithGAS | [specs/SPEC_MonolithGAS.md](specs/SPEC_MonolithGAS.md) | Gameplay Ability System integration (130 actions, WITH_GBA) |
| 3.14 | MonolithComboGraph | [specs/SPEC_MonolithComboGraph.md](specs/SPEC_MonolithComboGraph.md) | ComboGraph integration (13 actions, WITH_COMBOGRAPH) |
| 3.15 | MonolithLogicDriver | [specs/SPEC_MonolithLogicDriver.md](specs/SPEC_MonolithLogicDriver.md) | Logic Driver Pro integration (66 actions, WITH_LOGICDRIVER) |
| 3.16 | MonolithAI | [specs/SPEC_MonolithAI.md](specs/SPEC_MonolithAI.md) | Behavior Trees, Blackboards, State Trees, EQS, Smart Objects, Perception, Nav (229 actions) |
| 3.17 | MonolithAudio | [specs/SPEC_MonolithAudio.md](specs/SPEC_MonolithAudio.md) | Sound Cues, MetaSounds, batch audio ops (81 actions, MetaSound WITH_METASOUND) |

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
| unreal-ui | UI, HUD, widget, menu, settings, save game, accessibility, font, toast, dialog | `ui_query()` | 42 |

All skills follow a common structure: YAML frontmatter, Discovery section, Asset Path Conventions table, action tables, workflow examples, and rules.

---

## 7. Configuration

**Settings location:** Editor Preferences > Plugins > Monolith
**Config file:** `Config/MonolithSettings.ini` section `[/Script/MonolithCore.MonolithSettings]`

| Setting | Default | Description |
|---------|---------|-------------|
| ServerPort | 9316 | MCP HTTP server port |
| bAutoUpdateEnabled | True | GitHub Releases auto-check on startup |
| DatabasePathOverride | (empty) | Override default DB path (Plugins/Monolith/Saved/) |
| EngineSourceDBPathOverride | (empty) | Override engine source DB path |
| EngineSourcePath | (empty) | Override engine source directory |
| bBlueprintEnabled | True | Enable Blueprint module |
| bMaterialEnabled | True | Enable Material module |
| bAnimationEnabled | True | Enable Animation module |
| bNiagaraEnabled | True | Enable Niagara module |
| bEditorEnabled | True | Enable Editor module |
| bConfigEnabled | True | Enable Config module |
| bIndexEnabled | True | Enable Index module |
| bSourceEnabled | True | Enable Source module |
| bUIEnabled | True | Enable UI module |
| bMeshEnabled | True | Enable Mesh module (core actions) |
| bGASEnabled | True | Enable GAS module (requires GameplayAbilities plugin; no-op if `WITH_GBA=0`) |
| bEnableProceduralTownGen | **False** | Enable Procedural Town Generator actions (45 actions). Requires `bMeshEnabled`. **Work-in-progress** — known geometry issues, disabled by default. Unless you're willing to dig in and help improve it, best left alone |
| bEnableBlueprintAssist | True | Allow MonolithBABridge to register IMonolithGraphFormatter when Blueprint Assist is present. Set false to force built-in layout for all auto_layout calls |
| LogVerbosity | 3 (Log) | 0=Silent, 1=Error, 2=Warning, 3=Log, 4=Verbose |

**Note:** Module enable toggles are functional — each module checks its toggle at registration time and skips action registration if disabled.

---

## 8. Templates

| File | Purpose |
|------|---------|
| `Templates/.mcp.json.example` | Minimal MCP config. Transport type varies by client: `"http"` for Claude Code, `"streamableHttp"` for Cursor/Cline. URL: `http://localhost:9316/mcp` |
| `Templates/CLAUDE.md.example` | Project instructions template with tool reference, workflow, asset path conventions, and rules |

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
    CLAUDE.md.example
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
    MonolithUI/                    (17 source files — 9 .cpp + 8 .h)
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

---

## 12. Action Count Summary

| Module | Namespace | Actions |
|--------|-----------|---------|
| MonolithCore | monolith | 4 |
| MonolithBlueprint | blueprint | 89 |
| MonolithMaterial | material | 63 |
| MonolithAnimation | animation | 116 |
| MonolithNiagara | niagara | 108 |
| MonolithMesh | mesh | 242 (197 core + 45 experimental town gen) |
| MonolithEditor | editor | 20 |
| MonolithConfig | config | 6 |
| MonolithIndex | project | 7 |
| MonolithSource | source | 11 |
| MonolithUI | ui | 42 |
| MonolithGAS | gas | 130 |
| MonolithComboGraph | combograph | 13 |
| MonolithAI | ai | 229 |
| MonolithLogicDriver | logicdriver | 66 |
| MonolithAudio | audio | 81 |
| MonolithBABridge | — | 0 (integration only) |
| **Total** | | **1227** (1182 active by default) |

**Note:** MonolithMesh includes 197 core actions (always registered) plus 45 experimental Procedural Town Generator actions (registered only when `bEnableProceduralTownGen = true`, default: false — known geometry issues). MonolithGAS is conditional on `#if WITH_GBA` — projects without GameplayAbilities register 0 GAS actions. MonolithComboGraph is conditional on `#if WITH_COMBOGRAPH` — projects without the ComboGraph plugin register 0 combograph actions. MonolithAI is conditional on `#if WITH_STATETREE` + `#if WITH_SMARTOBJECTS` — projects without these register 0 AI actions. MonolithLogicDriver is conditional on `#if WITH_LOGICDRIVER` — projects without Logic Driver Pro register 0 logicdriver actions. MonolithAudio MetaSound actions are conditional on `#if WITH_METASOUND` — projects without MetaSound get Sound Cue + CRUD + batch actions but no MetaSound graph building. MonolithBABridge registers no MCP actions — it only provides the `IMonolithGraphFormatter` IModularFeatures bridge consumed by `auto_layout` in the blueprint, material, animation, and niagara modules. The original Python server had higher tool counts (~231 tools) due to fragmented action design — Monolith consolidates these into 19 MCP tools with namespaced actions.

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
