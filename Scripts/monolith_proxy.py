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

import hashlib
import json
import os
import socket
import sys
import threading
import time
import tempfile
import urllib.error
import urllib.request
from datetime import datetime, timezone
from io import TextIOWrapper
from pathlib import Path

MONOLITH_URL = os.environ.get("MONOLITH_URL", "http://127.0.0.1:9316/mcp")
MONOLITH_HEALTH = MONOLITH_URL.replace("/mcp", "/health")
PROXY_NAME = "monolith-proxy"
PROXY_VERSION = "1.2.0"


def _read_timeout() -> float:
    """Request timeout in seconds (MONOLITH_PROXY_TIMEOUT_S, default 120)."""
    raw = os.environ.get("MONOLITH_PROXY_TIMEOUT_S", "")
    if raw:
        try:
            val = float(raw)
            if 0.0 < val <= 86400.0:
                return val
        except ValueError:
            pass
        sys.stderr.write(f"[monolith-proxy] ignoring invalid MONOLITH_PROXY_TIMEOUT_S='{raw}'\n")
    return 120.0


TIMEOUT = _read_timeout()
# Set by _post_monolith: True when the last failure was a timeout rather than a refused connection.
_last_post_timed_out = False
POLL_INTERVAL = 5.0
POLL_START_DELAY = 3.0

# Track Monolith availability for list_changed notifications
_monolith_was_up = None
_stdout_lock = threading.Lock()

# Call-log state (Phase 4 / survivor F)
#
# NOTE: Saved/Logs/MonolithCalls.jsonl is project-root-relative and excluded
# from crash zip generation by UE's crash reporter (Saved/Logs/ tail capture
# only includes editor logs, not arbitrary jsonl). If a crash collector pattern
# elsewhere DOES sweep Saved/Logs/*, the user should add MonolithCalls.jsonl to
# the exclusion list. Single-user local dev tool; no phone-home.
_call_log_enabled = False           # resolved once at startup
_call_log_handle = None             # binary append-mode file handle
_call_log_lock = threading.Lock()

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


# ----------------------------------------------------------------------------
# JSONL call log (Phase 4 / survivor F)
#
# One line per upstream HTTP roundtrip:
#   {"ts":"2026-05-27T18:14:56Z","namespace":"editor","action":"get_build_errors",
#    "params_hash":"<40-char-sha1-hex>","duration_ms":42.5,"ok":true,
#    "error_code":null,"result_bytes":1834}
#
# Path: <project-root>/Saved/Logs/MonolithCalls.jsonl
# Opt-out: env var MONOLITH_CALL_LOG=0
# Atomicity: open(..., "ab") + threading.Lock around write+flush is sufficient
# for single-process emission. POSIX O_APPEND would give kernel-level atomicity
# for writes < PIPE_BUF (~4KB), but the lock makes the choice moot for our
# line sizes.
# ----------------------------------------------------------------------------


def _resolve_call_log_path() -> Path:
    """Resolve <project-root>/Saved/Logs/MonolithCalls.jsonl.

    Priority:
      1. MONOLITH_PROJECT_ROOT env var (explicit override).
      2. Current working directory (proxy CWD is the project root when launched
         by Claude Code's MCP config).
    """
    root = os.environ.get("MONOLITH_PROJECT_ROOT") or os.getcwd()
    logs_dir = Path(root) / "Saved" / "Logs"
    logs_dir.mkdir(parents=True, exist_ok=True)
    return logs_dir / "MonolithCalls.jsonl"


def _init_call_log() -> None:
    """Open the call-log file handle once at startup. Default-enabled."""
    global _call_log_enabled, _call_log_handle

    _call_log_enabled = os.environ.get("MONOLITH_CALL_LOG", "1") != "0"
    if not _call_log_enabled:
        _log("Call log disabled (MONOLITH_CALL_LOG=0)")
        return

    try:
        path = _resolve_call_log_path()
        # Append-binary mode; OS handles end-of-file positioning. We flush after
        # each write so a crash leaves complete lines on disk.
        _call_log_handle = open(path, "ab")
        _log(f"Call log: {path}")
    except OSError as e:
        _log(f"Failed to open call log: {e} -- logging disabled")
        _call_log_enabled = False
        _call_log_handle = None


