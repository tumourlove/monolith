# Monolith — Sibling Plugin Guide

How to extend Monolith with a separate UE plugin that bridges a third-party integration (paid marketplace plugin, license-isolated code, project-specific subsystem) into Monolith's MCP action registry — *without* merging into the core Monolith repo.

**Audience:** plugin authors who want their tool callable as `<namespace>_query({ action, params })` over MCP, but whose plugin can't or shouldn't ship inside Monolith itself.

---

## Why a sibling plugin?

In-tree modules (the ones living in `Plugins/Monolith/Source/Monolith*/`) ship inside every public Monolith release zip. That works for code Monolith owns and licenses permissively, but breaks down for:

| Reason | Example |
|---|---|
| **Paid marketplace dependency** — you can't redistribute someone else's plugin | A bridge that calls into an Epic Marketplace / Fab plugin (Inventory systems, networking kits, AI middleware, etc.) |
| **License isolation** — your code is GPL/MIT/proprietary and the host project is the other | A library wrapper where you need to keep the LICENSE files and source tree separate |
| **Project-specific** — useful for one game, noise for everyone else | A bridge into your game's bespoke save system, gameplay subsystem, or build pipeline |
| **Independent versioning** — your plugin ships on its own cadence | A subsystem that updates faster (or slower) than Monolith's release cycle |
| **Optional binary footprint** — users who don't have the dep shouldn't pay for the DLL | A bridge whose binaries would be dead weight in 90% of installs |

The sibling-plugin pattern lets you do all of this while still appearing as a first-class MCP tool to AI clients (Claude Code, Cursor, Cline) — same registry, same dispatch path, same discovery output.

---

## How it works (conceptually)

```
┌─────────────────────────────────────────────────────────┐
│  Plugins/Monolith/                                      │
│    Source/MonolithCore/                                 │
│      • FMonolithToolRegistry  (MONOLITHCORE_API)        │
│      • HTTP server, MCP dispatch                        │
│    Source/Monolith<Domain>/                             │
│      • In-tree modules: register("namespace", fn)       │
└─────────────────────────────────────────────────────────┘
              ▲
              │  resolves the same
              │  FMonolithToolRegistry::Get() singleton
              │
┌─────────────────────────────────────────────────────────┐
│  Plugins/MyPluginBridge/   ← YOUR sibling plugin         │
│    Source/MyPluginBridge/                                │
│      • StartupModule → register("myns", fn)              │
│      • Build.cs detects third-party dep + #if gates      │
└─────────────────────────────────────────────────────────┘
```

`FMonolithToolRegistry` is a process-wide singleton exported from Monolith's `MONOLITHCORE_API`. Any module — in-tree or sibling — that lists `MonolithCore` as a dependency can call `FMonolithToolRegistry::Get().RegisterHandler("namespace", "action", &MyHandler)` at startup and the action is immediately callable over MCP.

The sibling plugin's namespace shows up under `monolith_discover()` alongside in-tree namespaces. AI clients see no difference.

---

## Prerequisites

Before you start:

1. **Monolith is installed and loading cleanly** in your project. Sibling plugins depend on `Monolith` as a hard plugin dependency.
2. **Your third-party dependency** (if any) is installed and loadable in the same project.
3. **You've claimed a unique namespace** that doesn't collide with Monolith's existing namespaces. Run `monolith_discover()` first — current namespaces are listed in `Plugins/Monolith/Docs/SPEC_CORE.md` §Architecture (e.g. `material`, `mesh`, `niagara`, `inventory`, etc.). Pick something specific to your integration.

---

## Plugin folder layout

Sit beside Monolith, not inside it:

```
<YourProject>/
  Plugins/
    Monolith/                       ← unmodified
      Source/MonolithCore/...
      Monolith.uplugin
    MyPluginBridge/                 ← your sibling plugin
      Source/
        MyPluginBridge/
          MyPluginBridge.Build.cs
          Private/
            MyPluginBridgeModule.cpp
            MyPluginBridgeActions.cpp
          Public/
            MyPluginBridgeModule.h
            MyPluginBridgeActions.h
      MyPluginBridge.uplugin
      Docs/
        SPEC_CORE.md
        specs/
          SPEC_<Subsystem>.md
        ROADMAP.md
        TESTING.md
```

