#!/usr/bin/env python3
"""
Stdlib-only offline DEV FALLBACK for the canonical monolith_query.exe.
  Kept byte-identical to the exe by Scripts/verify_offline_parity.py.
  Serves source / project / monolith subcommands plus all 20 Reflection-Intelligence
  actions (cppreflect, network, decision, risk), read-only against the on-disk SQLite.
  No UE installation, no build, no editor required.

Monolith Offline CLI — query EngineSource.db and ProjectIndex.db without the editor.

Usage:
    python monolith_offline.py source <action> [params...]
    python monolith_offline.py project <action> [params...]
    python monolith_offline.py --version

Source actions:
    search_source <query> [--scope all|cpp|shaders] [--limit N] [--module M] [--kind K]
    read_source <symbol> [--header] [--max-lines N] [--members-only]
    find_references <symbol> [--ref-kind K] [--limit N]
    find_callers <symbol> [--limit N]
    find_callees <symbol> [--limit N]
    get_class_hierarchy <symbol> [--direction up|down|both] [--depth N]
    get_module_info <module_name>
    get_symbol_context <symbol> [--context-lines N]
    read_file <file_path> [--start N] [--end N]

Project actions:
    search <query> [--limit N]
    find_by_type <asset_class> [--limit N] [--offset N]
    find_references <asset_path>
    get_stats
    get_asset_details <asset_path>

Reflection Intelligence actions (read EngineSource.db reflect_* / risk_* / decision_* tables).
FULL offline parity with the live RI adapters (Source/MonolithReflectionIntel/Private/
{CppReflect,Network,Decision,Risk}/F*QueryAdapter.cpp) per Docs/plans/offline-parity-spec.md.

    cppreflect get_uclass <class_name> [--module_name M]
    cppreflect list_uproperties [class_name] [--blueprint_visible_only] [--limit N] [--cursor B64]
    cppreflect list_ufunctions [class_name] [--blueprint_callable_only] [--limit N] [--cursor B64]
    cppreflect find_interface_impls <interface_name>
    cppreflect find_class_specifier <specifier_name> [--limit N] [--cursor B64]
    cppreflect list_class_specifiers
    network    list_replicated_classes [--limit N] [--cursor B64]
    network    list_rpc_functions [--class_name C] [--rpc_kind Server|Client|Multicast] [--limit N] [--cursor B64]
    network    list_onrep_handlers [--class_name C] [--limit N] [--cursor B64]
    network    audit_unbalanced_onreps [--limit N] [--cursor B64]
    decision   list_decisions [--path_filter P] [--min_confidence F] [--status S] [--limit N] [--cursor B64]
    decision   get_decision <decision_id>
    decision   list_stale [max_age_days] [--path_filter P] [--limit N] [--cursor B64]
    decision   find_supersession_chain <decision_id> [--depth N]
    decision   find_referent_decisions <decision_id>
    risk       get_hotspot_score <file_path>
    risk       get_cochange_pairs <file_path> [--limit N] [--cursor B64]
    risk       get_file_churn <file_path> [--repo_tag R]
    risk       get_release_window_hotspots [--since_unix T] [--limit N] [--cursor B64]
    risk       list_conditional_gates [--macro_filter M] [--path_filter P] [--limit N] [--cursor B64]

NOTE: Only namespaces backed by on-disk SQLite are servable offline (source,
project, and the read-side RI namespaces above). The live MCP server exposes
~29 namespaces; the rest require a running editor + UObject reflection and CANNOT
be served by this tool. `monolith guide` stays exe-only (it depends on runtime
state the Python fallback does not reconstruct).
"""

import sys
import os
import re
import json
import base64
import time
import argparse
from pathlib import Path

# Parity revision string. MUST match the C++ exe sibling (Tools/MonolithQuery)
# so the HARD-GATE parity guard can assert both report the same rev.
PARITY_SPEC_REV = "2026-05-29.1"

# --- Database paths ---
SCRIPT_DIR = Path(__file__).parent


def _resolve_db(name: str) -> Path:
    candidates = [
        SCRIPT_DIR.parent / "Saved" / name,  # Scripts/ -> ../Saved/
        SCRIPT_DIR / name,                    # legacy: co-located in Saved/
    ]
    for c in candidates:
        if c.exists():
            return c
    return candidates[0]


SOURCE_DB = _resolve_db("EngineSource.db")
PROJECT_DB = _resolve_db("ProjectIndex.db")


def open_db(path: Path):
    import sqlite3
    if not path.exists():
        print(f"ERROR: Database not found: {path}", file=sys.stderr)
        sys.exit(1)
    conn = sqlite3.connect(str(path))
    conn.row_factory = sqlite3.Row
    conn.execute("PRAGMA journal_mode=DELETE;")
    conn.execute("PRAGMA query_only=ON;")
    return conn


def escape_fts(query: str) -> str:
    q = query.replace("::", " ")
    cleaned = re.sub(r'[^\w\s]', '', q)
    tokens = cleaned.split()
    if not tokens:
        return '""'
    return " ".join(f'"{t}"*' for t in tokens)


# ============================================================
# UE hash replication — produce byte-identical cursors to the live
# C++ adapters + the C++ exe sibling. The HARD-GATE parity test diffs the
# response JSON (which embeds `next_cursor`), so the FilterHash that goes into
# the cursor MUST match UE's GetTypeHash(FString)/GetTypeHash(double)/HashCombine
# exactly, not merely be self-consistent.
#
#   GetTypeHash(const FString&) -> FCrc::StrCrc32(*S)   (case-sensitive, verbatim)
#   GetTypeHash(double)         -> GetTypeHash(*(uint64*)&d) -> (uint32)v + ((uint32)(v>>32))*23
#   HashCombine(A, C)           -> the full Bob-Jenkins mix (TypeHash.h:36-52), NOT HashCombineFast
# ============================================================

_MASK32 = 0xFFFFFFFF


def _build_strihash_table():
    """== FCrc::CRCTable_DEPRECATED[256] (Crc.cpp:40). Forward (non-reflected)
    CRC-32, poly 0x04C11DB7, MSB-first. t[0]=0, t[1]=0x04C11DB7, t[255]=0xB1F740B4."""
    t = [0] * 256
    for i in range(256):
        c = (i << 24) & _MASK32
        for _ in range(8):
            c = ((c << 1) ^ 0x04C11DB7) & _MASK32 if (c & 0x80000000) else (c << 1) & _MASK32
        t[i] = c
    return t


_STRIHASH_T = _build_strihash_table()


def _get_type_hash_str(s: str) -> int:
    """GetTypeHash(const FString&) == FCrc::Strihash_DEPRECATED(Len, *S) (Crc.h:195).
    Case-insensitive (ToUpper per WIDECHAR), forward CRC-32 table, each UTF-16 code
    unit processed lo-byte then hi-byte. Empty string -> 0."""
    h = 0
    for ch in s:
        cu = ord(ch.upper()) & 0xFFFF  # WIDECHAR ToUpper (BMP); FString is UTF-16
        b = cu & 0xFF
        h = ((h >> 8) & 0x00FFFFFF) ^ _STRIHASH_T[(h ^ b) & 0xFF]
        b = (cu >> 8) & 0xFF
        h = ((h >> 8) & 0x00FFFFFF) ^ _STRIHASH_T[(h ^ b) & 0xFF]
    return h & _MASK32


def _get_type_hash_double(d: float) -> int:
    import struct
    v = struct.unpack("<Q", struct.pack("<d", float(d)))[0]  # bit-reinterpret double->uint64
    return ((v & _MASK32) + (((v >> 32) & _MASK32) * 23)) & _MASK32


def _hash_combine(a: int, c: int) -> int:
    """UE HashCombine (TypeHash.h:36-52) — the full mixing variant. uint32 math."""
    a &= _MASK32
    c &= _MASK32
    b = 0x9e3779b9
    a = (a + b) & _MASK32

    a = (a - b) & _MASK32; a = (a - c) & _MASK32; a ^= (c >> 13); a &= _MASK32
    b = (b - c) & _MASK32; b = (b - a) & _MASK32; b ^= ((a << 8) & _MASK32); b &= _MASK32
    c = (c - a) & _MASK32; c = (c - b) & _MASK32; c ^= (b >> 13); c &= _MASK32
    a = (a - b) & _MASK32; a = (a - c) & _MASK32; a ^= (c >> 12); a &= _MASK32
    b = (b - c) & _MASK32; b = (b - a) & _MASK32; b ^= ((a << 16) & _MASK32); b &= _MASK32
    c = (c - a) & _MASK32; c = (c - b) & _MASK32; c ^= (b >> 5); c &= _MASK32
    a = (a - b) & _MASK32; a = (a - c) & _MASK32; a ^= (c >> 3); a &= _MASK32
    b = (b - c) & _MASK32; b = (b - a) & _MASK32; b ^= ((a << 10) & _MASK32); b &= _MASK32
    c = (c - a) & _MASK32; c = (c - b) & _MASK32; c ^= (b >> 15); c &= _MASK32
    return c & _MASK32


def compute_filter_hash(parts) -> int:
    """ComputeFilterHash({...}) initializer-list form: H=0; for P: H=HashCombine(H, GetTypeHash(P))."""
    h = 0
    for p in parts:
        h = _hash_combine(h, _get_type_hash_str(p))
    return h


def compute_filter_hash_decision(path_filter: str, conf_or_age: float, status_or_tag: str) -> int:
    """Decision-adapter 3-arg form (spec NOTE): H=GetTypeHash(path); H=HashCombine(H, GetTypeHash(double));
    H=HashCombine(H, GetTypeHash(status))."""
    h = _get_type_hash_str(path_filter)
    h = _hash_combine(h, _get_type_hash_double(conf_or_age))
    h = _hash_combine(h, _get_type_hash_str(status_or_tag))
    return h


# ============================================================
# Cursor codec — base64(pretty JSON) matching UE's default TJsonWriterFactory<>
# (pretty-print policy = CRLF newlines + single-TAB indent + space after colon)
# + FBase64::Encode. Key order is the SetNumberField insertion order: qh, p, tc.
# Hand-built byte layout; do NOT use json.dumps formatting here.
# ============================================================

HARD_CAP = 200


def encode_cursor(query_hash: int, page: int, cached_total: int) -> str:
    # EXACT live bytes: {\r\n\t"qh": N,\r\n\t"p": N,\r\n\t"tc": N\r\n}
    body = (
        '{\r\n'
        f'\t"qh": {int(query_hash) & _MASK32},\r\n'
        f'\t"p": {int(page)},\r\n'
        f'\t"tc": {int(cached_total)}\r\n'
        '}'
    )
    return base64.b64encode(body.encode("ascii")).decode("ascii")


class CursorError(Exception):
    def __init__(self, reason: str):
        self.reason = reason


def decode_cursor(enc: str):
    """Returns (query_hash, page, cached_total). Raises CursorError on any failure."""
    if not enc:
        raise CursorError("Cursor decode failed; restart pagination without `cursor`.")
    try:
        raw = base64.b64decode(enc, validate=True)
        js = raw.decode("utf-8")
    except Exception:
        raise CursorError("Cursor decode failed; restart pagination without `cursor`.")
    try:
        o = json.loads(js)
    except Exception:
        raise CursorError("Cursor decode failed; restart pagination without `cursor`.")
    if not isinstance(o, dict):
        raise CursorError("Cursor decode failed; restart pagination without `cursor`.")
    if "qh" not in o or "p" not in o or "tc" not in o:
        raise CursorError("Cursor decode failed; restart pagination without `cursor`.")
    try:
        qh = float(o["qh"]); p = float(o["p"]); tc = float(o["tc"])
    except Exception:
        raise CursorError("Cursor decode failed; restart pagination without `cursor`.")
    if p < 0.0:
        raise CursorError("Cursor decode failed; restart pagination without `cursor`.")
    if qh < 0.0 or qh > float(_MASK32):
        raise CursorError("Cursor decode failed; restart pagination without `cursor`.")
    return (int(qh) & _MASK32, int(p), int(tc))


def resolve_page(cursor: str, expected_hash: int):
    """Decode cursor (if any), validate filter hash. Returns (page, had_cursor).
    page 0 + had_cursor False when no cursor supplied."""
    if not cursor:
        return (0, False)
    qh, page, _tc = decode_cursor(cursor)
    if qh != (expected_hash & _MASK32):
        raise CursorError("Cursor filter mismatch; restart pagination without `cursor`.")
    return (page, True)


def clamp_limit(limit) -> int:
    try:
        v = int(limit)
    except Exception:
        v = 50
    if v < 1:
        v = 1
    if v > HARD_CAP:
        v = HARD_CAP
    return v


# ============================================================
# Float sentinel — UE serializes every numeric JSON field via
# TJsonPrintPolicy::WriteDouble == FString::Printf("%.17g", Value) (JsonPrintPolicy.h:70).
# Python json.dumps emits floats via shortest-round-trip repr, which differs
# (0.6499999761581421 vs UE's 0.64999997615814209). We can't pass a per-float
# format to json.dumps, so we stash each fractional double as a unique sentinel
# string, let json.dumps quote it, then strip the quotes + sentinel markers,
# leaving the raw %.17g digits inline as a JSON number.
#
# REAL columns are stored float32 in the indexer; when UE reads them back into a
# C++ double they widen, so we float32 round-trip BEFORE %.17g to reproduce the
# widened value exactly.
# ============================================================

import struct

_FLT_OPEN = "\x01FLT:"
_FLT_CLOSE = "\x01"


def f32rt(v) -> float:
    """Widen a stored float32 to the double UE sees (struct round-trip)."""
    return struct.unpack("<f", struct.pack("<f", float(v)))[0]


def flt(v) -> str:
    """Wrap a fractional double field as a %.17g sentinel. Apply ONLY to the
    genuine-double surfaces the addendum lists (decision confidence; risk score /
    normalised_churn / normalised_complexity). Integer fields stay plain ints."""
    return _FLT_OPEN + ("%.17g" % float(v)) + _FLT_CLOSE