def _canonical_json(value) -> str:
    """sort_keys + tightest separators -- matches the cpp proxy."""
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=False)


def _extract_namespace_action(msg: dict) -> tuple[str, str]:
    """Mirror the cpp proxy's extraction logic for (namespace, action)."""
    method = msg.get("method", "") or ""
    if method != "tools/call":
        return method, ""

    params = msg.get("params") or {}
    if not isinstance(params, dict):
        return "tools/call", ""

    name = params.get("name", "") or ""
    if name.endswith("_query"):
        ns = name[: -len("_query")]
        args = params.get("arguments") or {}
        action = args.get("action", "") if isinstance(args, dict) else ""
        return ns, action or ""

    if name.startswith("monolith_"):
        return "monolith", name[len("monolith_"):]

    return name, ""


def _extract_params_for_hash(msg: dict):
    """For tools/call, hash arguments; for other methods, hash the whole params."""
    method = msg.get("method", "") or ""
    params = msg.get("params") or {}
    if not isinstance(params, dict):
        return {}

    if method == "tools/call":
        args = params.get("arguments") or {}
        return args if isinstance(args, dict) else {}
    return params


def _inspect_response(resp: str | None) -> tuple[bool, int | None, int]:
    """Returns (ok, error_code, result_bytes)."""
    if not resp:
        return False, None, 0
    try:
        parsed = json.loads(resp)
    except (json.JSONDecodeError, ValueError):
        return False, None, len(resp.encode("utf-8")) if resp else 0

    error = parsed.get("error") if isinstance(parsed, dict) else None
    if isinstance(error, dict):
        code = error.get("code")
        error_code = int(code) if isinstance(code, int) else None
        result = parsed.get("result")
        if result is not None:
            result_bytes = len(_canonical_json(result).encode("utf-8"))
        else:
            result_bytes = len(resp.encode("utf-8"))
        return False, error_code, result_bytes

    result = parsed.get("result") if isinstance(parsed, dict) else None
    if result is not None:
        result_bytes = len(_canonical_json(result).encode("utf-8"))
    else:
        result_bytes = len(resp.encode("utf-8"))
    return True, None, result_bytes


def _write_call_log_line(msg: dict, resp: str | None, duration_ms: float) -> None:
    """Append one JSONL line describing an upstream HTTP roundtrip."""
    if not _call_log_enabled or _call_log_handle is None:
        return
    try:
        ns, action = _extract_namespace_action(msg)
        params_for_hash = _extract_params_for_hash(msg)
        canonical = _canonical_json(params_for_hash)
        params_hash = hashlib.sha1(canonical.encode("utf-8")).hexdigest()

        ok, error_code, result_bytes = _inspect_response(resp)

        # Second-precision ISO-8601 UTC (matches cpp proxy)
        ts = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")

        line = {
            "ts": ts,
            "namespace": ns,
            "action": action,
            "params_hash": params_hash,
            "duration_ms": round(duration_ms, 3),
            "ok": ok,
            "error_code": error_code,
            "result_bytes": result_bytes,
        }
        payload = (json.dumps(line, separators=(",", ":")) + "\n").encode("utf-8")
        with _call_log_lock:
            _call_log_handle.write(payload)
            _call_log_handle.flush()
    except Exception as e:
        # Never let logging crash the proxy.
        _log(f"Call-log write failed: {e}")


