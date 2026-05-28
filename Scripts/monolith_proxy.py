#!/usr/bin/env python3
"""
Monolith MCP stdio-to-HTTP proxy.

Sits between Claude Code (stdio) and Monolith (HTTP on localhost).
Handles initialize locally, forwards tool calls to Monolith.
Survives editor restarts — proxy process never dies.
Background health poll auto-detects when the editor comes online.

Usage (in .mcp.json):
  {"mcpServers": {"monolith": {"command": "python", "args": ["Plugins/Monolith/Scripts/monolith_proxy.py"]}}}

Requirements: Python 3.8+ (stdlib only, no pip install needed)
"""

# PEP 563: defer annotation evaluation so PEP 604 unions (`str | None`) below
# parse on Python 3.8/3.9 too (macOS ships 3.9 by default via Xcode).
from __future__ import annotations

import json
import os
import sys
import threading
import time
import tempfile
import urllib.error
import urllib.request
from io import TextIOWrapper
from pathlib import Path

MONOLITH_URL = os.environ.get("MONOLITH_URL", "http://localhost:9316/mcp")
MONOLITH_HEALTH = MONOLITH_URL.replace("/mcp", "/health")
PROXY_NAME = "monolith-proxy"
PROXY_VERSION = "1.1.1"
TIMEOUT = 30.0
POLL_INTERVAL = 5.0
POLL_START_DELAY = 3.0

# Track Monolith availability for list_changed notifications
_monolith_was_up = None
_stdout_lock = threading.Lock()

CORE_QUERY_TOOLS = [
    "blueprint_query",
    "material_query",
    "animation_query",
    "niagara_query",
    "editor_query",
    "config_query",
    "project_query",
    "source_query",
    "ui_query",
    "mesh_query",
    "gas_query",
    "combograph_query",
    "ai_query",
    "logicdriver_query",
    "audio_query",
    "level_sequence_query",
]


def _log(msg: str) -> None:
    """Log to stderr (visible in Claude Code debug mode, never interferes with stdio)."""
    print(f"[monolith-proxy] {msg}", file=sys.stderr, flush=True)


def _post_monolith(body: str, timeout: float = TIMEOUT) -> str | None:
    """POST JSON-RPC to Monolith. Returns response body or None on failure."""
    try:
        req = urllib.request.Request(
            MONOLITH_URL,
            data=body.encode("utf-8"),
            headers={"Content-Type": "application/json"},
            method="POST",
        )
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return resp.read().decode("utf-8")
    except (urllib.error.URLError, OSError, TimeoutError) as e:
        _log(f"Monolith unreachable: {e}")
        return None


def _write(stdout, msg: str) -> None:
    """Write a JSON-RPC message to stdout (thread-safe)."""
    with _stdout_lock:
        stdout.write(msg + "\n")
        stdout.flush()


def _result(id, result: dict) -> str:
    return json.dumps({"jsonrpc": "2.0", "id": id, "result": result})


def _tool_error(id, message: str) -> str:
    """Return a tool result with isError=true (graceful failure, not protocol error)."""
    return json.dumps({
        "jsonrpc": "2.0",
        "id": id,
        "result": {
            "content": [{"type": "text", "text": message}],
            "isError": True,
        },
    })


def _jsonrpc_error(id, code: int, message: str) -> str:
    """Return a JSON-RPC protocol-level error."""
    return json.dumps({
        "jsonrpc": "2.0",
        "id": id,
        "error": {"code": code, "message": message},
    })


def _sanitize_cache_part(value: str) -> str:
    return "".join(c if c.isalnum() or c in "-_" else "_" for c in value)


def _tools_cache_path() -> Path:
    base = Path(os.environ.get("LOCALAPPDATA") or tempfile.gettempdir())
    cache_dir = base / "Monolith"
    cache_dir.mkdir(parents=True, exist_ok=True)

    host_port = MONOLITH_HEALTH.replace("http://", "").replace("https://", "")
    host_port = host_port.split("/", 1)[0]
    return cache_dir / f"monolith_proxy_tools_{_sanitize_cache_part(host_port)}.json"