def _strip_float_sentinels(s: str) -> str:
    """Replace every quoted sentinel "\x01FLT:<digits>\x01" with raw <digits>."""
    out = []
    i = 0
    token = '"' + _FLT_OPEN
    n = len(s)
    while i < n:
        j = s.find(token, i)
        if j < 0:
            out.append(s[i:])
            break
        out.append(s[i:j])
        # digits start after the opening quote + marker
        ds = j + len(token)
        de = s.find(_FLT_CLOSE + '"', ds)
        # de is guaranteed: we control both ends; sentinel chars (\x01) are JSON-escaped
        # as  by json.dumps ONLY if they appear in real data — but our markers are
        # the LITERAL control char, which json.dumps escapes to . So match the
        # escaped form instead. (Handled below.)
        out.append(s[ds:de])
        i = de + len(_FLT_CLOSE) + 1
    return "".join(out)


def emit(obj):
    raw = json.dumps(obj, indent=2)
    # json.dumps escapes the \x01 control char to the 6-char sequence .
    # So the in-string sentinel becomes "FLT:<digits>".
    raw = _strip_float_sentinels_escaped(raw)
    print(raw)


def _strip_float_sentinels_escaped(s: str) -> str:
    open_marker = '"\\u0001FLT:'
    close_marker = '\\u0001"'
    out = []
    i = 0
    n = len(s)
    while i < n:
        j = s.find(open_marker, i)
        if j < 0:
            out.append(s[i:])
            break
        out.append(s[i:j])
        ds = j + len(open_marker)
        de = s.find(close_marker, ds)
        out.append(s[ds:de])
        i = de + len(close_marker)
    return "".join(out)


def _emit_raw(obj):
    """emit without sentinel stripping — for payloads with no float sentinels."""
    print(json.dumps(obj, indent=2))


def emit_error(message: str, error_code: str = None):
    out = {"success": False, "error": message}
    if error_code:
        out["error_code"] = error_code
    print(json.dumps(out, indent=2))


def coerce_str(v) -> str:
    """NULL TEXT -> '' to match UE FString extraction."""
    return "" if v is None else str(v)


# ============================================================
# Fuzzy match for did_you_mean
# ============================================================

def _levenshtein(a: str, b: str) -> int:
    if a == b:
        return 0
    if not a:
        return len(b)
    if not b:
        return len(a)
    prev = list(range(len(b) + 1))
    for i, ca in enumerate(a, 1):
        cur = [i]
        for j, cb in enumerate(b, 1):
            cur.append(min(prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (ca != cb)))
        prev = cur
    return prev[-1]


def fuzzy_top(needle: str, keys, top_n: int = 3):
    if not needle or not keys or top_n <= 0:
        return []
    scored = []
    for k in keys:
        dist = _levenshtein(needle, k)
        worst = max(len(needle), len(k)) or 1
        scored.append((1.0 - dist / worst, k))
    scored.sort(key=lambda t: -t[0])
    return [k for _, k in scored[:top_n]]


def did_you_mean_suffix(needle: str, keys) -> str:
    sugg = fuzzy_top(needle, list(keys), 3)
    if not sugg:
        return ""
    return " Did you mean: " + ", ".join(sugg) + "?"


# Per-namespace valid actions (used for unknown-action errors).
ACTIONS_BY_NS = {
    "cppreflect": ["get_uclass", "list_uproperties", "list_ufunctions",
                   "find_interface_impls", "find_class_specifier", "list_class_specifiers"],
    "network": ["list_replicated_classes", "list_rpc_functions", "list_onrep_handlers",
                "audit_unbalanced_onreps"],
    "decision": ["list_decisions", "get_decision", "list_stale",
                 "find_supersession_chain", "find_referent_decisions"],
    "risk": ["get_hotspot_score", "get_cochange_pairs", "get_file_churn",
             "get_release_window_hotspots", "list_conditional_gates"],
}

OFFLINE_NAMESPACES = ["source", "project", "cppreflect", "network", "decision", "risk"]


def canon_path(p: str) -> str:
    """CanonPath — backslash -> forward-slash ONLY (no lowercasing)."""
    return (p or "").replace("\\", "/")


# ============================================================
# Source actions (UNCHANGED — do not touch)
# ============================================================

