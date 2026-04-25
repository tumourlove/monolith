# J1: UI<->GAS Attribute Binding Test Spec

**Status:** Spec only — implementation tests pending I1 landing + editor restart.

**Source plan:** `Docs/plans/2026-04-26-ui-gas-attribute-binding.md` (H1 design, I1 implementation)
**Phase J brief:** `Plugins/Monolith/Docs/plans/2026-04-25-comprehensive-fix-plan.md:331-335`
**Action surface under test:** `gas::bind_widget_to_attribute`, `gas::list_attribute_bindings`, `gas::unbind_widget_attribute`, `gas::clear_widget_attribute_bindings`, plus `ui::bind_widget_to_attribute` alias.
**Runtime classes under test:** `UMonolithGASAttributeBindingClassExtension`, `FMonolithGASAttributeBindingSpec`.

---

## Setup

### Prerequisites
1. **I1 has landed** — UBT clean, MonolithGAS module loads, `monolith_discover("gas")` reports the four new actions.
2. **Editor up**, MCP connected, project loaded.
3. **GAS plugin enabled** in `.uproject` (Leviathan ships with it; verify `WITH_GAMEPLAYABILITIES==1`).
4. **A C++ AttributeSet exists** for binding targets — Leviathan ships `ULeviathanVitalsSet` at `Source/Leviathan/Public/GAS/LeviathanVitalsSet.h` (production code, F4 / 2026-04-26). Six attributes: `Health`/`MaxHealth`, `Sanity`/`MaxSanity`, `Stamina`/`MaxStamina` (all default 100, replicated `REPNOTIFY_Always`, clamped via `PreAttributeChange` + `PostGameplayEffectExecute`). Bind strings use `ULeviathanVitalsSet.<Attr>`. Spec: `Docs/specs/SPEC_Vitals.md`. **No BP fallback needed** — the prior `UAS_TestVitals` disposable-fallback note is obsolete.
5. **A test pawn class** with a `UAbilitySystemComponent` and the AttributeSet granted. Author `BP_TestGASPawn` at `/Game/Tests/Monolith/BP_TestGASPawn` as a disposable per `feedback_test_assets_throwaway` — see steps below. **Does NOT pre-exist** (verified 2026-04-26: project ships no ASC-bearing pawn; `SandboxCharacter_CMC` is movement-only and is intentionally not reparented per `feedback_no_reparent_sandboxcharacter`).

### Disposable assets to create (per `feedback_test_assets_throwaway`)

#### Pawn fixture (Setup #5)
Author the test pawn explicitly via MCP — runtime ASC + AttributeSet grant happen on `BeginPlay`:
1. `blueprint_query("create_blueprint", {parent_class: "/Script/Engine.Pawn", path: "/Game/Tests/Monolith/BP_TestGASPawn"})`
2. `blueprint_query("add_component", {bp_path: "/Game/Tests/Monolith/BP_TestGASPawn", component_class: "/Script/GameplayAbilities.AbilitySystemComponent", component_name: "AbilitySystem"})`
3. Author a `BeginPlay` event that calls `ASC->InitAbilityActorInfo(this, this)` and grants `ULeviathanVitalsSet` (use `gas::give_ability` analogue; AttributeSet is added via `ASC->AddSet<ULeviathanVitalsSet>()` — wire via a small Blueprint event graph or a one-shot `gas::ensure_attribute_set_added` author-time helper if available).
4. `blueprint_query("compile_blueprint", {bp_path: "/Game/Tests/Monolith/BP_TestGASPawn"})` — assert `ok=true`.

If the editor's BP graph authoring is too costly via MCP for a given test agent, fall back to Lucas-driven manual authoring of the `BeginPlay` event — but the asset path and components above must be reachable via MCP first.