def _post_monolith(body: str, timeout: float = TIMEOUT) -> str | None:
    """POST JSON-RPC to Monolith. Returns response body or None on failure."""
    global _last_post_timed_out
    _last_post_timed_out = False
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
        reason = getattr(e, "reason", e)
        if isinstance(e, (TimeoutError, socket.timeout)) or isinstance(reason, (TimeoutError, socket.timeout)):
            _last_post_timed_out = True
            _log(f"Monolith request timed out after {timeout:g}s: {e}")
        else:
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
            "_fields": {
                "type": "array",
                "items": {"type": "string"},
                "description": "Optional top-level whitelist — return only these top-level fields of the response. Mutually exclusive with _omit.",
            },
            "_omit": {
                "type": "array",
                "items": {"type": "string"},
                "description": "Optional top-level blacklist — remove these top-level fields from the response. Mutually exclusive with _fields.",
            },
            "_compact_json": {
                "type": "boolean",
                "description": "Optional — when true, drop top-level fields whose value is null, empty string, empty array, or empty object.",
            },
        },
        "required": ["action"],
    }


def _empty_object_schema() -> dict:
    return {
        "type": "object",
        "properties": {
            "_fields": {
                "type": "array",
                "items": {"type": "string"},
                "description": "Optional top-level whitelist — return only these top-level fields of the response. Mutually exclusive with _omit.",
            },
            "_omit": {
                "type": "array",
                "items": {"type": "string"},
                "description": "Optional top-level blacklist — remove these top-level fields from the response. Mutually exclusive with _fields.",
            },
            "_compact_json": {
                "type": "boolean",
                "description": "Optional — when true, drop top-level fields whose value is null, empty string, empty array, or empty object.",
            },
        },
    }


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
                "_fields": {
                    "type": "array",
                    "items": {"type": "string"},
                    "description": "Optional top-level whitelist — return only these top-level fields of the response. Mutually exclusive with _omit.",
                },
                "_omit": {
                    "type": "array",
                    "items": {"type": "string"},
                    "description": "Optional top-level blacklist — remove these top-level fields from the response. Mutually exclusive with _fields.",
                },
                "_compact_json": {
                    "type": "boolean",
                    "description": "Optional — when true, drop top-level fields whose value is null, empty string, empty array, or empty object.",
                },
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
                },
                "_fields": {
                    "type": "array",
                    "items": {"type": "string"},
                    "description": "Optional top-level whitelist — return only these top-level fields of the response. Mutually exclusive with _omit.",
                },
                "_omit": {
                    "type": "array",
                    "items": {"type": "string"},
                    "description": "Optional top-level blacklist — remove these top-level fields from the response. Mutually exclusive with _fields.",
                },
                "_compact_json": {
                    "type": "boolean",
                    "description": "Optional — when true, drop top-level fields whose value is null, empty string, empty array, or empty object.",
                },
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
    t0 = time.perf_counter()
    resp = _post_monolith(json.dumps(msg))
    duration_ms = (time.perf_counter() - t0) * 1000.0
    _write_call_log_line(msg, resp, duration_ms)

    if resp:
        _write_tools_cache(resp)
        return resp
    return _fallback_tools_list(msg)


def handle_tools_call(msg: dict) -> str:
    """Forward tools/call to Monolith. Graceful error if down."""
    t0 = time.perf_counter()
    resp = _post_monolith(json.dumps(msg))
    duration_ms = (time.perf_counter() - t0) * 1000.0
    _write_call_log_line(msg, resp, duration_ms)

    if resp:
        return resp
    tool_name = msg.get("params", {}).get("name", "unknown")
    if _last_post_timed_out:
        return _tool_error(
            msg.get("id"),
            f"Monolith request for tool '{tool_name}' timed out after {TIMEOUT:g}s. "
            f"The Unreal Editor is still running and may have completed the action -- "
            f"verify the result before retrying. Raise the limit with MONOLITH_PROXY_TIMEOUT_S.",
        )
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

    _init_call_log()

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
            t0 = time.perf_counter()
            resp = _post_monolith(json.dumps(msg))
            duration_ms = (time.perf_counter() - t0) * 1000.0
            _write_call_log_line(msg, resp, duration_ms)

            if resp:
                response = resp
            elif msg_id is not None:
                response = _jsonrpc_error(msg_id, -32601, f"Method not found: {method}")

        if response:
            _write(stdout, response)


if __name__ == "__main__":
    main()