class SourceActions:
    def __init__(self):
        self.db = open_db(SOURCE_DB)

    def get_file_path(self, file_id: int) -> str:
        row = self.db.execute("SELECT path FROM files WHERE id = ?", (file_id,)).fetchone()
        return row["path"] if row else "<unknown>"

    def short_path(self, full_path: str) -> str:
        markers = ["Engine\\Source\\", "Engine/Source/", "Engine\\Shaders\\", "Engine/Shaders/"]
        for m in markers:
            idx = full_path.find(m)
            if idx >= 0:
                return full_path[idx:]
        return full_path

    def read_file_lines(self, file_path: str, start: int, end: int) -> str:
        try:
            with open(file_path, "r", encoding="utf-8", errors="replace") as f:
                lines = f.readlines()
        except FileNotFoundError:
            return f"[File not found: {file_path}]"
        start = max(1, start)
        end = min(len(lines), end)
        result = []
        for i in range(start - 1, end):
            result.append(f"{i+1:5d} | {lines[i].rstrip()}")
        return "\n".join(result)

    def search_source(self, args):
        query = args.query
        limit = args.limit
        module = getattr(args, 'module', None) or ""
        kind = getattr(args, 'kind', None) or ""
        fts_q = escape_fts(query)

        parts = []

        sql = """SELECT s.id, s.name, s.qualified_name, s.kind, s.file_id, s.line_start,
                        s.line_end, s.access, s.signature, s.docstring
                 FROM symbols_fts f JOIN symbols s ON s.id = f.rowid"""
        conditions = ["symbols_fts MATCH ?"]
        params = [fts_q]

        if module:
            sql += " JOIN files fi ON fi.id = s.file_id JOIN modules m ON m.id = fi.module_id"
            conditions.append("m.name = ?")
            params.append(module)
        if kind:
            conditions.append("s.kind = ?")
            params.append(kind)

        sql += " WHERE " + " AND ".join(conditions)
        sql += f" ORDER BY bm25(symbols_fts) LIMIT {limit}"

        rows = self.db.execute(sql, params).fetchall()
        if rows:
            parts.append("=== Symbol Matches ===")
            for r in rows:
                fp = self.short_path(self.get_file_path(r["file_id"]))
                parts.append(f"  [{r['kind']}] {r['qualified_name']} ({fp}:{r['line_start']})")
                if r["signature"]:
                    parts.append(f"         {r['signature']}")

        src_sql = """SELECT sf.file_id, sf.line_number, sf.text
                     FROM source_fts sf"""
        src_conditions = ["source_fts MATCH ?"]
        src_params = [fts_q]

        if module:
            src_sql += " JOIN files fi ON fi.id = sf.file_id JOIN modules m ON m.id = fi.module_id"
            src_conditions.append("m.name = ?")
            src_params.append(module)

        src_sql += " WHERE " + " AND ".join(src_conditions)
        src_sql += f" ORDER BY bm25(source_fts) LIMIT {limit}"

        src_rows = self.db.execute(src_sql, src_params).fetchall()
        if src_rows:
            parts.append("\n=== Source Line Matches ===")
            seen = set()
            for r in src_rows:
                key = (r["file_id"], r["line_number"])
                if key in seen:
                    continue
                seen.add(key)
                fp = self.short_path(self.get_file_path(r["file_id"]))
                text = r["text"].strip()[:120]
                parts.append(f"  {fp}:{r['line_number']}")
                parts.append(f"    {text}")

        print("\n".join(parts) if parts else f"No results found for '{query}'.")

    def read_source(self, args):
        symbol = args.symbol
        include_header = args.header
        max_lines = args.max_lines

        rows = self.db.execute(
            "SELECT * FROM symbols WHERE name = ? ORDER BY (line_end > line_start) DESC", (symbol,)
        ).fetchall()
        if not rows:
            fts_q = escape_fts(symbol)
            rows = self.db.execute(
                "SELECT s.* FROM symbols_fts f JOIN symbols s ON s.id = f.rowid WHERE symbols_fts MATCH ? ORDER BY bm25(symbols_fts) LIMIT 5",
                (fts_q,)
            ).fetchall()
        if not rows:
            print(f"No symbol found matching '{symbol}'.", file=sys.stderr)
            sys.exit(1)

        parts = []
        seen = set()
        for r in rows:
            key = (r["file_id"], r["line_start"], r["line_end"])
            if key in seen:
                continue
            seen.add(key)
            fp = self.get_file_path(r["file_id"])
            if not include_header and fp.endswith(".h"):
                continue
            header = f"--- {self.short_path(fp)} (lines {r['line_start']}-{r['line_end']}) ---"
            source = self.read_file_lines(fp, r["line_start"], r["line_end"])
            parts.append(f"{header}\n{source}")

        result = "\n\n".join(parts) if parts else f"Found symbol '{symbol}' but could not read source."
        if max_lines and max_lines > 0:
            lines = result.split("\n")
            if len(lines) > max_lines:
                remaining = len(lines) - max_lines
                result = "\n".join(lines[:max_lines]) + f"\n[...truncated, {remaining} more lines]"
        print(result)

    def find_references(self, args):
        symbol = args.symbol
        ref_kind = getattr(args, 'ref_kind', None) or ""
        limit = args.limit

        sym_rows = self.db.execute("SELECT id, name FROM symbols WHERE name = ?", (symbol,)).fetchall()
        if not sym_rows:
            fts_q = escape_fts(symbol)
            sym_rows = self.db.execute(
                "SELECT s.id, s.name FROM symbols_fts f JOIN symbols s ON s.id = f.rowid WHERE symbols_fts MATCH ? LIMIT 5",
                (fts_q,)
            ).fetchall()
        if not sym_rows:
            print(f"No symbol found matching '{symbol}'.", file=sys.stderr)
            sys.exit(1)

        lines = []
        for sym in sym_rows:
            if ref_kind:
                refs = self.db.execute(
                    """SELECT r.ref_kind, r.line, s.name as from_name, f.path
                       FROM "references" r JOIN symbols s ON s.id = r.from_symbol_id
                       JOIN files f ON f.id = r.file_id WHERE r.to_symbol_id = ? AND r.ref_kind = ? LIMIT ?""",
                    (sym["id"], ref_kind, limit)
                ).fetchall()
            else:
                refs = self.db.execute(
                    """SELECT r.ref_kind, r.line, s.name as from_name, f.path
                       FROM "references" r JOIN symbols s ON s.id = r.from_symbol_id
                       JOIN files f ON f.id = r.file_id WHERE r.to_symbol_id = ? LIMIT ?""",
                    (sym["id"], limit)
                ).fetchall()
            for ref in refs:
                lines.append(f"[{ref['ref_kind']}] {self.short_path(ref['path'])}:{ref['line']} (from {ref['from_name']})")

        print("\n".join(lines) if lines else f"No references found for '{symbol}'.")

    def find_callers(self, args):
        symbol = args.symbol
        limit = args.limit

        sym_rows = self.db.execute("SELECT id FROM symbols WHERE name = ? AND kind = 'function'", (symbol,)).fetchall()
        if not sym_rows:
            fts_q = escape_fts(symbol)
            sym_rows = self.db.execute(
                "SELECT s.id FROM symbols_fts f JOIN symbols s ON s.id = f.rowid WHERE symbols_fts MATCH ? AND s.kind = 'function' LIMIT 5",
                (fts_q,)
            ).fetchall()
        if not sym_rows:
            print(f"No function found matching '{symbol}'.", file=sys.stderr)
            sys.exit(1)

        lines = []
        for sym in sym_rows:
            refs = self.db.execute(
                """SELECT s.name as from_name, f.path, r.line
                   FROM "references" r JOIN symbols s ON s.id = r.from_symbol_id
                   JOIN files f ON f.id = r.file_id WHERE r.to_symbol_id = ? AND r.ref_kind = 'call' LIMIT ?""",
                (sym["id"], limit)
            ).fetchall()
            for ref in refs:
                lines.append(f"{ref['from_name']} -- {self.short_path(ref['path'])}:{ref['line']}")

        print("\n".join(lines) if lines else f"No callers found for '{symbol}'.")

    def find_callees(self, args):
        symbol = args.symbol
        limit = args.limit

        sym_rows = self.db.execute("SELECT id FROM symbols WHERE name = ? AND kind = 'function'", (symbol,)).fetchall()
        if not sym_rows:
            fts_q = escape_fts(symbol)
            sym_rows = self.db.execute(
                "SELECT s.id FROM symbols_fts f JOIN symbols s ON s.id = f.rowid WHERE symbols_fts MATCH ? AND s.kind = 'function' LIMIT 5",
                (fts_q,)
            ).fetchall()
        if not sym_rows:
            print(f"No function found matching '{symbol}'.", file=sys.stderr)
            sys.exit(1)

        lines = []
        for sym in sym_rows:
            refs = self.db.execute(
                """SELECT s.name as to_name, f.path, r.line
                   FROM "references" r JOIN symbols s ON s.id = r.to_symbol_id
                   JOIN files f ON f.id = r.file_id WHERE r.from_symbol_id = ? AND r.ref_kind = 'call' LIMIT ?""",
                (sym["id"], limit)
            ).fetchall()
            for ref in refs:
                lines.append(f"{ref['to_name']} -- {self.short_path(ref['path'])}:{ref['line']}")

        print("\n".join(lines) if lines else f"No callees found for '{symbol}'.")

    def get_class_hierarchy(self, args):
        symbol = args.symbol
        direction = args.direction
        depth = args.depth

        sym_rows = self.db.execute(
            "SELECT id, name, file_id FROM symbols WHERE name = ? AND kind IN ('class','struct') ORDER BY (line_end > line_start) DESC",
            (symbol,)
        ).fetchall()
        if not sym_rows:
            fts_q = escape_fts(symbol)
            sym_rows = self.db.execute(
                "SELECT s.id, s.name, s.file_id FROM symbols_fts f JOIN symbols s ON s.id = f.rowid WHERE symbols_fts MATCH ? AND s.kind IN ('class','struct') LIMIT 1",
                (fts_q,)
            ).fetchall()
        if not sym_rows:
            print(f"No class/struct found matching '{symbol}'.", file=sys.stderr)
            sys.exit(1)

        sym = sym_rows[0]
        fp = self.short_path(self.get_file_path(sym["file_id"]))
        lines = [f"{sym['name']} ({fp})"]

        visited = set()

        def walk_up(sid, indent, max_d):
            if indent > max_d or sid in visited:
                return
            visited.add(sid)
            parents = self.db.execute(
                "SELECT s.id, s.name FROM inheritance i JOIN symbols s ON s.id = i.parent_id WHERE i.child_id = ?",
                (sid,)
            ).fetchall()
            for p in parents:
                lines.append(f"{'  ' * indent}<- {p['name']}")
                walk_up(p["id"], indent + 1, max_d)

        def walk_down(sid, indent, max_d):
            if indent > max_d or sid in visited:
                return
            visited.add(sid)
            children = self.db.execute(
                "SELECT s.id, s.name FROM inheritance i JOIN symbols s ON s.id = i.child_id WHERE i.parent_id = ?",
                (sid,)
            ).fetchall()
            for c in children:
                lines.append(f"{'  ' * indent}-> {c['name']}")
                walk_down(c["id"], indent + 1, max_d)

        if direction in ("up", "both"):
            lines.append("\nAncestors:")
            count_before = len(lines)
            visited.clear()
            walk_up(sym["id"], 1, depth)
            if len(lines) == count_before:
                lines.append("  (none)")

        if direction in ("down", "both"):
            lines.append("\nDescendants:")
            count_before = len(lines)
            visited.clear()
            walk_down(sym["id"], 1, depth)
            if len(lines) == count_before:
                lines.append("  (none)")

        print("\n".join(lines))

    def get_module_info(self, args):
        module_name = args.module_name
        mod = self.db.execute("SELECT id, name, path, module_type FROM modules WHERE name = ?", (module_name,)).fetchone()
        if not mod:
            print(f"No module found matching '{module_name}'.", file=sys.stderr)
            sys.exit(1)

        file_count = self.db.execute("SELECT COUNT(*) as c FROM files WHERE module_id = ?", (mod["id"],)).fetchone()["c"]
        kind_rows = self.db.execute(
            "SELECT s.kind, COUNT(*) as cnt FROM symbols s JOIN files f ON f.id = s.file_id WHERE f.module_id = ? GROUP BY s.kind",
            (mod["id"],)
        ).fetchall()

        lines = [
            f"Module: {mod['name']}",
            f"Path: {self.short_path(mod['path'])}",
            f"Type: {mod['module_type']}",
            f"Files: {file_count}",
            "",
            "Symbol counts by kind:"
        ]
        for kr in sorted(kind_rows, key=lambda r: r["kind"]):
            lines.append(f"  {kr['kind']}: {kr['cnt']}")

        key_classes = self.db.execute(
            "SELECT s.name, s.line_start FROM symbols s JOIN files f ON f.id = s.file_id JOIN modules m ON m.id = f.module_id WHERE m.name = ? AND s.kind = 'class' LIMIT 20",
            (module_name,)
        ).fetchall()
        if key_classes:
            lines.extend(["", "Key classes:"])
            for c in key_classes:
                lines.append(f"  {c['name']} (line {c['line_start']})")

        print("\n".join(lines))

    def get_symbol_context(self, args):
        symbol = args.symbol
        ctx_lines = args.context_lines

        rows = self.db.execute("SELECT * FROM symbols WHERE name = ? ORDER BY (line_end > line_start) DESC", (symbol,)).fetchall()
        if not rows:
            fts_q = escape_fts(symbol)
            rows = self.db.execute(
                "SELECT s.* FROM symbols_fts f JOIN symbols s ON s.id = f.rowid WHERE symbols_fts MATCH ? LIMIT 5",
                (fts_q,)
            ).fetchall()
        if not rows:
            print(f"No symbol found matching '{symbol}'.", file=sys.stderr)
            sys.exit(1)

        parts = []
        for i, r in enumerate(rows):
            if i >= 3:
                break
            fp = self.get_file_path(r["file_id"])
            ctx_start = max(1, r["line_start"] - ctx_lines)
            ctx_end = r["line_end"] + ctx_lines

            header = f"--- {r['qualified_name']} ---"
            info = [f"File: {self.short_path(fp)} (lines {r['line_start']}-{r['line_end']})"]
            if r["signature"]:
                info.append(f"Signature: {r['signature']}")
            if r["docstring"]:
                info.append(f"Docstring: {r['docstring']}")
            source = self.read_file_lines(fp, ctx_start, ctx_end)
            parts.append(f"{header}\n" + "\n".join(info) + f"\n\n{source}")

        print("\n\n".join(parts))

    def read_file(self, args):
        file_path = args.file_path
        start = args.start
        end = args.end

        resolved = None
        if os.path.isfile(file_path):
            resolved = file_path
        else:
            normalized = file_path.replace("/", "\\")
            row = self.db.execute("SELECT path FROM files WHERE path = ?", (normalized,)).fetchone()
            if row:
                resolved = row["path"]
            else:
                row = self.db.execute("SELECT path FROM files WHERE path LIKE ? LIMIT 1", (f"%{normalized}",)).fetchone()
                if row:
                    resolved = row["path"]

        if not resolved:
            print(f"No file found matching '{file_path}'.", file=sys.stderr)
            sys.exit(1)

        if end <= 0:
            end = start + 199

        header = f"--- {self.short_path(resolved)} (lines {start}-{end}) ---"
        source = self.read_file_lines(resolved, start, end)
        print(f"{header}\n{source}")

    # ============================================================
    # Phase 1 — LLM C++ authoring ergonomics (items 1-3). Byte-equivalent to the
    # monolith_query.exe SourceActions methods (parity-gated).
    # ============================================================

    @staticmethod
    def derive_include_path(indexed_path, module_name):
        """Mirror of FMonolithSourceActions::DeriveIncludePath.
        Returns (include, includable, warning)."""
        path = (indexed_path or "").replace("\\", "/")
        for root in ("/Public/", "/Classes/", "/Internal/"):
            idx = path.rfind(root)
            if idx >= 0:
                return path[idx + len(root):], True, ""
        pidx = path.rfind("/Private/")
        if pidx >= 0:
            warn = ("Private header -- not includable outside "
                    + (module_name if module_name else "its module")
                    + "; same-module include shown")
            return path[pidx + len("/Private/"):], False, warn
        slash = path.rfind("/")
        return (path if slash < 0 else path[slash + 1:]), True, ""

    def resolve_symbol_row(self, symbol):
        lookup = symbol
        scope = symbol.rfind("::")
        if scope >= 0:
            lookup = symbol[:scope]
        rows = self.db.execute(
            "SELECT id, name, file_id FROM symbols WHERE name = ? ORDER BY (line_end > line_start) DESC",
            (lookup,)).fetchall()
        if not rows:
            fts_q = escape_fts(lookup)
            rows = self.db.execute(
                "SELECT s.id, s.name, s.file_id FROM symbols_fts f JOIN symbols s ON s.id = f.rowid "
                "WHERE symbols_fts MATCH ? LIMIT 5", (fts_q,)).fetchall()
        return rows[0] if rows else None

    def get_include_path(self, args):
        symbol = args.symbol
        sym = self.resolve_symbol_row(symbol)
        if not sym:
            print(f"No symbol found matching '{symbol}'.", file=sys.stderr)
            sys.exit(1)

        file_id = sym["file_id"]
        lookup = symbol
        scope = symbol.rfind("::")
        if scope >= 0:
            lookup = symbol[:scope]
        allrows = self.db.execute(
            "SELECT s.file_id, f.path FROM symbols s JOIN files f ON f.id = s.file_id WHERE s.name = ?",
            (lookup,)).fetchall()
        file_path = self.get_file_path(file_id)
        for r in allrows:
            p = r["path"]
            if p.endswith(".h"):
                file_id = r["file_id"]
                file_path = p
                break

        mrows = self.db.execute(
            "SELECT m.name, m.build_cs_path FROM files f JOIN modules m ON m.id = f.module_id WHERE f.id = ?",
            (file_id,)).fetchall()
        module_name = mrows[0]["name"] if mrows else ""
        build_cs = mrows[0]["build_cs_path"] if mrows else ""

        include, includable, warning = self.derive_include_path(file_path, module_name)

        build_cs_note = ""
        if module_name:
            if build_cs:
                base = build_cs.replace("\\", "/").rsplit("/", 1)[-1]
                build_cs_note = f"Module '{module_name}' -- add to your Build.cs deps ({base})"
            else:
                build_cs_note = f"Module '{module_name}' -- add to your Build.cs deps"

        out = [f'#include "{include}"']
        if module_name:
            out.append(f"Module: {module_name}")
        if build_cs_note:
            out.append(build_cs_note)
        if warning:
            out.append(f"WARNING: {warning}")
        print("\n".join(out))

    @staticmethod
    def compact_declaration(lines, start_idx):
        """Mirror of FMonolithSourceActions::CompactDeclaration."""
        accum = []
        paren_depth = 0
        saw_open = False
        for i in range(start_idx, min(len(lines), start_idx + 12)):
            line = lines[i].rstrip()
            if line.endswith("\\"):
                line = line[:-1].rstrip()
            done = False
            for c, ch in enumerate(line):
                if ch == "(":
                    paren_depth += 1
                    saw_open = True
                elif ch == ")":
                    paren_depth = max(0, paren_depth - 1)
                elif paren_depth == 0 and saw_open and ch in ("{", ";"):
                    # Prefix already accumulated char-by-char above; just stop
                    # (re-appending line[:c] duplicated the tail).
                    done = True
                    break
                else:
                    accum.append(ch)
                    continue
                accum.append(ch)
            if done:
                break
            accum.append(" ")
        # collapse whitespace
        raw = "".join(accum)
        out = []
        prev_space = False
        for ch in raw:
            if ch in (" ", "\t", "\r", "\n"):
                if not prev_space:
                    out.append(" ")
                prev_space = True
            else:
                out.append(ch)
                prev_space = False
        return "".join(out).strip()

    def get_signature(self, args):
        symbol = args.symbol
        limit = args.limit
        method = symbol
        scope = symbol.rfind("::")
        if scope >= 0:
            method = symbol[scope + 2:]

        overloads = []  # (sig, source, file, line)

        fnrows = self.db.execute(
            "SELECT signature, file_id, line_start FROM symbols WHERE name = ? AND kind = 'function'",
            (method,)).fetchall()
        for r in fnrows:
            if len(overloads) >= limit:
                break
            sig = r["signature"] or ""
            if not sig:
                continue
            if "{" in sig or "\\" in sig:
                continue
            sig = sig.strip()
            overloads.append((sig, "column", self.short_path(self.get_file_path(r["file_id"])), r["line_start"]))

        if not overloads:
            fts_q = escape_fts(symbol)
            chunks = self.db.execute(
                "SELECT file_id, line_number, text FROM source_fts WHERE source_fts MATCH ? "
                "ORDER BY bm25(source_fts) LIMIT 50", (fts_q,)).fetchall()
            seen = set()
            needle = method + "("
            for ch in chunks:
                if len(overloads) >= limit:
                    break
                fp = self.get_file_path(ch["file_id"])
                try:
                    with open(fp, "r", encoding="utf-8", errors="replace") as f:
                        file_lines = [ln.rstrip("\n") for ln in f.readlines()]
                except FileNotFoundError:
                    continue
                win_start = max(0, ch["line_number"] - 1)
                win_end = min(len(file_lines), win_start + 10)
                for i in range(win_start, win_end):
                    if len(overloads) >= limit:
                        break
                    line = file_lines[i]
                    didx = line.find(needle)
                    if didx < 0:
                        continue
                    if didx > 0:
                        prev = line[didx - 1]
                        if prev.isalnum() or prev == "_":
                            continue
                    sig = self.compact_declaration(file_lines, i)
                    if not sig or needle not in sig:
                        continue
                    if sig in seen:
                        continue
                    seen.add(sig)
                    overloads.append((sig, "declaration_read", self.short_path(fp), i + 1))

        if not overloads:
            print(f"No signature found for '{symbol}'.", file=sys.stderr)
            sys.exit(1)

        out = []
        for sig, source, fp, line in overloads:
            out.append(f"{sig}\n  // {source} @ {fp}:{line}")
        print("\n".join(out))

    def check_deprecations(self, args):
        symbols = args.symbols
        total = self.db.execute("SELECT COUNT(*) as c FROM symbol_deprecations").fetchone()["c"]
        if total == 0:
            print("Deprecation index is empty (schema v2 landed but not yet populated). "
                  "Run source.trigger_reindex to populate it.")
            return
        lines = []
        for name in symbols:
            row = self.db.execute(
                "SELECT version, message, kind FROM symbol_deprecations WHERE symbol_name = ? LIMIT 1",
                (name,)).fetchone()
            if row:
                lines.append(f"{name}: DEPRECATED ({row['version']}) [{row['kind']}] {row['message']}")
            else:
                lines.append(f"{name}: not deprecated")
        print("\n".join(lines))

    # --- shared composition helpers (mirror FMonolithSourceActions) ---

    def symbol_exists(self, symbol):
        lookup = symbol
        scope = symbol.rfind("::")
        if scope >= 0:
            lookup = symbol[:scope]
        if self.db.execute("SELECT id FROM symbols WHERE name = ? LIMIT 1", (lookup,)).fetchone():
            return True
        fts_q = escape_fts(lookup)
        if self.db.execute("SELECT s.id FROM symbols_fts f JOIN symbols s ON s.id = f.rowid "
                           "WHERE symbols_fts MATCH ? LIMIT 1", (fts_q,)).fetchone():
            return True
        method = symbol if scope < 0 else symbol[scope + 2:]
        needle = method + "("
        sfts = escape_fts(symbol)
        chunks = self.db.execute(
            "SELECT file_id, line_number FROM source_fts WHERE source_fts MATCH ? "
            "ORDER BY bm25(source_fts) LIMIT 25", (sfts,)).fetchall()
        for ch in chunks:
            try:
                with open(self.get_file_path(ch["file_id"]), "r", encoding="utf-8", errors="replace") as f:
                    fl = [ln.rstrip("\n").rstrip("\r") for ln in f.readlines()]
            except FileNotFoundError:
                continue
            ws = max(0, ch["line_number"] - 1)
            we = min(len(fl), ws + 10)
            for i in range(ws, we):
                di = fl[i].find(needle)
                if di < 0:
                    continue
                if di > 0 and (fl[i][di - 1].isalnum() or fl[i][di - 1] == "_"):
                    continue
                return True
        return False

    def first_signature(self, symbol):
        method = symbol
        scope = symbol.rfind("::")
        if scope >= 0:
            method = symbol[scope + 2:]
        for r in self.db.execute(
                "SELECT signature, file_id, line_start FROM symbols WHERE name = ? AND kind = 'function'",
                (method,)).fetchall():
            sig = r["signature"] or ""
            if not sig or "{" in sig or "\\" in sig:
                continue
            return sig.strip(), "column"
        fts_q = escape_fts(symbol)
        chunks = self.db.execute(
            "SELECT file_id, line_number FROM source_fts WHERE source_fts MATCH ? "
            "ORDER BY bm25(source_fts) LIMIT 50", (fts_q,)).fetchall()
        needle = method + "("
        for ch in chunks:
            try:
                with open(self.get_file_path(ch["file_id"]), "r", encoding="utf-8", errors="replace") as f:
                    fl = [ln.rstrip("\n") for ln in f.readlines()]
            except FileNotFoundError:
                continue
            ws = max(0, ch["line_number"] - 1)
            we = min(len(fl), ws + 10)
            for i in range(ws, we):
                di = fl[i].find(needle)
                if di < 0:
                    continue
                if di > 0 and (fl[i][di - 1].isalnum() or fl[i][di - 1] == "_"):
                    continue
                sig = self.compact_declaration(fl, i)
                if not sig or needle not in sig:
                    continue
                return sig, "declaration_read"
        return None, None

    def verify_symbols(self, args):
        symbols = args.symbols
        cnt = self.db.execute("SELECT COUNT(*) as c FROM symbol_deprecations").fetchone()["c"]
        dep_empty = cnt == 0

        lines = []
        for symbol in symbols:
            if not self.symbol_exists(symbol):
                lines.append(f"{symbol}: NOT FOUND")
                continue
            line = f"{symbol}: exists"

            sym = self.resolve_symbol_row(symbol)
            include = module_name = warning = ""
            includable = True
            if sym:
                lookup = symbol
                scope = symbol.rfind("::")
                if scope >= 0:
                    lookup = symbol[:scope]
                allrows = self.db.execute(
                    "SELECT s.file_id, f.path FROM symbols s JOIN files f ON f.id = s.file_id WHERE s.name = ?",
                    (lookup,)).fetchall()
                file_id = sym["file_id"]
                file_path = self.get_file_path(file_id)
                for r in allrows:
                    if r["path"].endswith(".h"):
                        file_id = r["file_id"]
                        file_path = r["path"]
                        break
                mrows = self.db.execute(
                    "SELECT m.name FROM files f JOIN modules m ON m.id = f.module_id WHERE f.id = ?",
                    (file_id,)).fetchall()
                module_name = mrows[0]["name"] if mrows else ""
                include, includable, warning = self.derive_include_path(file_path, module_name)
            if include:
                line += f' | #include "{include}"' + ("" if includable else " (NOT includable)")

            sig, _src = self.first_signature(symbol)
            if sig:
                line += f" | {sig}"

            if not dep_empty:
                method = symbol
                scope = symbol.rfind("::")
                if scope >= 0:
                    method = symbol[scope + 2:]
                drow = self.db.execute(
                    "SELECT version FROM symbol_deprecations WHERE symbol_name = ? LIMIT 1", (method,)).fetchone()
                if drow:
                    line += " | DEPRECATED"
            lines.append(line)
        print("\n".join(lines))

    def find_example_usage(self, args):
        symbol = args.symbol
        prefer = (getattr(args, "prefer", None) or "engine").lower()
        prefer_project = prefer == "project"
        limit = max(1, args.limit)
        HARD_CAP, FTS_FETCH = 500, 400

        method = symbol
        scope = symbol.rfind("::")
        if scope >= 0:
            method = symbol[scope + 2:]
        needle = method + "("

        usages = []  # (file, line, context, rank, sort_path)
        seen = set()
        fts_q = escape_fts(symbol)
        chunks = self.db.execute(
            "SELECT file_id, line_number FROM source_fts WHERE source_fts MATCH ? "
            "ORDER BY bm25(source_fts) LIMIT ?", (fts_q, FTS_FETCH)).fetchall()
        for ch in chunks:
            fp = self.get_file_path(ch["file_id"])
            try:
                with open(fp, "r", encoding="utf-8", errors="replace") as f:
                    fl = [ln.rstrip("\n").rstrip("\r") for ln in f.readlines()]
            except FileNotFoundError:
                continue
            ws = max(0, ch["line_number"] - 1)
            we = min(len(fl), ws + 10)
            for i in range(ws, we):
                hi = fl[i].find(needle)
                if hi < 0:
                    continue
                if hi > 0 and (fl[i][hi - 1].isalnum() or fl[i][hi - 1] == "_"):
                    continue
                key = f"{ch['file_id']}_{i + 1}"
                if key in seen:
                    continue
                seen.add(key)
                cs, ce = max(0, i - 3), min(len(fl) - 1, i + 3)
                ctx = "\n".join(f"{c+1:5d} | {fl[c]}" for c in range(cs, ce + 1))
                norm = fp.replace("\\", "/")
                is_engine = "Engine/Source/" in norm
                is_runtime = "/Source/Runtime/" in norm
                rank = 0 if (is_engine and is_runtime) else (1 if is_engine else 2)
                usages.append((self.short_path(fp), i + 1, ctx, rank, fp))

        def rank_key(u):
            if not prefer_project:
                return u[3]
            return {2: 0, 0: 1}.get(u[3], 2)

        usages.sort(key=lambda u: (rank_key(u), u[4], u[1]))

        total = len(usages)
        slice_end = min(min(limit, total), HARD_CAP)
        if total == 0:
            print(f"No call-site examples found for '{symbol}'.")
            return
        # PARITY: emit ONLY the rendered snippets (no total_estimate / next_cursor).
        # Those are structured-JSON fields the live handler sets on its result
        # object, not on content[].text. The text byte-compare runs against the
        # live content[].text, which likewise omits both. Do NOT add them here.
        parts = []
        for i in range(slice_end):
            f, ln, ctx, _r, _sp = usages[i]
            parts.append(f"--- {f}:{ln} ---\n{ctx}")
        print("\n\n".join(parts))

    # ------------------------------------------------------------------
    # item 7: lint_header — mirror of FMonolithSourceActions::LintHeaderLines
    # with an ALWAYS-EMPTY specifier vocabulary (the offline tool cannot reach
    # the RI registry; the invalid-specifier rule is always skipped, matching
    # the live "degrade gracefully when RI unavailable" path). Text output is
    # byte-identical to the live content[].text.
    # ------------------------------------------------------------------
    @staticmethod
    def _derive_module_from_path(file_path):
        p = (file_path or "").replace("\\", "/")
        src = p.rfind("/Source/")
        if src < 0:
            return ""
        after = p[src + len("/Source/"):]
        slash = after.find("/")
        return "" if slash < 0 else after[:slash]

    @staticmethod
    def _strip_block_comments(line, state):
        """state is a 1-element list [bool] threading the in-block flag."""
        result = []
        work = line
        while True:
            if state[0]:
                end = work.find("*/")
                if end < 0:
                    return "".join(result)
                work = work[end + 2:]
                state[0] = False
            open_idx = work.find("/*")
            if open_idx < 0:
                result.append(work)
                return "".join(result)
            result.append(work[:open_idx])
            work = work[open_idx + 2:]
            state[0] = True

    @staticmethod
    def _strip_line_comment(line):
        lc = line.find("//")
        return line if lc < 0 else line[:lc]

    def lint_header(self, args):
        file_path = args.file_path
        try:
            with open(file_path, "r", encoding="utf-8", errors="replace") as f:
                lines = [ln.rstrip("\n").rstrip("\r") for ln in f.readlines()]
        except FileNotFoundError:
            print(f"Could not read header file: {file_path}", file=sys.stderr)
            sys.exit(1)

        module = self._derive_module_from_path(file_path)
        api_macro = "" if not module else module.upper() + "_API"

        inc_re = re.compile(r'^\s*#\s*include\s+["<]([^">]+)[">]')
        decl_re = re.compile(r'^\s*(?:class|struct)\s+(?:[A-Z][A-Z0-9_]*_API\s+)?([A-Za-z_][A-Za-z0-9_]*)')
        api_re = re.compile(r'\b([A-Z][A-Z0-9_]*_API)\b')

        has_gen_body = False
        any_reflected = False
        gen_body_line = 0
        last_inc_line = 0
        last_inc_path = ""
        gen_h_line = 0
        gen_h_path = ""   # the ACTUAL *.generated.h include (NOT the last include -- fixes rule-c false positive)
        reflected = []  # list of (decl_line, name, has_api)
        pending = False

        findings = []  # (rule_id, line, message, severity)
        state = [False]
        for i, raw in enumerate(lines):
            code = self._strip_line_comment(self._strip_block_comments(raw, state))
            trimmed = code.strip()
            if not trimmed:
                continue
            m = inc_re.search(code)
            if m:
                inc = m.group(1)
                last_inc_line = i + 1
                last_inc_path = inc
                if inc.endswith(".generated.h"):
                    gen_h_line = i + 1
                    gen_h_path = inc
                continue
            if "GENERATED_BODY" in trimmed or "GENERATED_UCLASS_BODY" in trimmed:
                has_gen_body = True
                if gen_body_line == 0:
                    gen_body_line = i + 1
            if (trimmed.startswith("UCLASS") or trimmed.startswith("USTRUCT")
                    or trimmed.startswith("UENUM") or trimmed.startswith("UINTERFACE")):
                any_reflected = True
                if trimmed.startswith("UCLASS"):
                    pending = True
            if pending:
                dm = decl_re.search(code)
                if dm:
                    has_api = bool(api_re.search(code))
                    reflected.append((i + 1, dm.group(1), has_api))
                    pending = False
            # Invalid-specifier rule SKIPPED offline (empty vocabulary).

        # (a) missing GENERATED_BODY.
        if any_reflected and not has_gen_body:
            findings.append(("missing_generated_body",
                             reflected[0][0] if reflected else 0,
                             "Reflected type (UCLASS/USTRUCT) is missing a GENERATED_BODY() macro.",
                             "error"))
        # (b) generated.h not last.
        if gen_h_line != 0 and last_inc_line != 0 and gen_h_line != last_inc_line:
            findings.append(("generated_h_not_last", gen_h_line,
                             f"'*.generated.h' must be the LAST #include (an include at line "
                             f"{last_inc_line} follows it: \"{last_inc_path}\").",
                             "error"))
        elif any_reflected and has_gen_body and gen_h_line == 0:
            findings.append(("missing_generated_h_include", gen_body_line,
                             "Reflected type uses GENERATED_BODY() but no '*.generated.h' include is present (must be last).",
                             "error"))
        # (d) missing <MODULE>_API.
        header_base = os.path.splitext(os.path.basename(file_path.replace("\\", "/")))[0]
        for (decl_line, name, has_api) in reflected:
            if api_macro and not has_api:
                findings.append(("missing_api_macro", decl_line,
                                 f"UCLASS-declared type '{name}' is missing the '{api_macro}' "
                                 f"export macro (class {name} ...).",
                                 "warning"))
        # (c) generated.h name mismatch. Use the ACTUAL generated.h include
        # (gen_h_path), NOT the last include -- when generated.h is not last
        # (rule-b case) the last include is some other header and would fire a
        # bogus mismatch. Gate on the captured include ending in `.generated.h`.
        if gen_h_line != 0 and header_base and gen_h_path.endswith(".generated.h"):
            gen_base = gen_h_path.replace("\\", "/").rsplit("/", 1)[-1]
            gen_base = gen_base[:-len(".generated.h")]
            if gen_base and gen_base != header_base:
                findings.append(("generated_h_name_mismatch", gen_h_line,
                                 f"'{gen_base}.generated.h' does not match the header file name "
                                 f"'{header_base}.h' -- the GENERATED_BODY pairing requires "
                                 f"\"{header_base}.generated.h\".",
                                 "error"))
        # (e) UPROPERTY/UFUNCTION in a non-reflected file.
        if not any_reflected:
            state2 = [False]
            for i, raw in enumerate(lines):
                trimmed = self._strip_line_comment(self._strip_block_comments(raw, state2)).strip()
                if trimmed.startswith("UPROPERTY") or trimmed.startswith("UFUNCTION"):
                    findings.append(("reflected_member_in_non_reflected_type", i + 1,
                                     "UPROPERTY/UFUNCTION found but the file declares no reflected type "
                                     "(UCLASS/USTRUCT) -- the macro will not be processed by UHT.",
                                     "error"))

        findings.sort(key=lambda f: (f[1], f[0]))

        if not findings:
            print("Clean -- no lint findings.")
            return
        out = []
        for (rule_id, line, message, severity) in findings:
            out.append(f"[{severity}] L{line} ({rule_id}): {message}")
        print("\n".join(out))

    # ------------------------------------------------------------------
    # item 9: generate_class_stub — TEXT-RETURN-ONLY, offline-served (Decision 5).
    # ------------------------------------------------------------------
    def generate_class_stub(self, args):
        parent = args.parent
        class_name = args.class_name
        module = args.module
        if not parent or not class_name or not module:
            print("generate_class_stub requires parent, class_name, and module", file=sys.stderr)
            sys.exit(1)

        sym = self.resolve_symbol_row(parent)
        if not sym:
            print(f"Parent class '{parent}' not found in the source index. "
                  f"Run source.trigger_reindex if it is a project type.", file=sys.stderr)
            sys.exit(1)

        if not (parent[0] == "U" or parent[0] == "A"):
            print(f"Parent '{parent}' is not a UCLASS-derived type. generate_class_stub v1 "
                  f"supports UCLASS-derived parents only (no USTRUCT/UENUM/UINTERFACE).", file=sys.stderr)
            sys.exit(1)

        allrows = self.db.execute(
            "SELECT s.file_id, f.path FROM symbols s JOIN files f ON f.id = s.file_id WHERE s.name = ?",
            (parent,)).fetchall()
        parent_path = self.get_file_path(sym["file_id"])
        for r in allrows:
            p = r["path"]
            if p.endswith(".h"):
                parent_path = p
                break
        parent_module = self._derive_module_from_path(parent_path)
        parent_include, _includable, _warn = self.derive_include_path(parent_path, parent_module)

        # Constructor convention.
        ctors = self.db.execute(
            "SELECT signature FROM symbols WHERE name = ? AND kind = 'function'", (parent,)).fetchall()
        saw_any = saw_plain = saw_obj = False
        ctor_open = parent + "("
        for r in ctors:
            s = r["signature"] or ""
            if not s or ctor_open not in s:
                continue
            saw_any = True
            if "FObjectInitializer" in s:
                saw_obj = True
            else:
                saw_plain = True
        needs_obj_init = saw_any and saw_obj and not saw_plain

        api_macro = module.upper() + "_API"
        # UE "Add C++ Class" file-naming: drop the U/A UCLASS-derived prefix from the
        # FILE names (class UMyComp -> MyComp.h/.cpp/.generated.h). class_name is
        # validated UCLASS-derived, so strip a leading U/A only when followed by an
        # uppercase letter; else use the raw name. The class IDENTIFIER is unchanged.
        if (len(class_name) >= 2 and class_name[0] in ("U", "A") and class_name[1].isupper()):
            file_base = class_name[1:]
        else:
            file_base = class_name
        gen_inc = file_base + ".generated.h"

        h = []
        h.append("#pragma once")
        h.append("")
        h.append('#include "CoreMinimal.h"')
        if parent_include:
            h.append(f'#include "{parent_include}"')
        h.append(f'#include "{gen_inc}"')
        h.append("")
        h.append("UCLASS()")
        h.append(f"class {api_macro} {class_name} : public {parent}")
        h.append("{")
        h.append("\tGENERATED_BODY()")
        h.append("")
        h.append("public:")
        if needs_obj_init:
            h.append(f"\t{class_name}(const FObjectInitializer& ObjectInitializer);")
        else:
            h.append(f"\t{class_name}();")
        h.append("};")
        h.append("")
        header_text = "\n".join(h)

        c = []
        c.append(f'#include "{file_base}.h"')
        c.append("")
        if needs_obj_init:
            c.append(f"{class_name}::{class_name}(const FObjectInitializer& ObjectInitializer)")
            c.append("\t: Super(ObjectInitializer)")
            c.append("{")
            c.append("}")
        else:
            c.append(f"{class_name}::{class_name}()")
            c.append("{")
            c.append("}")
        c.append("")
        cpp_text = "\n".join(c)

        print(f"// === {file_base}.h ===\n{header_text}\n// === {file_base}.cpp ===\n{cpp_text}")


