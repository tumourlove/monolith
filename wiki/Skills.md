# Skills

Monolith bundles 15 Claude Code skills in `Skills/` for domain-specific workflows. These teach the AI how to use Monolith's tools effectively for each domain. Two reference packs (`material-reference`, `niagara-reference`) ship alongside as deeper-dive material/Niagara reference content.

## Installation

```bash
cp -r Plugins/Monolith/Skills/* ~/.claude/skills/
```

## Available Skills

| Skill | Description |
|-------|-------------|
| `unreal-blueprints` | Full Blueprint CRUD — read, create, edit variables/components/functions/nodes, compile |
| `unreal-materials` | PBR setup, graph building, validation |
| `unreal-animation` | Montages, ABP state machines, blend spaces |
| `unreal-niagara` | Particle system creation, HLSL modules, scalability |
| `unreal-audio` | Sound Cue + MetaSound graph building, audio asset CRUD, batch ops, templates |
| `unreal-mesh` | Mesh inspection, spatial queries, blockout, procedural geometry, horror/accessibility |
| `unreal-ui` | Widget Blueprint CRUD, templates, styling, accessibility |
| `unreal-gas` | Gameplay Ability System — abilities, effects, attributes, ASC, tags, cues |
| `unreal-logicdriver` | Logic Driver Pro state machines — SM CRUD, graph editing, JSON spec, scaffolding |
| `unreal-combograph` | ComboGraph combo trees — graph CRUD, nodes, edges, effects, ability scaffolding |
| `unreal-debugging` | Build errors, log search, crash context |
| `unreal-performance` | Config auditing, shader stats, INI tuning |
| `unreal-project-search` | FTS5 search syntax, reference tracing |
| `unreal-cpp` | API lookup, include paths, Build.cs gotchas |
| `unreal-build` | Smart build decision-making, Live Coding vs full rebuild |

## How Skills Work

Skills are workflow guides that Claude Code loads automatically when relevant. They tell the AI:
- Which Monolith actions to use for specific tasks
- The correct order of operations
- Common pitfalls and how to avoid them
- Parameter formats and expected outputs
