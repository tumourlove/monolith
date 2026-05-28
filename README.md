# Monolith

**One plugin. Every Unreal domain. Zero dependencies.**

[![UE 5.7+](https://img.shields.io/badge/Unreal-5.7%2B-blue)](https://unrealengine.com)
[![License: MIT](https://img.shields.io/badge/License-MIT-green.svg)](LICENSE)
[![MCP](https://img.shields.io/badge/Protocol-MCP-purple)](https://modelcontextprotocol.io)

---

## Why I built this

Most MCP integrations for Unreal register every action as a separate tool. That floods the AI's context window with hundreds of tool names before you've asked a single question — and the actually useful stuff gets buried. I built Monolith because I wanted my AI to spend its context on my problem, not on memorising a tool catalogue.

One plugin. One MCP endpoint. 23 tools instead of 1300+. The AI calls `monolith_discover()` and `monolith_guide()` when it needs to know what's available, and otherwise just hits `blueprint_query("create_asset", ...)`, `material_query("compile", ...)`, and so on.

I use it every day. It does what I need.

---

## What it does

Monolith exposes **1348 actions across 19 in-tree namespaces** through a namespace-dispatch pattern: each domain registers a single `{namespace}_query(action, params)` tool, and a central `monolith_discover()` lists everything available.

Covered domains: Blueprints, Materials, Animation, Niagara, Mesh, UI (incl. CommonUI), AI (Behavior Trees, State Trees, EQS, Smart Objects, Perception, Navigation), Gameplay Ability System, Logic Driver state machines, ComboGraph combo trees, Audio (Sound Cues + MetaSounds), Editor control (UBT builds, log capture, scene capture, asset preview & inspection), Engine source search (1M+ symbols, fully offline), Project asset search (SQLite FTS5), INI config, Level Sequences, plus a `bulk_fill` / `describe` reflection framework for deep property writes and a `monolith_guide` self-onboarding tool for your AI.

Full per-namespace breakdown: **[Tool Reference (wiki)](https://github.com/tumourlove/monolith/wiki/Tool-Reference)**.

Works with **Claude Code**, **Cursor**, **Cline**, or any MCP-compatible client. Windows, macOS, Linux.

---

## Quick install

**1. Drop into Plugins/**

```bash
cd YourProject/Plugins
git clone https://github.com/tumourlove/monolith.git Monolith
```

(Or grab the [latest release zip](https://github.com/tumourlove/monolith/releases) and extract to the same path. The release zip includes precompiled DLLs so Blueprint-only projects can open the editor immediately without rebuilding.)

**2. Create `.mcp.json`** in your project root (same directory as your `.uproject`):

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

The native C++ proxy keeps your AI session alive when the editor restarts. For **Cursor/Cline**, **macOS/Linux**, or the **Python fallback**, see the [Installation wiki page](https://github.com/tumourlove/monolith/wiki/Installation).

**3. Open the editor.** Wait 30-60 seconds for the first-launch index. When you see `Monolith MCP server listening on port 9316` in the Output Log (filter `LogMonolith`), connect your AI client and ask *"what Monolith tools do you have?"* to verify.

Project-instructions files (`CLAUDE.md`, `AGENTS.md`, `.cursorrules`, etc.) vary per assistant — just paste the namespace list into your AI and ask it to generate the right format for your toolchain. Full install variants, troubleshooting, and post-install setup live on the [Installation wiki](https://github.com/tumourlove/monolith/wiki/Installation).

---

## Standalone tools

Two zero-dependency C++ executables ship in `Binaries/` and work without the editor:

- **`monolith_proxy.exe`** — MCP stdio↔HTTP proxy. Keeps your AI session alive across editor restarts. Used by the `.mcp.json` config above.
- **`monolith_query.exe`** — Offline DB query tool. Queries the engine source index and project asset index without launching UE (instant startup). Useful for terminal-side lookups and CI.

Details: [wiki Tool Reference](https://github.com/tumourlove/monolith/wiki/Tool-Reference).

---

## Auto-updater

Off by default as of v0.14.6. Opt in via **Auto Update Enabled** in Editor Preferences > Plugins > Monolith — checks GitHub Releases on editor startup, verifies the downloaded zip's SHA256 against the release-notes marker, swaps the plugin on editor exit (after a Y/N prompt). See [Auto-Updater wiki](https://github.com/tumourlove/monolith/wiki/Auto-Updater).

---

## Network exposure

Monolith starts a local HTTP server on port 9316 to receive MCP traffic. UE's `FHttpServerModule` does **not** expose a bind-address parameter, so the listener is reachable on all network interfaces, not just `127.0.0.1`. CORS is restricted to localhost origins (which blocks browser-based cross-origin reads) but does **not** block direct HTTP requests from other devices on the same LAN.

If you work on an untrusted network: either add a Windows Firewall rule blocking inbound TCP on port 9316 from non-loopback addresses, or untick **MCP Server Enabled** in Editor Preferences > Plugins > Monolith and restart the editor.

See [SECURITY.md](SECURITY.md) for the full threat model and disclosure policy.

---

## Documentation

- **[Wiki](https://github.com/tumourlove/monolith/wiki)** — installation variants, tool reference, connecting your AI, configuration, auto-updater, FAQ, sibling plugin guide, skills, optional modules, engine source index details, mesh module deep dive, horror level design, procedural geometry, genre presets, test status
- **[API_REFERENCE.md](Docs/API_REFERENCE.md)** — full per-action parameter reference, regenerated from the live registry each release
- **[SPEC_CORE.md](Docs/SPEC_CORE.md)** — technical specification and architecture; per-module specs at [`Docs/specs/`](Docs/specs/)
- **[CHANGELOG.md](CHANGELOG.md)** — version history, contributor credits, breaking-change notes
- **[CONTRIBUTING.md](CONTRIBUTING.md)** — dev setup, coding conventions, how to add new actions, PR process

---

## Contributing

Contributions welcome. See [CONTRIBUTING.md](CONTRIBUTING.md). Every release [CHANGELOG](CHANGELOG.md) names the PR authors and issue reporters whose work shipped — credit goes where it's due.

---

## License

[MIT](LICENSE) — see [ATTRIBUTION.md](ATTRIBUTION.md) for credits.