# ============================================================
# Project actions — mirrors Source/MonolithIndex (see project search notes below)
# ============================================================

# Upper bound on a project search query, mirroring
# MonolithProjectSearchActionDetail::MaxQueryLength.
PROJECT_SEARCH_MAX_QUERY_LENGTH = 4096
# Each entry becomes one bound placeholder in an IN clause; mirrors the live cap.
PROJECT_SEARCH_MAX_CLASS_FILTERS = 32

# Diagnostics emitted by the FTS5 MATCH expression parser rather than by
# storage/schema access. Mirrors MonolithProjectSearchDetail::IsFts5QuerySyntaxError.
_FTS5_SYNTAX_ERROR_PATTERNS = (
    "fts5: syntax error",
    "unterminated string",
    "malformed match",
    "unknown special query",
    "expected integer, got",
)


def _is_fts5_syntax_error(message: str) -> bool:
    lowered = message.lower()
    return any(pattern in lowered for pattern in _FTS5_SYNTAX_ERROR_PATTERNS)


def _is_fts5_unknown_column_error(message: str) -> bool:
    # The two project FTS tables expose different columns, so on its own this
    # means "not answerable here", not "bad query". Only a rejection by BOTH
    # tables makes it a caller error.
    return "no such column" in message.lower()


# Mirrors the CREATE VIRTUAL TABLE column lists (fts_assets / fts_nodes).
_FTS_ASSET_COLUMNS = "asset_name, asset_class, description, package_path, module_name"
_FTS_NODE_COLUMNS = "node_name, node_class, node_type"


