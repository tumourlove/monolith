# Monolith — Technical Specification

**Version:** 0.21.3 (Beta)
**Wiki:** https://github.com/tumourlove/monolith/wiki
**Engine:** Unreal Engine 5.7+
**Platform:** Windows, macOS, Linux
**License:** MIT
**Author:** tumourlove
**Repository:** https://github.com/tumourlove/monolith

---

## 1. Overview

Monolith is a unified Unreal Engine editor plugin that consolidates 9 separate MCP (Model Context Protocol) servers and 4 C++ plugins into a single plugin with an embedded HTTP MCP server. It reduces ~220 individual tools down to a small set of MCP dispatcher tools (~1,400+ actions across 25+ in-tree namespaces — 45 experimental town gen actions disabled by default; query `monolith_discover()` for the live figure), cutting AI assistant context consumption by ~95%. The CommonUI action pack (51 actions, conditional on `WITH_COMMONUI`) shipped M0.5, v0.14.0 (2026-04-19), tested M0.5.1 (2026-04-25). Editor +2 (`run_automation_tests`, `list_automation_tests`) landed v0.14.8 — PR #48 by @MaxenceEpitech. v0.14.9 added Editor +2 (`run_python`, `load_level` — Issue #50, ported from @JCSopko's fork) and Animation +1 (`copy_bone_pose_between_sequences` — PR #51 by @MaxenceEpitech). v0.14.10 added Audio +12 MetaSound document introspection actions plus a new `FMetaSoundIndexer` deep indexer in MonolithIndex (PR #18 by @alakangas, refactored into existing `audio_query` namespace), AND Animation +1 (`list_bone_tracks`) + Editor +3 (`start_pie`, `stop_pie`, `run_console_command` with maintainer-pinned in-viewport mode + GEngine null-guard) — PR #54 by @MaxenceEpitech.

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
  MonolithBlueprint     — Blueprint inspection, variable/component/graph CRUD, node operations, compile, spawn, variable-contract reconciliation (~120+ actions)
  MonolithMaterial      — Material inspection + graph editing + CRUD + function suite + tiling quality + texture preview (~60+ actions)
  MonolithAnimation     — Animation sequences, montages, ABPs, curves, notifies, skeletons, PoseSearch, IK rig / retargeting (retarget pose + op-stack tuning + per-bone translation retargeting), locomotion authoring (root-motion speed, distance-curve baking), ABP/ControlRig write, anim-graph-authoring pack, layout (~170+ actions)
  MonolithNiagara       — Niagara particle systems, HLSL module/function creation, DI config, event handlers, sim stages, NPC, effect types, scalability, layout, temporal control, stateless-emitter factory, search & discovery (~120+ actions)
  MonolithEditor        — Build triggers, live compile, log capture, compile output, crash context, scene capture, texture import, flipbook stitching, asset deletion, viewport info, blank-map factory + module status (Phase J F8), automation test list/run, Python escape-hatch (`run_python`), map swap (`load_level`), PIE smoke/capture harness, deterministic PIE input/control + object introspection, stat-group readout (~50+ actions — query `monolith_discover("editor")` for the live figure)
  MonolithConfig        — Config/INI resolution and search (6 actions, fixed surface)
  MonolithIndex         — SQLite FTS5 deep project indexer, 18 internal indexers (incl. `FMetaSoundIndexer` from PR #18 by @alakangas — conditional on `WITH_METASOUND`) (~10+ MCP actions)
  MonolithSource        — Engine source + API lookup, auto-reindex on hot-reload (Phase J F17), LLM C++ authoring ergonomics (include/signature/deprecation/verify/example/lint/stub) (~20+ actions)
  MonolithUI            — Widget blueprint CRUD + slot/template/styling, animation v1 (deprecated) + v2 (hoisted), settings scaffolding, accessibility, UISpec build/dump/schema, EffectSurface sub-bag actions (reflective optional-provider probe, decoupled 2026-04-27), CommonUI categories A–I, Type Registry diagnostic, Style Service diagnostic. (66 always-on + 51 CommonUI = 117 module-owned + 4 GAS UI binding aliases registered cross-namespace). CommonUI actions conditional on #if WITH_COMMONUI. EffectSurface actions return -32010 ErrOptionalDepUnavailable when the optional EffectSurface provider is absent (see specs/SPEC_MonolithUI.md § "Error Contract"). Architecture expansion Phase A–L landed 2026-04-26 (Spec Builder, Type Registry, EffectSurface, Style Service, hoisted Design Import verbs)
  MonolithMesh          — Mesh inspection, scene manipulation, spatial queries, level blockout, GeometryScript ops, horror/accessibility, lighting, audio/acoustics, performance, decals, level design, tech art, context props, procedural geometry (sweep walls, auto-collision, proc mesh caching, blueprint prefabs), genre presets, encounter design, accessibility reports (~190+ core actions) + EXPERIMENTAL procedural town generator (45 actions, disabled by default via bEnableProceduralTownGen)
  MonolithGAS           — Gameplay Ability System integration: abilities, attributes, effects, ASC, tags, cues, targets, input, inspection, scaffolding, UI attribute binding (~130+ actions, incl. 4 also aliased into `ui` namespace). Conditional on #if WITH_GBA
  MonolithComboGraph    — ComboGraph plugin integration: combo graph CRUD, node/edge management, effects, cues, ability scaffolding (~13 actions). Conditional on #if WITH_COMBOGRAPH
  MonolithAI            — AI asset manipulation: Behavior Trees, Blackboards, State Trees, EQS, Smart Objects, AI Controllers, Perception, Navigation, Runtime/PIE, Scaffolds, Discovery, Advanced (~220+ actions). Conditional on #if WITH_STATETREE, #if WITH_SMARTOBJECTS (required); #if WITH_MASSENTITY, #if WITH_ZONEGRAPH (optional)
  MonolithLogicDriver   — Logic Driver Pro integration: SM CRUD, graph read/write, node config, runtime/PIE, JSON spec, scaffolding, discovery, components, text graph (~66 actions). Conditional on #if WITH_LOGICDRIVER
  MonolithAudio         — Audio asset creation, inspection, batch management, Sound Cue graph building, MetaSound graph building via Builder API, MetaSound document introspection (read-side, +12 actions by @alakangas, PR #18), AI Perception sound binding, sine-tone test wave (Phase J F18) (~90+ actions). MetaSound features conditional on #if WITH_METASOUND
  MonolithAudioRuntime  — Runtime sub-module (Type: Runtime) holding `UMonolithSoundPerceptionUserData` + `UMonolithAudioPerceptionSubsystem` consumed by `audio::bind_sound_to_perception`. **Editor-only ship caveat** — Monolith does not currently ship to cooked game builds, so the runtime sub-module is not present at runtime in shipped Steam builds. See `COOKED_BUILD_TODO.md`. (0 MCP actions — provides runtime classes only)
  MonolithBABridge      — Optional IModularFeatures bridge for Blueprint Assist integration. Exposes IMonolithGraphFormatter; enables BA-powered auto_layout across blueprint, material, animation, and niagara modules when Blueprint Assist is present (0 MCP actions — integration only)
  MonolithReflectionIntel — Deterministic reflection intelligence layer (~28 actions): `decision_query` (5 — markdown decision-record mining), `risk_query` (5 — git churn / co-change / hotspot scoring / conditional-gate inventory), `cppreflect_query` (6 — UE 5.7 reflection-edge queries from UHT artefacts cross-joined with `IAssetRegistry`, incl. the `list_class_specifiers` token-discovery action), `network_query` (4 — replicated-class / RPC / OnRep enumeration + unbalanced-OnRep audit), `pipeline_query` (2 — `pr_review` + `release_readiness` composers), `reflect_query` (1 — `rebuild_reflection_index` project-only force-rebuild of the RI reflection tables; WRITE/maintenance, idempotent), plus `source_query("audit_module_dep_reality")` (1 — UPROPERTY/Build.cs missing-dep audit) and 4 audit actions on existing namespaces (`material_query("audit_orphan_materials")`, `niagara_query("audit_cross_asset_refs")`, `blueprint_query("audit_cdo_drift")`, `project_query("audit_orphan_assets")`). All six indexers write into `EngineSource.db` with lazy bootstrap + hot-reload refresh. Phase 3b (tree-sitter native-tag tracking) and Phase 4b (tag-graph + thread-safety audits) are WISHLIST — both blocked on tree-sitter substrate.
```

**Custom sibling plugins (not inside core Monolith; source + per-module specs are private to their respective repos):**
Additional sibling plugins may register their own namespaces outside this repository. They are intentionally excluded from public Monolith action counts and release packages; their source, action rosters, and module specs belong in their own repos. Sibling-plugin namespaces are documented in those repos and are NEVER part of the public release zip or the advertised public action count. See §12 Total row for live counts.

**Optional widget runtime providers** (not bundled with the public Monolith release zip): MonolithUI can expose EffectSurface action handlers through a reflective UClass probe when an external provider supplies the expected widget classes. MonolithUI has zero compile-time dependency on that provider. When the provider is absent, EffectSurface actions return `-32010 ErrOptionalDepUnavailable` (see [`specs/SPEC_MonolithUI.md` § "Error Contract"](specs/SPEC_MonolithUI.md#error-contract--optional-effectsurface-provider-absence--32010)); the rest of `ui::` is fully functional. The `make_release.ps1` `$LeakSentinels` list defends against accidental optional-provider symbol leakage into public release DLLs.

For the architectural pattern that lets you write your own sibling plugin and register actions into Monolith's MCP registry from outside the core repo, see [`SIBLING_PLUGIN_GUIDE.md`](SIBLING_PLUGIN_GUIDE.md).

> **Live editor `monolith_status` will report a higher count than the in-tree total.** When sibling plugins are loaded the editor reports the union of in-tree and sibling actions. This is expected. The numbers in §12 below are the **in-tree** ground truth; sibling totals are specced in their own repos.

### Discovery/Dispatch Pattern

All domain modules register actions with `FMonolithToolRegistry` (central singleton). Each domain exposes a single `{namespace}_query(action, params)` MCP tool. The 4 core tools (`monolith_discover`, `monolith_status`, `monolith_reindex`, `monolith_update`) are standalone. Conditional modules gate registration on compile-time defines: MonolithGAS (`#if WITH_GBA`), MonolithComboGraph (`#if WITH_COMBOGRAPH`), MonolithLogicDriver (`#if WITH_LOGICDRIVER`), MonolithUI CommonUI actions (`#if WITH_COMMONUI`), MonolithAI (`#if WITH_STATETREE` + `#if WITH_SMARTOBJECTS` required; `#if WITH_MASSENTITY` + `#if WITH_ZONEGRAPH` optional), MonolithAudio (MetaSound actions conditional on `#if WITH_METASOUND`).

`monolith_discover(namespace)` is **terse by default** — it returns each action's name + a one-line description, not the full per-action param schema. For an action's full param schema call `describe_query action_schema`, or pass `detail=true` (alias `verbose=true`) to inline every action's schema. Optional `filter`/`offset`/`limit` narrow or paginate the listing (`limit=0` = the complete list). See [`specs/SPEC_MonolithCore.md` § "Terse per-namespace discover"](specs/SPEC_MonolithCore.md). The full `discover()` (no namespace) response is unchanged.

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

**`warnings[]` channel (Phase 1.0, 2026-05-27).** Successful responses may carry an optional top-level `warnings: string[]` array. The array is appended to (not replaced) by three independent sources before emit, and is omitted when empty:

1. **K3 unknown-param soft-warn** — typo / not-in-schema param keys. Hard error only when `STRICT_PARAMS=1`. K3's per-action allowlist additionally admits the three universal response-shaping params `_fields` / `_omit` / `_compact_json` (Phase 1.0) plus the pre-existing `asset_path` legacy alias. See `MonolithToolRegistry.cpp` around line 115.
2. **Survivor D AssetPath rewrite** — when a `Kind == AssetPath` param receives a backslash-bearing value, the dispatcher rewrites `\` → `/` and appends a notification. Never silent. See §14.2.
3. **Survivor B `_fields` / `_omit` collision** — when both response-shaping whitelist and blacklist are non-empty, `_fields` wins and a warning is appended. See §14.1.

All three sources emit free-text strings into the same array. No schema or envelope change — `FMonolithActionResult` shape is unchanged.

**`error.data.suggestions` channel (Phase 2, 2026-05-27).** On `ErrMethodNotFound` / `ErrInvalidParams` dispatch failures where the action name or namespace is unknown, the error envelope carries an optional `data: { kind: "action"|"namespace", suggestions: [{namespace, action, score}, ...] }` payload. Top-3 fuzzy matches over the registry keyspace via UE's `Algo::LevenshteinDistance`; normalised score = `1.0 - dist/max(len)`. The dispatcher snapshots the keyspace under `FScopeLock`, drops the lock, then scores and sorts — no read-path latency spike on the hot dispatch loop. Asset-path and property-name fuzzy matching are explicitly OUT-OF-SCOPE (O(N·L²) over 10K+ registry entries kills it; property-name overlap with the K3 unknown-key warning produces noise). See §14.4. (WISHLIST) K3 unknown-key counter as telemetry feedback for evaluating future retry-thrash interventions.

### Module Loading

| Module | Loading Phase | Type |
|--------|--------------|------|
| MonolithCore | PostEngineInit | Editor |
| All others (17 Editor + 1 Runtime) | Default | Editor (MonolithBABridge is optional — empty shell when Blueprint Assist absent). MonolithReflectionIntel ships v0.17.0 Phases 1 + 2 + 3a of Reflection Intelligence — `decision_query` + `risk_query` + `cppreflect_query` namespaces plus a `source_query("audit_module_dep_reality")` audit action. MonolithAudioRuntime is Type: Runtime — provides `UMonolithSoundPerceptionUserData` + `UMonolithAudioPerceptionSubsystem` for `audio::bind_sound_to_perception` |

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
| 3.1 | MonolithCore | [specs/SPEC_MonolithCore.md](specs/SPEC_MonolithCore.md) | HTTP server (bind retry, Restart(), `Monolith.Restart` console cmd), tool registry, discovery, settings, auto-updater, `monolith_guide` editorial cross-namespace guide (section-keyed), improved central error messages (15 sites carry inline recovery guidance) |
| 3.2 | MonolithBlueprint | [specs/SPEC_MonolithBlueprint.md](specs/SPEC_MonolithBlueprint.md) | Blueprint inspection, variable/component/graph CRUD, node ops, compile, spawn (~120+ actions) |
| 3.3 | MonolithMaterial | [specs/SPEC_MonolithMaterial.md](specs/SPEC_MonolithMaterial.md) | Material inspection + graph editing + CRUD + function suite (~60+ actions) |
| 3.4 | MonolithAnimation | [specs/SPEC_MonolithAnimation.md](specs/SPEC_MonolithAnimation.md) | Animation sequences, montages, ABPs, curves, notifies, skeletons, PoseSearch, ABP write, ControlRig (~150+ actions) |
| 3.5 | MonolithNiagara | [specs/SPEC_MonolithNiagara.md](specs/SPEC_MonolithNiagara.md) | Niagara particle systems, HLSL module/function, DI config, event handlers, sim stages, layout, temporal control, stateless-emitter factory, search & discovery (~120+ actions) |
| 3.6 | MonolithEditor | [specs/SPEC_MonolithEditor.md](specs/SPEC_MonolithEditor.md) | Build triggers, live compile, log capture, crash context, scene capture, PIE smoke/capture + input/control harness (~50+ actions) |
| 3.7 | MonolithConfig | [specs/SPEC_MonolithConfig.md](specs/SPEC_MonolithConfig.md) | Config/INI resolution and search (6 actions, fixed surface) |
| 3.8 | MonolithIndex | [specs/SPEC_MonolithIndex.md](specs/SPEC_MonolithIndex.md) | SQLite FTS5 deep project indexer (~10+ MCP actions, 18 internal indexers incl. `FMetaSoundIndexer` from PR #18 by @alakangas) |
| 3.9 | MonolithSource | [specs/SPEC_MonolithSource.md](specs/SPEC_MonolithSource.md) | Engine source + API lookup, LLM C++ authoring ergonomics (~20+ actions) |
| 3.10 | MonolithUI | [specs/SPEC_MonolithUI.md](specs/SPEC_MonolithUI.md) | Widget blueprint CRUD, slot/template/styling, animation v1+v2, bindings, settings/accessibility scaffolds, **Spec Builder + Type Registry + EffectSurface + Style Service** (Phase A–L expansion 2026-04-26), CommonUI categories A–I. **~130+ module-owned actions** (always-on + CommonUI under `WITH_COMMONUI`) + 4 GAS UI binding aliases |
| 3.11 | MonolithMesh | [specs/SPEC_MonolithMesh.md](specs/SPEC_MonolithMesh.md) | Mesh/scene/spatial/blockout/GeometryScript/procedural (~190+ core + 45 experimental town gen) |
| 3.12 | MonolithBABridge | [specs/SPEC_MonolithBABridge.md](specs/SPEC_MonolithBABridge.md) | IModularFeatures bridge for Blueprint Assist (0 MCP actions, integration only) |
| 3.13 | MonolithGAS | [specs/SPEC_MonolithGAS.md](specs/SPEC_MonolithGAS.md) | Gameplay Ability System integration (~130+ actions, incl. 4 UI binding aliased into `ui::`, WITH_GBA) |
| 3.14 | MonolithComboGraph | [specs/SPEC_MonolithComboGraph.md](specs/SPEC_MonolithComboGraph.md) | ComboGraph integration (~13 actions, WITH_COMBOGRAPH) |
| 3.15 | MonolithLogicDriver | [specs/SPEC_MonolithLogicDriver.md](specs/SPEC_MonolithLogicDriver.md) | Logic Driver Pro integration (~66 actions, WITH_LOGICDRIVER) |
| 3.16 | MonolithAI | [specs/SPEC_MonolithAI.md](specs/SPEC_MonolithAI.md) | Behavior Trees, Blackboards, State Trees, EQS, Smart Objects, Perception, Nav (~220+ actions) |
| 3.17 | MonolithAudio | [specs/SPEC_MonolithAudio.md](specs/SPEC_MonolithAudio.md) | Sound Cues, MetaSounds (Builder API + document introspection), batch audio ops, AI Perception bind (~90+ actions, MetaSound features WITH_METASOUND) |
| 3.18 | MonolithReflectionIntel | [specs/SPEC_MonolithReflectionIntel.md](specs/SPEC_MonolithReflectionIntel.md) | Deterministic, $0-LLM reflection intelligence — Decision + Risk + CppReflect + Network + Pipeline + Reflect (maintenance) + Module-Dep Reality Audit + 4 cross-namespace audit actions. v0.17.0 shipped 26 actions across `decision_query` (5), `risk_query` (5), `cppreflect_query` (5), `network_query` (4), `pipeline_query` (2), `source_query("audit_module_dep_reality")` (1), plus 4 audit actions on existing namespaces — `material_query("audit_orphan_materials")` / `niagara_query("audit_cross_asset_refs")` / `blueprint_query("audit_cdo_drift")` / `project_query("audit_orphan_assets")`; the `cppreflect_query("list_class_specifiers")` follow-up brings `cppreflect_query` to 6, and the `reflect_query("rebuild_reflection_index")` network-completeness follow-up adds the new `reflect` namespace (1 WRITE/maintenance action — module total ~28 actions). Phase 3b native-tag tracking (tree-sitter) and Phase 4b tag-graph / thread-safety audits are WISHLIST — both blocked on tree-sitter substrate. |

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

**Location:** `Scripts/monolith_offline.py`
**Dependencies:** Python stdlib only (sqlite3, argparse, json, re, pathlib) — no pip installs required
**Python version:** 3.8+

A companion CLI that queries `EngineSource.db` and `ProjectIndex.db` directly without the Unreal Editor running. Intended as a fallback when MCP is unavailable (editor down, CI environments, quick terminal lookups).

**Scope:** Read/query operations only. Write operations require the editor and MCP.

### Usage

```
python Scripts/monolith_offline.py <namespace> <action> [args...]
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

**Reflection Intelligence (20 actions) — full parity, byte-identical to live.** All four RI namespaces are fully servable offline: `cppreflect` (6 — `get_uclass`, `list_uproperties`, `list_ufunctions`, `find_interface_impls`, `find_class_specifier`, `list_class_specifiers`), `network` (4 — `list_replicated_classes`, `list_rpc_functions`, `list_onrep_handlers`, `audit_unbalanced_onreps`), `decision` (5 — `list_decisions`, `get_decision`, `list_stale`, `find_supersession_chain`, `find_referent_decisions`), and `risk` (5 — `get_hotspot_score`, `get_cochange_pairs`, `get_file_churn`, `get_release_window_hotspots`, `list_conditional_gates`). The offline JSON is **byte-identical** to the live MCP server: same field names, types, key ordering, row data, float formatting (`%.17g`), and cursor tokens (base64 of UE pretty-printed `{\r\n\t"qh":N,...}` using UE's `Strihash_DEPRECATED` filter-hash). Prior builds covered only 4 of the 20 with divergent shapes and a phantom `risk.list_hotspots` action (now removed).

**Two intentional, documented differences vs the live MCP payload (NOT bugs):**

1. **`success` envelope.** The offline CLI adds a top-level `success: true|false` as its in-band status channel — a CLI has no MCP protocol envelope to carry success/error out-of-band. Live payloads have no `success` key. The DATA payload nested under it is byte-identical.
2. **Wall-clock fields.** Time-derived values (`cutoff_unix`, `since_unix`) and any cursor whose filter-hash includes them (`risk.get_release_window_hotspots`) legitimately differ by the run-time gap across separate process invocations — on both live and offline. This is faithful parity with a live time-quirk, not drift.

**exe is canonical; py is a dev-only fallback in lockstep.** `monolith_query.exe` is the preferred path. `monolith_offline.py` is a stdlib-only fallback kept byte-for-byte equal to the exe — the parity guard enforces `exe == py`.

**Parity + freshness guards (ship-blocking):**

| Script | Role |
|--------|------|
| `Scripts/verify_offline_parity.py` | HARD-GATE parity guard — byte-diffs exe vs py across all 20 RI actions; exits non-zero on any diff. `make_release.ps1` runs it as a ship gate. |
| `Scripts/check_offline_exe_fresh.py` | Staleness guard — compares the exe's `--version` `source_hash` against a fresh hash of `monolith_query.cpp`; flags a stale exe. |
| `Tools/MonolithQuery/build.bat` | Injects `SOURCE_HASH` (certutil SHA256 of the source) into the exe via `/DSOURCE_HASH`. |

`make_release.ps1` now builds the exe fresh (vcvars + `build.bat`) before the Binaries copy, then runs the parity guard — a stale or drifted exe can never ship. The 20-action RI parity is the scope verified this sprint; the source/project namespaces share the same engine but were not re-audited for byte-parity in this pass.

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
| unreal-niagara | Niagara, particle, VFX, emitter | `niagara_query()` | 129 |
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
| bEnableIndex | True | If false, skips the indexing *run* at subsystem init (query actions stay registered, so an existing DB still answers). Escape hatch for the large-project deep-index GC-worker-exhaustion crash class |
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

**Note:** Module enable toggles are functional — each module checks its toggle at registration time and skips action registration if disabled. **Exception — `bEnableIndex`:** the Index module always registers its query actions (so `project_query` answers from an existing DB); the toggle instead gates the indexing *run* at `UMonolithIndexSubsystem::Initialize`. `bDeferFirstTimeIndex` similarly skips only the automatic first-time index, leaving it triggerable via the `Monolith.StartIndex` console command.

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
3. Selects the release asset for the running engine (compile-time `ENGINE_MINOR_VERSION`; fail-closed if a per-engine release has no matching asset) and parses the matching `Monolith-SHA256-v2-UE5.<minor>:` integrity marker from the release notes (fail-closed if a per-engine asset has no matching v2 marker; legacy assets use `Monolith-SHA256-v2:`, absent = proceed unverified with a warning)
4. If newer: shows a dialog window with full release notes + "Install Update" / "Remind Me Later"
5. Download stages to `Saved/Monolith/Staging/` (NOT Plugins/ — would cause UBT conflicts); the downloaded zip's SHA-256 is computed with the plugin's portable `MonolithSha256::Compute` (FIPS 180-4, no engine platform hook — `FPlatformMisc::GetSHA256Signature` has no Windows impl and its generic fallback fatally asserts, Issues #90/#94) and must equal the parsed marker or the install refuses. The "v2" marker generation exists because v0.14.7–v0.21.0 updaters hard-assert on the old marker names — old names must never be emitted again
6. On editor exit, a detached swap script runs:
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

### Editor Component Persistence (UPROPERTY-Backed References)

Any MCP action that builds components on an editor-spawned `AActor` via `NewObject<...>(Actor, ...)` MUST register each component on `AActor::InstanceComponents` (a `UPROPERTY(Instanced) TArray<TObjectPtr<UActorComponent>>` at `Engine/Source/Runtime/Engine/Classes/GameFramework/Actor.h:4331`). Without `AActor::AddInstanceComponent(Comp)` the owning actor holds no UPROPERTY-backed reference to the component, `RegisterComponent()` wires it up for the current session only, and the component disappears on level save/reload — silent data loss. This was the root cause of Issue #63 across three action sites. The canonical sequence — `Modify()` -> `NewObject(... RF_Transactional)` -> `SetupAttachment` / `SetRootComponent` -> `AddInstanceComponent` -> `RegisterComponent` -> component setup -> `MarkPackageDirty()` — matches `Engine/Source/Editor/UnrealEd/Private/Factories/ActorFactory.cpp:1321` and `Engine/Source/Editor/UnrealEd/Private/Kismet2/ComponentEditorUtils.cpp:621`:

```cpp
Actor->Modify();
USomeComponent* Comp = NewObject<USomeComponent>(
    Actor, USomeComponent::StaticClass(), TEXT("ComponentName"), RF_Transactional);
Comp->SetupAttachment(RootComp);   // or Actor->SetRootComponent(Comp) for the root
Actor->AddInstanceComponent(Comp); // THE persistence anchor — order vs RegisterComponent is flexible; what matters is the array references the component at save-time
Comp->RegisterComponent();
Comp->SetStaticMesh(...);          // component-specific configuration AFTER register
Actor->MarkPackageDirty();         // once all components are added
```

---

### Preview & Inspection Surface (`editor::`)

The `editor::` namespace exposes a tight family of capture and inspect actions that let AI agents introspect Unreal assets at higher fidelity than the default 256² thumbnail. Four new actions land alongside three extensions to the existing `editor::capture_scene_preview`. **Extended `asset_type` enum values:** `static_mesh`, `skeletal_mesh` (with optional `animation_path` + `seek_time` for posed-frame capture), and `widget` (UMG via `FWidgetRenderer` with `scale` DPI multiplier) join the prior `material` / `niagara`. **New actions:** `editor::capture_material_grid` (N material instances side-by-side under shared lighting, auto-grid via `ceil(sqrt(N))` with optional `columns` override); `editor::capture_with_overlay` (single-asset capture under one of five engine debug-view show flags — `wireframe`, `normals`, `uv_density`, `lightmap_density`, `shader_complexity`); `editor::inspect_material_pbr` (reflective walk of a material's texture parameter list, classifying each by PBR slot and detecting ORM / ARM / MRA channel-packing — pure JSON, no rendering); `editor::inspect_texture_channels` (per-channel R/G/B/A min/max/mean statistics + optional per-channel split PNGs via `emit_splits`). All seven are editor-only and live in `MonolithEditor`.

The canonical pattern for the capture-style actions is `FAdvancedPreviewScene` + `USceneCaptureComponent2D` + `UTextureRenderTarget2D` + `FImageUtils::SaveImageAutoFormat` (mirrors the existing `HandleCaptureScenePreview` recipe — game-thread invoke, render-thread enqueue via `CaptureScene()`, readback via `GameThread_GetRenderTargetResource()->ReadPixels()`). The widget path additionally uses `FWidgetRenderer::DrawWidget` against the same RT (guard with `FApp::CanEverRender()` — headless commandlets and `-nullrhi` will return a clear error). The inspect-style actions skip the render path entirely: `inspect_material_pbr` walks `UMaterialEditingLibrary::GetTextureParameterNames` + `GetTextureParameterValue` then routes each through a small PBR classifier; `inspect_texture_channels` locks the source mip via `FTextureSource::LockMipReadOnly`, computes statistics in a single pass, and writes split PNGs only when `emit_splits=true`.

Source-of-truth files: `Source/MonolithEditor/Private/MonolithEditorActions.cpp` (the `asset_type` enum extension lives inside `HandleCaptureScenePreview`), `Source/MonolithEditor/Private/MonolithEditorPreviewActions.cpp` (`capture_material_grid` + `capture_with_overlay`), `Source/MonolithEditor/Private/MonolithEditorInspectActions.cpp` (`inspect_material_pbr` + `inspect_texture_channels`). Header surface lives in `Source/MonolithEditor/Public/MonolithEditorActions.h` (four new static handler declarations alongside the existing capture handler).

**`editor::delete_assets` non-interactive deletion (v0.18.0).** `delete_assets` now takes an optional `force` bool (default `false`) and never raises a blocking modal. Before deleting each target it clears the package dirty flag and closes any open asset editor, then performs the delete inside an unattended-script guard so the engine cannot pop a Slate "asset in use" / "save changes" dialog (which would freeze an unattended MCP session). `force=false` soft-deletes after closing editors; `force=true` calls `ForceDeleteObjects`, nulling referencers. Per-asset failures are surfaced in a `failed_to_delete[]` response array instead of aborting the whole call. Native C++ — no Python. **Known limitation:** a `UNiagaraScript` created and compiled within the same session can't be deleted until its compile state clears (a transient compilation/traversal graph retains a reference — the engine's own Content Browser hits the same wall); it becomes deletable after the state clears, e.g. on editor restart. Per-action detail: [`SPEC_MonolithEditor.md`](specs/SPEC_MonolithEditor.md).

AI discoverability: `monolith_guide(section="recipes")` returns Recipe 5 ("Visual introspection — going beyond thumbnails") and Recipe 6 ("Reading asset structure without rendering"); `monolith_guide(section="decisions")` returns the `capture_scene_preview` vs `capture_material_grid` vs `inspect_material_pbr` decision matrix. Live param schemas come from `describe_query("action_schema", target_namespace="editor", target_action="<name>")` (`monolith_discover("editor")` lists action names + one-line descriptions only).

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
- **F4 (2026-04-26) — a project vitals AttributeSet shipped as production C++ (in a project source file).** A canonical vitals AttributeSet for the host project: three stat pairs (`Health` / `MaxHealth` plus two more, six `FGameplayAttributeData`, all default 100, all `BlueprintReadOnly`, replicated `REPNOTIFY_Always`). Standard GAS pattern via `ATTRIBUTE_ACCESSORS` macro. `PreAttributeChange` clamps current values into `[0, Max]`, Max attributes floor at 1. `PostGameplayEffectExecute` re-clamps base values defensively after instant executes and re-clamps `Current <= Max` when a Max attribute changes downward. Build.cs gained `GameplayAbilities` + `GameplayTasks` public deps (`GameplayTags` was already present). Resolves the J1 test-spec prerequisite that previously demanded a disposable BP fallback at `/Game/Tests/Monolith/AS_TestVitals` — bind targets against the project AttributeSet's attributes are now first-class. Additional resistance attributes DEFERRED per project scope decision. Plan: `Docs/plans/2026-04-26-monolith-j-phase-fix-plan.md` §F4.
- **F5 (2026-04-26) — J1 GAS UI binding response-shape & error-message drift cleanup (`MonolithGASUIBindingActions.cpp`).** Six impl changes that bring response shapes and error text into alignment with the J1 test spec: (1) `SerializeBindingRow` field `index` renamed to `binding_index` (matches the bind-response field name; previously the only inconsistency between bind and list outputs); (2) composite `attribute` and `max_attribute` strings (`"<ClassShortName>.<PropertyName>"`, derived via `FPackageName::ObjectPathToObjectName`) added alongside the existing split `attribute_set_class` + `attribute_name` (and `max_attribute_*`) fields, giving callers round-trip parity with the bind-input contract while keeping split fields for back-compat; (3) `widget_class` field added to each list-row by looking up the widget in the WBP tree (parity with the bind response); (4) `removed_binding_index` field added to the unbind response, captured pre-removal via `IndexOfBinding` (no `RemoveBinding` refactor needed); (5) two error sites enriched with valid-options enumerations — the "Widget '...' not found" site now appends `Available: [...]` listing widget-tree variable names (sorted, capped at 20 with a SPEC pointer beyond the cap) via new helper `BuildAvailableWidgetsClause`, and the "Unsupported (widget=...)" site rewritten as `"Property '...' invalid for <class>. Valid: [...]"` via new helper `BuildValidPropertiesClause` mirroring `ValidateWidgetProperty`'s accept branches; (6) `LoadWBP` split into raw-load + Cast so callers can distinguish "asset path doesn't exist" (`"Widget Blueprint asset not found: <path>"`) from "asset is the wrong UClass" (`"Asset at <path> is not a Widget Blueprint (got <UClassName>)"`) — the type-checked `LoadAssetByPath` overload was conflating both. Investigation: `Docs/research/2026-04-26-j1-gas-binding-findings.md` §B. Plan: `Docs/plans/2026-04-26-monolith-j-phase-fix-plan.md` §F5.
- **F8 (2026-04-26) — five new MCP scaffolding actions for J-phase test specs (+5 → 1282 total).** Net adds across three modules:
  - `editor::create_empty_map` — new file `MonolithEditorMapActions.cpp/.h`. Creates a fully blank `UWorld` asset on disk via `UWorldFactory` + `IAssetTools::CreateAsset`, then `UPackage::SavePackage`. v1 supports `map_template="blank"` only — `vr_basic` / `thirdperson_basic` reserved (UE 5.7 templates are populated by editor-only template files, not factory-creatable). Resolves J3 §Setup #5 ("disposable test scene") which previously required manual File > New Level.
  - `editor::get_module_status` — same new file. Wraps `IPluginManager::GetDiscoveredPlugins` (one-pass module-name → plugin reverse-index) + `FModuleManager::IsModuleLoaded`. Returns `{ module_name, plugin_name, enabled, loaded, is_runtime, version? }` per row. Empty input list returns rows for all Monolith modules. Unknown module names return `enabled=false / loaded=false / plugin_name=""` without error so callers can probe optional modules. Resolves J3 §Setup #3 reference to a previously non-existent action.
  - `gas::grant_ability_to_pawn` — extended existing `MonolithGASScaffoldActions.cpp/.h`. Locates a pawn BP's `UAbilitySystemComponent` SCS node (or native ASC on the parent CDO), reflects over the ASC class for any `TArray<TSubclassOf<UGameplayAbility>>` UPROPERTY whose name contains "Ability" (matches the conventional `StartupAbilities` / `DefaultAbilities` pattern across most projects), appends the resolved class via `FScriptArrayHelper`, marks BP structurally modified, then `FKismetEditorUtilities::CompileBlueprint`. Stock `UAbilitySystemComponent` has NO startup-abilities array; the action returns a clear "subclass it and add the array" error in that case (project-agnostic — no assumption of any specific project ASC subclass). Skips duplicates idempotently.
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
- **F17 (2026-04-26) — `MonolithSource` auto-reindex on hot-reload (`MonolithSourceSubsystem.h` + `.cpp`).** `UMonolithSourceSubsystem::Initialize` now binds `FCoreUObjectDelegates::ReloadCompleteDelegate` and kicks `TriggerProjectReindex()` on every Live Coding patch and post-UBT hot-reload. Without this hook agents saw stale `source_query` results until someone called `source.trigger_project_reindex` manually — and since `monolith_reindex` is the **asset** indexer (not the source DB), the canonical recovery message had been confusing in spec docs. The new handler `OnReloadComplete(EReloadCompleteReason)` has three guards: (1) `bIsIndexing` re-entrancy guard — UBT can fire one signal per reloaded module in quick succession; (2) 5-second cooldown via new `LastReindexTimeSeconds` member (`FPlatformTime::Seconds()`); (3) bootstrap-skip if `EngineSource.db` doesn't yet exist — incremental reindex requires the engine symbols already in place, so the very-first-install case stays silent and waits for a manual `source.trigger_reindex`. `Deinitialize` unbinds via the new `ReloadCompleteHandle` member BEFORE indexer teardown so a late-firing reload signal can't re-enter into a half-destroyed subsystem. Build.cs unchanged — `CoreUObject` is already a Public dep, no `HotReload` module needed (the `FCoreUObjectDelegates` route is the established pattern, also used by `MonolithUI` plan §B for hot-reloaded UClass discovery). Note: the `OnReloadComplete` declaration + `ReloadCompleteHandle` + `LastReindexTimeSeconds` field additions are header changes — Live Coding alone is insufficient; orchestrator must run a full UBT build. SPEC update: `Plugins/Monolith/Docs/specs/SPEC_MonolithSource.md` (new "Auto-Reindex on Hot-Reload (F17)" section + class-table annotation). Plan: `Docs/plans/2026-04-26-monolith-j-phase-fix-plan-v2.md` §F17. Investigation: `Docs/research/2026-04-26-j-misc-drift-findings.md` §C.Layer1.

---

## 12. Action Count Summary

> **Counts are intentionally approximate.** Exact per-namespace and grand-total integers drift on every action added and are no longer maintained to the unit. Treat the figures below as rough ballpark (`~`) values; for an authoritative live number query `monolith_discover()` (its `total_actions` field) at the moment you need it. The per-namespace rows are kept for structure, not precision.
>
> **Ballpark, current:** **~1,400+ actions across 25+ in-tree namespaces** (query `monolith_discover()` for the live figure — see Action Count Discipline in `monolith-release.md`).

Counts below were re-verified against the live `monolith_discover()` registry on 2026-05-11 (v0.14.11 [Unreleased] audit). Where a module's spec previously claimed a different number, those per-module SPEC files were corrected. Reflected: all v0.14.10 deltas (see [v0.14.10] in `CHANGELOG.md`) plus v0.14.11 [Unreleased] additions: **two new framework-level namespaces** `bulk_fill` (2 actions: `apply`, `list_namespaces`) and `describe` (2 actions: `schema`, `list_targets`) registered from `MonolithCore`, plus 12 per-namespace adapters (blueprint, gas, ui, ai, niagara, material, audio, mesh, animation, logicdriver, combograph, inventory) routed through `FMonolithBulkFillRegistry`. `blueprint` +2 (`set_cdo_properties`, `describe_cdo_schema` — Phase 1 adapter surface). `ui` +16 (Tier 1 fixes net +1 `create_bound_action_bar` `action_button_class` param is additive — counted by `dump_blueprint_compile_log` +1 Tier 2 action; Tier 2 +8: `rename_widget`, `add_widget_variable`, `audit_focus_chain`, `apply_token_binding`, `list_widget_property_enums`, `convert_textblock_to_common`, `set_action_bar_button_class`, `dump_blueprint_compile_log`; Tier 3 +4: `scaffold_main_menu`, `scaffold_settings_panel_with_tabs`, `scaffold_pause_menu`, `build_menu_from_spec`; Tier 4 polish docs-only). `animation` ergonomics: `add_anim_graph_node` accepts arbitrary loaded `UAnimGraphNode_Base` subclasses via `node_class` path/name (schema-only change, no count delta).

**MonolithAnimation ABP write note:** `animation.add_anim_graph_node` preserves the existing `node_type` aliases and also accepts a generic loaded non-abstract `UAnimGraphNode_Base` subclass by class path/name through either `node_type` (legacy clients) or `node_class` (explicit clients). The action validates unresolved, abstract, and non-AnimGraph classes before spawning.

| Module | Namespace | Actions | Source-of-truth notes |
|--------|-----------|---------|------------------------|
| MonolithCore | monolith | 5 | discover, status, update, reindex, guide. `guide` is the section-keyed editorial cross-namespace workflow guide (onboarding / recipes / decisions / errors / skills_map / gotchas) — hand-authored `Docs/MONOLITH_GUIDE.md` cached + H2-split, served alongside a live registry overlay; pull-only, zero per-success-call cost |
| MonolithBlueprint | blueprint | ~120 | Introspection-gap pass (2026-06-10) added 1 action `find_variable_references` (Gap 5) + read-serializer enhancements (Gap 1 `property_access` block on `get_node_details`/`get_graph_data`/`export_graph`; Gap 7 interface-implementation graphs in `list_graphs`/`get_graph_data`/`export_graph`) — no count delta from Gaps 1/7. Motion Matching scaffolding (2026-06-07) added 6 actions (`set_anim_class`, `apply_movement_preset`, `add_engine_component_typed`, `scaffold_locomotion_input`, `validate_animbp_variable_contract`, `scaffold_motion_matching_character`) + `EnhancedInput` Build.cs dep — see the 2026-06-07 Motion Matching note below. Test/profiling-harness gap-closure pass (2026-06-07) added behavior (no new action): `seed_data_asset` gained `read_back_values` (re-walk written top-level props through the shared reflection reader and attach `values:{field:<json>}` to the result — inline verify-after-write); `get_cdo_properties` is sanctioned as the canonical verify path for standalone/pre-existing DataAssets (routes through the same shared serializer); `get_component_details` falls back to inherited native components off the parent-class CDO when no SCS node exists (surfaces `skeletal_mesh` / `anim_class` / `animation_mode` / `is_inherited_native`); `get_blueprint_info` adds `native_component_count`. 89 baseline + 2 v0.14.11 bulk_fill adapter surface actions (`set_cdo_properties`, `describe_cdo_schema`) registered into `blueprint::` for symmetry with the existing `set_cdo_property` / `get_cdo_properties` reads + 1 (2026-05-22) `add_property_access` (cross-class VariableGet/Set, gap #11 of `Docs/plans/2026-05-22-monolith-ui-bp-gap-actions.md`) + 2 (2026-05-23, Phase 2) `override_parent_function` (override a value-returning parent `BlueprintImplementableEvent`/`BlueprintNativeEvent`) + `save_dirty_assets` (batch-save dirty BP/Widget packages) + 17 (2026-05-23, Part B — dataset ergonomics of the same plan) DataTable / CurveTable / StringTable read-CRUD-round-trip + `seed_data_asset` (full family in the Part B reconciliation note below) + 1 (v0.17.0 Phase 4a, registered cross-module from `MonolithReflectionIntel`) `audit_cdo_drift` — detects Blueprint child CDOs overriding native C++ parent defaults + 1 ([Unreleased]) `set_property_at_path` — surgical dotted+bracket nested-path UPROPERTY writer (writes `EditDefaultsOnly` DataAsset fields the editor blocks on saved instances; count stays ~120) |
| MonolithMaterial | material | 64 | 63 baseline + 1 (v0.17.0 Phase 4a, registered cross-module from `MonolithReflectionIntel`) `audit_orphan_materials` — `/Game/` path scan via `IAssetRegistry::GetReferencers` for zero-inbound-reference materials |
| MonolithAnimation | animation | ~170 | ABP-authoring pack ([Unreleased]) added 13 anim-graph-authoring actions (`add_apply_additive`, `add_apply_mesh_space_additive`, `add_slot_node`, `add_save_cached_pose`, `add_use_cached_pose`, `set_output_pose_source`, `set_state_result_source`, `add_blend_by_int`, `set_sync_group`, `set_layered_blend_bones`, `add_anim_control_rig_node`, `add_linked_anim_layer`, `add_conduit`) plus 2 extensions with no count delta (`set_anim_node_pin_binding` now bootstraps the binding object on previously-unbound nodes; `auto_layout` gained a Blueprint-Assist-free `builtin` formatter). Gap-closure ([Unreleased], already counted in prior syncs) added `bake_blend_space` / `set_blend_space_interpolation`, `remove_anim_state` / `set_anim_entry_state` / `remove_anim_transition`, `remove_ik_solver`. Introspection-gap pass (2026-06-10) added 5 actions: `get_anim_node_function_bindings` / `set_anim_node_function_binding` (Gap 2) + `get_anim_node_pin_bindings` / `set_anim_node_pin_binding` (Gap 12) + `sample_pie_timeseries` (Gap 9 — registered under `animation` but implemented in MonolithEditor, which owns the PIE-session infra). Enhancements with no count delta: Gap 3 dotted-path / friendly-name resolution in `sample_pie_anim_instance.variables`; `get_nodes` additive `bindings` + `pin_bindings` blocks. (The `chooser`-namespace `inspect_chooser` Gap 6 columns/`include_cells` enhancement is counted under the `chooser` row.) Motion Matching action pack (2026-06-07) added 14 PoseSearch / motion-matching-node actions (`create_normalization_set`, `add_database_to_normalization_set`, `set_database_normalization_set`, `add_database_entry`, `set_database_entry_tags`, `create_mirror_data_table`, `set_schema_mirror_data_table`, `configure_schema_channel`, `add_pose_search_notify`, `derive_schema_channels_from_skeleton`, `validate_pose_search_database`, `configure_pose_history_node`, `configure_motion_matching_node`, `build_motion_matching_node`); a retarget create/run pack of 4 (`create_ik_rig`, `create_ik_retargeter`, `set_retargeter_rigs`, `batch_retarget_animations` — auto-seed the default retarget op stack so retargets produce motion); and the read-only `get_anim_graph_output_connection`; plus `add_anim_graph_node` `pose_history`/`inertialization` aliases and `get_database_stats`/`get_pose_search_schema` read-back fields (no count delta) — see the 2026-06-07 Motion Matching note below. Test/profiling-harness gap-closure pass (2026-06-07) added `get_anim_graph_choosers` (walk an AnimBP's graphs — main AnimGraph + function graphs — for chooser-evaluating nodes, reporting `{node_guid, node_title, chooser_asset, output_pin_links}`; optional `recursive` expands the referenced chooser tree) and `get_transition_rule` (read back an authored transition rule's kind + operands + compile status). Also: `set_transition_rule` gained a structured `rule` object (`kind=bool/auto/compare`; `compare` = `{lhs, op, rhs}`; expression-graph authoring deferred), inherited-bool/float transition rules now validate against the parent AnimInstance class chain, and `get_nodes` gained `include_anim_graph` (traverse the main AnimGraph and emit `LinkedTo` endpoints). Includes 5 ABP write actions (`add_anim_graph_node`, `connect_anim_graph_pins`, `set_state_animation`, `add_variable_get`, `set_anim_graph_node_property`), 3 ControlRig write, 1 layout, plus 103 baseline (96 + 1 v0.14.9 `copy_bone_pose_between_sequences` — PR #51 by @MaxenceEpitech + 1 v0.14.10 `list_bone_tracks` — PR #54 by @MaxenceEpitech + 2 v0.14.10 PR #55 by @MaxenceEpitech: `get_skeleton_preview_attached_assets`, `get_bone_ref_pose` + 3 v0.14.10 PR #56 by @MaxenceEpitech: `{get,add,remove}_compatible_skeleton`) + 13 PoseSearch + 5 test/profiling harness Wave 2 graph-surgery (`rebuild_evaluate_chooser_node`, `replace_evaluate_chooser_nodes`, `duplicate_reparent_and_sanitize`, `find_node_slice`, `remove_node_slice`) + 3 test/profiling harness Wave 16 (`create_state_machine`, `build_state_machine` — declarative SM authoring, bool-var + automatic rules; float/expression rules deferred — + `sample_pie_anim_instance` — live PIE AnimInstance telemetry in the new `MonolithAnimationRuntimeActions.cpp`; see [`specs/SPEC_MonolithAnimation.md`](specs/SPEC_MonolithAnimation.md)). Retargeting + locomotion-authoring pass ([Unreleased]) added 14 actions: 8 retarget pose / op-stack tuning (`align_retarget_pose`, `get_retarget_pose` / `set_retarget_pose`, `get_retarget_chain_settings` / `set_retarget_chain_settings`, `set_retarget_root_settings`, `enable_foot_ground_lock`, `set_bone_translation_retargeting` / `get_bone_translation_retargeting`), 3 locomotion authoring (`get_root_motion_speed`, `bake_distance_curve`, `bind_threadsafe_update_function`), 2 IK Rig bone settings (`set_ik_rig_bone_settings` / `get_ik_rig_bone_settings`), and 1 inspection (`get_animated_bone_transform`); plus extensions with no count delta (`get_retargeter_info` `ops[]`; `apply_anim_modifier` `properties`/`persist`; `get_blend_space_info` per-sample root-motion speed + bake/interpolation flags; `get_anim_node_pin_bindings` wire-linked `type:"Link"` pins; `derive_foot_sync_markers` `from_bones` mode; `get_curve_keys` `monotonic`/`sign`; `set_anim_node_function_binding` `RequestRefreshExtensions`) — see [`specs/SPEC_MonolithAnimation.md`](specs/SPEC_MonolithAnimation.md) |
| MonolithAnimation | chooser | 10 | `WITH_CHOOSER`-gated `UChooserTable` namespace registered from `MonolithAnimation`: `inspect_chooser`, `duplicate_chooser_tree`, `set_context_object_class`, `set_result_asset_reference`, `set_evaluate_chooser_result_reference` (rewrite the child table an EvaluateChooser/root/nested row points at; `duplicate_chooser_tree` also does two-pass nested `FEvaluateChooser`/`FNestedChooser` remapping + per-row `row_remap_report`), `validate_chooser`, plus the 2026-06-07 Motion Matching authoring set `create_chooser_table` / `add_chooser_column` (optional `enum_class` on `Enum` columns) / `add_chooser_row` / `set_chooser_cell` (typed predicate cell write, per-kind dispatch). `inspect_chooser` gained an optional `recursive` flag (2026-06-07) that resolves the full nested chooser tree (`child_tables[]` with per-row resolved asset path + row kind) and, in the 2026-06-10 introspection-gap pass (Gap 6, no count delta), a richer `columns` array (per-column `input_binding` chain + `is_input`) alongside the back-compat `column_types`, with per-row cell values gated behind `include_cells` — see [`specs/SPEC_MonolithAnimation.md` § Chooser Namespace](specs/SPEC_MonolithAnimation.md). Inert without the Chooser plugin (`Engine/Plugins/Chooser`) |
| MonolithNiagara | niagara | 129 | 108 baseline + 1 layout (`auto_layout`) + 9 temporal-control (`get_system_timing`, `set_warmup_profile`, `set_fixed_tick_delta`, `set_require_current_frame_data`, `set_emitter_loop_profile`, `get_emitter_timing_summary`, `set_sim_stage_iteration_count`, `set_sim_stage_execute_behavior`, `set_particle_lifetime` — see [`specs/SPEC_MonolithNiagara.md` § Temporal Control](specs/SPEC_MonolithNiagara.md#temporal-control-9--added-2026-05-28-phases-1-4-of-plans2026-05-28-niagara-timing-actionsmd)) + 1 stateless-emitter factory (`create_stateless_emitter`, 2026-05-28 — also extends `set_emitter_loop_profile` + `get_emitter_timing_summary` with stateless-aware branches; see [`specs/SPEC_MonolithNiagara.md` § Stateless Emitters](specs/SPEC_MonolithNiagara.md#stateless-emitters-1--added-2026-05-28-phases-0-3-of-plans2026-05-28-niagara-stateless-timermd)) + 1 (v0.17.0 Phase 4a, registered cross-module from `MonolithReflectionIntel`) `audit_cross_asset_refs` — broken/stale asset reference scan over Niagara systems/emitters joined against Phase 3a `cpp_asset_edges` + 7 ([0.18.0], issue #64 Tranche 2) read-only search & discovery actions (`search_by_parameter`, `search_by_data_interface`, `query_niagara`, `find_similar_systems`, `search_by_material`, `find_niagara_references`, `list_system_data_interfaces`) — see [`specs/SPEC_MonolithNiagara.md` § Blueprint-Callable Surface](specs/SPEC_MonolithNiagara.md#blueprint-callable-surface-issue-64) + 2 ([0.18.0], PR #65) HLSL direct-editing actions (`get_custom_hlsl_text`, `set_custom_hlsl_text`) |
| MonolithMesh | mesh | 239 (194 core + 45 experimental town gen) | Town gen registered only when `bEnableProceduralTownGen=true` (default false) |
| MonolithEditor | editor | ~50 | Introspection-gap pass (2026-06-10) added 6 `editor`-namespace actions: 2 live-PIE object (`pie_get_object_properties`, `pie_call_function`, Gap 8) + 3 PIE input/control (`pie_set_control_rotation`, `pie_inject_input_action`, `pie_possess_spectator_free`, Gap 4 — adds an `EnhancedInput` Build.cs dep) + 1 stat readout (`get_stat_group_values`, Gap 10 — `#if STATS`-gated). `sample_pie_timeseries` (Gap 9) is also implemented here but registers under the `animation` namespace (counted in that row). Test/profiling-harness gap-closure pass (2026-06-07) added `author_map_settings` (set WorldSettings GameMode override + spawn `APlayerStart`s on any open map, independent of the nav-harness path). Also (no new action): the PIE-smoke family (`run_pie_smoke` / `capture_pie_movement_clip`) gained a declarative `actor_setup` block (spawn N actors of a class/Blueprint, reflectively apply a DataAsset's fields, issue an AI move), session-scoped profiling (`csv_profile`, `trace_channels` → reported artifact paths), `capture_pie_movement_clip` gained `discard_first_frames` (warm-up policy) + label-aware `view_target_actor` resolution, and `create_nav_harness_map` gained `game_mode_override` + `player_starts` + typed class/soft-class (`_C`-normalized) + object-array property support. 20 base + 2 J F8 (`create_empty_map`, `get_module_status`) + 2 PR #48 (`list_automation_tests`, `run_automation_tests`) + 2 Issue #50 (`run_python`, `load_level`) + 3 v0.14.10 PR #54 (`start_pie`, `stop_pie`, `run_console_command` — by @MaxenceEpitech) + 4 v0.16.0 preview & inspection (`capture_material_grid`, `capture_with_overlay`, `inspect_material_pbr`, `inspect_texture_channels`) + 7 test/profiling harness Wave 1 (`list_dirty_packages`, `save_packages`, `run_pie_smoke`, `poll_pie_smoke`, `stop_pie_smoke`, `capture_pie_movement_clip`, `create_nav_harness_map`) + 1 test/profiling harness Wave 2 (`list_errored_blueprints` — compile-error pre-flight feeding `run_pie_smoke`'s new `on_compile_errors` policy; the module also subscribes a `FCoreDelegates::PreSlateModal` watcher that logs `MODAL_OPEN` lines — no action surface) + 1 test/profiling harness Wave 3 (`capture_anim_frames` — AnimSequence/BlendSpace/AnimBlueprint preview→PNG in an isolated `FPreviewScene`, no PIE; plus no-count enhancements to the PIE-smoke family: `run_pie_smoke`/`capture_pie_movement_clip` `stages` staged hooks + clip-capture validity/`view_target_actor`/`runtime_identity`/`expected_anim_class`, `get_build_errors` `since_marker`/`since_iso`/`since_seconds`/`clear_baseline`/`exclude_categories` scoping + compile-vs-other bucketing, and `/Game` virtual-path output-dir resolution — see [`specs/SPEC_MonolithEditor.md`](specs/SPEC_MonolithEditor.md)) |
| MonolithConfig | config | 6 | |
| MonolithIndex | project | 12 | 7 baseline + 1 (v0.17.0 Phase 4a, registered cross-module from `MonolithReflectionIntel`) `audit_orphan_assets` — project-wide zero-reference scan across `IAssetRegistry::GetReferencers` cross-validated against Phase 3a `cpp_asset_edges` + 3 test/profiling harness Wave 1 (`refresh_assets`, `get_saved_asset_state`, `cleanup_generated_assets` — allowlist-guarded to `/Game/Tests/Monolith/`) + 1 (2026-06-10) `export_asset_text` (Gap 11 — native T3D text export escape hatch) |
| MonolithSource | source | ~20 | 11 baseline + 1 v0.17.0 Phase 2 audit action `audit_module_dep_reality` registered cross-module from `MonolithReflectionIntel` (catches UPROPERTY / API-symbol references whose owning module is missing from the declaring module's `Build.cs` deps; the audit handler lives in `MonolithReflectionIntel`, registration onto `source` is for caller ergonomics — agents already discover `source_query` first) + 8 ([Unreleased]) LLM C++ authoring-ergonomics actions across Phases 1-3 (`get_include_path`, `get_signature`, `check_deprecations` — Phase 1; `verify_symbols`, `find_example_usage` — Phase 2; `lint_header`, `generate_class_stub` — Phase 3; plus `suggest_build_cs_deps` registered cross-module from `MonolithReflectionIntel`). See [`specs/SPEC_MonolithSource.md`](specs/SPEC_MonolithSource.md). |
| MonolithUI | ui | 134 module-owned (79 always-on + 55 CommonUI under `WITH_COMMONUI`) + 4 GAS UI binding aliases (registered from `MonolithGAS`, conditional on `WITH_GBA`) = **138** distinct registrations into `ui::` in the full-stack configuration | Architecture expansion Phase A–L landed 2026-04-26 (see prior entry). v0.14.11 [Unreleased] Tier 1/2/3/4 close-the-loop ergonomics layered on top: Tier 1 fixes (hash-cache mis-keying, CommonUI allowlist additions, `create_bound_action_bar.action_button_class` param, `compile_widget` `errors[]`/`warnings[]` surface, `set_widget_property` `value` alias). Tier 2 +8 actions: `rename_widget`, `add_widget_variable`, `audit_focus_chain`, `apply_token_binding` (MVP-STUB — issue #2-10b for full BP-graph node-write completion), `list_widget_property_enums`, `convert_textblock_to_common`, `set_action_bar_button_class`, `dump_blueprint_compile_log` (gains `BlueprintGraph` + `Projects` deps in `MonolithUI.Build.cs`). Tier 3 +4 scaffolders: `scaffold_main_menu`, `scaffold_settings_panel_with_tabs`, `scaffold_pause_menu`, `build_menu_from_spec` (MVP — issue #3-18b for multi-screen aggregation). Tier 4 polish: `convert_button_to_common` Tokenforge auto-detect, `-32011` Tokenforge Provider Absence error contract parity with `-32010`, new CommonUI Property Allowlist Coverage section in [`specs/SPEC_MonolithUI.md`](specs/SPEC_MonolithUI.md). The 4 GAS aliases (`bind_widget_to_attribute`, `unbind_widget_attribute`, `list_attribute_bindings`, `clear_widget_attribute_bindings`) come from `MonolithGAS/Private/MonolithGASUIBindingActions.cpp` and dispatch to the same handlers as their canonical `gas::` versions. Phase 3 (2026-05-23) of [`Docs/plans/2026-05-22-monolith-ui-bp-gap-actions.md`](plans/2026-05-22-monolith-ui-bp-gap-actions.md) added 4 CommonUI-gated actions — `set_widget_navigation_bulk`, `dump_widget_navigation`, `convert_border_to_common`, `reparent_widget_root` — bumping CommonUI 51 → 55, module-owned 129 → 133, full-stack 133 → 137. Phase 4 (2026-05-23) added 1 always-on action — `set_widget_is_variable` — bumping always-on 78 → 79, module-owned 133 → 134, full-stack 137 → 138 (CommonUI subtotal unchanged) |
| MonolithGAS | gas | 135 | 131 documented in the Action Categories table + 4 UI binding actions (`bind_widget_to_attribute`, `unbind_widget_attribute`, `list_attribute_bindings`, `clear_widget_attribute_bindings`) — the latter four are also aliased into `ui` |
| MonolithComboGraph | combograph | 13 | |
| MonolithAI | ai | 223 | Phase J F8 added `add_perception_to_actor` and `get_bt_graph` — pre-J baseline was 219, not the previously documented 229 (per-category `~N` estimates in SPEC_MonolithAI were aspirational, not literal) + 2 test/profiling harness Wave 1 (`rebuild_navigation`, `validate_nav_points`). Motion Matching AI-wander runtime surface (2026-06-07, **no MCP-action delta**): 3 `UBTTaskNode` classes (`BTTask_SetMaxWalkSpeed`, `BTTask_SetCrouch`, `BTTask_RandomizeFloat` — placed via `add_bt_node`) + 1 Blueprintable `AAIController` (`AMonolithBehaviorTreeAIController`, runs its BT `UPROPERTY` on `OnPossess`). Fixes: `reorder_bt_children` order now persists (`NodePosX`-based); `build_behavior_tree_from_spec` now links `UBehaviorTree::BlackboardAsset`. See [`specs/SPEC_MonolithAI.md`](specs/SPEC_MonolithAI.md) § Motion Matching AI-wander runtime surface |
| MonolithLogicDriver | logicdriver | 66 | |
| MonolithAudio | audio | 98 | 82 documented in the Action Categories table + 4 perception bind actions (`bind_sound_to_perception`, `unbind_sound_from_perception`, `get_sound_perception_binding`, `list_perception_bound_sounds`) + **12 v0.14.10 [Unreleased] MetaSound document introspection actions** (`list_metasounds`, `list_metasound_documents`, `get_metasound_document`, `get_metasound_summary`, `inspect_metasound_node_instance`, `get_metasound_document_connections`, `get_metasound_document_variables`, `get_metasound_user_parameters`, `search_metasound_document_nodes`, `get_metasound_info`, `get_metasound_dependencies`, `validate_metasound` — PR #18 by @alakangas, refactored into `audio_query` namespace; conditional on `WITH_METASOUND`) |
| MonolithLevelSequence | level_sequence | 8 | `ping`, `list_directors`, `get_director_info`, `list_director_functions`, `list_director_variables`, `list_event_bindings`, `find_director_function_callers`, `list_bindings` (v0.14.8 PR #45 by @yashabogdanoff) |
| MonolithBABridge | — | 0 (integration only) | |
| MonolithCore | bulk_fill | 2 | `apply` + `list_namespaces`. Central JSON-tree write dispatcher routing on `target_namespace` to per-namespace adapters registered via `FMonolithBulkFillRegistry`. Framework primitives ship in `MonolithCore` (`FMonolithReflectionWalker`, `FBulkFillSpec`, `FDryRunReport`, `FSchemaDescriptor`, `FMonolithDryRunGuard`). [Unreleased] adds `FMonolithReflectionWalker::ResolvePath` — a dotted+bracket path resolver (`A.B[key].C`) that walks struct members / array indices / map keys to a single leaf `FProperty*`+value pointer for **surgical** in-place edits (vs. `WriteTree`'s whole-collection rebuild), plus a public `WriteLeaf` forwarder so callers reuse the walker's value coercion. Backs the `blueprint.set_property_at_path` action. v0.14.11 [Unreleased] landed 12 per-namespace adapters: `blueprint`, `material`, `animation`, `niagara`, `ui`, `mesh`, `gas` (`#if WITH_GBA` stub-pattern), `combograph` (`#if WITH_COMBOGRAPH` stub-pattern), `ai`, `logicdriver` (`#if WITH_LOGICDRIVER` stub-pattern), `audio` (MetaSound paths `#if WITH_METASOUND`), `inventory` (an optional sibling-plugin adapter, conditionally gated). Live `bulk_fill_query("list_namespaces")` reports `available: true` for all 12 when the corresponding compile-time gates are set. Per-adapter fill_kind catalogues live in each per-module SPEC's "Bulk Fill & Describe Surface" section. |
| MonolithCore | describe | 3 | `schema` + `list_targets` + (2026-05-23, Phase 2) `action_schema`. `schema`/`list_targets` return rich `FSchemaDescriptor` trees (type names, ImportText forms, enum-value lists, clamp ranges, nested children) for the same 12 adapter namespaces as `bulk_fill`; `action_schema` returns a registered action's param schema (names, types, required, defaults, aliases, descriptions) by `(target_namespace, action)`. Companion read-only schema introspection. |
| MonolithReflectionIntel | decision | 5 | `list_decisions`, `get_decision`, `list_stale`, `find_supersession_chain`, `find_referent_decisions`. Deterministic markdown decision-record harvest (specs / plans / CHANGELOG / `.claude/rules`) into `decision_records` + `decision_supersedes` SQLite tables on top of `EngineSource.db`. Lazy bootstrap on first call + `FCoreUObjectDelegates::ReloadCompleteDelegate` refresh. All five carry `readOnlyHint + idempotentHint` + `EMonolithParamKind::DiskPath` tagging on `path_filter`. `list_decisions` + `list_stale` cursor-paginated via the v0.17.0 codec pattern. Phase 1 of Reflection Intelligence — v0.17.0 (`Plugins/Monolith/Docs/specs/SPEC_MonolithReflectionIntel.md`). |
| MonolithReflectionIntel | risk | 5 | `get_hotspot_score`, `get_cochange_pairs`, `get_file_churn`, `get_release_window_hotspots`, `list_conditional_gates`. Deterministic git-log mining (per-file churn + co-change pairs across the configured nested git repositories via `FPlatformProcess::CreateProc` against each repo's `.git/`), LOC-blend hotspot scoring (`0.6 * normalised_churn + 0.4 * normalised_loc`), and regex sweep for `#if WITH_*` macros + `bHas*` 3-location probes + `MONOLITH_RELEASE_BUILD` bypasses. Writes into 4 new SQLite tables (`git_file_churn`, `git_cochange_pairs`, `risk_hotspot_scores`, `reflect_conditional_gates`) on `EngineSource.db`. All five carry `readOnlyHint + idempotentHint` + `EMonolithParamKind::DiskPath` on path-style params. `get_cochange_pairs`, `get_release_window_hotspots`, `list_conditional_gates` cursor-paginated. Phase 2 of Reflection Intelligence — v0.17.0 (`Plugins/Monolith/Docs/specs/SPEC_MonolithReflectionIntel.md` §4). |
| MonolithReflectionIntel | cppreflect | 6 | `get_uclass`, `list_uproperties`, `list_ufunctions`, `find_interface_impls`, `find_class_specifier`, `list_class_specifiers`. UE 5.7 reflection-edge queries driven by direct reads of UHT artefacts (`Intermediate/Build/Win64/.../Inc/<Module>/UHT/*.gen.cpp`) cross-joined with `IAssetRegistry::GetDependencies` — no tree-sitter dependency, no ThirdParty vendoring. Writes into 6 new SQLite tables on `EngineSource.db`: `reflect_uclasses`, `reflect_uproperties`, `reflect_ufunctions`, `reflect_uinterfaces`, `reflect_uinterface_impls`, `cpp_asset_edges`. All six carry `readOnlyHint + idempotentHint`. `list_uproperties`, `list_ufunctions`, `find_interface_impls`, `find_class_specifier` cursor-paginated. `list_class_specifiers` ([Unreleased]) returns the distinct universe of tokens stored in the `flags` column of `reflect_uclasses` with a per-token class count — those tokens are UHT metadata keys (`IsBlueprintBase`, `BlueprintType`, `Abstract`), NOT raw C++ UCLASS specifiers, so it's the discovery companion that tells you what `find_class_specifier` can actually match. `find_class_specifier` ([Unreleased] enhancement) now applies an alias map (`Blueprintable` -> `IsBlueprintBase`), returns an honest not-captured note for dropped specifiers UHT never stores (`MinimalAPI` / `NotBlueprintable`), and matches case-insensitively. Phase 3a of Reflection Intelligence — v0.17.0 (`Plugins/Monolith/Docs/specs/SPEC_MonolithReflectionIntel.md` §5). Phase 3b native-tag tracking (`reflect_native_tag_decls` / `reflect_native_tag_externs` + `list_native_tags`) deferred — see §5b. |
| MonolithReflectionIntel | network | 4 | `list_replicated_classes`, `list_rpc_functions`, `list_onrep_handlers`, `audit_unbalanced_onreps`. UE 5.7 replication inspection driven by a second UHT-artefact regex sweep (independent of Phase 3a's reader) focused on per-property `MetaData` blocks carrying `ReplicatedUsing` tags, cross-joined with Phase 3a's `reflect_ufunctions`. Writes into 1 new SQLite table on `EngineSource.db`: `reflect_replicated_properties`. All four carry `readOnlyHint + idempotentHint`; all four cursor-paginated. Known limitation: bare `UPROPERTY(Replicated)` without `ReplicatedUsing` not detected — signal lives in UHT's `PropPointers[]` block which Phase 4a does not parse. Phase 4a of Reflection Intelligence — v0.17.0 (`Plugins/Monolith/Docs/specs/SPEC_MonolithReflectionIntel.md` §6). Phase 4b bare-Replicated detection deferred — see §9. |
| MonolithReflectionIntel | pipeline | 2 | `pr_review`, `release_readiness`. Read-only composer actions that fan out other registered Monolith actions serially on the game thread and aggregate results into a single payload. `pr_review` bundles `risk_query("get_hotspot_score")` + `risk_query("get_cochange_pairs")` + `decision_query("list_decisions")` + `source_query("audit_module_dep_reality")` + (optional) `blueprint_query("audit_cdo_drift")` per changed file. `release_readiness` bundles `monolith_status()` + `decision_query("list_stale")` + `risk_query("get_release_window_hotspots")` + the sentinel-list audit + CHANGELOG completeness audit from `.claude/rules/scoped/monolith-release.md`. Both carry `readOnlyHint + idempotentHint`. Phase 4a of Reflection Intelligence — v0.17.0 (`Plugins/Monolith/Docs/specs/SPEC_MonolithReflectionIntel.md` §8). |
| MonolithReflectionIntel | reflect | 1 | `rebuild_reflection_index` (no params) — [Unreleased] network-completeness workstream. Project-only force-rebuild of the RI reflection tables (`reflect_uclasses` / `reflect_uproperties` / `reflect_ufunctions` / `reflect_uinterfaces` / `reflect_uinterface_impls` / `cpp_asset_edges` + `reflect_replicated_properties`). Re-runs the RI indexers over PROJECT UHT artefacts only (`bIncludeEnginePlugins=false` — engine excluded). Exists because after an indexer code change there's no other clean repopulation trigger — lazy bootstrap only fires on table-absence, `OnReloadComplete` only on Live Coding, and `source_query("trigger_reindex")` is the full engine reindex. It is a WRITE/maintenance action (not read-only), idempotent, non-destructive — regenerates the tables deterministically from the on-disk UHT artefacts. Phase 4a network-completeness follow-up — `Plugins/Monolith/Docs/specs/SPEC_MonolithReflectionIntel.md` §6. |
| **Total** | | **~1,400+ actions across 25+ in-tree namespaces** (18 module namespaces — `animation` also owns the `WITH_CHOOSER`-gated `chooser` namespace — + 8 framework: `monolith` meta, `level_sequence`, `bulk_fill`, `describe`, plus the six Reflection Intel namespaces `decision` / `risk` / `cppreflect` / `network` / `pipeline` / `reflect`). The per-namespace rows above are authoritative for structure; exact integers are no longer tracked to the unit (see the approximate-count note at the top of this section) — query `monolith_discover()` (its `total_actions` field) for the live figure. **Conditional gating** (qualitative — no per-config counts): the `chooser` namespace is `WITH_CHOOSER`-gated and registers nothing without the Chooser plugin; CommonUI / GAS / MetaSound actions are gated by `WITH_COMMONUI` / `WITH_GBA` / `WITH_METASOUND` respectively and don't register without those plugins; ~45 procedural town-gen experimental actions are OFF by default (`bEnableProceduralTownGen`). The `ui` namespace re-exposes 4 aliased GAS actions, so distinct handler count is slightly below the registration total. Query `monolith_discover()` for the live figure in any given build configuration. | ~25+ in-tree namespaces — 18 module namespaces (incl. the `WITH_CHOOSER`-gated `chooser`, registered from `MonolithAnimation`) plus the framework namespaces (`monolith` meta, `level_sequence`, `bulk_fill`, `describe`, and the six Reflection Intel namespaces `decision` / `risk` / `cppreflect` / `network` / `pipeline` / `reflect`). The `monolith_guide` action stays inside the existing `monolith` meta-namespace. Query `monolith_discover()` for the live namespace set. |

**2026-05-30 — [0.18.0] niagara +7 (issue #64 Tranche 2 search & discovery actions):** Tranche 2 of the Niagara Blueprint-Callable surface ([`specs/SPEC_MonolithNiagara.md` § Blueprint-Callable Surface](specs/SPEC_MonolithNiagara.md#blueprint-callable-surface-issue-64)) shipped 7 new read-only `niagara` dispatcher actions: `search_by_parameter`, `search_by_data_interface`, `query_niagara`, `find_similar_systems`, `search_by_material`, `find_niagara_references`, `list_system_data_interfaces`. Also in the same workstream (no action delta): the editor-only `UMonolithNiagaraQueryLibrary` Blueprint-Callable surface (issue #64 Tranche 1, +17 BP nodes wrapping existing read-only actions — zero new dispatcher actions, zero packaged-build cost). `niagara` row 120 → 127; in-tree total 1387 → 1394; distinct 1382 → 1389 (1427 → 1434 w/ town gen); live `monolith_discover()` 1610 → 1617. PR #65 (next note) then chains +2 → niagara 129, in-tree 1396, distinct 1391, live 1619. Namespace count unchanged at 25. Verify the live count at release time via `monolith_discover()` per Action Count Discipline.

**2026-06-01 — niagara +2 (PR #65 HLSL direct editing):** PR #65 by @middle233 shipped 2 new in-tree `niagara` actions for direct CustomHlsl-node editing: `get_custom_hlsl_text` (read HLSL from a CustomHlsl node via public UPROPERTY reflection) and `set_custom_hlsl_text` (overwrite under `Modify()` + transaction with recompile). Both ride public UPROPERTY reflection and are always registered regardless of the `WITH_NIAGARA_WIZARD_PRIVATE` build gate. `niagara` row 127 → 129; in-tree total 1394 → 1396; distinct 1389 → 1391; live `monolith_discover()` 1617 → 1619. Namespace count unchanged at 25. Also in this change (no additional action count — enhancements to existing actions): sim-stage / event-handler module-stack selectors (`usage: "particle_simulation_stage"` and `usage: "particle_event"` on `get_ordered_modules` / `add_module` / `move_module` / `duplicate_module`), `add_simulation_stage` output-node materialization, `add_event_handler` returning `handler_index` / `usage_id` / `usage` and rejecting unresolved inter-emitter sources, and the `create_module_from_hlsl` ParameterMap bridge graph with strict input/output type validation. See [`specs/SPEC_MonolithNiagara.md`](specs/SPEC_MonolithNiagara.md). Verify the live count at release time via `monolith_discover()` per Action Count Discipline.

**2026-05-22 — `add_property_access` (+1, blueprint, always-on):** Phase 1 of [`Docs/plans/2026-05-22-monolith-ui-bp-gap-actions.md`](plans/2026-05-22-monolith-ui-bp-gap-actions.md). The per-namespace `blueprint` row above is updated (91 → 92). The aggregate figures in the **Total** row (in-tree 1318, distinct 1314, the conditional `WITH_*` variants, and live 1542) each shift +1 and are deferred to the next holistic count-audit — together with the pre-existing cross-reference drift where the §3/§3.2 overview, module-index, and agent-cheatsheet summaries report 89/89/86 against §12's authoritative per-namespace count.

**2026-05-23 — Phase 2 (+3, always-on):** Phase 2 of [`Docs/plans/2026-05-22-monolith-ui-bp-gap-actions.md`](plans/2026-05-22-monolith-ui-bp-gap-actions.md). `blueprint` +2 (`override_parent_function`, `save_dirty_assets`; row updated 92 → 94) and `describe` +1 (`action_schema`; row updated 2 → 3). The describe namespace now has 3 actions (`schema`, `list_targets`, `action_schema`). Also: a NodeGuid behavioral fix (#15) — every MonolithBlueprint node-creation path (`add_node`, `add_event_node`, `add_timeline`, `promote_pin_to_variable`, `add_comment_node`, plus Phase 1 `add_property_access`) now calls `UEdGraphNode::CreateNewGuid()`, no action-count delta. The aggregate figures in the **Total** row (in-tree, distinct, the conditional `WITH_*` variants, and live) each shift a further +3 on top of the deferred +1 above and are deferred together to the next holistic count-audit.

**2026-05-23 — Phase 3 (+4 ui, get_variables enhanced):** Phase 3 of [`Docs/plans/2026-05-22-monolith-ui-bp-gap-actions.md`](plans/2026-05-22-monolith-ui-bp-gap-actions.md). `ui` +4 (`set_widget_navigation_bulk`, `dump_widget_navigation`, `convert_border_to_common`, `reparent_widget_root` — all CommonUI-gated; row updated CommonUI 51 → 55, module-owned 129 → 133, full-stack 133 → 137). No-delta enhancements: `blueprint::get_variables` gained an `include_bind_widgets` dual source — it now enumerates both C++ `BindWidget`/`BindWidgetOptional` references (`source=bind_widget_meta`) AND pure-Blueprint tree widgets exposed as variables / `UWidget::bIsVariable==true` (`source=tree_variable`), deduped; and `blueprint::add_node` / `blueprint::resolve_node` CallFunction first-match resolution (#14) now biases toward `UWidget`-derived owning classes when `target_class` is omitted on a Widget Blueprint (class-generic `IsChildOf(UWidget)` reflection, never sibling-widget names). Live `monolith_discover()` now reports total_actions **1549** (was 1545 before Phase 3). The aggregate figures in the **Total** row (in-tree, distinct, the conditional `WITH_*` variants, and live) each shift a further +4 on top of the deferred +1/+3 above and are deferred together to the next holistic count-audit.

**2026-05-23 — Phase 4 (+1 ui, alias/guard ergonomics):** Phase 4 of [`Docs/plans/2026-05-22-monolith-ui-bp-gap-actions.md`](plans/2026-05-22-monolith-ui-bp-gap-actions.md). `ui` +1 (`set_widget_is_variable` — always-on, NOT CommonUI-gated; row updated always-on 78 → 79, module-owned 133 → 134, full-stack 137 → 138). No-count-delta ergonomics: `blueprint::add_function` `name` param gained a `function_name` alias; `blueprint::add_node` `target_class` param gained `function_class` + `member_class` aliases; `blueprint::add_node` `position` param gained a `pos` alias (silences the prior unknown-param warning); `ui::get_widget_tree` now guards an empty/missing `asset_path` with a clear `missing required asset_path parameter` error instead of a confusing downstream "not found". Live `monolith_discover()` now reports total_actions **1550** (was 1549 before Phase 4). The aggregate figures in the **Total** row (in-tree, distinct, the conditional `WITH_*` variants, and live) each shift a further +1 on top of the deferred +1/+3/+4 above and are deferred together to the next holistic count-audit.

**2026-05-23 — Part B (+17 blueprint, dataset ergonomics):** Part B (Phases B1–B6) of [`Docs/plans/2026-05-22-monolith-ui-bp-gap-actions.md`](plans/2026-05-22-monolith-ui-bp-gap-actions.md) — the dataset-ergonomics workstream. `blueprint` +17 (row updated 94 → 111). All 17 are PUBLIC, in-tree `blueprint`-namespace actions — **zero sibling-plugin actions** (per `monolith-release.md` Action Count Discipline, the full +17 counts toward the public total). Grouped by family:
- **DataTable (8):** `read_data_table`, `describe_data_table_schema`, `set_data_table_rows`, `remove_data_table_row`, `rename_data_table_row`, `duplicate_data_table_row`, `export_data_table`, `import_data_table`.
- **DataAsset (1):** `seed_data_asset` (create + populate a DataAsset atomically; reuses the bulk_fill `FMonolithReflectionWalker::WriteTree` path).
- **CurveTable (5):** `read_curve_table`, `set_curve_table_keys`, `add_curve_table_row`, `remove_curve_table_row`, `rename_curve_table_row` (first CurveTable surface in Monolith).
- **StringTable (3):** `read_string_table`, `set_string_table_entries`, `remove_string_table_entry`.

ZERO new module deps across all 17 (`MonolithBlueprint.Build.cs` unchanged). Live `monolith_discover()` now reports total_actions **1567** (was 1550 before Part B). The aggregate figures in the **Total** row (in-tree, distinct, the conditional `WITH_*` variants, and live) each shift a further +17 on top of the deferred +1/+3/+4/+1 above and are deferred together to the next holistic count-audit. **Known log-vs-registry drift (deferred):** the `MonolithBlueprintModule.cpp` startup LOG string was bumped to 105 by the implementers, but the real registered count is 111 — this is the same pre-existing log-string-vs-registry drift §12 already tracks for other modules; it is reconciled in the next holistic count-audit, not patched here.

**2026-05-27 — v0.16.0 (+4 editor, preview & inspection surface expansion):** Editor namespace gains 4 new in-tree actions — `capture_material_grid` and `capture_with_overlay` (composite-capture rendering pass), `inspect_material_pbr` and `inspect_texture_channels` (pure structural-data reads, no rendering). The Editor row above is updated 29 → 33. The existing `capture_scene_preview` is additionally **extended** with three new `asset_type` enum values (`static_mesh`, `skeletal_mesh`, `widget`) — schema widening only, no count delta. All 4 new actions are PUBLIC, in-tree `MonolithEditor`-namespace — **zero sibling-plugin actions** (per `monolith-release.md` Action Count Discipline). Plus 1 schema-only widening of MCP `initialize` instructions (`HandleInitialize` + Python proxy point agents at `monolith_discover` / `describe_query("action_schema")` / `monolith_guide` — Issue #62 by @middle233) — no count delta. Plus persistence fixes across 3 existing actions (`mesh.convert_to_hism`, `mesh.place_spline`, `ai.place_smart_object_actor` — Issue #63 by @Heiselisha) — no count delta. The aggregate figures in the **Total** row (in-tree, distinct, the conditional `WITH_*` variants, and live) each shift a further +4 on top of the deferred +1/+3/+4/+1/+17 above and are deferred together to the next holistic count-audit. The `.uplugin` Description and `SPEC_MonolithEditor.md` per-module figures have been updated in this commit to match (29 → 33; in-tree subtotal 1344 → 1348).

**2026-05-28 — niagara temporal-control surface (+9 niagara, Phases 1-4 of `plans/2026-05-28-niagara-timing-actions.md`):** Niagara namespace gains 9 new in-tree actions covering system warmup / fixed-tick / current-frame-data, emitter loop topology, sim-stage iteration count + execute behavior, particle-lifetime convenience writes, and bundled read aggregators (`get_system_timing` + `get_emitter_timing_summary`). The Niagara row above is updated 109 → 118. Composite writes — `set_warmup_profile`, `set_emitter_loop_profile`, `set_particle_lifetime` — replace the prior scattered `set_system_property` + `set_static_switch_value` + `set_module_input_value` round-trips with intent-named single-call surfaces. Sim-stage aliases (`set_sim_stage_iteration_count`, `set_sim_stage_execute_behavior`) reuse PR #65's `stage_index` / `stage_name` selector convention atop `set_simulation_stage_property`. Stateless emitters early-out from `set_emitter_loop_profile` with a hint rather than erroring. All 9 are PUBLIC, in-tree `MonolithNiagara`-namespace — **zero sibling-plugin actions** (per `monolith-release.md` Action Count Discipline). Design: `plans/2026-05-28-niagara-timing-actions-design.md`. The aggregate figures in the **Total** row (in-tree, distinct, the conditional `WITH_*` variants, and live) each shift a further +9 on top of the deferred +1/+3/+4/+1/+17/+4 above and are deferred together to the next holistic count-audit. Live `monolith_discover("niagara")` post-Phase-5 returns 118 actions — count verified 2026-05-28.

**2026-05-28 — niagara stateless-emitter timer surface (+1 niagara, Phases 0-3 of `plans/2026-05-28-niagara-stateless-timer.md`):** Niagara namespace gains 1 new in-tree action — `create_stateless_emitter` (standalone `UNiagaraStatelessEmitter` / Lightweight Emitter asset factory; uses `FindObject<UClass>(nullptr, "/Script/Niagara.NiagaraStatelessEmitter")` + type-erased `NewObject` to avoid coupling to Niagara's `Internal/Stateless/` headers). The Niagara row above is updated 118 → 119. Two existing actions are **extended** (no count delta, same call surface): `set_emitter_loop_profile` now detects standalone stateless assets via `StaticLoadObject` + class-name match and dispatches into `WriteStatelessLoopProfile` (reflection-based UPROPERTY write on the protected `EmitterState` `FNiagaraEmitterStateData`) — converting the previous hint-and-no-op early-out into a real write-path — and gains an optional `loop_duration_mode` param (`"Fixed"` / `"Infinite"`, mapping to `ENiagaraLoopDurationMode`; stateful path emits a warning when supplied since the stateful `EmitterState` module has no equivalent input). `get_emitter_timing_summary` similarly dispatches into the stateless reader on standalone assets — response carries `stateless: true`, the 4 `InitializeParticle` lifetime fields are `null`, and `sim_stages: []` (stateless emitters have no sim stages by design). All changes are PUBLIC, in-tree `MonolithNiagara`-namespace — **zero sibling-plugin actions** (per `monolith-release.md` Action Count Discipline). Design: `plans/2026-05-28-niagara-stateless-timer-design.md`. Plan: `plans/2026-05-28-niagara-stateless-timer.md`. The aggregate figures in the **Total** row (in-tree, distinct, the conditional `WITH_*` variants, and live) each shift a further +1 on top of the deferred +1/+3/+4/+1/+17/+4/+9 above and are deferred together to the next holistic count-audit.

**[Unreleased] — cppreflect discovery + find_class_specifier forgiveness (+1 cppreflect):** The `cppreflect` row gains `list_class_specifiers` (5 → 6) — returns the distinct universe of tokens stored in the `flags` column of `reflect_uclasses`, each with a per-token class count. Those tokens are UHT metadata keys (`IsBlueprintBase`, `BlueprintType`, `Abstract`), NOT raw C++ UCLASS specifiers; the action is the discovery companion that tells callers what `find_class_specifier` can actually match. No params. Same release enhances `find_class_specifier` (no count delta): an alias map (`Blueprintable` -> `IsBlueprintBase`), an honest not-captured note for dropped specifiers UHT never stores (`MinimalAPI` / `NotBlueprintable`), and case-insensitive matching. PUBLIC, in-tree `MonolithReflectionIntel`-namespace — **zero sibling-plugin actions**. The **Total** row above is already reconciled to this delta: in-tree 1384 → 1385, distinct 1380 → 1381 (1425 → 1426 w/ town gen), live 1607 → 1608. Namespace count unchanged (24 in-tree / 28 total — `cppreflect` already existed).

**2026-05-29 — [Unreleased] new `reflect` namespace + `rebuild_reflection_index` (+1 reflect, network-completeness workstream):** A new `reflect` namespace lands from `MonolithReflectionIntel` with one action — `rebuild_reflection_index` (no params). It is a project-only force-rebuild of the RI reflection tables (`reflect_uclasses` / `reflect_uproperties` / `reflect_ufunctions` / `reflect_uinterfaces` / `reflect_uinterface_impls` / `cpp_asset_edges` + `reflect_replicated_properties`), re-running the RI indexers over PROJECT UHT artefacts only (`bIncludeEnginePlugins=false`, engine excluded). It exists because after an indexer code change there is no other clean repopulation trigger — lazy bootstrap only fires on table-absence, `OnReloadComplete` only on Live Coding, and `source_query("trigger_reindex")` is the full engine reindex. WRITE/maintenance action (NOT read-only), idempotent, non-destructive — regenerates deterministically from on-disk artefacts. PUBLIC, in-tree `MonolithReflectionIntel`-namespace — **zero sibling-plugin actions**. The **Total** row above is reconciled to this delta: in-tree 1385 → 1386, distinct 1381 → 1382 (1426 → 1427 w/ town gen), live 1608 → 1609. Namespace count 24 → 25 in-tree (28 → 29 total — `reflect` is brand new). Same workstream improves network detection (no count delta): `list_replicated_classes` now captures bare `UPROPERTY(Replicated)` + `DOREPLIFETIME` (via `CPF_Net`) in addition to `ReplicatedUsing` — verified E2E against a host project's replicated character/attribute classes; `list_rpc_functions` switched to specifier-based detection (`reflect_ufunctions.specifiers` parsed from `EFunctionFlags`) instead of name-prefix. (At the time of this note the scan scope was the project GAME MODULE only, so RPCs declared in project plugins were out of scope — the subsequent plugin-aware scan-scope ladder resolved this; see [`Docs/specs/SPEC_MonolithReflectionIntel.md` §6.5](specs/SPEC_MonolithReflectionIntel.md).) Module spec: [`Docs/specs/SPEC_MonolithReflectionIntel.md`](specs/SPEC_MonolithReflectionIntel.md) §6.

**2026-06-04 — test/profiling harness Wave 1 (+12: editor +7, ai +2, project +3):** Editor / nav / asset-save tooling for the test/profiling harness. `editor` +7 (`list_dirty_packages`, `save_packages`, `run_pie_smoke`, `poll_pie_smoke`, `stop_pie_smoke`, `capture_pie_movement_clip`, `create_nav_harness_map`; row 33 → 40 — the `run_pie_smoke` family is an async session model because a synchronous in-handler engine pump re-enters `UWorld::Tick` and crashes — see [`specs/SPEC_MonolithEditor.md`](specs/SPEC_MonolithEditor.md) § Test/Profiling Harness). `ai` +2 (`rebuild_navigation`, `validate_nav_points`; Navigation category 24 → 26, row 221 → 223 — see [`specs/SPEC_MonolithAI.md`](specs/SPEC_MonolithAI.md) § Test/Profiling Harness). `project` +3 (`refresh_assets`, `get_saved_asset_state`, `cleanup_generated_assets` — the last allowlist-guarded to `/Game/Tests/Monolith/`; row 8 → 11 — see [`specs/SPEC_MonolithIndex.md`](specs/SPEC_MonolithIndex.md)). All 12 are PUBLIC, in-tree — **zero sibling-plugin actions** (per `monolith-release.md` Action Count Discipline). In-tree total 1396 → 1408; distinct 1391 → 1403 (1436 → 1448 w/ town gen). Namespace count unchanged at 25. Wave 2 (chooser namespace + animation graph-surgery + editor compile-error pre-flight) is documented in the next note. Verify the live count at release time via `monolith_discover()` per Action Count Discipline.

**2026-06-05 — test/profiling harness Wave 2 (+11: chooser +5 new namespace, animation +5, editor +1):** AnimBP reparent / chooser-rewire tooling, now runtime-proven. **New `chooser` namespace** (+5, `WITH_CHOOSER`-gated, registered from `MonolithAnimation`): `inspect_chooser`, `duplicate_chooser_tree`, `set_context_object_class`, `set_result_asset_reference`, `validate_chooser` — `UChooserTable` inspection + edit (see [`specs/SPEC_MonolithAnimation.md` § Chooser Namespace](specs/SPEC_MonolithAnimation.md)). `animation` +5 graph-surgery (`rebuild_evaluate_chooser_node`, `replace_evaluate_chooser_nodes`, `duplicate_reparent_and_sanitize`, `find_node_slice`, `remove_node_slice`; row 125 → 130 — delete+reflectively-respawn Evaluate-Chooser nodes, duplicate-and-reparent ABP classification, directional node-slice compute/remove; see [`specs/SPEC_MonolithAnimation.md` § Graph Surgery](specs/SPEC_MonolithAnimation.md)). `editor` +1 (`list_errored_blueprints`; row 40 → 41 — read-only `BS_Error && bDisplayCompilePIEWarning` scan feeding `run_pie_smoke`'s new `on_compile_errors` `refuse`/`suppress` policy; the module also gains a no-action-surface `FCoreDelegates::PreSlateModal` watcher emitting `MODAL_OPEN` log lines for external-agent recovery — see [`specs/SPEC_MonolithEditor.md`](specs/SPEC_MonolithEditor.md) § Test/Profiling Harness Wave 2 + § Module-Level Modal Watcher). All 11 are PUBLIC, in-tree — **zero sibling-plugin actions** (per `monolith-release.md` Action Count Discipline). In-tree total 1408 → 1419; distinct 1403 → 1414 (1448 → 1459 w/ town gen). Namespace count 25 → 26 in-tree (29 → 30 total). Live `monolith_discover()` 1631 → 1642 (verified against the live registry 2026-06-05). The 5 chooser actions are inert without the Chooser plugin (`WITH_CHOOSER`). Verify the live count at release time via `monolith_discover()` per Action Count Discipline.

**2026-06-06 — test/profiling harness Wave 16 (+5: chooser +1, animation +3, editor +1):** Addressing field-surfaced Monolith shortcomings — runtime-proven AnimBP / state-machine authoring + PIE introspection. `chooser` +1 (`set_evaluate_chooser_result_reference` — rewrite the child `UChooserTable` an EvaluateChooser/root/nested result row points at, `FEvaluateChooser`; `set_result_asset_reference` rejects these; row 5 → 6) and `duplicate_chooser_tree` gained two-pass duplicate-then-remap that recurses nested `FEvaluateChooser` + `FNestedChooser` references (`ResultsStructs`/`FallbackResult`/`FOutputObjectColumn`, recursing `NestedObjects`) with normalized path matching + per-row `row_remap_report` (no count delta — enhancement). `animation` +3 (`create_state_machine` + `build_state_machine` — declarative SM authoring via `FEdGraphSchemaAction_NewStateNode`, bool-var + automatic rules, float/expression rules deferred-per-element — in `MonolithAnimationActions.cpp`; + `sample_pie_anim_instance` — live PIE AnimInstance telemetry (class/AnimClass/mode/montage/SM-state/bone+socket) in the new `MonolithAnimationRuntimeActions.cpp`; row 130 → 133). `editor` +1 (`capture_anim_frames` — AnimSequence/BlendSpace/AnimBlueprint preview→PNG in an isolated `FPreviewScene`, no PIE; row 41 → 42). No-count enhancements in the same pass: `run_pie_smoke` + `capture_pie_movement_clip` `stages` staged startup hooks (`pre_pie`/`on_begin_play`/`after_n_ticks`/`before_capture`); `capture_pie_movement_clip` capture-validity heuristic + `view_target_actor` (+ reported `view_target`) + `runtime_identity` + `expected_anim_class` assert; `run_pie_smoke` + `load_level` world-leak handshake (already specced Wave 1/2); grouped `log_patterns` + teardown bucketing (already specced); `get_build_errors` `since_marker`/`since_iso`/`since_seconds`/`clear_baseline`/`exclude_categories` scoping + `compile_errors` vs `other_errors` bucketing; `capture_anim_frames`/`capture_pie_movement_clip` `/Game` virtual-path output-dir resolution (or hard-error); and `describe`/`bulk_fill` `target_namespace`/`target_action` now accept namespace/action aliases (no new action). All 5 new actions are PUBLIC, in-tree — **zero sibling-plugin actions** (per `monolith-release.md` Action Count Discipline). In-tree total 1419 → 1424; distinct 1414 → 1419 (1459 → 1464 w/ town gen). Namespace count unchanged at 26 in-tree (30 total). Live `monolith_discover()` 1642 → 1647 (verified against the live registry 2026-06-06). The 6 chooser actions are inert without the Chooser plugin (`WITH_CHOOSER`). Verify the live count at release time via `monolith_discover()` per Action Count Discipline.

**2026-06-07 — test/profiling harness gap-closure pass (+3 actions + behavior):** Closes field-surfaced read-side / ergonomics gaps. **+3 new public in-tree actions:** `animation.get_anim_graph_choosers` (walk an AnimBP's graphs for chooser-evaluating nodes), `animation.get_transition_rule` (read back an authored transition rule), `editor.author_map_settings` (WorldSettings GameMode override + `APlayerStart` spawning on any open map). **Param / behavior additions (no count delta):** `blueprint.seed_data_asset.read_back_values` (inline verify-after-write); `blueprint.get_cdo_properties` sanctioned as the canonical verify-after-write path (shared serializer); `blueprint.get_component_details` inherited-native-component fallback (+ `skeletal_mesh` / `anim_class` / `animation_mode` / `is_inherited_native`); `blueprint.get_blueprint_info.native_component_count`; `chooser.inspect_chooser.recursive`; `animation.get_nodes.include_anim_graph`; `animation.set_transition_rule` structured `rule` (kind=bool/auto/compare; expression-graph authoring deferred); `editor.run_pie_smoke` / `capture_pie_movement_clip` gained `actor_setup` (declarative spawn / DataAsset-apply / AI-move), `csv_profile`, `trace_channels`; `capture_pie_movement_clip` gained `discard_first_frames` + label-aware `view_target_actor`; `create_nav_harness_map` gained `game_mode_override` + `player_starts` + typed class/soft-class (`_C`-normalized) + object-array property support. New C++ helper `FMonolithReflectionReader` (read-side `FProperty`→JSON) added in `MonolithCore`, deduplicating two prior serializer copies (the CDO action copy + the DataAsset-indexer copy). All new actions are PUBLIC, in-tree — **zero sibling-plugin actions**. In-tree total ~1424 → ~1427; namespace count unchanged at 26 in-tree (30 total). Per the approximate-count note at the top of §12, exact integers are no longer tracked — query `monolith_discover()` for the live figure. See [`specs/SPEC_MonolithAnimation.md`](specs/SPEC_MonolithAnimation.md), [`specs/SPEC_MonolithBlueprint.md`](specs/SPEC_MonolithBlueprint.md), [`specs/SPEC_MonolithEditor.md`](specs/SPEC_MonolithEditor.md), [`specs/SPEC_MonolithCore.md`](specs/SPEC_MonolithCore.md), [`specs/SPEC_MonolithIndex.md`](specs/SPEC_MonolithIndex.md).

**[Unreleased] — animation ABP-authoring pack (+13 animation) + 2 extensions:** A from-scratch anim-graph-authoring surface for the `animation` namespace: `add_apply_additive`, `add_apply_mesh_space_additive`, `add_slot_node` (validates the slot name against the skeleton's slot groups), `add_save_cached_pose` / `add_use_cached_pose` (paired by `cache_name`), `set_output_pose_source` (wire a node into the AnimGraph Output / Root result pin), `set_state_result_source` (wire a node into a state machine state's result pin), `add_blend_by_int` (grown to `num_poses` pins), `set_sync_group`, `set_layered_blend_bones` (per-bone branch filters), `add_anim_control_rig_node` (`control_rig_class`; IO pins regenerate), `add_linked_anim_layer` (`layer_name` + optional `interface_class`), and `add_conduit` (a state-machine conduit whose bound graph is a transition-logic graph, not an anim graph). Plus 2 extensions (no count delta): `set_anim_node_pin_binding` now bootstraps the binding object on previously-unbound nodes (was: refused), and `auto_layout` gained a Blueprint-Assist-free `builtin` formatter (also the `auto` fallback) that lays out anim graphs by dependency-aware layering, so layout works in release builds where Blueprint Assist is compiled out. All 13 new actions are PUBLIC, in-tree — **zero sibling-plugin actions**. The MonolithAnimation row above is updated ~155 → ~170; namespace count unchanged. Per the approximate-count note at the top of §12, exact integers are no longer tracked — query `monolith_discover()` for the live figure. See [`specs/SPEC_MonolithAnimation.md`](specs/SPEC_MonolithAnimation.md).

**2026-06-07 — Motion Matching AI-wander pack (~28 new actions: animation +14 pack +4 retarget +1 read, chooser +4, blueprint +6; plus AI runtime classes + fixes):** End-to-end UE 5.7 Motion Matching authoring surface plus the runtime AI-wander surface that drives a Motion-Matching-locomotion character.

- `animation` +14 Motion Matching pack (PoseSearch normalization sets / asset-type-agnostic database entries / schema mirroring + channels / notifies / validation, plus the Pose-History + Motion-Matching anim-graph node configure/build actions: `create_normalization_set`, `add_database_to_normalization_set`, `set_database_normalization_set`, `add_database_entry`, `set_database_entry_tags`, `create_mirror_data_table`, `set_schema_mirror_data_table`, `configure_schema_channel`, `add_pose_search_notify` (8 notify-state kinds), `derive_schema_channels_from_skeleton`, `validate_pose_search_database`, `configure_pose_history_node`, `configure_motion_matching_node`, `build_motion_matching_node`); **+4 retarget create/run pack** (`create_ik_rig`, `create_ik_retargeter`, `set_retargeter_rigs`, `batch_retarget_animations` — auto-seed the default retarget op stack Pelvis / FKChains / RunIK / IKChains / RootMotion / CurveRemap + `AutoMapChains`, optional `auto_map`); **+1 read** (`get_anim_graph_output_connection`). See [`specs/SPEC_MonolithAnimation.md` § PoseSearch / Motion Matching action pack](specs/SPEC_MonolithAnimation.md).
- `chooser` +4 (`create_chooser_table` w/ `output_class` resolution, `add_chooser_column` Bool/Enum/GameplayTag/FloatRange/OutputObject with optional `enum_class` on `Enum` columns, `add_chooser_row`, `set_chooser_cell` typed-predicate per-kind cell write — chooser table authoring; row 6 → 10 — see [`specs/SPEC_MonolithAnimation.md` § Chooser Namespace](specs/SPEC_MonolithAnimation.md)).
- `blueprint` +6 (`set_anim_class`, `apply_movement_preset`, `add_engine_component_typed`, `scaffold_locomotion_input`, `validate_animbp_variable_contract`, `scaffold_motion_matching_character` (composite, now also sets the skeletal mesh) — Motion-Matching character scaffolding + inherited native-component CDO-override persistence; see [`specs/SPEC_MonolithBlueprint.md`](specs/SPEC_MonolithBlueprint.md)). Plus the read-only `get_inherited_component_override`.
- **AI runtime surface (no MCP-action delta — runtime C++ classes):** 3 `UBTTaskNode` classes (`BTTask_SetMaxWalkSpeed`, `BTTask_SetCrouch`, `BTTask_RandomizeFloat`, placed via `add_bt_node`) + 1 Blueprintable `AAIController` (`AMonolithBehaviorTreeAIController`, runs its BT `UPROPERTY` on `OnPossess` — the canonical auto-start path). See [`specs/SPEC_MonolithAI.md` § Motion Matching AI-wander runtime surface](specs/SPEC_MonolithAI.md).

**Fixes:** `build_motion_matching_node` now wires the chain to the AnimGraph Output Pose (previously left disconnected); `batch_retarget_animations` no longer produces frozen clips (op-stack seeding); `reorder_bt_children` order now persists (`NodePosX`-based); `build_behavior_tree_from_spec` now links `UBehaviorTree::BlackboardAsset`; pre-existing crashes hardened in `get_database_stats` (unbuilt PoseSearch databases) and `add_anim_graph_node` (bound-graph nodes — now uses `FGraphNodeCreator`); inherited native-component CDO overrides now persist (`set_anim_class` / scaffold mesh / `set_component_property` use structural-modify + compile). **New Build.cs deps:** `MonolithAnimation += BlendStackEditor`; `MonolithBlueprint += EnhancedInput`. All new actions are PUBLIC, in-tree — **zero sibling-plugin actions** (per `monolith-release.md` Action Count Discipline). The animation/blueprint additions are always-on; the chooser additions are `WITH_CHOOSER`-gated; the AI surface is conditional on the existing AI build gates. Namespace count unchanged at 26 in-tree (30 total). Per the approximate-count note at the top of §12, exact integers are no longer tracked — query `monolith_discover()` for the live figure.

**Note:** MonolithMesh includes 194 core actions (always registered) plus 45 experimental Procedural Town Generator actions (registered only when `bEnableProceduralTownGen = true`, default: false — known geometry issues). MonolithGAS is conditional on `#if WITH_GBA` — projects without GameplayAbilities register 0 GAS actions. MonolithComboGraph is conditional on `#if WITH_COMBOGRAPH` — projects without the ComboGraph plugin register 0 combograph actions. MonolithAI is conditional on `#if WITH_STATETREE` + `#if WITH_SMARTOBJECTS` — projects without these register 0 AI actions. MonolithLogicDriver is conditional on `#if WITH_LOGICDRIVER` — projects without Logic Driver Pro register 0 logicdriver actions. MonolithAudio MetaSound actions are conditional on `#if WITH_METASOUND` — projects without MetaSound get Sound Cue + CRUD + batch actions but no MetaSound graph building. The MonolithAnimation `chooser` namespace (5 actions) is conditional on `#if WITH_CHOOSER` (Chooser plugin at `Engine/Plugins/Chooser`) — projects without the Chooser plugin register 0 chooser actions. MonolithUI includes 78 always-on actions (Widget CRUD + Slot + Templates + Styling + v1 Animation + v2 hoisted Animation + Bindings + Settings + Accessibility + Hoisted Design Import + EffectSurface + Spec Builder + Type Registry diagnostic + v0.14.11 Tier 2 close-the-loop + Tier 3 scaffolders) plus 51 CommonUI actions (50 in `Source/MonolithUI/Private/CommonUI/*.cpp` + 1 inline `dump_style_cache_stats` lambda in `MonolithUIModule.cpp`, all registered only when `WITH_COMMONUI=1`). Projects without CommonUI register 78 `ui::` actions; the full-stack configuration registers 129 (133 counting the 4 GAS aliases). The Phase A–L architecture expansion (2026-04-26) added the Spec System (`build_ui_from_spec` / `dump_ui_spec_schema` / `dump_ui_spec`), Type Registry + per-type property allowlist, EffectSurface widget + sub-bag setters, and the dedup-driven Style Service. v0.14.11 [Unreleased] adds 12 Tier 2+3 close-the-loop ergonomics actions (8 Tier 2 + 4 Tier 3 scaffolders) on top of the Tier 1 correctness fixes — see [`specs/SPEC_MonolithUI.md`](specs/SPEC_MonolithUI.md) for the full breakdown. MonolithBABridge registers no MCP actions — it only provides the `IMonolithGraphFormatter` IModularFeatures bridge consumed by `auto_layout` in the blueprint, material, animation, and niagara modules. v0.14.11 [Unreleased] also lights up 12 `bulk_fill_query` / `describe_query` adapter implementations (the framework dispatchers were Phase 0 in v0.14.10); per-namespace fill_kind catalogues live in each per-module SPEC's "Bulk Fill & Describe Surface" section.

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

---

## 14. Niagara Summary & Validation Semantics

`niagara.get_system_summary` and `niagara.get_emitter_summary` now accept an optional `detail_level: "compact" | "full"` parameter (default `"compact"`). Both actions remain schema-compatible — counts unchanged (see §12).

- **`compact`** (default): keeps the existing terse payload plus the new per-emitter `role_hint`, `spawn_location_mode`, `requires_persistent_ids`, `incoming_event_count`, `outgoing_link_count`, `event_generator_count`, `consumed_events[]`, `generated_events[]` summary fields. System-level adds `inter_emitter_link_count` + `independent_burst_emitters[]`. Use for orientation before reasoning about a system.
- **`full`**: additionally emits per-emitter `incoming_events[]`, `outgoing_links[]`, `event_generators[]`, `location_modules[]`, `semantic_notes[]`, and system-level `inter_emitter_topology[]`. Use when emitters may be linked by Niagara events (Death/Location/Collision) and you must reason about where particles spawn or explode.

`niagara.validate_system` now reasons about event chains in addition to the existing per-emitter checks: it walks the topology built by `CollectTopologyEdges` once, then per-emitter classifies source/receiver roles (`role_hint == "event_source" / "event_receiver" / "burst_receiver" / "independent_burst" / "shell_event_source" / "trail_follower"`), reports unresolved `SourceEmitterID`s, warns when a receiver consumes an event the named source does not generate, surfaces `requires_persistent_ids` guidance for any emitter participating in an event chain, and flags `mixed_event_and_local_shape` placement ambiguity. Per-emitter semantic analysis is cached in a `TMap<FGuid, FMonolithNiagaraEmitterSemantic>` keyed by emitter handle GUID so the inner source-emitter lookup is O(1) — replaces an O(N^2) re-analysis path that would have re-walked module graphs per source-emitter probe on event-heavy systems.

**Breaking shape change in `get_emitter_summary` (v0.14.11):** the legacy `event_handlers[]` array is removed. Callers reading `event_handlers[].source_emitter_id` migrate to `incoming_events[].source_emitter_id` (`full` only) or `consumed_events[]` for the canonicalised event-name list (`DeathEvent` / `LocationEvent` / `CollisionEvent`). All other existing fields preserved.

Per-emitter `role_hint` enum values are heuristic (driven by emitter-name substring + module-graph topology) and are intended as AI-guidance hints rather than authoritative classifications. Trust them for orientation, not for compile-time correctness gates. (WISHLIST) richer renderer-aware classification (mesh-renderer + burst name → "shell_burst_visual") is deferred to a follow-up.

### 14.1 Universal Response-Shaping Params (Phase 1.0, 2026-05-27)

Distinct from §14's domain-specific `detail_level` knob, every MCP action in the registry now accepts three opt-in universal params that reshape the JSON response post-dispatch. They are appended to the K3 unknown-key allowlist at registry level (see `MonolithToolRegistry.cpp` around line 115) so no per-action schema edit was required. The post-filter is implemented by `ApplyResponseShaping` in `MonolithCore/Public/MonolithJsonUtils.h`.

| Param | Type | Effect |
|-------|------|--------|
| `_fields` | `string[]` | Whitelist — keep ONLY these top-level response keys. Empty array is a no-op (does NOT strip everything). |
| `_omit` | `string[]` | Blacklist — drop these top-level response keys. Mutually exclusive with `_fields` (collision appends a warning, `_fields` wins). |
| `_compact_json` | `bool` | Drop top-level keys whose value is `null`, `""`, `{}`, or `[]`. |

**Underscore prefix is mandatory.** The unprefixed `fields` token already collides with `MonolithBlueprintStructActions::create_struct_with_fields`, so the universal params reserve `_*` to stay collision-free across the action surface forever.

**Top-level keys only in Phase 1.** JSONPath / JMESPath grammar and nested-traversal shaping are deferred — out of scope for this phase per the plan's §2 kill list. (WISHLIST) nested-path shaping.

**Warnings channel.** When `_fields` + `_omit` are both non-empty, the post-filter appends a free-text entry to the same `warnings[]` array that K3 unknown-param soft-warns and Survivor D AssetPath rewrites already populate. See §JSON-RPC error catalogue note at §2 — the channel's semantic scope now covers three sources.

**MCP wire engagement (fix landed `f7c5b57`, 2026-05-27).** Claude Code's MCP client serialises array-valued top-level arguments as JSON-encoded STRINGS (e.g. `_fields:"[\"count\"]"` rather than `_fields:["count"]`). `MonolithHttpServer.cpp:691-701` already special-cases this quirk for the `params` key; `MonolithJsonUtils::ReadStringArrayParam` now mirrors the same fallback — native `TryGetArrayField` FIRST (back-compat with automation tests and offline CLI callers), then `TryGetStringField` + `FJsonSerializer::Deserialize` validating `EJson::Array` before populating Out. The `_compact_json` bool read carries a parallel `TryGetStringField` + `FCString::ToBool` fallback. A `UE_LOG(LogMonolith, Verbose, ...)` line fires inside the recovery branches only, making future serialization-shape regressions observable without spamming default verbosity. Live-verified via `monolith_status({_fields:["version","total_actions"]})` returning exactly those two keys.

### 14.2 Schema-Tagged Param Kinds (`EMonolithParamKind`, Phase 1.0, 2026-05-27)

`FParamSchemaBuilder` (in `MonolithCore/Public/MonolithParamSchema.h`) now stamps an `EMonolithParamKind` enum on each declared param. The enum is serialised onto the per-param schema JSON as a string field `"kind"`, emitted only when non-default to keep `tools/list` bytes lean.

| Kind | Wire string | Dispatcher behaviour |
|------|-------------|----------------------|
| `Other` (default) | (omitted) | No path semantics. Never rewritten. Existing `.Required` / `.Optional` calls keep this default. |
| `AssetPath` | `"AssetPath"` | `/Game/...` style paths. Dispatcher rewrites `\` → `/` at dispatch time and appends a surfaced warning to `warnings[]`. Never silent. |
| `DiskPath` | `"DiskPath"` | Native OS path. Explicit opt-OUT for clarity. Never rewritten. |
| `GameplayTag` | `"GameplayTag"` | `A.B.C` tag string. Reserved; never rewritten in Phase 1. |

**Builder overloads.** Four sugar methods opt a param into a non-`Other` kind without touching the legacy `Required` / `Optional` signatures:

- `RequiredAssetPath(Name, Description)` / `OptionalAssetPath(Name, Description)`
- `RequiredDiskPath(Name, Description)` / `OptionalDiskPath(Name, Description)`

All four bind `Type = "string"` and `Default = ""`. Use the non-sugar overloads when a non-string type or explicit default is required.

**Dispatch ordering.** The `AssetPath` rewrite block in `FMonolithToolRegistry::ExecuteAction` runs AFTER K2 alias rewrite (so the rewrite sees the canonical key) and BEFORE the K3 unknown-key check. Only `Kind == AssetPath` params are rewritten; `DiskPath` / `GameplayTag` / `Other` all pass through untouched. Warnings funnel into the same `warnings[]` channel as §14.1's response-shaping warnings and the K3 unknown-param soft-warns.

**Per-namespace param tagging (Phase 1.1, SHIPPED 2026-05-27, commit `aaa557a`).** The 87-file audit sweep tagged ~913 `.Required(...)` / `.Optional(...)` call sites as `AssetPath` and ~18 as `DiskPath` across all 15 in-tree module namespaces (`MonolithAI`, `MonolithAnimation`, `MonolithAudio`, `MonolithBlueprint`, `MonolithComboGraph`, `MonolithEditor`, `MonolithGAS`, `MonolithLevelSequence`, `MonolithLogicDriver`, `MonolithMaterial`, `MonolithMesh`, `MonolithNiagara`, `MonolithSource`, `MonolithUI`, plus framework `MonolithCore` `bulk_fill`/`describe`). The dispatcher backslash→forward-slash rewrite now engages on ~900 real entry points instead of zero. ~50 params left as `Kind == Other` (conservative): class identifiers, dotted property/struct paths (`StateTree property_path`, `ControlRig struct_path`, AbpWrite `property_path`), Outliner folder paths, substring/glob filters, default-bearing optionals (sugar overloads do not accept defaults), and ambiguous semantics. Counter-examples explicitly tagged `DiskPath` (NEVER `AssetPath`) include `MonolithSourceActions HandleReadFile file_path` (DB stores native backslashes), `MonolithMeshTechArtActions FBX export file_path`, `MonolithMeshDebugViewActions output_path/output_dir`, `MonolithGASTagActions/EditorActions import source_path`, `MonolithLogicDriverSpecActions json_path_or_data`, and `MonolithMaterialActions include_file_paths` (HLSL virtual paths). `MonolithGASCueActions save_path_prefix` left as `Other` (handler expects trailing-slash directory join semantics). Tracking: `Docs/plans/2026-05-27-mcp-llm-ergonomics.md` §3.D. No `*.Build.cs` / `.uplugin` / module-dep delta; pure call-site refactor atop Phase 1.0 sugar overloads.

### 14.3 MCP Tool Annotations on `tools/list` (Phase 2, 2026-05-27)

`tools/list` now serialises four MCP-spec hint fields per tool so LLM clients can pre-filter destructive vs read-only calls without dispatching them first. Source: `FMonolithActionInfo` (per-action) and `FMonolithDispatcherAnnotations` (per-dispatcher), declared in `MonolithCore/Public/MonolithToolRegistry.h`.

| Field | Wire key | Meaning |
|-------|----------|---------|
| `bReadOnlyHint` | `readOnlyHint` | Tool performs no state mutation. |
| `bDestructiveHint` | `destructiveHint` | Tool may destroy / overwrite project state. |
| `bIdempotentHint` | `idempotentHint` | Repeat calls with identical params yield identical effect. |
| `Title` | `title` | Short human-readable display label. |

**Wire encoding.** Annotations are only emitted on `tools/list` when at least one field is non-default — avoids ~1567 × 4 bytes of boilerplate per session init. Serialisation lives in `FMonolithHttpServer::HandleToolsList`.

**Per-action vs per-dispatcher.** Single-action top-level tools (e.g. `monolith_discover`, `monolith_status`, `monolith_guide`, `monolith_reindex`) carry annotations on the underlying `FMonolithActionInfo`. Namespace dispatcher tools (`source_query`, etc.) carry annotations on a separate `FMonolithDispatcherAnnotations` struct via `FMonolithToolRegistry::SetDispatcherAnnotations` / `GetDispatcherAnnotations`, because per-action annotation is semantically wrong when sibling actions inside the same dispatcher disagree on `destructive` / `read-only`.

**Annotated in v0.16.0+.** Five tools audited and tagged: `monolith_discover` (read-only + idempotent + title), `monolith_status` (read-only + idempotent + title), `monolith_guide` (read-only + idempotent + title), `monolith_reindex` (idempotent only — modifies cache state), `source_query` (read-only + idempotent — dispatcher-level, all child actions are pure index reads). Set sites: `MonolithCoreTools.cpp`, `MonolithGuideTool.cpp`, `MonolithSourceActions.cpp`.

**Per-action annotation across the full 1567-action surface (WISHLIST).** Architecturally blocked by the 15-dispatcher collapse pattern — each `*_query` tool exposes many child actions with heterogeneous destructiveness. Dispatcher-level annotation is the only honest place for these. Per-action annotation would only be meaningful for the 5 standalone top-level tools, which is already done. Do not propose extending it.

**Dispatcher `description` does NOT inline the action list (2026-06-13).** Each domain dispatcher (`source_query`, `material_query`, etc.) carries a short `description` of the form `Query the <namespace> domain. Call monolith_discover("<namespace>") for the action list.` The full per-namespace action list is **not** duplicated into the description prose — it is carried authoritatively by the `action` property's `enum` array in the dispatcher's `inputSchema` (the closed value constraint typed MCP clients validate against). Inlining the list into `description` as well was a ~32k-char (~41% of the whole `tools/list` body) pure duplication; it was removed in the description build inside `FMonolithHttpServer::HandleToolsList`. The action-name array still drives the `enum` build (unchanged); only the prose copy is gone. Clients that need the human-readable list fetch it on demand via `monolith_discover("<namespace>")`. Net `tools/list` body: ~77.8k → ~46k bytes.

### 14.4 `did_you_mean` Fuzzy Match on Dispatch Errors (Phase 2, 2026-05-27)

Unknown action / unknown namespace dispatch errors now carry a `data.suggestions` payload listing the top-3 closest registry keys. Implementation: `MonolithCore/Public/MonolithFuzzyMatch.h` + `MonolithFuzzyMatch.cpp`, called from `FMonolithToolRegistry::ExecuteAction` around lines 199–251.

**Algorithm.** UE's `Algo::LevenshteinDistance` over the registry keyspace. Normalised score `1.0 - dist/max(len)` so closer matches score higher. Top-3 sorted descending.

**Concurrency pattern.** Snapshot the registry keyspace under `FScopeLock`, drop the lock, then score and sort. The hot dispatch loop's lock is held only for the snapshot — scoring runs lock-free off a local copy. No read-path latency spike on dispatch.

**Engage conditions.** Two paths only:

1. Unknown action name within a known namespace — `error.data.kind = "action"`, suggestions are sibling actions in the same namespace ranked by distance to the supplied action name.
2. Unknown namespace (top-level tool name unrecognised) — `error.data.kind = "namespace"`, suggestions are registered namespace names ranked by distance.

**Reachability scope (RI handover #11, 2026-05-29).** `did_you_mean` targets RAW / enumless MCP callers — the offline CLI (`monolith_query.exe`) and any client that posts an arbitrary `action` string. It is effectively unreachable from the typed Claude Code MCP client, because each per-namespace dispatcher advertises its `action` as a closed enum in the `tools/list` inputSchema, so an unknown action is rejected by the client before it can reach the dispatcher. This is by design, not a gap: the enum guard means the typo can't occur on the typed path. Bringing the suggestions to the offline CLI is tracked separately (handover #8).

**Error payload shape.**

```json
{
  "error": {
    "code": -32601,
    "message": "...",
    "data": {
      "kind": "action",
      "suggestions": [
        { "namespace": "blueprint", "action": "create_blueprint", "score": 0.88 },
        ...
      ]
    }
  }
}
```

**Out of scope.** Asset-path and property-name fuzzy matching deliberately omitted: O(N·L²) over a 10K+ asset registry kills the hot path, and property-name suggestions overlap with the K3 unknown-key warning channel — adding noise without information. (WISHLIST) K3 unknown-key counter as telemetry feedback for evaluating future retry-thrash interventions.

### 14.5 `source_query("search_source")` Cursor Pagination (Phase 3, 2026-05-27)

`source_query("search_source")` now supports opaque cursor pagination for large result sets. Implementation: `MonolithSource/Public/MonolithCursorCodec.h` + `MonolithCursorCodec.cpp`, called from `FMonolithSourceActions::HandleSearchSource`. Count helpers added to `FMonolithSourceDatabase`.

**In-params.**

| Param | Type | Meaning |
|-------|------|---------|
| `cursor` | `string?` | Optional opaque base64+JSON envelope. Omit on page 0; pass the previous response's `next_cursor` for subsequent pages. |

**Out-params.**

| Param | Type | Meaning |
|-------|------|---------|
| `next_cursor` | `string?` | Cursor to fetch the next page. `null` / omitted on the last page. |
| `total_estimate` | `int?` | Total row count across all pages. Page 0 only — server-side `MATCH COUNT(*)` against the symbol + source FTS tables. ~50–200 ms cold cache, then cached inside the cursor for subsequent pages. |

**Cursor envelope.** Base64-encoded JSON of shape `{ query_hash, symbol_page, source_page, cached_total_estimate }`. The decoder is fallible — empty / garbage / malformed input returns a clean `INVALID_CURSOR` error, never a panic.

**Hard cap.** 1000 rows total per query. A request with `limit=2000` is clamped to 1000.

**Query-hash mismatch.** If the supplied cursor's `query_hash` does not match the current request's query parameters, the dispatcher returns a clean `INVALID_CURSOR` error rather than silently serving the wrong slice.

**Rerun-slice scheme, not keyset.** FTS5 `bm25()` rank is unstable under inserts / deletes (the rank drifts on next index update). Keyset / row-id cursors would silently skip or duplicate rows. The honest option for a moving FTS index is to rerun the query and slice the result set — accepting the rerun cost in exchange for correctness. Documented in the Phase 3 design audit.

**`project_query("search")` cursor pagination (WISHLIST).** Architecturally blocked by `FMonolithIndexDatabase::FullTextSearch` at `MonolithIndexDatabase.cpp:1017-1079`, which performs a UNION-and-resort across multiple FTS tables without a deterministic ordering. A CTE / `UNION ALL` refactor with stable ordering is the prerequisite; deferred to a future Phase 5+ workstream.

### 14.6 Proxy Call Log (Phase 4, 2026-05-27)

Both proxies now emit a one-line-per-call JSONL log to `Saved/Logs/MonolithCalls.jsonl` (project-root-relative). Local-only, no phone-home.

**Schema.** Eight fields per line:

| Field | Type | Meaning |
|-------|------|---------|
| `ts` | string | ISO-8601 UTC timestamp. |
| `namespace` | string | Top-level tool name. |
| `action` | string | Action name within the namespace (or `""` for single-action tools). |
| `params_hash` | string | SHA-1 hex digest over canonicalised params JSON (sorted keys, no whitespace). |
| `duration_ms` | int | Round-trip wall time including server dispatch + JSON parse. |
| `ok` | bool | `true` on JSON-RPC success, `false` on JSON-RPC error. |
| `error_code` | int? | JSON-RPC error code on failure; omitted on success. |
| `result_bytes` | int | Size of the result body. |

**Canonicalisation.** Params are JSON-canonicalised (recursively sorted object keys, no whitespace) before hashing — `FCrc::StrCrc32` is NOT used; SHA-1 over the canonical bytes is the contract. Identical params shapes hash identically regardless of input key order.

**Parity.** Native proxy (`Tools/MonolithProxy/monolith_proxy.cpp`) and Python fallback (`Scripts/monolith_proxy.py`) implement the same schema. Native requires `build_proxy.bat` rebuild + Claude Code MCP reconnect to engage; Python picks up on next Claude Code start.

**Opt-out.** Set `MONOLITH_CALL_LOG=0` in the environment. Default is on.

**Rotation.** User-managed — delete `Saved/Logs/MonolithCalls.jsonl` to reset. The proxy appends; it does not truncate, rotate, or cap file size.

---

## 15. Guide Tool

`monolith_guide` (the `guide` action in the `monolith` meta-namespace) is an editorial cross-namespace workflow guide for AI agents — especially **external public-Monolith users** who lack a project `CLAUDE.md`, private skills, or an agent registry to orient them. It is hand-authored prose, not auto-generated from SPECs.

**Hybrid source.** The response is `Docs/MONOLITH_GUIDE.md` (loaded via `FFileHelper`, cached behind a session-lived `FCriticalSection`-guarded `TOptional<FString>`, split on `^## ` H2 headers) plus a **live registry overlay** — per-namespace action counts read from the running `FMonolithToolRegistry` and the plugin version, so the counts a caller sees always match the live build rather than a stale doc snapshot. The markdown cache refreshes on editor restart (no live file-watcher).

**Six sections.** `onboarding`, `recipes`, `decisions`, `errors`, `skills_map`, `gotchas`. Call `monolith_guide()` for the full index plus all sections, or `monolith_guide(section="recipes")` to return a single H2 section and bound context cost. An unknown section name returns a validation error listing the valid section keys.

**Pull-only, zero per-call cost.** No success response anywhere in Monolith carries guide content; the only breadcrumb is the single `guide_hint` string on the no-filter `monolith_discover()` response. Editorial content is fetched on demand, never pushed.

**No duplication.** The guide deliberately omits a pipelines section — cross-module pipeline chains live in [§13 Pipelines](#pipelines) and are cross-linked, not re-authored. It also omits a per-namespace action table (that is §12 + `Docs/references/MCP.md`). `skills_map` points at `Skills/<topic>/SKILL.md` rather than inlining skill bodies; `gotchas` cross-links `Docs/references/UE57Gotchas.md`.

**Offline parity.** `monolith_query.exe monolith guide` serves the same section-keyed surface from the standalone CLI.