#### Widget Blueprints (Setup §Disposable assets)
Author via `ui::create_widget_blueprint` + `ui::add_widget` so the named children actually exist (J1 first run had to substitute existing widgets that did not match the spec's named children):
1. `ui::create_widget_blueprint({asset_path: "/Game/Tests/Monolith/UIBinding/WBP_BindTest_HealthBar"})` then `ui::add_widget({wbp_path: "/Game/Tests/Monolith/UIBinding/WBP_BindTest_HealthBar", widget_class: "ProgressBar", widget_name: "HealthBar", parent: "<root canvas>"})`.
2. `ui::create_widget_blueprint({asset_path: "/Game/Tests/Monolith/UIBinding/WBP_BindTest_TextCounter"})` then `ui::add_widget({widget_class: "TextBlock", widget_name: "ManaCounter"})`.
3. `ui::create_widget_blueprint({asset_path: "/Game/Tests/Monolith/UIBinding/WBP_BindTest_ColorImage"})` then `ui::add_widget({widget_class: "Image", widget_name: "StaminaVignette"})`.
4. `ui::create_widget_blueprint({asset_path: "/Game/Tests/Monolith/UIBinding/WBP_BindTest_Multi"})` + three `ui::add_widget` calls for HealthBar (ProgressBar) / ManaCounter (TextBlock) / StaminaVignette (Image) under one root canvas.
5. `ui::create_widget_blueprint({asset_path: "/Game/Tests/Monolith/UIBinding/WBP_BindTest_NonBindable"})` then `ui::add_widget({widget_class: "CanvasPanel", widget_name: "Container"})` (failure-mode fixture).
6. Compile each: `blueprint_query("compile_blueprint", {bp_path: "<path>"})` → assert `ok=true` for all.

If `ui_query` is not surfaced by `ToolSearch` in the agent harness, run `ToolSearch` with `query: "select:mcp__monolith__ui_query"` first (per the dispatcher-loading rule documented in `Docs/research/2026-04-26-j-spec-environment-findings.md` §A).

#### GameplayEffect fixtures
Author via `gas::create_gameplay_effect` (or `gas::create_effect_from_template`):
1. `gas::create_gameplay_effect({asset_path: "/Game/Tests/Monolith/UIBinding/GE_TestDamage", duration_policy: "Instant", modifiers: [{attribute: "ULeviathanVitalsSet.Health", op: "Add", magnitude: -25}]})`
2. `gas::create_gameplay_effect({asset_path: "/Game/Tests/Monolith/UIBinding/GE_TestHeal", duration_policy: "Instant", modifiers: [{attribute: "ULeviathanVitalsSet.Health", op: "Add", magnitude: 10}]})`
3. `gas::create_gameplay_effect({asset_path: "/Game/Tests/Monolith/UIBinding/GE_TestMaxBoost", duration_policy: "Instant", modifiers: [{attribute: "ULeviathanVitalsSet.MaxHealth", op: "Add", magnitude: 50}]})`

### Initial-state assertions before any test runs
- `monolith_discover("gas")` includes `bind_widget_to_attribute`, `list_attribute_bindings`, `unbind_widget_attribute`, `clear_widget_attribute_bindings`.
- `monolith_discover("ui")` includes `bind_widget_to_attribute` (alias) IF GAS module is present; absent if `MONOLITH_RELEASE_BUILD=1` strip simulated.
- All `WBP_BindTest_*` assets compile clean (`blueprint_query("compile_blueprint", ...)` returns `ok=true`).
- All `WBP_BindTest_*` `list_attribute_bindings` returns `bindings: []` initially.

---

## Test Cases

### TC1.1 — Golden path: ProgressBar.Percent on damage GE
**Goal:** A bound HealthBar widget visibly tracks Health/MaxHealth as a damage GE applies.
**Setup:**
- `WBP_BindTest_HealthBar` with `HealthBar` UProgressBar.
- `BP_TestGASPawn` spawned, ASC granted `ULeviathanVitalsSet` with `Health=100, MaxHealth=100`.
- Bind authored via `gas::bind_widget_to_attribute` with:
  - `wbp_path=/Game/Tests/Monolith/UIBinding/WBP_BindTest_HealthBar`
  - `widget_name=HealthBar`, `target_property=Percent`
  - `attribute=ULeviathanVitalsSet.Health`, `max_attribute=ULeviathanVitalsSet.MaxHealth`
  - `owner_resolver=owning_player_pawn`
**Steps:**
1. Spawn the widget into the player viewport (`AddToPlayerScreen`).
2. Read `HealthBar.Percent` once — record initial value.
3. Apply `GE_TestDamage` to the pawn's ASC (Health 100 -> 75).
4. Wait 2 frames.
5. Read `HealthBar.Percent` again.
6. Apply `GE_TestDamage` twice more (Health 75 -> 50 -> 25).
7. Read `HealthBar.Percent`.
**Expected:** Percent = 1.0 -> 0.75 -> 0.25 within 2 frames per change.
**Pass criteria:** `abs(measured - expected) < 0.005` for each step. No log warnings about missing ASC. Binding fires on each GE apply (verified via `LogMonolithGAS` Trace lines counted = 3).

### TC1.2 — MaxAttribute change recomputes ratio
**Goal:** When MaxHealth changes, the ratio recomputes without manual re-bind.
**Setup:** As TC1.1, Health=50, MaxHealth=100, Percent=0.5.
**Steps:**
1. Apply `GE_TestMaxBoost` (MaxHealth 100 -> 150).
2. Wait 2 frames.
3. Read `HealthBar.Percent`.
**Expected:** Percent = 50/150 ≈ 0.333.
**Pass criteria:** `abs(0.333 - measured) < 0.005`. Confirms the extension subscribed to BOTH `Health` and `MaxHealth` change delegates.

### TC1.3 — Attribute exceeds max (overheal)
**Goal:** Binding clamps to [0,1] when Health > MaxHealth.
**Setup:** As TC1.1, Health=100, MaxHealth=100.
**Steps:**
1. Apply a custom GE setting Health to 150 (using `gas::set_attribute_value` with raw=true).
2. Wait 2 frames.
3. Read `HealthBar.Percent`.
**Expected:** Percent = 1.0 (clamped).
**Pass criteria:** Measured == 1.0 exactly (no float drift past clamp). One Log line at Verbosity=Log noting clamp engaged.

### TC1.4 — Widget detached mid-binding
**Goal:** When the widget is removed from viewport, the extension unsubscribes cleanly with no leaked delegates.
**Setup:** As TC1.1, widget visible, binding active.
**Steps:**
1. Snapshot delegate count: `ASC->GetGameplayAttributeValueChangeDelegate(HealthAttr).GetAllocatedSize()` proxy via test-only accessor or count via reflection on the multicast.
2. `RemoveFromParent()` on the widget.
3. Wait 1 frame.
4. Apply `GE_TestDamage` 50 times rapidly.
5. Snapshot delegate count again.
6. Force GC (`gc.Collect`).
7. Re-snapshot.
**Expected:** Delegate count drops by 2 (Health + MaxHealth) on `RemoveFromParent`. No tick-time work after detach. GC reclaims widget without complaint.
**Pass criteria:** Pre-detach count - post-detach count = 2. Post-GC count unchanged. No `LogUObjectGlobals` warnings about lingering refs to the widget. No new HealthBar.Percent writes after detach (Percent stays at last value before detach).

### TC1.5 — GE expires (duration GE rolls back)
**Goal:** Duration-based GE that increases MaxHealth temporarily — when it expires, ratio updates.
**Setup:** As TC1.1, Health=50, MaxHealth=100, Percent=0.5. Apply `GE_TestMaxBoost` as DURATION GE for 2 seconds.
**Steps:**
1. After applying, read Percent (should be ~0.333).
2. Wait 2.5 seconds (past expiry).
3. Read Percent.
**Expected:** Returns to 0.5 after GE expiry.
**Pass criteria:** `abs(0.5 - measured) < 0.005` post-expiry. No double-fire (delegate fires twice per expiry: once for revert, once for cleanup is fine; only the final value matters).

### TC1.6 — Smoothed update interpolates instead of snapping
**Goal:** `update_policy=on_change_smoothed` lerps over multiple frames.
**Setup:** As TC1.1 but bind with `update_policy=on_change_smoothed` and `format_args.smoothing_speed=2.0` (slow).
**Steps:**
1. Health=100, Percent=1.0 at frame 0.
2. Apply `GE_TestDamage` x3 in one frame (Health -> 25, target Percent=0.25).
3. Sample Percent at frame +1, +2, +5, +30.
**Expected:** Percent decreases monotonically over ~15 frames (depends on tick rate + lerp speed). Never overshoots target.
**Pass criteria:** Percent[+1] > 0.25 AND Percent[+30] within 0.01 of 0.25. Strictly monotonic decreasing samples.

### TC1.7 — Late owner spawn
**Goal:** Widget added before pawn-with-ASC exists — binding self-heals.
**Setup:** As TC1.1, but spawn the widget BEFORE possessing the pawn.
**Steps:**
1. Spawn widget (no pawn possessed yet — `GetOwningPlayer()->GetPawn()` returns null).
2. Read Percent — record (likely default 0).
3. Wait 0.1s — confirm no spammy Warning logs (Log-level is acceptable for first second).
4. Possess the pawn.
5. Wait 2 frames.
6. Read Percent.
**Expected:** Percent reads correct Health/MaxHealth ratio after possession.
**Pass criteria:** Final Percent matches actual ratio. Log warnings about missing ASC only at Log verbosity in first second, escalate to Warning only after 1s grace.

### TC1.8 — TextBlock format_string round-trip
**Goal:** `Text` target with `format_string` produces the exact templated FText.
**Setup:** `WBP_BindTest_TextCounter` with `ManaCounter` UTextBlock (widget name kept for legacy reasons; the bound attribute is `Sanity`, not Mana — `ULeviathanVitalsSet` ships Health/Sanity/Stamina per `Docs/specs/SPEC_Vitals.md`). Bind with:
- `attribute=ULeviathanVitalsSet.Sanity`, `max_attribute=ULeviathanVitalsSet.MaxSanity`
- `format=format_string`, `format_args={"template": "{0:int} / {1:int}"}`
- `BP_TestGASPawn` initial `Sanity=37, MaxSanity=100`.
**Steps:**
1. Spawn widget, possess pawn.
2. Wait 2 frames.
3. Read `ManaCounter.Text.ToString()`.
4. Apply heal GE on Sanity (+15).
5. Read text again.
**Expected:** First read = `"37 / 100"` exact. Second read = `"52 / 100"`.
**Pass criteria:** String equality (no padding, no locale formatting). UTF-8 round-trip clean.

### TC1.9 — Image gradient mode
**Goal:** `ColorAndOpacity` with `gradient` format lerps between two colors.
**Setup:** `WBP_BindTest_ColorImage` with `StaminaVignette` UImage. Bind with `attribute=Stamina`, `max_attribute=MaxStamina`, `format=gradient`, `format_args={"low":[1,0,0,0.6], "high":[1,1,1,0]}`.
**Steps:**
1. Set `Stamina=0` -> read ColorAndOpacity.
2. Set `Stamina=50, MaxStamina=100` -> read.
3. Set `Stamina=100` -> read.
**Expected:** [1,0,0,0.6] / [1, 0.5, 0.5, 0.3] / [1,1,1,0]. (mid is bilinear lerp.)
**Pass criteria:** Each component within `±0.01` of expected.

### TC1.10 — Multi-binding on same WBP
**Goal:** Three distinct bindings on the same WBP all fire independently.
**Setup:** `WBP_BindTest_Multi` with HealthBar (Percent), ManaCounter (Text bound to Sanity), StaminaVignette (ColorAndOpacity). Author all three bindings.
**Steps:**
1. `gas::list_attribute_bindings(wbp_path=...)` -> assert 3 entries returned in author order.
2. Spawn widget, possess pawn.
3. Drive Health, Sanity, Stamina each independently with three separate GEs.
4. Read all three target properties.
**Expected:** Each target reflects its own attribute. No cross-bleeding (e.g. Health change does not perturb Sanity text).
**Pass criteria:** `list_attribute_bindings` returns list with all expected fields per `Docs/plans/2026-04-26-ui-gas-attribute-binding.md` return-value spec. All three properties update correctly within 2 frames.

---

## Per-Action Sanity Tests

### TC1.11 — `bind_widget_to_attribute` response shape
**Goal:** Verify return JSON matches the contract.
**Steps:** Call action with TC1.1 inputs.
**Expected response shape:**
```jsonc
{
  "ok": true,
  "wbp_path": "/Game/Tests/Monolith/UIBinding/WBP_BindTest_HealthBar",
  "widget_name": "HealthBar",
  "widget_class": "ProgressBar",
  "target_property": "Percent",
  "attribute": "ULeviathanVitalsSet.Health",
  "max_attribute": "ULeviathanVitalsSet.MaxHealth",
  "format": "percent_0_1",       // resolved from auto
  "owner_resolver": "owning_player_pawn",
  "update_policy": "on_change",
  "extension_class": "UMonolithGASAttributeBindingClassExtension",
  "binding_index": 0,
  "replaced": false,             // Phase J F6: present whenever the bind succeeds.
                                 // true when an existing binding for the same (widget_name, target_property)
                                 // pair was overwritten via replace_existing=true (default); false on first author.
  "compiled": true,
  "saved": true
  // "warnings": [...]            // Phase J F6: OMITTED when empty; present (non-empty array) only when warnings were raised.
}
```
**Pass criteria:** All keys present, types correct, `binding_index` is integer >= 0, `replaced` is bool, `compiled==true`, `saved==true`. `warnings` is OMITTED when empty (impl pattern); when present it is a non-empty string array.

### TC1.12 — `list_attribute_bindings` response shape
**Goal:** Listing returns array with full per-binding spec.
**Setup:** Author 2 bindings on same WBP.
**Expected response:**
```jsonc
{
  "ok": true,
  "wbp_path": "...",
  "bindings": [
    {
      "binding_index": 0,
      "widget_name": "HealthBar",
      "widget_class": "ProgressBar",
      "target_property": "Percent",
      "attribute": "ULeviathanVitalsSet.Health",
      "max_attribute": "ULeviathanVitalsSet.MaxHealth",
      // Phase J F5 #2: split fields are also emitted alongside the composite for back-compat round-trip:
      "attribute_set_class": "/Script/Leviathan.LeviathanVitalsSet",
      "attribute_name": "Health",
      "max_attribute_set_class": "/Script/Leviathan.LeviathanVitalsSet",
      "max_attribute_name": "MaxHealth",
      "format": "percent_0_1",
      "owner_resolver": "owning_player_pawn",
      "update_policy": "on_change",
      "smoothing_speed": 6.0
    },
    { /* second binding */ }
  ],
  "count": 2,
  "note": "These are GAS attribute bindings (Monolith). Distinct from ui::get_widget_bindings which reads UMG FDelegateRuntimeBinding."
}
```
**Pass criteria:** Array length matches author count. `count` field equals array length. Order matches insertion order. Every field round-trips byte-identical to the author input (after auto-format resolution). Composite `attribute` / `max_attribute` strings are equal to `<short class name>.<property name>` (where short class name is `FPackageName::ObjectPathToObjectName(class_path)`).

### TC1.13 — `unbind_widget_attribute` removes correct entry
**Goal:** Removing one binding leaves others intact in original order.
**Setup:** Author 3 bindings: A, B, C.
**Steps:**
1. `unbind_widget_attribute(widget_name=B.widget_name, target_property=B.target_property)`.
2. `list_attribute_bindings` -> assert remaining are A and C in that order.
**Expected response on unbind:**
```jsonc
{
  "ok": true,
  "wbp_path": "...",
  "removed": true,
  "removed_binding_index": 1,
  "remaining_count": 2,
  "compiled": true,
  "saved": true
}
```
**Pass criteria:** Remaining bindings exactly A and C. `binding_index` after removal is reflowed (A=0, C=1). WBP still compiles clean.

### TC1.14 — `clear_widget_attribute_bindings` removes all
**Goal:** Clear empties the array but does not damage the WBP otherwise.
**Setup:** Author 3 bindings.
**Steps:**
1. `clear_widget_attribute_bindings`.
2. `list_attribute_bindings` -> assert empty.
3. Compile WBP and spawn instance — verify no startup warnings.
**Expected response:**
```jsonc
{
  "ok": true,
  "wbp_path": "...",
  "removed_count": 3,
  "compiled": true,
  "saved": true
}
```
**Pass criteria:** Empty bindings list. Spawned widget functions normally with no extension behaviour.

### TC1.15 — `replace_existing` semantics
**Goal:** With `replace_existing=true`, second author overwrites; with `=false`, second author errors.
**Setup:** Author binding A. Author binding A' (same widget_name + target_property, different attribute).
**Steps (variant A — replace_existing=true, default):**
1. Author A then A'.
2. `list_attribute_bindings` -> 1 entry, attribute = A's NEW attribute.
**Steps (variant B — replace_existing=false):**
1. Author A then A' with `replace_existing=false`.
2. Second call returns `ok=false, error="Binding already exists for (HealthBar, Percent)"`.
3. `list_attribute_bindings` -> 1 entry, attribute = ORIGINAL A's attribute (byte-identical).
**Pass criteria:** Variant A: array length 1, attribute updated. Variant B: error returned, original preserved (verify all spec fields unchanged).

---

## Failure Modes

| Case | Input | Expected Error |
|------|-------|----------------|
| Invalid `widget_name` | `widget_name="DoesNotExist"` | `ok=false, error="Widget 'DoesNotExist' not found in WBP tree. Available: [HealthBar, ManaCounter, ...]"` (lists the widgets actually in the tree, alphabetically sorted; capped at 20 — beyond that the message points at SPEC_GAS_UIBinding.md) |
| Invalid attribute_set_class | `attribute="UNotARealSet.Health"` | `ok=false, error="AttributeSet class not found: UNotARealSet"` (Phase J F6 #8: enumeration of all loaded `UAttributeSet` subclasses requires a full `TObjectIterator<UClass>` scan, grows with project size, and the resulting list is too long to scan visually — terser message accepted as canonical.) |
| Invalid attribute property | `attribute="ULeviathanVitalsSet.Heath"` (typo) | `ok=false, error="Property 'Heath' not found on AttributeSet 'LeviathanVitalsSet'. Valid: [Health, MaxHealth, Sanity, MaxSanity, Stamina, MaxStamina]"` (Phase J F6 #9: impl returns the FULL valid-property list rather than a single Levenshtein "Did you mean" suggestion. The full list handles ambiguous typos better and avoids the cost of a string-distance compute on every miss.) |
| Invalid `target_property` (typo) | `target_property="Percentage"` on UProgressBar | `ok=false, error="Property 'Percentage' invalid for ProgressBar. Valid: [Percent, RenderOpacity, Visibility]. See Docs/plans/2026-04-26-ui-gas-attribute-binding.md compatibility table."` |
| Bind on non-bindable widget type | `widget_name="Container"` (UCanvasPanel) | `ok=false, error="Widget class 'CanvasPanel' has no bindable properties in compatibility table. See SPEC_GAS_UIBinding.md."` |
| Image.ColorAndOpacity without gradient | `target_property=ColorAndOpacity, format=auto` | `ok=false, error="UImage.ColorAndOpacity requires format=gradient. Single attribute cannot fill 4 channels."` |
| Both `attribute` and `max_attribute` malformed | both missing the `.` separator | `ok=false, error="Attribute must be in form 'ClassName.PropertyName'"` |
| `wbp_path` does not exist | `/Game/Bogus/WBP_None` | `ok=false, error="Widget Blueprint asset not found: /Game/Bogus/WBP_None"` |
| `wbp_path` is not a Widget Blueprint | path to a regular Blueprint | `ok=false, error="Asset at /Game/.../BP_Foo is not a Widget Blueprint (got Blueprint)"` |
| Missing `max_attribute` for ProgressBar.Percent on a non-normalized attribute | author HealthBar.Percent with attribute that has default value > 1 and no max_attribute | `ok=true` BUT `warnings:["max_attribute not provided; values will be clamped to [0,1] which may cause permanent fill if Health > 1"]` (warn-and-proceed, not error) |
| `unbind_widget_attribute` with non-existent binding | widget_name+target_property pair never bound | `ok=false, error="No binding found for (NonExistent, Percent)"` |
| `list_attribute_bindings` on WBP with no extension installed | virgin WBP | `ok=true, bindings=[]` (NOT an error — empty is valid) |
| Action called when GAS module absent | simulate `MONOLITH_RELEASE_BUILD=1` strip | `ui::bind_widget_to_attribute` alias not registered. Direct `gas::bind_widget_to_attribute` returns `ok=false, error="Namespace 'gas' not registered (GameplayAbilities plugin disabled or stripped)"` |
| Owner resolver invalid string | `owner_resolver=banana` | `ok=false, error="Unknown owner_resolver 'banana'. Valid: [owning_player_pawn, owning_player_state, owning_player_controller, self_actor, named_socket:<tag>]"` |
| `format_args.template` missing required slots | `format=format_string` but template lacks `{0}` | `ok=false, error="format=format_string requires '{0}' (and '{1}' if max_attribute set) in template"` |

---

## Notes

### Open questions for I1 implementation team
1. **TC1.4 delegate-count introspection** — UE 5.7's multicast delegate doesn't expose count directly. Implementation may need a test-only friend accessor or a `monolith_diagnostic_delegate_count(attribute)` debug action. Flag at PR review.
2. **TC1.5 duration-GE expiry timing** — `OnGameplayAttributeValueChange` fires on duration GE expiry; verify in PIE before relying on it for the test. If flaky, add a 1-frame wait + manual `IGameplayAbilityComponentInterface::GetAttributeValue` recheck.
3. **TC1.7 grace-period log filter** — implementation must distinguish first-second silence from later-time loud Warning. Test verifies via log capture; ensure log category is `LogMonolithGAS` and verbosity escalates correctly.
4. **GBA Blueprint AttributeSet support (open question 1 in H1 plan)** — TC1.1 uses C++ `ULeviathanVitalsSet`. If GBA-authored AttributeSets are supported, add TC1.16 binding to a `/Game/Tests/Monolith/AS_TestVitals_C` BP set. If not supported in v1, document in failure-mode table.
5. **2-player PIE replication smoke test (open question 4 in H1 plan)** — out of scope for J1's main flow. Add as TC1.17 if time permits before release: 2-player PIE, server applies GE to host pawn, client widget on host pawn updates correctly.

### Dependencies on I-phase landing
- I1 must ship before any TC runs (no PIE before code).
- TC1.16 requires AS_TestVitals BP — author during test setup, not in I1's deliverables.
- All TCs require editor up + UBT successful for I1 changes.

### Coverage map (per H1 plan §Test Plan)
| H1 plan test | This spec maps to | Notes |
|---|---|---|
| Unit #1 RegisterActionsRoundtrip | Initial-state assertions | Pre-test sanity |
| Unit #2 AddBindingPersists | TC1.11 + reload check (manual step) | Add explicit reload to TC1.11 if not implicit |
| Unit #3 ReplaceExisting | TC1.15 | Direct map |
| Unit #4 RoundTripList | TC1.12, TC1.10 | Direct map |
| Unit #5 UnbindRemoves | TC1.13 | Direct map |
| Unit #6 ClearRemovesAll | TC1.14 | Direct map |
| Unit #7 RejectUnknownProperty | Failure-mode row | Direct map |
| Unit #8 RejectUnknownAttribute | Failure-mode row | Direct map |
| Unit #9 RejectMissingMaxForProgress | Failure-mode row | Warn-and-proceed, not error |
| Unit #10 RejectImageColorWithoutGradient | Failure-mode row | Direct map |
| Integration #11 PIE_HealthBarLive | TC1.1 | Direct map |
| Integration #12 PIE_TextFormatLive | TC1.8 | Direct map |
| Integration #13 PIE_ColorGradient | TC1.9 | Direct map |
| Integration #14 PIE_LateOwnerSpawn | TC1.7 | Direct map |
| Integration #15 PIE_DestructUnsubscribes | TC1.4 | Direct map |
| Negative #16 GASModuleAbsent | Failure-mode row | Strip-build env required |

Total this spec: **15 test cases + 14 failure-mode rows**.