def _describe_unknown_column(message: str) -> str:
    """Name the offending column and list the valid ones.

    Mirrors MonolithProjectSearchDetail::DescribeUnknownColumn.
    """
    marker = "no such column:"
    index = message.lower().find(marker)
    subject = message
    if index != -1:
        column_name = message[index + len(marker):].strip()
        if column_name:
            subject = f"no such column '{column_name}'"
    return (
        f"{subject}. Valid columns are {_FTS_ASSET_COLUMNS} (assets) or "
        f"{_FTS_NODE_COLUMNS} (nodes); one filter cannot span both tables."
    )


class ProjectActions:
    def __init__(self):
        import sqlite3
        self._sqlite3 = sqlite3
        self.db = open_db(PROJECT_DB)

    def search(self, args):
        query = (args.query or "").strip()
        if not query:
            emit_error("'query' must not be empty")
            return
        if len(query) > PROJECT_SEARCH_MAX_QUERY_LENGTH:
            emit_error(
                f"'query' must be {PROJECT_SEARCH_MAX_QUERY_LENGTH} characters or fewer "
                f"(got {len(query)})"
            )
            return

        limit = max(1, min(1000, args.limit))
        sqlite3 = self._sqlite3

        # Mirrors the live `asset_class` filter (SPEC_MonolithIndex "Asset-Class
        # Filtering"). Nothing gates this parity, so it is kept in step by hand.
        class_filter = []
        # Comma-separated within each occurrence, so `--asset-class A,B` and a
        # repeated `--asset-class` both work. The C++ tool's option map is
        # single-valued and only accepts the comma form, so this keeps the two
        # tools accepting the same syntax.
        for occurrence in (getattr(args, "asset_class", None) or []):
            for entry in (occurrence or "").split(","):
                entry = entry.strip()
                if entry and entry not in class_filter:
                    class_filter.append(entry)
        if len(class_filter) > PROJECT_SEARCH_MAX_CLASS_FILTERS:
            emit_error(
                f"'asset_class' must list {PROJECT_SEARCH_MAX_CLASS_FILTERS} classes or fewer "
                f"(got {len(class_filter)})"
            )
            return

        # Only the placeholder count is interpolated; the names stay bound.
        class_predicate = ""
        if class_filter:
            placeholders = ",".join("?" for _ in class_filter)
            class_predicate = f" AND a.asset_class COLLATE NOCASE IN ({placeholders})"

        results = []
        # Per-table verdicts: None = completed, or the unknown-column message.
        not_applicable = []

        def run_search(sql, table_name):
            """Returns True to continue, False once a failure has been emitted."""
            try:
                rows = self.db.execute(sql, (query, *class_filter, limit)).fetchall()
            except sqlite3.DatabaseError as exc:
                # DatabaseError (not just OperationalError) so DatabaseCorruptError
                # is reported as a structured failure instead of a traceback.
                message = str(exc)
                if _is_fts5_unknown_column_error(message):
                    not_applicable.append(message)
                    return True
                if _is_fts5_syntax_error(message):
                    emit_error(f"Invalid FTS5 query: {message}")
                else:
                    emit_error(
                        f"Project search failed: {table_name} FTS query failed: {message}"
                    )
                return False

            for r in rows:
                results.append({
                    "asset_path": r["package_path"],
                    "asset_name": r["asset_name"],
                    "asset_class": r["asset_class"],
                    "module_name": r["module_name"],
                    "match_context": r["ctx"],
                    "rank": r["rank"],
                })
            return True

        if not run_search(
            f"""SELECT a.package_path, a.asset_name, a.asset_class, a.module_name,
                      snippet(fts_assets, 2, '>>>', '<<<', '...', 32) as ctx, rank
               FROM fts_assets f JOIN assets a ON a.id = f.rowid
               WHERE fts_assets MATCH ?{class_predicate} ORDER BY rank LIMIT ?""",
            "assets",
        ):
            return

        if not run_search(
            f"""SELECT a.package_path, a.asset_name, a.asset_class, a.module_name,
                      snippet(fts_nodes, 0, '>>>', '<<<', '...', 32) as ctx, f.rank
               FROM fts_nodes f JOIN nodes n ON n.id = f.rowid
               JOIN assets a ON a.id = n.asset_id
               WHERE fts_nodes MATCH ?{class_predicate} ORDER BY f.rank LIMIT ?""",
            "nodes",
        ):
            return

        if len(not_applicable) == 2:
            # No FTS table exposes the requested column.
            emit_error(f"Invalid FTS5 query: {_describe_unknown_column(not_applicable[0])}")
            return

        results.sort(key=lambda x: x["rank"])
        results = results[:limit]

        print(json.dumps({"success": True, "count": len(results), "results": results}, indent=2))

    def find_by_type(self, args):
        asset_class = args.asset_class
        limit = args.limit
        offset = args.offset

        rows = self.db.execute(
            "SELECT package_path, asset_name, asset_class, module_name, description FROM assets WHERE asset_class = ? LIMIT ? OFFSET ?",
            (asset_class, limit, offset)
        ).fetchall()

        results = [dict(r) for r in rows]
        print(json.dumps({"success": True, "count": len(results), "results": results}, indent=2))

    def find_references(self, args):
        asset_path = args.asset_path
        asset = self.db.execute("SELECT id FROM assets WHERE package_path = ?", (asset_path,)).fetchone()
        if not asset:
            print(json.dumps({"success": False, "error": f"Asset not found: {asset_path}"}))
            return

        aid = asset["id"]

        deps = self.db.execute(
            """SELECT a.package_path, a.asset_class, d.dependency_type
               FROM dependencies d JOIN assets a ON a.id = d.target_asset_id WHERE d.source_asset_id = ?""",
            (aid,)
        ).fetchall()

        refs = self.db.execute(
            """SELECT a.package_path, a.asset_class, d.dependency_type
               FROM dependencies d JOIN assets a ON a.id = d.source_asset_id WHERE d.target_asset_id = ?""",
            (aid,)
        ).fetchall()

        print(json.dumps({
            "success": True,
            "depends_on": [{"path": r["package_path"], "class": r["asset_class"], "type": r["dependency_type"]} for r in deps],
            "referenced_by": [{"path": r["package_path"], "class": r["asset_class"], "type": r["dependency_type"]} for r in refs],
        }, indent=2))

    def get_stats(self, args):
        sqlite3 = self._sqlite3
        tables = ["assets", "nodes", "connections", "variables", "parameters", "dependencies", "actors", "tags", "configs", "datatable_rows"]
        stats = {}
        for t in tables:
            try:
                row = self.db.execute(f"SELECT COUNT(*) as c FROM {t}").fetchone()
                stats[t] = row["c"]
            except sqlite3.OperationalError:
                stats[t] = 0

        breakdown = {}
        try:
            rows = self.db.execute("SELECT asset_class, COUNT(*) as cnt FROM assets GROUP BY asset_class ORDER BY cnt DESC LIMIT 20").fetchall()
            for r in rows:
                breakdown[r["asset_class"]] = r["cnt"]
        except sqlite3.OperationalError:
            pass

        stats["asset_class_breakdown"] = breakdown

        try:
            mod_rows = self.db.execute(
                "SELECT CASE WHEN module_name = '' THEN 'Project' ELSE module_name END as mod, COUNT(*) as cnt FROM assets GROUP BY module_name ORDER BY cnt DESC"
            ).fetchall()
            stats["module_breakdown"] = {r["mod"]: r["cnt"] for r in mod_rows}
        except sqlite3.OperationalError:
            stats["module_breakdown"] = {}

        print(json.dumps(stats, indent=2))

    def get_asset_details(self, args):
        asset_path = args.asset_path
        asset = self.db.execute(
            "SELECT * FROM assets WHERE package_path = ?", (asset_path,)
        ).fetchone()
        if not asset:
            print(json.dumps({"error": f"Asset not found: {asset_path}"}))
            return

        details = dict(asset)
        aid = asset["id"]

        nodes = self.db.execute(
            "SELECT node_type, node_name, node_class FROM nodes WHERE asset_id = ?", (aid,)
        ).fetchall()
        details["nodes"] = [dict(n) for n in nodes]

        variables = self.db.execute(
            "SELECT var_name, var_type, category, default_value, is_exposed, is_replicated FROM variables WHERE asset_id = ?",
            (aid,)
        ).fetchall()
        details["variables"] = [dict(v) for v in variables]

        params = self.db.execute(
            "SELECT param_name, param_type, param_group, default_value FROM parameters WHERE asset_id = ?",
            (aid,)
        ).fetchall()
        details["parameters"] = [dict(p) for p in params]

        print(json.dumps(details, indent=2, default=str))


