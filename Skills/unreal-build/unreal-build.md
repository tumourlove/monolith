---
name: unreal-build
description: Use when building, compiling, or fixing build errors in Unreal Engine projects. Determines whether to use Live Coding or UBT based on what changed.
---

# Unreal Build — Smart Build Decision Guide

## Step 1: Check What Changed

Analyze the files you modified. Classify each change:

| Change Type | Build Method |
|---|---|
| `.cpp` body changes only | Live Coding |
| `.h` modified (members, layout, macros) | UBT (editor must close) |
| `.h` added | UBT (editor must close) |
| `.cpp` added | UBT (editor must close) |
| `.cpp` deleted | UBT (editor must close) |
| `.Build.cs` changed | UBT (editor must close) |
| `.uplugin` changed | UBT (editor must close) |

**Rule: If ANY file requires UBT, the whole build requires UBT.**

## Step 2: Check Editor Status

Try calling Monolith MCP: `editor_query({action: 'get_build_status'})` or `monolith_status()`.

- **MCP responds** → Editor is running
- **MCP fails/timeout** → Editor is closed

## Step 3: Execute Build

### Live Coding Path (editor open + .cpp-only changes)

1. Call `editor_query({ action: "trigger_build" })` via MCP
2. Wait ~10 seconds for compilation
3. Call `editor_query({ action: "get_compile_output" })` to check result
4. If errors: call `editor_query({ action: "get_build_errors", params: { compile_only: true } })`

### UBT Path (editor closed OR header/new-file/Build.cs changes)

**If editor is open and UBT is needed:**
> Tell the user: "Header/structural changes detected — Live Coding can't handle these. Please close the editor so I can run a full UBT build, then reopen after."
>
> Do NOT attempt UBT while editor is running. You will get: `"Unable to build while Live Coding is active"`

**When editor is confirmed closed, run:**

```bash
'C:\Program Files (x86)\UE_5.7\Engine\Binaries\DotNET\UnrealBuildTool\UnrealBuildTool.exe' YourProjectEditor Win64 Development '-Project=D:\Path\To\YourProject.uproject' -waitmutex
```

> **IMPORTANT:** Use single quotes around the UBT path — bash can't handle `(x86)` in parentheses unquoted. Do NOT use `Build.bat`.

Check exit code: `0` = success, non-zero = failure. On failure, grep output for `error` lines.

## Decision Matrix (Quick Reference)

| Editor | Changes | Action |
|--------|---------|--------|
| Open | .cpp only | `editor_query("trigger_build")` via MCP |
| Open | .h / new files / Build.cs | Ask user to close editor → UBT |
| Open | .uplugin | Ask user to close editor → UBT |
| Closed | Any | Run UBT directly |

## Live Coding Gotchas

- **Header changes** (new members, class layout, UCLASS/USTRUCT) → requires editor restart + full UBT build
- **New .cpp files** are NOT picked up by Live Coding — UBT required
- **Deleted files** are NOT handled by Live Coding — UBT required
- After triggering Live Coding, **wait ~10s** before checking compile result
- `"Unable to build while Live Coding is active"` → use `editor_query("trigger_build")` instead of UBT, or close editor first
- When in doubt, close editor and use UBT — it always works
