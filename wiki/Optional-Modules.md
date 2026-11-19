# Optional Modules

Extend Monolith with new MCP action namespaces for any third-party plugin — without breaking the build for users who don't own it.

---

## Table of Contents

- [What Are Optional Modules?](#what-are-optional-modules)
- [Two Patterns](#two-patterns)
- [Tutorial: Adding a New Optional Module](#tutorial-adding-a-new-optional-module)
- [What CAN and CANNOT Go in #if Blocks](#what-can-and-cannot-go-in-if-blocks)
- [The Blueprint Assist Bridge (Reference Implementation)](#the-blueprint-assist-bridge-reference-implementation)
- [Tips for LLM Agents](#tips-for-llm-agents)
- [Binary Distribution](#binary-distribution)

---

## What Are Optional Modules?

### The Problem

Monolith is a single `.uplugin` with ~1,500+ actions across 27 in-tree namespaces (MonolithCore alone exposes the `monolith` meta-namespace plus the `bulk_fill` + `describe` ergonomics framework, and v0.17.0 plus the [Unreleased] `reflect` follow-up added six more from `MonolithReflectionIntel`). All modules compile into DLLs that load together. If any module links against a DLL that doesn't exist on the user's machine, the **entire plugin fails to load**. UE shows an error dialog, and every Monolith action is dead.

There is no `bOptional` flag on module descriptors. `ConditionallyLoadedModuleNames` was removed in UE 5.7. `SoftDependencies` on `.uplugin` files never existed. You cannot gracefully degrade at the module level within a single plugin.

This means you can't just add `GBAPlugin` to your module's dependency list and hope for the best. If the user doesn't own GBA, they can't use Monolith at all.

### The Solution

**Compile-time detection + empty shell pattern.**

`Build.cs` runs as C# at build time. It can probe the filesystem with `Directory.Exists()` to check whether a third-party plugin is installed. If found, it adds the dependency and defines a preprocessor symbol (`WITH_FOO=1`). If not, it defines `WITH_FOO=0`. The C++ code uses `#if WITH_FOO` guards around all third-party includes and API calls. When the plugin is absent, the module compiles to a tiny stub that loads, does nothing, and gets out of the way.

One `.uplugin`. No extra DLLs to manage. No error dialogs. Users who don't own the third-party plugin never know the integration exists. Users who do get new MCP actions automatically.

### Why You'd Want This

You have a marketplace plugin or custom tool and you want AI agents to interact with it through Monolith's MCP server. Examples:

- **GAS via GBAPlugin** — `gba_query` namespace for listing abilities, inspecting attribute sets, querying gameplay effects
- **ComboGraph** — `combograph_query` for managing combo trees and state graphs
- **DialogueTree** — `dialogue_query` for reading/writing dialogue nodes and conditions
- **Inventory systems** — `inventory_query` for inspecting item databases and container configurations
- **AI behavior** — `behaviortree_query` for external BT plugin integrations
- **Your own custom editor tools** — any C++ plugin with an API you want to expose to AI

Each optional module registers its own MCP tool namespace. When `monolith_discover` runs, the new namespace shows up alongside the core ones. AI agents call it the same way they call `blueprint_query` or `material_query` — JSON in, JSON out.

---

## Two Patterns

Monolith uses two complementary patterns for optional dependencies. Choose based on **who the caller is**.

### Pattern 1: Direct FMonolithToolRegistry (Most Common)

**Use when:** Your module adds a new MCP action namespace that AI agents call directly.

The module registers actions into `FMonolithToolRegistry` — the same singleton that all core modules use. The HTTP server picks them up automatically. AI agents discover them via `monolith_discover` and call them via `{namespace}_query(action, params)`.

```
AI Agent  -->  MCP HTTP Server  -->  FMonolithToolRegistry  -->  Your action handler
                                          ^
                                          |
                              MonolithFoo registers here
```

This is the pattern for GBA, ComboGraph, and most third-party integrations. Your module owns its namespace entirely — it registers actions in `StartupModule()` and unregisters in `ShutdownModule()`. No core module changes needed.

### Pattern 2: IModularFeatures Bridge (Less Common)

**Use when:** A core Monolith module needs to optionally call into your plugin's C++ API at action time.

The abstract interface lives in `MonolithCore` (always compiled). The implementation lives in a bridge module that depends on the third-party plugin. The core module calls `IsAvailable()` at runtime before touching the interface. Zero compile-time coupling between the core module and the third-party API.

```
MonolithBlueprint  -->  IMonolithGraphFormatter::IsAvailable()
                            |
                            v  (if registered)
                        FMonolithBAFormatterImpl  -->  Blueprint Assist API
```

This is currently used for one thing: the `auto_layout` action in MonolithBlueprint optionally delegates to Blueprint Assist's formatter via `IMonolithGraphFormatter`.

### When to Use Which

| Scenario | Pattern |
|----------|---------|
| AI agent calls your optional MCP actions (`foo_query`) | **FMonolithToolRegistry** |
| Core C++ module calls your optional C++ API at action time | **IModularFeatures bridge** |
| Both — your module owns MCP actions AND core needs your C++ API | **Both simultaneously** |

**Rule of thumb:** If the consumer is JSON, use the registry. If the consumer is C++ inside a core module, use IModularFeatures.

---

## Tutorial: Adding a New Optional Module

This walkthrough creates a hypothetical `MonolithFoo` module that adds a `foo_query` namespace for a marketplace plugin called "FooPlugin". By the end, AI agents will be able to call `foo_query("list_foos", {})` and get results.

### Step 1: Create the Module Directory

```
Source/MonolithFoo/
    MonolithFoo.Build.cs
    Private/
        MonolithFooModule.cpp
        MonolithFooActions.h
        MonolithFooActions.cpp
```

Four files. That's it.

### Step 2: Write Build.cs

This is the critical file. It detects whether FooPlugin is installed and sets the preprocessor define accordingly.

```csharp
// Source/MonolithFoo/MonolithFoo.Build.cs
using UnrealBuildTool;
using System.IO;

public class MonolithFoo : ModuleRules
{
    public MonolithFoo(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        // ---- Probe for FooPlugin on disk ----
        bool bHasFoo = false;

        // 1. Check project Plugins/ folder (manual install, git clone, symlink)
        string ProjectPluginsDir = Path.Combine(
            Target.ProjectFile.Directory.FullName, "Plugins");
        if (Directory.Exists(ProjectPluginsDir))
        {
            // Exact name first (manual install typically keeps the clean name)
            bHasFoo = Directory.Exists(
                Path.Combine(ProjectPluginsDir, "FooPlugin"));

            // Marketplace obfuscated name fallback.
            // When installed via Fab/Epic Games Launcher, the directory name is
            // an obfuscated slug like "FooPlug1a2b3c4d5e6fV3". The prefix derives
            // from the marketplace product name and is stable across versions.
            // Find YOUR plugin's prefix by checking a Fab install on disk.
            if (!bHasFoo)
            {
                bHasFoo = Directory.GetDirectories(
                    ProjectPluginsDir, "FooPlug*",
                    SearchOption.TopDirectoryOnly).Length > 0;
            }
        }

        // 2. Check Engine Plugins/Marketplace/ folder (Fab/launcher install)
        if (!bHasFoo)
        {
            string EngineDir = Path.GetFullPath(Target.RelativeEnginePath);
            string MarketplaceDir = Path.Combine(EngineDir, "Plugins", "Marketplace");
            if (Directory.Exists(MarketplaceDir))
            {
                bHasFoo = Directory.GetDirectories(
                    MarketplaceDir, "FooPlug*",
                    SearchOption.TopDirectoryOnly).Length > 0;
            }

            // 3. Check Engine Plugins/ root (some installs land here)
            if (!bHasFoo)
            {
                string EnginePluginsDir = Path.Combine(EngineDir, "Plugins");
                bHasFoo = Directory.Exists(
                    Path.Combine(EnginePluginsDir, "FooPlugin"));
            }
        }

        // ---- Configure dependencies based on detection ----
        if (bHasFoo)
        {
            PrivateDependencyModuleNames.AddRange(new string[]
            {
                "Core", "CoreUObject", "Engine",
                "MonolithCore",
                "Json", "JsonUtilities",
                "FooPlugin"          // The third-party module name
            });
            PublicDefinitions.Add("WITH_FOO=1");
        }
        else
        {
            // Empty shell — compiles clean, does nothing at runtime
            PrivateDependencyModuleNames.AddRange(new string[]
            {
                "Core", "CoreUObject", "Engine",
                "MonolithCore"
            });
            PublicDefinitions.Add("WITH_FOO=0");
        }
    }
}
```

**Key points:**

- **Always define to 0 or 1**, never leave it undefined. `#if WITH_FOO` on an undefined symbol is 0 anyway, but you'll get "undefined macro" warnings.
- **Use `PublicDefinitions`**, not `PrivateDefinitions`. This way header files can use the guard too.
- **Check three locations**: project `Plugins/`, engine `Plugins/Marketplace/`, engine `Plugins/`. Fab installs go to different places depending on the product and UE version.
- **Obfuscated directory names** are real. Marketplace plugins installed via Fab get slugified names like `Gameplaya1dbec2bf155V7` (that's GBA). You must find your plugin's prefix by examining an actual Fab install. The prefix is stable across versions but unique per product.
- **Use `PrivateDependencyModuleNames`** for third-party modules (not `PublicDependencyModuleNames`) unless you're exposing their types in your public headers.

> **UBT Caching Warning:** UBT caches `Build.cs` evaluation results aggressively. If a user installs or removes FooPlugin after a previous build, UBT may not re-evaluate `MonolithFoo.Build.cs` because no MonolithFoo source files changed. The `WITH_FOO` define will be stale. **Users must do a full rebuild** (delete `Intermediate/` or Build > Rebuild Solution) after installing or removing third-party plugins.

### Step 3: Write the Module .cpp

```cpp
// Source/MonolithFoo/Private/MonolithFooModule.cpp

#include "Modules/ModuleManager.h"
#include "MonolithSettings.h"
#include "MonolithFooActions.h"

DECLARE_LOG_CATEGORY_EXTERN(LogMonolithFoo, Log, All);
DEFINE_LOG_CATEGORY(LogMonolithFoo);

class FMonolithFooModule : public IModuleInterface
{
public:
    virtual void StartupModule() override
    {
        // Check settings toggle — lets users disable without uninstalling
        const UMonolithSettings* Settings = GetDefault<UMonolithSettings>();
        if (!Settings || !Settings->bEnableFoo)
        {
            UE_LOG(LogMonolithFoo, Log,
                TEXT("MonolithFoo: Foo integration disabled in settings"));
            return;
        }

        FMonolithFooActions::RegisterActions();
    }

    virtual void ShutdownModule() override
    {
        FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("foo"));
    }
};

IMPLEMENT_MODULE(FMonolithFooModule, MonolithFoo)
```

That's the entire module file. `StartupModule` checks the settings toggle, calls your action registration, done. `ShutdownModule` cleans up the namespace.

### Step 4: Write Action Handlers

This is where the `#if WITH_FOO` guards live.

**Header:**

```cpp
// Source/MonolithFoo/Private/MonolithFooActions.h
#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

class FMonolithFooActions
{
public:
    static void RegisterActions();

private:
    static FMonolithActionResult HandleListFoos(
        const TSharedPtr<FJsonObject>& Params);

    static FMonolithActionResult HandleGetFooDetails(
        const TSharedPtr<FJsonObject>& Params);
};
```

**Implementation:**

```cpp
// Source/MonolithFoo/Private/MonolithFooActions.cpp

#include "MonolithFooActions.h"
#include "MonolithToolRegistry.h"

DECLARE_LOG_CATEGORY_EXTERN(LogMonolithFoo, Log, All);

#if WITH_FOO

// Third-party includes go INSIDE the guard
#include "FooSubsystem.h"
#include "FooAsset.h"

void FMonolithFooActions::RegisterActions()
{
    FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();

    // Build parameter schema (optional but recommended — enables auto-docs)
    TSharedPtr<FJsonObject> ListSchema = MakeShared<FJsonObject>();
    // ... define expected params as JSON Schema ...

    Registry.RegisterAction(
        TEXT("foo"),                  // Namespace — becomes "foo_query" MCP tool
        TEXT("list_foos"),            // Action name
        TEXT("List all Foo assets in the project"),
        FMonolithActionHandler::CreateStatic(&FMonolithFooActions::HandleListFoos),
        ListSchema
    );

    Registry.RegisterAction(
        TEXT("foo"),
        TEXT("get_foo_details"),
        TEXT("Get detailed information about a specific Foo asset"),
        FMonolithActionHandler::CreateStatic(&FMonolithFooActions::HandleGetFooDetails)
    );

    UE_LOG(LogMonolithFoo, Log,
        TEXT("MonolithFoo: Registered %d actions in 'foo' namespace"), 2);
}

FMonolithActionResult FMonolithFooActions::HandleListFoos(
    const TSharedPtr<FJsonObject>& Params)
{
    // Call FooPlugin APIs here
    UFooSubsystem* FooSub = GEditor->GetEditorSubsystem<UFooSubsystem>();
    if (!FooSub)
    {
        return FMonolithActionResult::Error(
            TEXT("FooSubsystem not available"));
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    TArray<TSharedPtr<FJsonValue>> FooArray;

    for (const UFooAsset* Foo : FooSub->GetAllFoos())
    {
        TSharedPtr<FJsonObject> FooObj = MakeShared<FJsonObject>();
        FooObj->SetStringField(TEXT("name"), Foo->GetName());
        FooObj->SetStringField(TEXT("path"), Foo->GetPathName());
        FooObj->SetNumberField(TEXT("value"), Foo->SomeValue);
        FooArray.Add(MakeShared<FJsonValueObject>(FooObj));
    }

    Result->SetArrayField(TEXT("foos"), FooArray);
    Result->SetNumberField(TEXT("count"), FooArray.Num());
    return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithFooActions::HandleGetFooDetails(
    const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath = Params->GetStringField(TEXT("asset_path"));
    if (AssetPath.IsEmpty())
    {
        return FMonolithActionResult::Error(
            TEXT("Missing required parameter: asset_path"));
    }

    // ... query FooPlugin APIs, build JSON result ...

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    // ... populate ...
    return FMonolithActionResult::Success(Result);
}

#else // WITH_FOO == 0

// Stub — module loads but registers nothing
void FMonolithFooActions::RegisterActions()
{
    UE_LOG(LogMonolithFoo, Log,
        TEXT("MonolithFoo: FooPlugin not found at compile time, integration inactive"));
}

// Stubs for the handlers — never called, but the linker needs them
FMonolithActionResult FMonolithFooActions::HandleListFoos(
    const TSharedPtr<FJsonObject>& Params)
{
    return FMonolithActionResult::Error(TEXT("FooPlugin not available"));
}

FMonolithActionResult FMonolithFooActions::HandleGetFooDetails(
    const TSharedPtr<FJsonObject>& Params)
{
    return FMonolithActionResult::Error(TEXT("FooPlugin not available"));
}

#endif // WITH_FOO
```

**Important:** Use `CreateStatic` for action handler delegates, never `CreateRaw(this, ...)`. The registry copies the delegate and may execute it after releasing its internal lock. `CreateRaw` captures a raw pointer that can dangle. `CreateStatic` captures a static function address that's valid for the lifetime of the DLL.

### Step 5: Add Settings Toggle

In `Source/MonolithCore/Public/MonolithSettings.h`, add under the `// --- Optional Module Toggles ---` section:

```cpp
UPROPERTY(config, EditAnywhere, Category="Modules|Optional",
    meta=(DisplayName="Enable Foo Integration",
          ToolTip="When enabled and FooPlugin is installed, provides foo_query MCP actions."))
bool bEnableFoo = true;
```

**Do NOT wrap this property in `#if WITH_FOO`.** `UMonolithSettings` lives in MonolithCore, which has no `WITH_FOO` define and must never depend on optional modules. The `#if` guard would always evaluate to 0 in MonolithCore's compilation unit. The toggle simply does nothing when the module isn't loaded — the startup log tells the user why.

### Step 6: Update Monolith.uplugin

Add the module to the `Modules` array:

```json
{
    "Name": "MonolithFoo",
    "Type": "Editor",
    "LoadingPhase": "Default"
}
```

**Use `Default` loading phase, not `PostEngineInit`.** MonolithCore starts its HTTP server at `PostEngineInit`. UE processes loading phases in order across ALL plugins: `Default` runs for every plugin before `PostEngineInit` runs for any plugin. Your module must register its actions before the HTTP server starts serving `tools/list`. If you use `PostEngineInit`, there's no guaranteed ordering between your module and MonolithCore across different plugins — the actions might not appear.

If FooPlugin has a corresponding engine plugin dependency, add it to the `Plugins` array with `"Optional": true`:

```json
{
    "Name": "FooPlugin",
    "Enabled": true,
    "Optional": true
}
```

`"Optional": true` means: if FooPlugin isn't installed, Monolith still loads. This is a plugin-level dependency hint, NOT a module-level one. The actual DLL-level safety comes from the `Build.cs` detection pattern.

### Step 7: Test Both Paths

**WITH_FOO=1 (plugin installed):**

1. Install FooPlugin
2. Full rebuild (delete `Intermediate/` if coming from a previous build without Foo)
3. Open editor, check logs for `MonolithFoo: Registered N actions in 'foo' namespace`
4. Call `monolith_discover("foo")` — should list your actions
5. Call `foo_query("list_foos", {})` — should return data

**WITH_FOO=0 (plugin not installed):**

1. Remove/disable FooPlugin
2. Full rebuild
3. Open editor, check logs for `MonolithFoo: FooPlugin not found at compile time, integration inactive`
4. `monolith_discover()` — `foo` namespace should not appear
5. All core Monolith actions work normally

---

## What CAN and CANNOT Go in #if Blocks

UHT (Unreal Header Tool) parses your headers **before** the C preprocessor runs. It does not evaluate `#if` directives. This creates hard rules about what can live inside conditional blocks.

### CAN Go Inside `#if WITH_FOO`

| Element | Example |
|---------|---------|
| `#include` directives | `#include "FooSubsystem.h"` |
| Function bodies | `void DoStuff() { FooAPI::Call(); }` |
| Local variables | `UFooAsset* Asset = ...;` |
| Forward declarations | `class UFooAsset;` |
| Static helper functions | `static TArray<UFooAsset*> GatherFoos() { ... }` |
| Entire `.cpp` files | Wrap the whole file in `#if WITH_FOO` / `#endif` |
| Non-reflected class members | `TUniquePtr<FFooData> CachedData;` |

### CANNOT Go Inside `#if WITH_FOO`

| Element | Why |
|---------|-----|
| `UPROPERTY()` | UHT generates reflection code unconditionally |
| `UCLASS()` | UHT needs to see it regardless of preprocessor state |
| `USTRUCT()` | Same — UHT parses before preprocessor |
| `UENUM()` | Same |
| `UFUNCTION()` | Same |
| `GENERATED_BODY()` | Required by UHT-visible types |

### Workarounds for Typed Members

If you need a class member that references a third-party type:

```cpp
// BAD — UHT will choke
UPROPERTY()
#if WITH_FOO
UFooComponent* FooComp;
#endif

// GOOD — non-reflected pointer, managed manually
#if WITH_FOO
class UFooComponent* CachedFooComp = nullptr;
#endif

// GOOD — forward-declared wrapper in a non-reflected struct
#if WITH_FOO
struct FFooIntegrationData
{
    UFooComponent* FooComp = nullptr;
    TArray<UFooAsset*> CachedAssets;
};
TUniquePtr<FFooIntegrationData> FooData;
#endif
```

For Monolith's use case (editor-only action handlers that query third-party APIs and return JSON), this limitation rarely matters. Action handlers are static functions bound via delegates — they're not reflected, they don't use UPROPERTY, and they don't store state between calls.

---

## The Blueprint Assist Bridge (Reference Implementation)

MonolithBABridge is the canonical optional module. It uses the **IModularFeatures bridge** pattern (not the registry pattern) because the consumer is MonolithBlueprint's C++ code, not an AI agent calling a new namespace.

### What It Does

MonolithBlueprint's `auto_layout` action can optionally delegate graph formatting to Blueprint Assist (a marketplace plugin with superior layout algorithms). When BA is installed, formatting uses BA's engine. When it's not, the action falls back to Monolith's built-in Sugiyama algorithm.

### File Layout

```
Source/MonolithBABridge/
    MonolithBABridge.Build.cs            # Directory.Exists() probe for BA
    Private/
        MonolithBABridgeModule.cpp       # Registers/unregisters the IModularFeature
        MonolithBAFormatterImpl.h        # Implementation header (#if WITH_BLUEPRINT_ASSIST)
        MonolithBAFormatterImpl.cpp      # Implementation (#if WITH_BLUEPRINT_ASSIST)

Source/MonolithCore/Public/
    IMonolithGraphFormatter.h            # Abstract interface (no BA dependency)
```

### How the Pieces Connect

**`IMonolithGraphFormatter.h`** (in MonolithCore) defines a pure virtual interface with three methods: `SupportsGraph()`, `FormatGraph()`, and `GetFormatterInfo()`. It also provides static `IsAvailable()` and `Get()` helpers. No third-party headers are included — this file is safe to include anywhere.

**`MonolithBABridge.Build.cs`** probes for Blueprint Assist in project `Plugins/`, engine `Plugins/Marketplace/`, and engine `Plugins/` root. Defines `WITH_BLUEPRINT_ASSIST=1` or `=0`.

**`MonolithBABridgeModule.cpp`** checks `UMonolithSettings::bEnableBlueprintAssist`, then (inside `#if WITH_BLUEPRINT_ASSIST`) creates the implementation and registers it as an IModularFeature.

**`MonolithBAFormatterImpl`** (header + cpp, both inside `#if WITH_BLUEPRINT_ASSIST`) implements the interface by calling Blueprint Assist's `FBATabHandler`, `FBAGraphHandler`, and `FBAUtils` APIs.

**MonolithBlueprint** (the consumer) includes only `IMonolithGraphFormatter.h`. At action time:

```cpp
if (IMonolithGraphFormatter::IsAvailable()
    && IMonolithGraphFormatter::Get().SupportsGraph(Graph))
{
    int32 NodesFormatted = 0;
    FString ErrorMessage;
    IMonolithGraphFormatter::Get().FormatGraph(Graph, NodesFormatted, ErrorMessage);
}
```

Zero compile-time coupling. MonolithBlueprint has no idea Blueprint Assist exists.

### Source Files

- [`MonolithBABridge.Build.cs`](../Source/MonolithBABridge/MonolithBABridge.Build.cs)
- [`MonolithBABridgeModule.cpp`](../Source/MonolithBABridge/Private/MonolithBABridgeModule.cpp)
- [`MonolithBAFormatterImpl.h`](../Source/MonolithBABridge/Private/MonolithBAFormatterImpl.h)
- [`MonolithBAFormatterImpl.cpp`](../Source/MonolithBABridge/Private/MonolithBAFormatterImpl.cpp)
- [`IMonolithGraphFormatter.h`](../Source/MonolithCore/Public/IMonolithGraphFormatter.h)

For the full IModularFeatures bridge pattern documentation, see [`Docs/OPTIONAL_MODULES.md`](../Docs/OPTIONAL_MODULES.md).

---

## MonolithGAS (GBA Conditional)

MonolithGAS registers its actions (most in `gas`, plus 4 aliased into `ui`) under the `gas` namespace for Gameplay Ability System manipulation. It uses the **registry pattern** (actions registered into `FMonolithToolRegistry`) with a conditional `#if WITH_GBA` boundary for GBA (Gameplay Blueprint Attributes) features. Phase J F4 added a baseline vitals AttributeSet template that works without GBA.

### How GBA Conditionality Works

GAS engine modules (`GameplayAbilities`, `GameplayTags`, `GameplayTasks`) are always available — they ship with every UE install. The GBA plugin (`BlueprintAttributes` from Fab) is optional and enables Blueprint-only AttributeSets.

**Source mode:** `MonolithGAS.Build.cs` probes for GBA in project `Plugins/`, engine `Plugins/Marketplace/` (using `Gameplaya*` wildcard for obfuscated Fab directory), and engine `Plugins/` root. Defines `WITH_GBA=1` or `=0`.

**Binary release mode:** `MonolithGAS.Build.cs.release` unconditionally sets `WITH_GBA=0`. GBA features are not available in binary releases — users who want them must build from source.

### What's Behind `#if WITH_GBA`

- Blueprint AttributeSet creation (`create_attribute_set` with `mode: "blueprint"`) — uses `UGBAAttributeSetBlueprintBase`
- `FGBAGameplayClampedAttributeData` for min/max clamping
- K2_PreGameplayEffectExecute auto-wiring
- DataTable `_C` suffix stripping

Everything else (130+ actions for abilities, effects, C++ attribute sets, ASC management, tags, cues, targeting, input, debugging) works without GBA.

### Binary Release Checklist

The current release pipeline uses the unified `MONOLITH_RELEASE_BUILD=1` kill switch — when set, `MonolithGAS.Build.cs` forces `bHasGBA = false` automatically. No file swaps needed:
1. Run `Scripts/make_release.ps1 -Version "<X.Y.Z>"` (sets the env var, runs UBT with `-DisableUnity`, packages the zip)
2. Dev rebuild without `MONOLITH_RELEASE_BUILD` to restore your local dev binaries with `WITH_GBA=1`

### Source Files

- [`MonolithGAS.Build.cs`](../Source/MonolithGAS/MonolithGAS.Build.cs) — conditional GBA detection with `MONOLITH_RELEASE_BUILD=1` bypass
- [`MonolithGASModule.cpp`](../Source/MonolithGAS/Private/MonolithGASModule.cpp) — startup with `bEnableGAS` settings check
- [`MonolithGASAttributeActions.cpp`](../Source/MonolithGAS/Private/MonolithGASAttributeActions.cpp) — contains `#if WITH_GBA` guards

---

## MonolithComboGraph

MonolithComboGraph registers 13 actions under the `combograph` namespace for ComboGraph plugin integration. It uses the **registry pattern** (actions registered into `FMonolithToolRegistry`) with a `#if WITH_COMBOGRAPH` compile-time boundary.

### How ComboGraph Conditionality Works

ComboGraph is a marketplace plugin for visual combo tree editing. It is NOT a standard engine module — it must be purchased and installed separately.

**Source mode:** `MonolithComboGraph.Build.cs` probes for ComboGraph in project `Plugins/` and engine `Plugins/Marketplace/` (using wildcard matching for obfuscated Fab directory names). Defines `WITH_COMBOGRAPH=1` or `=0`.

**Binary release mode:** `MONOLITH_RELEASE_BUILD=1` forces `WITH_COMBOGRAPH=0`. ComboGraph features are not available in binary releases — users who want them must build from source after installing ComboGraph.

### Reflection-Only Integration

Unlike most optional modules that link against the third-party plugin's C++ API, MonolithComboGraph uses **UObject reflection exclusively**:

- `FindPropertyByName` / `FProperty::GetValue_InContainer` for reading/writing properties
- `UComboGraphFactory` discovered via `UClass::TryFindTypeSlow` at runtime
- Asset Registry queries to discover ComboGraph assets by native class

This makes the integration version-agnostic — it works with any ComboGraph version as long as the reflected property names are stable. No header includes from ComboGraph are needed inside `#if WITH_COMBOGRAPH`.

### EdGraph Sync

ComboGraph assets contain dual graph representations: a runtime graph (UComboGraph with nodes/edges) and an editor graph (UEdGraph for visual editing). Write actions (add_combo_node, add_combo_edge, etc.) update both graphs so changes are immediately visible in the ComboGraph editor without manual refresh.

### What the 12 Actions Cover

| Category | Actions | Details |
|----------|---------|---------|
| Read | 4 | List graphs, inspect structure, read node effects, validate integrity |
| Create | 5 | Create graphs, add nodes with montages, add edges, set effects, set cues |
| Scaffold | 3 | Create combo abilities, link abilities to graphs, scaffold from montage lists |

### GAS Cross-Integration

`create_combo_ability` and `link_ability_to_combo_graph` bridge ComboGraph with GAS. They require both plugins to be present. The combo ability scaffolding wires up the appropriate ability task to drive the combo graph at runtime.

### Source Files

- [`MonolithComboGraph.Build.cs`](../Source/MonolithComboGraph/MonolithComboGraph.Build.cs) — conditional ComboGraph detection
- [`MonolithComboGraphModule.cpp`](../Source/MonolithComboGraph/Private/MonolithComboGraphModule.cpp) — startup with `bEnableComboGraph` settings check
- [`MonolithComboGraphActions.cpp`](../Source/MonolithComboGraph/Private/MonolithComboGraphActions.cpp) — contains `#if WITH_COMBOGRAPH` guards

---

## MonolithLogicDriver

MonolithLogicDriver registers 66 actions under the `logicdriver` namespace for Logic Driver Pro state machine manipulation. It uses the **registry pattern** with a `#if WITH_LOGICDRIVER` compile-time boundary.

### How LogicDriver Conditionality Works

Logic Driver Pro is a marketplace plugin for visual state machine editing. It is NOT a standard engine module — it must be purchased and installed separately.

**Source mode:** `MonolithLogicDriver.Build.cs` probes for Logic Driver Pro in three locations: project `Plugins/`, engine `Plugins/Marketplace/` (using wildcard matching for obfuscated Fab directory names), and engine `Plugins/` root. Defines `WITH_LOGICDRIVER=1` or `=0`.

**Binary release mode:** `MONOLITH_RELEASE_BUILD=1` forces `WITH_LOGICDRIVER=0`. LogicDriver features are not available in binary releases — users who want them must build from source after installing Logic Driver Pro.

### Reflection-Only Integration

Like MonolithComboGraph, MonolithLogicDriver uses **UObject reflection exclusively**:

- `FindPropertyByName` / `FProperty` for reading/writing state machine properties
- Asset factory discovered via `UClass::TryFindTypeSlow` at runtime
- Asset Registry queries to discover state machine assets by native class

This makes the integration version-agnostic — it works with any Logic Driver Pro version as long as the reflected property names are stable. No header includes from Logic Driver are needed inside `#if WITH_LOGICDRIVER`.

### What the 66 Actions Cover

| Category | Actions | Details |
|----------|---------|---------|
| SM CRUD | 7 | Create, inspect, compile, delete, list, duplicate, validate |
| Graph Read/Write | 12 | Add states, transitions, configure properties, set rules |
| Node Config | 8 | State nodes, conduits, transition events, property editing |
| Runtime/PIE | 6 | Start, stop, query active states, trigger, inspect |
| JSON Spec | 5 | `build_sm_from_spec`, export, import, validate spec, diff |
| Scaffolding | 10 | Door, health, AI patrol, dialogue, elevator, puzzle, inventory patterns |
| Components | 5 | Add/configure Logic Driver components on actors |
| Text Graph | 3 | Text visualization, graph summary, state dump |
| Discovery | 10 | List node classes, state types, templates, available assets |

### Source Files

- [`MonolithLogicDriver.Build.cs`](../Source/MonolithLogicDriver/MonolithLogicDriver.Build.cs) — conditional Logic Driver detection
- [`MonolithLogicDriverModule.cpp`](../Source/MonolithLogicDriver/Private/MonolithLogicDriverModule.cpp) — startup with `bEnableLogicDriver` settings check
- [`MonolithLogicDriverActions.cpp`](../Source/MonolithLogicDriver/Private/MonolithLogicDriverActions.cpp) — contains `#if WITH_LOGICDRIVER` guards

---

## Tips for LLM Agents

If you're an AI assistant writing an optional module for Monolith, follow these rules:

### Before Writing Code

1. **Call `monolith_discover()` first.** Never guess action names or namespace conventions. The discover response tells you what already exists.
2. **Use `source_query("search_source", {"query": "..."})` to verify third-party API signatures.** Don't guess function names, parameter types, or include paths. If the third-party plugin is indexed, search for it. If not, read its headers from disk.
3. **Check existing modules for patterns.** Every Monolith module follows the same structure. Read any `Source/Monolith*/Private/*Module.cpp` for the registration pattern.

### Writing the Code

4. **Register actions with `CreateStatic`, never `CreateRaw(this, ...)`.** The registry copies the delegate and may execute it after releasing its lock. `CreateRaw` captures a raw pointer that can dangle if the module is destroyed. Static function addresses are valid for the DLL's lifetime.
5. **Use unique namespace names that won't collide.** Check `monolith_discover()` output. Core in-tree namespaces are: `blueprint`, `material`, `animation`, `niagara`, `mesh`, `ai`, `editor`, `config`, `source`, `project`, `ui`, `gas`, `logicdriver`, `combograph`, `audio`, `monolith`, `level_sequence`, `bulk_fill`, `describe`, `decision`, `risk`, `cppreflect`, `network`, `pipeline`. Don't shadow these. Extension plugins should choose their own stable namespaces and document them in their own repositories.
6. **Always define `WITH_FOO` to 0 or 1 in Build.cs.** Both branches. Never leave it undefined.
7. **Put ALL third-party includes inside `#if WITH_FOO`.** No exceptions. A bare `#include "ThirdPartyHeader.h"` outside the guard will break builds for users without the plugin.
8. **Provide handler stubs in the `#else` branch.** The linker needs symbols even when the plugin is absent. The stubs should return `FMonolithActionResult::Error()` with a descriptive message.

### Testing

9. **Test both `WITH_FOO=0` and `WITH_FOO=1` builds.** The most common bug is a third-party include or API call that leaked outside the `#if` guard.
10. **Full rebuild required after installing/removing the target plugin.** UBT caches `Build.cs` evaluation. If the `WITH_FOO` define seems stale, delete `Intermediate/` and rebuild.
11. **Check `monolith_discover("your_namespace")` after the editor loads.** If your actions don't appear, check the output log for your module's startup messages.

### JSON Conventions

12. **Return JSON objects, not arrays, at the top level.** Monolith actions always return `FMonolithActionResult::Success(TSharedPtr<FJsonObject>)`. Wrap arrays in a named field (e.g., `{"foos": [...], "count": 5}`).
13. **Use snake_case for action names and parameter names.** Matches the existing convention across all 1387 in-tree actions.
14. **Include `count` fields alongside arrays.** Agents use these for quick checks without parsing the full array.
15. **Return asset paths in `/Game/Path/To/Asset` format** (no `.uasset` extension). This is the UE convention used across all Monolith namespaces.

---

## Binary Distribution & Extension Plugins

For both source users and binary release users, the in-tree optional modules (GAS, ComboGraph, LogicDriver, MetaSound features in Audio, CommonUI features in UI) live inside `Monolith.uplugin` with conditional `Build.cs` 3-location detection plus the `MONOLITH_RELEASE_BUILD=1` kill switch. The release script (`Scripts/make_release.ps1`) sets `MONOLITH_RELEASE_BUILD=1`, which forces all optional `WITH_*` macros to 0 and produces a release zip that loads cleanly on any UE 5.7+ project regardless of which marketplace plugins the end user owns.

For **new** integrations against third-party or paid marketplace plugins, prefer the **extension plugin pattern** — a separate `.uplugin` that lives beside Monolith at the project's `Plugins/` level and registers actions into Monolith's shared `FMonolithToolRegistry` from outside the core repo. This keeps the main Monolith release zip lean and lets the extension have its own release lifecycle.

When extension plugins are loaded the live `monolith_status` reports the union of in-tree + extension actions. The in-tree total of **~1,500+ actions across 27 namespaces** is the ground truth for the Monolith release zip itself (approximate and rounded down by policy — query `monolith_discover()` for the live figure).

For the canonical extension-plugin pattern with copy-paste Build.cs, module entry, and registration code, see [`Docs/SIBLING_PLUGIN_GUIDE.md`](../Docs/SIBLING_PLUGIN_GUIDE.md).

---

## Architecture Diagram

```
                    Monolith.uplugin
                    (always loads — single .uplugin, conditional modules inside)
    +-----------------------------------------------+
    |  MonolithCore                                 |
    |    FMonolithToolRegistry (singleton)  <-------+---- Extension plugins
    |    FMonolithHttpServer                        |     register here too
    |    UMonolithSettings                          |
    |    monolith_discover / monolith_guide         |
    |    bulk_fill (2) + describe (3) framework     |
    |-----------------------------------------------+
    |  MonolithBlueprint  (112 actions)             |
    |  MonolithMaterial   (64 actions)              |
    |  MonolithAnimation  (125 actions)             |
    |  MonolithNiagara    (120 actions)             |
    |  MonolithMesh       (194 actions, +45 town)   |
    |  MonolithAI         (221 actions, WITH_*)     |
    |  MonolithEditor     (33 actions)              |
    |  MonolithConfig     (6 actions)               |
    |  MonolithIndex      (8 actions)               |
    |  MonolithSource     (12 actions)              |
    |  MonolithUI         (138 actions)             |
    |  MonolithGAS        (135 actions, WITH_GBA)   |
    |  MonolithComboGraph (13 actions, WITH_*)      |
    |  MonolithLogicDriver(66 actions, WITH_*)      |
    |  MonolithAudio      (98 actions, WITH_META*)  |
    |  MonolithLevelSequence (8 actions)            |
    |  MonolithReflectionIntel (21 actions, v0.17.0)|
    |  MonolithAudioRuntime (runtime classes only)  |
    |  MonolithBABridge   (IModularFeatures)        |
    +-----------------------------------------------+
                              ^
                              |
    +------------------------+----------------+
    |                        |                |
    | Extension plugins (separate .uplugins, separate repos):
    |   ExtensionA                — custom-a    (project-specific)
    |   ExtensionB                — custom-b    (project-specific)
    |   Additional private bridges — custom namespaces
    +-------------------------------------------------+
                              |
                              v
                    FMonolithToolRegistry::Get()
                    (same singleton, shared across all plugins in the project)
```

All modules — core in-tree and external extensions — register into the same `FMonolithToolRegistry` singleton exported by MonolithCore. The HTTP server builds `tools/list` dynamically from whatever is registered. Add an extension plugin, restart the editor, and AI agents see the new namespace immediately.

---

## Quick Reference: Checklist for New Optional Modules

- [ ] `Source/MonolithFoo/MonolithFoo.Build.cs` with `Directory.Exists()` detection
- [ ] `WITH_FOO=1` / `WITH_FOO=0` via `PublicDefinitions` (never undefined)
- [ ] Check project `Plugins/`, engine `Plugins/Marketplace/`, engine `Plugins/` root
- [ ] Third-party `#include` and API calls inside `#if WITH_FOO` only
- [ ] Stub `RegisterActions()` in `#else` branch with log message
- [ ] No `UPROPERTY`/`UCLASS`/`USTRUCT`/`UENUM` inside `#if` blocks
- [ ] `StartupModule()` checks `UMonolithSettings` toggle
- [ ] `ShutdownModule()` calls `UnregisterNamespace()`
- [ ] Action handlers use `CreateStatic`, never `CreateRaw`
- [ ] Module added to `Monolith.uplugin` with `"LoadingPhase": "Default"`
- [ ] Plugin dep added to `Monolith.uplugin` `Plugins` array with `"Optional": true`
- [ ] `bool bEnableFoo` added to `UMonolithSettings` under `Category="Modules|Optional"`
- [ ] Tested with `WITH_FOO=1` (plugin installed, actions register)
- [ ] Tested with `WITH_FOO=0` (plugin absent, stub compiles, core works)
- [ ] `MONOLITH_RELEASE_BUILD=1` bypass added to your `Build.cs` if your module ships in the public release zip
- [ ] OR — for new third-party integrations, build it as a separate extension plugin instead. See [`Docs/SIBLING_PLUGIN_GUIDE.md`](../Docs/SIBLING_PLUGIN_GUIDE.md).

---

*See also: [`Docs/SIBLING_PLUGIN_GUIDE.md`](../Docs/SIBLING_PLUGIN_GUIDE.md) (canonical pattern for new third-party integrations) | [`Docs/OPTIONAL_MODULES.md`](../Docs/OPTIONAL_MODULES.md) (IModularFeatures bridge pattern)*