# ============================================================
# Reflection Intelligence — FULL parity with the 4 live RI adapters.
# ============================================================

class ReflectionActions:
    """All RI tables live in EngineSource.db (reflect_* / risk_* / git_* / decision_*)."""

    def __init__(self):
        import sqlite3
        self._sqlite3 = sqlite3
        self.db = open_db(SOURCE_DB)

    def _require_table(self, table: str) -> bool:
        row = self.db.execute(
            "SELECT 1 FROM sqlite_master WHERE type='table' AND name=? LIMIT 1", (table,)
        ).fetchone()
        if not row:
            emit_error(f"{table} not in EngineSource.db. Build the project + rebuild_reflection_index in-editor.")
            return False
        return True

    def _count(self, where_sql: str, binds, table: str) -> int:
        row = self.db.execute(f"SELECT COUNT(*) FROM {table} {where_sql}", binds).fetchone()
        return int(row[0])

    # ---------------- cppreflect ----------------

    def get_uclass(self, args):
        if not self._require_table("reflect_uclasses"):
            return
        class_name = args.class_name
        module = getattr(args, "module_name", None) or ""

        sql = "SELECT class_name, module_name, parent_class, source_path, source_line, flags FROM reflect_uclasses WHERE class_name = ?"
        binds = [class_name]
        if module:
            sql += " AND module_name = ?"
            binds.append(module)
        sql += " LIMIT 1"
        row = self.db.execute(sql, binds).fetchone()
        if not row:
            emit({"success": True, "uclass": None})
            return

        cname = coerce_str(row["class_name"])
        mname = coerce_str(row["module_name"])
        source_line = int(row["source_line"] or 0)
        source_path = coerce_str(row["source_path"])

        # Source auto-join when line==0 or path=="".
        if source_line == 0 or source_path == "":
            j = self.db.execute(
                "SELECT s.line_start, f.path FROM symbols s JOIN files f ON f.id = s.file_id "
                "WHERE s.name = ? AND s.kind IN ('class','struct') LIMIT 1",
                (cname,)
            ).fetchone()
            if j:
                if source_line == 0:
                    source_line = int(j["line_start"] or 0)
                if source_path == "":
                    source_path = coerce_str(j["path"])

        # Parent chain (iterative, cap 16).
        parent_chain = []
        cur = coerce_str(row["parent_class"])
        steps = 0
        last_appended = None
        while cur and steps < 16:
            parent_chain.append(cur)
            last_appended = cur
            pr = self.db.execute(
                "SELECT parent_class FROM reflect_uclasses WHERE class_name = ? LIMIT 1", (cur,)
            ).fetchone()
            if not pr:
                break  # engine class outside index; cur already appended
            nxt = coerce_str(pr["parent_class"])
            if nxt == last_appended:
                break
            cur = nxt
            steps += 1
            if not cur:
                break

        # UPROPERTYs (module-scoped only when module passed).
        psql = ("SELECT property_name, property_type, cpp_module, blueprint_visibility, specifiers "
                "FROM reflect_uproperties WHERE owning_class = ?")
        pbinds = [cname]
        if module:
            psql += " AND cpp_module = ?"
            pbinds.append(module)
        psql += " ORDER BY property_name"
        props = []
        for p in self.db.execute(psql, pbinds).fetchall():
            props.append({
                "property_name": coerce_str(p["property_name"]),
                "property_type": coerce_str(p["property_type"]),
                "cpp_module": coerce_str(p["cpp_module"]),
                "blueprint_visibility": coerce_str(p["blueprint_visibility"]),
                "specifiers": coerce_str(p["specifiers"]),
            })

        # UFUNCTIONs (module-scoped only when module passed).
        fsql = ("SELECT function_name, return_type, blueprint_callable, cpp_module, specifiers "
                "FROM reflect_ufunctions WHERE owning_class = ?")
        fbinds = [cname]
        if module:
            fsql += " AND cpp_module = ?"
            fbinds.append(module)
        fsql += " ORDER BY function_name"
        funcs = []
        for f in self.db.execute(fsql, fbinds).fetchall():
            funcs.append({
                "function_name": coerce_str(f["function_name"]),
                "return_type": coerce_str(f["return_type"]),
                "blueprint_callable": bool(f["blueprint_callable"]),
                "cpp_module": coerce_str(f["cpp_module"]),
                "specifiers": coerce_str(f["specifiers"]),
            })

        uclass = {
            "class_name": cname,
            "module_name": mname,
            "parent_class": coerce_str(row["parent_class"]),
            "source_path": source_path,
            "source_line": source_line,
            "flags": coerce_str(row["flags"]),
            "parent_chain": parent_chain,
            "uproperties": props,
            "ufunctions": funcs,
        }
        emit({"success": True, "uclass": uclass})

    def list_uproperties(self, args):
        if not self._require_table("reflect_uproperties"):
            return
        # exe precedence: --class_name flag wins, else the positional (Args::opt then positional[0]).
        class_name = (getattr(args, "class_name", None) or
                      getattr(args, "class_name_pos", None) or "")
        bp_only = bool(getattr(args, "blueprint_visible_only", False))
        limit = clamp_limit(args.limit)
        fh = compute_filter_hash([class_name, "1" if bp_only else "0"])

        try:
            page, had_cursor = resolve_page(getattr(args, "cursor", None) or "", fh)
        except CursorError as e:
            emit_error(e.reason, "INVALID_CURSOR")
            return

        where = "WHERE 1=1"
        binds = []
        if class_name:
            where += " AND owning_class = ?"
            binds.append(class_name)
        if bp_only:
            where += " AND blueprint_visibility IS NOT NULL AND blueprint_visibility <> ''"

        sql = (f"SELECT owning_class, property_name, property_type, cpp_module, blueprint_visibility, specifiers "
               f"FROM reflect_uproperties {where} ORDER BY owning_class, property_name LIMIT ? OFFSET ?")
        rows = self.db.execute(sql, binds + [limit, page * limit]).fetchall()

        out = {"success": True, "uproperties": [{
            "owning_class": coerce_str(r["owning_class"]),
            "property_name": coerce_str(r["property_name"]),
            "property_type": coerce_str(r["property_type"]),
            "cpp_module": coerce_str(r["cpp_module"]),
            "blueprint_visibility": coerce_str(r["blueprint_visibility"]),
            "specifiers": coerce_str(r["specifiers"]),
        } for r in rows]}

        total = -1
        if not had_cursor:
            total = self._count(where, binds, "reflect_uproperties")
            out["total_estimate"] = total
        if len(rows) == limit:
            out["next_cursor"] = encode_cursor(fh, page + 1, total if not had_cursor else -1)
        emit(out)

    def list_ufunctions(self, args):
        if not self._require_table("reflect_ufunctions"):
            return
        # exe precedence: --class_name flag wins, else the positional (Args::opt then positional[0]).
        class_name = (getattr(args, "class_name", None) or
                      getattr(args, "class_name_pos", None) or "")
        bp_only = bool(getattr(args, "blueprint_callable_only", False))
        limit = clamp_limit(args.limit)
        fh = compute_filter_hash([class_name, "1" if bp_only else "0"])

        try:
            page, had_cursor = resolve_page(getattr(args, "cursor", None) or "", fh)
        except CursorError as e:
            emit_error(e.reason, "INVALID_CURSOR")
            return

        where = "WHERE 1=1"
        binds = []
        if class_name:
            where += " AND owning_class = ?"
            binds.append(class_name)
        if bp_only:
            where += " AND blueprint_callable = 1"

        sql = (f"SELECT owning_class, function_name, return_type, blueprint_callable, cpp_module, specifiers, source_line "
               f"FROM reflect_ufunctions {where} ORDER BY owning_class, function_name LIMIT ? OFFSET ?")
        rows = self.db.execute(sql, binds + [limit, page * limit]).fetchall()

        funcs = []
        for r in rows:
            line = int(r["source_line"] or 0)
            if line == 0:
                line = self._lookup_function_source_line(coerce_str(r["owning_class"]), coerce_str(r["function_name"]))
            funcs.append({
                "owning_class": coerce_str(r["owning_class"]),
                "function_name": coerce_str(r["function_name"]),
                "return_type": coerce_str(r["return_type"]),
                "blueprint_callable": bool(r["blueprint_callable"]),
                "cpp_module": coerce_str(r["cpp_module"]),
                "specifiers": coerce_str(r["specifiers"]),
                "source_line": line,
            })

        out = {"success": True, "ufunctions": funcs}
        total = -1
        if not had_cursor:
            total = self._count(where, binds, "reflect_ufunctions")
            out["total_estimate"] = total
        if len(rows) == limit:
            out["next_cursor"] = encode_cursor(fh, page + 1, total if not had_cursor else -1)
        emit(out)

    def _lookup_function_source_line(self, owning_class: str, function_name: str) -> int:
        if not function_name:
            return 0
        if owning_class:
            r = self.db.execute(
                "SELECT s.line_start FROM symbols s JOIN symbols p ON p.id = s.parent_symbol_id "
                "WHERE s.name = ? AND s.kind IN ('function','method') AND p.name = ? "
                "AND p.kind IN ('class','struct') LIMIT 1",
                (function_name, owning_class)
            ).fetchone()
        else:
            r = self.db.execute(
                "SELECT line_start FROM symbols WHERE name = ? AND kind IN ('function','method') LIMIT 1",
                (function_name,)
            ).fetchone()
        return int(r["line_start"]) if r else 0

    def find_interface_impls(self, args):
        if not self._require_table("reflect_uinterface_impls"):
            return
        interface_name = args.interface_name
        rows = self.db.execute(
            "SELECT impl.implementing_class, impl.cpp_module, cls.source_path "
            "FROM reflect_uinterface_impls impl "
            "LEFT JOIN reflect_uclasses cls "
            "  ON cls.class_name = impl.implementing_class AND cls.module_name = impl.cpp_module "
            "WHERE impl.interface_name = ? "
            "ORDER BY impl.cpp_module, impl.implementing_class",
            (interface_name,)
        ).fetchall()
        impls = [{
            "implementing_class": coerce_str(r["implementing_class"]),
            "cpp_module": coerce_str(r["cpp_module"]),
            "source_path": coerce_str(r["source_path"]),
        } for r in rows]
        emit({"success": True, "interface_name": interface_name, "implementers": impls})

    def _token_universe(self):
        """Distinct flags-token counts. Returns (sorted list of {token,count}, distinct_count)."""
        counts = {}
        for r in self.db.execute(
            "SELECT flags FROM reflect_uclasses WHERE flags IS NOT NULL AND flags <> ''"
        ).fetchall():
            seen = set()
            for tok in coerce_str(r["flags"]).split(":"):
                tok = tok.strip()
                if not tok or tok in seen:
                    continue
                seen.add(tok)
                counts[tok] = counts.get(tok, 0) + 1
        items = sorted(counts.items(), key=lambda kv: (-kv[1], kv[0]))
        return ([{"token": t, "count": c} for t, c in items], len(counts))

    def find_class_specifier(self, args):
        if not self._require_table("reflect_uclasses"):
            return
        specifier_name = args.specifier_name
        limit = clamp_limit(args.limit)
        fh = compute_filter_hash([specifier_name])

        try:
            page, had_cursor = resolve_page(getattr(args, "cursor", None) or "", fh)
        except CursorError as e:
            emit_error(e.reason, "INVALID_CURSOR")
            return

        spec_lower = specifier_name.lower()

        # Dropped tokens — never stored in the flags vocabulary.
        if spec_lower in ("minimalapi", "notblueprintable"):
            known, _ = self._token_universe()
            note = (f'"{specifier_name}" is a C++ UCLASS specifier that UHT does not store in the '
                    f'metadata-key vocabulary (the `flags` column), so it can never match. '
                    f'Call list_class_specifiers to see the tokens that are queryable.')
            emit({
                "success": True,
                "specifier_name": specifier_name,
                "uclasses": [],
                "token_stored": False,
                "note": note,
                "known_tokens": known,
            })
            return

        effective = specifier_name
        if spec_lower == "blueprintable":
            effective = "IsBlueprintBase"

        sql = ("SELECT class_name, module_name, parent_class, source_path, flags FROM reflect_uclasses "
               "WHERE flags = ? COLLATE NOCASE OR flags LIKE ? OR flags LIKE ? OR flags LIKE ? "
               "ORDER BY module_name, class_name LIMIT ? OFFSET ?")
        binds = [effective, effective + ":%", "%:" + effective + ":%", "%:" + effective,
                 limit, page * limit]
        rows = self.db.execute(sql, binds).fetchall()

        uclasses = [{
            "class_name": coerce_str(r["class_name"]),
            "module_name": coerce_str(r["module_name"]),
            "parent_class": coerce_str(r["parent_class"]),
            "source_path": coerce_str(r["source_path"]),
            "flags": coerce_str(r["flags"]),
        } for r in rows]

        out = {"success": True, "specifier_name": specifier_name}
        if effective != specifier_name:
            out["effective_token"] = effective
        out["uclasses"] = uclasses
        if len(rows) == 0 and not had_cursor:
            known, _ = self._token_universe()
            out["known_tokens"] = known
        if len(rows) == limit:
            out["next_cursor"] = encode_cursor(fh, page + 1, -1)
        emit(out)

    def list_class_specifiers(self, args):
        if not self._require_table("reflect_uclasses"):
            return
        specifiers, distinct = self._token_universe()
        emit({"success": True, "specifiers": specifiers, "distinct_count": distinct})

    # ---------------- network ----------------

    def list_replicated_classes(self, args):
        if not self._require_table("reflect_replicated_properties"):
            return
        limit = clamp_limit(args.limit)
        fh = compute_filter_hash([])  # empty parts -> 0
        try:
            page, had_cursor = resolve_page(getattr(args, "cursor", None) or "", fh)
        except CursorError as e:
            emit_error(e.reason, "INVALID_CURSOR")
            return

        rows = self.db.execute(
            "SELECT owning_class, cpp_module, COUNT(*) AS prop_count "
            "FROM reflect_replicated_properties "
            "GROUP BY owning_class, cpp_module "
            "ORDER BY cpp_module, owning_class LIMIT ? OFFSET ?",
            (limit, page * limit)
        ).fetchall()

        out = {"success": True, "classes": [{
            "owning_class": coerce_str(r["owning_class"]),
            "cpp_module": coerce_str(r["cpp_module"]),
            "replicated_property_count": int(r["prop_count"]),
        } for r in rows]}

        total = -1
        if not had_cursor:
            row = self.db.execute(
                "SELECT COUNT(*) FROM (SELECT 1 FROM reflect_replicated_properties "
                "GROUP BY owning_class, cpp_module)"
            ).fetchone()
            total = int(row[0])
            out["total_estimate"] = total
        if len(rows) == limit:
            out["next_cursor"] = encode_cursor(fh, page + 1, total if not had_cursor else -1)
        emit(out)

    def list_rpc_functions(self, args):
        if not self._require_table("reflect_ufunctions"):
            return
        class_name = getattr(args, "class_name", None) or ""
        rpc_kind = getattr(args, "rpc_kind", None) or ""
        limit = clamp_limit(args.limit)
        fh = compute_filter_hash([class_name, rpc_kind])
        try:
            page, had_cursor = resolve_page(getattr(args, "cursor", None) or "", fh)
        except CursorError as e:
            emit_error(e.reason, "INVALID_CURSOR")
            return

        where = ("WHERE (specifiers LIKE '%Server%' OR specifiers LIKE '%Client%' "
                 "OR specifiers LIKE '%NetMulticast%')")
        binds = []
        if class_name:
            where += " AND owning_class = ?"
            binds.append(class_name)

        sql = (f"SELECT owning_class, function_name, cpp_module, blueprint_callable, specifiers "
               f"FROM reflect_ufunctions {where} ORDER BY cpp_module, owning_class, function_name "
               f"LIMIT ? OFFSET ?")
        rows = self.db.execute(sql, binds + [limit, page * limit]).fetchall()

        def classify(spec: str) -> str:
            if "Server" in spec:
                return "Server"
            if "Client" in spec:
                return "Client"
            if "NetMulticast" in spec:
                return "Multicast"
            return ""

        rpcs = []
        for r in rows:
            spec = coerce_str(r["specifiers"])
            kind = classify(spec)
            if rpc_kind and kind.lower() != rpc_kind.lower():
                continue
            rpcs.append({
                "owning_class": coerce_str(r["owning_class"]),
                "function_name": coerce_str(r["function_name"]),
                "cpp_module": coerce_str(r["cpp_module"]),
                "rpc_kind": kind,
                "specifiers": spec,
                "blueprint_callable": bool(r["blueprint_callable"]),
            })

        out = {"success": True, "rpcs": rpcs}
        # next_cursor gated on EMITTED count == limit (post-filter).
        if len(rpcs) == limit:
            out["next_cursor"] = encode_cursor(fh, page + 1, -1)
        emit(out)

    def list_onrep_handlers(self, args):
        if not self._require_table("reflect_ufunctions"):
            return
        class_name = getattr(args, "class_name", None) or ""
        limit = clamp_limit(args.limit)
        fh = compute_filter_hash([class_name])
        try:
            page, had_cursor = resolve_page(getattr(args, "cursor", None) or "", fh)
        except CursorError as e:
            emit_error(e.reason, "INVALID_CURSOR")
            return

        where = "WHERE function_name LIKE 'OnRep_%'"
        binds = []
        if class_name:
            where += " AND owning_class = ?"
            binds.append(class_name)

        sql = (f"SELECT owning_class, function_name, cpp_module FROM reflect_ufunctions {where} "
               f"ORDER BY cpp_module, owning_class, function_name LIMIT ? OFFSET ?")
        rows = self.db.execute(sql, binds + [limit, page * limit]).fetchall()

        out = {"success": True, "handlers": [{
            "owning_class": coerce_str(r["owning_class"]),
            "function_name": coerce_str(r["function_name"]),
            "cpp_module": coerce_str(r["cpp_module"]),
        } for r in rows]}
        if len(rows) == limit:
            out["next_cursor"] = encode_cursor(fh, page + 1, -1)
        emit(out)

    def audit_unbalanced_onreps(self, args):
        if not self._require_table("reflect_replicated_properties"):
            return
        limit = clamp_limit(args.limit)
        fh = compute_filter_hash([])
        try:
            page, had_cursor = resolve_page(getattr(args, "cursor", None) or "", fh)
        except CursorError as e:
            emit_error(e.reason, "INVALID_CURSOR")
            return

        sql = (
            "SELECT rp.owning_class, rp.property_name, rp.cpp_module, rp.rep_notify_func "
            "FROM reflect_replicated_properties rp "
            "LEFT JOIN reflect_ufunctions uf "
            "  ON uf.owning_class = rp.owning_class AND uf.function_name = rp.rep_notify_func "
            "     AND uf.cpp_module = rp.cpp_module "
            "WHERE rp.rep_kind = 'ReplicatedUsing' "
            "  AND rp.rep_notify_func IS NOT NULL AND rp.rep_notify_func <> '' "
            "  AND uf.function_name IS NULL "
            "ORDER BY rp.cpp_module, rp.owning_class, rp.property_name LIMIT ? OFFSET ?"
        )
        rows = self.db.execute(sql, (limit, page * limit)).fetchall()

        out = {"success": True, "violations": [{
            "owning_class": coerce_str(r["owning_class"]),
            "property_name": coerce_str(r["property_name"]),
            "cpp_module": coerce_str(r["cpp_module"]),
            "missing_function": coerce_str(r["rep_notify_func"]),
            "violation": "UPROPERTY(ReplicatedUsing) references an OnRep_ UFUNCTION that does not exist on this class.",
        } for r in rows]}
        if len(rows) == limit:
            out["next_cursor"] = encode_cursor(fh, page + 1, -1)
        emit(out)

    # ---------------- decision ----------------

    _DECISION_COLS = "decision_id, title, status, source_path, source_line, confidence, rationale, source_mtime"

    def _decision_row_to_json(self, r):
        return {
            "decision_id": coerce_str(r["decision_id"]),
            "title": coerce_str(r["title"]),
            "status": coerce_str(r["status"]),
            "source_path": coerce_str(r["source_path"]),
            "source_line": int(r["source_line"] or 0),
            "confidence": flt(r["confidence"] or 0.0),  # raw double -> %.17g
            "rationale": coerce_str(r["rationale"]),
            "source_mtime": int(r["source_mtime"] or 0),
        }

    def list_decisions(self, args):
        if not self._require_table("decision_records"):
            return
        path_filter = getattr(args, "path_filter", None) or ""
        min_conf = getattr(args, "min_confidence", None)
        min_conf = 0.6 if min_conf is None else float(min_conf)
        status = getattr(args, "status", None) or ""
        limit = clamp_limit(args.limit)
        fh = compute_filter_hash_decision(path_filter, min_conf, status)
        try:
            page, had_cursor = resolve_page(getattr(args, "cursor", None) or "", fh)
        except CursorError as e:
            emit_error(e.reason, "INVALID_CURSOR")
            return

        where = "WHERE confidence >= ?"
        binds = [min_conf]
        if path_filter:
            where += " AND source_path LIKE ?"
            binds.append("%" + path_filter + "%")
        if status:
            where += " AND status = ?"
            binds.append(status)

        sql = (f"SELECT {self._DECISION_COLS} FROM decision_records {where} "
               f"ORDER BY source_path, source_line LIMIT ? OFFSET ?")
        rows = self.db.execute(sql, binds + [limit, page * limit]).fetchall()

        out = {"success": True, "decisions": [self._decision_row_to_json(r) for r in rows]}
        total = -1
        if not had_cursor:
            total = self._count(where, binds, "decision_records")
            out["total_estimate"] = total
        if len(rows) == limit:
            out["next_cursor"] = encode_cursor(fh, page + 1, total if not had_cursor else -1)
        emit(out)

    def get_decision(self, args):
        if not self._require_table("decision_records"):
            return
        row = self.db.execute(
            f"SELECT {self._DECISION_COLS} FROM decision_records WHERE decision_id = ? LIMIT 1",
            (args.decision_id,)
        ).fetchone()
        if not row:
            emit({"success": True, "decision": None})
            return
        emit({"success": True, "decision": self._decision_row_to_json(row)})

    def list_stale(self, args):
        if not self._require_table("decision_records"):
            return
        # exe precedence: opt("max_age_days") flag wins, else positional; missing -> 0.
        opt_val = getattr(args, "max_age_days_opt", None)
        max_age = int(opt_val if opt_val is not None else (getattr(args, "max_age_days", 0) or 0))
        if max_age <= 0:
            emit_error("`max_age_days` must be positive.")
            return
        path_filter = getattr(args, "path_filter", None) or ""
        limit = clamp_limit(args.limit)
        cutoff = int(time.time()) - max_age * 86400
        fh = compute_filter_hash_decision(path_filter, float(max_age), "__stale__")
        try:
            page, had_cursor = resolve_page(getattr(args, "cursor", None) or "", fh)
        except CursorError as e:
            emit_error(e.reason, "INVALID_CURSOR")
            return

        where = "WHERE source_mtime > 0 AND source_mtime < ?"
        binds = [cutoff]
        if path_filter:
            where += " AND source_path LIKE ?"
            binds.append("%" + path_filter + "%")

        sql = (f"SELECT {self._DECISION_COLS} FROM decision_records {where} "
               f"ORDER BY source_mtime ASC LIMIT ? OFFSET ?")
        rows = self.db.execute(sql, binds + [limit, page * limit]).fetchall()

        out = {
            "success": True,
            "stale_decisions": [self._decision_row_to_json(r) for r in rows],
            "cutoff_unix": cutoff,
        }
        if len(rows) == limit:
            out["next_cursor"] = encode_cursor(fh, page + 1, -1)
        emit(out)

    def find_supersession_chain(self, args):
        if not self._require_table("decision_supersedes"):
            return
        start = args.decision_id
        depth = getattr(args, "depth", None)
        depth = 10 if depth is None else int(depth)
        if depth < 1:
            depth = 1
        if depth > 50:
            depth = 50

        visited = {start}
        frontier = [start]
        chain = []
        cur_depth = 0
        truncated = False
        while frontier and cur_depth < depth:
            cur_depth += 1
            nxt = []
            for frm in frontier:
                tos = self.db.execute(
                    "SELECT to_decision_id FROM decision_supersedes WHERE from_decision_id = ?", (frm,)
                ).fetchall()
                for t in tos:
                    to = coerce_str(t["to_decision_id"])
                    if to in visited:
                        continue
                    visited.add(to)
                    chain.append({"from": frm, "to": to, "depth": cur_depth})
                    nxt.append(to)
            frontier = nxt
        if frontier:
            truncated = True
        emit({"success": True, "start": start, "chain": chain, "truncated": truncated})

    def find_referent_decisions(self, args):
        if not self._require_table("decision_supersedes"):
            return
        did = args.decision_id
        rows = self.db.execute(
            "SELECT r.decision_id, r.title, r.status, r.source_path, r.source_line, "
            "r.confidence, r.rationale, r.source_mtime "
            "FROM decision_supersedes s "
            "JOIN decision_records r ON r.decision_id = s.from_decision_id "
            "WHERE s.to_decision_id = ? "
            "ORDER BY r.source_path, r.source_line",
            (did,)
        ).fetchall()
        emit({
            "success": True,
            "decision_id": did,
            "referent_decisions": [self._decision_row_to_json(r) for r in rows],
        })

    # ---------------- risk ----------------

    def get_hotspot_score(self, args):
        if not self._require_table("risk_hotspot_scores"):
            return
        cp = canon_path(args.file_path)
        row = self.db.execute(
            "SELECT file_path, churn, complexity_proxy, normalised_churn, normalised_complexity, score "
            "FROM risk_hotspot_scores WHERE file_path = ? LIMIT 1", (cp,)
        ).fetchone()
        if not row:
            emit({"success": True, "hotspot": None})
            return
        emit({"success": True, "hotspot": {
            "file_path": coerce_str(row["file_path"]),
            "churn": int(row["churn"] or 0),
            "complexity_proxy": int(row["complexity_proxy"] or 0),
            "normalised_churn": flt(row["normalised_churn"] or 0.0),
            "normalised_complexity": flt(row["normalised_complexity"] or 0.0),
            "score": flt(row["score"] or 0.0),
        }})

    def get_cochange_pairs(self, args):
        if not self._require_table("git_cochange_pairs"):
            return
        cp = canon_path(args.file_path)
        limit = clamp_limit(args.limit)
        fh = compute_filter_hash([cp])
        try:
            page, had_cursor = resolve_page(getattr(args, "cursor", None) or "", fh)
        except CursorError as e:
            emit_error(e.reason, "INVALID_CURSOR")
            return

        rows = self.db.execute(
            "SELECT repo_tag, CASE WHEN file_a = ? THEN file_b ELSE file_a END AS partner, count "
            "FROM git_cochange_pairs WHERE file_a = ? OR file_b = ? "
            "ORDER BY count DESC, partner ASC LIMIT ? OFFSET ?",
            (cp, cp, cp, limit, page * limit)
        ).fetchall()

        out = {"success": True, "file_path": cp, "partners": [{
            "repo_tag": coerce_str(r["repo_tag"]),
            "partner": coerce_str(r["partner"]),
            "count": int(r["count"]),
        } for r in rows]}

        total = -1
        if not had_cursor:
            row = self.db.execute(
                "SELECT COUNT(*) FROM git_cochange_pairs WHERE file_a = ? OR file_b = ?", (cp, cp)
            ).fetchone()
            total = int(row[0])
            out["total_estimate"] = total
        if len(rows) == limit:
            out["next_cursor"] = encode_cursor(fh, page + 1, total if not had_cursor else -1)
        emit(out)

    def get_file_churn(self, args):
        if not self._require_table("git_file_churn"):
            return
        cp = canon_path(args.file_path)
        repo_tag = getattr(args, "repo_tag", None) or ""
        sql = "SELECT repo_tag, commit_count, last_touched FROM git_file_churn WHERE file_path = ?"
        binds = [cp]
        if repo_tag:
            sql += " AND repo_tag = ?"
            binds.append(repo_tag)
        rows = self.db.execute(sql, binds).fetchall()
        emit({"success": True, "file_path": cp, "churn_by_repo": [{
            "repo_tag": coerce_str(r["repo_tag"]),
            "commit_count": int(r["commit_count"] or 0),
            "last_touched_unix": int(r["last_touched"] or 0),
        } for r in rows]})

    def get_release_window_hotspots(self, args):
        if not self._require_table("risk_hotspot_scores"):
            return
        since = getattr(args, "since_unix", None)
        since = (int(time.time()) - 30 * 86400) if since is None else int(since)
        limit = clamp_limit(args.limit)
        fh = compute_filter_hash([str(since)])
        try:
            page, had_cursor = resolve_page(getattr(args, "cursor", None) or "", fh)
        except CursorError as e:
            emit_error(e.reason, "INVALID_CURSOR")
            return

        rows = self.db.execute(
            "SELECT h.file_path, h.score, h.churn, h.complexity_proxy, "
            "  (SELECT MAX(c.last_touched) FROM git_file_churn c WHERE c.file_path = h.file_path) AS last_touched "
            "FROM risk_hotspot_scores h "
            "WHERE (SELECT MAX(c2.last_touched) FROM git_file_churn c2 WHERE c2.file_path = h.file_path) >= ? "
            "ORDER BY h.score DESC LIMIT ? OFFSET ?",
            (since, limit, page * limit)
        ).fetchall()

        out = {"success": True, "since_unix": since, "hotspots": [{
            "file_path": coerce_str(r["file_path"]),
            "score": flt(r["score"] or 0.0),
            "churn": int(r["churn"] or 0),
            "complexity_proxy": int(r["complexity_proxy"] or 0),
            "last_touched_unix": int(r["last_touched"] or 0),
        } for r in rows]}
        if len(rows) == limit:
            out["next_cursor"] = encode_cursor(fh, page + 1, -1)
        emit(out)

    def list_conditional_gates(self, args):
        if not self._require_table("reflect_conditional_gates"):
            return
        macro_filter = getattr(args, "macro_filter", None) or ""
        path_filter = getattr(args, "path_filter", None) or ""
        limit = clamp_limit(args.limit)
        fh = compute_filter_hash([macro_filter, path_filter])
        try:
            page, had_cursor = resolve_page(getattr(args, "cursor", None) or "", fh)
        except CursorError as e:
            emit_error(e.reason, "INVALID_CURSOR")
            return

        where = "WHERE 1=1"
        binds = []
        if macro_filter:
            where += " AND macro_name LIKE ?"
            binds.append("%" + macro_filter + "%")
        if path_filter:
            where += " AND source_path LIKE ?"
            binds.append("%" + path_filter + "%")

        sql = (f"SELECT id, source_path, source_line, macro_name, gate_kind, context_snippet "
               f"FROM reflect_conditional_gates {where} ORDER BY source_path, source_line LIMIT ? OFFSET ?")
        rows = self.db.execute(sql, binds + [limit, page * limit]).fetchall()

        out = {"success": True, "gates": [{
            "id": int(r["id"]),
            "source_path": coerce_str(r["source_path"]),
            "source_line": int(r["source_line"] or 0),
            "macro_name": coerce_str(r["macro_name"]),
            "gate_kind": coerce_str(r["gate_kind"]),
            "context_snippet": coerce_str(r["context_snippet"]),
        } for r in rows]}
        if len(rows) == limit:
            out["next_cursor"] = encode_cursor(fh, page + 1, -1)
        emit(out)