Same docs convention as in-tree Monolith modules (per-module SPEC split for big plugins, single SPEC for small focused ones). See `feedback_local_plugin_docs_convention.md` if you're documenting your bridge.

---

## The .uplugin file

Minimum viable shape:

```json
{
  "FileVersion": 3,
  "Version": 1,
  "VersionName": "0.1.0",
  "FriendlyName": "MyPluginBridge",
  "Description": "Bridges <ThirdPartyPlugin> into the Monolith MCP registry.",
  "Category": "Editor",
  "CreatedBy": "<you>",
  "EnabledByDefault": true,
  "Installed": false,

  "Plugins": [
    { "Name": "Monolith",            "Enabled": true },
    { "Name": "<ThirdPartyPlugin>",  "Enabled": true, "Optional": true }
  ],

  "Modules": [
    {
      "Name": "MyPluginBridge",
      "Type": "Editor",
      "LoadingPhase": "Default"
    }
  ]
}
```

Notes:
- **`Monolith` is a hard plugin dep** — your bridge is meaningless without it.
- **The third-party dep is `Optional: true`** so UE doesn't refuse to load your bridge when the user removes the third-party plugin. Your bridge degrades gracefully (see Build.cs section).
- **`LoadingPhase: Default`** — runs after `PostEngineInit` (when `MonolithCore` finishes initializing the registry). If your bridge needs to register into a subsystem that initializes later, use `PostDefault`.

---

## Build.cs — third-party detection + release-build kill-switch

The canonical pattern is **detect-then-gate**: probe for the third-party plugin at compile time, set a `WITH_<DEP>=0/1` `PublicDefinition`, then gate every dep-using `#include` and call site behind that define. **And** honour `MONOLITH_RELEASE_BUILD=1` so release builds force the dep off regardless of on-disk state.

```csharp
using UnrealBuildTool;
using System.IO;

public class MyPluginBridge : ModuleRules
{
    public MyPluginBridge(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        // Always-on dependencies (Monolith + UE basics)
        PrivateDependencyModuleNames.AddRange(new string[]
        {
            "Core", "CoreUObject", "Engine",
            "MonolithCore"
        });

        // Release builds: set MONOLITH_RELEASE_BUILD=1 to force optional deps off.
        // Public Monolith release zips set this so accidentally-bundled siblings
        // can't end up with a hard import on a paid third-party DLL the user
        // doesn't have installed.
        bool bReleaseBuild =
            System.Environment.GetEnvironmentVariable("MONOLITH_RELEASE_BUILD") == "1";

        bool bHasThirdParty = false;
        if (!bReleaseBuild)
        {
            // Check three locations: project Plugins/, Engine Plugins/Marketplace/, Engine Plugins/
            string ProjectPluginsDir = Path.Combine(
                Target.ProjectFile.Directory.FullName, "Plugins");
            if (Directory.Exists(ProjectPluginsDir))
            {
                bHasThirdParty = Directory.GetDirectories(
                    ProjectPluginsDir, "ThirdParty*",
                    SearchOption.TopDirectoryOnly).Length > 0;
            }

            if (!bHasThirdParty)
            {
                string EngineDir = Path.GetFullPath(Target.RelativeEnginePath);
                string MarketplaceDir = Path.Combine(EngineDir, "Plugins", "Marketplace");
                if (Directory.Exists(MarketplaceDir))
                {
                    bHasThirdParty = Directory.GetDirectories(
                        MarketplaceDir, "ThirdParty*",
                        SearchOption.TopDirectoryOnly).Length > 0;
                }
            }
        }

        if (bHasThirdParty)
        {
            PrivateDependencyModuleNames.Add("ThirdPartyModule");
            PublicDefinitions.Add("WITH_THIRDPARTY=1");
        }
        else
        {
            PublicDefinitions.Add("WITH_THIRDPARTY=0");
        }
    }
}
```

