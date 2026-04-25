# J2: BT<->GAS Ability Task Test Spec

**Status:** Spec only — implementation tests pending I2 landing + editor restart.

**Source plan:** `Docs/plans/2026-04-26-bt-gas-ability-task.md` (H2 design, I2 implementation)
**Phase J brief:** `Plugins/Monolith/Docs/plans/2026-04-25-comprehensive-fix-plan.md:336-340`
**Action surface under test:** `ai::add_bt_use_ability_task`
**Runtime classes under test:** `UBTTask_TryActivateAbility` (new C++ in `MonolithAI/Public/BehaviorTreeTasks/`)

---

## Setup

### Prerequisites
1. **I2 has landed** — UBT clean, MonolithAI module rebuilt with GameplayAbilities dependency, `monolith_discover("ai")` reports `add_bt_use_ability_task`.
2. **Editor up**, MCP connected, project loaded.
3. **GAS plugin enabled**, `WITH_GAMEPLAYABILITIES==1` confirmed via `source_query` probe.
4. **Disposable test abilities** under `/Game/Tests/Monolith/Abilities/` (create as disposables; verified 2026-04-26 — these may already exist from prior test runs):
   - `GA_Test_Instant` — succeeds in one tick (calls `EndAbility` from `ActivateAbility`); cost 0; no cooldown.
   - `GA_Test_Duration1s` — calls `K2_EndAbility` after 1.0s `WaitDelay`.
   - `GA_Test_Duration5s` — same pattern, 5.0s duration. Used for abort tests.
   - `GA_Test_Cooldown` — has a 3s cooldown GE; instant otherwise.
   - `GA_Test_BlockedByTag` — has BlockAbilitiesWithTag = `Ability.Blocked`. ASC granted `Ability.Blocked` for the test.
   - `GA_Test_MeleeLight` — tagged `Ability.Combat.Melee.Light + Ability.Combat.Melee` for tag-based activation tests. (F16: uses existing `Ability.Combat.Melee.Light` registry tag; `Ability.Combat.Punch` deliberately not in tree per survival-horror curation — see `Config/DefaultGameplayTags.ini:20-37`.)
   - `GA_Test_MeleeHeavy` — tagged `Ability.Combat.Melee.Heavy + Ability.Combat.Melee`. (F16: uses existing `Ability.Combat.Melee.Heavy` registry tag; `Ability.Combat.Kick` deliberately not in tree per survival-horror curation.)
   - `GA_Test_Cancellable` — 5s duration, registers `CancelAbilitiesWithTag = Ability.CancelMe`. Used for cancellation test.
   - If absent, author each via `gas::create_ability` (or `gas::create_ability_from_template`) under `/Game/Tests/Monolith/Abilities/`.

5. **Disposable test BTs** under `/Game/Tests/Monolith/AI/` — **DO NOT pre-exist** (verified 2026-04-26 — `Content/Tests/Monolith/AI/` directory not present). Author both as part of test setup:
   - `ai_query("create_behavior_tree", {asset_path: "/Game/Tests/Monolith/AI/BT_AbilityTest_Empty"})` — empty BT (root only) for TCs that author single nodes off the root composite.
   - `ai_query("create_behavior_tree", {asset_path: "/Game/Tests/Monolith/AI/BT_AbilityTest_Sequence"})` then `ai_query("add_bt_node", {asset_path: "/Game/Tests/Monolith/AI/BT_AbilityTest_Sequence", node_class: "/Script/AIModule.BTComposite_Sequence", parent_id: ""})` — root + Sequence composite for TCs that need parented nodes (TC2.18). Capture the returned `node_id` for the Sequence — TC2.18 needs that GUID as the `parent_id`.
   - **Note (Phase J F1 BT crash hardening):** F-Phase F1 added a Tier-1 invariant — tasks may NOT attach as direct children of root. Always wire a composite first, then parent tasks under that composite. See §Failure Modes "F1 BT crash hardening" rows below for the protected error messages.

