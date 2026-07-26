# Updater Binary Release Selection Verification

**Date:** 2026-07-26
**Scope:** Fail-closed selection of installable Monolith release ZIP assets
**Hosts:** Disposable UE 5.8 and UE 5.7 projects outside the plugin checkout

---

## 1. Purpose

Verify that the updater offers an install only when a GitHub release contains an
explicit Monolith binary ZIP. GitHub-generated source archives, unrelated ZIP
assets, another plugin's engine-specific ZIP, and mismatched Unreal Engine
minor-version artifacts must not become installation candidates.

---

## 2. Verification Results

| Gate | Result | Evidence |
|---|---|---|
| `git diff --check` | PASS | No whitespace errors |
| Static policy differential | PASS | Newer Speed static policy reported 36 blockers on both clean upstream and this branch; this branch introduced zero blockers |
| UE 5.8 editor build | PASS | Isolated protected host full build completed 436/436 actions; the current test-only refresh rebuilt and relinked `UnrealEditor-MonolithCore.dll` successfully |
| UE 5.8 release-selection automation | PASS, 7/7 | `D:\P4\MonolithPR104ProtectedHost\Saved\Logs\ActivationHost.log` records seven succeeded tests, zero failed tests, and an empty automation queue |
| UE 5.7 editor build | PASS | Clean isolated source worktree, 430/430 actions |
| Screenshot / Discord upload | N/A | Updater asset selection has no visual or asset-presentation change |

---

## 3. Covered Contracts

The `Monolith.Updater.ReleaseSelection` test prefix verifies:

1. a `Monolith-*.zip` binary asset is accepted;
2. an unrelated ZIP does not offer installation;
3. a mismatched Unreal Engine minor version fails closed;
4. an engine-specific ZIP still requires the `Monolith-` prefix;
5. another plugin's engine-tagged ZIP cannot switch a generic Monolith release into per-engine mode;
6. a release containing only GitHub `zipball_url` source output does not offer installation;
7. the engine token is bounded, so `UE5.8` does not match `UE5.80`.

The current UE 5.8 report contains seven succeeded tests and zero failed tests.
The protected host resolved the engine from `ActivationHost.uproject` and loaded
the freshly linked `UnrealEditor-MonolithCore.dll`.

---

## 4. Static-Policy Compatibility Note

The current upstream revision predates the repository's newer hosted static
checker and configuration. A same-config comparison against clean upstream
found no branch-introduced blocker. The comparison reported line-ending
advisories for the three new selector/test files and unchanged
`Scripts/monolith_proxy.sh`; `git ls-files --eol` confirms every committed blob
is LF, so these are Windows working-tree checkout observations rather than
canonical diff defects.

---

## 5. Explicit Non-Scope

The existing `bPerEngineRelease` policy for eligible `Monolith-*.zip` assets is
intentionally unchanged. Maintainer review identified a broader rewrite of that
policy as a separate concern. This PR only prevents unrelated-plugin archives
from influencing the Monolith release mode while making release-asset selection
safe and testable.