**Why three locations?** Marketplace / Fab installs land in different folders across UE versions and install methods. The wildcard glob (`"ThirdParty*"`) survives obfuscated marketplace folder names.

**Why `MONOLITH_RELEASE_BUILD`?** Without it, anyone building a public Monolith release on a machine that *happens* to have your third-party plugin installed would produce binaries with hard imports on `UnrealEditor-ThirdPartyModule.dll`. Users who download that release and don't have the third-party plugin will hit `LoadLibrary` failures with `GetLastError=126` at module load. The kill-switch is a hard guard against accidental leakage.

This same pattern is used by every in-tree Monolith optional module: `MonolithBABridge` (Blueprint Assist), `MonolithComboGraph`, `MonolithLogicDriver`, `MonolithGAS` (Gameplay Behaviors), `MonolithAudio` (MetaSound), `MonolithAI` (Mass Entity / Zone Graph), `MonolithUI` (CommonUI), `MonolithMesh` (GeometryScripting). Reference any of those Build.cs files for variants.

---

## Module entry — registry registration

```cpp
// MyPluginBridgeModule.cpp
#include "MyPluginBridgeModule.h"
#include "MyPluginBridgeActions.h"
#include "MonolithToolRegistry.h"
#include "Modules/ModuleManager.h"

#define LOCTEXT_NAMESPACE "FMyPluginBridgeModule"

void FMyPluginBridgeModule::StartupModule()
{
#if WITH_THIRDPARTY
    FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
    FMyPluginBridgeActions::RegisterActions(Registry);
    UE_LOG(LogTemp, Log,
        TEXT("MyPluginBridge — registered N actions in 'myns' namespace"));
#else
    UE_LOG(LogTemp, Log,
        TEXT("MyPluginBridge — third-party dep not detected, bridge inactive"));
#endif
}

void FMyPluginBridgeModule::ShutdownModule()
{
    // Always unregister the namespace, even if we never registered handlers.
    // Monolith's registry treats unregister-of-empty as a no-op.
    if (FMonolithToolRegistry::IsAvailable())
    {
        FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("myns"));
    }
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FMyPluginBridgeModule, MyPluginBridge)
```

Notes:
- **Always-register the shutdown sweep** even when `WITH_THIRDPARTY=0`. Costs nothing, prevents stale entries surviving hot-reload edge cases.
- **`IsAvailable()` guard on shutdown** — Monolith might be tearing down before you. Defensive.
- **Don't fail StartupModule when the dep is missing.** Log, return cleanly. Your bridge should be invisible (zero registered actions) rather than crashing the editor.

---

## Action handlers — namespace and signatures

Match the Monolith action handler signature so dispatch finds you:

```cpp
// MyPluginBridgeActions.h
#pragma once

#include "CoreMinimal.h"
#include "MonolithActionResult.h"

class FMonolithToolRegistry;

class FMyPluginBridgeActions
{
public:
    static void RegisterActions(FMonolithToolRegistry& Registry);

#if WITH_THIRDPARTY
    static FMonolithActionResult HandleDoSomething(
        const TSharedPtr<FJsonObject>& Params);
#endif
};
```