def _query_tool_schema() -> dict:
    return {
        "type": "object",
        "properties": {
            "action": {
                "type": "string",
                "description": "The action to execute. Use monolith_discover first when the editor is available.",
            },
            "params": {
                "type": "object",
                "description": "Parameters for the selected action.",
            },
        },
        "required": ["action"],
    }


def _empty_object_schema() -> dict:
    return {"type": "object", "properties": {}}


def _make_tool(name: str, description: str, schema: dict) -> dict:
    return {"name": name, "description": description, "inputSchema": schema}


def _seed_tools() -> list[dict]:
    tools = []
    for name in CORE_QUERY_TOOLS:
        domain = name[:-6] if name.endswith("_query") else name
        tools.append(_make_tool(
            name,
            f"Query the {domain} domain. The editor may be offline at session start; retry after Monolith is healthy.",
            _query_tool_schema(),
        ))

    tools.append(_make_tool(
        "monolith_discover",
        "List available tool namespaces and their actions. Pass namespace and optional category to filter.",
        {
            "type": "object",
            "properties": {
                "namespace": {"type": "string", "description": "Optional: filter to a specific namespace"},
                "category": {"type": "string", "description": "Optional: filter actions within the namespace by category"},
            },
        },
    ))
    tools.append(_make_tool(
        "monolith_status",
        "Get Monolith server health: version, uptime, port, registered action count, and module status.",
        _empty_object_schema(),
    ))
    tools.append(_make_tool(
        "monolith_update",
        "Check for or install Monolith updates from GitHub Releases.",
        {
            "type": "object",
            "properties": {
                "action": {
                    "type": "string",
                    "description": "'check' to compare versions, 'install' to download and stage update",
                    "default": "check",
                }
            },
        },
    ))
    tools.append(_make_tool(
        "monolith_reindex",
        "Re-index the Monolith project database. Requires the editor-side Monolith server.",
        _empty_object_schema(),
    ))
    return tools


def _write_tools_cache(resp: str) -> None:
    try:
        payload = json.loads(resp)
        tools = payload.get("result", {}).get("tools", [])
        if isinstance(tools, list) and tools:
            _tools_cache_path().write_text(json.dumps(tools), encoding="utf-8")
    except Exception as e:
        _log(f"Failed to write tools/list cache: {e}")


def _read_tools_cache() -> list[dict] | None:
    try:
        path = _tools_cache_path()
        if not path.exists():
            return None
        tools = json.loads(path.read_text(encoding="utf-8"))
        if isinstance(tools, list) and tools:
            return tools
    except Exception as e:
        _log(f"Failed to read tools/list cache: {e}")
    return None


def _fallback_tools_list(msg: dict) -> str:
    cached = _read_tools_cache()
    if cached:
        _log("Monolith down during tools/list — returning cached tools")
        return _result(msg.get("id"), {"tools": cached})

    _log("Monolith down during tools/list — returning seed tools")
    return _result(msg.get("id"), {"tools": _seed_tools()})


def _check_monolith_up() -> bool:
    """Lightweight health check via GET /health endpoint."""
    try:
        req = urllib.request.Request(MONOLITH_HEALTH, method="GET")
        with urllib.request.urlopen(req, timeout=3) as resp:
            return resp.status == 200
    except Exception:
        return False


def _send_list_changed(stdout) -> bool:
    """Send tools/list_changed notification. Returns False if stdout is broken."""
    try:
        _write(stdout, json.dumps({
            "jsonrpc": "2.0",
            "method": "notifications/tools/list_changed",
        }))
        return True
    except (BrokenPipeError, OSError):
        return False


def check_monolith_state_change(stdout) -> None:
    """Check for state transition and notify if changed."""
    global _monolith_was_up
    is_up = _check_monolith_up()

    if _monolith_was_up is not None and is_up != _monolith_was_up:
        direction = "online" if is_up else "offline"
        _log(f"Monolith went {direction} — sending tools/list_changed")
        _send_list_changed(stdout)

    _monolith_was_up = is_up


