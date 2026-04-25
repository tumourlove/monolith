# J1 Phase D Regression Smoke — F12 sanity

**Headline:** Phase D smoke confirms F12 did not regress F2/F3/F5/F6.

**Executed:** 2026-04-26 against Monolith **v0.14.7** (post-F12 / dv.commit.425). Editor up, MCP nominal at port 9316, 1462 total actions, 20 namespaces. Engine `++UE5+Release-5.7-CL-51494982`.

**Scope:** Sample-based regression smoke after F12 added cross-namespace MCP dispatcher tooling to 5 agents' frontmatter. F12 is purely additive (frontmatter only, no GAS code touch). 11 high-leverage spot checks vs the J1 spec (`Plugins/Monolith/Docs/testing/2026-04-26-j1-ui-gas-binding-test.md`) and the J1 v0.14.6 baseline (`Plugins/Monolith/Docs/testing/2026-04-26-j1-results.md`).

---

## Fixtures

Created under `/Game/Tests/Monolith/J1/` per `feedback_test_assets_throwaway`:

| Asset | Class | Purpose |
|-------|-------|---------|
| `WBP_J1_RegressionTarget` | UWidgetBlueprint (UserWidget root CanvasPanel) | Bind target. Children: `HealthBar` UProgressBar, `ManaCounter` UTextBlock. Authored via `ui::create_widget_blueprint` + 2x `ui::add_widget`. Compiled clean (`status: UpToDate, errors: 0, warnings: 0`). |
| `BP_J1_NotAWidget` | UBlueprint (parent: AActor) | Non-WBP fixture for E2 LoadWBP wrong-class branch. |

`ULeviathanVitalsSet` (F4 / 2026-04-26 / `Source/Leviathan/Public/GAS/LeviathanVitalsSet.h:40`) used directly as the AttributeSet target — no disposable BP fallback needed this run. Bind strings used the bare-class form `LeviathanVitalsSet.<Attr>` because the resolver's `U`-prefix retry path (`MonolithGASUIBindingActions.cpp:137`) doesn't strip a leading `U` from user input — it adds one. The spec example `ULeviathanVitalsSet.<Attr>` therefore needs the bare-name variant in practice. (Pre-existing impl/spec drift; **not** a Phase J regression.)

---

## Per-Row Results

### A. F2 ParseOwner regression — 3/3 PASS
Source: `MonolithGASUIBindingActions.cpp:185-221` (`ParseOwner`).

| # | Input | Expected | Observed | Verdict |
|---|-------|----------|----------|---------|
| A1 | `owner_resolver=banana` | `ok=false`, error enumerates valid options | `Unknown owner_resolver 'banana'. Valid: [owning_player_pawn, owning_player_state, owning_player_controller, self_actor, named_socket:<tag>]` — **byte-exact** match vs spec failure-mode row 14 | **PASS** |
| A2 | `owner_resolver=""` | `ok=true`, defaults to `owning_player_pawn` | `ok=true, owner_resolver:"owning_player_pawn", binding_index:0, replaced:false` | **PASS** (back-compat preserved per source line 187) |
| A3 | `owner_resolver=named_socket:HUDTag` | `ok=true`, socket tag parsed | `ok=true, owner_resolver:"named_socket"`. Subsequent `list_attribute_bindings` confirms `owner_socket_tag:"HUDTag"` stored separately on the spec | **PASS** |

### B. F3 ValidateFormatStringPayload regression — 2/2 PASS
Source: `MonolithGASUIBindingActions.cpp:275-297` (`ValidateFormatStringPayload`).

| # | Input | Expected | Observed | Verdict |
|---|-------|----------|----------|---------|
| B1 | `format=format_string:NoSlots`, max_attribute bound | `ok=false`, error mentions missing slot | `format=format_string requires '{0}' (and '{1}' if max_attribute set) in template, got: NoSlots` — **byte-exact** match vs spec failure-mode row 15 | **PASS** |
| B2 | `format=format_string:HP: {0}/{1}`, max_attribute bound | `ok=true` | `ok=true, format:"format_string", format_payload:"HP: {0}/{1}"`, both slots accepted | **PASS** |

### C. F5 response shape — 3/3 PASS
Source: F5 list-shape spec at `2026-04-26-j1-ui-gas-binding-test.md:215-247` and unbind shape at `:255-265`.

