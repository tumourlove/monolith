# Configuration

Plugin settings are at **Project Settings > Plugins > Monolith**.

| Setting | Default | Description |
|---------|---------|-------------|
| MCP Server Port | `9316` | Port for the embedded HTTP server |
| Auto-Update | `Off` (as of v0.14.6) | Check GitHub Releases on editor startup. Opt-in. |
| Module Toggles | All enabled | Enable/disable individual domain modules |
| Database Path | Project-local | Override SQLite database storage location |
| Index Marketplace Plugins | `On` | Index content from installed marketplace/Fab plugins |
| Index Data Assets | `On` | Deep-index DataAsset subclasses (14 indexers) |
| Additional Content Paths | `[]` | Extra content paths to include in the project index |
| Enable Procedural Town Gen | `Off` | Experimental: 45 additional mesh actions for procedural town generation. Known geometry issues — disabled by default |
| Enable Gameplay Tags | `On` | `gameplay_tags` namespace (engine GameplayTagsEditor plugin, on by default). Independent of the GAS toggle |
| MCP Server Enabled | `On` | Untick to stop the editor listening on port 9316 (restart required) |
| Enable GAS | `On` | Gameplay Ability System integration (requires GameplayAbilities plugin) |
| Enable Logic Driver | `On` | Logic Driver Pro state machine integration (requires Logic Driver Pro marketplace plugin) |
| Enable ComboGraph | `On` | ComboGraph combo tree integration (requires ComboGraph marketplace plugin) |
| Enable Blueprint Assist | `On` | Enhanced auto_layout via Blueprint Assist (requires BA marketplace plugin) |

## Module Toggles

Each domain module can be independently enabled or disabled. When disabled, a module's actions are not registered with the MCP server and won't appear in `monolith_discover()`.

Optional modules (GAS, Logic Driver, ComboGraph, Blueprint Assist bridge, CommonUI features in MonolithUI, MetaSound features in MonolithAudio) are automatically disabled if their required third-party plugins aren't installed — no manual toggle needed. Detection happens at build time via `Build.cs` 3-location probes; the corresponding `WITH_*` macro gates the action registration.

## Reflection Intelligence settings (new in v0.17.0)

The `MonolithReflectionIntel` module ships its own settings page at **Project Settings > Plugins > "Monolith Reflection Intel"**. Section: `[/Script/MonolithReflectionIntel.MonolithReflectionIntelSettings]` in `Config/MonolithSettings.ini`.

| Setting | Default | Description |
|---------|---------|-------------|
| Enable Decision Mining | `On` | When off, `decision_query` returns a status string instead of running the markdown harvest |
| Decision Min Confidence | `0.6` | Heuristic confidence floor for `list_decisions` results. Clamped `[0, 1]`. Per-call `min_confidence` always wins over this default. |
| Decision Markdown Roots | `[]` (auto) | Project-relative paths the indexer scans. Empty defaults to `Docs/`, `Plugins/Monolith/Docs/`, `.claude/rules/`. |
| Index Project Plugin Reflection | `On` | When on, the CppReflect + Network indexers scan every enabled `LoadedFrom == Project` plugin's UHT artefacts (your project plugins) in addition to the game module. This is what brings project-plugin replicated classes and RPCs into scope. Set off to fall back to game-module-only scanning. |
| Index Marketplace Plugin Reflection | `Off` | When on, ALSO scans enabled engine-installed marketplace plugins (`LoadedFrom == Engine` under `/Plugins/Marketplace/`, e.g. installed marketplace plugins). Off by default — marketplace surface is usually relevant but noisier. Epic engine built-ins remain governed separately by Index Engine Plugin Reflection. |
| Index Engine Plugin Reflection | `Off` | When off, the CppReflect/Network sweep restricts to the game module + project plugins (and marketplace plugins when enabled), excluding Epic engine built-in plugins. Engine-side surface area floods low-signal hits. |
| UHT Artefact Root | `""` (auto) | When empty, resolves to `FPaths::ProjectIntermediateDir() / TEXT("Build")`. Set explicitly only for non-standard intermediate layouts. |
| Enable Git Co-Change Mining | `On` | When off, all three Phase 2 indexers short-circuit at `Run()` entry |
| Max Co-Change Window Commits | `50` | Caps the commit-history window walked per repo for pair detection. Clamped `[10, 500]`. |
| Max Commit File Count | `50` | Caps per-commit file touches before pair emission. Commits exceeding the cap (tree-wide refactors, initial imports) contribute zero pairs. Clamped `[5, 500]`. |
| Git Mining Noise Filter | `["Saved/*", "Intermediate/*", "Binaries/*", "*.uasset", "*.umap"]` | Blacklists file patterns before churn / pair aggregation |
| Enable Network Replication Audit | `On` | When off, `FNetworkIndexer::Run` short-circuits and `network_query` returns a status string per call |
| Enable Pipeline Composers | `On` | When off, both `pipeline_query` composers return a status string instead of fanning out. The four cross-namespace audit actions on `material` / `niagara` / `blueprint` / `project` are NOT gated by this — they continue to function. |

## Port Conflicts

If port 9316 is already in use:

1. Change the port in Project Settings
2. Update `.mcp.json` to match the new port
3. Restart both the editor and your MCP client
