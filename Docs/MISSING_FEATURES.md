# Monolith — Missing Features / MCP Action Gaps

**Purpose:** Running inbox of empirically-discovered Monolith MCP capability gaps — actions that are missing, broken, or that forced a manual in-editor workaround during real project work. Each entry proposes the action to add. Distinct from `ROADMAP.md` (milestones / ship-status); this is the raw gap log, fed from the field.

**Internal planning doc.** Lives in `Docs/` — if it should NOT ship in the public release zip, add it to the `make_release.ps1` strip list alongside `ROADMAP.md`.

---

## 2026-09-10 — OPEN: carried into 0.23.0

Four items known-open at the 0.23.0 release. None of them blocks it; each is stated here so nobody rediscovers them.

1. **PR #123 (index memory-pressure hardening) is deferred, not rejected.** It overlaps the editor-exit deadlock fix that shipped in 0.23.0 — both rework the same indexing-worker wait/dispatch paths in the same file, and the deadlock fix changed the shape those waits have (bounded polling with a provably-stopped abandon condition, plus an abort token on game-thread dispatches). Landing #123 on top of that as a merge would produce a file neither change was designed against.
   - **Add:** a combined design pass over the worker's wait, dispatch and memory-pressure behaviour, then reimplement #123's intent against the post-0.23.0 shape.

2. **PR #104 (updater release-asset selection) is still open, pending an end-to-end Windows install rehearsal.** The auto-update path is client-side and time-shifted — it only runs when a NEWER release exists, and the code that runs is the *old installed version's*, not HEAD. "It compiles and the dialog appears" is not the path; v0.14.7 shipped an install path that could never once succeed on Windows and no release in fifteen cycles noticed (issues #90/#94).
   - **Add:** point a client at a real release and drive the full Install click through hash verification to staged swap, on Windows, before merging. This is the standing rule for any change to the update path, not a one-off for this PR.

3. **Issue #142 — shipped binaries are unsigned.** Windows Smart App Control blocks them when it is in enforcement mode. This is not a code defect and cannot be fixed in the source tree: it needs a code-signing certificate and a signing step in `make_release.ps1`.
   - **Add:** acquire a code-signing certificate, then sign the per-engine DLLs and `monolith_query.exe` / `monolith_proxy.exe` as part of the release pipeline.

4. **One un-migrated UE 5.8 sampler call site.** `MonolithMaterial/Private/MonolithMaterialActions.cpp` still calls the deprecated `UMaterialExpressionTextureBase::GetSamplerTypeForTexture` directly rather than `MonolithMaterialSamplerCompat::GetSamplerTypeForTexture`, so it still emits C4996 on 5.8. Missed by the 0.23.0 deprecation sweep because the shim was introduced for the `MonolithIndex` call site.
   - **Add:** route it through the shim. One line, plus the `MonolithMaterialSamplerCompat.h` include.

---

## 2026-08-01 — OPEN: Blueprint pin-type and component-resolver follow-ups

Raised by the code review of the #115 / #116 / #102-part-1 work and deliberately not folded into it — none of them block those fixes, and each is a behaviour change that deserves its own verification rather than riding along.

1. **Pin-type strictness is asymmetric across the variable actions.** `add_variable` now routes through `TryParsePinType` and errors on an unresolvable type token, but `set_variable_type`, `add_local_variable` and `add_replicated_variable` still use the best-effort legacy parser, so `enum:Bogus` there still yields a silent plain byte. A *valid* `enum:` resolves correctly through either path, so the documented `change_variable_type` recovery for #115 still works — this is about the failure mode, not the success one.
   - **Add:** migrate the remaining three call sites to `TryParsePinType` so every variable-creation surface fails loudly on an unresolvable type.

2. **A failed component write can leave the asset dirty.** `set_component_property` resolves with write intent (creating the Inheritable Component Handler override) *before* validating that the property exists, so a bogus `property_name` on an inherited-SCS component creates an ICH record and dirties the package, then returns an error without compiling. It is self-healing — the record is archetype-identical and gets pruned on the next compile — but the user sees an unexplained dirty flag from a call that failed. `set_mesh` in the motion-matching scaffold has the same shape.
   - **Add:** resolve read-only, validate, then re-resolve with write intent.

3. **The `Root` alias drops the class constraint.** Every resolver tier honours `RequiredClass` except the `Root` alias branch, which returns the root component without re-checking `IsA`. Unreachable from current callers (it needs `RequiredClass` to be a strict subclass of `USceneComponent`), so this is latent rather than live.
   - **Add:** apply the same `IsA(FilterClass)` check the other tiers use.