6. **Disposable test pawn** `/Game/Tests/Monolith/AI/BP_TestAIPawn` — **DO NOT pre-exist** (verified 2026-04-26 — project ships no ASC-bearing pawn). Author via:
   1. `blueprint_query("create_blueprint", {parent_class: "/Script/Engine.Pawn", path: "/Game/Tests/Monolith/AI/BP_TestAIPawn"})`
   2. `blueprint_query("add_component", {bp_path: "/Game/Tests/Monolith/AI/BP_TestAIPawn", component_class: "/Script/GameplayAbilities.AbilitySystemComponent", component_name: "AbilitySystem"})` (replication mode = Mixed for multiplayer TCs).
   3. Author or assign `AAIController` BP that calls `RunBehaviorTree` driving a configurable BT slot.
   4. Author a `BeginPlay` event that calls `ASC->InitAbilityActorInfo(this, this)`. For ability granting: **preferred** (Phase J F8) — call `gas_query("grant_ability_to_pawn", {pawn_bp_path: "/Game/Tests/Monolith/AI/BP_TestAIPawn", ability_class_path: "/Game/Tests/Monolith/Abilities/GA_Test_Instant"})` per ability. NOTE: requires the ASC subclass to expose a `TArray<TSubclassOf<UGameplayAbility>>` UPROPERTY whose name contains "Ability" (stock `UAbilitySystemComponent` does NOT — author a project ASC subclass with `StartupAbilities` first). Fallback if no ASC subclass is in play: author a one-shot BeginPlay test helper that loops `gas::give_ability` at runtime.
   5. `blueprint_query("compile_blueprint", {bp_path: "/Game/Tests/Monolith/AI/BP_TestAIPawn"})` — assert `ok=true`.

   The BeginPlay event-graph authoring may exceed agent-MCP fluency; if so, fall back to Lucas-driven manual authoring of just that event graph, but the asset path + ASC component + class parent above are MCP-reachable and SHOULD be authored that way.

7. **No-ASC variant pawn** `BP_TestAIPawn_NoASC` — same author steps as #6 MINUS step 2 (no ASC component) and step 4 (no ability grants). Used for missing-ASC failure path.

### Initial-state assertions before any test runs
- `monolith_discover("ai")` includes `add_bt_use_ability_task`.
- Action count for `ai` namespace == previous count + 1 (record current count first via discovery).
- `BT_AbilityTest_Empty` has exactly one node (root) before any test runs.

---

## Test Cases

