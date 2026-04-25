# Monolith — MonolithBABridge Module

**Parent:** [SPEC_CORE.md](../SPEC_CORE.md)
**Engine:** Unreal Engine 5.7+
**Version:** 0.14.1 (Beta)

---

## MonolithBABridge

**Dependencies:** Core, CoreUObject, Engine, MonolithCore (optional — loads only when both Monolith and Blueprint Assist are present)

MonolithBABridge is an **optional** editor module that bridges Blueprint Assist's graph formatter into Monolith's `auto_layout` actions. It registers no MCP actions of its own. Its sole job is to expose BA's layout logic via `IModularFeatures` so that blueprint, material, animation, and niagara modules can consume it without a hard dependency on Blueprint Assist.

### Classes

| Class | Responsibility |
|-------|---------------|
| `FMonolithBABridgeModule` | IModuleInterface. On startup, checks for Blueprint Assist via `FModuleManager::IsModuleLoaded("BlueprintAssist")` and registers `IMonolithGraphFormatter` impl via `IModularFeatures::Get().RegisterFeature()` |
| `FMonolithBAGraphFormatter` | Concrete `IMonolithGraphFormatter` impl. Delegates to BA's `FBAFormatterUtils` / `FBANodePositioner`. Reads cached node sizes from `FBACache` when available |

### IMonolithGraphFormatter Interface

```cpp
class IMonolithGraphFormatter
{
public:
    virtual ~IMonolithGraphFormatter() = default;

    /** Feature name used with IModularFeatures */
    static const FName FeatureName;

    /**
     * Format a graph using the registered formatter.
     * @param Graph  Target graph to layout
     * @return true if layout was applied
     */
    virtual bool FormatGraph(UEdGraph* Graph) = 0;
};
```

Consumer pattern used by `auto_layout` actions in each domain module:

```cpp
// Check at call time — BA may not be loaded
if (IModularFeatures::Get().IsFeatureAvailable(IMonolithGraphFormatter::FeatureName))
{
    auto& Formatter = IModularFeatures::Get().GetFeature<IMonolithGraphFormatter>(
        IMonolithGraphFormatter::FeatureName);
    Formatter.FormatGraph(Graph);
}
```

### `formatter` Parameter (three-mode behavior)

All four `auto_layout` actions accept an optional `formatter` param:

| Value | Behavior |
|-------|----------|
| `"auto"` (default) | Uses Blueprint Assist formatter if `IMonolithGraphFormatter` is registered; otherwise falls back to built-in hierarchical layout. Never errors |
| `"blueprint_assist"` | Forces BA formatter. Returns an error if MonolithBABridge is not loaded or BA is not present |
| `"builtin"` | Forces built-in layout regardless of BA presence |

### `bEnableBlueprintAssist` Setting

`UMonolithSettings` exposes a toggle that controls whether MonolithBABridge attempts registration on startup:

| Setting | Default | Description |
|---------|---------|-------------|
| `bEnableBlueprintAssist` | True | When false, MonolithBABridge skips `IModularFeatures` registration even if Blueprint Assist is present. `formatter: "auto"` will fall back to built-in; `formatter: "blueprint_assist"` will error |

**Config key:** `bEnableBlueprintAssist` in `[/Script/MonolithCore.MonolithSettings]`