4. **The `asset_path` alias is one action wide.** It was added to `get_inherited_component_override` only; the other seven `bp_path` actions in the same file still accept `bp_path` alone. The inconsistency is the kind of thing that generates the next issue.
   - **Add:** accept `asset_path` as an alias across that action family.

---

## 2026-08-01 — OPEN: risk-mining follow-ups (after the #119 root-discovery fix)

Scoped out while replacing the hardcoded git roots with runtime discovery (`ResolveGitRepoRoots`, the churn path rebase, the config fingerprint, and `risk get_mining_status`). None of these block the fix; all four were deliberate omissions.

1. **Repository discovery is one level deep under `Plugins/`.** A repository nested any deeper — `Plugins/<Group>/<Plugin>`, or one under `Source/` or `Content/` — is invisible to auto-resolution and has to be named explicitly in `GitRepoRoots`. A recursive walk was rejected for this pass: an unbounded `.git` search over a large project is a cost nobody asked for at bootstrap time.
   - **Add:** an opt-in depth setting, or a `Plugins/*/*` second level, for grouped-plugin layouts.

2. **No unit coverage for the resolver or the path rebase.** `FGitCoChangeIndexer::ComputeChurnPathRebase`'s four cases (repository is the project, under it, an ancestor of it, unrelated) and `ResolveGitRepoRoots`'s override-versus-auto branch are exercised only by the in-editor V8 checks. Both are pure functions over strings and are cheap to test directly.
   - **Add:** automation tests in `Private/Tests/RiskQueryTests.cpp` covering all four rebase cases and the ancestor-probe walk, plus a fingerprint test asserting the writer's and the reader's inputs produce the same `int32`.

3. **`repo_tag` is still a basename.** `RepoTagFor` takes the last path segment, so a project-root repository and a nested plugin repository that happen to share a folder name produce the same tag. Harmless to the data today — the rebase makes their file sets disjoint and the primary key is `(repo_tag, file_path)` — but the tag is user-visible as `get_file_churn`'s `repo_tag` filter, where it is ambiguous.
   - **Add:** disambiguate the tag by project-relative offset when two roots collide.

4. **The noise filter cannot express "this directory only".** Matching is case-insensitive substring containment, so `Docs/plans/` also suppresses `Plugins/X/Docs/plans/`. That is usually what is wanted, but neither behaviour is expressible. Glob or anchored-prefix support was left alone rather than changing existing users' filter semantics inside a fix.
   - **Add:** an anchored or glob mode for `GitMiningNoiseFilter`, opt-in so current substring behaviour is preserved.

---

## 2026-08-01 — OPEN: `project search` follow-ups (after the #113 error-classification work)

Scoped out while classifying search errors and validating `project search` input. Neither blocks that work.

1. **Offline `project.*` parity is ungated.** `Scripts/verify_offline_parity.py` asserts live-vs-offline byte-identical output for `cppreflect`, `network`, `decision`, `risk` and `source` — it has **no `project` cases**. Verified in the script's own case list. So the three `project search` implementations (live `MonolithIndex`, `Tools/MonolithQuery/monolith_query.cpp`, `Scripts/monolith_offline.py`) are kept in step only by hand, and `make_release.ps1`'s parity gate cannot catch a drift between them. A drift here produces a wrong result *shape* from the offline tools, not a crash. Until it is closed, treat any edit to one of the three as an edit to all three.
   - **Still open as of 0.23.0, and the surface grew.** The `asset_class` filter added in 0.23.0 was mirrored into all three implementations by hand, including its validation rules (`EJson` type check, 32-entry cap, whitespace rejection) and its `--asset-class` CLI spelling. None of that is gated, so the three can now drift on filter semantics as well as on result shape.
   - **Add:** `project` cases to the parity harness. They need a project index present on the machine running the gate — the reason they were skipped originally.

2. **`project search` cannot answer a query whose column filters span both FTS tables.** `asset_name:BP_Enemy` and `node_name:Branch` each work, because the table that does not carry the column is skipped as not-applicable. But `asset_name:X OR node_name:Y` names columns from both tables, no single table can satisfy it, and it is refused as an invalid query. Serving it would require projecting the query separately onto each table's column set. PR #113 proposed exactly that as a 1449-line hand-written FTS5 grammar parser; that parser was **rejected** — fuzzing showed its AST destructor recursed without a depth guard, killing the editor process on a query of roughly 12,000 chained terms, and a 15,935-query differential against real SQLite found 62 false accepts and 315 false rejects.
   - **Add:** per-table query projection that does not reintroduce a hand-rolled grammar — the query stays bound into `MATCH ?`, with SQLite's own bounded parser as the authority.