### TC2.1 — Golden path: ability fires when task ticks (instant)
**Goal:** Action authors a node; pawn running the BT activates the instant ability immediately.
**Setup:**
- Author task: `ai::add_bt_use_ability_task(asset_path=/Game/Tests/Monolith/AI/BT_AbilityTest_Empty, ability_class=/Game/Tests/Monolith/Abilities/GA_Test_Instant, wait_for_end=true)`.
- Verify response shape (TC2.10).
- Spawn `BP_TestAIPawn` running `BT_AbilityTest_Empty`.
**Steps:**
1. Hook `ASC->OnAbilityActivated` (via test-only listener) and count fires.
2. Hook `ASC->OnAbilityEnded` and count fires.
3. Run BT for 0.5s.
4. Read fire counts.
5. Read BT execution trail by tailing the editor log: `mcp__leviathan-dev-toolkit__tail_log({lines: 200})` and grep for `LogMonolithAI` / `LogBehaviorTree` lines emitted by the new task. (Note: `ai::get_bt_execution_log` does NOT exist and is NOT in the Phase F8 scope — log-tailing is the canonical mechanism. A structured execution-log action remains an open extension if a future phase decides it's worth the engine-instrumentation cost.)
**Expected:** Activation count >= 1. End count >= 1. Task reports `Succeeded`.
**Pass criteria:** Both counts > 0. Final BT node status `Succeeded`. No `Failed` events on the task during the run.

### TC2.2 — Wait-for-end vs fire-and-forget (1s duration ability)
**Goal:** `wait_for_end` toggle changes BT timing.
**Setup A (`wait_for_end=true`):** Author task with `GA_Test_Duration1s`, `wait_for_end=true`.
**Setup B (`wait_for_end=false`):** Same ability, `wait_for_end=false`.
**Steps (each variant):**
1. Spawn pawn, run BT.
2. Record timestamp T0 at BT start.
3. Record T1 at task `Succeeded`.
4. Compute `dt = T1 - T0`.
**Expected:**
- Variant A: `dt >= 1.0s` (task held until ability ended).
- Variant B: `dt < 0.05s` (task succeeded immediately on activation).
**Pass criteria:** Variant A `dt` between 1.0s and 1.2s (allowing for tick scheduling). Variant B `dt` < 50ms.

### TC2.3 — Ability succeeds Succeeded, cancel -> Failed (with default flag)
**Goal:** End-reason determines success/failure mapping.
**Setup:** Author task with `GA_Test_Cancellable`, `wait_for_end=true`, `succeed_on_blocked=false` (default).
**Steps:**
1. Spawn pawn, run BT — ability activates.
2. After 0.5s, externally call `ASC->CancelAbilities(&Tag(Ability.CancelMe), nullptr)`.
3. Wait for BT task to terminate.
4. Read final task result.
**Expected:** Task reports `Failed` (not Succeeded), because `EndedAbility->WasCancelled() == true`.
**Pass criteria:** Final BT log line for the task = `Failed`. `OnAbilityEnded` fired with cancelled flag set.

### TC2.4 — Ability on cooldown
**Goal:** Cooldown blocks activation; default flag => Failed; flag => Succeeded.
**Setup:** Author task with `GA_Test_Cooldown`. ASC pre-loaded with cooldown effect (apply once, leave 2s remaining).
**Variant A (`succeed_on_blocked=false`):** Run BT — task runs ExecuteTask once.
**Variant B (`succeed_on_blocked=true`):** Same, with flag.
**Expected:**
- Variant A: `TryActivateAbilityByClass` returns false; task `Failed` immediately; no end-delegate subscribed (verify weak-ptr count).
- Variant B: Same activation result but task `Succeeded`.
**Pass criteria:** Variant A reports Failed; Variant B reports Succeeded; in BOTH variants `OnAbilityActivated` count == 0; `OnAbilityEnded` count == 0; `LogMonolithAI Warning` line emitted explaining "activation blocked: cooldown".

### TC2.5 — Ability requirements not met (missing ASC)
**Goal:** Pawn without ASC -> task fails immediately, no crash.
**Setup:** Author task on `BT_AbilityTest_Empty`. Spawn `BP_TestAIPawn_NoASC` running it.
**Variant A (`succeed_on_blocked=false`):** task Failed.
**Variant B (`succeed_on_blocked=true`):** task Succeeded.
**Steps:**
1. Spawn no-ASC pawn.
2. Run BT.
3. Read result.
**Expected:** No crash. No null deref in `ResolveASC`. Log message identifies the ASC-resolution failure.
**Pass criteria:** Variant A `Failed`, Variant B `Succeeded`. Log line `LogMonolithAI Warning: BTTask_TryActivateAbility: no ASC on pawn 'BP_TestAIPawn_NoASC_C_0'`. Zero crashes; zero ensures.

### TC2.6 — Ability blocked by tag query
**Goal:** Block-by-tag is treated identically to cooldown (activation false).
**Setup:** Author task with `GA_Test_BlockedByTag`. Pre-grant `Ability.Blocked` tag to the ASC's loose tags.
**Steps:**
1. Run BT.
2. Read result.
**Expected:** Activation returns false. Task `Failed` (default).
**Pass criteria:** As TC2.4 Variant A. Distinguishable log message: "activation blocked: tag query".

### TC2.7 — Ability cancelled mid-execution by external system
**Goal:** External `CancelAbility` while BT is in `InProgress` cleans up correctly.
**Setup:** Author task with `GA_Test_Duration5s`, `wait_for_end=true`.
**Steps:**
1. Run BT — ability activates, task is InProgress.
2. Wait 1.0s.
3. Externally call `ASC->CancelAbility(theAbilityInstance)`.
4. Read task result within 2 frames.
5. Verify delegate cleanup: `ASC->OnAbilityEnded` no longer holds the BTTask handle.
**Expected:** Task transitions InProgress -> Failed (with `succeed_on_blocked=false`). Delegate handle was reset (weak-ptr null after cleanup).
**Pass criteria:** Task `Failed`. `EndedHandle.IsValid() == false` post-resolution. No double-fire (multicast safe).

### TC2.8 — BT branch abort cancels in-flight ability
**Goal:** When BT aborts the task, `AbortTask` calls `CancelAbility` to avoid leak.
**Setup:** BT_Sequence with two children: `[TryActivateAbility(GA_Test_Duration5s), Wait(0.1s)]`. Use a Decorator on the Sequence root that becomes false at 1.0s, forcing abort.
**Steps:**
1. Run BT — first task starts (InProgress, ability activated).
2. Wait 1.0s — decorator flips, parent aborts, AbortTask runs.
3. Verify `ASC->GetCurrentMontageAbility()` or equivalent — the ability instance should be cancelled.
4. Verify delegate handle reset.
**Expected:** Task returns `Aborted`. Ability instance cancelled (verify via `CancelAbility` was called - mock tap or LogVerbose). No leftover end-delegate subscription.
**Pass criteria:** `AbortTask` returned `Aborted`. `OnAbilityEnded` fires once with cancelled=true. `EndedHandle` reset to invalid. No log warnings about leaked delegates after GC.

### TC2.9 — `ability_tags` activation path
**Goal:** Tag-based activation chooses one matching ability and tracks it correctly.
**Setup:** Author task with `ability_tags=["Ability.Combat.Melee"]`. Pawn has both `GA_Test_MeleeLight` and `GA_Test_MeleeHeavy` granted (both tagged Melee). (F16: parent `Ability.Combat.Melee` tag is registered and matches both child abilities — query semantics unchanged from prior Punch/Kick framing.)
**Steps:**
1. Run BT, `wait_for_end=true`.
2. Verify exactly ONE ability fires (via `OnAbilityActivated` count == 1).
3. Wait for ability end. Read task result.
**Expected:** Exactly one ability activated (UE picks first by spec order or server priority). Task awaits its end and reports Succeeded.
**Pass criteria:** Activation count == 1. `bIsOurs` filter matches (the activated ability's `AbilityTags.HasAll(AbilityTags)` is true). Task `Succeeded`.

### TC2.10 — `ability_class` vs `ability_tags` in API surface
**Goal:** Mutually exclusive params; design-time validation rejects conflicts.
**Steps:**
1. Author task with BOTH `ability_class` and `ability_tags` set.
2. Author task with NEITHER set.
3. Author task with `ability_class` only — succeeds.
4. Author task with `ability_tags` only — succeeds.
**Expected:** Steps 1-2 return `ok=false` with explanatory error. Steps 3-4 return `ok=true` with full response.
**Pass criteria:** Failure-mode rows (below) match. Success cases produce a node_id GUID, node_class == `BTTask_TryActivateAbility`.

### TC2.11 — Tag-based filter ambiguity (open question 3 in H2 plan)
**Goal:** When two abilities share the activation tag and one ends BEFORE the activated one, BTTask must not finish on the wrong end.
**Setup:** Author task with `ability_tags=["Ability.Combat.Melee"]`, `wait_for_end=true`. Grant `GA_Test_MeleeLight` + `GA_Test_MeleeHeavy`. Activate `GA_Test_MeleeLight` externally on a different control flow (e.g., player input) BEFORE BT activates anything. (F16: uses existing Melee.Light/Heavy registry tags; Punch/Kick deliberately not in tree per survival-horror curation.)
**Steps:**
1. Externally activate `GA_Test_MeleeLight` (5s duration variant); confirm running.
2. Start BT — task activates an ability via tag-match (might be `GA_Test_MeleeHeavy` or another `GA_Test_MeleeLight` instance).
3. End the externally-activated `GA_Test_MeleeLight` instance after 1s.
4. Verify BT task does NOT finish at this point.
5. Wait for BT-activated ability's actual end.
6. Verify BT task finishes now.
**Expected:** BT task only resolves on its OWN activated ability's end. The externally-activated `GA_Test_MeleeLight` end does not trigger `FinishLatentTask`.
**Pass criteria:** `dt` from BT-activation to task-finish matches the BT-activated ability's duration, NOT the external one's. Implementation must use `FGameplayAbilitySpecHandle` capture (per H2 plan open question 3) to disambiguate. If implementation falls back to "first matching tag", document the ambiguity in test notes and FAIL the TC pending fix.

### TC2.12 — `event_tag` fires gameplay event on activation
**Goal:** `event_tag` parameter sends `SendGameplayEventToActor` after successful activation.
**Setup:** Author task with `ability_class=GA_Test_Instant, event_tag=Event.Test.Ping`.
**Steps:**
1. Hook `ASC->GenericGameplayEventCallbacks.FindOrAdd(Event.Test.Ping)` and count fires.
2. Run BT.
3. Read event count.
**Expected:** Event count == 1, fired AFTER `OnAbilityActivated` (verify ordering via timestamps).
**Pass criteria:** Event count == 1. Event payload includes `Instigator == ASC->GetOwner()` and `EventTag == Event.Test.Ping`. If activation FAILS, event count == 0 (no event sent on failure).

### TC2.13 — Multi-instance BT memory isolation
**Goal:** Three pawns running the same BT in parallel each have independent FBTTaskMemory.
**Setup:** Author task with `GA_Test_Duration1s`. Spawn three pawns running the BT simultaneously.
**Steps:**
1. Stagger BT starts by 100ms each (pawn A at T+0, B at T+0.1, C at T+0.2).
2. Cancel pawn A's ability externally at T+0.5.
3. Verify pawn B and C continue normally.
4. Read each pawn's task result.
**Expected:** A reports Failed (cancelled). B and C report Succeeded (~T+1.1 and T+1.2). No cross-pawn state corruption.
**Pass criteria:** Three independent results. `EndedHandle` per memory block does not collide. `GetInstanceMemorySize()` returned correct value (verify via test inspecting `GetInstanceMemorySize() == sizeof(FBTTaskMemory)`).

### TC2.14 — ABP / ability recompile survival
**Goal:** Recompiling the GA Blueprint while BT exists doesn't crash node instances.
**Setup:** Author task with a Blueprint ability `/Game/Tests/Monolith/Abilities/GA_Test_BP_Instant`. Save BT.
**Steps:**
1. Spawn pawn, run BT once - verify pass.
2. Open `GA_Test_BP_Instant`, modify a tag, recompile.
3. Spawn fresh pawn, run BT again.
**Expected:** Second run still passes. `TSubclassOf<UGameplayAbility>` re-resolved against the recompiled CDO. No crash on the BT graph re-link.
**Pass criteria:** Both runs succeed. No `Ensure` or crash log between recompile and run #2.

### TC2.15 — Round-trip via `export_bt_spec` / `import_bt_spec`
**Goal:** Authored task survives spec-export/import.
**Steps:**
1. Author task on `BT_AbilityTest_Empty`.
2. `ai::export_bt_spec(asset_path=...)` -> capture spec.
3. Author identical task on a fresh BT `BT_AbilityTest_RoundTrip` (or import to it via `import_bt_spec`).
4. Compare specs field-by-field.
**Expected:** Specs equivalent (excluding GUIDs which differ). All UPROPERTYs round-trip: `AbilityClass`, `AbilityTags`, `bWaitForEnd`, `bSucceedOnBlocked`, `EventTagOnActivate`, NodeName.
**Pass criteria:** Field equality (modulo node_id). No data loss.

---

## Per-Action Sanity

### TC2.16 — `add_bt_use_ability_task` response shape (by class)
**Goal:** Response JSON matches the convention from `add_bt_run_eqs_task`.
**Steps:** Call action with TC2.1 inputs.
**Expected response shape (omit-when-empty per F14, matches F5/F6 ADR pattern — null/empty fields are OMITTED rather than echoed):**
```jsonc
{
  "ok": true,
  "asset_path": "/Game/Tests/Monolith/AI/BT_AbilityTest_Empty",
  "node_id": "<GUID>",
  "node_class": "BTTask_TryActivateAbility",
  "ability_class": "/Game/Tests/Monolith/Abilities/GA_Test_Instant",
  // ability_tags  — OMITTED when not set (mutually exclusive with ability_class)
  "wait_for_end": true,
  "succeed_on_blocked": false,
  // event_tag     — OMITTED when not set / invalid (warn-and-proceed semantic)
  // node_name     — OMITTED when caller did not supply a non-empty override
  "message": "TryActivateAbility task added and configured"
}
```
**Pass criteria:** Required keys present (`ok`, `asset_path`, `node_id`, `node_class`, `wait_for_end`, `succeed_on_blocked`, `message`). Exactly one of `ability_class` / `ability_tags` present (mutually exclusive). `event_tag` and `node_name` are OPTIONAL — present only when caller supplied non-empty input. `node_id` is a parseable GUID. `node_class` exact match. Consumers should use `.get(field, default)` rather than asserting strict presence.

### TC2.17 — `add_bt_use_ability_task` response shape (by tags)
**Goal:** Tag-based call returns `ability_tags=[...]` with `ability_class` OMITTED.
**Steps:** Call with `ability_tags=["Ability.Combat.Melee.Light","Ability.Combat.Melee"]`.
**Expected:** Same omit-when-empty shape as TC2.16. `ability_tags` is an array of two strings; `ability_class` is OMITTED (mutually exclusive). `event_tag` / `node_name` OMITTED unless supplied. Per F14, the omit-when-empty pattern is canonical (matches F5/F6 ADR — relax the spec to match the impl rather than expand the impl).
**Pass criteria:** Field types match contract. Exactly one of `ability_class` / `ability_tags` present (here: tags). No strict-presence assertion on optional fields.

### TC2.18 — Parent_id wiring
**Goal:** Adding under a Sequence puts the node as that Sequence's child, not root's.
**Steps:**
1. On `BT_AbilityTest_Sequence`, capture the Sequence node's GUID. Three options:
   - **Preferred:** `ai_query("get_bt_graph", {asset_path: "/Game/Tests/Monolith/AI/BT_AbilityTest_Sequence"})` (Phase J F8) — returns a flat node array; find the row with `node_class="BTComposite_Sequence"` and read its `node_id`.
   - **Equivalent:** the GUID returned by the `add_bt_node` call in §Setup #5 (creating the Sequence) — store and reuse if you have it.
   - **Fallback:** `ai_query("export_bt_spec", {asset_path: "/Game/Tests/Monolith/AI/BT_AbilityTest_Sequence"})` and parse the returned spec for the Sequence node's `node_id`.
2. Author task with `parent_id=<sequence-guid>`.
3. Read graph (via `export_bt_spec` again) — task is child of Sequence, not root.
**Pass criteria:** Verified via reading the graph topology (export_bt_spec) after authoring.

---

## Failure Modes

| Case | Input | Expected Error |
|------|-------|----------------|
| Both `ability_class` and `ability_tags` set | both populated | `ok=false, error="Specify exactly one of ability_class or ability_tags, not both"` |
| Neither set | both null/empty | `ok=false, error="Must specify either ability_class or ability_tags"` |
| `ability_class` path doesn't exist | `/Game/Bogus/GA_None` | `ok=false, error="Ability class asset not found: /Game/Bogus/GA_None"` |
| `ability_class` path exists but isn't `UGameplayAbility` subclass | path to `/Game/Tests/Monolith/UIBinding/AS_TestVitals` (the AttributeSet authored in J1 §Setup; verified to exist 2026-04-26) | `ok=false, error="Asset at /Game/Tests/Monolith/UIBinding/AS_TestVitals is not a UGameplayAbility subclass (got AttributeSet)"` |
| Native class name not found | `ability_class="UNotARealAbility"` | `ok=false, error="Native class 'UNotARealAbility' not found"` |
| `ability_tags` empty array | `ability_tags=[]` | `ok=false, error="ability_tags must contain at least one tag"` |
| `ability_tags` all invalid | `ability_tags=["Bogus.Tag"]` (not registered) | `ok=false, error="No valid GameplayTags in ability_tags. Unknown: [Bogus.Tag]"` (if strict) OR `ok=true, warnings=["Unknown tag: Bogus.Tag"]` (if permissive — H2 plan defaults permissive via RequestGameplayTag bErrorIfNotFound=false) |
| `event_tag` invalid | `event_tag="Bogus.Tag"` | `ok=true, warnings=["event_tag 'Bogus.Tag' is not a registered GameplayTag; runtime will skip event send"]` (warn-and-proceed; runtime is no-op as per failure matrix) |
| `asset_path` not a Behavior Tree | path to a regular BP | `ok=false, error="Asset at /Game/.../BP_Foo is not a BehaviorTree (got Blueprint)"` |
| `asset_path` doesn't exist | `/Game/Bogus/BT_None` | `ok=false, error="Behavior Tree asset not found: /Game/Bogus/BT_None"` |
| `parent_id` not a valid GUID in this BT | `parent_id="abc"` (not a GUID) | `ok=false, error="parent_id 'abc' is not a valid GUID"` |
| `parent_id` valid GUID but not a node in this BT | random valid GUID | `ok=false, error="No node with GUID '...' in BT 'BT_X'"` |
| `parent_id` is a non-composite (e.g., a Task itself) | leaf GUID | `ok=false, error="parent node 'Task_X' is not a composite (cannot have children)"` |
| Ability not granted on ASC at runtime | `TryActivate*` returns false | `LogMonolithAI Warning: TryActivate failed for class GA_X on ASC ASC_Y` — not an action error, runtime path |

### Failure Modes — Phase F1 BT crash hardening (added 2026-04-26)

These guard the Tier-1 `ValidateParentForChildTask` invariant introduced for the J2 BT-ability-task crash. Each row exercises a scenario that previously produced an editor access violation (`BehaviorTreeGraph.cpp:517`) and now must return a clean error. **`J2-AB-Crash-01` is the smoke-blocker that would have caught the original ship.**

| Row | Action | Pre-state | Params | Expected | Why |
|------|--------|-----------|--------|----------|-----|
| J2-AB-Crash-01 | `add_bt_use_ability_task` | BT exists, graph has only auto-generated Root | `asset_path=BT_Empty`, `ability_class=GA_X`, `parent_id` omitted | `ok=false, error="add_bt_use_ability_task: Cannot add task as direct child of root: BT root has no composite. Add a composite node first via add_bt_node(class=BTComposite_Selector) then re-target this action with parent_id=<composite_guid>."` — NO crash | The exact crash reproduced 2026-04-26 |
| J2-AB-Crash-02 | `add_bt_use_ability_task` | BT exists, Root + Selector wired | `asset_path=BT_OK`, `ability_class=GA_X`, `parent_id=<Selector.guid>` | Success, node attached | Sanity / happy path |
| J2-AB-Crash-03 | `add_bt_use_ability_task` | BT exists, Root + Selector wired | `asset_path=BT_OK`, `ability_class=GA_X`, `parent_id=<Root.guid>` (explicit Root) | `ok=false, error="add_bt_use_ability_task: Cannot add task as direct child of root..."` | Defends against caller passing Root explicitly |
| J2-AB-Crash-04 | `add_bt_use_ability_task` | BT exists, Root + Selector + child Task wired | `asset_path=BT_OK`, `ability_class=GA_X`, `parent_id=<existing-task.guid>` | `ok=false, error="add_bt_use_ability_task: parent node '<guid>' is a BTTask_<X>; tasks may only be attached to composites (Selector/Sequence/Parallel/SimpleParallel)."` | Defends sibling-of-task case |
| J2-Sibling-Crash-05 | `add_bt_node` (with `node_class=BTTask_Wait`) | empty BT (Root only) | `parent_id` omitted | `ok=false, error="add_bt_node: Cannot add task as direct child of root..."` | Same hazard, sibling site `:2152` |
| J2-Sibling-Crash-06 | `add_bt_run_eqs_task` | empty BT (Root only) | `parent_id` omitted, valid `eqs_path`+`bb_result_key` | `ok=false, error="add_bt_run_eqs_task: Cannot add task as direct child of root..."` | Same hazard, sibling site `:2845` |
| J2-Sibling-Crash-07 | `add_bt_smart_object_task` | empty BT (Root only) | `parent_id` omitted | `ok=false, error="add_bt_smart_object_task: Cannot add task as direct child of root..."` | Same hazard, sibling site `:2984` |
| J2-Sibling-Crash-08 | `build_behavior_tree_from_spec` | brand-new path | `spec.root.type="BTTask_Wait"` (a task at root) | `ok=false, error="build_behavior_tree_from_spec: BT root must be a Composite node (Selector/Sequence/Parallel/SimpleParallel), got Task 'BTTask_Wait'. Wrap your task in a composite."` | Tightens the existing pre-validator at `:3299` |
| J2-AB-OK-09 | `add_bt_use_ability_task` after `add_bt_node(BTComposite_Selector)` against the empty BT | empty BT | two-step recipe: 1) `add_bt_node(class=BTComposite_Selector, parent_id="")` 2) `add_bt_use_ability_task(parent_id=<Selector.guid>, ability_class=GA_X)` | Step 1 success; Step 2 success | Documents the supported recovery path |

