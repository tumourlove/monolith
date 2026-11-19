# Attribution

## Concept Inspiration

**Codeturion** — [unreal-api-mcp](https://github.com/Codeturion/unreal-api-mcp)

The `unreal-api-mcp` project by Codeturion pioneered the concept of exposing Unreal Engine
API documentation and source intelligence through MCP tooling. Monolith's `MonolithSource`
module reimplements this functionality from scratch in C++/Python but credits Codeturion for
the original idea and approach. The original project is licensed under PolyForm Noncommercial
and is not bundled or derived from — only the concept is carried forward.

## Original tumourlove Servers

Monolith consolidates the following original MCP servers and C++ plugins, all authored by
tumourlove and now unified into this single package:

### Python MCP Servers (Replaced)
- `unreal-blueprint-mcp` — Blueprint graph inspection (5 tools)
- `unreal-editor-mcp` — Editor automation and build management (11 tools)
- `unreal-material-mcp` — Material creation and editing (46 tools)
- `unreal-niagara-mcp` — Niagara particle system authoring (70 tools)
- `unreal-animation-mcp` — Animation sequence and montage editing (62 tools)
- `unreal-config-mcp` — Config/INI resolution and search (6 tools)
- `unreal-source-mcp` — Engine source code indexing and search (9 tools)
- `unreal-project-mcp` — Project-wide asset indexing and search (17 tools)

### C++ Plugins (Absorbed)
- `BlueprintReader` — 5 UFUNCTIONs for blueprint graph data extraction
- `MaterialMCPReader` — 13 UFUNCTIONs for material graph inspection
- `AnimationMCPReader` — 23 UFUNCTIONs for animation asset introspection
- `NiagaraMCPBridge` — 39 UFUNCTIONs across 7 classes for Niagara system access

**Total: 231 original tools folded in — and vastly expanded since. Monolith today exposes ~1,500+ actions across 27 in-tree namespaces (query `monolith_discover()` for the live count).**