---

## 2026-08-01 — OPEN: generic graph-node property read/write (PR #102 part 2, deferred)

PR #102 arrived in two halves. Part 1 (the Inheritable Component Handler write tier) shipped, folded into the unified component resolver that also fixes #116 — see `specs/SPEC_MonolithBlueprint.md` § Component resolver. **Part 2 — `set_graph_node_property` / `get_graph_node_properties` — was deferred pending a redesign, not rejected.** The need is real: there is no general way to read or write a `UEdGraphNode`'s own UPROPERTYs, and `ReconstructNode()` after such a write is the genuinely new insight in the submission. Keep that part.

**Why the submitted shape cannot ship as-is.** It resolves an arbitrary `UObject` from a free-floating object path and then writes any `FProperty` on it, with no `CPF_Edit` filter and with `save` defaulting to **true**. A mistyped path therefore modifies and saves an engine or marketplace asset, and the action bypasses every allowlist the namespace's other write actions go through.

**The safe shape to build.**
- Anchor on `asset_path` (a Blueprint the caller names) and locate the node *within* it — `graph_name` + `node_id` / `node_guid`, the same addressing `blueprint`'s other node actions already use. Never accept a bare object path.
- Restrict writes to `CPF_Edit` properties on `UEdGraphNode` subclasses, and refuse properties the node exposes only as pins (those belong to `set_pin_default`).
- Default `save` to **false**, matching the rest of the namespace.
- Call `ReconstructNode()` after a write that changes node topology, then report the resulting pin set so the caller can see what the reconstruct did.
- Read side (`get_graph_node_properties`) is uncontroversial and can land first.

---

## 2026-08-01 — OPEN: server-lifecycle and indexer follow-ups (after the #114 defect fixes)

Scoped out while taking the genuine fixes from #114 (the listener teardown, the four use-after-free dispatch sites, the live-callback re-arm, honest reindex results, the indexer failure exits, and the non-blocking port probe). The durable-activation feature the PR was built around was declined, and none of the items below block the fixes that were taken.

1. **`Start()`'s success check is weakened on the stop-then-start path.** Because `Stop()` no longer stops the shared HTTP listener, the port stays bound for the lifetime of the editor — so `ProbePort` succeeds after a `Monolith.StopServer` regardless of whether routes were rebound. The routes *are* rebound and the behaviour is correct, but the "listening on port %d" log line is no longer evidence that the bind is fresh. Nothing observable is wrong today; the log line simply proves less than it used to.
   - **Add:** a route-level readiness check (a loopback `GET /health` on the rebind path) so the success line reflects Monolith's own availability rather than the port's.

2. **No automation coverage for the four game-thread dispatch sites.** The use-after-free fix is verified only by closing the editor during a full index and observing no crash. The invariant it depends on — no fire-and-forget `AsyncTask` inside `FIndexingTask::Run` captures `this` — is a source property that a future edit can silently break, and the crash it produces is timing-dependent and rare enough to survive several releases unnoticed (this one did).
   - **Add:** a source-level guard, either a build-time grep in the test target or an automation test that constructs and destroys a task with a queued progress callback outstanding.

3. **A failed worker-thread creation is untested on both indexers.** `MonolithSourceIndexerFailureTest.cpp` covers the database-open exit, which is reachable with a fixture. The `FRunnableThread::Create` failure exit — for the source indexer and the project indexer alike — is not, because there is no supported way to make thread creation fail from a test.
   - **Add:** a seam (an overridable thread factory, or a test-only failure switch) so both latch-the-flag exits are covered rather than only the one that happens to be reachable.

4. **`bIsIndexing` is still the only concurrency gate on the index subsystems.** The re-arm and honest-result fixes both work by keeping that single flag truthful, which is adequate for the current single-run model but says nothing about *which* run is in flight. A caller that starts a reindex and then wants to know whether its own request is the one running has no way to ask.
   - **Add:** a run token on the start result and on `get_index_status`, so a caller can correlate a completion with the request that caused it.

---

## 2026-08-01 — OPEN: full-index resume follow-ups