**Recommendation:** wire `J2-AB-Crash-01` as a smoke-blocker in the test runner — 1-second test, blocks any merge that regresses the crash hardening.

**Runtime-only failure rows** (covered by TC matrix above; restated for completeness):
| Runtime case | Default flag | With `bSucceedOnBlocked=true` |
|---|---|---|
| ASC missing | `Failed` | `Succeeded` |
| Ability not granted | `Failed` | `Succeeded` |
| On cooldown | `Failed` | `Succeeded` |
| Tag-blocked | `Failed` | `Succeeded` |
| Cancelled mid-run | `Failed` | `Succeeded` |
| Aborted by BT branch | `Aborted` (not toggleable) | `Aborted` (not toggleable) |

---

## Notes

### Open questions for I2 implementation team
1. **`UGameplayAbility::WasCancelled()` availability (H2 open question 1)** — TC2.3 / TC2.7 rely on it. Implementation must verify via `source_query`. If absent in 5.7, switch to `OnAbilityEndedWithData` and read `bWasCancelled` from `FAbilityEndedData`. Test author for J2 should assume `WasCancelled()` works; if PR review flags switch, test code shifts to use the data variant.
2. **TC2.11 spec-handle filtering (H2 open question 3)** — currently drafted as a test that will FAIL if implementation uses tag-only filtering. Confirm with I2 implementer whether spec-handle capture lands; if not, mark TC2.11 as "expected fail, deferred fix" until follow-up.
3. **Native class name resolution (H2 open question 2)** — TC2.16 covers the asset-path path; add TC2.19 if native class strings are accepted: author task with `ability_class="UGA_DefaultMeleeAttack"` (native). Validate via `FindFirstObject<UClass>`.
4. **TC2.5 missing-ASC log message** — implementation should emit a clear, controller-name-tagged log line. Verify it's `LogMonolithAI` not `LogTemp`.
5. **TC2.9 first-match determinism** — UE's `TryActivateAbilitiesByTag` order is "first matching ability spec by handle order". This is not a stable contract across spec re-grants. Test currently asserts "exactly ONE fires"; do NOT assert which one without spec-order control.

