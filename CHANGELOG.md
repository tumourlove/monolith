# Changelog

All notable changes to Monolith will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

### Fixed

- **`blueprint.set_pin_default` now writes `Pin->DefaultObject` for class-typed (`PC_Class`) and object-typed (`PC_Object`) pins** — previously wrote the value string into `Pin->DefaultValue` only, never touching `Pin->DefaultObject`. UE's reflection reads `DefaultObject` for ref-typed pins, so authored class/object values silently reverted to the pin's static base type at compile/load. Fix introduces `MonolithBlueprintInternal::ResolveDefaultObjectForPin` (header-only inline helper) accepting native class names with `A`/`U` prefix retry (PC_Class only), object/class paths via `StaticLoadObject`, and Blueprint class paths with auto `_C`-suffix retry. Type-constraint enforced against `Pin->PinType.PinSubCategoryObject`. Cross-category mismatch (class pin given an instance, object pin given a UClass) returns an error. `set_pin_defaults_bulk` and `batch_execute` inherit the fix automatically (already delegate to `HandleSetPinDefault`). Soft refs (`PC_SoftObject` / `PC_SoftClass`) and `PC_Interface` fall through to the existing primitives path; deferred until concrete demand surfaces. PR [#52](https://github.com/tumourlove/monolith/pull/52), Issue [#53](https://github.com/tumourlove/monolith/issues/53), by **@danielandric**.

## [0.14.8] - 2026-05-02

This release rolls up six work-streams: (1) a new in-tree module **MonolithLevelSequence** (8 actions, dedicated SQLite indexer, UE 5.7 custom-binding awareness) authored as PR [#45](https://github.com/tumourlove/monolith/pull/45) by community contributor @yashabogdanoff; (2) a major **MonolithUI architecture expansion** (Phase A–L) lifting the module from a flat action toolbox to a schema-driven Spec / Type Registry / Style Service / EffectSurface architecture, plus the box-slot primitive completion and a sequence of CommonUI button-conversion fixes; (3) **delegate-node authoring** for `blueprint.add_node` (`ComponentBoundEvent`, `AddDelegate`, `RemoveDelegate`, `ClearDelegate`, `CallDelegate`) shipped as PR [#44](https://github.com/tumourlove/monolith/pull/44) by @danielandric; (4) the **`editor.run_automation_tests` + `list_automation_tests`** action pair shipped as PR [#48](https://github.com/tumourlove/monolith/pull/48) by @MaxenceEpitech, letting agents drive the UE automation framework in-process without a second editor instance or commandlet; (5) a stack of **CDO / index hardening fixes** — TInstancedStruct CDO serialization (PR [#40](https://github.com/tumourlove/monolith/pull/40) by @fp12), the `RF_Transient`-on-UPackage corruption-of-cross-package-refs root-cause fix (PR [#43](https://github.com/tumourlove/monolith/pull/43), Issue [#42](https://github.com/tumourlove/monolith/issues/42), by @danielandric), the `add_event_node`-on-`UUserWidget` widget-`Tick` fix (PR [#46](https://github.com/tumourlove/monolith/pull/46), Issue [#47](https://github.com/tumourlove/monolith/issues/47), by @danielandric), and the `CreateBlueprint` flow `RF_Transient`-leak fix (PR [#49](https://github.com/tumourlove/monolith/pull/49) by @JCSopko); (6) the new **`mesh.export_mesh` FBX exporter** (PR [#41](https://github.com/tumourlove/monolith/pull/41) by @MaxenceEpitech), inverse of the existing `import_mesh`. Plus an action-count audit, public-doc path-leak scrub, and a sibling-plugin-name scrub across docs, specs, and the v0.14.7 release notes.

**Public action count: 1271** across 16 in-tree namespaces in the Monolith plugin proper (1267 distinct handlers; the +4 delta is the GAS UI binding aliases registered cross-namespace into `ui::`). Action namespaces from internal sibling plugins are not part of this release — sibling counts are specced in their own repos. With the experimental town-gen registration (`bEnableProceduralTownGen=true`), the in-tree total rises to 1316 (+45). For the authoritative per-namespace breakdown see `Plugins/Monolith/Docs/SPEC_CORE.md` §12.

### Added

- **MonolithLevelSequence — new in-tree module** (8 actions, `level_sequence` namespace) — PR [#45](https://github.com/tumourlove/monolith/pull/45) by **@yashabogdanoff**. Indexes every `ULevelSequence` asset end-to-end, not just those carrying a Director Blueprint. The indexer captures five custom SQLite tables: `level_sequence_directors` (one row per LS with a Director, with name + counts); `level_sequence_director_functions` (own user `FunctionGraphs` plus `K2Node_CustomEvent` in `UbergraphPages` plus the synthetic `SequenceEvent__ENTRYPOINT*` UFunctions UE generates for Sequencer Quick-Bind entries, classified as `user` / `custom_event` / `sequencer_endpoint`; inherited base methods and compiler `ExecuteUbergraph*` dispatchers excluded — matches the `MonolithBlueprint` `get_functions` convention); `level_sequence_director_variables` (each `NewVariables` entry, declaration order); `level_sequence_event_bindings` (every `FMovieSceneEvent` trigger / repeater across event tracks, with binding context + Director-function FK resolved via a per-asset post-pass JOIN); and the new `level_sequence_bindings` table (every `FGuid+BindingIndex` pair regardless of event-track presence — covers the UE 5.7 `UMovieSceneCustomBinding` family on `Sequence->GetBindingReferences()` that legacy `FindPossessable`/`FindSpawnable` would miss). Eight actions ship: `list_directors`, `get_director_info`, `list_director_functions`, `list_director_variables`, `list_event_bindings`, `find_director_function_callers`, `list_bindings`, plus `level_sequence.ping` smoke. Indexer write paths use `FSQLitePreparedStatement` end-to-end (CONTRIBUTING.md SQL discipline), no FK on `ls_asset_id` (core's `ResetDatabase()` would block reindex DELETEs as Issue #42's class), `LogMonolithLevelSequence` log category (mirrors `MonolithAI` / `MonolithGAS`). Two `UMonolithSettings` toggles (`bIndexLevelSequences` / `bEnableLevelSequence`, both default `true`) follow the existing `bIndex*` / `bEnable*` split. Spec at `Docs/specs/SPEC_MonolithLevelSequence.md`; skill at `Skills/unreal-level-sequences/`. Ships with full UE 5.7 custom-binding classification (`possessable` / `spawnable` / `replaceable` / `custom`) so modern Spawnables stop misresolving as legacy upgrade-stub possessables.
- **MonolithUI Phase A–L architecture expansion** — `ef9cc0a` lands the schema-driven Spec / Type Registry / Style Service / EffectSurface architecture that promotes `MonolithUI` from a flat action toolbox: 23,585 LOC added across UISpec / UISpecBuilder / UISpecSerializer / UISpecValidator, the Hoisted Design Import verbs (AnimationCore, AnimationEvent, FontIngest, Gradient, RoundedCorner, Shadow, TextureIngest), the Spec Builders sub-tree (PanelBuilder / LeafBuilder / CommonUIBuilder / EffectSurfaceBuilder), the Type Registry and Property Allowlist, the Style Service, the Animation MovieScene builder, and the UI Registry Subsystem. Phase L lands the EffectSurface optional-provider decoupling (reflective `UClass` probe; zero compile-time dependency on the provider; `-32010 ErrOptionalDepUnavailable` returned for the 10 EffectSurface action handlers when the provider is absent — see `Docs/specs/SPEC_MonolithUI.md` § "Error Contract"). Module action count moves to 117 module-owned (66 always-on + 51 CommonUI conditional on `WITH_COMMONUI`) plus the 4 GAS UI binding aliases registered cross-namespace into `ui::` for a tooling total of 121.
- **`blueprint::add_node` delegate-node family** (PR [#44](https://github.com/tumourlove/monolith/pull/44) by **@danielandric**) — Closes the workflow gap where authoring a UMG button event or a runtime delegate binding required manual Designer clicks. Five new `node_type` values: `ComponentBoundEvent` (the green event-entry node spawned by clicking "+" beside a component delegate in Designer; validates the component variable resolves on the BP `GeneratedClass`, that the delegate is `BlueprintAssignable`, and rejects duplicate `(component, delegate)` pairs BP-wide via `FKismetEditorUtilities::FindBoundEventForComponent` — matches the editor's own dedupe across ubergraph pages; works on widget BPs because `FindComponentProperty` accepts UMG widget properties); `AddDelegate` (`Bind Event to ...` runtime-binding node, `SetFromProperty` walks `DelegateProp->GetOwnerClass()` so inherited delegates resolve to the declaring class); plus `RemoveDelegate`, `ClearDelegate`, and `CallDelegate` covering the rest of the multicast-delegate node family that derive from `UK2Node_BaseMCDelegate` (closes the asymmetry where the editor's right-click menu exposes Bind / Unbind / Unbind all / Call but Monolith only authored Bind). `resolve_node` gains dry-run support for all five; `SerializeNode` extended with a `K2Node_BaseMCDelegate` branch covering future delegate node types transparently. `add_nodes_bulk` and `batch_execute` pick up all five with no dispatch-layer changes.
- **`editor.run_automation_tests` + `editor.list_automation_tests` actions** (PR [#48](https://github.com/tumourlove/monolith/pull/48) by **@MaxenceEpitech**) — Run / enumerate UE automation tests by full-path prefix (e.g. `MazeLegends.Bow`) via `FAutomationTestFramework::StartTestByName` + `StopTest` from inside the running editor. No PIE, no commandlet, no second editor process — sidesteps the `.uproject` file-lock that prevents `UnrealEditor -ExecCmds="Automation RunTests <prefix>"` from running while the editor is open. `run_automation_tests` returns a structured JSON summary (`success`, `total`, `passed`, `failed`, `skipped`) plus per-test results with error messages, so agents can drive a regression suite end-to-end (e.g. "lock down a calibrated weapon's data-asset values; assert across edits"). Latent / async tests (TickTests-driven) are not exercised by this sync path and are reported as `skipped` for visibility. Editor action count: 22 → 24.
- **`mesh::export_mesh` FBX export action** (PR [#41](https://github.com/tumourlove/monolith/pull/41) by **@MaxenceEpitech**) — Inverse of the existing `import_mesh`. Calls `UExporter::FindExporter` + `RunAssetExportTask` with the engine's built-in FBX exporter, supporting both `UStaticMesh` and `USkeletalMesh`. Round-trip workflow for editing project meshes in DCC tools (Blender, Maya) directly from the agent — no manual *Asset Actions → Export* needed. Params: `asset_path` (string, required), `file_path` (absolute output FBX path, required), `replace_existing` (bool, default `true`). Returns `{ asset_path, file_path, asset_class, file_size_bytes }`.
- **`blueprint` CDO read serializes `TInstancedStruct` properties** (PR [#40](https://github.com/tumourlove/monolith/pull/40) by **@fp12**) — `PropertyToJsonValue` now detects `FInstancedStruct` properties, unwraps the concrete inner struct, and emits a JSON object with a `__struct` field (the `UScriptStruct` asset path) plus all inner fields serialized recursively. Previously, `TInstancedStruct` fields fell through to the generic struct branch and returned empty/incorrect data, breaking `get_cdo_property` (and any other CDO read path) for DataAssets that use `TInstancedStruct` for polymorphic data — e.g. `UCyTargetingPattern` entries in CyberVikings. The original PR added a `StructUtils` module dependency; that was subsequently dropped in `ecdb42f` because `FInstancedStruct` and friends relocated into `CoreUObject`'s public surface in UE 5.5+ (existing `#include "StructUtils/InstancedStruct.h"` paths resolve transparently from `CoreUObject` now).
- **MonolithUI box slot primitives — sizeRule / fillWeight + min/max desired** — `bee2c03` lifts `UVerticalBoxSlot` / `UHorizontalBoxSlot` from `{hAlign, vAlign, padding}` to `{hAlign, vAlign, padding, sizeRule, fillWeight}` in the Spec round-trip, and adds `SizeBox` `MinDesired*` / `MaxDesiredHeight*` overrides to the read path alongside the existing `Width/HeightOverride` capture. Closes the §6.3.3 surface-map gap so the `dump_ui_spec` → `build_ui_from_spec` round-trip preserves the box-slot fields agents actually tune.
- **JSON-RPC error catalogue documented in `SPEC_CORE.md`** (`ef9cc0a`) — Standard codes (`-32700` parse, `-32600` invalid request, `-32601` method not found, `-32602` invalid params, `-32603` internal error) mirror JSON-RPC 2.0; Monolith's server-defined `-32000..-32099` range carries `ErrOptionalDepUnavailable=-32010` for the optional-sibling-plugin-absent case (first consumer: the 10 EffectSurface action handlers). Reserved range `-32011..-32019` left open for future "optional dep" codes. Constants in `Plugins/Monolith/Source/MonolithCore/Public/MonolithJsonUtils.h`.

### Fixed

- **`MonolithIndex` `RF_Transient` corruption of cross-package `TObjectPtr` saves** (PR [#43](https://github.com/tumourlove/monolith/pull/43), Issue [#42](https://github.com/tumourlove/monolith/issues/42), by **@danielandric**) — `TryUnloadPackage` was setting `RF_Transient` on indexed-asset `UPackage`s to encourage GC, but `RF_Transient` is a save flag (`ObjectMacros.h:565`, "Don't save object."), not a GC flag. `GARBAGE_COLLECTION_KEEPFLAGS` in editor is `RF_Standalone` only (`GarbageCollection.h:28`); `RF_Transient` is never consulted by reclamation. When GC failed to reclaim a package (any package still pinned by a BP CDO, editor watcher, asset registry, or thumbnail cache), the live `UPackage` retained `RF_Transient`. `UObject::IsAsset()` (`Obj.cpp:2733`) then returned `false` for every asset in that package, and cross-package `TObjectPtr` saves silently stripped refs to those targets — no warning, no error. The corruption survived cold restart because the indexer pass re-applied the flag on every editor startup. Triggered under default settings on any asset class with a registered deep indexer routing through `TryUnloadPackage` (`UInputAction`, `UMaterial`, `UStaticMesh`, `UNiagaraSystem`, `UWorld`). Fix: drop `SetFlags(RF_Transient)`. The GC-eligibility intent is fully delivered by `Package->ClearFlags(RF_Standalone)`, which is preserved.
- **`blueprint.add_event_node` resolves inherited overrides on non-`AActor` parents** (PR [#46](https://github.com/tumourlove/monolith/pull/46), Issue [#47](https://github.com/tumourlove/monolith/issues/47), by **@danielandric**) — `HandleAddEventNode` aliases `AActor`-style event names to their `ReceiveX` counterparts before walking the parent class chain (e.g. `Tick` → `ReceiveTick`). Non-`AActor` `BlueprintImplementableEvent` hosts use the bare names — `UUserWidget` declares `Tick`, not `ReceiveTick`. The alias-resolved walk therefore returned no match and the action silently fell through to the `K2Node_CustomEvent` branch, producing a custom event titled `Tick` that compiled but never fired on widget tick. Authoring widget-`Tick` chains via `add_event_node` was blocked. Fix: when the alias-resolved walk finds no `UFunction` AND the alias actually changed the input name, retry the parent-chain walk with the original un-aliased `EventName`. On a hit, realign both `EventFName` and `ResolvedEventName` so the downstream override-uniqueness check, `SetExternalMember` call, and response telemetry all reference the function that exists on the resolved `DeclaringClass`. UE 5.7 confirms `UUserWidget`'s function name is `Tick` — the local C++ symbol `ReceiveTickEvent` in the compiler is a misleading variable name over a `GET_FUNCTION_NAME_CHECKED(UUserWidget, Tick)` lookup (`Engine/Source/Editor/UMGEditor/Private/WidgetBlueprintCompiler.cpp:1044`).
- **`blueprint.create_blueprint` flow no longer leaks `RF_Transient` onto fresh BPGCs** (PR [#49](https://github.com/tumourlove/monolith/pull/49) by **@JCSopko**) — Two operations in `HandleCreateBlueprint` diverged from the canonical `IAssetTools::CreateAsset` path (`AssetTools.cpp:1718-1782`) and together formed the `RF_Transient` leak path observed in HOFF 6 (Cozy SquirrelTamagotchi, 2026-04-30 session): four BPs created with stale `.uasset` paths on disk, multi-step `set_cdo_property` between create and save, overlapping prior-session `delete_assets` calls — all `save_asset` calls returned `saved:false`, then a load via `LinkerLoad.cpp:5032` crashed on a serial-size-mismatch reading the partial-bytes CDO. Removals: (1) `Package->FullyLoad()` after `CreatePackage` — `CreatePackage` never touches disk (`UObjectGlobals.cpp:1040-1050`), so `FullyLoad` on the existing-in-memory hit path forces a serialization read that pulled stale `RF_Transient` flags from a leftover `.uasset` into the live package; `AssetTools.cpp:1755-1772` omits this call. (2) Redundant `FKismetEditorUtilities::CompileBlueprint` after `FKismetEditorUtilities::CreateBlueprint` — `CreateBlueprint` already calls `FBlueprintCompilationManager::CompileSynchronously` before returning (`Kismet2.cpp:514-516`); the second compile triggered a reinstance pass that propagated `RF_Transient` onto the BPGC. Inline comments cite the engine-source rule each removal depends on so future readers can verify rather than re-derive.
- **`run_automation_tests` register-filter widening + class-name lookup + crash guard** (`1eaf84c` follow-up to PR [#48](https://github.com/tumourlove/monolith/pull/48) by **@MaxenceEpitech**) — Two bugs found while smoke-testing the new action against a real game-module test suite. (1) `FAutomationTestFramework::RequestedTestFilter` defaults to `SmokeFilter` only; game-module tests typically register with `ProductFilter`, so `GetValidTestNames()` returned 395 engine tests and 0 project tests on a fresh editor session. Fix: `SetRequestedTestFilter` to a union of all filter buckets (`Smoke|Engine|Product|Perf|Stress|Negative`) before enumerating. (2) `StartTestByName` looks up the registry by **class name** (e.g. `FBowDataAssetTest`), not the human-readable full path (`MazeLegends.Bow.DataAsset`). Passing the full path failed silently, left `GIsAutomationTesting=false`, and the subsequent `StopTest` tripped `check(GIsAutomationTesting)` → editor crash. Fix: use `Info.GetTestName()` (= class name) as the lookup key, pass the full path as the optional `InFullTestPath` argument so engine logs still show the readable name. Also gate on `ContainsTest()` up-front so a stale or malformed entry produces `status=skipped` instead of crashing. Verified: 3/3 pass on a real test suite, regression case (intentional value drift in `DA_Bow.ArrowScale3P`) returns `failed=1` with the assertion message captured in `results[].errors`.
- **MonolithUI box-shadow hardening for single-child wrappers** (`194c6d9`) — Box-shadow application was synthesizing a wrapper widget around each shadowed widget; when the wrapper held a single child the shadow placement could leak through the parent slot. The hoisted `ShadowActions` now hardens this case with explicit single-child-wrapper handling, plus a 244-LOC `ApplyBoxShadowTests` battery that locks the contract.
- **MonolithUI cleans up failed shadow widget insertions** (`6e50be1`) — When shadow application failed mid-insertion the partially-inserted shadow widget would survive, polluting the WidgetTree. The action now walks back the partial mutation on failure so the WBP is left untouched.
- **MonolithUI CommonUI button child-variable retirement is safe** (`914bdcc`) — Converting an existing button to a CommonUI button retired the original button's child variables; the previous path could leak the retired variable into the post-conversion `WidgetTree`. The retirement path is now driven by `MonolithUICommon` helpers that take the child variable down cleanly across the BP recompile.
- **MonolithUI CommonUI button conversion GUID cleanup** (`0dd4fe1`) — On CommonUI button conversion the source button's GUID identity was not being retired alongside the widget retirement; subsequent `dump_ui_spec` runs could surface a phantom GUID with no live widget. The conversion now scrubs the source GUID in the same pass.
- **MonolithUI Spec Builder dry-run is a true no-op** (`64f79c9`) — `dry_run=true` on `build_ui_from_spec` previously cancelled the `FScopedTransaction` at end but had already created the package, run widget construction, and compiled the blueprint by that point — so a dry-run could leave a transient `UWidgetBlueprint` behind on disk if something failed between `CreatePackage` and the cancel. The dry-run path now runs validation + `AssetRegistry` overwrite/parent inspection + diff counting **before** any package creation, widget construction, transaction, compile, or save; on `dry_run=true` it returns directly from the inspection phase. Plus a 271-LOC roundtrip-fidelity test pass and 224-LOC `LeafBuilder` test pass.

### Changed

- **MonolithUI Phase L EffectSurface decoupling** (`ef9cc0a`) — The 10 EffectSurface action handlers used to compile-time-depend on an external widget runtime provider that supplied the EffectSurface widget classes; that compile-time link prevented the public Monolith release zip from carrying those handlers cleanly. They are now invoked through a reflective `UClass` probe on registered widget classes, with `MonolithUI` carrying zero compile-time dependency on the provider. When the provider is absent the 10 handlers return `-32010 ErrOptionalDepUnavailable` — the action remains in the registry (so callers can still introspect via `monolith_discover`) and the rest of `ui::` is fully functional. The `make_release.ps1` `$LeakSentinels` list is the build-time defence against accidental optional-provider symbol leakage into public release DLLs.
- **`Monolith.uplugin` Description and per-namespace counts refreshed for v0.14.8** (`82f4e84`) — Description updated to 1271 in-tree actions across 16 in-tree domains; per-namespace counts updated (Mesh 240 → 194 default-active, Editor 22 → 24, UI 96 → 121); LevelSequence 8 added; experimental town-gen registration condition (`bEnableProceduralTownGen=true`) called out. Sibling-plugin actions deliberately excluded from the in-tree count. Wiki submodule pointer bumped to `3584630` ("docs: refresh wiki for v0.14.8") which carries the matching action-count corrections across 5 wiki pages.
- **Public docs scrubbed of absolute user paths** (`7719ad9`) — Absolute Windows project-root paths replaced with neutral `<project-root>` / `<YourProject>` placeholders in `CHANGELOG.md` (auto-updater example), `Skills/unreal-build/unreal-build.md` (UBT command example), and `Tools/MonolithProxy/README.md` (`.mcp.json` proxy paths). Path-leak hygiene per the author-attribution audit rules — literal local paths were leaking maintainer drive-layout into shipping documentation.
- **Action count audit baseline restated as 1271 in-tree across 16 namespaces** (`e6866c4`) — Re-verified against the live `monolith_discover()` registry on 2026-04-30. Editor 22 → 24 (+2 from PR [#48](https://github.com/tumourlove/monolith/pull/48)). UI 96 → 121 (Phase A–L expansion: 66 always-on + 51 CommonUI + 4 GAS aliases). Mesh 240 → 239 (one experimental town-gen action retired). With town gen registered: 1316 (+45). Sibling-plugin live-registry total reaches higher when host-project siblings are loaded; that delta is intentionally outside the public count.

### Internal

- **Sibling-plugin name scrub across public docs, specs, wiki, and v0.14.7 release notes** (`e1042bc`, `dd2e232`, `e2d6891`, `b47edc6`, `1d06c95`) — Sibling plugins are private internal work and shouldn't be enumerated by name in public release notes, public specs, or the public wiki. Five-commit sweep across `CHANGELOG.md`, `Docs/API_REFERENCE.md`, `Docs/SIBLING_PLUGIN_GUIDE.md`, `Docs/SPEC_CORE.md`, `Docs/specs/SPEC_MonolithUI.md`, `Scripts/make_release.ps1`, `Source/MonolithCore/Public/MonolithSettings.h`, `Source/MonolithIndex/Public/MonolithIndexSubsystem.h`, `Source/MonolithUI/Public/Spec/UISpec.h`, `Source/MonolithUI/Public/Spec/UISpecSerializer.h`, the `MonolithIndex` ProjectFindByType action, and the wiki submodule pointer. Two locations in the v0.14.7 entry rewritten: header now reads "Action namespaces from internal sibling plugins are not part of this release" instead of enumerating the four siblings; the Changed section drops the per-sibling action-count breakdown but retains the public-action-count discipline rationale. Tag `v0.14.7` stays at `a8982a7` (matches the shipped zip's git state); older release entries left as-published.
- **`StructUtils` module dependency dropped from `MonolithBlueprint`** (`ecdb42f`) — `FInstancedStruct` and friends relocated to `CoreUObject`'s public surface in UE 5.5+ (`Engine/Source/Runtime/CoreUObject/Public/StructUtils/`). The `StructUtils` module token added to `MonolithBlueprint.Build.cs` by PR [#40](https://github.com/tumourlove/monolith/pull/40) is no longer needed — resolves transparently via the `CoreUObject` public dep. Eliminates a UBT warning and pre-empts the eventual hard-removal of the deprecated plugin (already marked `DeprecatedEngineVersion=5.5`).
- **`MonolithLevelSequence` indexer write paths use prepared statements** (`8b7cf15`) — `CONTRIBUTING.md` requires "All SQL must use prepared statements to prevent injection. Never use string formatting to build SQL queries." The indexer's `INSERT` / `UPDATE` / `DELETE` paths were initially using `FString::Printf` with manual single-quote escaping (action handlers were already using prepared statements). This commit switches all indexer write paths to `FSQLitePreparedStatement` and removes the `EscapeSql` / `SqlText` helpers. Two new helpers added in the anonymous namespace: `BindNullableString` (binds NULL for empty `FString`s via the no-arg `SetBindingValueByIndex(int32)` overload) and `ExecWithInt64` (convenience for `DELETE/UPDATE WHERE col=?` single-int64-binding shape). Naming hygiene: `path_filter` parameter renamed to `asset_path_filter` in `list_directors` so both glob filters across the namespace share the same name (consistent with `find_director_function_callers`). `LogMonolithLevelSequence` `DECLARE/DEFINE` pair added; module startup + indexer-registration log lines routed through it instead of `LogMonolith`.
- **Redundant Level Sequence INI overrides retired** (`bb36f5c`) — Both `bIndexLevelSequences` and `bEnableLevelSequence` default to `true` in `UMonolithSettings` UPROPERTY initializers (`MonolithSettings.h`), so restating them in `Config/MonolithSettings.ini` was a no-op and stood out from convention — no other module carves out its own labelled section in the defaults INI. C++ defaults remain authoritative.

### Known limitations

- **MonolithGAS + MonolithIndex still hard-link `GameplayAbilities`** — the v0.14.7-flagged plan to migrate this to `Optional: true` + `WITH_GAMEPLAYABILITIES` source gate did not land in v0.14.8. `MonolithGAS.Build.cs:14` still carries `GameplayAbilities` unconditionally in `PublicDependencyModuleNames`; `MonolithIndex.Build.cs:32` carries it unconditionally in `PrivateDependencyModuleNames`; neither module has a `bHasGameplayAbilities` 3-location probe; no `#if WITH_GAMEPLAYABILITIES` guards exist at any GAS API call site in either module; `Monolith.uplugin` retains `GameplayAbilities` as a hard dependency (no `"Optional": true` flag); `make_release.ps1` `$LeakSentinels` still excludes the module per the v0.14.7 rationale. Functionally safe today under the .uplugin hard-dep auto-enable contract — the engine guarantees `GameplayAbilities` is loaded before any Monolith DLL initialises, so the hard-link cannot fault on a fresh end-user install. The `MonolithAI` F22 retrofit pattern (`bHasStateTree` / `bHasSmartObjects` 3-location probe + `MONOLITH_RELEASE_BUILD=1` force-OFF + per-`.cpp` `#if WITH_<MACRO>` guards) remains the implementation reference. Migration deferred to a future release; the gap is documented rather than hidden.

### Credits

- **@yashabogdanoff** — PR [#45](https://github.com/tumourlove/monolith/pull/45) the entire `MonolithLevelSequence` module: indexer + 5 schema tables (incl. UE 5.7 custom-binding awareness via `Sequence->GetBindingReferences()`), 8 actions, prepared-statement refactor, dedicated spec + skill + README integration. Substantial multi-commit greenfield contribution that extends Monolith's deep-indexer architecture into a new asset family.
- **@danielandric** — PR [#43](https://github.com/tumourlove/monolith/pull/43) `RF_Transient`-on-UPackage root-cause fix (Issue [#42](https://github.com/tumourlove/monolith/issues/42)) — the canonical "obvious-looking save flag, devastating GC consequences" trap. PR [#44](https://github.com/tumourlove/monolith/pull/44) the full delegate-node family for `add_node` (`ComponentBoundEvent` + `AddDelegate` / `RemoveDelegate` / `ClearDelegate` / `CallDelegate`), closing the asymmetry where Monolith only authored the editor's Bind verb. PR [#46](https://github.com/tumourlove/monolith/pull/46) the `add_event_node`-on-`UUserWidget` widget-`Tick` fix (Issue [#47](https://github.com/tumourlove/monolith/issues/47)) — the misleading `ReceiveTickEvent` C++ variable name was bait, the real engine name is `Tick`.
- **@MaxenceEpitech** — PR [#48](https://github.com/tumourlove/monolith/pull/48) the `editor.run_automation_tests` + `list_automation_tests` action pair (and the follow-up `1eaf84c` filter-widen + class-name-key + crash-guard hardening), plus PR [#41](https://github.com/tumourlove/monolith/pull/41) the `mesh.export_mesh` FBX exporter. The automation actions are particularly load-bearing: they let agents drive UE's automation framework in-process without spawning a second editor or commandlet, sidestepping the `.uproject` file-lock entirely.
- **@JCSopko** — PR [#49](https://github.com/tumourlove/monolith/pull/49) `CreateBlueprint` flow `RF_Transient` leak fix. Engine-source-cited removal of two operations (`FullyLoad` after `CreatePackage`; redundant `CompileBlueprint` after `CreateBlueprint`) that diverged from `IAssetTools::CreateAsset`'s canonical path. Closes the HOFF 6 four-BP corruption-on-save repro from the 2026-04-30 Cozy SquirrelTamagotchi session.
- **@fp12** — PR [#40](https://github.com/tumourlove/monolith/pull/40) `TInstancedStruct` CDO read-path serialization. Polymorphic-data DataAssets (e.g. `UCyTargetingPattern`) now round-trip through `get_cdo_property` cleanly with `__struct` typing.

Full diff: [v0.14.7...v0.14.8](https://github.com/tumourlove/monolith/compare/v0.14.7...v0.14.8)

## [0.14.7] - 2026-04-26

This release rolls up four work-streams: (1) responsible-disclosure security response to [#38](https://github.com/tumourlove/monolith/issues/38) (CORS lockdown, MCP kill-switch, auto-update SHA256 verification, default-off auto-update); (2) **F22 P0 SmartObjects + StateTree gating retrofit** — closes the same class of bug as [#30](https://github.com/tumourlove/monolith/issues/30) and [#32](https://github.com/tumourlove/monolith/issues/32) where end users hit C1083/LNK2019 on plugins they hadn't enabled in their `.uproject`; (3) the Phase J fix sprint (audio/BT/GAS validation + observability + spec corrections); (4) StructUtils deprecation cleanup post-F22 — the deprecated plugin's headers relocated into CoreUObject in 5.5+. Plus PR [#37](https://github.com/tumourlove/monolith/pull/37) (community contribution by @MaxenceEpitech: anim graph property setter + native-component overrides + extended HTTP retry), the CommonUI M0.5 action pack (50 new actions), and PR [#39](https://github.com/tumourlove/monolith/pull/39) by @danielandric (recursive cradle sub-case + walker unification).

**Public action count: 1239** across 16 namespaces in the Monolith plugin proper. Action namespaces from internal sibling plugins are not part of this release. For authoritative per-namespace breakdown see `Plugins/Monolith/Docs/SPEC_CORE.md` §12.

### Security ([#38](https://github.com/tumourlove/monolith/issues/38))

Public responsible-disclosure response to a security audit by @playtabegg. The CORS finding was the only realistically exploitable item (browser tab pinging localhost while editor is open); the rest were defence-in-depth hardening.

- **CORS restricted to localhost origins** — the previous wildcard CORS header allowed any browser tab on any origin to hit the localhost MCP listener while the editor was open. Now strictly checks `Origin` against `localhost` / `127.0.0.1` / `[::1]` patterns.
- **MCP HTTP server kill-switch** (`bMcpServerEnabled`) — settable via `Project Settings → Plugins → Monolith` or environment variable. When false, the in-process HTTP listener never binds; the rest of the plugin still works (offline `monolith_query.exe` etc.). Default `true` to preserve existing behaviour.
- **Auto-update opt-in default `false`** (`bAutoUpdateEnabled`) — closes a small window where the C++ default (`true`) was used before the shipped INI default (`false`) loaded, allowing auto-update to fire without explicit opt-in on a fresh project.
- **SHA256 verification of auto-update tarballs** — auto-update path now hashes the downloaded tarball against the release manifest before extraction. Previously the tarball was trusted on download.
- **`SECURITY.md` disclosure policy** — published. Future findings via private email rather than public issue comments.
- **README MCP-exposure section** — explicit documentation of what the MCP HTTP server exposes, what it does NOT expose, and how to disable.

### Added

- **`audio::create_test_wave` action** (F18) — procedurally generates a sine-tone `USoundWave` for test fixtures with no asset dependencies. Validates `frequency_hz` (20–20000), `duration_seconds` (0.05–5.0), `sample_rate` ({22050,44100,48000}), `amplitude` ((0,1]). UE 5.7 `FEditorAudioBulkData::UpdatePayload(FSharedBuffer, Owner)` payload write (legacy `Lock`/`Realloc`/`Unlock` removed in UE 5.4+). Unblocks J3 TC3.19 (USoundWave direct binding) and any future test needing a disposable wave.
- **5 helper MCP actions** (F8) — `editor::create_empty_map` (UWorldFactory + IAssetTools), `editor::get_module_status` (IPluginManager + FModuleManager reflection), `gas::grant_ability_to_pawn` (CDO mutation via reflection on convention-named `TArray<TSubclassOf<UGameplayAbility>>` UPROPERTY), `ai::add_perception_to_actor` (any actor BP, `senses` array), `ai::get_bt_graph` (flat node_id/parent_id/children GUID dump). Resolves J2/J3 spec prerequisites that previously blocked agent-driven test setup.
- **`ULeviathanVitalsSet` AttributeSet** (F4) — six `FGameplayAttributeData` (Health/MaxHealth/Sanity/MaxSanity/Stamina/MaxSamina), `PreAttributeChange` clamps, `PostGameplayEffectExecute` re-clamps, REPNOTIFY_Always replication. Eldritch resistance attributes deferred to horror-system spec.
- **`MonolithSource` auto-reindex on hot-reload** (F17) — `UMonolithSourceSubsystem` binds `FCoreUObjectDelegates::ReloadCompleteDelegate` and kicks `TriggerProjectReindex()` (project-only — engine source DB stays frozen at bootstrap) on every Live Coding patch and post-UBT hot-reload. Three guards: 5-second cooldown, `bIsIndexing` re-entrancy, bootstrap-DB-missing skip. Eliminates manual `source.trigger_project_reindex` calls in the dev loop.
- **GAS UI binding observability** (F9) — 8 new `UE_LOG` sites: 4 handler-success (bind/unbind/list-Verbose/clear) plus per-fire `ApplyValue` trace at Verbose plus owner-resolution Warning escalation gated by 1-second grace window (`FActiveSub::FirstSubscribeAttemptTime` + `bGraceEscalated`). All 7 pre-existing UE_LOG sites unified under parent `LogMonolithGAS` (file-static `LogMonolithGASUIBinding`/`LogMonolithGASUIBindingExt` retired).
- **Frontmatter Tool-Allowlist Discipline rule** (F13) — `.claude/rules/always/agent-rules.md` adds rule preventing future F10-style drift (foreign-namespace tool named in agent prompt MUST appear in `tools:` frontmatter). New `Plugins/Monolith/Scripts/lint_agent_tools.py` automates the check (pure stdlib, exit 1 on violations, walks all 30 agents).
- **F22 — P0 SmartObjects + StateTree gating retrofit** (`MonolithAI.Build.cs`) — The prior Build.cs hard-added 7 modules to `PrivateDependencyModuleNames` and force-defined `WITH_STATETREE=1` + `WITH_SMARTOBJECTS=1`. The five backing engine plugins (StateTree, GameplayStateTree, PropertyBindingUtils, StructUtils, SmartObjects) all carry `EnabledByDefault: false` in their `.uplugin` manifests — end users on a fresh project install hit C1083 (missing headers) and LNK2019 (missing module exports) when loading the Monolith plugin without first enabling these engine plugins via the .uproject Plugins panel. Same shape as Issue [#30](https://github.com/tumourlove/monolith/issues/30) where MonolithMesh.dll hard-linked GeometryScriptingCore.dll. Fix: two new conditional probe blocks (`bHasStateTree` + `bHasSmartObjects`) modeled on the existing `bHasGameplayAbilities` / GBA / CommonUI patterns. Each probes 3 locations (engine `Plugins/Runtime/<Plugin>/`, engine `Plugins/AI/<Plugin>/`, project `Plugins/<Plugin>/`) and honours `MONOLITH_RELEASE_BUILD=1` to force OFF for binary releases. `.cpp` action sites already guarded with `#if WITH_STATETREE` / `#if WITH_SMARTOBJECTS` — `RegisterActions` becomes empty when the macro is 0 so the StateTree + SmartObjects actions simply do not register on hosts without those plugins.
- **CommonUI action pack — M0.5 milestone** (50 new actions) — Activatable widget infrastructure (stack, switcher, push/pop), CommonUI button / text / border style classes (class-as-data Blueprint pattern), input action data tables and bound action bars, generic input listeners, focus management (navigation, initial focus, focus path, force-focus, focus ring enforcement), animated switcher, widget carousel, hardware visibility border, lazy-image, load-guard, common message dialogs, modal overlays, tab list. Conditional on `#if WITH_COMMONUI` with 3-location Build.cs detection (consistent with other optional integrations). Default button class auto-created at `/Game/Monolith/CommonUI/MonolithDefaultCommonButton`. Authored by @tumourlove; verified PASS on M0.5.1 testing pass.
- **PR [#37](https://github.com/tumourlove/monolith/pull/37) — anim graph property setter, native-component property setter, extended HTTP retry** (community contribution by @MaxenceEpitech) — `set_anim_graph_node_property` lets agents tune existing AnimNode pins after the node is placed. `native-component set_component_property` extends the property setter to native-component instances on Blueprint classes (a long-standing gap). Extended HTTP bind retry hardens the v0.14.3 base (`Monolith.Restart` console command + 5-attempt exponential backoff) for additional zombie-listener cases.

### Fixed

- **Behavior Tree crash hardening** (F1) — Five `ai::add_bt_*` actions and `build_behavior_tree_from_spec` now reject Task-under-Root parenting at the API entry point via `ValidateParentForChildTask` helper plus schema-checked `ConnectParentChild`. Root cause: `UBehaviorTreeGraphNode_Root::NodeInstance` is `nullptr` by engine design; wiring a Task there produced a malformed graph that crashed `UBehaviorTreeGraph::UpdateAsset()` at `BehaviorTreeGraph.cpp:517`.
- **`gas::bind_widget_to_attribute` rejects unknown `owner_resolver`** (F2) — `ParseOwner` no longer silently coerces unrecognized strings (e.g. `"banana"`) to `OwningPlayerPawn`. Returns enumerated valid-list error: `[owning_player_pawn, owning_player_state, owning_player_controller, self_actor, named_socket:<tag>]`. Empty input still defaults (back-compat).
- **`gas::bind_widget_to_attribute` rejects malformed `format_string` templates** (F3) — New `ValidateFormatStringPayload` helper enforces `{0}` slot when `format=format_string`, plus `{1}` whenever `max_attribute` is bound. Both bare and typed-slot forms accepted. Catches user-supplied `format=format_string:NoSlots` AND `format=auto` auto-promoted to FormatString without template.
- **`audio::bind_sound_to_perception` rejects four silent-accept input seams** (F11) — pre-flight `ValidateBindingParams` rejects `loudness < 0`, `max_range < 0`, `tag.Len() > 255`. New `ParseSenseClass` strict allowlist: Hearing only (case-insensitive, accepts `"Hearing"` and `"AISense_Hearing"`); future classes (Sight/Damage/Touch/Team/Prediction) return distinct `"deferred to v2"` error; everything else returns `"Unsupported sense_class '<X>'"`. Replaces buggy `TObjectIterator` walk where `"AISense_Sight".Equals("Sight", IgnoreCase)` was FALSE causing silent fallback to Hearing.
- **Invalid-GUID vs unknown-GUID error messages now distinct** (F15) — 16 sibling sites in `MonolithAIBehaviorTreeActions.cpp` hoisted into new `RequireBtNodeByGuid` helper. Parse failure → `"<param> 'X' is not a valid GUID"`. Lookup failure → `"No node with GUID 'X' in BT 'Y'"`. Bonus: 4 empty-or-resolve sites also emit `"Root node not found in BT graph"` distinct from GUID-resolve failures.
- **GAS UI binding response-shape & error-text drift** (F5) — `index` → `binding_index`, composite `attribute`/`max_attribute` strings added alongside split fields, `widget_class` field added to list response, `removed_binding_index` added to unbind response, "Available widgets: [...]" enrichment via `BuildAvailableWidgetsClause` (sorted, capped at 20), `BuildValidPropertiesClause` enrichment for invalid-property errors, `LoadWBP` split into not-found vs wrong-class branches.
- **CDO save pipeline cradle/walker fixes** (F9 — PR [#39](https://github.com/tumourlove/monolith/pull/39) by **@danielandric**) — Four-mechanism fix: transient-outer reparent (`MonolithEditCradle::ReparentTransientInstancedSubobjects`), walker unification (`WalkObjectRefLeaves`), `FMapProperty::ValueProp` double-offset fix, sparse-iteration fix (`Helper.GetMaxIndex()` + `IsValidIndex`). Closes inline-subobject sub-case left after [#29](https://github.com/tumourlove/monolith/issues/29) (v0.14.3's recursive cradle).
- **Drop deprecated StructUtils plugin/module dep** — Plugin marked `DeprecatedEngineVersion=5.5`; `FInstancedStruct`, `FStructView`, `FSharedStruct`, `UserDefinedStruct` etc. all relocated into CoreUObject's public surface in 5.5+ (`Engine/Source/Runtime/CoreUObject/Public/StructUtils/`). Removed `"StructUtils"` token from `MonolithAI.Build.cs` `bHasStateTree` block and `Monolith.uplugin`'s plugin entry. Existing `#include "StructUtils/InstancedStruct.h"` paths resolve transparently from CoreUObject — no source-include changes needed. Silences the per-launch `LogPluginManager: Display: The Plugin StructUtils has been marked deprecated for 5.5 and will be removed soon` warning and pre-empts the eventual hard-removal that would detonate `MonolithAI` mid-build with no warning.
- **Native-component overrides persist across editor restart** (PR [#37](https://github.com/tumourlove/monolith/pull/37) follow-up by @MaxenceEpitech + @tumourlove) — Components added to a Blueprint via `add_component` previously had their property overrides discarded on save+reopen. Routes property writes through the UPROPERTY Setter meta and special-cases `SkinnedAsset` (which has a non-trivial setter chain).

### Changed

- **J1/J2/J3 spec corrections** (F6 + F7 + F14 + F16) — 17 prereq corrections across J specs (9 missing fixtures promoted to create-as-disposable, 5 wrong-facts corrected including Mana → Sanity drift, 3 non-existent actions TODO'd then resolved by F8). J1 `warnings` field documented as omit-when-empty. Levenshtein "did you mean" replaced with full valid-property list. J2 TC2.16/TC2.17 sample responses rewritten to document `event_tag`/`node_name` as omit-when-empty. J2 swept of `Ability.Combat.Punch`/`Kick` references — replaced with existing `Ability.Combat.Melee.Light`/`Heavy` registry tags (verified at `Config/DefaultGameplayTags.ini:26-27`); fixture abilities renamed.
- **Public action count restated as 1239** (16 public namespaces). The previous "1277 → 1283 (+6 from Phase J)" framing didn't reflect the actual public surface in the release zip — it included pre-Phase-J counts that hadn't been audited against ground truth, and conflated internal sibling-plugin actions with the public Monolith plugin proper. The +6 Phase J adds (F8 + F18) and other in-release additions (CommonUI M0.5 +50 actions, PR #37 anim graph setter etc., F22 retrofit gating) all roll up into the 1239 figure.

### Removed

- **`Templates/CLAUDE.md.example` no longer ships** — The shipped CLAUDE.md template was a static snapshot that grew stale fast (tool list, action counts, conventions all drift). For a project-instructions file that fits your toolchain, ask your AI assistant directly. Practical prompt to feed your LLM after installing Monolith: *"I've installed the Monolith Unreal plugin. It exposes ~1239 actions over an in-process MCP HTTP listener at `http://localhost:9316/mcp`. What's the best-practice format for a project-instructions file for this assistant — `CLAUDE.md` / `AGENTS.md` / `.cursorrules` / `.github/copilot-instructions.md` / etc.? Should help with action discovery, asset-path conventions like `/Game/Path/Asset`, and verifying UE 5.7 APIs via `source_query` before writing code."* — different tools have different conventions and they evolve faster than a template can keep up.

### Internal

- **Agent frontmatter cross-namespace dispatcher additions** (F12) — 5 agents had cross-namespace `mcp__monolith__*` tools added to their `tools:` frontmatter line so `ToolSearch select:` could load them: `unreal-ai-expert`, `unreal-audio-expert`, `gas-expert`, `interface-architect`, `unreal-blueprint-expert`. Fixes the F10 prose-only patch where agents were told to use cross-namespace dispatchers but the dispatcher tool names were missing from their allowlists.
- **Domain Agents Are Editor Specialists rule** (new) — `.claude/rules/always/agent-rules.md` codifies that all domain agents (gas-expert, unreal-audio-expert, unreal-ai-expert, etc.) are editor specialists, not C++ implementation agents. Runtime C++ writing/refactoring belongs to `cpp-performance-expert` or `refactoring-expert`. Generalizes the prior anim-only rule. Cross-ref in `Docs/references/AgentRegistry.md`.
- **F22 ADR amendment in `SPEC_CORE.md`** — F22 entry updated post-StructUtils-cleanup to record that the deprecated StructUtils plugin module was subsequently dropped from the gated set in the same release. Preserves archaeological record without leaving the spec contradicting reality.
- **Sibling-plugin strip auto-discovery in `make_release.ps1`** — The release script now auto-discovers all `Plugins/Monolith*/` sibling folders (excluding Monolith itself) for `$StrippedModules` defense-in-depth, instead of a hardcoded list. New siblings get protected automatically without script maintenance.

### Known limitations (planned for v0.14.8)

- **MonolithGAS + MonolithIndex still hard-link `GameplayAbilities`** — they haven't received the F22 conditional probe treatment yet. Functionally fine in practice because `GameplayAbilities` is declared as a hard dep in `Monolith.uplugin` (no `Optional` flag), so the engine auto-enables it on Monolith install and guarantees load order before Monolith DLLs initialise. The release smoke check normally flags this as a sentinel hit, but the sentinel was relaxed for v0.14.7 specifically because the .uplugin contract makes it functionally safe. Honest take: this release has been through more testing rounds than I want to admit and we're shipping with the documented gap rather than rolling another full cycle. Migration to optional + `WITH_GAMEPLAYABILITIES` source gate is planned for v0.14.8 alongside the StructUtils-cleanup follow-up.

### Credits

- **@playtabegg** — Issue [#38](https://github.com/tumourlove/monolith/issues/38) responsible-disclosure security audit (CORS reachability + adjacent findings). Direct, fast-turnaround report with realistic exploit framing.
- **@MaxenceEpitech** — PR [#37](https://github.com/tumourlove/monolith/pull/37) anim graph property setter + native-component setter + extended HTTP retry. Substantial multi-area contribution.
- **@danielandric** — PR [#39](https://github.com/tumourlove/monolith/pull/39) recursive cradle sub-case + walker unification + `FMapProperty::ValueProp` offset fix + sparse-iteration fix. Closes the inline-subobject sub-case left after the v0.14.3 fix to Issue [#29](https://github.com/tumourlove/monolith/issues/29).

Full diff: [v0.14.5...v0.14.7](https://github.com/tumourlove/monolith/compare/v0.14.5...v0.14.7)

## [0.14.4] - 2026-04-24

### Fixed

- **Build error: missing `MonolithPackagePathValidator.h`** ([#35](https://github.com/tumourlove/monolith/issues/35)) — Header was added to working tree but not tracked by git when v0.14.3 was cut. Three modules (`MonolithAI`, `MonolithGAS`, `MonolithUI`) included it, causing `C1083` on clean builds. Now properly tracked. Reported by **@krojew**.

Full diff: [v0.14.3...v0.14.4](https://github.com/tumourlove/monolith/compare/v0.14.3...v0.14.4)

## [0.14.3] - 2026-04-24

### Added

- **HTTP bind retry with port probe** ([#33](https://github.com/tumourlove/monolith/pull/33)) — `Start()` now retries up to 5 times with exponential backoff when the port is held by a zombie editor instance. A TCP socket probe verifies the bind actually took, instead of trusting `StartAllListeners()` which can fail silently. New `Monolith.Restart` console command for manual recovery without restarting the editor. PR by **@MaxenceEpitech**.

- **Animation IK and bone control nodes** ([#34](https://github.com/tumourlove/monolith/pull/34)) — `add_anim_graph_node` now supports `TwoBoneIK`, `ModifyBone`, `LocalToComponentSpace`, and `ComponentToLocalSpace` node types. TwoBoneIK auto-exposes `EffectorLocation`, `JointTargetLocation`, and `Alpha` as input pins. New `expose_pins` parameter for manual pin control on any node type. PR by **@MaxenceEpitech**.

- **`add_variable_get` action** ([#34](https://github.com/tumourlove/monolith/pull/34)) — Places a `K2Node_VariableGet` in an ABP anim graph for reading AnimInstance member variables. Validates the variable exists on the skeleton class before spawning. Animation action count: 115 → 116. PR by **@MaxenceEpitech**.

### Fixed

- **Nested struct/array cross-package TObjectPtr serialization** ([#29](https://github.com/tumourlove/monolith/issues/29)) — `set_cdo_property` now fires recursive `PreEditChange`/`PostEditChangeChainProperty` on every nested sub-property containing object references, matching the Details panel's full edit cradle. Previously only the outer property got the notification, so inner `TObjectPtr` fields in structs and arrays would serialize as null on save. Also wired the cradle into `create_data_asset` and `create_blueprint` to fix creation-side `FOverridableManager` poisoning. Reported by **@danielandric**.

### Credits

- **@MaxenceEpitech** — PRs [#33](https://github.com/tumourlove/monolith/pull/33), [#34](https://github.com/tumourlove/monolith/pull/34) (HTTP retry + animation IK nodes). Two solid contributions in the same day.
- **@danielandric** — Issue [#29](https://github.com/tumourlove/monolith/issues/29) (nested property cradle). Thorough repro with the IMC DefaultKeyMappings case — made the fix straightforward.

Full diff: [v0.14.2...v0.14.3](https://github.com/tumourlove/monolith/compare/v0.14.2...v0.14.3)

## [0.14.0] - 2026-04-20

### Added

- **macOS (Apple Silicon) support** ([#24](https://github.com/tumourlove/monolith/pull/24)) — Monolith now builds and runs on macOS 15 / Apple Silicon under UE 5.7. Uses the existing Python proxy as the stdio↔HTTP bridge (the native C++ proxy remains Windows-only for now).
  - New `Scripts/monolith_proxy.sh` shell launcher with `python3`/`python` auto-detection and 3.8+ version gate (parity with `monolith_proxy.bat`).
  - `Scripts/monolith_proxy.py` now declares `from __future__ import annotations` so PEP 604 type syntax (`str | None`) works on Python 3.8+ — macOS ships 3.9 by default.
  - `MonolithNiagaraActions.cpp`: renamed local `NO` → `NodeObj` to dodge the `<objc/objc.h>` `#define NO __objc_no` macro leak that transitively reaches `ApplePlatformProcess.h` and broke compilation.
  - `Monolith.uplugin`: dropped a ghost private-integration module reference after the integration moved to a sibling plugin outside `Plugins/Monolith/`; sibling plugins are naturally excluded from release zips by `git ls-files` scope, so no explicit stripping is required.
  - README + CONTRIBUTING updated to document macOS/Linux support and `.sh` launcher.
  - PR by **@MaxenceEpitech**.
  - **Note for macOS users:** this release ships Windows binaries only. Please clone the repo and build from source per `CONTRIBUTING.md` — the macOS build path is proven (all 17 Monolith dylibs compile on UE 5.7 / Apple Silicon). Prebuilt macOS dylibs will follow once a GitHub Actions macOS runner is wired up.

### Fixed

- **Editor crash on indexer pass with WorldPartition-enabled persistent level** ([#20](https://github.com/tumourlove/monolith/issues/20), fix [#21](https://github.com/tumourlove/monolith/pull/21)) — `LevelIndexer::IndexAsset` loaded level packages via `LoadPackage` to enumerate actors, which initializes `UWorldPartition` for WP-enabled levels (UE 5.4+ default). Because `LoadPackage` skips the editor's open-level flow, nothing tore down the subsystem, and when the batch loop marked the package for unload and GC eventually ran, `UWorldPartitionSubsystem::Deinitialize` asserted at `WorldPartitionSubsystem.cpp:507`. Fix uninitializes WorldPartition after `IndexActorsInLevel` and before `TryUnloadPackage(World)`. Affected every UE 5.4+ project with a WP-enabled persistent level and the default `bIndexLevels` setting. Reported and fixed by **@danielandric**.
- **Full Monolith rebuild on every UBT invocation after ZIP install** ([#22](https://github.com/tumourlove/monolith/issues/22), fix [#23](https://github.com/tumourlove/monolith/pull/23)) — PowerShell's `Compress-Archive` writes only DOS time (no NTFS or Unix extended timestamp), and DOS time is naked wall-clock with no timezone tag. `Expand-Archive` reinterprets the stored bytes as the user's local time, so a UTC+10-packaged ZIP extracted on UTC+0 landed with file mtimes ~10 hours in the user's future. UBT's `TargetMakefile.IsValidForSourceFiles` compares `ExternalDependency.LastWriteTimeUtc` against `Makefile.CreateTimeUtc`, so a future mtime on `Monolith.uplugin` tripped the check on every build and forced a full Monolith rebuild until the user's wall clock caught up. Affected every C++ user with auto-update on (default) and every C++ user installing from the ZIP manually. Fix mirrors POSIX tar's `--touch`: the auto-updater swap scripts (Windows + macOS/Linux) touch installed files post-xcopy, and `MonolithCoreModule::StartupModule` runs an idempotent self-heal that walks the plugin tree if `Monolith.uplugin` shows a future mtime (covers manual-install users). Microsoft acknowledged the underlying ZIP design flaw in [PowerShell/Microsoft.PowerShell.Archive#133](https://github.com/PowerShell/Microsoft.PowerShell.Archive/issues/133); their fix has not shipped. Reported and fixed by **@danielandric**.

### Changed

- **Release builds now run non-unity** — `Scripts/make_release.ps1` passes `-DisableUnity` to UBT so missing includes and unity-only symbol collisions get caught before they reach a public release.

### Credits

- **@danielandric** — PR [#21](https://github.com/tumourlove/monolith/pull/21) + issue [#20](https://github.com/tumourlove/monolith/issues/20) (WorldPartition indexer crash), PR [#23](https://github.com/tumourlove/monolith/pull/23) + issue [#22](https://github.com/tumourlove/monolith/issues/22) (ZIP mtime normalization). Thank you for two clean, well-diagnosed fixes in a single day.
- **@MaxenceEpitech** — PR [#24](https://github.com/tumourlove/monolith/pull/24) (macOS support — shell launcher, Python compat, Objective-C macro dodge, ghost module cleanup). Thanks for putting in the proof-of-work end-to-end build verification on Apple Silicon.

Full diff: [v0.13.2...v0.14.0](https://github.com/tumourlove/monolith/compare/v0.13.2...v0.14.0)

## [0.13.2] - 2026-04-19

### Hotfix

- **Pulled v0.13.1 — it accidentally shipped with some work-in-progress CommonUI stuff in `MonolithUI` that I forgot was sitting in my working tree.** Same #19 fix as 0.13.1, just rebuilt clean from a committed tree. Grab this one instead. The release script now refuses to run with a dirty working tree so this doesn't happen again.

## [0.13.1] - 2026-04-19 — DO NOT USE

Withdrawn. Use [v0.13.2](#0132---2026-04-19) — same fix, built from a clean tree. 0.13.1's release zip contained uncommitted WIP for unrelated `MonolithUI` work.

### Fixed

- **Indexer fatal crash: "Calling FinishCompilation is not allowed during PostCompilation"** ([#19](https://github.com/tumourlove/monolith/issues/19)) — sorry about this one, the fix I shipped for [#16](https://github.com/tumourlove/monolith/issues/16) in 0.13.0 caused the regression. I was calling `FAssetCompilingManager::FinishAllCompilation()` from inside `AsyncTask(ENamedThreads::GameThread, ...)` lambdas to avoid the reentrant compile crash, but those lambdas can land on the game thread while UE is already mid-`FTextureCompilingManager::PostCompilation`, and the engine fatals on that reentrance (`TextureCompiler.cpp:454`). Epic's own comment on the line above says workers should use `ExecuteOnGameThread` or tick-scheduled dispatch instead of `AsyncTask(GT)`. Done and done.
  - New `FMonolithCompilerSafeDispatch::RunOnGameThreadWhenCompilerIdle` helper — schedules work via `FTSTicker` (main tick loop, not task graph) and only fires when `FAssetCompilingManager::GetNumRemainingAssets() == 0`, with a 120s timeout safeguard.
  - All 8 asset-loading `AsyncTask(GT)` sites in `MonolithIndexSubsystem.cpp` rerouted through the helper: deep-index batch, dependency, level, data table, animation, gameplay tag, niagara, mesh catalog indexers.
  - All 5 `FinishAllCompilation()` calls inside indexer payloads deleted — the helper's idle-precondition is now the single point of compiler synchronization.
  - Reported by **@asafdubaaa**.

### Credits

- **@asafdubaaa** — issue [#19](https://github.com/tumourlove/monolith/issues/19) (caught the regression fast, thanks for the stack traces)

Full diff: [v0.13.0...v0.13.2](https://github.com/tumourlove/monolith/compare/v0.13.0...v0.13.2)

## [0.13.0] - 2026-04-18

### Added

- **MonolithAudio module shipped** — 81 actions across Phases 0-2: Sound asset CRUD (15), query/search (10), batch operations (10), Sound Cue graph building (21), MetaSound Builder API integration (25). Includes three power actions: `build_sound_cue_from_spec`, `build_metasound_from_spec`, `apply_audio_template`. MetaSound features gated on `WITH_METASOUND`. Phases 0-2 fully tested (28/28 PASS, 5 bugs fixed during test pass). Module had been completed + tested on 2026-04-08 but was not yet public.
- **Indexer RAM tier auto-detect** — `FMonolithMemoryHelper` now picks a memory budget and batch sizes based on installed RAM: 64+ GB → 32768 MB / deep=8 / post=4; 32+ GB → 16384 MB / deep=8 / post=4; 16 GB → 6144 MB / deep=4 / post=2; <16 GB → 3072 MB / deep=2 / post=1. Settings defaults changed to `0` (auto-detect sentinel) for `MemoryBudgetMB`, `DeepIndexBatchSize`, `PostPassBatchSize`. Users can still override via Project Settings > Monolith > Indexing > Performance. Tier logged once per editor session on first index run.

### Fixed

- **Indexer OOM + reentrant texture compiler crash on large projects** ([#16](https://github.com/tumourlove/monolith/issues/16)) — deep-index batches could exhaust physical RAM or re-enter `FTextureCompilingManager::ProcessAsyncTasks`, crashing the editor on large projects (>20 GB content). Fix introduces `FAssetCompilingManager::FinishAllCompilation()` guards before each batch, forced GC between batches, Slate-safe yields, emergency pause when available memory drops below 2 GB, and honors the async notification Cancel button. Shipped as PR [#17](https://github.com/tumourlove/monolith/pull/17) from **@alakangas**. Reported by **@MAYLYBY**.

### Changed

- **`bLogMemoryStats` default flipped to `false`** — opt in when debugging indexer memory behavior. Keeps shipped-project logs quiet.

### Credits

- **@alakangas** — PR [#17](https://github.com/tumourlove/monolith/pull/17) (indexer OOM + reentrant compiler crash fix)
- **@MAYLYBY** — issue [#16](https://github.com/tumourlove/monolith/issues/16) (detailed crash report that drove the fix and uncovered the low-spec regression we addressed with the RAM tier auto-detect)

Full diff: [v0.12.1...v0.13.0](https://github.com/tumourlove/monolith/compare/v0.12.1...v0.13.0)

## [0.12.1] - 2026-04-03

### Fixed

- **UE 5.7 compatibility** — resolved deprecated API usages that caused C2220 (warning-as-error) failures in non-unity builds ([#12](https://github.com/tumourlove/monolith/issues/12))
  - NavMesh: direct property access → `GetCellSize`/`SetCellSize`/`GetCellHeight`/`SetCellHeight`/`GetAgentMaxStepHeight`/`SetAgentMaxStepHeight` with `ENavigationDataResolution`
  - GAS: `EGameplayAbilityInstancingPolicy::NonInstanced` removed → legacy value cast
  - GAS: `StackingType` made private → reflection-based getter/setter
  - `FPackageName::DoesPackageExist` signature change (removed nullable param)
  - `FCollisionQueryParams::ClearIgnoredActors` → `ClearIgnoredSourceObjects`
  - `GetUsedTextures` simplified signature
  - `UMovieScene::GetBindings` const correctness, `FMovieSceneBinding::GetName` removal
- **Non-unity build** — fixed symbol collisions across 8 files (`VecToArr`, `ParseVectorArray`, `ParseStringArray`, `GetAssetPath` renamed to module-prefixed variants)
- **ComboGraph log category** — eliminated duplicate `DEFINE_LOG_CATEGORY_STATIC` across `#if`/`#else` branches; proper extern declaration in header
- **Uninitialized variables** — `FVector` locals now zero-initialized to suppress C4700/C6001

### Improved

- **StateTree schema resolution** — `ResolveStateTreeSchemaClass` searches multiple candidate paths including `/Script/GameplayStateTreeModule.*`
- **UI animation system** — `FindOrCreateWidgetAnimationBinding`, `FindOrCreateFloatTrack` helpers; transform/color component keyframes; proper `WidgetVariableNameToGuidMap` bookkeeping
- **UI param handling** — new `TryGetRequiredString`, `GetOptionalString`, `GetOptionalBool` helpers; duplicate-asset guard on widget creation

## [0.12.0] - 2026-04-01

Biggest release yet: +310 actions (815 to 1125). Two new domain modules (MonolithAI, MonolithLogicDriver), ComboGraph expansion. Python-to-C++ port of standalone tools eliminates Python as a runtime dependency. 14 skills (up from 12).

### Added

**MonolithAI (229 actions) — AI Asset Manipulation**

The most comprehensive AI tooling available through any MCP server. 229 actions across 15 categories, 24K lines C++, 30 files. Covers Behavior Trees, Blackboards, State Trees, EQS, Smart Objects, AI Controllers, AI Perception, Navigation, Runtime debugging, Scaffolds, Discovery, and Advanced categories.

- Crown jewels: `build_behavior_tree_from_spec` and `build_state_tree_from_spec` — create complete AI assets from a JSON specification
- BT nodes: tasks, decorators, services, composites — full CRUD with property configuration
- Blackboard: key CRUD with type configuration (bool, int, float, string, name, vector, rotator, enum, object, class)
- State Trees: states, transitions, conditions, tasks, evaluators — full structural editing
- EQS: queries, generators, tests — complete template management
- Smart Objects: definitions, slots, behaviors, tags, claim policies
- AI Controllers: Blueprint creation, BT/BB assignment, perception, pathfinding config
- Perception: sight, hearing, damage, touch — sense parameters, stimulus sources
- Navigation: mesh queries, path filtering, invokers, query filters, avoidance
- Runtime/PIE: start/stop BTs, query active nodes, inspect BB values, debug perception
- Scaffolding: patrol AI, combat AI, companion AI, guard AI — complete setup generation
- Conditional on `#if WITH_STATETREE`, `#if WITH_SMARTOBJECTS` (required). Optional: `#if WITH_MASSENTITY`, `#if WITH_ZONEGRAPH`

**MonolithLogicDriver (66 actions) — Logic Driver Pro State Machines**

Full integration with Logic Driver Pro marketplace plugin. Reflection-only (no direct C++ API linkage, version-agnostic). Conditional on `#if WITH_LOGICDRIVER` with 3-location Build.cs detection.

- SM CRUD: create, inspect, compile, delete, list, duplicate, validate
- Graph read/write: add states, transitions, configure properties, set transition rules
- Node config: state nodes, conduits, transition events, property editing via reflection
- Runtime/PIE: start, stop, query active states, trigger transitions, inspect variables
- `build_sm_from_spec`: one-shot state machine creation from JSON specification
- JSON spec: export, import, validate, diff for templating and version control
- Scaffolding: door controller, health system, AI patrol, dialogue, elevator, puzzle, inventory
- Components: add/configure Logic Driver components on actors
- Text graph: text-based visualization for debugging
- Discovery: list node classes, state types, templates

**MonolithComboGraph expanded (12 → 13 actions)**

- Added `auto_layout` action for combo graph node arrangement

**Standalone C++ Tools (Python-to-C++ port)**

Two standalone C++ executables replace the Python scripts. Zero Python dependency at runtime, zero UE dependency, instant startup.

- **`monolith_proxy.exe`** (473KB) — MCP stdio-to-HTTP proxy, replaces `Scripts/monolith_proxy.py`. Full feature parity: JSON-RPC, health poll, tool dedup, editor query splitting, action allowlist/denylist. Built with WinHTTP + nlohmann/json. Source: `Tools/MonolithProxy/monolith_proxy.cpp` (775 lines).
- **`monolith_query.exe`** (1.8MB) — Offline DB query tool, replaces `monolith_offline.py` AND `MonolithQueryCommandlet`. 14 actions: 9 source (search_source, read_source, find_callers, find_callees, find_references, get_class_hierarchy, get_module_info, get_symbol_context, read_file) + 5 project (search, find_by_type, find_references, get_stats, get_asset_details). Built with sqlite3 amalgamation. Source: `Tools/MonolithQuery/monolith_query.cpp` (1080 lines).
- Python scripts remain as deprecated fallbacks for environments without the exe
- `MonolithQueryCommandlet` deleted — standalone exe is faster (instant startup vs 6+ second UE engine load)
- 2 new Claude Code skills: `unreal-logicdriver`, `unreal-combograph`

### Changed

- Total: 815 → 1125 actions across 15 modules (was 13), exposed through 18 MCP tools (was 15)
- Blueprint: 86 → 88 actions
- ComboGraph: 12 → 13 actions
- Skills: 12 → 14 bundled with plugin
- `.mcp.json` recommended proxy config changed from Python script to `monolith_proxy.exe`
- Python is no longer required for any core functionality (only for optional project C++ source indexing via `Scripts/index_project.py`)

### Removed

- `MonolithQueryCommandlet` — replaced by standalone `monolith_query.exe`

---

## [0.11.0] - 2026-03-30

Massive expansion: +372 actions (443 → 815). Three new modules (MonolithMesh, MonolithGAS, MonolithUI) plus MonolithBABridge integration. MCP auto-reconnect proxy for Claude Code. Optional module system for third-party plugin integrations. Automated release builds with MONOLITH_RELEASE_BUILD env var. 12 skills (up from 9).

### Added

**MCP Auto-Reconnect Proxy**

Claude Code has a known issue where HTTP MCP sessions die permanently when the Unreal Editor restarts — forcing you to restart Claude Code every time you recompile, crash, or close the editor. Monolith now ships with a **stdio-to-HTTP proxy** (`Scripts/monolith_proxy.py`) that eliminates this entirely.

**Who it's for:** Claude Code users. Cursor and Cline handle reconnection natively and don't need this.

**What it does:**
- Keeps your MCP session alive across editor restarts — zero manual intervention
- Background health poll auto-detects when the editor comes up or goes down
- Sends `notifications/tools/list_changed` so Claude Code refreshes its tool list automatically
- When the editor is down, tool calls return graceful errors instead of killing the session
- When the editor comes back, the next tool call just works

**How to use it:** Update your `.mcp.json` to use the proxy instead of direct HTTP:

```json
{
  "mcpServers": {
    "monolith": {
      "command": "Plugins/Monolith/Scripts/monolith_proxy.bat",
      "args": []
    }
  }
}
```

Requires Python 3.8+ (stdlib only, no pip install). The `.bat` launcher auto-finds Python. No Python? The direct HTTP config still works — you'll just need to restart Claude Code after editor restarts.

- `Scripts/monolith_proxy.py` — stdio-to-HTTP proxy (pure Python, zero dependencies)
- `Scripts/monolith_proxy.bat` — Windows launcher that auto-detects Python
- `Templates/.mcp.json.proxy.example` — ready-to-copy config template

**MonolithMesh (242 actions — 197 core + 45 experimental town gen)**

The biggest module in Monolith. 22 capability tiers covering the entire spatial side of Unreal level design:

- **Core (197 actions):** Mesh inspection and comparison. Full actor CRUD with scene manipulation. Physics-based spatial queries (raycasts, sweeps, overlaps, navmesh) that work in-editor without PIE. Level blockout workflow with auto-matching and atomic replacement. GeometryScript mesh operations (boolean, simplify, remesh, LOD gen, UV projection, mirroring). Horror spatial analysis — sightlines, hiding spots, ambush points, choke points, zone tension, pacing curves, dead-end detection. Accessibility validation with A-F grading. Lighting analysis with dark corner detection and mood-based placement suggestions. Audio/acoustics with Sabine RT60 reverb estimation, stealth maps, quiet path routing, AI hearing simulation. Performance budgeting with region analysis, overdraw detection, shadow cost analysis. Decal placement with Catmull-Rom paths and 5 horror storytelling presets. Level design tools for lights, volumes, sublevels, prefabs, splines, HISM instancing. Tech art pipeline for mesh import, LOD gen, texel density, collision authoring, lightmap analysis. Context-aware prop scatter on any surface (floors, shelves, tables, walls, ceilings) with room disturbance levels. Procedural geometry — parametric furniture (15 types), horror props (7 types), architectural structures, multi-story buildings, mazes (3 algorithms), pipes, mesh fragmentation, terrain patches. Genre preset system (storytelling patterns, acoustic profiles, tension profiles) for any game type. Encounter design with patrol routes, safe room evaluation, scare sequence generation, and accessibility intensity validation. Quality and polish tools including naming conventions, batch rename, proxy mesh generation, HLOD, texture budgets, composition framing, and monster reveal scoring.
- **Town Gen (45 actions) — Work-in-progress:** Procedural floor plans, building facades, roofs, city blocks, spatial registry, auto-volumes, terrain/foundations, architectural features, room furnishing, debug views. Disabled by default (`bEnableProceduralTownGen` in Editor Preferences). Known geometry issues — wall misalignment, room separation. Very much a WIP; unless you're willing to dig in and help improve it, it's best left alone for now.

**MonolithGAS (130 actions)**

Complete Gameplay Ability System integration across 10 categories. All 4 implementation phases complete, 53/53 tests PASS.

- **Abilities (14):** Create, grant, revoke, activate, inspect, list, configure activation policies, cooldowns, costs, tags
- **Attributes (16):** AttributeSet CRUD for both C++ and Blueprint-based sets (Blueprint mode requires optional GBA plugin). Define attributes with min/max clamping, initialize from DataTables
- **Effects (18):** Gameplay Effect authoring with modifiers, duration policies, stacking, period, conditional application, granted abilities, immunity
- **ASC (12):** Ability System Component management — grant/revoke abilities, apply/remove effects, query active abilities and effects, initialize from DataTables
- **Tags (10):** Gameplay Tag utilities — query, add, remove, check tag containers, hierarchical queries
- **Cues (10):** Gameplay Cue management — create, trigger, inspect cues for audio/visual feedback
- **Targeting (12):** Target data generation, targeting tasks, trace-based and actor-based targeting
- **Input (8):** Input binding for ability activation via Enhanced Input
- **Inspect (16):** Runtime debugging — query active effects, ability states, attribute values, cooldown status, blocking tags
- **Scaffold (14):** Generate complete GAS setups from templates — character ability sets, attribute configurations, effect libraries, common gameplay patterns. Accessibility-focused infinite-duration GEs for reduced difficulty modes

**MonolithUI (42 actions)**

Widget Blueprint CRUD with a template system that scaffolds complete, functional UI. Pre-built templates for HUDs, main menus, settings screens, dialog boxes, loading screens, inventory grids, save/load menus, and toast notifications. Styling system for consistent theming. Widget animation support. Game-level scaffolding for settings, save systems, audio managers, input rebinding, and accessibility configurations. Built-in accessibility audit with colorblind modes and text scaling.

**Optional Module System**

- **MonolithBABridge** — Blueprint Assist integration via IModularFeatures bridge pattern. `auto_layout` action optionally delegates to BA's superior formatter when the marketplace plugin is installed. Compiles as empty shell when BA is absent — zero impact on users who don't own it.
- **Conditional GBA support** — MonolithGAS detects the GBA (Blueprint Attributes) plugin at compile time. Blueprint AttributeSet creation requires GBA; all other 130 GAS actions work without it.
- **`Build.cs` probe pattern** — Optional dependencies detected via `Directory.Exists()` at build time. `WITH_FOO=1` or `=0` preprocessor defines. No DLL load failures, no error dialogs. Full tutorial on [[Optional Modules]] wiki page.

**Skills (+3)**

- `unreal-mesh` — Mesh inspection, spatial queries, blockout, procedural geometry, horror/accessibility workflows
- `unreal-ui` — Widget Blueprint CRUD, templates, styling, accessibility workflows
- `unreal-gas` — Gameplay Ability System workflows: abilities, effects, attributes, ASC, tags, cues

### Changed

- **Total** — Action count 443 -> 815 (across 13 modules, exposed through 15 MCP tools)
- **Skills** — 9 -> 12 Claude Code skills bundled with plugin
- **Modules** — 10 -> 13 (added MonolithMesh, MonolithGAS, MonolithBABridge)

### Fixed

- SQLite multi-statement DDL was silently only executing the first statement, leaving source index schema incomplete (thanks @fp12 / ArcstoneGames)
- Plugin path resolution now uses `IPluginManager` instead of hardcoded `Plugins/Monolith/` — works for nested install paths like `Plugins/Marketplace/Monolith/` (thanks @fp12 / ArcstoneGames)

### Contributors

Thanks to **@fp12** (ArcstoneGames) for two solid PRs this release — real bugs caught and clean fixes shipped.

## [0.10.0] - 2026-03-25

Massive expansion across all modules: +153 actions (290 to 443). Niagara nearly doubles with 31 new actions and 10 bug fixes. Blueprint and Animation get major expansions. Material function suite rounds out the material pipeline.

### Added

**Niagara (+31, 65 -> 96)**

- `add_dynamic_input` / `remove_dynamic_input` / `set_dynamic_input_value` / `get_dynamic_input_info` / `search_dynamic_inputs` -- full dynamic input CRUD
- `add_event_handler` / `remove_event_handler` / `list_event_handlers` -- event handler management
- `add_simulation_stage` / `remove_simulation_stage` / `list_simulation_stages` -- simulation stage CRUD
- `create_npc_system` / `add_npc_behavior` / `get_npc_info` / `set_npc_property` / `list_npc_templates` -- NPC particle system support
- `create_effect_type` / `get_effect_type_info` / `set_effect_type_property` -- effect type CRUD
- `list_available_renderers` / `set_renderer_mesh` / `configure_ribbon` / `configure_subuv` -- renderer helpers
- `diff_systems` -- diff two Niagara systems side-by-side
- `save_emitter_as_template` -- save an emitter as a reusable template
- `clone_module_overrides` -- clone module overrides between emitters
- `preview_system` -- trigger a system preview in the editor
- `get_available_parameters` / `get_module_output_parameters` -- parameter introspection
- `rename_emitter` -- rename an emitter within a system
- `get_emitter_property` -- read a single emitter property
- `export_system_spec` expanded -- now includes event handlers, sim stages, static switches, and dynamic inputs

**Blueprint (+20, 66 -> 86)**

- `auto_layout` -- Modified Sugiyama graph layout algorithm for automatic node arrangement
- 22 new actions including expanded node types, resolve improvements, DataTable field resolution
- `batch_execute` improvements for bulk operations

**Animation (+41, 74 -> 115)**

- 41 new actions covering expanded montage editing, blend space manipulation, skeletal mesh queries, and animation asset management

**Material (+9, 48 -> 57)**

- `create_material_function` / `build_function_graph` / `get_function_info` -- material function full suite
- `batch_set_material_property` / `batch_recompile` -- batch operations
- `import_texture` -- image file import as UTexture2D
- `list_material_instances` / `replace_expression` / `rename_expression` -- additional utilities

**Project (+2, 5 -> 7)**

- 2 new project index actions for deeper asset discovery

### Fixed

**Niagara (10 fixes)**

- `batch_execute` reads now return data correctly instead of silently succeeding
- Type validation on module inputs catches mismatched types before crash
- GUID collision fix when duplicating emitters with shared module references
- ShapeLocation race condition on freshly-created emitters with shape DIs
- Color curve fan-out when multiple emitters share the same curve keys
- NPC namespace routing fixed for NPC-specific actions
- `move_module` now preserves parameter overrides during reorder
- 3 test-driven fixes from Phase 1-6 testing

**Material (6 fixes)**

- `AssetTagsFinalized` renamed to match UE 5.7 API change
- 5 missing includes that caused compile failures on clean builds

**Blueprint (5 fixes)**

- DataTable UDS field resolution -- match by display name
- `resolve_node` expanded -- Self, MacroInstance, Return, generic fallback
- K2Node generic fallback -- strip U prefix for UObject name lookup
- Simplified templates -- removed broken function refs
- Code review cleanup -- dead code, magic numbers, perf, correctness

### Changed

- **Niagara** -- Action count 65 -> 96
- **Blueprint** -- Action count 66 -> 86
- **Animation** -- Action count 74 -> 115
- **Material** -- Action count 48 -> 57
- **Project** -- Action count 5 -> 7
- **Total** -- Action count 290 -> 443 (across 10 modules)

## [0.9.0] - 2026-03-19

Major feature expansion: +69 actions across Blueprint, Material, Niagara, and Animation. IKRig, IK Retargeter, Control Rig, and AnimBP structural write support. Full Material instance CRUD. Niagara dynamic inputs, event handlers, and simulation stages. 60 bug fixes. 220 → 290 actions total.

### Added

**Blueprint (+20, 47 → 67)**

- `batch_execute` — dispatch multiple Blueprint operations in a single call
- `resolve_node` — resolve a node reference to its target (function, variable, etc.)
- `search_functions` — search functions and events by name across a Blueprint
- `get_node_details` — full detail dump for a single node (pins, defaults, metadata)
- `add_nodes_bulk` — add multiple nodes to a graph in one call
- `connect_pins_bulk` — connect multiple pin pairs in one call
- `set_pin_defaults_bulk` — set multiple pin default values in one call
- `scaffold_interface_implementation` — auto-generate stub event nodes for an unimplemented interface
- `add_timeline` — add a Timeline node to a graph
- `add_event_node` — add a named event node to a graph
- `add_comment_node` — add a comment box to a graph
- `get_function_signature` — return param list and return type for a Blueprint function
- `get_blueprint_info` — comprehensive Blueprint summary (class, interfaces, components, variable count)
- `get_event_dispatcher_details` — full detail for a single event dispatcher
- `remove_event_dispatcher` — remove an event dispatcher from a Blueprint
- `set_event_dispatcher_params` — change the parameter signature of an event dispatcher
- `validate_blueprint` (enhanced) — now detects unimplemented interfaces and duplicate events
- `promote_pin_to_variable` — promote a pin's value to a Blueprint variable
- `add_replicated_variable` — add a replicated variable with configurable RepNotify
- `add_node` (extended) — now supports cast node creation (`CastTo<ClassName>`)

**Material (+22, 25 → 47)**

- `auto_layout` — auto-arrange expression nodes in the material graph
- `duplicate_expression` — duplicate an expression node in-place
- `list_expression_classes` — list all available material expression class names
- `get_expression_connections` — return all connections into/out of an expression
- `move_expression` — move an expression node to a new graph position
- `get_material_properties` — return material-level properties (blend mode, shading model, etc.)
- `get_instance_parameters` — list all parameter overrides on a material instance
- `set_instance_parameters` — set multiple parameters on a material instance in one call
- `set_instance_parent` — reparent a material instance to a different material
- `clear_instance_parameter` — clear a parameter override on a material instance (revert to parent)
- `save_material` — explicitly save a material asset (bypass auto-save)
- `update_custom_hlsl_node` — update the HLSL code or description on a CustomHLSL expression
- `replace_expression` — swap an expression node for a different type, preserving connections
- `get_expression_pin_info` — return pin names, types, and connection state for an expression
- `rename_expression` — rename an expression node's parameter name
- `list_material_instances` — find all material instances derived from a material
- `create_material_function` — create a new UMaterialFunction asset
- `build_function_graph` — build a material function's node graph from a declarative spec
- `get_function_info` — return inputs, outputs, and description of a material function
- `batch_set_material_property` — set a property on multiple materials in one call
- `batch_recompile` — recompile multiple materials in one call
- `import_texture` — import an image file as a UTexture2D asset

**Niagara (+17, 47 → 64)**

- `get_system_summary` — high-level system overview (emitter count, renderer count, param count)
- `get_emitter_summary` — high-level emitter overview (module count, renderer count, sim target)
- `list_emitter_properties` — list all editable UPROPERTY fields on an emitter asset
- `get_module_input_value` — read the current value of a single module input
- `configure_curve_keys` — set the full key list on a curve data interface in one call
- `configure_data_interface` — set multiple properties on a data interface in one call
- `duplicate_system` — duplicate a Niagara system asset to a new path
- `set_fixed_bounds` — set fixed world-space bounds on a Niagara system
- `set_effect_type` — assign an effect type asset to a Niagara system
- `create_emitter` — create a standalone Niagara emitter asset from scratch
- `export_system_spec` — export a system's full spec as JSON (reverse of `create_system_from_spec`)
- `add_dynamic_input` — add a dynamic input module to a module's input slot
- `set_dynamic_input_value` — set an input value on a dynamic input module
- `search_dynamic_inputs` — search available dynamic input scripts by keyword
- `add_event_handler` — add an event handler stage to an emitter
- `validate_system` — validate system for GPU+Light renderer conflicts, missing materials, and bounds warnings
- `add_simulation_stage` — add a simulation stage to a GPU emitter

**Animation (+12, 62 → 74)**

- `get_ikrig_info` — return IKRig asset info: chains, goals, solvers, retarget root
- `add_ik_solver` — add a solver (PBIK, TwoBone, etc.) to an IKRig
- `get_retargeter_info` — return IK Retargeter asset info: source/target rigs, chain mappings
- `set_retarget_chain_mapping` — set or update a chain mapping on an IK Retargeter
- `get_control_rig_info` — return Control Rig hierarchy: bones, controls, nulls, curves
- `get_control_rig_variables` — list variables on a Control Rig Blueprint
- `add_control_rig_element` — add a bone, control, or null to a Control Rig hierarchy
- `get_abp_variables` — list variables defined in an Animation Blueprint
- `get_abp_linked_assets` — list assets linked to an Animation Blueprint (skeletons, rigs, etc.)
- `add_state_to_machine` — add a new state to an AnimBP state machine
- `add_transition` — add a transition between two states in a state machine
- `set_transition_rule` — set the condition expression on a state machine transition

### Fixed

**Blueprint (21 fixes)**

- 5 crash fixes: null graph reference, invalid pin access on removed nodes, blueprint-not-compiled guard, interface scaffold on abstract classes, cast node creation with missing target class
- 7 logic bugs: `get_functions` missing latent function flags, `find_nodes_by_class` incorrect prefix handling, `connect_pins` direction mismatch silent failure, `remove_node` orphaned connections, `get_event_dispatchers` missing param types, `validate_blueprint` false-positive on native interfaces, `get_graph_data` stale node references after compile
- 9 UX improvements: clearer error messages for invalid pin names, node class alias expansion in `add_node`, bulk op partial-success reporting, better param validation messages, schema enrichment for all 20 new actions

**Material (11 fixes)**

- 3 bugs: `build_function_graph` node class resolution for function-context expressions, `connect_expressions` direction detection when both nodes have same-named pins, `get_material_parameters` missing static switch params on instanced materials
- 3 UX: `list_material_instances` now recurses through instance chains, `get_compilation_stats` includes VS/PS instruction counts, `set_instance_parameter` accepts both short and full parameter names
- 5 minor: null-safety guards in expression walker, consistent use of `PostEditChangeProperty` across all write actions, `save_material` marks package dirty before save, `import_texture` sets sRGB correctly for normal maps, `batch_recompile` returns per-asset results

**Niagara (16 fixes)**

- 2 crash fixes: `configure_data_interface` null DI reference on freshly-created emitters, `add_event_handler` accessing uninitialized event receiver
- 5 bugs: `get_module_input_value` mismatched override vs default value for bound inputs, `set_dynamic_input_value` namespace aliasing for dynamic input params, `validate_system` false-positive on CPU+Light (only GPU+Light is invalid), `export_system_spec` missing user parameter defaults, `add_simulation_stage` not calling `RebuildEmitterNodes` after add
- 9 UX: `get_system_summary` includes compile status, `list_emitter_properties` groups by category, `configure_curve_keys` validates key ordering, `duplicate_system` deep-copies override table, consistent emitter param naming across all actions, `set_fixed_bounds` validates axis order, `search_dynamic_inputs` supports multi-word queries, `get_emitter_summary` includes module names, `add_event_handler` returns handler index

**Animation (12 fixes)**

- 1 crash fix: `add_ik_solver` null pointer when IKRig asset has no chain defined yet
- 6 bugs: `get_ikrig_info` missing retarget root bone, `set_retarget_chain_mapping` overwrote existing mappings instead of merging, `get_control_rig_info` excluded null-type elements, `get_abp_linked_assets` missed indirect skeleton links via pose asset references, `add_state_to_machine` duplicate state name collision not detected, `set_transition_rule` lost existing conditions on complex rule expressions
- 5 UX: `get_ikrig_info` now includes goal offsets and weight settings, `get_retargeter_info` includes auto-map status per chain, `add_transition` accepts both state names and state indices, `get_abp_variables` includes type info and default values, `add_control_rig_element` returns new element's full path

### Changed

- **Blueprint** — Action count 47 → 67
- **Material** — Action count 25 → 47
- **Niagara** — Action count 47 → 64
- **Animation** — Action count 62 → 74
- **Total** — Action count 220 → 290

## [0.8.0] - 2026-03-15

Native C++ source indexer, marketplace plugin content indexing, CDO property reader, and project C++ source indexing. Community PRs from NRG-Nad. 219 → 220 actions total.

### Added

**Source — Native C++ indexer (replaces Python/tree-sitter)**

- **MonolithSource** — Completely rewrote the source indexer in native C++ (4,119 lines). Eliminates the Python/tree-sitter dependency entirely — engine source indexing now works out of the box with no Python install. Two indexing modes: full (entire engine source tree) and incremental (project C++ source only, much faster).
- **MonolithSource** — New `MonolithQueryCommandlet` for offline source queries from the command line, without launching the full editor.
- **MonolithSource** — New `trigger_project_reindex` action: triggers an incremental re-index of project C++ source from within an MCP session. **220 total actions.**

**Index — Marketplace plugin content**

- **MonolithIndex** — Auto-discovers installed marketplace and Fab plugins via `IPluginManager` and indexes their content alongside project assets. Opt out per-plugin or globally with the new `bIndexMarketplacePlugins` toggle in plugin settings.

**Index — Configurable content paths**

- **MonolithIndex** — `AdditionalContentPaths` setting: add arbitrary content paths (e.g. external asset packs, shared libraries) to the project index. `GetIndexedContentPaths()` and `IsIndexedContentPath()` helpers available for tools that need path-aware filtering.

**Blueprint — CDO property reader (#5)**

- **MonolithBlueprint** — New `get_cdo_properties` action: reads `UPROPERTY` defaults from any Blueprint CDO or `UObject` asset. Works on any class with a valid CDO. Credit: **NRG-Nad** ([#5](https://github.com/tumourlove/monolith/pull/5)).
- **MonolithIndex** — New `FDataAssetIndexer`: deep-indexes DataAsset subclasses. 15 registered indexers total (up from 14). `bIndexDataAssets` toggle in plugin settings. Credit: **NRG-Nad** ([#5](https://github.com/tumourlove/monolith/pull/5)).

**Source — Project C++ source indexing (#6)**

- **MonolithSource** — `Scripts/index_project.py`: indexes project C++ source into `EngineSource.db` alongside engine symbols, enabling `find_callers`/`find_callees`/`get_class_hierarchy` across project code. Incremental pipeline with `_finalize()` and `load_existing_symbols()` — only changed files are reprocessed. Source DB grows from ~1.8 GB (engine only) to ~3.4 GB with a full project. Credit: **NRG-Nad** ([#6](https://github.com/tumourlove/monolith/pull/6)).

### Fixed

- **MonolithSource** — Improved error handling and recovery throughout the source indexer pipeline.
- **MonolithNiagara** — Resolved 5 bugs in DI handling, static switch inputs, SimTarget changes, and renderer class naming.

### Changed

- **MonolithSource** — Source indexer no longer requires Python. The C++ indexer runs natively inside the editor on startup. Python (`index_project.py`) is still available for project C++ source indexing as a separate optional step.
- **MonolithBlueprint** — Action count 46 → 47 (`get_cdo_properties`).
- **Total** — Action count 219 → 220 (`trigger_project_reindex`).

## [0.7.3] - 2026-03-15

Blueprint module fully realized (6 → 46 actions). Niagara HLSL module creation implemented. Major Niagara, Material, and MCP reliability fixes across all modules. 217 → 218 actions total.

### Added

**Blueprint — Write Actions (40 new)**

- **Blueprint — Variable CRUD (7):** `add_variable`, `remove_variable`, `set_variable_default`, `set_variable_type`, `set_variable_flags` (exposed, editable, replicated, transient), `rename_variable`, `get_variable_details`
- **Blueprint — Component CRUD (6):** `add_component`, `remove_component`, `set_component_property`, `get_components`, `get_component_details`, `reparent_component`
- **Blueprint — Graph Management (9):** `add_function_graph`, `remove_function_graph`, `add_macro_graph`, `remove_macro_graph`, `add_event_graph`, `remove_event_graph`, `get_functions`, `get_event_dispatchers`, `get_construction_script`
- **Blueprint — Node & Pin Operations (6):** `add_node`, `remove_node`, `connect_pins`, `disconnect_pins`, `get_pin_info`, `find_nodes_by_class`
- **Blueprint — Compile & Create (5):** `compile_blueprint`, `create_blueprint`, `reparent_blueprint`, `add_interface`, `remove_interface`
- **Blueprint — Read Actions expanded (4 new):** `get_parent_class`, `get_interfaces`, `get_construction_script` (graph data), `get_component_details`

**Blueprint — `add_node` usability:**
- Common node class aliases (`CallFunction`, `VariableGet`, `VariableSet`, `Branch`, `Sequence`, `ForEach`) resolve without full K2Node_ prefix
- Automatic `K2_` prefix fallback when a bare function name doesn't resolve

**Niagara — HLSL module authoring (2 new):**

- **Niagara** — `create_module_from_hlsl`: creates a standalone `UNiagaraScript` asset (module usage) with a CustomHlsl node, typed ParameterMap I/O pins, and user-defined input/output pin declarations. Supports CPU and GPU sim targets. Inputs are exposed as overridable parameters on the FunctionCall — `get_module_inputs` and `set_module_input_value` work on the result.
- **Niagara** — `create_function_from_hlsl`: same as above in function usage context — for reusable HLSL logic called from other modules. Direct typed pin wiring (no ParameterMap wrapper).
- **Niagara** — Dot validation for I/O pin names: dotted names (e.g. `Module.Color`) are now rejected with a clear error at creation time, with usage-specific guidance (modules: write via ParameterMap in HLSL body; functions: use bare names). Prevents cryptic HLSL compiler errors downstream.

**Niagara — System controls (2 new):**

- **Niagara** — `set_system_property`: sets a system-level property (e.g. `WarmupTime`, `bDeterminism`) via UE reflection. No hardcoded property list — any `UPROPERTY` on `UNiagaraSystem` is settable.
- **Niagara** — `set_static_switch_value`: sets a static switch input value on a Niagara module. Static switches control compile-time code paths in the Niagara module stack.

**Niagara — Discovery (2 new):**

- **Niagara** — `list_module_scripts`: searches available Niagara module scripts by keyword. Returns matching asset paths — useful for finding engine modules to add via `add_module`.
- **Niagara** — `list_renderer_properties`: lists editable UPROPERTY fields on a renderer via reflection. Params: `asset_path`, `emitter`, `renderer`. Returns property names, types, and current values.

**Niagara — Diagnostics (1 new):**

- **Niagara** — `get_system_diagnostics`: returns compile errors, warnings, renderer/SimTarget incompatibility flags, GPU + dynamic bounds warnings, and per-script stats (op count, register count, compile status). Also exposed `CalculateBoundsMode` in `set_emitter_property`.

**MCP — Client usability:**

- **MCP** — `tools/list` now embeds per-action param schemas for all actions at session start. AI clients see full param documentation (names, types, required/optional) without calling `monolith_discover()` first.
- **MCP** — Registry-level required param validation: missing required params return a clear error listing which params were provided vs which are required, before the handler is even called.

**Offline CLI:**

- **Core** — `Saved/monolith_offline.py`: pure Python (stdlib, zero deps) read-only CLI that queries `EngineSource.db` and `ProjectIndex.db` directly when the editor is not running. 14 actions across 2 namespaces: `source` (9 actions, mirrors `source_query`) and `project` (5 actions, mirrors `project_query`). Fallback for when MCP/editor is unavailable.

### Fixed

**Niagara — Emitter lifecycle:**

- **Niagara** — `create_system` + `add_emitter`: emitters added via `add_emitter` did not persist in the saved asset. Fixed by replacing raw `System->AddEmitterHandle()` with `FNiagaraEditorUtilities::AddEmitterToSystem()`, which calls `RebuildEmitterNodes()` + `SynchronizeOverviewGraphWithSystem()`. `SavePackage` now called in both `HandleCreateSystem` and `HandleAddEmitter`.
- **Niagara** — `create_system_from_spec`: was failing with `failed_steps:1` on any spec with modules. Fixed by adding synchronous `RequestCompile(true)` + `WaitForCompilationComplete()` after each emitter add in the spec flow, before module operations begin. Failed sub-operations now report in an `"errors"` array instead of silently incrementing a counter.
- **Niagara** — `set_emitter_property` SimTarget change caused "Data missing please force a recompile" in the editor. Raw field assignment on `SimTarget` bypassed `MarkNotSynchronized`, so `RequestCompile(false)` saw an unchanged hash and skipped compilation. Fixed: now calls `PostEditChangeVersionedProperty` + `RebuildEmitterNodes` + `SynchronizeOverviewGraphWithSystem` after `SimTarget`, `bLocalSpace`, and `bDeterminism` changes.
- **Niagara** — `list_emitters` was missing the emitter GUID in its output. Added `"id": Handle.GetId().ToString()` — provides a stable round-trip token for subsequent operations.

**Niagara — Parameter namespace correctness:**

- **Niagara** — `set_module_input_value` and `set_module_input_binding` were passing the stripped short name to `FNiagaraParameterHandle::CreateAliasedModuleParameterHandle` instead of the full `Module.`-prefixed name from `In.GetName()`. This caused namespace warnings on every subsequent Niagara compile. Both actions now use the full name.
- **Niagara** — `FindEmitterHandleIndex` accepted numeric string indices (`"0"`, `"1"`) as a last-resort fallback. `list_emitters` returns `"index"` for each emitter — this lets you pass that index directly instead of having to remember the emitter name.

**Niagara — Module input coverage:**

- **Niagara** — `get_module_inputs` and `set_module_input_value` now work with CustomHlsl modules. When `GetStackFunctionInputs` returns empty (no `Module.`-prefixed map entries, as is the case for CustomHlsl scripts), both actions fall back to reading typed pins directly from the FunctionCall node.
- **Niagara** — `get_module_inputs` now returns actual `FRichCurve` key data for DataInterface curve inputs, instead of just the DI class name.
- **Niagara** — `get_module_inputs` now correctly deserializes `LinearColor` and vector default values from their string-serialized JSON fallback. Previously returned zeroed values for these types.
- **Niagara** — `set_module_input_di` and `get_di_functions` now auto-resolve DI class names — both `NiagaraDataInterfaceCurve` and `UNiagaraDataInterfaceCurve` are accepted (U prefix stripped/added as needed).

**Niagara — Renderer:**

- **Niagara** — `list_renderers` now returns the short renderer class name in the `type` field (e.g. `SpriteRenderer`) instead of the full UClass path.

**Material — Editor integration:**

- **Material** — `set_expression_property` was calling `PostEditChange()` with no arguments, which didn't trigger `MaterialGraph->RebuildGraph()`. Now calls `PostEditChangeProperty(FPropertyChangedEvent(Prop))` with the actual property — changes reflect in the editor display without a manual recompile.
- **Material** — `build_material_graph` now auto-recompiles on success. Response includes `"recompiled": true`.
- **Material** — `delete_expression`, `connect_expressions`, and `disconnect_expression` now wrap operations in `PreEditChange`/`PostEditChange` for correct undo history and editor update.
- **Material** — `set_material_property`, `create_material`, `delete_expression`, and `connect_expressions` now call `Mat->PreEditChange(nullptr)` + `Mat->PostEditChange()` to push changes through the material graph system.
- **Material** — `disconnect_expression`: added optional `input_name`/`output_name` params for targeted disconnection. Previously always disconnected all connections on the expression — now supports disconnecting a specific pin pair while leaving others intact.

**Blueprint:**

- **Blueprint** — `add_node` now resolves common node class aliases (`CallFunction`, `VariableGet`, `VariableSet`, `Branch`, `Sequence`, `ForEach`) and automatically tries the `K2_` prefix for function call nodes when the bare name doesn't resolve. Previously failed with a class-not-found error on all common node types.

**Core — Asset loading:**

- **Core** — `LoadAssetByPath` now queries `IAssetRegistry::GetAssetByObjectPath()` + `FAssetData::GetAsset()` first, falling back to `StaticLoadObject` only if the Asset Registry has no record. Prevents stale `RF_Standalone` ghost objects from shadowing assets that were deleted and recreated in the same editor session.

### Changed

- **Blueprint** — Action count 6 → 46. Module refactored from one file into six focused source files: Actions (core read), Variables, Components, Graph, Nodes, Compile.
- **Niagara** — Action count 41 → 47. Added `set_system_property`, `set_static_switch_value`, `list_module_scripts`, `list_renderer_properties`, `get_system_diagnostics`, `create_module_from_hlsl`, `create_function_from_hlsl`. Param aliases added (`module_node`/`module_name`/`module`, `input`/`input_name`, `property`/`property_name`, `class`/`renderer_class`/`renderer_type`).
- **Total** — Action count 177 → 218

## [0.7.2] - 2026-03-13

### Fixed

- **Niagara** — `set_module_input_value`, `set_module_input_binding`, and `set_curve_value` silently defaulted to `GetFloatDef()` when input name didn't match any module input, creating orphaned override entries in the parameter map that cannot be removed. Now returns an error with the list of valid input names. Common trigger: CamelCase names vs spaced names (e.g. `LifetimeMin` vs `Lifetime Min`). (Thanks [@playtabegg](https://github.com/playtabegg) — [#2](https://github.com/tumourlove/monolith/pull/2))

## [0.7.1] - 2026-03-11

Niagara write testing: all 41 actions verified. 12 bugs found and fixed, plus a major improvement to `get_module_inputs`.

### Fixed

- **CRASH: Niagara** — `GetAssetPath` infinite recursion: fallback called itself instead of reading `system_path`. Crashed `create_system_from_spec` and any action using `system_path` param
- **CRASH: Niagara** — `HandleCreateSystem` used raw `NewObject<UNiagaraSystem>` without initialization. `AddEmitterHandle` crashed with array-out-of-bounds on the uninitialized system. Fixed: calls `UNiagaraSystemFactoryNew::InitializeSystem()` after creation
- **CRASH: Niagara** — `HandleAddEmitter` crashed when emitter asset had no versions. Added version count guard before `AddEmitterHandle`
- **CRASH: Niagara** — `set_module_input_di` crashed with assertion `OverridePin.LinkedTo.Num() == 0` when pin already had links. Added `BreakAllPinLinks()` guard before `SetDataInterfaceValueForFunctionInput`
- **Niagara** — `set_module_input_di` accepted nonexistent input names silently. Now validates input exists using full engine input enumeration
- **Niagara** — `set_module_input_di` accepted non-DataInterface types (e.g. setting a curve DI on a Vector3f input). Now validates `IsDataInterface()` on the input type
- **Niagara** — `set_module_input_di` `config` param was parsed as string, not JSON object. Now accepts both JSON object (correct) and string (legacy)
- **Niagara** — `get_module_inputs` only returned static switch pins from the FunctionCall node. Now uses `FNiagaraStackGraphUtilities::GetStackFunctionInputs` with `FCompileConstantResolver` to return ALL inputs (floats, vectors, colors, DIs, enums, bools, positions, quaternions)
- **Niagara** — `GetStackFunctionInputOverridePin` helper only searched FunctionCall node pins. Now also walks upstream to ParameterMapSet override node (mirrors engine logic), so `has_override` correctly detects data input overrides
- **Niagara** — `get_module_inputs` returned `Module.`-prefixed names (e.g. `Module.Gravity`). Now strips prefix for consistency with write actions. Write actions accept both short and prefixed names
- **Niagara** — `batch_execute` dispatch table was missing 8 write ops: `remove_user_parameter`, `set_parameter_default`, `set_module_input_di`, `set_curve_value`, `reorder_emitters`, `duplicate_emitter`, `set_renderer_binding`, `request_compile`
- **Niagara** — `FindEmitterHandleIndex` auto-selected the only emitter even when a specific non-matching name was passed. Now only auto-selects when caller passes an empty string
- **Niagara** — `set_module_input_value` and `set_curve_value` didn't break existing pin links before setting literal values. Added `BreakAllPinLinks()` guard so literal values take effect when overriding a previous binding

## [0.7.0] - 2026-03-10

Animation Wave 2: 44 new actions across animation and PoseSearch, bringing the module from 23 to 67 actions and the plugin total to 177.

### Added

- **Animation — Curve Operations (7):** `get_curves`, `add_curve`, `remove_curve`, `set_curve_keys`, `get_curve_keys`, `rename_curve`, `get_curve_data`
- **Animation — Bone Track Inspection (3):** `get_bone_tracks`, `get_bone_track_data`, `get_animation_statistics`
- **Animation — Sync Markers (3):** `get_sync_markers`, `add_sync_marker`, `remove_sync_marker`
- **Animation — Root Motion (2):** `get_root_motion_info`, `extract_root_motion`
- **Animation — Compression (2):** `get_compression_settings`, `apply_compression`
- **Animation — BlendSpace Operations (5):** `get_blendspace_info`, `add_blendspace_sample`, `remove_blendspace_sample`, `set_blendspace_axis`, `get_blendspace_samples`
- **Animation — AnimBP Inspection (5):** `get_anim_blueprint_info`, `get_state_machines`, `get_state_info`, `get_transitions`, `get_anim_graph_nodes`
- **Animation — Montage Operations (5):** `get_montage_info`, `add_montage_section`, `delete_montage_section`, `set_montage_section_link`, `get_montage_slots`
- **Animation — Skeleton Operations (5):** `get_skeleton_info`, `add_virtual_bone`, `remove_virtual_bones`, `get_socket_info`, `add_socket`
- **Animation — Batch & Modifiers (2):** `batch_get_animation_info`, `run_animation_modifier`
- **Animation — PoseSearch (5):** `get_pose_search_schema`, `get_pose_search_database`, `add_database_sequence`, `remove_database_sequence`, `get_database_stats`

### Fixed

- **Animation** — `get_transitions` cast fix: uses `UAnimStateNodeBase` with conduit support, adds `from_type`/`to_type`
- **Animation** — State machine names stripped of `\n` suffix
- **Animation** — `get_state_info` now validates required params (`machine_name`, `state_name`)
- **Animation** — State machine matching changed from fuzzy `Contains()` to exact match
- **Animation** — `get_nodes` now accepts optional `graph_name` filter

### Changed

- **Animation** — Action count 23 → 67 (62 animation + 5 PoseSearch)
- **Total** — Action count 133 → 177

## [0.6.1] - 2026-03-10

MCP tool discovery fix — tools now register natively in Claude Code's ToolSearch.

### Fixed

- **MCP** — Tool names changed from dot notation (`material.query`) to underscore (`material_query`). Dots in tool names broke Claude Code's `mcp__server__tool` name mapping, causing silent registration failure. Legacy `.query` names still accepted for backwards compatibility via curl.
- **MCP** — Protocol version negotiation: server now echoes back the client's requested version (`2024-11-05` or `2025-03-26`) instead of always returning `2025-03-26`.

### Changed

- **Docs** — All documentation, skills, wiki, templates, and CLAUDE.md updated to use underscore tool naming.

## [0.6.0] - 2026-03-10

Material Wave 2: Full material CRUD coverage with 11 new write actions. Critical updater fix.

### Added

- **Material** — `create_material` action: create UMaterial at path with configurable defaults (Opaque/DefaultLit/Surface)
- **Material** — `create_material_instance` action: create UMaterialInstanceConstant from parent with parameter overrides
- **Material** — `set_material_property` action: set blend_mode, shading_model, two_sided, etc. via UMaterialEditingLibrary
- **Material** — `delete_expression` action: delete expression node by name from material graph
- **Material** — `get_material_parameters` action: list scalar/vector/texture/static_switch params with values (works on UMaterial and MIC)
- **Material** — `set_instance_parameter` action: set parameters on material instances (scalar, vector, texture, static switch)
- **Material** — `recompile_material` action: force material recompile
- **Material** — `duplicate_material` action: duplicate material to new asset path
- **Material** — `get_compilation_stats` action: sampler count, texture estimates, UV scalars, blend mode, expression count
- **Material** — `set_expression_property` action: set properties on expression nodes (e.g., DefaultValue)
- **Material** — `connect_expressions` action: wire expression outputs to inputs or material property inputs

### Fixed

- **Material** — `build_material_graph` class lookup: `FindObject<UClass>` → `FindFirstObject<UClass>` with U-prefix fallback. Short names like "Constant" now resolve correctly
- **Material** — `disconnect_expression` now disconnects material output pins (was only checking expr→expr, missing expr→material property)
- **CRITICAL: Auto-Updater** — Hot-swap script was deleting `Saved/` directory (containing EngineSource.db 1.8GB and ProjectIndex.db). Fixed: swap script and C++ template now preserve `Saved/` alongside `.git`

### Changed

- **Material** — Action count 14 → 25
- **Total** — Action count 122 → 133

## [0.5.2] - 2026-03-09

Wave 2: Blueprint expansion, Material export controls, Niagara HLSL auto-compile, and discover param schemas.

### Added

- **Blueprint** — `get_graph_summary` action: lightweight graph overview (id/class/title + exec connections only, ~10KB vs 172KB)
- **Blueprint** — `get_graph_data` now accepts optional `node_class_filter` param
- **Material** — `export_material_graph` now accepts `include_properties` (bool) and `include_positions` (bool) params
- **Material** — `get_thumbnail` now accepts `save_to_file` (bool) param
- **All** — Per-action param schemas in `monolith_discover()` output — all 122 actions now self-document their params

### Fixed

- **Blueprint** — `get_variables` now reads default values from CDO (was always empty)
- **Blueprint** — BlueprintIndexer CDO fix — same default value extraction applied to indexer
- **Niagara** — `get_compiled_gpu_hlsl` auto-compiles system if HLSL not available
- **Niagara** — `User.` prefix now stripped in `get_parameter_value`, `trace_parameter_binding`, `remove_user_parameter`, `set_parameter_default`

### Changed

- **Blueprint** — Action count 5 -> 6
- **Total** — Action count 121 -> 122

## [0.5.1] - 2026-03-09

Indexer reliability, Niagara usability, and Animation accuracy fixes.

### Fixed

- **Indexer** — Auto-index deferred to `IAssetRegistry::OnFilesLoaded()` — was running too early, only indexing 193 of 9560 assets
- **Indexer** — Sanity check: if fewer than 500 assets indexed, skip writing `last_full_index` so next launch retries
- **Indexer** — `bIsIndexing` reset in `Deinitialize()` to prevent stuck flag across editor sessions
- **Indexer** — Index DB changed from WAL to DELETE journal mode
- **Niagara** — `trace_parameter_binding` missing OR fallback for `User.` prefix
- **Niagara** — `get_di_functions` reversed class name pattern — now tries `UNiagaraDataInterface<Name>`
- **Niagara** — `batch_execute` had 3 op name mismatches — old names kept as aliases
- **Animation** — State machine names stripped of `\n` suffix (clean names like "InAir" instead of "InAir\nState Machine")
- **Animation** — `get_state_info` now validates required params (`machine_name`, `state_name`)
- **Animation** — State machine matching changed from fuzzy `Contains()` to exact match

### Added

- **Niagara** — `list_emitters` action: returns emitter names, index, enabled, sim_target, renderer_count
- **Niagara** — `list_renderers` action: returns renderer class, index, enabled, material
- **Niagara** — All actions now accept `asset_path` (preferred) with `system_path` as backward-compat alias
- **Niagara** — `duplicate_emitter` accepts `emitter` as alias for `source_emitter`
- **Niagara** — `set_curve_value` accepts `module_node` as alias for `module`
- **Animation** — `get_nodes` now accepts optional `graph_name` filter (makes `get_blend_nodes` redundant for filtered queries)

### Changed

- **Niagara** — Action count 39 → 41
- **Total** — Action count 119 → 121

## [0.5.0] - 2026-03-08

Auto-updater rewrite — fixes all swap script failures on Windows.

### Fixed

- **Auto-Updater** — Swap script now polls `tasklist` for `UnrealEditor.exe` instead of a cosmetic 10-second countdown (was launching before editor fully exited)
- **Auto-Updater** — `errorlevel` check after retry rename was unreachable due to cmd.exe resetting `%ERRORLEVEL%` on closing `)` — replaced with `goto` pattern
- **Auto-Updater** — Launcher script now uses outer-double-quote trick for `cmd /c` paths with spaces (e.g. paths under `Documents\Unreal Projects\...`)
- **Auto-Updater** — Switched from `ren` (bare filename only) to `move` (full path support) for plugin folder rename
- **Auto-Updater** — Retry now cleans stale backup before re-attempting rename
- **Auto-Updater** — Rollback on failed xcopy now removes partial destination before restoring backup
- **Auto-Updater** — Added `/h` flag to primary xcopy to include hidden-attribute files
- **Auto-Updater** — Enabled `DelayedExpansion` for correct variable expansion inside `if` blocks

## [0.2.0] - 2026-03-08

Source indexer overhaul and auto-updater improvements.

### Fixed

- **Source Indexer** — UE macros (UCLASS, ENGINE_API, GENERATED_BODY) now stripped before tree-sitter parsing, fixing class hierarchy and inheritance resolution
- **Source Indexer** — Class definitions increased from ~0 to 62,059; inheritance links from ~0 to 37,010
- **Source Indexer** — `read_source members_only` now returns class members correctly
- **Source Indexer** — `get_class_hierarchy` ancestor traversal now works
- **MonolithSource** — `get_class_hierarchy` accepts both `symbol` and `class_name` params (was inconsistent)

### Added

- **Source Indexer** — UE macro preprocessor (`ue_preprocessor.py`) with balanced-paren stripping for UCLASS/USTRUCT/UENUM/UINTERFACE
- **Source Indexer** — `--clean` flag for `__main__.py` to delete DB before reindexing
- **Source Indexer** — Diagnostic output after indexing (definitions, forward decls, inheritance stats)
- **Auto-Updater** — Release notes now shown in update notification toast and logged to Output Log

### Changed

- **Source Indexer** — `reference_builder.py` now preprocesses source before tree-sitter parsing

### Important

- **You MUST delete your existing source database and reindex** after updating to 0.2.0. The old database has empty class hierarchy data. Delete the `.db` file in your Saved/Monolith/ directory and run the indexer with `--clean`.

## [0.1.0] - 2026-03-07

Initial beta release. One plugin, 9 domains, 119 actions.

### Added

- **MonolithCore** — Embedded Streamable HTTP MCP server with JSON-RPC 2.0 dispatch
- **MonolithCore** — Central tool registry with discovery/dispatch pattern (~14 namespace tools instead of ~117 individual tools)
- **MonolithCore** — Plugin settings via UDeveloperSettings (port, auto-update, module toggles, DB paths)
- **MonolithCore** — Auto-updater via GitHub Releases (download, stage, notify)
- **MonolithCore** — Asset loading with 4-tier fallback (StaticLoadObject -> PackageName.ObjectName -> FindObject+_C -> ForEachObjectWithPackage)
- **MonolithBlueprint** — 6 actions: graph topology, graph summary, variables, execution flow tracing, node search
- **MonolithMaterial** — 14 actions: inspection, graph editing, build/export/import, validation, preview rendering, Custom HLSL nodes
- **MonolithAnimation** — 23 actions: montage sections, blend space samples, ABP graph reading, notify editing, bone tracks, skeleton info
- **MonolithNiagara** — 39 actions: system/emitter management, module stack operations, parameters, renderers, batch execute, declarative system builder
- **MonolithNiagara** — 6 reimplemented NiagaraEditor helpers (Epic APIs not exported)
- **MonolithEditor** — 13 actions: Live Coding build triggers, compile output capture, log ring buffer (10K entries), crash context
- **MonolithConfig** — 6 actions: INI resolution, explain (multi-layer), diff from default, search, section read
- **MonolithIndex** — SQLite FTS5 project indexer with 4 indexers (Blueprint, Material, Generic, Dependency)
- **MonolithIndex** — 5 actions: full-text search, reference tracing, type filtering, stats, asset deep inspection
- **MonolithSource** — Python tree-sitter engine source indexer (C++ and shader parsing)
- **MonolithSource** — 10 actions: source reading, call graphs, class hierarchy, symbol context, module info
- **9 Claude Code skills** — Domain-specific workflow guides for animation, blueprints, build decisions, C++, debugging, materials, Niagara, performance, project search
- **Templates** — `.mcp.json.example` and `CLAUDE.md.example` for quick project setup
- All 9 modules compiling clean on UE 5.7
- **MonolithEditor** — `get_compile_output` action for Live Coding compile result capture with time-windowed error filtering
- **MonolithEditor** — Auto hot-swap on editor exit (stages update, swaps on close)
- **MonolithEditor** — Re-index buttons in Project Settings UI
- **MonolithEditor** — Improved Live Coding integration with compile output capture, time-windowed errors, category filtering
- **unreal-build skill** — Smart build decision-making guide (Live Coding vs full rebuild)
- **Logging** — 80% reduction in Log-level noise across all modules (kept Warnings/Errors, demoted routine logs to Verbose)
- **README** — Complete rewrite with Installation for Dummies walkthrough

### Fixed

- HTTP body null-termination causing malformed JSON-RPC responses
- Niagara graph traversal crash when accessing emitter shared graphs
- Niagara emitter lookup failures — added case-insensitive matching with fallbacks
- Source DB WAL journal mode causing lock contention — switched to DELETE mode
- SQL schema creation with nested BEGIN/END depth tracking for triggers
- Reindex dispatch — switched from `FindFunctionByName` to `StartFullIndex` with UFUNCTION
- Asset loading crash from `FastGetAsset` on background thread — removed unsafe call
- Animation `remove_bone_track` — now uses `RemoveBoneCurve(FName)` per bone with child traversal
- MonolithIndex `last_full_index` — added `WriteMeta()` call, guarded with `!bShouldStop`
- Niagara `move_module` — rewires stack-flow pins only, preserves override inputs
- Editor `get_build_errors` — uses `ELogVerbosity` enum instead of substring matching
- MonolithIndex SQL injection — all 13 insert methods converted to `FSQLitePreparedStatement`
- Animation modules using `LogTemp` instead of `LogMonolith`
- Editor `CachedLogCapture` dangling pointer — added `ClearCachedLogCapture()` in `ShutdownModule`
- MonolithSource vestigial outer module — flattened structure, deleted stub
- Session expiry / reconnection issues — removed session tracking entirely (server is stateless)
- Claude tools failing on first invocation — fixed transport type in `.mcp.json` (`"http"` -> `"streamableHttp"`) and fixed MonolithSource stub not registering actions