Scoped out while making an interrupted full index resumable (#117). The resume covers the metadata pass and the deep pass — between them the overwhelming majority of index time, and where crashes actually happen, since that is where assets are loaded. The post-pass sentinels were deliberately left out of it.

1. **The post-pass sentinels have no checkpoints, and they append rather than replace.** `dependencies`, `actors`, `datatable_rows`, `configs`, `cpp_symbols`, `tags`/`tag_references` and the animation nodes are all written by sentinel indexers that enumerate the Asset Registry themselves rather than working from `assets` rows with ids, so `deep_indexed_hash` / `deep_index_attempts` cannot apply to them. A resume repeats the whole post-pass phase, and because those indexers insert without clearing, an interruption *during* that phase (rather than during the far longer deep pass) leaves duplicate rows behind. `monolith_reindex force=true` clears them, and the duplicates degrade `search` / `find_references` results rather than corrupting the index — but the user is not told. Per-indexer scoping was rejected for this pass: PR #120's approach was a name-to-SQL ladder in the subsystem, which puts each indexer's storage knowledge in the wrong layer and was already incomplete for several indexers.
   - **Add:** let `IMonolithIndexer` declare the data it owns (a `ClearOwnedData(DB)` hook, defaulting to a no-op), and have the resume path call it on each sentinel before the post-pass phase — so the knowledge lives with the indexer, and a new indexer cannot silently miss it.

2. **The animation pass has transactions but no resume state.** `FAnimationIndexer` now commits per batch, so an interruption costs one batch instead of the whole pass, but a resume still re-walks every animation asset from the start. Per-asset progress for it needs item 1's ownership hook (its rows are keyed by asset id but it is dispatched through the `__Animations__` sentinel, so the full-index queue never sees them individually).
   - **Add:** checkpointing for the sentinel passes, once indexers can describe their own rows.

3. **Three registered sentinel indexers never run.** `FGASIndexer`, `FMetaSoundIndexer` and `FAIIndexer` are constructed and registered but the full-index post-pass has no branch that dispatches them, so their data is never written. This is a real latent bug, separate from the resume work, and it needs its own verification — activating three indexers that have never executed is not a change to fold into a fix for something else.
   - **Add:** dispatch them from the post-pass phase, with an in-editor verification pass over the data each produces.

4. **Deleted packages are not reaped by a full index.** A full index that resumes (rather than wiping) never removes rows for assets that disappeared while the editor was closed. The live Asset Registry callbacks and the startup delta pass both handle deletions, so this only bites for packages deleted outside the editor between an interrupted run and its resume.
   - **Add:** a reconciliation step on the resume path that drops `assets` rows whose package the Asset Registry no longer reports.

5. **`monolith_reindex()` with no arguments wipes a resumable index.** It picks between incremental and full via `CanDoIncrementalIndex()`, which requires `last_full_index` — a marker an interrupted run does not have — so it takes the full-reset path and discards recoverable progress. The automatic startup path and `Monolith.StartIndex` both resume correctly; only the explicit no-argument reindex does not.
   - **Add:** route `monolith_reindex()` (no `force`) to `ResumeFullIndex()` when an interrupted index is present. Touches `MonolithCoreTools.cpp::HandleReindex`, which reaches the subsystem by reflection, so it also needs `ResumeFullIndex` exposed as a `UFUNCTION`.

---

## 2026-07-30 — OPEN: ABP-native anim layer follow-ups

Deferred while shipping `animation add_anim_layer_graph` (ABP-native animation layer graphs) and the `add_linked_anim_layer` self-layer resolution. The shipped surface covers pose inputs, topology, compile, and pin regeneration; these four were scoped out deliberately.

1. **Non-pose parameter pins on animation layers.** `add_anim_layer_graph`'s `input_poses` takes pose NAMES only. `UAnimGraphNode_LinkedInputPose::Inputs` also accepts typed non-pose parameters (float, bool, struct, object …), which a layer needs to be driven by anything other than an upstream pose. The type-resolution half of this is now done — issue #115 shipped `MonolithPinTypeGrammar` (`MonolithCore/Public/MonolithPinTypeGrammar.h`), a shared type-string → `FEdGraphPinType` grammar whose `TryParsePinType` hard-errors on an unresolvable token instead of silently mistyping the pin, and MonolithAnimation links `BlueprintGraph`, so the action can include it directly. What remains is the parameter list itself.
   - **Add:** an optional typed-parameter list alongside `input_poses`, parsed with `MonolithPinTypeGrammar::TryParsePinType`. No new resolver is needed.

2. **`get_linked_layers` cannot distinguish a self layer from an interface layer.** It emits the node `title` (a self layer reads `"<Layer>\nAnim Layer (self)"`), but no structured field — so a caller has to string-match the title to tell the two apart. An `is_self_layer` / `layer_kind` field was considered during implementation and deliberately not added, to keep the change to the write path.
   - **Add:** a structured `layer_kind` (or `is_self_layer`) field on `get_linked_layers` output.

3. **No post-compile second reconstruct on the consumer node.** Self-layer pose pins resolve against the ABP's `SkeletonGeneratedClass`, so placing a node for a layer that is not yet compiled into that class yields a node with NO pose pins, and nothing re-reconstructs it once a later compile makes the layer resolvable. The documented workaround is to keep `add_anim_layer_graph`'s default `compile: true`, or recompile before placing.
   - **Add:** a post-compile reconstruct pass (or a `reconstruct_linked_layer_node` action) so a node placed early recovers its pins.

4. **PIE evaluation of a native layer is unverified.** This session verified graph topology, schema, compile, and pin regeneration — it did not run a native layer in PIE and confirm it evaluates and contributes pose. Not a known defect; simply untested.
   - **Add:** a PIE smoke over an ABP driving an ABP-native layer.

---

## 2026-06-07 — RESOLVED: Motion Matching authoring pack + PIE/profiling harness (0.18.1)

Field gaps surfaced building an end-to-end Motion Matching setup + autonomous AI locomotion workflow, now addressed (see `SPEC_CORE.md` §12 2026-06-07 notes, `specs/SPEC_MonolithAnimation.md`, `specs/SPEC_MonolithBlueprint.md`, `specs/SPEC_MonolithAI.md`, `specs/SPEC_MonolithEditor.md`):

- **Motion Matching authoring.** Pose Search schema/database primitives, mirror data tables, chooser-table authoring (`create_chooser_table` / `add_chooser_column` w/ `enum_class` / `add_chooser_row` / `set_chooser_cell`), AnimBP graph (`build_motion_matching_node` etc.), and `build_foot_ik_pass`.
- **Thread-safe AnimBP authoring.** `add_property_access_node`, `set_function_thread_safe`, `bind_chooser_database_via_threadsafe`; `scaffold_locomotion_anim_values` now emits a fully-wired thread-safe body and can target a named function graph.
- **Character/actor scaffolding.** `scaffold_motion_matching_character` + the `blueprint` MM scaffolders; inherited native-component CDO-override persistence.
- **Retarget create/run.** `create_ik_rig` / `create_ik_retargeter` / `set_retargeter_rigs` / `batch_retarget_animations` (auto-seeds the retarget op stack so clips are no longer frozen).
- **AI autonomy.** `AMonolithBehaviorTreeAIController` (runs a BT on `OnPossess`) + movement-driving BT task classes; `reorder_bt_children` persistence + Blackboard linking fixes.
- **PIE / profiling harness.** Async PIE-smoke sessions, CSV/Insights profiling brackets, clip + anim-frame capture, `actor_setup`, map authoring, nav rebuild/validate.

**OPEN follow-up:**
- **`build_foot_ik_pass` ground-trace effector drive.** Foot IK is wired and pose-driving (non-Ignore `ModifyBone` mode + Two-Bone IK effector space/bone set), but the effector is NOT yet driven from a ground trace — full ground-contact adaptation is a follow-up. Document honestly; do not claim ground-adaptive foot IK until the trace drive lands.
- **State-machine float/expression transition rules.** `build_state_machine` supports bool-var + automatic rules; float and expression-graph rules are still deferred.

---

## 2026-06-06 — RESOLVED: field-surfaced shortcomings pass (Wave 16)

Field gaps surfaced building an AnimBP / state-machine authoring + test/profiling harness workflow, now addressed (5 new actions + behaviors; see `SPEC_CORE.md` §12 2026-06-06 note, `specs/SPEC_MonolithEditor.md`, `specs/SPEC_MonolithAnimation.md`):

- **Chooser nested-remap + EvaluateChooser setter.** `duplicate_chooser_tree` now does a two-pass duplicate-then-remap (order-independent) recursing nested `FEvaluateChooser` + `FNestedChooser` refs (`ResultsStructs`/`FallbackResult`/`FOutputObjectColumn` → `NestedObjects`) with normalized path matching + per-row `row_remap_report`; new `chooser::set_evaluate_chooser_result_reference` rewrites root/nested EvaluateChooser result rows that `set_result_asset_reference` rejects.
- **State-machine authoring + PIE telemetry.** New `animation::create_state_machine` + `build_state_machine` (declarative; bool-var + automatic rules — float/expression rules deferred-per-element) and `animation::sample_pie_anim_instance` (live PIE AnimInstance class/AnimClass/mode/montage/SM-state/bone+socket).
- **PIE-smoke lifecycle / world-leak hardening.** `run_pie_smoke` + `load_level` world-leak handshake; grouped `log_patterns` + teardown bucketing; session `lifecycle` field; `probe_scripts` + `stages` staged hooks.
- **Clip capture validity / staging / identity + anim preview capture.** `capture_pie_movement_clip` gained a capture-validity heuristic + `view_target_actor`/`view_target` + `stages` + `runtime_identity` + `expected_anim_class` assert; new `editor::capture_anim_frames` renders AnimSequence/BlendSpace/AnimBlueprint preview→PNG with no PIE.
- **Build-error scope.** `get_build_errors` gained `since_marker`/`since_iso`/`since_seconds`/`clear_baseline` + `compile_errors` vs `other_errors` buckets + `exclude_categories`.
- **describe/bulk_fill aliases.** `target_namespace`/`target_action` now accept namespace/action aliases (no new action).
- **GAS-free data-asset authoring** confirmed already covered via existing `blueprint::seed_data_asset` (no new action needed).

---

## 2026-05-23 — AnimBP / Blueprint (first-person framework work)

Surfaced finishing a first-person weapon framework AnimBP. Each gap blocked an action or forced a manual Details-panel workaround. Source refs are UE 5.7 engine paths.

1. **Set/read an AnimBP function `Thread Safe` flag.** No MCP path to SET `bThreadSafe` (`BlueprintThreadSafe` meta) on a user function, and `get_functions` doesn't surface it for READ either — so a thread-safe AnimBP getter can't be authored end-to-end via MCP.
   - **Add:** `blueprint::set_function_thread_safe(asset, function, bool)` (mirror `OnIsThreadSafeFunctionModified`) + expose `thread_safe` in `get_functions` output.
   - **Refs:** `BlueprintDetailsCustomization.cpp:6421-6461`; `UK2Node_FunctionEntry::MetaData.bThreadSafe` → `MD_ThreadSafe`.

2. **Override / implement an interface `BlueprintNativeEvent`.** `override_parent_function` on an already-implemented interface event (e.g. `GetProceduralSourceActors`) throws a duplicate-name compile error.
   - **Add:** `blueprint::implement_interface_event(asset, interface, event)` that binds the inherited UFunction instead of redeclaring.

3. **Read/write `FBoneReference` node-internal properties.** AnimGraph node bone-ref fields (e.g. ProceduralHandIK `HandL/R`, `TargetHandL/R`, `LowerarmL/R`; ProceduralAimOffset `SpineBoneParams`) are not pin-exposed, so `get_node_details` returns nothing for them and there's no setter — forcing manual Details-panel entry for every bone reference.
   - **Add:** read + write support for `FBoneReference` (and arrays of structs containing them, e.g. `TArray<FBoneParams>`) in `get_node_details` / `set_anim_graph_node_property`.

4. **Anim-namespace `add_function` / `add_variable` aliases.** No dedicated anim-namespace creators; must fall back to the `blueprint::` namespace.
   - **Add:** thin anim-namespace aliases for discoverability.

5. **`add_function` rejects an `outputs` param.** Function outputs can't be declared at creation; requires a follow-up `set_function_params`.
   - **Add:** accept `inputs` / `outputs` directly in `add_function`.

6. **Author Instanced polymorphic `TArray` elements (DataAsset presets).** MCP can SIZE a `TArray` of `Instanced` polymorphic UObjects (e.g. `UProceduralPresetData.Presets : TArray<UProceduralPreset*>`) but cannot set each element's CONCRETE CLASS or populate its nested struct fields (`FSwayData`). `seed_data_asset` / `set_cdo_properties` / `bulk_fill_query apply` / `set_cdo_property` treat each element as opaque (garbage fields placed INSIDE an element pass even under `strict:true`); `set_cdo_property` ImportText on the array path forces JSON and rejects ImportText grammar; `describe_query` can't introspect the nested `FStruct` layout (resolves bare types to `UScriptStruct` internals). This forces full MANUAL editor authoring of preset DataAssets (blocked the `DA_Viper_PFPPreset` native-preset rebuild 2026-05-23).
   - **Add:** instanced-element authoring (set element concrete class + nested struct values) in `set_cdo_property`/`seed_data_asset`, + `FStruct` layout introspection in `describe_query`.
