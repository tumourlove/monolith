# MonolithUI SpecBuilder Dry-Run Fix Test

**Date:** 2026-05-01
**Editor:** Unreal Engine 5.7, Leviathan project
**Monolith:** 0.14.8, MCP port 9316

## Purpose

Verify that `ui::build_ui_from_spec` with `dry_run=true` does not create or register a Widget Blueprint asset before returning its diff.

## Commands

```powershell
# In-editor Monolith automation
editor_query("run_automation_tests", {
  "prefix": "MonolithUI.SpecBuilder.DryRunYieldsDiffNoCommit",
  "max_tests": 1
})

editor_query("run_automation_tests", {
  "prefix": "MonolithUI.SpecBuilder",
  "max_tests": 100
})

editor_query("run_automation_tests", {
  "prefix": "MonolithUI.SpecSerializer",
  "max_tests": 100
})
```

## Results

| Gate | Result | Notes |
|---|---|---|
| `MonolithUI.SpecBuilder.DryRunYieldsDiffNoCommit` | PASS, 1/1 | Confirms dry-run leaves no asset behind. |
| `MonolithUI.SpecBuilder` | PASS, 10/10 | Full builder prefix passed. |
| `MonolithUI.SpecSerializer` | PASS, 5/5 | Roundtrip and supported-field serializer gates passed. |

## Notes

The fix returns from the dry-run path before `GetOrCreateWBP`, package creation, transaction creation, compile, or save. The test now uses a GUID-suffixed dry-run asset path so stale registry state from older failed runs cannot mask the contract.
