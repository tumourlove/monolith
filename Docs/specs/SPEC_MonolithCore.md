# Monolith — MonolithCore Module

**Parent:** [SPEC_CORE.md](../SPEC_CORE.md)
**Engine:** Unreal Engine 5.7+
**Version:** 0.14.1 (Beta)

---

## MonolithCore

**Dependencies:** Core, CoreUObject, Engine, HTTP, HTTPServer, Json, JsonUtilities, Slate, SlateCore, DeveloperSettings, Projects, AssetRegistry, EditorSubsystem, UnrealEd

### Classes

| Class | Responsibility |
|-------|---------------|
| `FMonolithCoreModule` | IModuleInterface. Starts HTTP server, registers core tools, owns `TUniquePtr<FMonolithHttpServer>` |
| `FMonolithHttpServer` | Embedded MCP HTTP server. JSON-RPC 2.0 dispatch over HTTP. Fully stateless (no session tracking). `tools/list` response embeds per-action param schemas in the `params` property description (`*name(type)` format, `*` = required) so AI clients see param names without calling `monolith_discover` first |
| `FMonolithToolRegistry` | Central singleton action registry. `TMap<FString, FRegisteredAction>` keyed by "namespace.action". Thread-safe — releases lock before executing handlers. Validates required params from schema before dispatch (skips `asset_path` — `GetAssetPath()` handles aliases itself). Returns descriptive error listing missing + provided keys |
| `FMonolithJsonUtils` | Static JSON-RPC 2.0 helpers. Standard error codes (-32700 through -32603). Declares `LogMonolith` category |
| `FMonolithAssetUtils` | Asset loading with 4-tier fallback: StaticLoadObject(resolved) -> PackageName.ObjectName -> FindObject+_C suffix -> ForEachObjectWithPackage |
| `UMonolithSettings` | UDeveloperSettings (config=Monolith). ServerPort, bAutoUpdateEnabled, DatabasePathOverride, EngineSourceDBPathOverride, EngineSourcePath, 10 module enable toggles + `bEnableProceduralTownGen` (experimental, default false) (functional — checked at registration time), LogVerbosity. Settings UI customized via `FMonolithSettingsCustomization` (IDetailCustomization) with re-index buttons for project and source databases |
| `UMonolithUpdateSubsystem` | UEditorSubsystem. GitHub Releases auto-updater. Shows dialog window with full release notes on update detection. Downloads zip, cross-platform extraction (PowerShell on Windows, unzip on Mac/Linux). Stages to Saved/Monolith/Staging/, hot-swaps on editor exit via FCoreDelegates::OnPreExit. Current version always from compiled MONOLITH_VERSION (version.json only stores pending/staging state). Release zips include pre-compiled DLLs. |
| `FMonolithCoreTools` | Registers 4 core actions |

### Helpers

| Symbol | Header | Responsibility |
|--------|--------|---------------|
| `MonolithCore::ValidatePackagePath(const FString&)` | `MonolithPackagePathValidator.h` (inline) | Wraps `FPackageName::IsValidLongPackageName` with an empty-string-on-success / error-msg-on-failure contract. Rejects empty input, double-slash (`//Game/...`), missing `/Game/` root, trailing slash, illegal chars. Added `dv.367` after a fatal `UObjectGlobals.cpp:1012` ensure from a malformed `//Game/...` JSON payload reaching `CreatePackage`. Currently routed at three sites: `HandleCreateWidgetBlueprint` (direct crash site), `MonolithAIInternal::GetOrCreatePackage` (~17 AI callers), `MonolithGASInternal::GetOrCreatePackage` (~6 GAS callers). ~24 of 80 `CreatePackage` call sites guarded; remaining ~56 sites across MonolithBlueprint / MonolithMaterial / MonolithLogicDriver / MonolithUITemplateActions / MonolithCommonUI* / MonolithMesh are follow-up backlog. |

### Actions (4 — namespace: "monolith")

| Action | MCP Tool | Description |
|--------|----------|-------------|
| `discover` | `monolith_discover` | List available tool namespaces and their actions. Optional `namespace` filter |
| `status` | `monolith_status` | Server health: version, uptime, port, action count, engine_version, project_name |
| `update` | `monolith_update` | Check/install updates from GitHub Releases. `action`: "check" or "install" |
| `reindex` | `monolith_reindex` | Trigger project re-index. Defaults to incremental (hash-based delta); pass `force=true` for full wipe-and-rebuild (via reflection to MonolithIndex, no hard dependency) |

---