# ============================================================
# CLI setup
# ============================================================

def _add_paginated(p):
    p.add_argument("--limit", type=int, default=50)
    p.add_argument("--cursor", default="")


def build_parser():
    parser = argparse.ArgumentParser(
        description="Monolith Offline CLI — query source/project/RI databases without the editor",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__
    )
    parser.add_argument("--version", action="store_true", help="Print parity-rev string and exit")
    sub = parser.add_subparsers(dest="namespace")

    # --- source namespace ---
    src = sub.add_parser("source", help="Query engine source database")
    src_sub = src.add_subparsers(dest="action", required=True)

    p = src_sub.add_parser("search_source")
    p.add_argument("query")
    p.add_argument("--scope", default="all", choices=["all", "cpp", "shaders"])
    p.add_argument("--limit", type=int, default=20)
    p.add_argument("--module", default="")
    p.add_argument("--kind", default="")

    p = src_sub.add_parser("read_source")
    p.add_argument("symbol")
    p.add_argument("--header", action="store_true", default=True)
    p.add_argument("--no-header", dest="header", action="store_false")
    p.add_argument("--max-lines", type=int, default=0)
    p.add_argument("--members-only", action="store_true")

    p = src_sub.add_parser("find_references")
    p.add_argument("symbol")
    p.add_argument("--ref-kind", default="")
    p.add_argument("--limit", type=int, default=50)

    p = src_sub.add_parser("find_callers")
    p.add_argument("symbol")
    p.add_argument("--limit", type=int, default=50)

    p = src_sub.add_parser("find_callees")
    p.add_argument("symbol")
    p.add_argument("--limit", type=int, default=50)

    p = src_sub.add_parser("get_class_hierarchy")
    p.add_argument("symbol")
    p.add_argument("--direction", default="both", choices=["up", "down", "both"])
    p.add_argument("--depth", type=int, default=5)

    p = src_sub.add_parser("get_module_info")
    p.add_argument("module_name")

    p = src_sub.add_parser("get_symbol_context")
    p.add_argument("symbol")
    p.add_argument("--context-lines", type=int, default=10)

    p = src_sub.add_parser("read_file")
    p.add_argument("file_path")
    p.add_argument("--start", type=int, default=1)
    p.add_argument("--end", type=int, default=0)

    p = src_sub.add_parser("get_include_path")
    p.add_argument("symbol")

    p = src_sub.add_parser("get_signature")
    p.add_argument("symbol")
    p.add_argument("--limit", type=int, default=10)

    p = src_sub.add_parser("check_deprecations")
    p.add_argument("symbols", nargs="+")

    p = src_sub.add_parser("verify_symbols")
    p.add_argument("symbols", nargs="+")

    p = src_sub.add_parser("find_example_usage")
    p.add_argument("symbol")
    p.add_argument("--prefer", default="engine", choices=["engine", "project"])
    p.add_argument("--limit", type=int, default=10)

    p = src_sub.add_parser("lint_header")
    p.add_argument("file_path")

    p = src_sub.add_parser("generate_class_stub")
    # Positional parent class_name module to match the exe sibling
    # (`monolith_query.exe source generate_class_stub <parent> <class_name> <module>`).
    p.add_argument("parent")
    p.add_argument("class_name")
    p.add_argument("module")

    # --- project namespace ---
    prj = sub.add_parser("project", help="Query project index database")
    prj_sub = prj.add_subparsers(dest="action", required=True)

    p = prj_sub.add_parser("search")
    p.add_argument("query")
    p.add_argument("--limit", type=int, default=50)
    p.add_argument("--asset-class", dest="asset_class", action="append",
                   help="Restrict to this asset class (repeatable, case-insensitive)")

    p = prj_sub.add_parser("find_by_type")
    p.add_argument("asset_class")
    p.add_argument("--limit", type=int, default=50)
    p.add_argument("--offset", type=int, default=0)

    p = prj_sub.add_parser("find_references")
    p.add_argument("asset_path")

    prj_sub.add_parser("get_stats")

    p = prj_sub.add_parser("get_asset_details")
    p.add_argument("asset_path")

    # --- cppreflect namespace (full RI) ---
    cpr = sub.add_parser("cppreflect", help="RI: C++ UCLASS/UPROPERTY/UFUNCTION structure")
    cpr_sub = cpr.add_subparsers(dest="action", required=True)

    p = cpr_sub.add_parser("get_uclass")
    p.add_argument("class_name")
    p.add_argument("--module_name", "--module", dest="module_name", default="")

    p = cpr_sub.add_parser("list_uproperties")
    # Class name is POSITIONAL to match the exe sibling
    # (`monolith_query.exe cppreflect list_uproperties <ClassName>`); --class_name kept as alias.
    p.add_argument("class_name_pos", nargs="?", default="")
    p.add_argument("--class_name", default="")
    p.add_argument("--blueprint_visible_only", action="store_true")
    _add_paginated(p)

    p = cpr_sub.add_parser("list_ufunctions")
    p.add_argument("class_name_pos", nargs="?", default="")
    p.add_argument("--class_name", default="")
    p.add_argument("--blueprint_callable_only", action="store_true")
    _add_paginated(p)

    p = cpr_sub.add_parser("find_interface_impls")
    p.add_argument("interface_name")

    p = cpr_sub.add_parser("find_class_specifier")
    p.add_argument("specifier_name")
    _add_paginated(p)

    cpr_sub.add_parser("list_class_specifiers")

    # --- network namespace (full RI) ---
    net = sub.add_parser("network", help="RI: replication audit")
    net_sub = net.add_subparsers(dest="action", required=True)

    p = net_sub.add_parser("list_replicated_classes")
    _add_paginated(p)

    p = net_sub.add_parser("list_rpc_functions")
    p.add_argument("--class_name", default="")
    p.add_argument("--rpc_kind", default="")
    _add_paginated(p)

    p = net_sub.add_parser("list_onrep_handlers")
    p.add_argument("--class_name", default="")
    _add_paginated(p)

    p = net_sub.add_parser("audit_unbalanced_onreps")
    _add_paginated(p)

    # --- decision namespace (full RI) ---
    dec = sub.add_parser("decision", help="RI: decision records")
    dec_sub = dec.add_subparsers(dest="action", required=True)

    p = dec_sub.add_parser("list_decisions")
    p.add_argument("--path_filter", default="")
    p.add_argument("--min_confidence", type=float, default=None)
    p.add_argument("--status", default="")
    _add_paginated(p)

    p = dec_sub.add_parser("get_decision")
    p.add_argument("decision_id")

    p = dec_sub.add_parser("list_stale")
    # OPTIONAL positional to match the exe sibling: the exe reads opt("max_age_days") then
    # positional, defaulting to 0 when omitted, then emits the clean "`max_age_days` must be
    # positive." error payload (FDecisionQueryAdapter.cpp:493-497 does the same). No numeric
    # default exists in spec/live; missing -> 0 -> clean error JSON (NOT an argparse exit-2).
    p.add_argument("max_age_days", type=int, nargs="?", default=0)
    p.add_argument("--max_age_days", dest="max_age_days_opt", type=int, default=None)
    p.add_argument("--path_filter", default="")
    _add_paginated(p)

    p = dec_sub.add_parser("find_supersession_chain")
    p.add_argument("decision_id")
    p.add_argument("--depth", type=int, default=10)

    p = dec_sub.add_parser("find_referent_decisions")
    p.add_argument("decision_id")

    # --- risk namespace (full RI) ---
    rsk = sub.add_parser("risk", help="RI: git hotspot / co-change / conditional-gate signals")
    rsk_sub = rsk.add_subparsers(dest="action", required=True)

    p = rsk_sub.add_parser("get_hotspot_score")
    p.add_argument("file_path")

    p = rsk_sub.add_parser("get_cochange_pairs")
    p.add_argument("file_path")
    _add_paginated(p)

    p = rsk_sub.add_parser("get_file_churn")
    p.add_argument("file_path")
    p.add_argument("--repo_tag", default="")

    p = rsk_sub.add_parser("get_release_window_hotspots")
    p.add_argument("--since_unix", type=int, default=None)
    _add_paginated(p)

    p = rsk_sub.add_parser("list_conditional_gates")
    p.add_argument("--macro_filter", default="")
    p.add_argument("--path_filter", default="")
    _add_paginated(p)

    return parser


