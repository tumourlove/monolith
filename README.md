# Monolith

**One plugin. Every Unreal domain. Zero Python bridges.**

[![UE 5.7+](https://img.shields.io/badge/Unreal-5.7%2B-blue)](https://unrealengine.com)
[![License: MIT](https://img.shields.io/badge/License-MIT-green.svg)](LICENSE)
[![MCP](https://img.shields.io/badge/Protocol-MCP-purple)](https://modelcontextprotocol.io)

---

## What is Monolith?

Monolith is an Unreal Engine editor plugin that gives AI assistants full read/write access to your project through the [Model Context Protocol (MCP)](https://modelcontextprotocol.io). Install one plugin, point your AI client at one endpoint, and it can work with Blueprints, Materials, Animation, Niagara, project configuration, and more.

It works with **Claude Code**, **Cursor**, or any MCP-compatible client. If your AI tool speaks MCP, it can drive Monolith.

> **Platform:** Windows only. Mac and Linux support is coming soon.

## Why Monolith?

Most MCP integrations register every action as a separate tool, which floods the AI's context window with tool descriptions. Monolith uses a **namespace dispatch pattern** instead: each domain exposes a single `{namespace}_query(action, params)` tool, and a central `monolith_discover()` call lists what's available. This keeps the tool list small (12 tools) while still exposing 177 actions across nine domains.

## Features

- **Full Blueprint inspection** — Graph topology, variables, execution flow, node search
- **Material graph editing** — Read, build, and validate material graphs with preview support
- **Animation coverage** — Montages, blend spaces, ABP state machines, skeletons, bone tracks
- **Niagara particle systems** — Create and edit systems, emitters, modules, parameters, renderers, and HLSL
- **Editor integration** — Build triggers, log capture, compile output, crash context, Live Coding support
- **Config management** — INI resolution, diff, search, and explain
- **Deep project search** — SQLite FTS5 full-text search across all indexed assets
- **Engine source intelligence** — Tree-sitter C++ parsing of the UE source tree with call graphs and class hierarchy
- **Auto-updater** — Checks GitHub Releases on editor startup, one-click update
- **Claude Code skills** — Domain-specific workflow guides bundled with the plugin
- **Pure C++** — Direct UE API access, embedded Streamable HTTP server

---

## Installation for Dummies (Step-by-Step)

### Prerequisites

- **Unreal Engine 5.7+** — Launcher or source build ([unrealengine.com](https://unrealengine.com))

> **Platform:** Windows only. Mac and Linux support is coming soon.

- **Claude Code, Cursor, or another MCP client** — Any tool that supports the Model Context Protocol
- **(Optional) Python 3.10+** — Only needed if you want engine source code lookups

### Step 1: Download Monolith

Every Unreal project has a `Plugins/` folder. If yours doesn't exist yet, create it. It lives at the same level as your `.uproject` file:

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

Download the latest release from [GitHub Releases](https://github.com/tumourlove/monolith/releases), extract it, and place the folder at `YourProject/Plugins/Monolith/`. The release ZIP includes precompiled DLLs — Blueprint-only projects can launch the editor immediately without rebuilding. C++ projects should rebuild first.

**Option C: Let your AI install it**

If you're already in a Claude Code, Cursor, or Cline session, just tell your AI:

> "Install the Monolith plugin from https://github.com/tumourlove/monolith into my project's Plugins folder"

The AI will clone the repo, create `.mcp.json`, and configure everything for you. It can also detect your MCP client and use the correct transport type automatically. After it finishes, **restart your AI session** so it picks up the new MCP server from `.mcp.json`, then skip to [Step 3](#step-3-launch-unreal-editor).

### Step 2: Configure the MCP Connection

Create a file called `.mcp.json` in your **project root** — the same directory as your `.uproject` file:

```
YourProject/
  YourProject.uproject
  .mcp.json          <-- create this file
  Plugins/
    Monolith/
```

Paste this exact content into `.mcp.json`:

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

**For Claude Code:**

```json
{
  "mcpServers": {
    "monolith": {
      "type": "http",
      "url": "http://localhost:9316/mcp"
    }
  }
}
```

> **Important:** Claude Code uses `"http"` as the transport type, while Cursor and Cline use `"streamableHttp"`. Using the wrong type will cause connection failures.

This tells your AI client where to find Monolith's MCP server. You can also copy the template:

```bash
cp Plugins/Monolith/Templates/.mcp.json.example .mcp.json
```

### Step 3: Launch Unreal Editor

Open your `.uproject` file as normal. On first launch:

1. Monolith auto-indexes your entire project (takes 30-60 seconds depending on project size)
2. Open the **Output Log** (Window > Developer Tools > Output Log)
3. Filter for `LogMonolith` — you should see messages about the server starting and indexing completing

If you see `Monolith MCP server listening on port 9316`, you're good.

### Step 4: Connect Your AI

1. Open **Claude Code** (or your MCP client) in your project directory — the one containing `.mcp.json`
2. Claude Code auto-detects `.mcp.json` and connects to Monolith
3. Verify it works by asking: *"What Monolith tools do you have?"*

Your AI should list the Monolith namespace tools (`blueprint_query`, `material_query`, etc.).

### Step 5: (Optional) Engine Source Index

If you want your AI to look up Unreal Engine C++ APIs, function signatures, call graphs, and class hierarchies:

1. Install **Python 3.10+**
2. Run the source indexer script (see `Source/MonolithSource/` for details)
3. This builds a local SQLite database of the entire UE source tree
4. Once indexed, `source_query` actions become available

### Verify Everything Works

With the editor open, run this curl command from any terminal:

```bash
curl -X POST http://localhost:9316/mcp \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":1,"method":"tools/list"}'
```

You should get a JSON response listing all available Monolith tools. If you get "connection refused", make sure the editor is running and check the Output Log for errors.

### (Optional) Install Claude Code Skills

Monolith ships with domain-specific workflow skills for Claude Code:

```bash
cp -r Plugins/Monolith/Skills/* ~/.claude/skills/
```

---

## Architecture

```
Monolith.uplugin
  MonolithCore          — HTTP server, tool registry, discovery, auto-updater (4 actions)
  MonolithBlueprint     — Blueprint graph reading (6 actions)
  MonolithMaterial      — Material inspection + graph editing + CRUD (25 actions)
  MonolithAnimation     — Animation sequences, montages, ABPs, PoseSearch (67 actions)
  MonolithNiagara       — Niagara particle systems (41 actions)
  MonolithEditor        — Build triggers, log capture, compile output, crash context (13 actions)
  MonolithConfig        — Config/INI resolution and search (6 actions)
  MonolithIndex         — SQLite FTS5 deep project indexer (5 actions)
  MonolithSource        — Engine source + API lookup (10 actions)
```

**177 actions total across 9 modules, exposed through 12 MCP tools.**

### Tool Reference

| Namespace | Tool | Actions | Description |
|-----------|------|---------|-------------|
| `monolith` | `monolith_discover` | — | List available actions per namespace |
| `monolith` | `monolith_status` | — | Server health, version, index status |
| `monolith` | `monolith_reindex` | — | Trigger full project re-index |
| `monolith` | `monolith_update` | — | Check or install updates |
| `blueprint` | `blueprint_query` | 6 | Graph topology, variables, execution flow, node search, graph summary |
| `material` | `material_query` | 25 | Inspection, editing, graph building, previews, validation, CRUD |
| `animation` | `animation_query` | 67 | Montages, blend spaces, ABPs, skeletons, bone tracks, PoseSearch |
| `niagara` | `niagara_query` | 41 | Systems, emitters, modules, parameters, renderers, HLSL |
| `editor` | `editor_query` | 13 | Build triggers, error logs, compile output, crash context |
| `config` | `config_query` | 6 | INI resolution, explain, diff, search |
| `project` | `project_query` | 5 | Deep project search — FTS5 across all indexed assets |
| `source` | `source_query` | 10 | Engine source lookup, call graphs, class hierarchy |

### What Can the AI Actually Do?

**Blueprint** — Read any Blueprint's graph structure, trace execution flow between nodes, list variables with defaults, search for specific node types, and get a lightweight summary. Useful for understanding existing logic, auditing complexity, or planning a Blueprint-to-C++ migration.

**Material** — Create materials and material instances from scratch, add and connect expression nodes, set parameters (scalars, vectors, textures), build full PBR graphs programmatically, recompile, validate for errors, and inspect compiled shader stats. The AI can build a complete material from a text description.

**Animation** — Inspect and modify animation sequences, montages, blend spaces, Animation Blueprints, state machines, skeletons, and PoseSearch databases. Read bone hierarchies, add/remove notifies, edit montage sections, create blend space samples, and trace ABP state transitions. Covers the full animation pipeline.

**Niagara** — Create particle systems from specs, add/remove emitters and modules, set module input values and bindings, configure data interfaces and renderers, edit parameters, read compiled GPU HLSL, and batch-execute multiple operations atomically. The AI can build a complete VFX system from a text description.

**Editor** — Trigger builds (full UBT or Live Coding), read build errors and compiler output, search editor logs, get crash context after failures, and query editor state. The AI can compile your code and diagnose build failures without you touching the editor.

**Config** — Read, search, and diff INI configuration files with full resolution chain awareness (Base → Platform → Project → User). Explain what a setting does and where it's overridden. Useful for performance tuning and debugging config issues.

**Project** — Full-text search across every indexed asset in your project. Find assets by name, type, path, or content. Trace references between assets. The search index updates automatically when assets change.

**Source** — Look up any Unreal Engine C++ API: read function implementations, search across the entire engine source, get class hierarchies, trace call graphs (callers and callees), and verify include paths. The AI never has to guess a function signature — it can check the actual source.

---

## Auto-Updater

Monolith includes a built-in auto-updater:

1. **On editor startup** — Checks GitHub Releases for a newer version
2. **Downloads and stages** — If an update is found, it downloads and stages the new version
3. **Auto-swaps on exit** — The plugin is replaced when you close the editor
4. **Manual check** — Use the `monolith_update` tool to check for updates at any time
5. **Disable** — Toggle off in **Editor Preferences > Plugins > Monolith**

---

## Troubleshooting / FAQ

| Problem | Solution |
|---------|----------|
| **Plugin doesn't appear in editor** | Verify the folder is at `YourProject/Plugins/Monolith/` and contains `Monolith.uplugin`. Check you're on UE 5.7+. |
| **MCP connection refused** | Make sure the editor is open and running. Check Output Log for port conflicts. Verify `.mcp.json` is in your project root. |
| **Index shows 0 assets** | Run `monolith_reindex` or restart the editor. Check Output Log for indexing errors. |
| **Source tools return empty results** | The Python engine source indexer hasn't been run. See Step 5 above. |
| **Claude can't find any tools** | Check `.mcp.json` transport type: Claude Code uses `"http"`, Cursor/Cline use `"streamableHttp"`. Restart your AI client after creating the file. |
| **Tools fail on first try** | Restart Claude Code to refresh the MCP connection. This is a known quirk with initial connection timing. |
| **Port 9316 already in use** | Change the port in Editor Preferences > Plugins > Monolith, then update the port in `.mcp.json` to match. |
| **Mac/Linux not working** | Monolith currently supports Windows only. Mac and Linux support is planned for a future release. |

---

## Configuration

Plugin settings are at **Editor Preferences > Plugins > Monolith**:

| Setting | Default | Description |
|---------|---------|-------------|
| MCP Server Port | `9316` | Port for the embedded HTTP server |
| Auto-Update | `On` | Check GitHub Releases on editor startup |
| Module Toggles | All enabled | Enable/disable individual domain modules |
| Database Path | Project-local | Override SQLite database storage location |

---

## Skills

Monolith bundles 9 Claude Code skills in `Skills/` for domain-specific workflows:

| Skill | Description |
|-------|-------------|
| `unreal-blueprints` | Graph reading, variable inspection, execution flow |
| `unreal-materials` | PBR setup, graph building, validation |
| `unreal-animation` | Montages, ABP state machines, blend spaces |
| `unreal-niagara` | Particle system creation, HLSL modules, scalability |
| `unreal-debugging` | Build errors, log search, crash context |
| `unreal-performance` | Config auditing, shader stats, INI tuning |
| `unreal-project-search` | FTS5 search syntax, reference tracing |
| `unreal-cpp` | API lookup, include paths, Build.cs gotchas |
| `unreal-build` | Smart build decision-making, Live Coding vs full rebuild |

---

## Documentation

- [API_REFERENCE.md](Docs/API_REFERENCE.md) — Full action reference with parameters
- [SPEC.md](Docs/SPEC.md) — Technical specification and design decisions
- [CONTRIBUTING.md](CONTRIBUTING.md) — Development setup, coding conventions, PR process
- [CHANGELOG.md](CHANGELOG.md) — Version history and release notes
- [Wiki](https://github.com/tumourlove/monolith/wiki) — Installation guides, test status, FAQ

---

## Contributing

Contributions are welcome! See [CONTRIBUTING.md](CONTRIBUTING.md) for dev environment setup, coding conventions, how to add new actions, and the PR process.

---

## License

[MIT](LICENSE) — See [ATTRIBUTION.md](ATTRIBUTION.md) for credits.
