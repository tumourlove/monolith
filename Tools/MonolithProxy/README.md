# Monolith MCP Proxy Configuration

## Current Configuration

The Monolith MCP is configured to use the C++ proxy executable:

```json
{
  "command": "<project-root>/Plugins/Monolith/Binaries/monolith_proxy.exe",
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
  "args": ["<project-root>/Scripts/monolith_proxy.py"]
}
```

Update the monolith entry in:
1. `<project-root>/.mcp.json`
2. `%USERPROFILE%\.claude.json` (Windows) or `~/.claude.json` (macOS / Linux)

Then restart Claude Code.

## Proxy Details

- **Python proxy:** `Scripts/monolith_proxy.py` — Stdio-to-HTTP proxy, survives editor restarts via background health polling
- **C++ proxy:** `Plugins/Monolith/Binaries/monolith_proxy.exe` — Native executable, faster startup
- **Backend:** Both connect to the same Monolith HTTP server running in the Unreal Editor
- **Editor-down startup:** Both proxies return a cached Monolith tool list when available, or a stable seed list of namespace/meta tools. This prevents MCP clients that do not fully refresh on `tools/list_changed` from starting with an empty Monolith catalog.

## Native Proxy Build

Run `build_proxy.bat` from `Tools\MonolithProxy`; `build.bat` is a compatibility entry point that delegates to the same implementation. The build uses `cl.exe` from the active developer environment or discovers the installed x64 Visual C++ toolchain through `vswhere.exe`.

Compilation happens in a private staging directory. Publication then copies the completed executable to a unique candidate beside the destination, verifies its size, and renames that candidate over `Binaries\monolith_proxy.exe`. Output-directory creation, candidate copy, final replacement, and final size are all checked. A failure returns non-zero without printing success, removes only files owned by that invocation, and preserves any existing proxy when replacement did not occur.

For isolated verification, `MONOLITH_PROXY_SOURCE_FILE`, `MONOLITH_PROXY_OUTPUT_DIR`, and `MONOLITH_PROXY_STAGING_DIR` override the source, destination, and not-yet-existing staging directory. `MONOLITH_PROXY_VSWHERE` may point to an explicit `vswhere.exe`. Run `powershell -NoProfile -ExecutionPolicy Bypass -File Scripts\test_proxy_build.ps1` to verify both entry points plus compile- and publication-failure preservation without writing to the repository's `Binaries` directory.

## Call Log

Both proxies append one JSONL line per upstream MCP roundtrip to:

```
<project-root>/Saved/Logs/MonolithCalls.jsonl
```

Path resolution: `MONOLITH_PROJECT_ROOT` env var if set, otherwise the proxy's
current working directory (Claude Code launches the proxy with the project root
as CWD). The `Saved/Logs/` parent directories are created on first run.

### Schema (one object per line, terminated by `\n`)

```json
{"ts":"2026-05-27T18:14:56Z","namespace":"editor","action":"get_build_errors","params_hash":"da39a3ee5e6b4b0d3255bfef95601890afd80709","duration_ms":42.5,"ok":true,"error_code":null,"result_bytes":1834}
```

| Field | Type | Notes |
|---|---|---|
| `ts` | string | ISO-8601 UTC, second precision. |
| `namespace` | string | For `*_query` tools: the prefix (e.g. `editor`). For `monolith_*` tools: `monolith`. For non-`tools/call` methods: the method name (`initialize`, `tools/list`, `ping`). |
| `action` | string | For `*_query`: the `action` argument. For `monolith_*`: the suffix (`discover`, `status`, etc.). Empty otherwise. |
| `params_hash` | string | 40-char hex SHA-1 over canonicalised JSON of the params dict (`sort_keys=True`, tightest separators). Used to recognise repeat calls without storing arguments. NOT 32-bit FCrc — collision-safe. |
| `duration_ms` | number | Wall time between sending the upstream HTTP request and receiving the response. |
| `ok` | bool | `true` iff the response has no JSON-RPC `error` field. |
| `error_code` | int or null | The JSON-RPC `error.code` when `ok` is false; null otherwise. |
| `result_bytes` | int | Byte length of the serialised `result` payload (or the full response body if no `result`). |

### Opt-out

Set the environment variable `MONOLITH_CALL_LOG=0` before launching the proxy.
This disables emission entirely — no file handle is opened, no writes are made.
Any other value (including unset) leaves logging enabled. The env var is read
once at startup; toggling it mid-session requires a proxy restart.

### Use cases

- Post-hoc grep / analysis of which actions an agent session called.
- Spot the silent retries hidden by the dedup window.
- Pipe through your own log-tailing tool for live tailing.
- Cheap input to future Markov-style breadcrumb analytics (deferred — substrate
  ships first, consumers later).

### Privacy

Local-only. Nothing is uploaded. The `params_hash` is one-way — the original
parameter values cannot be recovered from the log. Filenames and asset paths
inside the hashed JSON are not extractable from the line.

### Rotation / reset

User-managed. Delete the file to start fresh; the proxies recreate it on the
next call. No automatic rotation in v1 — the file is small (≈200 bytes/line) and
single-user.

### Crash-reporter exclusion

UE's crash reporter sweeps editor logs from `Saved/Logs/`, not arbitrary JSONL.
If a downstream crash collector pattern is added that sweeps `Saved/Logs/*`,
add `MonolithCalls.jsonl` to its exclusion list. The file is intentionally
local-only.
