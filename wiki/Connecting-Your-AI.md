# Connecting Your AI

Monolith works with any MCP-compatible client.

## Claude Code (recommended: proxy mode)

Claude Code has a known issue where HTTP MCP sessions die permanently when the Unreal Editor restarts. Monolith ships with a **stdio-to-HTTP proxy** that solves this — it keeps the MCP session alive across editor restarts, no manual reconnection needed.

The proxy is available as a **standalone C++ executable** (zero dependencies, recommended) or a Python script (fallback).

**Option A: Native C++ proxy (recommended — no Python required)**

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

**Option B: Use the Python launcher**

Requires Python 3.8+ ([python.org](https://python.org)).

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

**Option C: Call Python directly (if you need a specific Python)**

```json
{
  "mcpServers": {
    "monolith": {
      "command": "python",
      "args": ["Plugins/Monolith/Scripts/monolith_proxy.py"]
    }
  }
}
```

Or copy the template:

```bash
cp Plugins/Monolith/Templates/.mcp.json.proxy.example .mcp.json
```

> **How it works:** The proxy handles `initialize` locally so Claude Code always sees a live MCP server. It forwards tool calls to Monolith's HTTP endpoint (`127.0.0.1:9316`). A background health poll detects when the editor comes up or goes down and sends `notifications/tools/list_changed` — Claude Code auto-refreshes its tool list. When the editor is down, tool calls return graceful errors instead of killing the session.

**Option D: Direct HTTP (no proxy, no auto-reconnect)**

If you prefer a simpler setup, you can connect directly. You'll need to restart Claude Code each time the editor restarts.

```json
{
  "mcpServers": {
    "monolith": {
      "type": "http",
      "url": "http://127.0.0.1:9316/mcp"
    }
  }
}
```

## Cursor

Add the MCP server in Cursor's settings. The endpoint is `http://127.0.0.1:9316/mcp`.

```json
{
  "mcpServers": {
    "monolith": {
      "type": "streamableHttp",
      "url": "http://127.0.0.1:9316/mcp"
    }
  }
}
```

> Cursor and Cline handle server restarts natively — the proxy isn't needed.

## Other MCP Clients

Any client supporting the Streamable HTTP transport can connect. Point it at:

```
http://127.0.0.1:9316/mcp
```

## How It Works

Monolith uses a **namespace dispatch pattern** — each domain exposes a single `{namespace}_query(action, params)` tool. Call `monolith_discover()` first to see what's available, or `monolith_guide()` to have your AI self-onboard. This keeps the tool list small while exposing **~1,500+ actions across 27 in-tree namespaces** (counts are approximate — query `monolith_discover()` for the live figure).

## Reducing MCP Context Cost

When an MCP client connects, it fetches the server's `tools/list` manifest once and keeps it in context for the session. Monolith exposes a small set of namespace-dispatch tools rather than ~1,500+ individual ones, but the manifest still carries each dispatcher's parameter schema, so it has a real (one-time) context cost.

The server now ships a **trimmed manifest**. Each dispatcher's `action` enum (the authoritative list of valid actions) is kept, but the full action list is no longer also duplicated as prose in the dispatcher `description`. That removed roughly 16k tokens (~40%) from the manifest. The action list is still available on demand — call `monolith_discover("<namespace>")` for any namespace.

You can trim further with client-side configuration:

- **Codex** loads the full tool manifest eagerly on every request and has no native deferred-loading yet ([codex#14507](https://github.com/openai/codex/issues/14507)). Use the `enabled_tools` allowlist (and optionally `disabled_tools` / profiles) under `[mcp_servers.monolith]` in `~/.codex/config.toml` to surface only the namespaces you need. The server-side manifest trim above is the main lever that reduces Codex's per-request load.

- **Claude Code** defers MCP tools by default through Tool Search (`ENABLE_TOOL_SEARCH` unset or `auto`) — deferred tools are surfaced on demand instead of being loaded eagerly. HTTP-transport deferral has been version-sensitive in the past ([claude-code#40314](https://github.com/anthropics/claude-code/issues/40314)), so **verify on your build** by running `/context` and checking whether the `monolith` tools are listed as deferred or eagerly loaded. On current Claude Code builds, Monolith's HTTP MCP tools were observed deferred (surfaced via Tool Search, not eagerly loaded).

## Testing the Connection

```bash
curl -X POST http://127.0.0.1:9316/mcp \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":1,"method":"tools/list"}'
```
