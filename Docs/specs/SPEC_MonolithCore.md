# Monolith — MonolithCore Module

**Parent:** [SPEC_CORE.md](../SPEC_CORE.md)
**Engine:** Unreal Engine 5.7+
**Version:** 0.14.11 (Beta)

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
| `FBulkFillSpec` | USTRUCT (`BlueprintType`). Input shape for `bulk_fill_query("apply")`: `target_namespace`, `target` (asset path or class), nested JSON `tree`, plus `dry_run` / `strict` toggles. Same shape consumed by every per-namespace adapter registered via `FMonolithBulkFillRegistry`. |
| `FDryRunReport` | USTRUCT (`BlueprintType`). Output shape returned when `dry_run=true`. Carries per-field `FieldWrites`, `SilentDrops` (with reason — covers `FGameplayAttribute`-rename hazard class and other type-mismatch / unknown-field cases), `Clamps` (engine clamp annotations such as the AI `lose_sight_radius >= 1.1 × sight_radius` rule), `Errors`. Promoted to hard error and transaction-rollback when `strict=true`. |
| `FSchemaDescriptor` | USTRUCT (`BlueprintType`). Output shape returned by `describe_query("schema")`. Recursive tree of `FFieldDescriptor` nodes: type name, ImportText sample form, `range_min` / `range_max`, `enum_values`, `conditional_on` discriminators (for tagged-union fields like GE modifier magnitudes), nested struct/array/map/set children. Authoritative source for legal `set_cdo_properties` / `bulk_fill` payload grammar. |
| `FMonolithReflectionWalker` | Reflection-walker primitive that drives every bulk-fill adapter. Recursively walks UE 5.7 `FProperty` / `FStructProperty` / `FArrayProperty` / `FMapProperty` / `FSetProperty` / `FObjectProperty` / `FSoftObjectProperty` / `FEnumProperty` against a JSON tree. `InspectTree(...)` returns an `FDryRunReport` without mutation; `ApplyTree(...)` performs the writes under the caller-owned transaction. Single source of truth for ImportText parsing, type coercion, and clamp/enum validation. |
| `FMonolithBulkFillRegistry` | String-keyed singleton dispatcher. Per-namespace adapters call `RegisterAdapter(namespace, fn)` from their owning module's `StartupModule`; the central `bulk_fill_query("apply")` / `describe_query("schema")` handlers look up the adapter by `target_namespace` and delegate. **Zero compile-time linkage from `MonolithCore` into adapter modules** — preserves the Issue #30 / #32 hazard class (no hard imports of optional/conditional sibling modules). When a namespace adapter is absent or its module-level `WITH_*` gate is off, the registry returns a typed error rather than silently no-op'ing. |
| `FMonolithDryRunGuard` | RAII helper toggling the dry-run mode on a per-call basis. Adapters opt into the framework's `dry_run:true` preview-without-persist semantics by constructing this guard at the top of `ApplyTree`; on destruction the guard discards any provisional writes if dry-run was set. |

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

### Actions (2 — namespace: "bulk_fill")

Framework-level dispatchers for transacted JSON-tree writes against any registered per-namespace adapter. Adapters live in their owning modules (`MonolithBlueprint`, `MonolithGAS`, `MonolithUI`, `MonolithAI`, `MonolithNiagara`, `MonolithMaterial`, `MonolithAudio`, `MonolithMesh`, `MonolithAnimation`, `MonolithLogicDriver`, `MonolithComboGraph`, and the sibling-plugin `inventory` adapter under `MonolithISX`); `MonolithCore` owns only the framework primitives and the dispatch surface.

| Action | MCP Tool | Description |
|--------|----------|-------------|
| `apply` | `bulk_fill_query` | Apply a JSON `tree` to `target` under `target_namespace`. Routes to the per-namespace adapter via `FMonolithBulkFillRegistry`. Params: `target_namespace` (string), `target` (asset path or class identifier), `tree` (nested JSON), `dry_run` (bool, default false — returns full `FDryRunReport` without mutation), `strict` (bool, default false — promotes silent drops / clamps / unknown-fields to hard errors and cancels the transaction). Returns the populated `FDryRunReport` either way. |
| `list_namespaces` | `bulk_fill_query` | Enumerate registered adapter namespaces and their `available` flag (false when the owning module's `WITH_*` gate is off — e.g. `gas` adapter `available:false` when `WITH_GBA=0`). No params. Use before `apply` to confirm the target namespace is hot in the current build. |

### Actions (3 — namespace: "describe")

Read-only schema introspection companion to `bulk_fill`. Same 12 adapter namespaces, same registry.

| Action | MCP Tool | Description |
|--------|----------|-------------|
| `schema` | `describe_query` | Return the rich `FSchemaDescriptor` tree for `target` under `target_namespace`. Params: `target_namespace` (string), `target` (asset path or class — empty string returns a root descriptor enumerating every supported `fill_kind` for the namespace). Output includes ImportText sample forms, `range_min` / `range_max`, `enum_values`, `conditional_on` discriminators for tagged-union fields. Authoritative input source for authoring valid `bulk_fill.apply` payloads. |
| `list_targets` | `describe_query` | Enumerate the legal `target` shapes for a namespace's adapter (asset class names, fill_kind discriminators). Params: `target_namespace` (string). |
| `action_schema` | `describe_query` | Return a registered ACTION's param schema (names, types, required, defaults, aliases, descriptions) by `(target_namespace, action)`. Params: `target_namespace` (string), `action` (string). Closes param-name discoverability so callers stop trial-and-erroring param names. |

---