### Dependencies on I-phase landing
- I2 must ship before any TC runs.
- All test abilities (`GA_Test_*`) must be authored as part of test setup, not in I2's deliverables.
- `BT_AbilityTest_Empty` and `_Sequence` similarly are test fixtures.
- TC2.14 requires editor runtime BP recompilation; needs interactive editor session.

### Coverage map (per H2 plan §Test Plan)
| H2 plan test | This spec maps to | Notes |
|---|---|---|
| pytest #1 by_class | TC2.16 + TC2.1 | Author + run |
| pytest #2 by_tags | TC2.17 + TC2.9 | Author + run |
| pytest #3 validation_neither | Failure-mode row | Direct map |
| pytest #4 validation_both | Failure-mode row | Direct map |
| pytest #5 invalid_class | Failure-mode row | Direct map |
| pytest #6 non_ability_class | Failure-mode row | Direct map |
| pytest #7 with_event_tag | TC2.12 | Direct map |
| pytest #8 round_trip | TC2.15 | Direct map |
| C++ #9 Class | TC2.1 | Direct map |
| C++ #10 WaitForEnd | TC2.2 | Direct map |
| C++ #11 NoASC | TC2.5 | Direct map |
| C++ #12 Cooldown | TC2.4 | Direct map |
| C++ #13 AbortMidExecution | TC2.8 | Direct map |
| C++ #14 ABP_Recompile | TC2.14 | Direct map |
| C++ #15 MultiInstance | TC2.13 | Direct map |

Total this spec: **18 test cases + 13 failure-mode rows + 6 runtime-failure rows**.