| # | Check | Observed | Verdict |
|---|-------|----------|---------|
| C1 | `bind_widget_to_attribute` returns `binding_index` (NOT `index`) | A2 response: `binding_index:0`. B2 response: `binding_index:1`. Field name `index` not present anywhere | **PASS** |
| C2 | `list_attribute_bindings` rows have composite `attribute` + split `attribute_set_class`+`attribute_name` | Both rows carry: `attribute:"LeviathanVitalsSet.Health"` + `attribute_set_class:"/Script/Leviathan.LeviathanVitalsSet"` + `attribute_name:"Health"`. Same pattern for `max_attribute_set_class` + `max_attribute_name`. `widget_class` field also present per row | **PASS** |
| C3 | `unbind_widget_attribute` returns `removed_binding_index` | `removed:true, removed_binding_index:0, remaining_count:1` — the `removed_binding_index` field that prior J1 (v0.14.6) marked as missing is now emitted | **PASS** |

### D. F6 omit-when-empty — 1/1 PASS

| # | Check | Observed | Verdict |
|---|-------|----------|---------|
| D1 | `list_attribute_bindings` with no warnings → `warnings` field OMITTED | The C2 list response above contains zero `warnings` keys at the top level OR per-row level — the field is genuinely absent (not `[]`, not `null`). Matches F6 spec note `:213` "OMITTED when empty" | **PASS** |

### E. F5 LoadWBP branch split — 2/2 PASS
The Phase J F5 fix split the failure-mode row 9 (path-exists-but-wrong-class) out of the row-8 collapsed "not found" branch.

| # | Input | Expected error | Observed | Verdict |
|---|-------|----------------|----------|---------|
| E1 | `wbp_path=/Game/Tests/Monolith/J1/DoesNotExist` | "Widget Blueprint asset not found: ..." | `Widget Blueprint asset not found: /Game/Tests/Monolith/J1/DoesNotExist` — byte-exact match vs FM row 8 | **PASS** |
| E2 | `wbp_path=/Game/Tests/Monolith/J1/BP_J1_NotAWidget` (UBlueprint, not UWidgetBlueprint) | "Asset at ... is not a Widget Blueprint (got Blueprint)" | `Asset at /Game/Tests/Monolith/J1/BP_J1_NotAWidget is not a Widget Blueprint (got Blueprint)` — byte-exact match vs FM row 9. **Branch split now functional** (was collapsed in v0.14.6 J1 run) | **PASS** |

---

## Cleanup

`editor::delete_assets` removed 2/2 (`requested:2, found:2, deleted:2`). `/Game/Tests/Monolith/J1/` folder cleared.

---

## Editor Health Timeline

| Checkpoint | Result |
|------------|--------|
| Pre-flight (T0) | `monolith_status` ok, server_running=true, 1462 actions, port 9316 |
| Mid-run (after fixture creation) | `monolith_status` stable |
| Mid-run (after E1/E2 negative tests) | `monolith_status` stable |
| Post-cleanup (final) | `monolith_status` stable, same version + action count |

**Crash flags:** none.

---

## Net J1 State Update

| Metric | v0.14.6 (prior) | v0.14.7 (this run) |
|--------|-----------------|--------------------|
| Test cases full PASS | 5 | unchanged for the 5 sampled (TC1.10, TC1.11 sub-shape, TC1.12 sub-shape, TC1.13 sub-shape, TC1.14, TC1.15 not re-tested in smoke; TC1.11/TC1.12/TC1.13 shape drifts confirmed FIXED) |
| FM full PASS | 5 | **+2** (FM row 9 LoadWBP branch split now PASS, was PARTIAL; FM row 14 owner_resolver=banana now PASS, was FAIL; FM row 15 format_string slot-check now PASS, was FAIL) |
| FM FAIL (release blockers) | 2 | **0** (both Phase J F2/F3 release blockers RESOLVED) |

---

## Regression Verdict

**F12 did NOT introduce regressions in the GAS UI binding surface.** The F2/F3 validation gates (release-blocker fixes), the F5 response-shape rewrites, and the F6 omit-when-empty pattern all behave per spec at v0.14.7 / dv.commit.425. The release blockers from the v0.14.6 J1 run (FM rows 14 + 15) are resolved. F12's frontmatter-only additive scope is consistent with the observed zero-impact result.
