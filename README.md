# Monolith

**One plugin. Every Unreal domain. Zero dependencies.**

[![UE 5.7+](https://img.shields.io/badge/Unreal-5.7%2B-blue)](https://unrealengine.com)
[![License: MIT](https://img.shields.io/badge/License-MIT-green.svg)](LICENSE)
[![MCP](https://img.shields.io/badge/Protocol-MCP-purple)](https://modelcontextprotocol.io)

---

## What is Monolith?

Monolith is an Unreal Engine editor plugin that gives your AI full read/write access to your project through the [Model Context Protocol (MCP)](https://modelcontextprotocol.io). Install one plugin, point your AI client at one endpoint, and it can work with Blueprints, Materials, Animation, Niagara, project configuration, and more.

It works with **Claude Code**, **Cursor**, or any MCP-compatible client. If your AI tool speaks MCP, it speaks Monolith.

> **Platform:** Windows only. Mac and Linux support is coming soon.

## Why Monolith?

Most MCP integrations register every action as a separate tool, which floods the AI's context window and buries the actually useful stuff. Monolith uses a **namespace dispatch pattern** instead: each domain exposes a single `{namespace}_query(action, params)` tool, and a central `monolith_discover()` call lists everything available. Small tool list (15 tools), massive capability (815 actions across 13 modules). The AI gets oriented fast and spends its context on your actual problem.

## What Can It Actually Do?

**Blueprint (86 actions)** — Full programmatic control of every Blueprint in your project. Create from any parent class, build entire node graphs from a JSON spec, add/remove/connect/disconnect nodes in bulk, manage variables, components, functions, macros, and event dispatchers. Implement interfaces, reparent hierarchies, edit construction scripts, read/write CDO properties on any Blueprint or DataAsset. The auto-layout engine uses a modified Sugiyama algorithm so AI-generated graphs actually look clean. Compare two Blueprints side-by-side, scaffold from templates, manage data tables, user defined structs and enums. Hand the AI a description and it builds the whole thing — or point it at an existing Blueprint and it'll surgically rewire what you need.

**Material (57 actions)** — Create materials, material instances, and material functions from scratch. Build entire PBR graphs programmatically — add expressions, connect pins, auto-layout, recompile. Drop in custom HLSL nodes. Import textures from disk and wire them directly into material slots. Batch-set properties across dozens of instances at once. Render material previews and thumbnails without leaving the AI session. Full material function support: create, build internal graphs, export/import between projects. Get compilation stats, validate for errors, inspect shader complexity. Covers the full material workflow from creation to validation.

**Animation (115 actions)** — The entire animation pipeline, end to end. Create and edit sequences with bone tracks, curves, notifies, and sync markers. Build montages with sections, slots, blending config, and anim segments. Set up 1D/2D Blend Spaces and Aim Offsets with sample points. **Animation Blueprint graph writing** — add states to state machines, create transitions, set transition rules, add and connect anim graph nodes, set state animations. AI can build ABP locomotion setups programmatically, not just read them. PoseSearch integration: create schemas and databases, configure channels, rebuild the search index. Control Rig graph manipulation with node wiring and variable management. Physics Asset editing for body and constraint properties. IK Rig and Retargeter support — chain mapping, solver configuration, the works. Skeleton management with sockets, virtual bones, and curves. 115 actions covering the full animation pipeline.

**Niagara (96 actions)** — Full system and emitter lifecycle — create, duplicate, configure, compile. Module CRUD with override-preserving reorder so you don't blow away artist tweaks. Complete dynamic input lifecycle: attach inputs, inspect the tree, read values, remove them. Event handler and simulation stage CRUD. Niagara Parameter Collections with full param management. Effect Type creation with scalability and culling configuration. Renderer helpers for every type — mesh assignment, ribbon presets (trail, beam, lightning, tube), SubUV and flipbook setup. Data interface configuration handles JSON arrays and structs natively. Diff two systems to see exactly what changed. Clone overrides between modules, discover parameter bindings, inspect module outputs. Batch execute with read-only optimization so queries don't trigger unnecessary recompiles. Full `export_system_spec` dumps everything — event handlers, sim stages, static switches, dynamic inputs. Covers the full Niagara workflow from system creation to final polish.

**UI (42 actions)** — Widget Blueprint CRUD with full widget tree manipulation. Pre-built templates for common game UI: HUD elements, menus, settings panels, confirmation dialogs, loading screens, inventory grids, save slot lists, notification toasts. Style everything — brushes, fonts, color schemes, batch style operations. Create keyframed widget animations. Full game scaffolding: settings systems, save/load, audio config, input remapping, accessibility features. Run accessibility audits, set up colorblind modes, configure text scaling. Covers the full UI workflow from widget creation to accessibility.

**Editor (19 actions)** — Trigger full UBT builds or Live Coding compiles, read build errors and compiler output, search and tail editor logs, get crash context after failures. Capture preview screenshots of any asset — materials, Niagara systems, meshes. Import textures, stitch flipbooks, delete assets. The AI can compile your code, read the errors, fix the C++, recompile, and verify the fix — all without you touching the editor.

**Config (6 actions)** — Full INI resolution chain awareness: Base, Platform, Project, User. Ask what any setting does, where it's overridden, what the effective value is, and how it differs from the engine default. Search across all config files at once. Perfect for performance tuning sessions where you want the AI to just sort out your INIs.

**Source (11 actions)** — Search over 1M+ Unreal Engine C++ symbols instantly. Read function implementations, get full class hierarchies, trace call graphs (callers and callees), verify include paths — all against a local index, fully offline. The native C++ indexer runs automatically on editor startup. No Python, no setup. Optionally index your project's own C++ source for the same coverage on your code. The AI never has to guess at a function signature again.

**Project (7 actions)** — SQLite FTS5 full-text search across every indexed asset in your project. Find assets by name, type, path, or content. Trace references between assets. Search gameplay tags. Get detailed asset metadata. The index updates live as assets change and covers marketplace/Fab plugin content too — 15 deep indexers registered including DataAsset subclasses.

**Mesh (242 actions)** — The biggest module by far. 197 core actions across 22 capability tiers, plus 45 procedural town generation actions (work-in-progress -- disabled by default, and unless you're willing to dig in and help improve it, best left alone for now). Mesh inspection and comparison. Full actor CRUD with scene manipulation. Physics-based spatial queries (raycasts, sweeps, overlaps) that work in-editor without PIE. Level blockout workflow with auto-matching and atomic replacement. GeometryScript mesh operations (boolean, simplify, remesh, LOD gen, UV projection). Horror spatial analysis — sightlines, hiding spots, ambush points, zone tension, pacing curves (WIP). Accessibility validation with A-F grading. Lighting analysis (WIP), audio/acoustics with Sabine RT60 and stealth maps (WIP), performance budgeting (WIP). Decal placement with storytelling presets. Level design tools for lights, volumes, sublevels, prefabs, HISM instancing. Tech art pipeline for mesh import, LOD gen, texel density, collision authoring. Context-aware prop scatter on any surface. Procedural geometry — parametric furniture (15 types), horror props (7 types), architectural structures, mazes, pipes, terrain. Genre preset system for any game type. Encounter design with patrol routes, safe room evaluation, and scare sequence generation. Full accessibility reporting.

**GAS (130 actions)** — Complete Gameplay Ability System integration. Create and manage Gameplay Abilities with activation policies, cooldowns, costs, and tags. Full AttributeSet CRUD — both C++ and Blueprint-based (via optional GBA plugin). Gameplay Effect authoring with modifiers, duration policies, stacking, period, and conditional application. Ability System Component (ASC) management — grant/revoke abilities, apply/remove effects, query active abilities and effects. Gameplay Tag utilities. Gameplay Cue management — create, trigger, inspect cues for audio/visual feedback. Target data generation and targeting tasks. Input binding for ability activation. Runtime inspection and debugging tools. Scaffolding actions that generate complete GAS setups from templates. Accessibility-focused infinite-duration GEs for reduced difficulty modes.

---

## Features

- **Blueprint (86 actions)** — Full CRUD, node graph manipulation, JSON-to-Blueprint building, auto-layout (Sugiyama), CDO property access, data tables, structs, enums, template system, Blueprint comparison. Works as a complete Blueprint co-pilot with any MCP client
- **Material authoring (57 actions)** — Programmatic PBR graph building, custom HLSL, material functions, texture import, batch operations, preview rendering, compilation stats
- **Animation (115 actions)** — Sequences, montages, blend spaces, Animation Blueprint graph writing (add states, transitions, rules, wire nodes), PoseSearch, Control Rig, Physics Assets, IK Rigs, Retargeters, skeleton management
- **Niagara VFX (96 actions)** — System/emitter lifecycle, dynamic inputs, event handlers, sim stages, Parameter Collections, Effect Types, renderer presets, data interfaces, system diffing, batch execute
- **Mesh (242 actions)** — 22 capability tiers: mesh inspection, scene manipulation, spatial queries, blockout-to-production, GeometryScript ops, horror spatial analysis (WIP), accessibility validation (A-F grading), lighting (WIP), audio/acoustics (WIP), performance budgeting (WIP), decals, level design, tech art pipeline, context-aware props, procedural geometry (furniture, horror props, structures, mazes, terrain), genre presets, encounter design, accessibility reports. +45 town gen actions (work-in-progress, disabled by default)
- **GAS (130 actions)** — Full Gameplay Ability System: abilities, AttributeSets (C++ and Blueprint via optional GBA), Gameplay Effects, ASC management, tags, cues, targeting, input binding, runtime inspection, scaffolding templates, accessibility-focused infinite-duration GEs
- **UI (42 actions)** — Widget Blueprint CRUD, pre-built templates (HUDs, menus, settings, inventory, save slots), styling, animation, game system scaffolding (save/load, audio, input remapping), accessibility audit, colorblind modes, text scaling
- **Editor control (19 actions)** — UBT builds, Live Coding, error diagnosis, log search, scene capture, texture import, crash context
- **Config intelligence (6 actions)** — Full INI resolution chain, explain, diff, search across all config files
- **Project search (7 actions)** — SQLite FTS5 across all indexed assets including marketplace/Fab content, reference tracing, 15 deep indexers
- **Engine source (11 actions)** — Native C++ indexer over 1M+ symbols, call graphs, class hierarchy, offline — no Python required
- **Auto-updater** — Checks GitHub Releases on editor startup, downloads and stages updates, auto-swaps on exit
- **MCP auto-reconnect proxy** — stdio-to-HTTP proxy keeps Claude Code sessions alive across editor restarts, zero manual intervention
- **Optional module system** — Extend Monolith with new MCP namespaces for third-party plugins (GeometryScripting, BlueprintAssist, GBA) without breaking the build for users who don't own them
- **Claude Code skills** — 12 domain-specific workflow guides bundled with the plugin
- **Pure C++** — Direct UE API access, embedded Streamable HTTP server, zero external dependencies

---

## Installation

### Prerequisites

- **Unreal Engine 5.7+** — Launcher or source build ([unrealengine.com](https://unrealengine.com))

> **Platform:** Windows only. Mac and Linux support is coming soon.

- **Claude Code, Cursor, or another MCP client** — Any tool that supports the Model Context Protocol
- **(Optional) Python 3.8+** — Recommended for Claude Code users (enables the auto-reconnect proxy that survives editor restarts). Also needed to index your own project's C++ source. Engine source indexing is built-in and needs no Python.

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

Monolith ships with a stdio-to-HTTP proxy that keeps your MCP session alive when the Unreal Editor restarts. No manual reconnection needed. Requires Python 3.8+ ([python.org](https://python.org)).

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

Or copy the template: `cp Plugins/Monolith/Templates/.mcp.json.proxy.example .mcp.json`

> **No Python?** Use direct HTTP instead — you'll just need to restart Claude Code each time the editor restarts:
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

### Step 5: (Optional) Index your project's C++ source

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
  MonolithBlueprint     — Blueprint read/write, variable/component/graph CRUD, node operations, compile, CDO reader (86 actions)
  MonolithMaterial      — Material inspection + graph editing + CRUD + material functions (57 actions)
  MonolithAnimation     — Animation sequences, montages, ABPs, PoseSearch, IKRig, Control Rig (115 actions)
  MonolithNiagara       — Niagara particle systems, dynamic inputs, event handlers, sim stages, NPC (96 actions)
  MonolithMesh          — Mesh inspection, scene manipulation, spatial queries, blockout, procedural geometry, horror/accessibility (242 actions)
  MonolithEditor        — Build triggers, log capture, compile output, crash context (19 actions)
  MonolithConfig        — Config/INI resolution and search (6 actions)
  MonolithIndex         — SQLite FTS5 deep project indexer, marketplace content, 15 asset indexers (7 actions)
  MonolithSource        — Native C++ engine source indexer, call graphs, class hierarchy (11 actions)
  MonolithUI            — UI widget Blueprint CRUD, templates, styling, animation (42 actions)
  MonolithGAS           — Gameplay Ability System: abilities, effects, attributes, ASC, tags, cues, targeting (130 actions)
  MonolithBABridge      — Blueprint Assist integration bridge (0 MCP actions — IModularFeatures only)
```

**815 actions total across 13 modules, exposed through 15 MCP tools.**

### Tool Reference

| Namespace | Tool | Actions | Description |
|-----------|------|---------|-------------|
| `monolith` | `monolith_discover` | — | List available actions per namespace |
| `monolith` | `monolith_status` | — | Server health, version, index status |
| `monolith` | `monolith_reindex` | — | Trigger full project re-index |
| `monolith` | `monolith_update` | — | Check or install updates |
| `blueprint` | `blueprint_query` | 86 | Full Blueprint CRUD — read/write graphs, variables, components, functions, nodes, compile, CDO properties, auto-layout |
| `material` | `material_query` | 57 | Inspection, editing, graph building, material functions, previews, validation, CRUD |
| `animation` | `animation_query` | 115 | Montages, blend spaces, ABPs, skeletons, bone tracks, PoseSearch, IKRig, Control Rig |
| `niagara` | `niagara_query` | 96 | Systems, emitters, modules, parameters, renderers, HLSL, dynamic inputs, event handlers, sim stages, NPC, effect types |
| `editor` | `editor_query` | 19 | Build triggers, error logs, compile output, crash context, scene capture, texture import |
| `config` | `config_query` | 6 | INI resolution, explain, diff, search |
| `project` | `project_query` | 7 | Deep project search — FTS5 across all indexed assets including marketplace plugins |
| `source` | `source_query` | 11 | Native C++ engine source lookup, call graphs, class hierarchy, project reindex |
| `ui` | `ui_query` | 42 | UI widget Blueprint CRUD, templates, styling, animation, settings scaffolding, accessibility |
| `mesh` | `mesh_query` | 242 | Mesh inspection, scene manipulation, spatial queries, blockout, GeometryScript, horror analysis, lighting, audio, performance, procedural geometry, encounter design |
| `gas` | `gas_query` | 130 | Gameplay Ability System — abilities, effects, attributes, ASC, tags, cues, targeting, input, inspect, scaffold |

---

## Auto-Updater

Monolith checks for new versions on editor startup so you don't have to babysit GitHub.

1. **On editor startup** — Checks GitHub Releases for a newer version
2. **Downloads and stages** — If an update is found, it downloads and stages the new version
3. **Auto-swaps on exit** — The plugin is replaced when you close the editor
4. **Manual check** — `monolith_update` tool to check anytime
5. **Disable** — Toggle off in **Editor Preferences > Plugins > Monolith**

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
| **Mac/Linux not working** | Windows only for now. Mac and Linux are planned. |

---

## Configuration

Settings live at **Editor Preferences > Plugins > Monolith**:

| Setting | Default | Description |
|---------|---------|-------------|
| MCP Server Port | `9316` | Port for the embedded HTTP server |
| Auto-Update | `On` | Check GitHub Releases on editor startup |
| Module Toggles | All enabled | Enable/disable individual domain modules |
| Database Path | Project-local | Override SQLite database storage location |
| Index Marketplace Plugins | `On` | Index content from installed marketplace/Fab plugins |
| Index Data Assets | `On` | Deep-index DataAsset subclasses (15 indexers) |
| Additional Content Paths | `[]` | Extra content paths to include in the project index |
| Enable Procedural Town Gen | `Off` | **Work-in-progress** — 45 additional mesh actions for procedural building/town generation. Very much a WIP; unless you're willing to dig in and help improve it, best left alone for now |
| Enable GAS | `On` | Gameplay Ability System integration (130 actions, requires GameplayAbilities plugin) |
| Enable Blueprint Assist | `On` | Blueprint Assist integration for enhanced auto_layout (requires BA marketplace plugin) |

---

## Skills

Monolith bundles 12 Claude Code skills in `Skills/` — domain-specific workflow guides that give your AI the right mental model for each area:

| Skill | Description |
|-------|-------------|
| `unreal-blueprints` | Full Blueprint CRUD — read, create, edit variables/components/functions/nodes, compile |
| `unreal-materials` | PBR setup, graph building, validation |
| `unreal-animation` | Montages, ABP state machines, blend spaces |
| `unreal-niagara` | Particle system creation, HLSL modules, scalability |
| `unreal-mesh` | Mesh inspection, spatial queries, blockout, procedural geometry, horror/accessibility |
| `unreal-ui` | Widget Blueprint CRUD, templates, styling, accessibility |
| `unreal-gas` | Gameplay Ability System — abilities, effects, attributes, ASC, tags, cues |
| `unreal-debugging` | Build errors, log search, crash context |
| `unreal-performance` | Config auditing, shader stats, INI tuning |
| `unreal-project-search` | FTS5 search syntax, reference tracing |
| `unreal-cpp` | API lookup, include paths, Build.cs gotchas |
| `unreal-build` | Smart build decision-making, Live Coding vs full rebuild |

---

## Documentation

- [API_REFERENCE.md](Docs/API_REFERENCE.md) — Full action reference with parameters
- [SPEC.md](Docs/SPEC.md) — Technical specification and design decisions
- [CONTRIBUTING.md](CONTRIBUTING.md) — Dev setup, coding conventions, PR process
- [CHANGELOG.md](CHANGELOG.md) — Version history and release notes
- [Wiki](https://github.com/tumourlove/monolith/wiki) — Installation guides, test status, FAQ

---

## Contributing

Contributions are welcome. See [CONTRIBUTING.md](CONTRIBUTING.md) for dev environment setup, coding conventions, how to add new actions, and the PR process.

---

## License

[MIT](LICENSE) — See [ATTRIBUTION.md](ATTRIBUTION.md) for credits.
