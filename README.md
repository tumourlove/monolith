# Monolith

**One plugin. Every Unreal domain. Zero dependencies.**

[![UE 5.7+](https://img.shields.io/badge/Unreal-5.7%2B-blue)](https://unrealengine.com)
[![License: MIT](https://img.shields.io/badge/License-MIT-green.svg)](LICENSE)
[![MCP](https://img.shields.io/badge/Protocol-MCP-purple)](https://modelcontextprotocol.io)

---

## What is Monolith?

Monolith is an Unreal Engine editor plugin that gives your AI full read/write access to your project through the [Model Context Protocol (MCP)](https://modelcontextprotocol.io). Install one plugin, point your AI client at one endpoint, and it can work with Blueprints, Materials, Animation, Niagara, Audio (Sound Cues, MetaSounds), AI (Behavior Trees, State Trees, EQS, Smart Objects), Gameplay Ability System, Logic Driver state machines, project configuration, and more.

It works with **Claude Code**, **Cursor**, or any MCP-compatible client. If your AI tool speaks MCP, it speaks Monolith.

> **Platform:** Windows, macOS, Linux.

## Why Monolith?

Most MCP integrations register every action as a separate tool, which floods the AI's context window and buries the actually useful stuff. Monolith uses a **namespace dispatch pattern** instead: each domain exposes a single `{namespace}_query(action, params)` tool, and a central `monolith_discover()` call lists everything available. Small tool list (20 tools), massive capability (1293 actions across 17 modules; sibling plugins push it past 1460 when loaded). The AI gets oriented fast and spends its context on your actual problem.

## What Can It Actually Do?

**Blueprint (89 actions)** — Full programmatic control of every Blueprint in your project. Create from any parent class, build entire node graphs from a JSON spec, add/remove/connect/disconnect nodes in bulk, manage variables, components, functions, macros, and event dispatchers. Implement interfaces, reparent hierarchies, edit construction scripts, read/write CDO properties on any Blueprint or DataAsset. The auto-layout engine uses a modified Sugiyama algorithm so AI-generated graphs actually look clean. Compare two Blueprints side-by-side, scaffold from templates, manage data tables, user defined structs and enums. Hand the AI a description and it builds the whole thing — or point it at an existing Blueprint and it'll surgically rewire what you need.

**Material (63 actions)** — Create materials, material instances, and material functions from scratch. Build entire PBR graphs programmatically — add expressions, connect pins, auto-layout, recompile. Drop in custom HLSL nodes. Import textures from disk and wire them directly into material slots. Batch-set properties across dozens of instances at once. Render material previews and thumbnails without leaving the AI session. Preview textures with full metadata, check tiling quality with anti-tiling analysis, batch delete expressions, clear entire graphs. Full material function support: create, build internal graphs, export/import between projects. Get compilation stats, validate for errors, inspect shader complexity. Covers the full material workflow from creation to validation.

**Animation (118 actions)** — The entire animation pipeline, end to end. Create and edit sequences with bone tracks, curves, notifies, and sync markers. Build montages with sections, slots, blending config, and anim segments. Set up 1D/2D Blend Spaces and Aim Offsets with sample points. **Animation Blueprint graph writing** — add states to state machines, create transitions, set transition rules, add and connect anim graph nodes, set state animations. AI can build ABP locomotion setups programmatically, not just read them. PoseSearch integration: create schemas and databases, configure channels, rebuild the search index. Control Rig graph manipulation with node wiring and variable management. Physics Asset editing for body and constraint properties. IK Rig and Retargeter support — chain mapping, solver configuration, the works. Skeleton management with sockets, virtual bones, and curves. 118 actions covering the full animation pipeline.

**Niagara (109 actions)** — Full system and emitter lifecycle — create, duplicate, configure, compile, save. Module CRUD with override-preserving reorder so you don't blow away artist tweaks. Complete dynamic input lifecycle: attach inputs, inspect the tree, read values, remove them. Event handler and simulation stage CRUD. Niagara Parameter Collections with full param management. Effect Type creation with scalability and culling configuration. Per-quality-level scalability settings. Renderer helpers for every type — mesh assignment, ribbon presets (trail, beam, lightning, tube), SubUV and flipbook setup. Data interface configuration and property inspection handles JSON arrays and structs natively. Diff two systems to see exactly what changed. Clone overrides between modules, duplicate modules, discover parameter bindings, inspect module outputs, rename user parameters. Batch execute with read-only optimization so queries don't trigger unnecessary recompiles. Full `export_system_spec` and `import_system_spec` with merge mode. Covers the full Niagara workflow from system creation to final polish.

**UI (96 actions)** — Widget Blueprint CRUD with full widget tree manipulation. 42 UMG baseline actions plus 50 CommonUI actions (CommonUI conditional on `WITH_COMMONUI`, 9 categories shipped v0.14.0). 4 GAS attribute-binding actions also surface in the `ui` namespace as aliases. Pre-built templates for common game UI: HUD elements, menus, settings panels, confirmation dialogs, loading screens, inventory grids, save slot lists, notification toasts. Style everything — brushes, fonts, color schemes, batch style operations. Create keyframed widget animations. Full game scaffolding: settings systems, save/load, audio config, input remapping, accessibility features. Run accessibility audits, set up colorblind modes, configure text scaling. Covers the full UI workflow from widget creation to accessibility.

**Editor (22 actions)** — Trigger full UBT builds or Live Coding compiles, read build errors and compiler output, search and tail editor logs, get crash context after failures. Capture preview screenshots of any asset — materials, Niagara systems, meshes. Capture multi-frame GIF sequences. Import textures, stitch flipbooks, delete assets, create blank maps from the factory, query module status. The AI can compile your code, read the errors, fix the C++, recompile, and verify the fix — all without you touching the editor.

**Config (6 actions)** — Full INI resolution chain awareness: Base, Platform, Project, User. Ask what any setting does, where it's overridden, what the effective value is, and how it differs from the engine default. Search across all config files at once. Perfect for performance tuning sessions where you want the AI to just sort out your INIs.

**Source (11 actions)** — Search over 1M+ Unreal Engine C++ symbols instantly. Read function implementations, get full class hierarchies, trace call graphs (callers and callees), verify include paths — all against a local index, fully offline. The native C++ indexer runs automatically on editor startup. No Python, no setup. Optionally index your project's own C++ source for the same coverage on your code. The AI never has to guess at a function signature again.

**Project (7 actions)** — SQLite FTS5 full-text search across every indexed asset in your project. Find assets by name, type, path, or content. Trace references between assets. Search gameplay tags. Get detailed asset metadata. The index updates live as assets change and covers marketplace/Fab plugin content too — 15 deep indexers registered including DataAsset subclasses.

**Mesh (240 actions)** — The biggest module by far. 195 core actions across 22 capability tiers, plus 45 procedural town generation actions (work-in-progress -- disabled by default, and unless you're willing to dig in and help improve it, best left alone for now). Mesh inspection and comparison. Full actor CRUD with scene manipulation. Physics-based spatial queries (raycasts, sweeps, overlaps) that work in-editor without PIE. Level blockout workflow with auto-matching and atomic replacement. GeometryScript mesh operations (boolean, simplify, remesh, LOD gen, UV projection). Horror spatial analysis — sightlines, hiding spots, ambush points, zone tension, pacing curves (WIP). Accessibility validation with A-F grading. Lighting analysis (WIP), audio/acoustics with Sabine RT60 and stealth maps (WIP), performance budgeting (WIP). Decal placement with storytelling presets. Level design tools for lights, volumes, sublevels, prefabs, HISM instancing. Tech art pipeline for mesh import, LOD gen, texel density, collision authoring. Context-aware prop scatter on any surface. Procedural geometry — parametric furniture (15 types), horror props (7 types), architectural structures, mazes, pipes, terrain. Genre preset system for any game type. Encounter design with patrol routes, safe room evaluation, and scare sequence generation. Full accessibility reporting.

**GAS (135 actions)** — Complete Gameplay Ability System integration. 131 GAS-namespace actions plus 4 widget attribute-binding actions also aliased into the `ui` namespace. Create and manage Gameplay Abilities with activation policies, cooldowns, costs, and tags. Full AttributeSet CRUD — both C++ and Blueprint-based (via optional GBA plugin). Ships with `ULeviathanVitalsSet` AttributeSet template (Phase J F4) so projects without GBA still get a working starter set. Gameplay Effect authoring with modifiers, duration policies, stacking, period, and conditional application. Ability System Component (ASC) management — grant/revoke abilities, apply/remove effects, query active abilities and effects. Gameplay Tag utilities. Gameplay Cue management — create, trigger, inspect cues for audio/visual feedback. Target data generation and targeting tasks. Input binding for ability activation. Runtime inspection and debugging tools. Scaffolding actions that generate complete GAS setups from templates. Accessibility-focused infinite-duration GEs for reduced difficulty modes.

**AI (221 actions)** — The most comprehensive AI tooling available through any MCP server. Full lifecycle management for Behavior Trees, Blackboards, State Trees, Environment Query System (EQS), Smart Objects, AI Controllers, AI Perception, Navigation, and runtime debugging. Crown jewel actions: `build_behavior_tree_from_spec` and `build_state_tree_from_spec` — hand the AI a JSON description of your desired AI behavior and it builds the entire asset programmatically. Phase J shipped BT crash hardening (F1) and BT graph + perception inspection helpers (F8). Create BT nodes (tasks, decorators, services), wire them into trees, configure blackboard keys, set up EQS queries with generators and tests, define Smart Object slots with behavior configs, configure perception senses (sight, hearing, damage, touch), manage navigation filters and query filters, inspect and debug AI at runtime during PIE. Scaffolding actions generate complete AI setups from templates — patrol AI, combat AI, companion AI, and more. 221 actions across 15 categories. Conditional on State Tree and Smart Objects plugins (both ship with UE) — gated via `WITH_STATETREE` and `WITH_SMARTOBJECTS` (Phase J F22 retrofit). Optional Mass Entity and Zone Graph integration for large-scale AI.

**Logic Driver (66 actions)** — Full integration with Logic Driver Pro, a marketplace state machine plugin. State machine CRUD — create, inspect, compile, delete. Graph read/write — add states, transitions, configure properties, set transition rules. Node configuration for state nodes, conduit nodes, and transition events. Runtime/PIE control — start, stop, query active states, trigger transitions. One-shot `build_sm_from_spec` builds complete state machines from a JSON specification. JSON spec import/export for templating and version control. Scaffolding actions generate common patterns (door controller, health system, AI patrol, dialogue system, elevator, puzzle, inventory). Component management — add/configure Logic Driver components on actors. Text graph visualization for debugging. Discovery actions list available node classes and templates. Reflection-only integration (no direct C++ API linkage) — works with any Logic Driver Pro version. Conditional on `#if WITH_LOGICDRIVER` — auto-detected at build time.

**ComboGraph (13 actions)** — Integration with the ComboGraph marketplace plugin for visual combo tree editing. Graph CRUD — create, inspect, validate combo graphs. Node and edge management — add combo nodes with montage animations, wire them with edges, configure effects and cues. GAS cross-integration — scaffold combo abilities that bridge ComboGraph with Gameplay Ability System. Reflection-only integration, conditional on `#if WITH_COMBOGRAPH`.

**Audio (86 actions)** — Editor-time audio asset authoring across the full UE audio pipeline. 82 audio-namespace actions plus 4 perception-binding actions (`bind_sound_to_perception` and friends, Phase J integration). Full CRUD on the 5 configurable audio asset types — SoundAttenuation, SoundClass, SoundMix, SoundConcurrency, SoundSubmix. Sound Cue graph construction — add nodes (22 types), wire them, set properties via reflection. MetaSound Builder API integration for programmatic MetaSound authoring — nodes, pins, graph inputs/outputs, interfaces, variables. Crown jewels: `build_sound_cue_from_spec` and `build_metasound_from_spec` — declarative JSON-to-graph in a single call. Batch operations for class/attenuation/submix/concurrency/compression/looping/virtualization across dozens of assets at once. Audio health checks — find unused sounds, missing attenuation, unassigned classes. Built-in `create_test_wave` (Phase J F18) generates a sine SoundWave on demand for diagnostic work. Phase J F11 added a hardened audio asset validator. Five template Sound Cues (random, layered, looping ambient, distance crossfade, switch) and four template MetaSounds (oneshot SFX, looping ambient, synth tone, interactive). SoundWave inspection is read-only; reflection-based property edits still work for batch sound wave tuning. MetaSound features gated on `#if WITH_METASOUND` — graceful degradation when absent.

---

## Features

- **Blueprint (89 actions)** — Full CRUD, node graph manipulation, JSON-to-Blueprint building, auto-layout (Sugiyama), CDO property access, data tables, structs, enums, template system, Blueprint comparison. Works as a complete Blueprint co-pilot with any MCP client
- **Material authoring (63 actions)** — Programmatic PBR graph building, custom HLSL, material functions, texture import, batch operations, preview rendering, compilation stats, tiling quality analysis, texture preview
- **Animation (118 actions)** — Sequences, montages, blend spaces, Animation Blueprint graph writing (add states, transitions, rules, wire nodes), PoseSearch, Control Rig, Physics Assets, IK Rigs, Retargeters, skeleton management
- **Niagara VFX (109 actions)** — System/emitter lifecycle, dynamic inputs, event handlers, sim stages, Parameter Collections, Effect Types, scalability settings, renderer presets, data interfaces, system diffing, batch execute
- **Mesh (240 actions)** — 22 capability tiers: mesh inspection, scene manipulation, spatial queries, blockout-to-production, GeometryScript ops, horror spatial analysis (WIP), accessibility validation (A-F grading), lighting (WIP), audio/acoustics (WIP), performance budgeting (WIP), decals, level design, tech art pipeline, context-aware props, procedural geometry (furniture, horror props, structures, mazes, terrain), genre presets, encounter design, accessibility reports. +45 town gen actions (work-in-progress, disabled by default)
- **AI (221 actions)** — Behavior Trees, Blackboards, State Trees, EQS, Smart Objects, AI Controllers, Perception, Navigation, runtime debugging, scaffolding. Crown jewels: `build_behavior_tree_from_spec` and `build_state_tree_from_spec`. Gated on `WITH_STATETREE` + `WITH_SMARTOBJECTS` (Phase J F22)
- **GAS (135 actions)** — Full Gameplay Ability System: abilities, AttributeSets (C++ + `ULeviathanVitalsSet` template; Blueprint sets via optional GBA), Gameplay Effects, ASC management, tags, cues, targeting, input binding, runtime inspection, scaffolding templates, accessibility-focused infinite-duration GEs. 4 attribute-binding actions surface in the `ui` namespace as aliases
- **Logic Driver (66 actions)** — Logic Driver Pro state machines: SM CRUD, graph read/write, node config, runtime/PIE, `build_sm_from_spec`, JSON spec, scaffolding (door, health, AI patrol, dialogue, elevator, puzzle, inventory), component management
- **ComboGraph (13 actions)** — ComboGraph combo trees: graph CRUD, nodes, edges, effects, cues, GAS cross-integration, ability scaffolding
- **Audio (86 actions)** — Sound asset CRUD (SoundAttenuation, SoundClass, SoundMix, SoundConcurrency, SoundSubmix), Sound Cue graph building, MetaSound Builder API (conditional on `WITH_METASOUND`), batch ops, audio health checks, `build_sound_cue_from_spec`, `build_metasound_from_spec`, `apply_audio_template`, template cues + MetaSounds, sine-wave test asset factory, AI perception sound binding
- **UI (96 actions)** — 42 UMG baseline + 50 CommonUI (gated on `WITH_COMMONUI`, shipped v0.14.0) + 4 GAS attribute-binding aliases. Widget Blueprint CRUD, pre-built templates (HUDs, menus, settings, inventory, save slots), styling, animation, game system scaffolding (save/load, audio, input remapping), accessibility audit, colorblind modes, text scaling
- **Editor control (22 actions)** — UBT builds, Live Coding, error diagnosis, log search, scene capture, GIF capture, texture import, crash context, blank map factory, module status (Phase J F8)
- **Config intelligence (6 actions)** — Full INI resolution chain, explain, diff, search across all config files
- **Project search (7 actions)** — SQLite FTS5 across all indexed assets including marketplace/Fab content, reference tracing, 14 deep indexers
- **Engine source (11 actions)** — Native C++ indexer over 1M+ symbols, call graphs, class hierarchy, offline — no Python required. Auto-reindex on hot-reload (Phase J F17)
- **Standalone C++ tools** — `monolith_proxy.exe` (MCP proxy) and `monolith_query.exe` (offline DB queries) — zero Python, zero UE dependency, instant startup
- **Auto-updater** — Off by default as of v0.14.6. When enabled, checks GitHub Releases on editor startup, verifies SHA256 against the release notes marker, downloads and stages updates, auto-swaps on exit
- **MCP auto-reconnect proxy** — stdio-to-HTTP proxy keeps Claude Code sessions alive across editor restarts. Available as native exe (zero dependencies) or Python script (fallback)
- **Optional module system** — Extend Monolith with new MCP namespaces for third-party plugins (GeometryScripting, BlueprintAssist, GBA, ComboGraph, Logic Driver, MetaSound) without breaking the build for users who don't own them. The sibling-plugin pattern lets you ship your own integration plugin alongside Monolith — see `Docs/SIBLING_PLUGIN_GUIDE.md`
- **Claude Code skills** — 16 domain-specific workflow guides bundled with the plugin
- **Pure C++** — Direct UE API access, embedded HTTP MCP server, zero external dependencies

---

## Installation

### Prerequisites

- **Unreal Engine 5.7+** — Launcher or source build ([unrealengine.com](https://unrealengine.com))

> **Platform:** Windows, macOS, Linux.

- **Claude Code, Cursor, or another MCP client** — Any tool that supports the Model Context Protocol
- **(Optional) Python 3.8+** — Only needed to index your own project's C++ source via `Scripts/index_project.py`. Engine source indexing is built-in and needs no Python. The MCP proxy and offline query tools are now standalone C++ executables — Python is no longer required for any core functionality.

### Step 1: Drop Monolith into your project

Every Unreal project has a `Plugins/` folder. If yours doesn't have one yet, create it next to your `.uproject` file:

```
YourProject/
  YourProject.uproject
  Content/
  Source/
  Plugins/          <-- here
    Monolith/
```

**Option A: Git clone (recommended)**

```bash
cd YourProject/Plugins
git clone https://github.com/tumourlove/monolith.git Monolith
```

**Option B: Download ZIP**

Grab the latest release from [GitHub Releases](https://github.com/tumourlove/monolith/releases), extract it, and drop the folder at `YourProject/Plugins/Monolith/`. The release ZIP includes precompiled DLLs — Blueprint-only projects can open the editor immediately without rebuilding. C++ projects should rebuild first.

**Option C: Let your AI do it**

If you're already in a Claude Code, Cursor, or Cline session, just say:

> "Install the Monolith plugin from https://github.com/tumourlove/monolith into my project's Plugins folder"

It'll clone the repo, create `.mcp.json`, and configure everything. After it's done, **restart your AI session** so it picks up the new `.mcp.json`, then skip to [Step 3](#step-3-open-the-editor).

### Step 2: Create `.mcp.json`

This file tells your AI client where to find Monolith's MCP server. Create it in your **project root** — same directory as your `.uproject`:

```
YourProject/
  YourProject.uproject
  .mcp.json          <-- create this
  Plugins/
    Monolith/
```

**For Claude Code (recommended: auto-reconnect proxy)**

Monolith ships with a stdio-to-HTTP proxy that keeps your MCP session alive when the Unreal Editor restarts. No manual reconnection needed. The proxy is available as a **standalone C++ executable** (zero dependencies) or a Python script (fallback).

**Option A: Native C++ proxy — Windows only (no Python required)**

```json
{
  "mcpServers": {
    "monolith": {
      "command": "Plugins/Monolith/Binaries/monolith_proxy.exe",
      "args": []
    }
  }
}
```

**Option B: Python proxy (fallback — works on all platforms)**

Requires Python 3.8+ ([python.org](https://python.org)).

Windows:
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

macOS / Linux:
```json
{
  "mcpServers": {
    "monolith": {
      "command": "Plugins/Monolith/Scripts/monolith_proxy.sh",
      "args": []
    }
  }
}
```

> **No proxy?** Use direct HTTP instead — you'll just need to restart Claude Code each time the editor restarts:
> ```json
> {"mcpServers": {"monolith": {"type": "http", "url": "http://localhost:9316/mcp"}}}
> ```

**For Cursor / Cline:**

```json
{
  "mcpServers": {
    "monolith": {
      "type": "streamableHttp",
      "url": "http://localhost:9316/mcp"
    }
  }
}
```

> Cursor and Cline handle server restarts natively — the proxy isn't needed.

### Step 3: Open the editor

Open your `.uproject` as normal. On first launch:

1. Monolith auto-indexes your project (30-60 seconds depending on size — go get a coffee)
2. Open the **Output Log** (Window > Developer Tools > Output Log)
3. Filter for `LogMonolith` — you'll see the server start up and the index complete

When you see `Monolith MCP server listening on port 9316`, you're in business.

### Step 4: Connect your AI

1. Open **Claude Code** (or your MCP client) from your project directory — the one with `.mcp.json`
2. Claude Code auto-detects `.mcp.json` on startup and connects to Monolith
3. Sanity check: ask *"What Monolith tools do you have?"*

You should get back a list of namespace tools (`blueprint_query`, `material_query`, etc.). If you do, everything's working.

### Step 5: Add project instructions for your AI

Different AI coding assistants use different conventions for project-instructions files (`CLAUDE.md` for Claude Code, `AGENTS.md` for Codex, `.cursorrules` for Cursor, `.github/copilot-instructions.md` for Copilot, plus a long tail). Those conventions evolve faster than a static template can keep up — so rather than ship a template that grows stale, the recommended workflow is to ask your AI directly.

Practical prompt to feed your assistant once Monolith is installed and running:

> *"I've installed the Monolith Unreal plugin. It exposes ~1239 actions across 16 namespaces (`blueprint`, `material`, `animation`, `niagara`, `mesh`, `ui`, `gas`, `ai`, `audio`, etc.) over an in-process MCP HTTP listener at `http://localhost:9316/mcp`. What's the best-practice format for a project-instructions file for [your assistant — e.g. `CLAUDE.md`, `AGENTS.md`, `.cursorrules`]? It should help with action discovery via `monolith_discover()`, asset-path conventions like `/Game/Path/Asset`, and verifying UE 5.7 APIs via `source_query` before writing code."*

Whatever your AI generates, drop it at the appropriate path for your toolchain. The action counts and workflow notes from this README's earlier sections are usable input.

### Step 6: (Optional) Index your project's C++ source

Engine source indexing is automatic — `source_query` works immediately with no setup.

If you also want your AI to search your **own project's C++ source** (find callers, callees, and class hierarchies across your own code):

1. Install **Python 3.10+**
2. Run `python Plugins/Monolith/Scripts/index_project.py` from your project root
3. Your project source gets indexed into `EngineSource.db` alongside engine symbols
4. To re-run the indexer without leaving the editor: `source_query("trigger_project_reindex")`

### Verify it's alive

With the editor running, hit this from any terminal:

```bash
curl -X POST http://localhost:9316/mcp \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":1,"method":"tools/list"}'
```

You'll get a JSON response listing all Monolith tools. If you get "connection refused", the editor isn't running or something went sideways — check the Output Log for `LogMonolith` errors.

### (Optional) Install Claude Code skills

Monolith ships domain-specific workflow skills for Claude Code:

```bash
cp -r Plugins/Monolith/Skills/* ~/.claude/skills/
```

---

## Architecture

```
Monolith.uplugin
  MonolithCore          — HTTP server, tool registry, discovery, auto-updater (4 actions)
  MonolithBlueprint     — Blueprint read/write, variable/component/graph CRUD, node operations, compile, CDO reader (89 actions)
  MonolithMaterial      — Material inspection + graph editing + CRUD + material functions + tiling quality (63 actions)
  MonolithAnimation     — Animation sequences, montages, ABPs, PoseSearch, IKRig, Control Rig (118 actions)
  MonolithNiagara       — Niagara particle systems, dynamic inputs, event handlers, sim stages, NPC, scalability (109 actions)
  MonolithMesh          — Mesh inspection, scene manipulation, spatial queries, blockout, procedural geometry, horror/accessibility (240 actions: 195 core + 45 experimental town gen)
  MonolithAI            — Behavior Trees, Blackboards, State Trees, EQS, Smart Objects, Controllers, Perception, Navigation (221 actions)
  MonolithEditor        — Build triggers, log capture, compile output, crash context, GIF capture, blank map factory, module status (22 actions)
  MonolithConfig        — Config/INI resolution and search (6 actions)
  MonolithIndex         — SQLite FTS5 deep project indexer, marketplace content, 14 asset indexers (7 actions)
  MonolithSource        — Native C++ engine source indexer, call graphs, class hierarchy, hot-reload-aware reindex (11 actions)
  MonolithUI            — UI widget Blueprint CRUD, templates, styling, animation, CommonUI (96 actions: 42 UMG + 50 CommonUI + 4 GAS aliases)
  MonolithGAS           — Gameplay Ability System: abilities, effects, attributes, ASC, tags, cues, targeting, ULeviathanVitalsSet template (135 actions)
  MonolithLogicDriver   — Logic Driver Pro state machines: SM CRUD, graph read/write, JSON spec, scaffolding (66 actions)
  MonolithComboGraph    — ComboGraph combo trees: graph CRUD, nodes, edges, effects, cues (13 actions)
  MonolithAudio         — Audio asset CRUD, Sound Cue + MetaSound graph building, batch ops, templates, AI perception binding, sine test wave (86 actions)
  MonolithAudioRuntime  — Runtime sub-module supplying perception classes for audio.bind_sound_to_perception (0 MCP actions)
  MonolithBABridge      — Blueprint Assist integration bridge (0 MCP actions — IModularFeatures only)
  MonolithLevelSequence — Level Sequence introspection: full per-LS binding inventory (legacy Possessable/Spawnable + UE 5.7 UMovieSceneCustomBinding family), Director Blueprint functions/variables, event-track bindings, cross-sequence reverse lookup (8 actions)

Standalone Tools (in Binaries/)
  monolith_proxy.exe    — MCP stdio-to-HTTP proxy (zero UE dependency, WinHTTP + nlohmann/json)
  monolith_query.exe    — Offline DB query tool (zero UE dependency, sqlite3 amalgamation)
```

**1293 actions total across 17 namespaces (1248 active by default; 45 town-gen experimental disabled), exposed through 20 MCP tools. Distinct handlers: 1289 — the `ui` namespace double-counts 4 aliased GAS attribute-binding actions.** Live editors with sibling plugins loaded report higher counts (e.g. with all 4 sibling plugins loaded: 1469 actions across 21 namespaces).

### Tool Reference

| Namespace | Tool | Actions | Description |
|-----------|------|---------|-------------|
| `monolith` | `monolith_discover` | — | List available actions per namespace |
| `monolith` | `monolith_status` | — | Server health, version, index status |
| `monolith` | `monolith_reindex` | — | Trigger full project re-index |
| `monolith` | `monolith_update` | — | Check or install updates |
| `blueprint` | `blueprint_query` | 89 | Full Blueprint CRUD — read/write graphs, variables, components, functions, nodes, compile, CDO properties, auto-layout |
| `material` | `material_query` | 63 | Inspection, editing, graph building, material functions, previews, validation, tiling quality, texture preview, CRUD |
| `animation` | `animation_query` | 118 | Montages, blend spaces, ABPs, skeletons, bone tracks, PoseSearch, IKRig, Control Rig, ABP/ControlRig writes |
| `niagara` | `niagara_query` | 109 | Systems, emitters, modules, parameters, renderers, HLSL, dynamic inputs, event handlers, sim stages, NPC, effect types, scalability |
| `mesh` | `mesh_query` | 240 (195 + 45) | Mesh inspection, scene manipulation, spatial queries, blockout, GeometryScript, horror analysis, lighting, audio, performance, procedural geometry, encounter design. Town gen 45 actions registered only when `bEnableProceduralTownGen=true` |
| `ai` | `ai_query` | 221 | BT, BB, State Trees, EQS, Smart Objects, Controllers, Perception, Navigation, runtime debugging, scaffolding. Conditional on `WITH_STATETREE` + `WITH_SMARTOBJECTS` |
| `gas` | `gas_query` | 135 | Gameplay Ability System — abilities, effects, attributes (incl. `ULeviathanVitalsSet`), ASC, tags, cues, targeting, input, inspect, scaffold. Conditional on `WITH_GBA` for Blueprint AttributeSets |
| `logicdriver` | `logicdriver_query` | 66 | Logic Driver Pro state machines — SM CRUD, graph read/write, JSON spec, scaffolding, components. Conditional on `WITH_LOGICDRIVER` |
| `combograph` | `combograph_query` | 13 | ComboGraph combo trees — graph CRUD, nodes, edges, effects, cues, ability scaffolding. Conditional on `WITH_COMBOGRAPH` |
| `audio` | `audio_query` | 86 | Sound asset CRUD, Sound Cue + MetaSound graph building, batch ops, audio health checks, templates, sine test wave, AI perception binding. MetaSound features conditional on `WITH_METASOUND` |
| `ui` | `ui_query` | 96 (42 + 50 + 4) | UMG widget CRUD, templates, styling, animation, settings scaffolding, accessibility. CommonUI 50 actions conditional on `WITH_COMMONUI`. 4 GAS attribute-binding aliases also live here |
| `editor` | `editor_query` | 22 | Build triggers, error logs, compile output, crash context, scene capture, GIF capture, texture import, blank map factory, module status |
| `config` | `config_query` | 6 | INI resolution, explain, diff, search |
| `project` | `project_query` | 7 | Deep project search — FTS5 across all indexed assets including marketplace plugins |
| `source` | `source_query` | 11 | Native C++ engine source lookup, call graphs, class hierarchy, project reindex, hot-reload-aware refresh |
| `level_sequence` | `level_sequence_query` | 8 | Level Sequence inspection: full binding inventory (one row per Guid×BindingIndex with kind classification — legacy Possessable/Spawnable + UE 5.7 UMovieSceneSpawnableActorBinding / Replaceable / Custom), Director Blueprint own functions (user / custom_event / sequencer_endpoint) and variables, event-track bindings with Director-function resolution, cross-sequence reverse lookup of function callers |

---

## Standalone Tools

Monolith ships two standalone C++ executables that work without the Unreal Editor, without Python, and without any external dependencies. Both are in `Binaries/` and included in every release.

### monolith_proxy.exe — MCP Proxy

A stdio-to-HTTP proxy that keeps Claude Code MCP sessions alive across editor restarts. Full feature parity with the Python `monolith_proxy.py`:

- JSON-RPC message handling with editor query splitting
- Background health poll with `notifications/tools/list_changed`
- Tool deduplication and action allowlist/denylist
- Built with WinHTTP + nlohmann/json, zero UE dependency

**Usage:** Set as the MCP command in `.mcp.json`:

```json
{
  "mcpServers": {
    "monolith": {
      "command": "Plugins/Monolith/Binaries/monolith_proxy.exe",
      "args": []
    }
  }
}
```

**Migrating from Python proxy:** Replace `{"command": "python", "args": ["Plugins/Monolith/Scripts/monolith_proxy.py"]}` with the config above. The Python scripts remain as deprecated fallbacks.

**Source:** `Tools/MonolithProxy/monolith_proxy.cpp` (775 lines)

### monolith_query.exe — Offline Query Tool

A standalone database query tool that replaces both `monolith_offline.py` and the old `MonolithQueryCommandlet`. Instant startup (no 6+ second UE engine load), queries `EngineSource.db` and `ProjectIndex.db` directly.

**14 actions:** 9 source (search_source, read_source, find_callers, find_callees, find_references, get_class_hierarchy, get_module_info, get_symbol_context, read_file) + 5 project (search, find_by_type, find_references, get_stats, get_asset_details)

**Usage:**

```bash
# Engine source queries
Plugins/Monolith/Binaries/monolith_query.exe source search_source FCharacterMovementComponent --limit=5
Plugins/Monolith/Binaries/monolith_query.exe source read_source ACharacter --max-lines=50
Plugins/Monolith/Binaries/monolith_query.exe source get_class_hierarchy ACharacter --depth=3 --direction=down

# Project asset queries (JSON output)
Plugins/Monolith/Binaries/monolith_query.exe project search damage --limit=10
Plugins/Monolith/Binaries/monolith_query.exe project find_by_type Blueprint --limit=20
Plugins/Monolith/Binaries/monolith_query.exe project get_stats
```

Auto-detects database paths relative to exe location. No configuration needed.

**Source:** `Tools/MonolithQuery/monolith_query.cpp` (1080 lines)

### Building from Source

Both tools use standard C/C++ with no UE dependency:

```bash
# Proxy (requires WinHTTP, nlohmann/json header-only)
cl /O2 /EHsc /std:c++17 Tools/MonolithProxy/monolith_proxy.cpp /Fe:Binaries/monolith_proxy.exe winhttp.lib

# Query tool (sqlite3 amalgamation bundled)
cl /O2 /EHsc /std:c++17 Tools/MonolithQuery/monolith_query.cpp /Fe:Binaries/monolith_query.exe
```

Precompiled binaries are included in every release — building from source is only needed if you want to modify the tools.

---

## Auto-Updater

Monolith can check for new versions on editor startup so you don't have to babysit GitHub. **Off by default** as of v0.14.6 — opt in if you want it.

1. **Opt in** — Tick **Auto Update Enabled** in Editor Preferences > Plugins > Monolith
2. **On editor startup** — Checks GitHub Releases for a newer version
3. **Downloads and verifies** — If an update is found, it downloads the zip and verifies the SHA256 hash against the `Monolith-SHA256:` marker in the release notes. Mismatch aborts the install.
4. **Auto-swaps on exit** — The plugin is replaced when you close the editor (after a Y/N prompt in the swap script)
5. **Manual check** — `monolith_update` tool to check anytime
6. **Releases without a SHA256 marker** — log a warning and proceed without integrity check (legacy releases do not have markers; future-only releases will hard-fail without one)

See [the wiki Auto-Updater page](https://github.com/tumourlove/monolith/wiki/Auto-Updater) for full details.

---

## Network Exposure

Monolith starts a local HTTP server on port 9316 to receive MCP traffic from AI assistants. UE's `FHttpServerModule` does **not** expose a bind-address parameter, so the listener is reachable on all network interfaces, not just `127.0.0.1`. CORS is restricted to localhost origins (`http(s)://localhost`, `127.0.0.1`, `[::1]`), which blocks browser-based cross-origin reads, but does **not** block direct HTTP requests from other devices on the same LAN.

If you work on an untrusted network, choose one of:

- **Add a Windows Firewall rule** blocking inbound TCP on port 9316 from non-loopback addresses, OR
- **Disable the server** by unticking **MCP Server Enabled** in Editor Preferences > Plugins > Monolith and restarting the editor.

See [SECURITY.md](SECURITY.md) for the full threat model and disclosure policy.

---

## Troubleshooting

| Problem | Solution |
|---------|----------|
| **Plugin doesn't appear in editor** | Verify the folder is at `YourProject/Plugins/Monolith/` and contains `Monolith.uplugin`. Check you're on UE 5.7+. |
| **MCP connection refused** | Make sure the editor is open and running. Check Output Log for port conflicts. Verify `.mcp.json` is in your project root. |
| **Index shows 0 assets** | Run `monolith_reindex` or restart the editor. Check Output Log for indexing errors. |
| **Source tools return empty results** | Run `monolith_reindex()` and wait for completion, then retry. Engine source indexing is built-in — if results are still empty, check the Output Log for `LogMonolith` errors. |
| **Claude can't find any tools** | Check `.mcp.json` transport type: Claude Code uses `"http"`, Cursor/Cline use `"streamableHttp"`. Restart your AI client after creating the file. |
| **Tools fail on first try** | Restart Claude Code to refresh the MCP connection. Known quirk with initial connection timing. |
| **Port 9316 already in use** | Change the port in Editor Preferences > Plugins > Monolith, then update `.mcp.json` to match. |
| **Proxy says "Python 3 not found"** | On Windows, switch to the C++ proxy (`monolith_proxy.exe`) — no Python needed. On macOS/Linux, install Python 3.8+ and ensure `python3` is on your PATH. |
| **monolith_query.exe returns no results** | The exe looks for databases relative to its own location. Make sure `Saved/Monolith/EngineSource.db` and `Saved/Monolith/ProjectIndex.db` exist (created on first editor launch). |
| **macOS: `monolith_proxy.sh` permission denied** | Make sure the script is executable: `chmod +x Plugins/Monolith/Scripts/monolith_proxy.sh`. |
| **macOS/Linux: native C++ proxy not available** | The prebuilt `monolith_proxy.exe` is Windows-only for now. Use the Python proxy (`monolith_proxy.sh`) — same protocol, same features. |

---

## Configuration

Settings live at **Editor Preferences > Plugins > Monolith**:

| Setting | Default | Description |
|---------|---------|-------------|
| MCP Server Port | `9316` | Port for the embedded HTTP server |
| Auto-Update | `Off` (as of v0.14.6) | Check GitHub Releases on editor startup. Opt-in. |
| Module Toggles | All enabled | Enable/disable individual domain modules |
| Database Path | Project-local | Override SQLite database storage location |
| Index Marketplace Plugins | `On` | Index content from installed marketplace/Fab plugins |
| Index Data Assets | `On` | Deep-index DataAsset subclasses (14 indexers) |
| Additional Content Paths | `[]` | Extra content paths to include in the project index |
| Enable Procedural Town Gen | `Off` | **Work-in-progress** — 45 additional mesh actions for procedural building/town generation. Very much a WIP; unless you're willing to dig in and help improve it, best left alone for now |
| Enable GAS | `On` | Gameplay Ability System integration (135 actions, requires GameplayAbilities plugin) |
| Enable Logic Driver | `On` | Logic Driver Pro state machine integration (66 actions, requires Logic Driver Pro marketplace plugin) |
| Enable ComboGraph | `On` | ComboGraph combo tree integration (13 actions, requires ComboGraph marketplace plugin) |
| Enable Blueprint Assist | `On` | Blueprint Assist integration for enhanced auto_layout (requires BA marketplace plugin) |

---

## Skills

Monolith bundles 16 Claude Code skills in `Skills/` — domain-specific workflow guides that give your AI the right mental model for each area:

| Skill | Description |
|-------|-------------|
| `unreal-blueprints` | Full Blueprint CRUD — read, create, edit variables/components/functions/nodes, compile |
| `unreal-materials` | PBR setup, graph building, validation |
| `unreal-animation` | Montages, ABP state machines, blend spaces |
| `unreal-niagara` | Particle system creation, HLSL modules, scalability |
| `unreal-audio` | Sound Cue + MetaSound graph building, audio asset CRUD, batch ops, templates |
| `unreal-mesh` | Mesh inspection, spatial queries, blockout, procedural geometry, horror/accessibility |
| `unreal-ui` | Widget Blueprint CRUD, templates, styling, accessibility |
| `unreal-gas` | Gameplay Ability System — abilities, effects, attributes, ASC, tags, cues |
| `unreal-logicdriver` | Logic Driver Pro state machines — SM CRUD, graph editing, JSON spec, scaffolding |
| `unreal-combograph` | ComboGraph combo trees — graph CRUD, nodes, edges, effects, ability scaffolding |
| `unreal-level-sequences` | Level Sequence inspection — full binding inventory (legacy + UE 5.7 custom bindings), Director Blueprint functions/variables, event-track bindings, cross-sequence reverse lookup |
| `unreal-debugging` | Build errors, log search, crash context |
| `unreal-performance` | Config auditing, shader stats, INI tuning |
| `unreal-project-search` | FTS5 search syntax, reference tracing |
| `unreal-cpp` | API lookup, include paths, Build.cs gotchas |
| `unreal-build` | Smart build decision-making, Live Coding vs full rebuild |

---

## Documentation

- [API_REFERENCE.md](Docs/API_REFERENCE.md) — Full action reference with parameters
- [SPEC_CORE.md](Docs/SPEC_CORE.md) — Technical specification and design decisions (per-module specs under [Docs/specs/](Docs/specs/))
- [CONTRIBUTING.md](CONTRIBUTING.md) — Dev setup, coding conventions, PR process
- [CHANGELOG.md](CHANGELOG.md) — Version history and release notes
- [Wiki](https://github.com/tumourlove/monolith/wiki) — Installation guides, test status, FAQ

---

## Contributing

Contributions are welcome. See [CONTRIBUTING.md](CONTRIBUTING.md) for dev environment setup, coding conventions, how to add new actions, and the PR process.

---

## License

[MIT](LICENSE) — See [ATTRIBUTION.md](ATTRIBUTION.md) for credits.
