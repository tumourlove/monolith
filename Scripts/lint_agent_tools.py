#!/usr/bin/env python3
"""Lint agent .md files for frontmatter tool-allowlist drift.

Walks every agent file in `.claude/agents/` and verifies that any
`mcp__monolith__<name>` dispatcher referenced in the prompt body is
also declared in the YAML frontmatter `tools:` line. ToolSearch's
`select:` operates over the agent's surfaced deferred-tool universe,
so anything absent from `tools:` is invisible to `select:` -- which
is exactly the F10 drift this script prevents.

Pure stdlib, Python 3.10+. Exit 0 on clean, 1 on violations.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

# Project root is two parents up from Plugins/Monolith/Scripts/.
SCRIPT_DIR = Path(__file__).resolve().parent
PROJECT_ROOT = SCRIPT_DIR.parent.parent.parent
AGENTS_DIR = PROJECT_ROOT / ".claude" / "agents"

DISPATCHER_RE = re.compile(r"mcp__monolith__[A-Za-z_]+")


def parse_tools_line(text: str) -> tuple[set[str], int] | None:
    """Extract the `tools:` frontmatter set and its line number.

    Returns (tool_set, line_number_1based) or None if no `tools:`
    line was found inside the leading `---` frontmatter block.
    """
    lines = text.splitlines()
    if not lines or lines[0].strip() != "---":
        return None

    for idx in range(1, len(lines)):
        line = lines[idx]
        if line.strip() == "---":
            return None  # Frontmatter ended without a tools: line.
        if line.lstrip().startswith("tools:"):
            value = line.split(":", 1)[1].strip()
            tools = {tok.strip() for tok in value.split(",") if tok.strip()}
            return tools, idx + 1
    return None


def find_dispatcher_refs(text: str, frontmatter_end: int) -> list[tuple[str, int]]:
    """Return (dispatcher_name, line_number_1based) for each prompt-body match.

    `frontmatter_end` is the 1-based line number of the closing `---`;
    we only scan AFTER that to avoid double-counting the tools: line.
    """
    refs: list[tuple[str, int]] = []
    for lineno, line in enumerate(text.splitlines(), start=1):
        if lineno <= frontmatter_end:
            continue
        for match in DISPATCHER_RE.finditer(line):
            refs.append((match.group(0), lineno))
    return refs


def find_frontmatter_end(text: str) -> int:
    """Return 1-based line number of the closing `---` (or 0 if absent)."""
    lines = text.splitlines()
    if not lines or lines[0].strip() != "---":
        return 0
    for idx in range(1, len(lines)):
        if lines[idx].strip() == "---":
            return idx + 1
    return 0


def lint_agent(path: Path) -> list[tuple[Path, str, int]]:
    """Return list of (path, missing_tool, line_number) violations."""
    text = path.read_text(encoding="utf-8")
    parsed = parse_tools_line(text)
    if parsed is None:
        # No tools: line -- can't lint this agent. Skip silently;
        # missing-frontmatter is a separate concern.
        return []
    tools, _tools_lineno = parsed
    fm_end = find_frontmatter_end(text)
    refs = find_dispatcher_refs(text, fm_end)

    violations: list[tuple[Path, str, int]] = []
    seen: set[tuple[str, int]] = set()
    for name, lineno in refs:
        if name in tools:
            continue
        key = (name, lineno)
        if key in seen:
            continue
        seen.add(key)
        violations.append((path, name, lineno))
    return violations


def main() -> int:
    if not AGENTS_DIR.is_dir():
        print(f"ERROR: agents directory not found: {AGENTS_DIR}", file=sys.stderr)
        return 2

    agent_files = sorted(AGENTS_DIR.glob("*.md"))
    all_violations: list[tuple[Path, str, int]] = []
    for path in agent_files:
        all_violations.extend(lint_agent(path))

    n = len(agent_files)
    if not all_violations:
        print(f"OK -- {n} agents linted, 0 violations.")
        return 0

    print(f"FAIL -- {n} agents linted, {len(all_violations)} violation(s):")
    for path, name, lineno in all_violations:
        print(f"  {path.name}: missing '{name}' (referenced at line {lineno})")
    return 1


if __name__ == "__main__":
    sys.exit(main())