def _health_poll_thread(stdout) -> None:
    """Background thread that polls Monolith and sends list_changed on state transitions."""
    time.sleep(POLL_START_DELAY)
    _log(f"Health poll started (interval={POLL_INTERVAL}s)")

    while True:
        try:
            check_monolith_state_change(stdout)
        except (BrokenPipeError, OSError):
            _log("stdout broken, health poll exiting")
            return
        except Exception as e:
            _log(f"Health poll error: {e}")

        time.sleep(POLL_INTERVAL)


def handle_initialize(msg: dict) -> str:
    """Handle initialize locally. Proxy is always available."""
    client_version = msg.get("params", {}).get("protocolVersion", "2025-11-25")
    supported = {"2024-11-05", "2025-03-26", "2025-06-18", "2025-11-25"}
    version = client_version if client_version in supported else "2025-11-25"

    return _result(msg.get("id"), {
        "protocolVersion": version,
        "capabilities": {
            "tools": {"listChanged": True},
        },
        "serverInfo": {"name": PROXY_NAME, "version": PROXY_VERSION},
        "instructions": (
            "Monolith MCP proxy for Unreal Engine. Tools are forwarded to the Unreal Editor. "
            "Before calling a domain action, check its schema instead of guessing: "
            "monolith_discover() lists namespaces, monolith_discover('<namespace>') lists a "
            "namespace's actions, and describe_query('action_schema', ...) returns an action's "
            "exact parameter schema. monolith_guide(section='recipes') gives cross-namespace "
            "workflows, decision matrices, and gotchas. "
            "If tools return errors about the editor not running, wait and retry."
        ),
    })


def handle_ping(msg: dict) -> str:
    return _result(msg.get("id"), {})


def handle_tools_list(msg: dict) -> str:
    """Forward tools/list to Monolith. Stable cached/seed list if down."""
    resp = _post_monolith(json.dumps(msg))
    if resp:
        _write_tools_cache(resp)
        return resp
    return _fallback_tools_list(msg)


def handle_tools_call(msg: dict) -> str:
    """Forward tools/call to Monolith. Graceful error if down."""
    resp = _post_monolith(json.dumps(msg))
    if resp:
        return resp
    tool_name = msg.get("params", {}).get("name", "unknown")
    return _tool_error(
        msg.get("id"),
        f"Monolith MCP is not available (Unreal Editor not running). "
        f"Tool '{tool_name}' cannot execute. Start the editor and try again.",
    )


def main() -> None:
    # Use binary-safe IO for Windows compatibility
    stdin = TextIOWrapper(sys.stdin.buffer, encoding="utf-8", newline="\n")
    stdout = TextIOWrapper(sys.stdout.buffer, encoding="utf-8", newline="\n")

    _log(f"Started. Forwarding to {MONOLITH_URL}")

    # Start background health poller
    poller = threading.Thread(
        target=_health_poll_thread,
        args=(stdout,),
        daemon=True,
        name="monolith-health-poll",
    )
    poller.start()

    for line in stdin:
        line = line.strip()
        if not line:
            continue

        try:
            msg = json.loads(line)
        except json.JSONDecodeError as e:
            _log(f"Bad JSON: {e}")
            continue

        method = msg.get("method", "")
        msg_id = msg.get("id")  # None for notifications
        response = None

        if method == "initialize":
            response = handle_initialize(msg)
            _log("Initialized")

        elif method in ("notifications/initialized", "initialized"):
            # Notification — no response. Check if Monolith is up.
            check_monolith_state_change(stdout)

        elif method == "ping":
            response = handle_ping(msg)

        elif method == "tools/list":
            check_monolith_state_change(stdout)
            response = handle_tools_list(msg)

        elif method == "tools/call":
            response = handle_tools_call(msg)

        else:
            # Forward unknown methods to Monolith
            resp = _post_monolith(json.dumps(msg))
            if resp:
                response = resp
            elif msg_id is not None:
                response = _jsonrpc_error(msg_id, -32601, f"Method not found: {method}")

        if response:
            _write(stdout, response)


if __name__ == "__main__":
    main()