```cpp
// MyPluginBridgeActions.cpp
#include "MyPluginBridgeActions.h"
#include "MonolithToolRegistry.h"

#if WITH_THIRDPARTY
#include "ThirdParty/ThirdPartyAPI.h"
#endif

void FMyPluginBridgeActions::RegisterActions(FMonolithToolRegistry& Registry)
{
#if WITH_THIRDPARTY
    Registry.RegisterHandler(
        TEXT("myns"),               // namespace claim
        TEXT("do_something"),       // action name
        &FMyPluginBridgeActions::HandleDoSomething);

    // ... more handlers ...
#endif
}

#if WITH_THIRDPARTY
FMonolithActionResult FMyPluginBridgeActions::HandleDoSomething(
    const TSharedPtr<FJsonObject>& Params)
{
    // Validate params...
    FString TargetName;
    if (!Params->TryGetStringField(TEXT("target"), TargetName))
    {
        return FMonolithActionResult::Error(
            -32602, TEXT("Missing required param: target"));
    }

    // Call into third-party API...
    bool bOk = ThirdPartyAPI::Do(TargetName);

    // Return result as JSON...
    TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), bOk);
    Result->SetStringField(TEXT("target"), TargetName);
    return FMonolithActionResult::Ok(Result);
}
#endif
```

Conventions:
- **Namespace = lowercase, no spaces, project-specific.** Goes to `<myns>_query` MCP tool.
- **Action name = snake_case.** Verbs preferred (`get_x`, `set_y`, `create_z`).
- **Handler signature is fixed:** `static FMonolithActionResult Handle*(const TSharedPtr<FJsonObject>& Params)`.
- **Errors return JSON-RPC error codes.** `-32601` (method not found), `-32602` (invalid params), `-32603` (internal error). Use `FMonolithActionResult::Error(code, message)`.
- **Successful results return a `TSharedRef<FJsonObject>`** with whatever fields make sense for the action. Wrap in `FMonolithActionResult::Ok(payload)`.

If you have many handler classes, follow the in-tree Monolith convention: one class per logical subsystem (`FMyPluginInspectionActions`, `FMyPluginCRUDActions`, etc.), each with its own `RegisterActions` entry point that the module entry calls in sequence.

---

## Distribution & release discipline

Two distribution scenarios:

### 1. Bridge ships publicly (as a Marketplace / Fab plugin or open-source repo)

- **Independent `.uplugin` versioning** — your bridge has its own VersionName, decoupled from Monolith's.
- **Dependency declaration** — `Plugins[]` lists `Monolith` as a hard dep. Users install both.
- **CI builds on multiple UE versions** if you want to support more than one engine release. Honour the same conditional patterns Monolith uses (`#if ENGINE_MINOR_VERSION >= ...`).
- **Don't redistribute the third-party plugin.** Tell users to install it themselves; your Build.cs detects it.

### 2. Bridge stays private (internal-use, paid-dep wrapper, project-specific)