def main():
    # --version short-circuit.
    if "--version" in sys.argv[1:]:
        print(PARITY_SPEC_REV)
        return

    # Unknown-namespace interception (before argparse) to emit the live error format.
    if len(sys.argv) >= 2 and not sys.argv[1].startswith("-"):
        requested_ns = sys.argv[1]
        if requested_ns not in OFFLINE_NAMESPACES:
            suffix = did_you_mean_suffix(requested_ns, OFFLINE_NAMESPACES + ["monolith"])
            msg = (f"Unknown namespace: {requested_ns} — call monolith_discover() "
                   f"to enumerate valid namespaces.{suffix}")
            emit_error(msg)
            sys.exit(2)

    # Unknown-action interception for RI namespaces (before argparse strict parsing).
    if len(sys.argv) >= 3 and sys.argv[1] in ACTIONS_BY_NS and not sys.argv[2].startswith("-"):
        ns = sys.argv[1]
        act = sys.argv[2]
        if act not in ACTIONS_BY_NS[ns]:
            suffix = did_you_mean_suffix(act, ACTIONS_BY_NS[ns])
            msg = (f"Unknown action: {ns}.{act} — call monolith_discover(\"{ns}\") "
                   f"to enumerate valid actions in this namespace.{suffix}")
            emit_error(msg)
            sys.exit(2)

    parser = build_parser()
    args = parser.parse_args()

    if not getattr(args, "namespace", None):
        parser.print_help()
        sys.exit(1)

    if args.namespace == "source":
        sa = SourceActions()
        getattr(sa, args.action)(args)
    elif args.namespace == "project":
        pa = ProjectActions()
        getattr(pa, args.action)(args)
    elif args.namespace in ("cppreflect", "network", "decision", "risk"):
        ra = ReflectionActions()
        getattr(ra, args.action)(args)


if __name__ == "__main__":
    main()
