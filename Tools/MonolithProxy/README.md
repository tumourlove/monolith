# Monolith MCP Proxy Configuration

## Current Configuration

The Monolith MCP is configured to use the C++ proxy executable:

```json
{
  "command": "D:/Unreal Projects/Leviathan/Plugins/Monolith/Binaries/monolith_proxy.exe",
  "args": []
}
```

This configuration is set in:
- `.mcp.json` (project-level)
- `~/.claude.json` (user-level)

## Rollback to Python Proxy (if needed)

If the C++ proxy encounters issues, you can revert to the Python proxy by updating both config files to:

```json
{
  "command": "python",
  "args": ["D:/Unreal Projects/Leviathan/Scripts/monolith_proxy.py"]
}
```

Update the monolith entry in:
1. `D:\Unreal Projects\Leviathan\.mcp.json`
2. `%USERPROFILE%\.claude.json` (Windows) or `~/.claude.json` (macOS / Linux)

Then restart Claude Code.

## Proxy Details

- **Python proxy:** `Scripts/monolith_proxy.py` — Stdio-to-HTTP proxy, survives editor restarts via background health polling
- **C++ proxy:** `Plugins/Monolith/Binaries/monolith_proxy.exe` — Native executable, faster startup
- **Backend:** Both connect to the same Monolith HTTP server running in the Unreal Editor
