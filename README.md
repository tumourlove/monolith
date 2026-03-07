# Monolith

**One plugin. Every Unreal domain. Zero Python bridges.**

[![UE 5.7+](https://img.shields.io/badge/Unreal-5.7%2B-blue)](https://unrealengine.com)
[![License: MIT](https://img.shields.io/badge/License-MIT-green.svg)](LICENSE)
[![MCP](https://img.shields.io/badge/Protocol-MCP-purple)](https://modelcontextprotocol.io)

---

## What is Monolith?

Monolith is a single Unreal Engine editor plugin that lets AI assistants control your UE editor through the **Model Context Protocol (MCP)** — an open standard that lets AI tools talk to external systems. Install one plugin, connect one endpoint, and your AI assistant gains full read/write access to Blueprints, Materials, Animation, Niagara, project configuration, and more.

Under the hood, Monolith embeds a Streamable HTTP server directly in the editor. It exposes **119 actions across 9 domains** — from inspecting Blueprint graph topology to building entire Niagara particle systems from a declarative spec. A built-in SQLite FTS5 project indexer lets your AI search across every asset in your project, and an optional engine source indexer provides offline C++ API lookups with call graphs and class hierarchies.

Monolith is built for UE developers using **Claude Code**, **Cursor**, or any other MCP-compatible client. If your AI tool speaks MCP, it can drive Monolith.

## Features

- **9 domains** — Blueprints, Materials, Animation, Niagara, Editor, Config, Project Index, Engine Source
- **~14 namespace tools** — Discovery/dispatch pattern keeps AI context lean (~95% reduction vs individual tools)
- **119 actions** — Full read/write coverage across all domains
- **Deep project indexer** — SQLite FTS5 full-text search across all asset types
- **Engine source intelligence** — Tree-sitter C++ parsing of the entire UE source tree with call graphs and inheritance
- **Auto-updater** — Checks GitHub Releases on editor startup, one-click update
- **9 Claude Code skills** — Domain-specific workflow guides bundled with the plugin
- **Live Coding integration** — Compile output capture with time-windowed error filtering
- **Pure C++** — Direct UE API access, embedded Streamable HTTP MCP server
- **Windows-only** — Mac and Linux support coming soon

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

Download the latest release from [GitHub Releases](https://github.com/tumourlove/monolith/releases), extract it, and place the folder at `YourProject/Plugins/Monolith/`.

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

This tells your AI client where to find Monolith's MCP server. You can also copy the template:

```bash
cp Plugins/Monolith/Templates/.mcp.json.example .mcp.json
```

### Step 3: Launch Unreal Editor

Open your `.uproject` file as normal. On first launch:

1. Monolith auto-indexes your entire project (takes 30–60 seconds depending on project size)
2. Open the **Output Log** (Window > Developer Tools > Output Log)
3. Filter for `LogMonolith` — you should see messages about the server starting and indexing completing

If you see `Monolith MCP server listening on port 9316`, you're good.

### Step 4: Connect Your AI

1. Open **Claude Code** (or your MCP client) in your project directory — the one containing `.mcp.json`
2. Claude Code auto-detects `.mcp.json` and connects to Monolith
3. Verify it works by asking: *"What Monolith tools do you have?"*

Your AI should list the Monolith namespace tools (blueprint.query, material.query, etc.).

### Step 5: (Optional) Engine Source Index

If you want your AI to look up Unreal Engine C++ APIs, function signatures, call graphs, and class hierarchies:

1. Install **Python 3.10+**
2. Run the source indexer script (see `Source/MonolithSource/` for details)
3. This builds a local SQLite database of the entire UE source tree
4. Once indexed, `source.query` actions become available

### Verify Everything Works

With the editor open, run this curl command from any terminal:

```bash
curl -X POST http://localhost:9316/mcp \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":1,"method":"tools/list"}'
```

You should get a JSON response listing all available Monolith tools. If you get "connection refused", make sure the editor is running and check the Output Log for errors.

### (Optional) Install Claude Code Skills

Monolith ships with 9 domain-specific workflow skills for Claude Code:

```bash
cp -r Plugins/Monolith/Skills/* ~/.claude/skills/
```

---

## How It Works

Monolith uses a **discovery/dispatch pattern**. Instead of registering 119 individual MCP tools (which would flood your AI's context window), each domain exposes a single `{namespace}.query(action, params)` tool. Call `monolith.discover()` to see what actions are available, then call the relevant namespace tool with the action name.

This means your AI only sees ~14 tools instead of 119, reducing token overhead by ~95% while keeping every action accessible. The central `FMonolithToolRegistry` routes each request to the correct handler.

---

## Architecture

```
Monolith.uplugin (119 actions total)
  MonolithCore          — HTTP server, tool registry, discovery, auto-updater (4 actions)
  MonolithBlueprint     — Blueprint graph reading (5 actions)
  MonolithMaterial      — Material inspection + graph editing (14 actions)
  MonolithAnimation     — Animation sequences, montages, ABPs (23 actions)
  MonolithNiagara       — Niagara particle systems (39 actions)
  MonolithEditor        — Build triggers, log capture, compile output, crash context (13 actions)
  MonolithConfig        — Config/INI resolution and search (6 actions)
  MonolithIndex         — SQLite FTS5 deep project indexer (5 actions)
  MonolithSource        — Engine source + API lookup (10 actions)
```

### Tool Reference

| Namespace | Tool | Actions | Description |
|-----------|------|---------|-------------|
| `monolith` | `monolith_discover` | — | List available actions per namespace |
| `monolith` | `monolith_status` | — | Server health, version, index status |
| `monolith` | `monolith_reindex` | — | Trigger full project re-index |
| `monolith` | `monolith_update` | — | Check or install updates |
| `blueprint` | `blueprint.query` | 5 | Graph topology, variables, execution flow, node search |
| `material` | `material.query` | 14 | Inspection, editing, graph building, previews, validation |
| `animation` | `animation.query` | 23 | Montages, blend spaces, ABPs, skeletons, bone tracks |
| `niagara` | `niagara.query` | 39 | Systems, emitters, modules, parameters, renderers, HLSL |
| `editor` | `editor.query` | 13 | Build triggers, error logs, compile output, crash context |
| `config` | `config.query` | 6 | INI resolution, explain, diff, search |
| `project` | `project.query` | 5 | Deep project search — FTS5 across all indexed assets |
| `source` | `source.query` | 10 | Engine source lookup, call graphs, class hierarchy |

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
| **Claude can't find any tools** | Check that `.mcp.json` has `"type": "streamableHttp"` (not `"http"` or `"sse"`). Restart Claude Code after creating the file. |
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

- [API_REFERENCE.md](Docs/API_REFERENCE.md) — Full reference for all 119 actions with parameters
- [SPEC.md](Docs/SPEC.md) — Technical specification and design decisions
- [CONTRIBUTING.md](CONTRIBUTING.md) — Development setup, coding conventions, PR process
- [CHANGELOG.md](CHANGELOG.md) — Version history and release notes

---

## Contributing

Contributions are welcome! See [CONTRIBUTING.md](CONTRIBUTING.md) for dev environment setup, coding conventions, how to add new actions, and the PR process.

---

## License

[MIT](LICENSE) — See [ATTRIBUTION.md](ATTRIBUTION.md) for credits.
