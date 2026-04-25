# Contributing to Monolith

Thanks for your interest in contributing. This guide covers everything you need to get started.

## Dev Environment Setup

### Prerequisites

- **Unreal Engine 5.7+** (source or launcher build)
- **Windows, macOS, or Linux** — see [README Installation](README.md#installation) for per-platform proxy setup
- **Python 3.10+** (only needed for engine source indexing and for the cross-platform MCP proxy on macOS/Linux)
- **Git**

### Clone & Build

```bash
# Clone into your project's Plugins directory
cd YourProject/Plugins
git clone https://github.com/tumourlove/monolith.git Monolith

# Or clone the standalone development repo
git clone https://github.com/tumourlove/monolith.git C:\Projects\Monolith
```

Generate project files and build from your UE project as usual. Monolith is an editor-only plugin — all 13 modules have `Type: "Editor"`.

### Development Workflow

Clone the repo into your UE project's `Plugins/` folder and develop in-place:

```
YourProject/Plugins/Monolith/   — edit, build, commit, push from here
```

---

## Code Structure

Monolith has 13 modules, each owning a specific domain:

| Module | Namespace | Actions | What It Does |
|--------|-----------|---------|--------------|
| **MonolithCore** | `monolith` | 4 | HTTP server, tool registry, discovery, settings, auto-updater |
| **MonolithBlueprint** | `blueprint` | 86 | Blueprint read/write, variable/component/graph CRUD, node operations, compile, auto-layout |
| **MonolithMaterial** | `material` | 57 | Material graph editing, inspection, CRUD, instances, functions, HLSL |
| **MonolithAnimation** | `animation` | 115 | Sequences, montages, ABPs, curves, notifies, skeletons, PoseSearch, IKRig, Control Rig |
| **MonolithNiagara** | `niagara` | 96 | Particle systems, emitters, modules, renderers, HLSL, dynamic inputs, event handlers, sim stages |
| **MonolithMesh** | `mesh` | 242 | Mesh inspection, scene manipulation, spatial queries, blockout, procedural geometry, lighting, audio, town gen (197 core + 45 experimental) |
| **MonolithEditor** | `editor` | 19 | Build triggers, live compile, log capture, crash context, scene capture, texture import |
| **MonolithConfig** | `config` | 6 | INI resolution, explain, diff, search |
| **MonolithIndex** | `project` | 7 | SQLite FTS5 deep project indexer |
| **MonolithSource** | `source` | 11 | Engine source lookup, call graphs, class hierarchy |
| **MonolithUI** | `ui` | 42 | Widget Blueprint CRUD, templates, styling, animation, settings scaffolding, accessibility |
| **MonolithGAS** | `gas` | 130 | Gameplay Ability System: abilities, attributes, effects, ASC, tags, cues, targeting, input, inspect, scaffold |
| **MonolithBABridge** | — | 0 | Optional Blueprint Assist integration bridge (no MCP actions — integration only) |

Each module follows the same file structure:

```
Source/MonolithFoo/
  Public/
    MonolithFooModule.h
    MonolithFooActions.h
  Private/
    MonolithFooModule.cpp
    MonolithFooActions.cpp
```

---

## How to Add a New Action

Actions are the atomic units of functionality. Each domain module registers actions with the central `FMonolithToolRegistry`.

### 1. Declare the handler

In your module's `Actions.h`, add a static method:

```cpp
static TSharedPtr<FJsonObject> HandleMyAction(const TSharedPtr<FJsonObject>& Params);
```

### 2. Implement the handler

In your module's `Actions.cpp`:

```cpp
TSharedPtr<FJsonObject> FMonolithFooActions::HandleMyAction(const TSharedPtr<FJsonObject>& Params)
{
    // Extract params
    FString AssetPath = Params->GetStringField(TEXT("asset_path"));

    // Do work (on game thread — handlers run on game thread via AsyncTask)

    // Return result
    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("status"), TEXT("success"));
    return Result;
}
```

### 3. Register in StartupModule

In your module's `Module.cpp`:

```cpp
void FMonolithFooModule::StartupModule()
{
    FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();

    Registry.RegisterAction(
        TEXT("foo"),                    // namespace
        TEXT("my_action"),             // action name
        TEXT("Description of what it does"),
        TEXT("{\"asset_path\": \"string (required)\"}"),  // param schema
        &FMonolithFooActions::HandleMyAction
    );
}
```

### 4. Update the skill

If your domain has a skill in `Skills/`, add the new action to its action table.

---

## How to Add a New Indexer

MonolithIndex uses a plugin-style indexer system. Each indexer implements `IMonolithIndexer`.

### 1. Create the indexer class

```cpp
class FMyIndexer : public IMonolithIndexer
{
public:
    virtual TArray<UClass*> GetSupportedClasses() const override
    {
        return { UMyAssetClass::StaticClass() };
    }

    virtual void IndexAsset(
        FMonolithIndexDatabase& DB,
        const FAssetData& AssetData,
        UObject* LoadedAsset) override
    {
        // Extract data and write to DB using prepared statements
        DB.InsertNode(AssetId, NodeName, NodeClass, NodeType);
    }

    virtual FString GetName() const override { return TEXT("MyIndexer"); }
};
```

### 2. Register in the subsystem

Add your indexer to `UMonolithIndexSubsystem::Initialize()`:

```cpp
Indexers.Add(MakeUnique<FMyIndexer>());
```

### 3. Add DB tables if needed

If your indexer needs new tables, add the schema in `FMonolithIndexDatabase::CreateSchema()`. Follow the existing pattern with `CREATE TABLE IF NOT EXISTS`.

---

## Coding Conventions

### General

- **UE coding standard** — `F` prefix for structs, `U` for UObjects, `T` for templates, `b` prefix for bools
- **Static action handlers** — All action classes use static methods, no instance state
- **Game thread execution** — Handlers execute on the game thread via `AsyncTask(ENamedThreads::GameThread, ...)`

### Logging

Use the `LogMonolith` category for all log output:

```cpp
UE_LOG(LogMonolith, Log, TEXT("Something happened: %s"), *Value);
UE_LOG(LogMonolith, Warning, TEXT("Something unexpected: %s"), *Value);
UE_LOG(LogMonolith, Error, TEXT("Something failed: %s"), *Error);
```

Do **not** use `LogTemp`.

### Database Access

All SQL must use prepared statements to prevent injection:

```cpp
FSQLitePreparedStatement Stmt;
Stmt.Create(*Database, TEXT("INSERT INTO nodes (asset_id, name) VALUES (?, ?)"));
Stmt.SetBindingValueByIndex(1, AssetId);
Stmt.SetBindingValueByIndex(2, NodeName);
Stmt.Execute();
```

Never use string formatting to build SQL queries.

### Error Handling

Return errors as JSON with a clear message:

```cpp
TSharedPtr<FJsonObject> Error = MakeShared<FJsonObject>();
Error->SetStringField(TEXT("error"), TEXT("Asset not found"));
Error->SetStringField(TEXT("asset_path"), AssetPath);
return Error;
```

### Asset Loading

Use the 4-tier fallback in `FMonolithAssetUtils`:

```cpp
UBlueprint* BP = FMonolithAssetUtils::LoadAssetByPath<UBlueprint>(AssetPath);
```

This handles: StaticLoadObject -> PackageName.ObjectName -> FindObject+_C suffix -> ForEachObjectWithPackage.

---

## Testing

Monolith exposes a Streamable HTTP MCP server. You can test with curl or any MCP client.

### curl Examples

**Discover available tools:**
```bash
curl -X POST http://localhost:9316/mcp \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":1,"method":"tools/list"}'
```

**Call an action:**
```bash
curl -X POST http://localhost:9316/mcp \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"blueprint_query","arguments":{"action":"list_graphs","asset_path":"/Game/MyBlueprint.MyBlueprint"}}}'
```

**Check server status:**
```bash
curl -X POST http://localhost:9316/mcp \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"monolith_status","arguments":{}}}'
```

### MCP Client

Configure your `.mcp.json` (see `Templates/.mcp.json.example`):

```json
{
  "mcpServers": {
    "monolith": {
      "type": "streamableHttp",
      "url": "http://localhost:9316/mcp"
    }
  }
}
```

Then use Claude Code or any MCP-compatible client to interact with the tools.

### What to Verify

- Your action appears in `monolith_discover` output
- Valid params return correct results
- Missing/invalid params return clear error JSON (not crashes)
- Asset paths with various formats work (the 4-tier fallback)

---

## Pull Request Process

1. **Branch from `master`** — Use descriptive branch names: `feature/niagara-scalability`, `fix/material-connection-crash`

2. **Test in-editor** — Build and run in your UE project. Verify with curl or an MCP client that your changes work

4. **Update docs** — If you add actions, update:
   - The relevant skill in `Skills/`
   - `Docs/specs/SPEC_<Module>.md` action tables (per-module spec for the namespace you touched)
   - `README.md` action counts (if totals change)

5. **Commit messages** — Use conventional format: `feat:`, `fix:`, `docs:`, `refactor:`

6. **One concern per PR** — Don't mix unrelated changes

---

## Architecture Notes

- **Discovery/dispatch pattern** — Each domain exposes one `{namespace}_query(action, params)` MCP tool. The registry dispatches to the correct handler. This keeps AI context lean (15 tools instead of 815 individual endpoints).
- **Thread safety** — `FMonolithToolRegistry` releases its lock before executing handlers. DB access uses `FCriticalSection`.
- **Stateless server** — No session tracking. Every request is independent.
- **MCP protocol version** — 2025-03-26, Streamable HTTP transport.

---

## License

By contributing, you agree that your contributions will be licensed under the [MIT License](LICENSE).