- **Sibling plugin lives outside the Monolith repo entirely** (separate VCS branch, separate folder). Public Monolith release zip never sees it.
- **Defence in depth:**
  - Public Monolith release script (`Plugins/Monolith/Scripts/make_release.ps1`) gates against accidental sibling inclusion via `git ls-files` (only files tracked by Monolith's own VCS make it into the zip).
  - `MONOLITH_RELEASE_BUILD=1` env-var forces all optional deps off so even an accidentally-bundled sibling can't carry a hard DLL import.
  - Public Monolith docs scrub references to private siblings before push.
- **Per-module specs in your sibling stay private.** Public docs may *acknowledge the existence* of the integration if it's not embarrassing, but the action roster + implementation specs are yours.

---

## Examples in the wild

| Plugin | Status | Bridges | Namespace | Notes |
|---|---|---|---|---|
| **MonolithISX** | private repo | InventorySystemX (paid Fab plugin) | `inventory` | 158 actions. Lives at `Plugins/MonolithISX/`. Source + per-module specs private; existence acknowledged in `SPEC_CORE.md`. |
| **MonolithSteamBridge** | private repo | Steam Integration Kit (paid Fab plugin) | `steam` | 28 actions across 7 subsystems (Achievement, Cloud, Infra, Leaderboard, Overlay, Stat, UserAuth). Composed with `MonolithSteamBridgeLeaderboard` sibling for full-fidelity leaderboard upload/download (UHT + SIK USTRUCT workaround). Source + per-module specs private; existence acknowledged in `SPEC_CORE.md`. |

The in-tree optional modules (`MonolithBABridge`, `MonolithComboGraph`, `MonolithLogicDriver`, `MonolithGAS`, `MonolithAudio`, `MonolithAI`, `MonolithUI`, `MonolithMesh`) all use the same conditional-compilation pattern described in the Build.cs section above — those are the canonical reference implementations. Crack any of them open for variants of the detection probe and `RegisterActions` shape.

---

## Testing your sibling plugin

1. **Compile both with and without the third-party dep installed.**
   - With dep present: `WITH_THIRDPARTY=1`, all your actions register, full functionality.
   - With dep absent: `WITH_THIRDPARTY=0`, your module compiles clean, no actions register, log line states "bridge inactive".
2. **Compile with `MONOLITH_RELEASE_BUILD=1` set.** Should behave identically to "dep absent" — no hard DLL import, zero registered actions. Verify with `dumpbin /imports Binaries/Win64/UnrealEditor-MyPluginBridge.dll` (Windows) or `otool -L` (macOS).
3. **`monolith_discover()`** should list your namespace in the response when `WITH_THIRDPARTY=1`, and *not* list it when `=0`.
4. **Round-trip an MCP call:**
   ```
   curl -sS -X POST http://localhost:9316/mcp \
     -H "Content-Type: application/json" \
     -d '{"jsonrpc":"2.0","id":1,"method":"tools/call",
          "params":{"name":"myns_query",
                    "arguments":{"action":"do_something",
                                 "params":{"target":"foo"}}}}'
   ```
5. **Hot-reload survival.** Edit a handler, recompile via Live Coding, re-run the MCP call. The handler should execute the new code without an editor restart. (Live Coding patches are in-memory only; for header changes, full editor restart + UBT.)

---

## Common pitfalls

| Pitfall | Symptom | Fix |
|---|---|---|
| **Forgot `MONOLITH_RELEASE_BUILD` honor** | Released DLL hard-imports a third-party DLL the user doesn't have → `GetLastError=126` at module load | Add the env-var check before `Directory.Exists` (see Build.cs section) |
| **Namespace collision** | Your handlers don't fire; Monolith's existing handlers fire instead | Run `monolith_discover()` before claiming a namespace; pick something specific |
| **Module loads before MonolithCore** | `FMonolithToolRegistry::Get()` returns null at startup | `LoadingPhase: Default` (not `PreEngineInit` / `PostConfigInit`) |
| **Forgot `IsAvailable()` guard on shutdown** | Editor exit crash on plugin unload during teardown | Wrap registry calls in `if (FMonolithToolRegistry::IsAvailable())` in `ShutdownModule` |
| **Registered actions outside `#if WITH_THIRDPARTY`** | Actions register but every call returns -32601 because handler bodies are stubbed out | Move both `RegisterHandler` calls AND the handler bodies inside the `#if` |
| **Handler doesn't return JSON-RPC error on missing param** | AI client gets cryptic 500 instead of actionable error message | Validate params at the top, return `FMonolithActionResult::Error(-32602, ...)` |

---

## See also

- [`SPEC_CORE.md`](SPEC_CORE.md) — Monolith plugin overview, registered namespaces, action counts
- [`Plugins/Monolith/Source/MonolithCore/Public/MonolithToolRegistry.h`](../Source/MonolithCore/Public/MonolithToolRegistry.h) — registry API surface (RegisterHandler, UnregisterNamespace, Get, IsAvailable)
- [`Plugins/Monolith/Source/MonolithBABridge/MonolithBABridge.Build.cs`](../Source/MonolithBABridge/MonolithBABridge.Build.cs) — canonical detection-probe + `MONOLITH_RELEASE_BUILD` Build.cs pattern
- [`Plugins/Monolith/Scripts/make_release.ps1`](../Scripts/make_release.ps1) — public release pipeline; understand its dirty-tree gate, `MONOLITH_RELEASE_BUILD=1` injection, and `$StrippedModules` belt-and-braces strip list
