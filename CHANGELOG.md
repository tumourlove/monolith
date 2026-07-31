# Changelog

All notable changes to Monolith will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

## [0.21.3] - 2026-07-26

This release closes out the open pull-request queue. Every fix below was reported or prototyped by a contributor — thanks to **@Thomasbehan**, **@whalemenace**, and **@kunkunGames** for the write-ups, which were detailed enough to reproduce from directly.

### Added

- **`niagara get_module_graph` can emit link topology.** Pass `links: true` and the response gains a top-level `edges` array plus a `pin_id` on every pin, so you can trace branches through static switches or find dead ones without exporting the asset to T3D and parsing `LinkedTo` yourself. Output is unchanged when the flag is absent. Edges are keyed by the `(node_guid, pin_id)` pair, because pin IDs alone are not unique within a graph — copy-paste duplicates them. Thanks **@whalemenace** (#109).

### Fixed

- **`ui set_widget_property` can now set Margin and Vector4 properties.** The `value` parameter was declared as a string, so every documented shape for these types — `[22,14,22,14]`, `{"left":26,...}`, or a bare `20` for a uniform margin — arrived as text and was rejected, while the error message listed those exact shapes as valid. `value` is now typed `any`, and both parsers additionally accept string-encoded and comma-separated forms, since proxies still serialise nested parameters to text. Thanks **@Thomasbehan** (#110).
- **Malformed destination paths are rejected before anything is created.** Seventeen asset-creation actions across `blueprint`, `material`, and `niagara` now validate the destination package path up front, instead of passing input such as `//Game/Foo` down to low-level package APIs. Nothing is created, loaded, or left dirty on a rejected call. In the compound PBR workflow, an invalid material option or an existing-material collision now fails *before* the first texture is imported rather than after. Thanks **@kunkunGames** (#106, #107, #108).
- **Material creation no longer risks overwriting an unindexed asset.** The destination collision probe is an Asset Registry query, which does not see a `.uasset` that is on disk but not yet indexed — during a startup scan, or copied in out of band. Material creation now forces the package load that makes that check authoritative, so existing content is merged rather than clobbered.
- **A spurious error log on every successful asset creation is gone.** Destination probes previously attempted a load that was expected to fail, emitting `LogEditorAssetSubsystem: Error` on the normal path.

### Changed

Three user-visible behaviour changes, all in the direction of failing loudly instead of quietly producing something broken:

- **Destination paths containing characters Unreal forbids in a package name are now errors.** That set is `\ : * ? " < > | ' <space> , . & ! ~ @ #`. Previously such input passed a much weaker check and produced a mangled asset. If a path you rely on is rejected and you believe it is legitimate, please open an issue rather than working around it.
- **Object-path destinations are accepted and normalised.** Because the forbidden set includes `.`, the stricter validation above would otherwise have rejected `/Game/Foo/M_Bar.M_Bar` — which is exactly what you get when you copy a path out of a search result. The redundant suffix is now stripped when it unambiguously names the same asset. A mismatched leaf, a dot inside a folder segment, or more than one dot is still rejected.
- **`blueprint seed_data_asset` with `dry_run: true` now reports a malformed path as an error** rather than returning a dry-run report, since validation runs before the dry-run branch.

### Internal

- Line endings are normalised through `.gitattributes`, so contributions no longer arrive as whole-file CRLF rewrites that bury a small change in a full-file diff.
- Releases are published as a draft and only flipped live once a new gate confirms all three assets are uploaded and every SHA256 marker matches a fresh hash of the artifact. This closes a window in which the auto-updater could see a release whose assets had not finished uploading.
- `CONTRIBUTING.md` now states that UE 5.7 is the compile floor and that changes must build on both 5.7 and 5.8, and a pull-request template checks for it.

## [0.21.2] - 2026-07-22

### Fixed

- **`blueprint` wildcard array pins now resolve.** Tool-created `Array_*` nodes (`MakeArray`, `AddItem`, and friends) now spawn the palette-correct `UK2Node_CallFunction` subclass, so their wildcard pins take a concrete type from the connections you make; the schema-mediated disconnect path resets the wildcard pins back to their neutral state when you unhook them. Thanks **@Alexbeav** (#95).
- **`blueprint` reference-audit blind spots closed.** `list_graphs`, `search_nodes`, `find_variable_references`, and `get_graph_data` now see inside collapsed-composite subgraphs, read input-pin default values, catch string-bound timer callers (`SetTimerByFunctionName` and friends), and account for inherited-scope variables. Nested graphs are classified through a shared helper so every audit action walks the same graph set. Thanks **@Alexbeav** (#96).
- **`blueprint` local-variable data pins now materialize.** `VariableGet` / `VariableSet` nodes for local variables now bind via `SetLocalMember` when the target graph declares a matching local, so the node comes up with its data pin instead of a bare exec-only node. Thanks **@Alexbeav** (#97).
- **`blueprint` enum member variables are now enum-typed.** `VariableGet` / `VariableSet` nodes for enum member variables now use the schema's byte + subobject convention, so their pins carry the enum type and connect to enum consumers (switches, comparisons) directly. Thanks **@Alexbeav** (#98).
- **UE 5.8 / Linux source-build compatibility.** Build fixes for the 5.8 `FJsonObject` shared-string keys and the new `-Werror` sites, plus Reflection Intelligence support for the 5.8 UHT codegen format — replicated properties, RPCs, `OnRep` handlers, interfaces, and parent chains are all parsed again on 5.8. Thanks **@gregorygmwhite** (#99).
- **MetaSound build error on UE 5.8.** Fixed the MetaSound-side 5.8 compile break, and hardened `FJsonObject` key iteration across versions more broadly — GAS, LogicDriver, and ComboGraph bulk-fill plus MetaSound now route key access through the `MonolithKeyToString` helper so the same source builds on 5.7 and 5.8. Reported by **@Matt-Makes** (#100).

### Changed

- **`blueprint` `find_variable_references` now defaults `include_inherited` to `true`** (was `false`). A deletion-safety audit should over-report rather than return a false zero for a variable declared on a parent class — with the old default, auditing a variable you inherited came back empty and made deletion look safe when it wasn't. Pass `include_inherited: false` for the strict own-class-only view. Thanks **@Alexbeav** (#96).

## [0.21.1] - 2026-07-20

### Fixed

- **CRITICAL — auto-updater no longer crashes the editor on Install (Windows).** The integrity check added in v0.14.7 (Issue #38) called `FPlatformMisc::GetSHA256Signature`, which has **no Windows implementation** — the engine's generic fallback is a fatal `checkf` assert, so clicking **Install** on the update notification killed the editor whenever the target release's notes carried a SHA256 marker (#90, #94). The updater now uses a self-contained, portable FIPS 180-4 SHA-256 (`MonolithSha256::Compute`, verified against NIST test vectors), so the integrity check works on every platform. Thanks **@Alexbeav** for the root-cause report (#90) and the fix (#91), and **@SilkroadLabs** for the parallel report (#94).
- **SHA256 release-note markers migrated to a "v2" generation** (`Monolith-SHA256-v2-UE5.7:` / `-v2-UE5.8:` / `Monolith-SHA256-v2:`). Updaters shipped in v0.14.7–v0.21.0 hard-crash on the *old* marker names (see above) — the v2 names are invisible to them, so pre-fix updaters fail safe (fail-closed abort or unverified install) instead of asserting, while fixed updaters verify normally. Release tooling (`make_release.ps1`) emits only the v2 names from now on; the old names must never appear in a release body again.
- **`blueprint` `add_event_dispatcher`** now creates the multicast delegate **member variable** (mirroring the Blueprint editor's own flow), so `CallDelegate` / `AddDelegate` / `RemoveDelegate` can actually bind the dispatcher; `remove_event_dispatcher` removes the variable alongside the signature graph. Thanks **@Alexbeav** (#84).
- **`blueprint` `add_node` / `resolve_node`** no longer crash the editor on `node_type: "SpawnActor"` — the bare name now aliases to `SpawnActorFromClass` instead of instantiating the legacy `UK2Node_SpawnActor` (whose `GetNodeTitle()` null-derefs unconfigured), and generic-fallback nodes report a safe title. Thanks **@Alexbeav** (#85).
- **`blueprint` `add_timeline_track`** registers the new track in the timeline template's display-track list and reconstructs the owning `K2Node_Timeline`, so the track's output pin actually appears and is connectable. Thanks **@Alexbeav** (#86).
- **`blueprint` `batch_execute`** honors a top-level `graph_name` as the default graph for all operations (per-op values still win) instead of silently running every op against the EventGraph. Thanks **@Alexbeav** (#87).
- **`blueprint` `resolve_node`** dry-runs of delegate nodes (`CallDelegate` / `AddDelegate` / `RemoveDelegate` / `ClearDelegate`) now resolve the real delegate signature pins instead of reporting only base pins. Thanks **@Alexbeav** (#92).
- **String-encoded array/object params are now recovered centrally** in the tool registry: MCP clients that serialize complex argument values to JSON strings (Claude Code among them) no longer hit "array is required" errors on actions like `delete_assets`, `save_packages`, `set_function_params`, `set_event_dispatcher_params`. Thanks **@Alexbeav** (#93).

### Added

- **`editor` `load_level` fail-closed guards**: refuses (with the dirty package list) when the current map has unsaved changes that the unattended `LoadLevel` would silently discard — override with `dirty_policy:"discard"`; and refuses (instead of tripping the fatal "World Memory Leaks" assert) when a stale rooted in-memory copy of the target world is resident. Thanks **@Alexbeav** (#89).
- **Paired `MODAL_OPEN` / `MODAL_CLOSE` log telemetry** around blocking Slate modals, with window identity and slow-task flag on UE 5.8+ — log consumers can now distinguish a healthy open→close from a modal still blocking the game thread (and starving the in-process MCP server). Thanks **@Alexbeav** (#88).

## [0.21.0] - 2026-07-19

### Added

Small ergonomics upgrades to the `ui` and `blueprint` action packs, driven by requests on the Ideas board (Discussion #74, thanks @k-s-s). No new actions — existing actions gained aliases, defaults, and smarter resolution.

- **`ui` `add_widget`** — `parent` is now accepted as an alias for `parent_name`, so you can target a non-root panel directly.
- **`ui` `set_widget_property`** — common `UWidget` properties (`Visibility`, `RenderOpacity`, `ToolTipText`, `bIsEnabled`, and `RenderTransform.Angle` / `.Scale` / `.Translation`) are now allowlisted by default, so setting them no longer needs `raw_mode=true`.
- **`ui` `set_brush`** — `property_name` is now optional and auto-resolves the brush property from the widget type (Image → `Brush`, Border → `Background`); `color` is accepted as an alias for `tint_color`.
- **`ui` `set_slot_property`** — grid-slot support: `row`, `column`, `row_span`, and `column_span` now work on `UUniformGridSlot` and `UGridSlot`.
- **`blueprint` `describe_cdo_schema`** — now emits the correct **positional** `TMap` ImportText hint (the old hint implied a keyed form the importer rejects), including the struct literal for struct-valued maps.
- **`blueprint` `add_variable` / `set_variable_type`** — map type strings now accept prefixed key **and** value types (e.g. `map:enum:ESlateVisibility:struct:LinearColor`), so enum-keyed and struct-valued maps are authorable; previously a compound key/value type after `map:` was split on the wrong colon and silently fell back to a `bool` key/value.
- **`blueprint` `connect_pins`** — cross-graph node-ID disambiguation: when the same node ID exists in multiple graphs, the error now points you at `graph_name` instead of connecting the wrong node.
- **`blueprint` `add_node` / `resolve_node` (`K2Node_SwitchEnum`)** — resolves user-defined `UENUM`s by short name, `/Script` path, or unloaded `UserDefinedEnum` asset; adds `enum` / `enum_path` aliases and accepts the `k2node_switchenum` node-class alias.
- **`blueprint` `add_node` / `resolve_node` (`K2Node_CallFunction`)** — resolves Blueprint-defined functions (both `self` and external Blueprints), not just native/engine functions.

> **Note (issue #82, deferred MCP context loading):** already delivered in v0.20.3 by terse-by-default `monolith_discover`, `describe_query` on-demand schemas, and host-side tool deferral — no new work needed. Thanks @aggitti for validating the direction.

### Fixed

- **BlueprintAssist bridge broke against BA 4.9.0+.** `MonolithBABridge` referenced `RequestFormatAll` and `GetNumberOfPendingNodesToCache`, which changed in BlueprintAssist 4.9.0, causing a C2039 for users on the newer BA. Fixed with `__has_include` version detection so both pre-4.9 and 4.9.0+ compile. PR #78 — thanks @tc-imba (parallel fix from @mewliks in #76).
- **GeometryScripting delay-load DLLs now gated to Win64.** The delay-load entries weren't platform-gated, which broke `BuildEnvironment.Unique` and the macOS source link. PR #77 — thanks @itismyfield.
- **UE 5.8 source builds (CL 55116800).** `FJsonObject` keys became `FSharedString` in 5.8; two `MonolithAnimation` call sites now route through a `MonolithKeyToString` shim so the same source compiles on both 5.7 and 5.8. Issue #80 — thanks @Baba-Ramsi (also reported by @dulanw in #79).
- **Linux / clang source builds.** Nested `/*` inside doc comments tripped `-Werror,-Wcomment` at four `MonolithReflectionIntel` sites. Fixed. Issue #83 — thanks @daschatten-tb.
- **Indexer no longer strips `RF_Standalone` from live assets (data-loss guard).** The source indexer could clear `RF_Standalone` on assets that were already loaded and referenced, risking a save that dropped data (guard flag `0x10000002`). Added a load-time residency gate across seven `TryUnloadPackage` sites plus the landscape `CleanupWorld` branch, so the indexer only unloads packages it actually brought in. Issue #81 — thanks @Alexbeav.
- **Optional-plugin detection no longer false-positives on plugins sharing a name prefix** (e.g. `BlueprintRetarget` was wrongly detected as BlueprintAssist, hard-linking an absent module and breaking the user's build). Tightened the disk-presence globs in MonolithBABridge/MonolithGAS/MonolithLogicDriver/MonolithComboGraph from truncated prefixes (`Blueprin*`, `Gameplaya*`, `LogicDri*`, `ComboGra*`) to full plugin names (`BlueprintAssist*`, `GameplayAbilities*`, `LogicDriver*`, `ComboGraph*`); the trailing `*` still matches versioned install folders. Reported by @k-s-s (#66).

## [0.20.3] - 2026-06-20

### Added

- **UE 5.7 + 5.8 cross-engine releases.** Monolith now ships one flat per-engine zip per supported Unreal version: `Monolith-vX.Y.Z-UE5.7.zip` and `Monolith-vX.Y.Z-UE5.8.zip`. Download the zip matching your engine -- the plugin loads with no compiler, and the same source builds and runs on both 5.7 and 5.8.
- **Engine-aware auto-updater.** The updater detects the running engine (compile-time `ENGINE_MINOR_VERSION`) and fetches the matching release asset, verified by a per-engine `Monolith-SHA256-UE5.x:` marker; it fails closed if no build exists for your engine. Updating from a pre-cross-engine release falls back to the legacy single-zip path.

- **Terse-by-default `monolith_discover(namespace)`.** The per-namespace branch now returns each action's name + a one-line description only — the full per-action `params` JSON-Schema is no longer emitted by default. Fetch a single action's schema with `describe_query action_schema` (~54 tokens), or pass `detail=true` (alias `verbose=true`) to inline every action's schema (reproduces the pre-change response shape byte-for-byte). New optional params: `filter` (case-insensitive substring on action name OR full description) and opt-in `offset`/`limit` pagination (`limit=0` = the complete list; no action is hidden by default). Terse responses carry top-level `total` (always), `next_offset` (only when a positive `limit` leaves more), and `schema_hint` (points callers at `describe_query action_schema` / `detail=true`). The one-line description is trimmed to its first sentence, else hard-capped at 150 chars on a word boundary with a trailing `"..."`; the full description is preserved in `detail` mode and via `describe_query action_schema`. The full `discover()` (no namespace) response is unchanged. Measured per-namespace token reduction (terse vs `detail`, same-server baseline):

  | namespace | detail (baseline) | terse | reduction |
  |-----------|-------------------|-------|-----------|
  | blueprint | 24,521 | 4,027 | 83.6% |
  | animation | 36,236 | 6,682 | 81.6% |
  | ui | 20,590 | 4,944 | 76.0% |
  | ai | 20,157 | 5,588 | 72.3% |

- **Surgical nested-path UPROPERTY writer (`blueprint`) — `set_property_at_path`.** Sets a single value deep inside a CDO / UObject asset addressed by a dotted+bracket path — `Standing.Gaits[Jog].Starts.Forward` (`.` = struct member, `[N]` = array index, `[Key]` = map key, the key imported through the map's key grammar so enum names / ints / FName / string keys all resolve). Unlike `set_cdo_properties` (which rebuilds whole arrays/maps from a JSON tree) the write is in place, leaving sibling elements / keys untouched. Leaf values accept scalars, enum names, ImportText struct literals, hard object refs, and `TSoftObjectPtr` asset paths, identical to `set_cdo_property`. The write goes through the engine edit cradle (FProperty reflection, not the editor's edit-flag gate), so it writes `EditDefaultsOnly` DataAsset fields the editor refuses on saved instances. `create_missing_keys` adds an absent map key; `dry_run` resolves + validates without mutating; `save` persists the package to disk. Backed by a new generic `FMonolithReflectionWalker::ResolvePath` path resolver in `MonolithCore`.

- **Retarget pose + op-stack tuning (`animation`) — 8 new actions.** Author the IK Retargeter retarget pose and the per-chain / root / foot-lock op settings, plus per-bone `USkeleton` translation-retargeting, so a retarget can be dialed in rather than left at op-stack defaults.
  - `align_retarget_pose` — auto-align an IK Retargeter's source or target retarget pose via AutoAlign + SnapBoneToGround.
  - `get_retarget_pose` / `set_retarget_pose` — read/edit a retarget pose. `set_retarget_pose` `mode`: `from_reference` (reset to the rig reference pose) or `bone_deltas` (apply per-bone rotation/translation offsets); a `from_animation` mode is deferred.
  - `get_retarget_chain_settings` / `set_retarget_chain_settings` — read/write a retarget chain's FK and IK op-stack settings (rotation/translation modes, IK blend, etc.), written reflectively.
  - `set_retarget_root_settings` — set the Pelvis Motion op settings: vertical scale, floor constraint, and whether root motion affects the IK goals.
  - `enable_foot_ground_lock` — configure the Speed Planting op's foot ground-lock on named IK chains.
  - `set_bone_translation_retargeting` / `get_bone_translation_retargeting` — read/write per-bone `USkeleton` translation-retargeting modes (`Animation` / `Skeleton` / `AnimationScaled` / `AnimationRelative` / `OrientAndScale`); the setter also accepts a `biped_locomotion` preset.

- **Locomotion authoring (`animation`) — 3 new actions.**
  - `get_root_motion_speed` — report a sequence's authored root-motion ground speed in cm/s, with an explicit "unknowable" signal for root-locked or no-root-motion clips instead of a misleading zero.
  - `bake_distance_curve` — bake a `Distance` curve onto a sequence via the engine `DistanceCurveModifier` (removes any existing same-named curve first, then persists the baked curve into the asset's modifier stack).
  - `bind_threadsafe_update_function` — wire a Blueprint-library static call into an AnimBP's `BlueprintThreadSafeUpdateAnimation` graph (v1a: known-signature binding).

- **IK Rig bone settings (`animation`) — 2 new actions.** `set_ik_rig_bone_settings` / `get_ik_rig_bone_settings` — write/read a bone's per-solver IK Rig bone settings, addressed reflectively so solver-specific fields are reachable.

- **Bone transform inspection (`animation`).** `get_animated_bone_transform` — the FK-composed transform of a bone at a given frame or time, in component space or world space.

### Changed

- **`get_retargeter_info` (`animation`)** now emits an `ops[]` array — per retarget op its type plus reflected settings — alongside the existing chain mappings.
- **`apply_anim_modifier` (`animation`)** now accepts a `properties` object (a reflective field set written onto the modifier before it runs) and a `persist` flag (register the modifier into the asset's `AnimationModifiers` stack so it survives reimport, rather than a one-shot run).
- **`get_blend_space_info` (`animation`)** now reports each sample's authored root-motion speed plus the `triangulation_baked` and `interpolate_using_grid` flags.
- **`get_anim_node_pin_bindings` (`animation`)** now also emits wire-linked input pins (`type:"Link"` entries carrying the source node/pin driving the input), so property-access bindings and graph wires on a node are both visible in one read.
- **`derive_foot_sync_markers` (`animation`)** gains a `from_bones` mode that derives foot plants from per-frame foot-bone height plus planar-speed minima, for clips with no markers/notifies/curves but a usable foot-bone track.
- **`get_curve_keys` (`animation`)** now reports `monotonic` and `sign` flags on the returned curve.
- **`set_anim_node_function_binding` (`animation`)** now calls `RequestRefreshExtensions` so the recompile regenerates the anim-subsystem set for the changed binding — without it the changed binding left a null `NodeRelevancy` subsystem at runtime.

### Fixed

## [0.20.2] - 2026-06-15

### Fixed

- **From-source rebuilds still hard-linked optional plugins MonolithAI couldn't load without.** v0.20.1 rebuilt the shipped release binaries clean, but it didn't fix the source path — so anyone who rebuilt Monolith from source on a stock engine kept hitting the same `GetLastError 126` load failure on `MonolithAI.dll`. The root cause: the optional-plugin gates in `MonolithAI`, `MonolithMesh`, `MonolithIndex`, `MonolithAudio`, and `MonolithAnimation` only checked whether a plugin was **present on disk**. Engine plugins like MassSpawner / ZoneGraph are always on disk, so the gates linked them in even when they weren't enabled in the project, and a stock engine doesn't load them at runtime. Those five `Build.cs` files now read the `.uproject` ProjectDescriptor and gate on whether the plugin is actually **enabled in the project**, not merely present. A from-source build with those plugins disabled now produces a `MonolithAI.dll` with zero MassSpawner / ZoneGraph imports; builds that do enable them keep the features. This is the source-side complement to v0.20.1's binary fix. (#71, reported by @aggitti)

### Internal

- Dropped `GameplayAbilities` from the release import-leak sentinel list — it's a hard dep in `Monolith.uplugin` (auto-enable contract guarantees load order), so it's functionally safe to hard-link and was triggering a false-positive ship block.

## [0.20.1] - 2026-06-15

### Fixed

- **MonolithAI failed to load on stock UE 5.7.4 (Epic Launcher) with `GetLastError 126`.** The v0.20.0 release binary hard-linked the optional `MassSpawner` (MassEntity / MassGameplay) and `ZoneGraph` plugin DLLs, which aren't enabled in a stock engine build, so on a clean install the plugin could not load. The release binary is now correctly gated and MonolithAI loads without those experimental plugins. (#71, reported by @aggitti)

### Added

- **`derive_foot_sync_markers` (`animation`).** Auto-derives left/right foot-plant sync markers on an `AnimSequence` from data already in the clip — no manual eyeballing — via a 5-signal availability cascade (first signal that yields plants wins, with `source` + `confidence` reported): existing authored markers, then footstep notifies (foot side from the owning track name, with class-suffix and alternate-by-time fallbacks), then `contact_l`/`contact_r` float curves (mid-threshold rising edge + hysteresis re-arm + stride-period debounce), then a `Phase` sawtooth curve (key extrema, with a 0..1-ramp heuristic fallback), then component-space foot-bone speed minima (a native port of the engine `FootstepAnimEventsModifier` FootBoneSpeed technique, so no per-project modifier-config Blueprint is needed). Project-agnostic: marker names (`left_marker_name`/`right_marker_name`, default `L_Foot`/`R_Foot`), `track_index`, foot bone names (`foot_bones`, else common-name auto-resolve against the skeleton), notify-track patterns, `phase_invert`, and all detection `thresholds` (`contact_mid`, `contact_low`, `speed_threshold`, `sample_rate`, `debounce_fraction`, `ground_threshold`) are overridable. `method` (`auto`/`existing`/`notifies`/`contact`/`phase`/`footspeed`) forces a single signal and errors cleanly if it is unavailable; `clear_existing` (default true) makes re-runs idempotent; `dry_run` reports the derived `left`/`right` times without mutating the asset. Static poses/aim-offsets return an empty result with a `static pose, no plants` note rather than an error.

### Internal

- Hardened the release import-leak smoke check: the `dumpbin` import scan is now mandatory (a release can no longer ship if it cannot run), the optional-plugin sentinel list was completed and corrected, and a drift assertion ties the list to the Build.cs gates -- so this hard-link leak class cannot ship again.

## [0.20.0] - 2026-06-14

### Added

- **Blend space baking + interpolation control (`animation`).** Two new actions for getting blend spaces runtime-correct.
  - `bake_blend_space` — rebuild a blend space's triangulation (`FBlendSpaceData`) via `ResampleData()` and mark it dirty, for blend spaces authored externally or before this release's auto-bake fix. Params: `asset_path`. Returns `has_blendspace_data`, `sample_count`, `baked`, and a `warning` when a 2D blend space has fewer than 3 samples (triangulation needs at least 3). Works on 1D and 2D blend spaces.
  - `set_blend_space_interpolation` — set a blend space's input-interpolation settings, then resample + dirty. `use_grid` toggles `bInterpolateUsingGrid` (true = runtime uses the grid, false = the triangulation); `preferred_triangulation_direction` chooses the edge direction (`None` / `Tangential` / `Radial`). Returns the resulting flags including `has_blendspace_data`. In grid mode the triangulation is intentionally empty, so `has_blendspace_data` is `false` — that is correct, not a failure.

- **State machine editing — remove states, remove transitions, re-point the entry (`animation`).** Three new actions completing the anim-blueprint state-machine authoring surface. Previously you could create states and transitions and set the entry only at creation time; now you can edit a state machine after the fact.
  - `remove_anim_state` — remove a state from a state machine and tear down its inner anim graph. Params: `asset_path`, `machine_name`, `state_name`, and `remove_dependent_transitions` (default `true`, also removes transitions that referenced the state). Refuses to remove the state machine's current entry state — re-point the entry with `set_anim_entry_state` first, then remove.
  - `set_anim_entry_state` — re-point a state machine's Entry node at an existing state. Params: `asset_path`, `machine_name`, `state_name`. Returns the previous entry target, and reports `unchanged` when the named state is already the entry.
  - `remove_anim_transition` — remove the transition from one state to another. Params: `asset_path`, `machine_name`, `from_state`, `to_state`. Reports how many matching transitions were removed.

- **Remove an IK Rig solver (`animation`).** `remove_ik_solver` removes a solver from an IK Rig's solver stack by index. Params: `asset_path`, `solver_index` (0-based). Validates the index against the current solver count — an out-of-range index returns a clear error naming the valid range — and reports `removed_index` and `solver_count_after`.

- **AnimGraph-authoring pack (`animation`) — 14 new actions.** Pose-composition, slot, cached-pose, output-wiring, blend, sync, layered-blend, Control Rig, and linked-layer anim-graph node authoring, composing with the existing `add_anim_graph_node` / `connect_anim_graph_pins` write surface.
  - `add_apply_additive` — add an Apply Additive node (Base / Additive / Alpha pins).
  - `add_apply_mesh_space_additive` — add an Apply Mesh-Space Additive node.
  - `add_slot_node` — add a Slot node; validates the slot name against the skeleton's slot groups before spawning.
  - `add_save_cached_pose` — add a Save Cached Pose node (`cache_name`).
  - `add_use_cached_pose` — add a Use Cached Pose node (`cache_name`; pairs by name with the matching save).
  - `set_output_pose_source` — wire a node's pose output into the anim graph's Output / Root result pin.
  - `set_state_result_source` — wire a node's pose output into a state machine state's result pin.
  - `add_blend_by_int` — add a Blend Poses by Int node grown to `num_poses` pose pins.
  - `add_blend_by_enum` — add a Blend Poses by Enum node bound to a `UEnum` (`enum_path`), one pose pin per exposed enumerator plus a Default/else pin; skips the auto `_MAX` sentinel and `Hidden` enumerators. Optional `enumerators` exposes a subset; optional `graph_name` / `state_name` target a specific graph or state. Companion to `add_blend_by_int`.
  - `set_sync_group` — set a player node's sync group name / role / method.
  - `set_layered_blend_bones` — set per-bone branch filters (bone + blend depth) on a Layered Blend Per Bone node.
  - `add_anim_control_rig_node` — add a Control Rig anim-graph node (`control_rig_class`; IO pins regenerate).
  - `add_linked_anim_layer` — add a Linked Anim Layer node (`layer_name`, optional `interface_class`).
  - `add_conduit` — add a conduit node to a state machine (its bound graph is a transition-logic graph, not an anim graph).

- **`set_anim_node_pin_binding` bootstraps the binding object (`animation`).** The action now constructs a node's binding object when it has none, so it works on previously-unbound AnimGraph nodes instead of refusing them.

- **`auto_layout` built-in formatter (`animation`).** A new `formatter:"builtin"` (also the `"auto"` fallback) lays out anim graphs by dependency-aware layering WITHOUT Blueprint Assist, so layout works in release builds where Blueprint Assist is compiled out.

- **`set_transition_rule` gains an `expression` kind (`animation`).** Compound multi-term transition conditions, extending the existing `bool` / `auto` / `compare` kinds: `terms: [{ lhs, op, rhs, abs?, negate? }]` combined with `combine: "and" | "or"`. Each term builds one comparison sub-node — optional per-term `abs` wraps the left-hand side, optional per-term `negate` inverts the term result — and all terms fold through Boolean AND/OR into the transition result. `get_transition_rule` decodes it back.

### Fixed

- **MCP-authored blend spaces played bind/A-pose at runtime (`animation`).** The four blend space mutators — `add_blendspace_sample`, `edit_blendspace_sample`, `delete_blendspace_sample`, `set_blend_space_axis` — mutated samples without rebuilding the triangulation (`FBlendSpaceData`), so a blend space authored over MCP shipped with empty triangulation and evaluated to the bind/reference pose at runtime. The asset-editor preview recomputed the triangulation live, which masked the problem in-editor. Each of the four actions now calls `ResampleData()` after the edit and marks the package dirty, so every sample/axis change re-bakes correctly. Existing broken blend spaces can be repaired in place with the new `bake_blend_space` action.

- **`add_ik_solver` failed to add solvers, Full Body IK especially (`animation`).** The action resolved the solver type through a hardcoded reflected struct path (`/Script/IKRig.FullBodyIKSolver`) that does not resolve in UE 5.7, so the add silently failed. `solver_type` is now resolved against the live solver-struct table the engine registers — a friendly alias (`fullbodyik`/`fbik`, `limb`, `pole`, `bodymover`, `settransform`, `stretchlimb`) or the exact struct name, falling back to a unique substring; an ambiguous value returns the candidate solvers and an unknown value returns the available list. Solvers, including Full Body IK, now add correctly.

## [0.19.0] - 2026-06-13

An LLM-C++-ergonomics release: an eight-action `source` pack so your AI can resolve an include, signature, deprecation, Build.cs deps, header lint, or a UCLASS stub in one round-trip, plus a parser fix that tripled the engine source index. Layered on top: live-PIE introspection + driving (`editor`), anim-node binding read/write and time-series PIE sampling (`animation`), Blueprint variable census + contract reconciliation (`blueprint`), and T3D asset-text export (`project`). Two first-launch crash/load fixes (issue #70, thanks @aggitti) and a ~40% smaller `tools/list` manifest round it out.

> **Action count is approximate.** As of this release Monolith exposes **~1,400+ actions across 25+ in-tree namespaces** (public, in-tree only). Query `monolith_discover()` (its `total_actions` field) for the exact live figure at any moment — docs are kept in the right ballpark, not pinned to an integer.

> **⚠️ Re-run a full engine source index after upgrading.** This release's index gains only populate after one full `source.trigger_reindex` (the editor's "Reindex Engine Source" / `monolith_reindex`): the ~3x larger engine symbol table from allman-brace class/struct indexing (`symbols` rows `301,590 → 967,491`), the new deprecation index (`EngineSource.db` schema v2), and the `build_cs_path` backfill. Existing `EngineSource.db` files enrich on the next full pass — a project-only reindex does NOT cover engine symbols. Until you reindex, the new `source` ergonomics actions (`get_signature`, `verify_symbols`, `check_deprecations`, `find_example_usage`, `get_include_path`, …) operate against the old, smaller table and `check_deprecations` returns `index_state: "empty"`.

### Added

- **LLM C++ authoring ergonomics — Phase 1 (`source`).** Three demand-proven lookups so an agent can resolve an include, a signature, or a deprecation status in one round-trip without reading raw source. All read-only / idempotent and offline-served by `monolith_query.exe` / `monolith_offline.py`.
  - `get_include_path` — canonical `#include` for a symbol, derived from its indexed file path. `Public/` / `Classes/` / `Internal/` prefixes strip to an includable cross-module form (`includable: true`); a `Private/` header reports `includable: false` plus a `warning` and the same-module relative form; engine headers fall back to the basename. Emits the owning module and a `build_cs_note`. A `Class::Method` input resolves the include via the owning class row, so the method need not be an indexed symbol.
  - `get_signature` — exact overload signature(s) for a function/method. Declaration-read is the primary mechanism (resolve via the owning class row + `source_fts`, read the declaration line(s), strip trailing macro `\` and any inline body); the indexed `signature` column is used as a fast path only when present and body-free. The `source` field reports `"declaration_read"` or `"column"`; overloads are returned separately.
  - `check_deprecations` — batch deprecation status for a `symbols[]` list, read from the new `symbol_deprecations` index; per symbol `{deprecated, version, message, kind}`. When the index has no rows it returns `{ index_state: "empty", hint: "run source.trigger_reindex" }` and omits per-symbol verdicts — never a false clean bill.
  - **Deprecation index (schema v2).** `EngineSource.db` schema bumps 1→2, adding a `symbol_deprecations` table populated by the indexer (regex-scan of `UE_DEPRECATED` / `_FORENGINE` / `_FORGAME` + the `DeprecatedFunction` specifier; `symbol_id` is nullable, lookups key on `symbol_name`). The table appears empty on the next index pass after the bump — **engine deprecation rows require one full `source.trigger_reindex`**; the project-only reindex does not cover engine deprecations.
  - **`build_cs_path` backfill.** The previously-empty `modules.build_cs_path` column is now backfilled during the same index pass (`<Module>.Build.cs` derived from each module's `Source/<Module>/` dir); `get_include_path`'s `build_cs_note` reads it. Existing DBs pick up the value on the same full-reindex bootstrap.

- **LLM C++ authoring ergonomics — Phase 2 (`source`).** Three round-trip-compression actions on top of the Phase-1 lookups. All read-only / idempotent.
  - `verify_symbols` — batch pre-flight verdict for a `symbols[]` list. Per symbol composes existence + include + signature + deprecation into one record (via shared C++ helpers, not by re-parsing the Phase-1 JSON handlers). `exists` for a `Class::Method` is decided by the owning class row + a source-line declaration hit (`Name(`), never `symbols`-table presence — engine class-body methods have no symbols row, so a symbols-only check would false-negative most of the engine API. A missing symbol reports `exists: false` with no error. Offline-served.
  - `find_example_usage` — ranked real call-site examples via source-line FTS over `source_fts` (pattern `SymbolName(`; NOT the `references` table, which is empty for engine API methods). Returns ±3 context lines per hit, cursor-paginated via the rerun-slice scheme. `prefer` (`engine` default / `project`) controls rank order: engine `Runtime` first, then other engine, then project (or project-first when `prefer:project`); tie-break by file path then line. Offline-served.
  - `suggest_build_cs_deps` — required Build.cs modules + missing deps for a file (or symbol list). Forward direction of `audit_module_dep_reality`; lives in `MonolithReflectionIntel`, registered onto the `source` namespace. Resolves the declaring module path-first from `file_path` (works on uncommitted/unindexed files), reads its `<Module>.Build.cs` from disk, resolves used types' owning modules via the source index, diffs against declared deps (Core/CoreUObject/Engine/Projects/RHI/RenderCore implicit whitelist), and returns `required_modules[]` + `missing[]`. RI/live-only — not offline-served (matches `audit_module_dep_reality`).

- **LLM C++ authoring ergonomics — Phase 3 (`source` + `editor`).** Pre-flight header lint, deterministic build-error fix hints, and class-stub scaffolding.
  - `lint_header` (`source`) — regex-level UHT-gotcha lint of a single header (`file_path`). **Works on UNINDEXED files** (a header the agent just wrote). Deterministic rule table: missing `GENERATED_BODY()`, `*.generated.h` not last include, `*.generated.h` name mismatch, missing `<MODULE>_API` (module derived PRIMARILY from the file path), `UPROPERTY`/`UFUNCTION` in a non-reflected type, and an invalid-specifier cross-check against the cppreflect `list_class_specifiers` vocabulary (degrades gracefully — rule skipped — when RI is unavailable, e.g. offline). Returns structured findings `{rule_id, line, message, severity}`; a clean header → zero findings. `FRegexMatcher` locals only. Read-only / idempotent, offline-served.
  - `generate_class_stub` (`source`) — UCLASS-derived `.h`/`.cpp` stub pair returned as TEXT (`parent`, `class_name`, `module`). Resolves the parent header + owning module via the source DB; emits `<MODULE>_API`, the parent header include FIRST, `"<Class>.generated.h"` LAST, `GENERATED_BODY()`, and a plain default constructor (the `FObjectInitializer&` overload only when the parent's indexed constructor requires it). **TEXT-RETURN-ONLY — never writes to disk** (no `write_to_disk` / `target_dir` param, no `written_path`). v1 supports UCLASS-derived parents only (USTRUCT/UENUM/UINTERFACE rejected cleanly). Read-only / idempotent, offline-served (pure DB read).
  - `get_build_errors` (`editor`) **`fix_hints[]` — ADDITIVE.** A deterministic error→fix-hint pattern table appends a top-level `fix_hints[]` array (`{error_index, pattern, hint}`) plus a per-error `fix_hint` string. Patterns: `LNK2019` on `Z_Construct_*` (resolves the missing module via the borrowed source DB), `UDeveloperSettings` LNK2019, `C4996` deprecation (attaches the `symbol_deprecations` message), and `generated.h`-must-be-last. Every pre-existing field is byte-identical — purely additive. Borrows `FMonolithSourceDatabase::GetRawHandle()` under `FScopeLock(GetLock())` on the game thread (statement finalized before the lock releases). NOT offline-served — reads the in-process log capture.

- **Blueprint→C++ variable-contract reconciliation (`blueprint`).** Two new actions in `MonolithBlueprintContractActions.cpp` for promoting a Blueprint variable set onto a native parent contract during nativization, without breaking the bindings AnimGraph / chooser / PropertyAccess pins compare against.
  - `compare_class_variable_contract` — pure read/report diff of two classes' variable contracts. Each side is a Blueprint asset path or a native class name. Per-variable descriptor reports base type, container kind (`scalar`/`Array`/`Set`/`Map`), enum subtype (UEnum path), struct subtype (UScriptStruct path), object class, and a `mismatch` classification (`ok`, `missing-on-left`, `missing-on-right`, `type-mismatch`, `container-mismatch`, `enum-subtype-mismatch`, `struct-subtype-mismatch`). For a Blueprint side it sources descriptors from the authoritative `FEdGraphPinType` of `NewVariables`, overlaying the compiled-FProperty walk — this catches enum-subtype mismatches that the KismetCompiler hides by lowering a UserDefinedEnum pin to a plain `FIntProperty`, and BP-local variables that shadow a native parent property (which never materialize as a direct generated-class property). Struct/enum identity is compared by path (BP pins compare struct types by exact `UScriptStruct` pointer identity). Mutates nothing.
  - `promote_variables_to_parent` — reconcile a Blueprint's named local variables against its native parent class (resolved by walking the `ClassGeneratedBy` chain to the first native class). `verify` (default) reports which the parent already satisfies (name+type+container+enum/struct parity) vs which it does not yet declare compatibly; it authors no C++. `remove_shadowed` deletes the now-redundant BP-local duplicate only for variables that pass parity AND are genuinely BP-local member variables — never one the parent lacks or declares incompatibly.

- **Lockstep AnimInstance parity comparison (`animation`).** `sample_pie_anim_instance` gained an optional, additive `compare_to_actor` (+ `compare_component_name`, `tolerance`) that samples a second PIE actor's `AnimInstance` in lockstep and emits a `comparison` block: per-variable delta + a per-type tolerance pass/fail (`exact` for bool/enum/int/name/string/object; `float` epsilon; `vector`/`rotator`/`transform` per-component) and an `overall_pass` roll-up. Per-type tolerances are overridable (`{float, vector, rotator, transform}`, defaults `1e-3`/`1e-2`/`1e-2`/`1e-2`). Backward-compatible — without `compare_to_actor` the single-instance payload is unchanged (no `comparison` key).

- **Anim-node function + pin bindings: read and write (`animation`).** Four new actions surfacing two previously invisible/unwritable binding surfaces on AnimGraph nodes.
  - `get_anim_node_function_bindings` / `set_anim_node_function_binding` — the per-node On Initial Update / On Become Relevant / On Update function slots (the public `FMemberReference` UPROPERTYs). The read reports `function_name`, `member_parent_class`, `is_self_context`, `thread_safe` per slot; omit `node_id` to list every node with any function binding. The write mirrors the engine's own `ValidateFunctionRef`: it resolves the `UFunction`, checks `IsSignatureCompatibleWith` against the node's `PrototypeFunction` metadata, and HARD REJECTS a non-thread-safe function (override with `allow_non_thread_safe=true`) before setting the member, marking the BP modified, and recompiling. Empty `function_name` clears; `function_class` targets an external library class (default: self-member on the AnimBP class).
  - `get_anim_node_pin_bindings` / `set_anim_node_pin_binding` — the per-pin property-access bindings in the node's `UAnimGraphNodeBinding_Base::PropertyBindings` map (a `MinimalAPI`/Private-header class reached via `FProperty` reflection; the value struct `FAnimGraphNodePropertyBinding` is public). The read reports `{pin, path, type, is_bound}` per entry; omit `node_id` to list every node with any pin binding. The write builds the binding struct, replaces it in the reflected map, then calls `ReconstructNode()` (which re-derives the binding pin type via `OnReconstructNode` → `RecalculateBindingType`) before recompiling. Empty `path` clears via the node's public `RemoveBindings`. v1 requires an existing binding object on the node.
  - `get_nodes` now also emits compact, additive `bindings` (function) + `pin_bindings` objects per `UAnimGraphNode_Base` node, omitted when empty — for at-a-glance discovery without the focused readers.

- **Time-series PIE sampling (`animation`).** `sample_pie_timeseries` starts an ASYNC PIE session and returns immediately (`{session_id, status:'running'}`), reusing the existing PIE-smoke session machinery — poll the accumulating series + provocation fire log with `poll_pie_smoke`, force-end with `stop_pie_smoke`. The editor's real frame loop advances PIE while it samples the resolved target's dotted, UDS-friendly `variables` each tick (gated by `sample_interval`, capped by `max_samples`) and fires typed `provocations` once each when session-elapsed crosses their `time`: `set_control_rotation`, `add_movement_input`, `jump` (target must be a Character), and `console_command`. Resolve the target via `actor` (exact label) / `pawn_class` (class substring) / `object_name`, optionally hopping to a `component_name` or the skeletal mesh's anim instance (`anim_instance=true`). Registered under the `animation` namespace but implemented in MonolithEditor (which owns the PIE-session infra; the registry is namespace-string-keyed).

- **Live-PIE object read/call (`editor`).** Two new actions for reading and driving a live PIE object directly.
  - `pie_get_object_properties` — read UPROPERTY values off a resolved PIE object by dotted member path. Resolve the target via `actor_label` / `object_name` / `class_name` (substring), optionally hopping to a `component_name` or the skeletal mesh's active anim instance (`anim_instance=true`). `properties[]` are dotted paths that descend nested structs and match `UUserDefinedStruct` members by their friendly (authored) name — the GUID-suffixed internal fields editor python cannot read — returning each path's JSON value (scalars directly; enums/structs/vectors as export text). Read-only; struct-member traversal only.
  - `pie_call_function` — call a BlueprintCallable function/event on a resolved PIE object, marshalling JSON `args` (keyed by parameter name) into the UFunction parameter frame and invoking via `ProcessEvent`. A struct arg (including a UserDefinedStruct with GUID-suffixed fields) is built from a nested JSON object keyed by friendly field names; v1 supports top-level args + one nested struct level. Requires `FUNC_BlueprintCallable` unless `allow_non_callable=true`, and rejects network-replicated (`FUNC_Net`) and latent functions. Returns out-params + the return value. MUTATES LIVE PIE STATE — this executes real gameplay code.

- **Deterministic PIE input / control driving (`editor`).** Three new actions for scripted, camera-independent PIE driving. **Adds an `EnhancedInput` Build.cs dep to MonolithEditor** (a stock, always-enabled engine plugin module — release-build safe, no `WITH_*` gate).
  - `pie_set_control_rotation` — set the control rotation (`pitch` / `yaw` / `roll`, omitted components default to 0) on a PIE player controller, optional `player_index`. `hold_frames` re-applies each frame for that many frames so it can outlast a per-tick camera/control system; the hold is best-effort, not frame-perfect.
  - `pie_inject_input_action` — inject a value for an Enhanced Input action into a live PIE local player via `UEnhancedInputLocalPlayerSubsystem::InjectInputForAction`, running that action's modifiers and triggers as if real input arrived. `input_action` is a UInputAction asset path or short name; `value` maps to `FInputActionValue` by JSON shape (bool → Boolean, number → Axis1D, array[2] → Axis2D, array[3] → Axis3D); optional `repeat_frames`.
  - `pie_possess_spectator_free` — `enable=true` detaches the controller to a free-fly spectator pawn (stores + unpossesses the current pawn, enters the Spectating state); `enable=false` re-possesses the stored original pawn. Spectator spawning depends on the game mode providing a `SpectatorClass`.

- **Stat-group counter readout (`editor`).** `get_stat_group_values` reads a stats group programmatically into a structured response — enables collection for the named `group_name` (full `STATGROUP_Anim` or short `Anim` form; project-defined groups work the same), reads the most recent settled stat frame(s), and returns each stat's counter value (int64/double) and cycle-stat timing in milliseconds. `sample_frames` (default 1; >1 aggregates per-stat min/avg/max over the window). A group this action enables is disabled again on completion. **`#if STATS`-gated** — compiled into Development editor builds but NOT Shipping/Test; off-gate it returns a clean "stats system not compiled in" error. Reads the live stats stream, so it only produces data while overall collection is active (e.g. PIE running with an on-screen stat, or after `run_console_command("stat <group>")`).

- **Variable reference census (`blueprint`).** `find_variable_references` finds every graph node that reads or writes a Blueprint member variable. Walks all graphs (event graphs, functions, macros, delegate signatures, and interface-implementation graphs) and calls the engine's `UK2Node::ReferencesVariable` against the generated class. Per match, `access` is classified `read` (a VariableGet), `write` (a VariableSet), `read` for a thread-safe Property Access node whose path resolves to the variable, or `other` (transition rules, split-struct pins, etc.). Each entry returns `graph`, `graph_type`, `node_id`, `node_title`, `access`; Property Access matches additionally carry a `property_access` path block, and a `summary` object reports total/reads/writes/other counts. `include_inherited` also matches the variable where it is scoped to a parent class. v1 covers member variables only.

- **First-class asset text (T3D) export (`project`).** `export_asset_text` exports an asset to its native T3D text dump (or grepped excerpts) and returns it directly — the universal escape hatch for surfaces no typed read exposes. PREFER the typed read actions first (`get_node_details` for Blueprint/AnimGraph nodes, `inspect_chooser` for chooser tables, `list_graphs` for graph structure); reach for this only when no typed action exposes what you need. Scope with `object_filter` (name/class substring → a single sub-object) and/or narrow with `grep_pattern` (matching lines plus context), and respect `max_bytes` (default 262144) — a payload over budget hard-errors rather than truncating silently.

### Changed

- **`tools/list` manifest trimmed — duplicated action list removed from dispatcher descriptions (~16k-token / ~41% manifest reduction).** Each domain dispatcher (`source_query`, `material_query`, etc.) previously inlined its full `"Available actions: a, b, c..."` list into the tool `description` field — a ~32k-char copy of names already carried authoritatively by the per-dispatcher `action` enum in the `inputSchema`. The prose join is dropped; the description now points clients at `monolith_discover("<namespace>")` for the action list. The `action` enum is unchanged (still the closed value constraint typed MCP clients validate against), so no dispatch behaviour changes. `tools/list` body drops from ~77.8k bytes to ~46k. Serialisation: `FMonolithHttpServer::HandleToolsList` (`MonolithCore/Private/MonolithHttpServer.cpp`). `.cpp`-only, no schema/ABI change.

- **PropertyAccess node paths now visible (`blueprint` / `animation`).** Blueprint `get_node_details` / `get_graph_data` / `export_graph` (all route through the shared K2 node serializer) and `animation` `get_nodes` (`include_anim_graph` path) now emit an additive `property_access` block on `K2Node_PropertyAccess` nodes: the resolved `path` array + dotted `text_path`, plus `context_id` and the resolved pin type. The class is engine-private (`MinimalAPI`, unlinkable) so the path is read via `FProperty` reflection — mirroring the existing add-side write — and degrades gracefully (`resolved: false` / omitted fields) if the engine layout changes. No new action.

- **Friendly-name / dotted-path struct sampling (`animation`).** `sample_pie_anim_instance` `variables[]` now accept dotted paths (e.g. `CharacterProperties.OrientationIntent`) that descend nested structs and resolve `UUserDefinedStruct` members by their friendly (authored) name via `UUserDefinedStruct::GetAuthoredNameForField`, reading the GUID-suffixed internal fields a flat lookup misses. Plain (non-dotted) names keep working; struct-member traversal only (array/map indexing out of scope). The `compare_to_actor` lockstep path shares the same resolver.

- **Chooser column bindings + cells now visible (`chooser`).** `inspect_chooser` now emits a richer `columns` array (alongside the back-compat `column_types`): per-column `input_binding` chain (the bound context property/function as a `chain` array + dotted `display`) and `is_input`, with per-row cell values gated behind an `include_cells` flag (default off, for payload size). Binding read mirrors the proven authoring write; the four input column types (Bool / Enum / GameplayTag / FloatRange) are handled explicitly, unknown types degrade without erroring. `WITH_CHOOSER`-gated. No new action.

- **Interface-implementation graphs now enumerated (`blueprint`).** `list_graphs` now appends interface-implementation graphs (flagged `graph_type: interface` plus an `interface` field naming the implemented interface), and `get_graph_data` / `export_graph` resolve a graph name against interface graphs too — so an interface function graph is dumpable by name instead of returning "Graph not found". Iterates the already-known `ImplementedInterfaces[].Graphs`. No new action.

### Fixed

- **MonolithMesh first-launch load failure — GeometryScripting DLLs now delay-loaded (`mesh`, issue #70).** On a clean source-tree build with GeometryScripting present, `UnrealEditor-MonolithMesh.dll` carried a load-time hard import on `UnrealEditor-GeometryScriptingCore.dll` (+ `UnrealEditor-GeometryFramework.dll` / `UnrealEditor-GeometryCore.dll`), producing a `CouldNotBeLoadedByOS` (`LoadLibrary` null, `GetLastError=126`) first-build/load race — the module failed to load on first editor launch, with subsequent launches fine. Fix: the three GeometryScripting module DLLs are added to `PublicDelayLoadDLLs` in `MonolithMesh.Build.cs` (inside the existing `bHasGeometryScripting` block, so `MONOLITH_RELEASE_BUILD=1` still excludes them). The Windows loader now binds these imports lazily on first Tier-5 GeometryScript call instead of at module load, so `MonolithMesh.dll` loads even when the dependency is momentarily unresolvable at load time. The full Tier-5 (`WITH_GEOMETRYSCRIPT=1`) mesh-op surface is preserved — `PrivateDependencyModuleNames` is unchanged, nothing is compiled out. The `.uplugin` Plugins array is UNCHANGED, so this adds no new mandatory end-user dependency (the rejected "drop `Optional:true`" alternative would have been a placebo against the diagnosed failure and an issue-#32-class release regression). Verified against the `ModuleRules.PublicDelayLoadDLLs` API (UE 5.7); the authoritative closure check is `dumpbin /imports` showing the three DLLs move from the normal import table to the delay-load import table.
- **Deep indexer no longer asserts on UserDefinedStruct fields with unresolved class/struct/enum types (`source`, issue #70).** `FUserDefinedStructIndexer::IndexAsset` called `FProperty::GetCPPType()` unconditionally while indexing UDS fields. For several property subclasses `GetCPPType()` dereferences the property's inner type pointer with no null guard, so a field whose type can no longer resolve — e.g. a `TSubclassOf<X>` pointing at a deleted Blueprint, which leaves the property's `MetaClass` null — asserted (`check(MetaClass)`, `PropertyClass.cpp:160`) and took the editor down during deep indexing. A file-local `SafeGetCPPType` helper now returns `GetCPPType()` for every well-formed property and a `<unresolved>` placeholder only when the inner pointer the assert would dereference is null. It covers the verified asserting paths: `FObjectProperty`/`FSoftObjectProperty` (`PropertyClass`), `FClassProperty`/`FSoftClassProperty` (`MetaClass` and `PropertyClass`), `FStructProperty` (`Struct`), and `FEnumProperty` (`GetEnum()`); `FByteProperty` already null-guards internally and needs none. Both the JSON `type` field and the indexed-variable `VarType` route through the helper, so a broken field is now indexed as `<unresolved>` instead of crashing the indexer. Behavior is identical for all well-formed properties.
- **Plain-class indexing gap — allman-style classes/structs now indexed (`source`).** `FMonolithCppParser` Phase B (the non-reflected class/struct extractor) required the opening `{` **on the declaration line**, but Epic's coding standard puts the brace on the next line (allman). This structurally excluded `>99%` of exported non-reflected engine types from the `symbols` table — `FCollisionShape`, `FSceneView`, `FPrimitiveSceneProxy`, `FScopeLock`, `FPaths`, `FRegexPattern` and ~19.6K others had no row and no extracted members, so `get_include_path`, `read_source`, `get_class_hierarchy`, `verify_symbols`, `get_signature` and `find_example_usage` all degraded or hard-failed on them. Phase B now mirrors Phase A's brace lookahead: the declaration pattern accepts an allman brace (confirmed by a ≤3-clean-line lookahead requiring the `{` to open a following line) AND today's same-line/inline one-liners (no regression). Forward declarations (`class FFoo;`) and bare multi-line template parameters (`class T` on its own line, guarded by a preceding `<`/`,` check) correctly produce NO row. Indexer-side only — no schema change; existing `EngineSource.db` files enrich on the next full `source.trigger_reindex`, and the offline tools return the richer results automatically. Deltas after the reindex (measured 2026-06-11): `symbols` rows `301,590 → 967,491` (plain classes `1,584 → 40,454`, plain structs `2,754 → 36,832`, functions `77,451 → 411,303` via member extraction of the newly indexed classes); DB file size unchanged at ~7.0 GB (SQLite page reuse); full-reindex wall time ~28.5 min, comparable to prior full passes. The fix also hardened the member extractor against two latent crash classes it exposed (an unterminated-brace-at-EOF out-of-bounds read and a use-after-free when the symbol array reallocates mid-extraction — both previously reachable from Phase A as well).

## [0.18.1] - 2026-06-07

A motion-matching-focused release: a from-scratch Motion Matching authoring pack across the `animation`, `chooser`, and `blueprint` namespaces, plus a PIE / profiling harness, thread-safe AnimBP authoring primitives, and a batch of AI / editor / blueprint additions.

> **Action count is now approximate.** The surface is too large to track to the unit. As of this release Monolith exposes **1,400+ actions across 25+ in-tree namespaces** (public, in-tree only). Query `monolith_discover()` (its `total_actions` field) for the exact live figure at any moment — docs are kept in the right ballpark, not pinned to an integer.

### Added

- **Motion Matching authoring pack (headline).** A from-scratch path to a functional Motion Matching setup — Pose Search, choosers, the AnimBP graph, and the character — all authored over MCP, no hand-editing required.
  - **Pose Search schema + database primitives (`animation`):** `create_normalization_set`, `add_database_to_normalization_set`, `set_database_normalization_set`, `add_database_entry` (any `UAnimationAsset` via the non-deprecated 5.7 overload), `set_database_entry_tags` (per-entry mirror / reselection / enabled flags), `configure_schema_channel` (Trajectory sample offset/flags/weight or Pose sampled bones), `derive_schema_channels_from_skeleton` (auto-pick foot bones, populate Pose + Trajectory channels), `add_pose_search_notify` (any of the 8 `UAnimNotifyState_PoseSearch*` states), `validate_pose_search_database` (per-entry skeleton compatibility + stale/unbuilt-index detection, no mutation). `get_database_stats` / `get_pose_search_schema` gained richer read-back (per-skeleton mirror table, per-channel sample/bone detail, entry list, normalization set).
  - **Mirror data tables (`animation`):** `create_mirror_data_table` (from a skeleton, default biped L/R rules or supplied pairs) and `set_schema_mirror_data_table` (assign a mirror table to a Pose Search schema skeleton slot).
  - **AnimBP motion-matching graph authoring (`animation`):** `configure_pose_history_node`, `configure_motion_matching_node` (blend time, jump threshold, search throttle, inertial blend, max active blends, notify filtering), `build_motion_matching_node` (composite — spawns the Motion Matching node + a Pose History collector, wires them, sets the database, compiles), `add_evaluate_chooser_node`, `wire_chooser_to_motion_matching`, `build_foot_ik_pass` (component-space pelvis ModifyBone + Two-Bone IK on the foot chains, curve-gated), `assign_post_process_anim_rig`. `add_anim_graph_node` gained `pose_history` and `inertialization` aliases.
  - **Chooser-table authoring (`chooser`):** `create_chooser_table`, `add_chooser_column` (Bool / Enum / GameplayTag / FloatRange / OutputObject, with an optional `enum_class` for enum-class predicate columns), `add_chooser_row`, `set_chooser_cell` (typed per-column predicate write), `set_evaluate_chooser_result_reference` (edit EvaluateChooser result rows that `set_result_asset_reference` can't reach). `inspect_chooser` gained an optional `recursive` flag that emits the full nested chooser tree, and `duplicate_chooser_tree` now remaps nested `FEvaluateChooser` / `FNestedChooser` references (two-pass, order-independent, with a per-row remap report).
  - **Character / actor scaffolding (`blueprint`):** `set_anim_class`, `apply_movement_preset` (orient-to-movement / strafe), `add_engine_component_typed`, `scaffold_locomotion_input` (Input Mapping Context + Input Actions + AddMovementInput wiring), `validate_animbp_variable_contract`, `scaffold_motion_matching_character` (composite — create/reparent, set mesh, set AnimClass, apply movement preset, compile). Adds an `EnhancedInput` Build.cs dep.
  - **Thread-safe AnimBP authoring (`blueprint` / `animation`):** `add_property_access_node` (reflective `K2Node_PropertyAccess` for genuine thread-safe property reads), `set_function_thread_safe` (mark a Blueprint function `BlueprintThreadSafe` so it compiles thread-safe-clean when called from `BlueprintThreadSafeUpdateAnimation`), `bind_chooser_database_via_threadsafe` (authors an exec-driven `EvaluateChooser2` inside a thread-safe function, stores the result, and feeds the Motion Matching database from it — no AnimGraph chooser pruning). `scaffold_locomotion_anim_values` now emits a fully-wired thread-safe body via Property Access and can target a named function graph.
  - **Retarget create/run pack (`animation`):** `create_ik_rig`, `create_ik_retargeter`, `set_retargeter_rigs`, `batch_retarget_animations` (run a retarget over a clip list to an output folder).

- **PIE / profiling harness (`editor` / `ai` / `project`).** An async PIE-smoke session model plus profiling, capture, and map-authoring primitives.
  - **PIE smoke + capture (`editor`):** `run_pie_smoke` / `poll_pie_smoke` / `stop_pie_smoke` (single-observer async session), `capture_pie_movement_clip` (with `discard_first_frames` warm-up, label-aware `view_target_actor`, per-frame validity heuristic, staged `pre_pie` / `on_begin_play` / `after_n_ticks` / `before_capture` hooks, runtime-identity report + `expected_anim_class` assert), `capture_anim_frames` (preview an AnimSequence / BlendSpace / AnimBlueprint to PNG), `list_dirty_packages`, `save_packages`, `list_errored_blueprints`. A declarative `actor_setup` block (spawn N actors, optionally copy a DataAsset's reflected fields onto matching props, AIController MoveToLocation) plus `csv_profile` / `trace_channels` brackets that scope an engine CSV profiler / Insights trace to exactly the PIE window. `get_build_errors` gained `since_marker` / `since_iso` / `clear_baseline` + compile-vs-other buckets with category exclusion.
  - **Map authoring (`editor`):** `author_map_settings` (WorldSettings GameMode override + PlayerStarts + actor instances on any map), `create_nav_harness_map` (now with `game_mode_override` + `player_starts`).
  - **Navigation (`ai`):** `rebuild_navigation` (bounded async wait, optional save), `validate_nav_points` (per-point projection + per-pair path checks).
  - **Project (`project`):** `refresh_assets`, `get_saved_asset_state`, `cleanup_generated_assets` (hard allowlist guard, `dry_run` default true).

- **State-machine authoring + live anim telemetry (`animation`).** `create_state_machine` (spawn a State Machine node into an AnimBP that has none), `build_state_machine` (author a full SM from a declarative spec — states, state animations, transitions, bool-var / automatic rules, single compile), `sample_pie_anim_instance` (sample a live PIE AnimInstance — class, AnimClass, mode, montage, SM state, and bone/socket transforms resolved by name). `get_anim_graph_choosers` enumerates an AnimBP's EvaluateChooser nodes with resolved chooser asset paths (+ optional recursive expansion). `get_transition_rule` reads a transition's rule back as structured data; `set_transition_rule` accepts a structured `kind=compare`. `get_nodes` gained `include_anim_graph`. Graph-surgery additions: `rebuild_evaluate_chooser_node`, `replace_evaluate_chooser_nodes`, `duplicate_reparent_and_sanitize`, `find_node_slice`, `remove_node_slice`.

- **AI controller + movement BT tasks (`ai`).** `AMonolithBehaviorTreeAIController` (Blueprintable) runs a `UBehaviorTree` from `OnPossess()` — the correct hook for autonomous BT startup — and exposes a BlueprintCallable `StartBehaviorTree()`. Three reusable `UBTTaskNode` subclasses for locomotion control: `BTTask_SetMaxWalkSpeed`, `BTTask_SetCrouch`, `BTTask_RandomizeFloat`. Authored via the existing `add_bt_node`; no new dispatcher actions for the task classes.

- **Blueprint inspection (`blueprint`).** `get_component_details` now falls back to the parent-class CDO subobject for inherited native components (reporting `is_inherited_native` plus relative transform and, for skeletal-mesh components, `skeletal_mesh` / `anim_class` / `animation_mode`); `get_blueprint_info` adds `native_component_count`. `seed_data_asset` gained an optional `read_back_values` (re-reads written top-level fields live), routed through the new shared `FMonolithReflectionReader` in `MonolithCore`; `get_cdo_properties` now routes through the same reader as the canonical verify-after-write path.

- **Param aliases (`describe` / `bulk_fill`).** Schemas now accept `namespace` / `action` as aliases for `target_namespace` / `target_action`, so the natural call shape works.

### Fixed

- **Motion Matching graph dead-ended before the Output Pose.** `build_motion_matching_node` now connects the Pose History collector's `Pose` output to the AnimGraph Output Pose (`Root.Result`) — previously the graph compiled clean but emitted the ref pose. Reports `output_pose_wired`; the new `get_anim_graph_output_connection` reader confirms it.
- **Inherited native-component CDO overrides didn't persist.** `set_anim_class` / scaffold mesh-set / `set_component_property` now persist inherited native-component (e.g. `CharacterMesh0`) CDO overrides via `Modify` + `MarkBlueprintAsStructurallyModified` + compile (`MarkBlueprintAsModified` alone dropped them on reload). New reader: `get_inherited_component_override`.
- **`batch_retarget_animations` produced frozen clips.** `create_ik_retargeter` / `set_retargeter_rigs` never seeded the retarget op stack; the batch op now seeds the default ops + AutoMapChains and self-heals empty-op assets, so retargeted clips carry per-frame motion.
- **`reorder_bt_children` didn't persist.** BT child execution order derives from graph `NodePosX`, not pin link order, so re-linking was discarded; the action now reassigns `NodePosX` and marks the graph dirty.
- **`build_behavior_tree_from_spec` didn't link the Blackboard.** The root graph node's `BlackboardAsset` clobbered `UBehaviorTree::BlackboardAsset` on every update, so `RunBehaviorTree` created no Blackboard component; the link now persists.
- **`get_database_stats` asserted on never-indexed databases.** It called `GetSearchIndex()` unconditionally; it now guards via `RequestAsyncBuildIndex` and reports `index_built:false` gracefully.
- **`add_anim_graph_node` crashed on bound-graph-owning nodes.** BlendStack-derived nodes (including Motion Matching) asserted `BoundGraph==nullptr` on the template path; they now spawn via `FGraphNodeCreator` on a pristine node. The plain-node path is unchanged.
- **PIE world-leak on repeated map loads.** `run_pie_smoke` / `load_level` now end any resident PIE, run a bounded `EndPlayMap` + GC before any map load, with a refuse-while-running backstop — repeated map loads no longer assert.
- **`author_map_settings` / `create_nav_harness_map` re-dirtied the package on identical GameMode re-apply.** They now compare the current `DefaultGameMode` against the incoming class and skip `Modify` / assign / `MarkPackageDirty` when unchanged (reported via `game_mode_override_changed`).
- **Inherited Blueprint-visible bools were rejected by transition-rule validation.** `set_transition_rule` / `build_state_machine` now walk the skeleton / generated / parent class for visible bools, not just the AnimBP's own variables.
- **`duplicate_chooser_tree` dropped nested chooser references.** It now remaps nested `FEvaluateChooser` / `FNestedChooser` references the engine walks (results, fallback, output-object cells), two-pass and order-independent.

### Changed

- **`build_foot_ik_pass` now sets a non-Ignore `ModifyBone` mode and the Two-Bone IK effector space/bone** so the nodes are functional rather than inert. Note: full ground-trace effector drive is a follow-up — foot IK is wired and pose-driving, but does not yet trace to ground contact.
- **Generic public naming.** PIE smoke marker defaults and example identifiers in the public action surface use generic tokens (`MONOLITH_SMOKE` / `MONOLITH_CLIP`); no param or action names changed.

### Internal

- **`FMonolithReflectionReader` (MonolithCore)** hoists the `FProperty` → JSON serializer out of two duplicate copies (CDO actions + DataAsset indexer) into one shared read-side implementation, the counterpart to the write-side reflection walker.
- **Docs + skills sync** for the motion-matching pack and the gap-closure / harness passes across `SPEC_CORE`, the per-module SPECs, `API_REFERENCE`, `MONOLITH_GUIDE`, and the shipped skills.

## [0.18.0] - 2026-06-01

> **Action count:** public **1396** in-tree actions across **25** in-tree namespaces (the release headline; sibling plugins excluded). Live `monolith_discover()` with all siblings installed returns **1619** = 1396 in-tree + 223 sibling (inventory 158 + steam 28 + substance 26 + claudedesign 11) — the 223 sibling actions are advisory only and never part of the public headline. The `niagara` namespace is **129** this release (was 120), a net **+9**: issue #64 Tranche 2 (+7) and PR #65 HLSL (+2).

### Added

- **Niagara search & discovery actions (issue #64 Tranche 2) — +7 read-only `niagara` dispatcher actions.** Read-only search / discovery over Niagara systems and emitters, all backed by Asset Registry and per-system traversal (no PIE, no mutation):
  - **`search_by_parameter`** — find systems exposing a User parameter by case-insensitive substring name, with an optional type filter.
  - **`search_by_data_interface`** — find systems using a Data Interface whose class name matches (per-system `ForEachDataInterface` traversal).
  - **`query_niagara`** — structured-filter DSL over all systems (AND-joined conditions: `emitters >/</= N`, `sim_target=GPU|CPU`, `has_renderer=<name>`).
  - **`find_similar_systems`** — rank systems by structural similarity to a reference (weighted: emitter-count proximity + renderer-class Jaccard + module-name Jaccard; self = 1.0).
  - **`search_by_material`** — find systems whose emitter renderers reference a given material.
  - **`find_niagara_references`** — find all assets referencing a given Niagara asset (Asset Registry referencer graph).
  - **`list_system_data_interfaces`** — enumerate the Data Interfaces actually USED BY a given system (per-system traversal; distinct from the CDO-only `get_di_properties`).
  - **Action count:** `niagara` namespace 120 → **127** (+7 in-tree). Combined with PR #65's +2 HLSL actions below, the namespace lands at **129** for this release; public in-tree total now **1396** across **25** namespaces. Each Tranche 2 action also has a Blueprint-callable wrapper node on `UMonolithNiagaraQueryLibrary` (see Tranche 1 below). Docs: [`SPEC_MonolithNiagara.md`](Docs/specs/SPEC_MonolithNiagara.md) § Blueprint-Callable Surface. (issue #64 Tranche 2)

- **`UMonolithNiagaraQueryLibrary` — Blueprint-callable Niagara inspection/search surface (issue #64 Tranche 1).** A new `UBlueprintFunctionLibrary` in the **editor-only** `MonolithNiagara` module that exposes existing read-only `niagara` dispatcher actions as Blueprint-callable nodes for Blueprint utilities and Editor Utility Widgets. Each node is a thin forwarder over `FMonolithToolRegistry::ExecuteAction("niagara", …)` returning a JSON `FString` plus `bSuccess` / `OutError` out-params. **Zero new dispatcher actions** (pure wrappers) and **zero cost in packaged / runtime builds** (the module is editor-only). Tranche 1 ships 17 wrapper nodes over already-shipped read actions; Tranche 2's +7 search/discovery actions add 7 more wrappers (24 nodes total). (issue #64 Tranche 1)

- **Niagara HLSL direct-editing + simulation-stage / event-handler authoring (PR #65, by @middle233).** Two net-new `niagara` actions and a set of module-stack/script/event enhancements for MCP-driven Niagara workflows:
  - **`get_custom_hlsl_text`** — reads the HLSL source from a `CustomHlsl` node via public UPROPERTY reflection. Params: `script_path` (required), optional `node_guid` to disambiguate multi-`CustomHlsl`-node scripts.
  - **`set_custom_hlsl_text`** — overwrites a `CustomHlsl` node's HLSL source under a `Modify()` + transaction with a recompile. Params: `script_path` (required), `hlsl` (required), optional `node_guid`.
  - **Selector-based stage targeting on the module-stack actions.** `get_ordered_modules` / `add_module` / `move_module` / `duplicate_module` now accept `usage: "particle_simulation_stage"` (selectors `usage_id` / `stage_name` / `stage_index`) and `usage: "particle_event"` (selectors `usage_id` / `handler_index`), letting callers target shared-graph simulation-stage and event scripts directly.
  - **`add_simulation_stage`** now materializes the matching `particle_simulation_stage` output node in the emitter graph and returns `usage_id`, `stage_id`, and `graph_outputs`.
  - **`add_event_handler`** now returns `handler_index` + `usage_id` + `usage`, and **rejects unresolved inter-emitter source emitters** instead of silently creating an empty `SourceEmitterID`.
  - **`create_module_from_hlsl`** now generates a ParameterMap bridge graph (InputMap → ParameterMapGet → CustomHlsl → ParameterMapSet → OutputNode), preserves Data-Interface input types (NeighborGrid3D / Grid3D / ParticleRead), and **strictly validates HLSL input/output types** — unknown types now hard-fail instead of silently degrading to `float`.
  - **Build gating:** the engine-private NiagaraEditor wizard linkage (used only by the ParameterMap bridge) is gated behind a new `WITH_NIAGARA_WIZARD_PRIVATE` `Build.cs` flag — ON for dev, forced OFF under `MONOLITH_RELEASE_BUILD=1` with an internal fallback path. The two new actions ride public UPROPERTY reflection and are **always available**, gate or not.
  - Also folded in: a cherry-picked crash fix (detach embedded emitter before `add_emitter`) and an action-duration telemetry log line in the HTTP server.
  - **Action count:** `niagara` namespace 127 → **129** (+2 in-tree, on top of issue #64 Tranche 2's +7 above — net +9 for the namespace this release, 120 → 129). Public in-tree total now **1396** across **25** namespaces; live `monolith_discover()` with siblings **1619**. Docs updated: [`SPEC_MonolithNiagara.md`](Docs/specs/SPEC_MonolithNiagara.md), [`API_REFERENCE.md`](Docs/API_REFERENCE.md), `Skills/unreal-niagara/unreal-niagara.md`.

### Fixed

- **`editor.delete_assets` could pop a blocking modal and freeze the editor.** Deleting an asset that was open in an editor tab, or still referenced, would raise a Slate "asset in use" / "save changes" dialog — which hangs an unattended MCP session because no human is there to click it. The action now (1) clears the package dirty flag and closes any open asset editor for each target before deleting, and (2) runs the delete inside an unattended-script guard so the engine never raises a blocking dialog. A new optional `force` bool (default `false`) selects the delete path: `force=false` soft-deletes after closing editors; `force=true` calls `ForceDeleteObjects`, nulling out referencers. Per-asset failures are now reported in a `failed_to_delete` array in the response instead of aborting the whole call. The fix is native C++ — no Python required. **Known limitation:** a NiagaraScript created and compiled in the same session can't be deleted until its compile state clears — a transient compilation/traversal graph holds a reference (the engine's own Content Browser hits the same wall). It becomes deletable after the state clears, e.g. on editor restart.

- **Handled-but-noisy ensure during full level indexing on landscape-heavy projects** (`WorldSubsystem.cpp:158`). `FLevelIndexer` loads each World via `LoadPackage` purely to enumerate placed actors — this never calls `UWorld::InitWorld`, but a placed landscape actor lazily creates and initializes a `ULandscapeSubsystem`. Tearing that world down then lets GC destroy it while the subsystem is still `bInitialized`, tripping the ensure. A bare `UWorld::CleanupWorld` (without unregistering proxy components first) is unsafe — it deinitializes the landscape subsystem while a grass-builder component still references the null render scene, crashing with an access violation; that ordering was tried and rejected. **Fix:** for a landscape world the indexer now unregisters every landscape proxy's components (`AActor::UnregisterAllComponents`, world-wide), nulling the grass-builder state so the subsystem's `Deinitialize` no longer dereferences a null render scene, and then drives `UWorld::CleanupWorld` — which clears the world subsystem collection's `bInitialized` (no GC ensure) and tears the world down normally; non-landscape worlds keep the existing WorldPartition-uninit + unload path. Runtime-verified on a landscape-heavy project: a full reindex with level indexing enabled completes with zero ensures, zero crashes, and all ~80 landscape worlds torn down with no residency cost. (#67, reported by @likeitlotlot-commits)

- **From-scratch unity editor builds failed to compile** due to duplicated file-local helpers (anonymous-namespace types/functions + file-`static`s) colliding across `.cpp`s in the same module in `MonolithReflectionIntel`, `MonolithNiagara`, `MonolithGAS`, and `MonolithBlueprint`. UE adaptive unity concatenates same-module `.cpp` into one translation unit, so these internal-linkage symbols clashed (C2084/C2011/C2668). Masked from releases by `make_release.ps1`'s `-DisableUnity`, and masked locally because adaptive unity excludes recently-edited files — so it only bit fresh-clone / full-unity (end-user first-compile) builds. Helpers are now unity-safe: the duplicated RI cursor-codec + path helpers are hoisted into shared module-internal units (`Private/Shared/RICursorCodec.{h,cpp}`, `Private/Shared/RIPathUtils.{h,cpp}`), and the remaining collisions use file-unique names. Behaviour-preserving (renames + pure extracts); no `.Build.cs`, action, or API-surface change. A full non-adaptive unity build is the acceptance gate. (#68, reported by @likeitlotlot-commits)

### Internal

- **`make_release.ps1` gained a forced-full-unity collision gate (issue #68 defense).** A second UBT pass under forced full unity (temporarily flips `bUseAdaptiveUnityBuild=false`, always restored) ship-blocks on Monolith-path symbol collisions (C2084/C2011/C2086/C2668 …) that the existing `-DisableUnity` pass is structurally blind to. Build tooling only — end users never invoke it. (commit 6539c71)

## [0.17.1] - 2026-05-29

### Fixed

- **Editor crash (`Assertion failed: FreeIndex < FPageAllocator::NumWorkerCaches` — "Exceeded max active GC worker contexts", `GarbageCollection.cpp:2133`) during deep indexing of large projects on high-core-count machines.** UE 5.7's editor runs *incremental* reachability GC by default (`gc.AllowIncrementalReachability=1`, 2ms time budget). GC worker indices come from a process-global 64-slot bitmask; when an incremental reachability pass exceeds its time budget it suspends mid-pass and retains its worker contexts (and their slots) across frames, and the engine only frees them once no pass is suspended. The deep indexer drives GC continuously — a forced collect per batch plus a per-asset synchronous load (`FlushAsyncLoading`) that each kick fresh reachability passes — so on asset-heavy projects (~17k assets, heavy Fab/MetaHuman dependency graphs) suspended passes accumulate and exhaust the 64-slot pool, asserting on a parallel-GC worker thread at the same point every run. **Fix:** the deep-index run now forces *non-incremental* (fully blocking) GC for its duration via a scoped `gc.AllowIncrementalReachability=0` override (captured-and-restored on every exit path — normal, error, cancel, and editor-shutdown-mid-index). A blocking collect always runs reachability to completion in a single call, so worker slots are always freed and the leak is structurally impossible. Worker count scales with task-graph threads, so high-core-count CPUs (e.g. 24 threads) hit the original leak fastest.

### Changed

- **Wired up two previously-inert `MonolithSettings` index toggles** (both had been declared but had zero runtime consumers): `bEnableIndex` (when `false`, skips the indexing run at subsystem init while leaving query actions registered so an existing DB still answers) and `bDeferFirstTimeIndex` (when `true`, skips the automatic first-time index). Both give end users a config-level escape hatch for the deep-index crash class above. Added the `Monolith.StartIndex` console command (referenced by the existing `bDeferFirstTimeIndex` docs but never previously implemented) to trigger a full index manually after a deferred first-time index.

## [0.17.0] - 2026-05-29

### Added

- **Reflection Intelligence — `decision_query` namespace (5 new actions) from the new `MonolithReflectionIntel` module (Phase 1 of the four-phase reflection intelligence layer; Phase 2 co-shipped in this release — see separate bullet below; Phases 3–4 remain WISHLIST).** Deterministic markdown decision-record harvest — specs, plans, `CHANGELOG.md`, `.claude/rules/` — into `decision_records` + `decision_supersedes` SQLite tables on top of `EngineSource.db`. Zero LLM calls, zero network, zero new optional dependencies. Actions: `list_decisions` (filterable by source-path substring + minimum heuristic confidence; cursor-paginated), `get_decision` (single record by stable id), `list_stale` (records whose source markdown has gone untouched past a configurable mtime cutoff; cursor-paginated), `find_supersession_chain` (transitive walk over `decision_supersedes` outward from a starting record), `find_referent_decisions` (inverse — records that explicitly supersede the given id). Three detection tiers with distinct confidence floors: YAML frontmatter `decision: true` / `status:` (0.90), `## ADR-N` / `## Architectural Decision` headers (0.85), markdown header followed within 8 lines by a paragraph containing `because` / `rationale` / `evidence` / `decision:` (0.65). The `DecisionMinConfidence` floor is applied at query time, so per-call `min_confidence` always wins over the settings default. Every action ships with the v0.17.0 ergonomics surface: `EMonolithParamKind::DiskPath` on `path_filter` (a native OS path that is never rewritten — paths in the index use `/`; a backslash in `path_filter` now produces a surfaced warning rather than a silent rewrite), `readOnlyHint: true` + `idempotentHint: true` in `tools/list` annotations, universal response shaping via `_fields` / `_omit` / `_compact_json`, opaque base64+JSON cursor pagination on the two list-style actions. Module spec: [`Docs/specs/SPEC_MonolithReflectionIntel.md`](Docs/specs/SPEC_MonolithReflectionIntel.md). Public in-tree namespace count 19 → 20; public in-tree action count 1358 → 1363.

- **`UMonolithReflectionIntelSettings` UDeveloperSettings (Editor Preferences → Plugins → "Monolith Reflection Intel").** Phase 1 surfaces `bEnableDecisionMining` (default `true`), `DecisionMinConfidence` (default `0.6`, clamped `[0,1]`), and `DecisionMarkdownRoots` (project-relative paths the indexer scans; defaults to `Docs/`, `Plugins/Monolith/Docs/`, `.claude/rules/` when empty). Phase 2 risk-mining toggles ship as `(WISHLIST)` stubs so the INI schema is stable from day one. Section: `[/Script/MonolithReflectionIntel.MonolithReflectionIntelSettings]` in `Config/MonolithSettings.ini`.

- **Lazy bootstrap + hot-reload refresh for the decision corpus.** `MonolithReflectionIntel` does NOT eagerly run the indexer on `StartupModule` — the first `decision_query` call detects table absence in `EngineSource.db` and runs the indexer once before serving the query, and `FCoreUObjectDelegates::ReloadCompleteDelegate` is bound so Live Coding / UBT hot-reloads automatically refresh the corpus. RI does NOT open its own handle to `EngineSource.db` — UE 5.7's SQLite is built with `SQLITE_OS_OTHER=1` and a custom `unreal-fs` VFS that allows only ONE open of a given file per process (a second open returns `SQLITE_IOERR`). Instead, RI borrows `UMonolithSourceSubsystem`'s already-open handle via `FMonolithSourceDatabase::GetRawHandle()` (the open `FSQLiteDatabase*`) and `GetLock()` (`FCriticalSection&`): read-path adapters borrow the handle under a game-thread-only contract enforced by `ensure(IsInGameThread())` (the subsystem closes its handle on the game thread and its async indexer uses a separate worker handle, so game-thread reads serialise against the close without a per-read lock), and write-path bootstrap indexers run under `FScopeLock(&GetLock())`. `MonolithReflectionIntel.Build.cs` now depends on `MonolithSource` + `UnrealEd` + `EditorSubsystem` (RI → MonolithSource is non-circular).

- **+4 automation tests** under `Monolith.ReflectionIntel.Decision.*` — `SchemaBootstrap`, `HeuristicAccuracy`, `SupersessionChain`, `StalenessFlag`. Disposable test DBs live at `FPaths::AutomationTransientDir()`; the 5-file fixture corpus lives at `Source/MonolithReflectionIntel/Private/Tests/Fixtures/DecisionCorpus/`. The real `EngineSource.db` is never touched by tests.

- **Reflection Intelligence Phase 2 — new `risk_query` namespace (5 actions) for repo-level risk signals.** Layered onto the same `MonolithReflectionIntel` module that shipped Phase 1's `decision_query`. Actions: `get_hotspot_score` (per-file blended churn × LOC score), `get_cochange_pairs` (files that frequently change together with a given anchor file, cursor-paginated), `get_file_churn` (per-file commit count + line delta), `get_release_window_hotspots` (files whose score exceeds a configurable threshold, cursor-paginated), `list_conditional_gates` (`#if WITH_*` macros + `bHas*` 3-location probes + `MONOLITH_RELEASE_BUILD` bypasses across the project, cursor-paginated). Mines the configured nested git repositories via `FPlatformProcess::CreateProc` against each repo's `.git/`, skipping any path without `.git/` present. A host project root that uses a non-git VCS is correctly skipped (no `.git/`). Hotspot score formula is deterministic and traceable: `0.6 * normalised_churn + 0.4 * normalised_loc`, both normalised per-repo. Co-change pair detection caps per-commit file count at `MaxCommitFileCount` (default 50) to suppress tree-wide refactor and initial-import noise. Zero LLM calls, zero embeddings, zero network. All five actions carry `readOnlyHint + idempotentHint` and `EMonolithParamKind::DiskPath` on path-style params (`\` → `/` rewrite with surfaced warning). Combined with Phase 1 + the `source_query("audit_module_dep_reality")` audit registration, the v0.17.0 in-tree total lands at **1369 actions across 21 in-tree namespaces** (up from Phase 1's 1363 across 20).

- **Reflection Intelligence Phase 2 — new `source_query("audit_module_dep_reality")` action** that catches the missing-module-in-Build.cs bug class — UPROPERTY (or any reflection-touching declaration) referencing a foreign-module type whose owning module is missing from the declaring module's `Build.cs` `Private/PublicDependencyModuleNames`. UHT generates `Z_Construct_*_NoRegister` calls that link against the foreign module's API macro at link time; the failure surfaces as a confusing LNK2019. Algorithm: regex-parse every `*.Build.cs` under `Source/` for declared deps, regex-extract type-bearing reflection declarations from every `*.h` / `*.cpp`, resolve each extracted type against `EngineSource.db`'s existing symbol → owning module mapping, emit a violation when the owning module is not declared and not on the implicit-deps whitelist (`Core`, `CoreUObject`, `Engine`, `Projects`, `RHI`, `RenderCore`). Cursor-paginated with `module_filter` substring scope. Heuristic — multi-arg templates extract only the first argument and typedef aliases are not chased to their underlying type; both gaps land in Phase 3 (UHT-artefact parser). The audit handler is owned by `MonolithReflectionIntel` but registers onto the existing `source` namespace for caller ergonomics — agents already discover `source_query` first.

- **New SQLite tables on `EngineSource.db` (Reflection Intelligence Phase 2):** `git_file_churn` (per-file commit count + line delta + first/last commit timestamp), `git_cochange_pairs` (`A < B` lexicographic-pair commit-window counts), `risk_hotspot_scores` (precomputed blended score with normalised components surfaced), `reflect_conditional_gates` (`with_macro` / `bhas_probe` / `release_bypass` rows with module + source path + line + probe arity). All four follow Phase 1's wipe-and-rewrite semantics per `Run()` invocation inside a single `BEGIN TRANSACTION ... COMMIT`. Three new indexer workers feed them — `FGitChurnIndexer`, `FGitCoChangeIndexer`, `FConditionalGateIndexer`.

- **`UMonolithReflectionIntelSettings` extended with risk-side properties (Reflection Intelligence Phase 2).** `bEnableGitCoChangeMining` (default `true`, was `false`+no-op in Phase 1) now short-circuits all three Phase 2 indexers at `Run()` entry when false. `MaxCoChangeWindowCommits` (default `50`, clamped `[10, 500]`) caps the commit-history window walked per repo for pair detection. New `MaxCommitFileCount` (default `50`, clamped `[5, 500]`) caps per-commit file touches before pair emission — commits exceeding the cap (tree-wide refactors, initial imports) contribute zero pairs. New `GitMiningNoiseFilter` (default `["Saved/*", "Intermediate/*", "Binaries/*", "*.uasset", "*.umap"]`) blacklists file patterns before churn / pair aggregation. INI section: `[/Script/MonolithReflectionIntel.MonolithReflectionIntelSettings]` in `Config/MonolithSettings.ini`.

- **+6 automation tests** under `Monolith.ReflectionIntel.Risk.*` — `RiskSchemaBootstrap` (all 4 Phase 2 tables present after empty-corpus `Run()`), `ChurnAggregation` (fixture mini-repo with 5 known commits produces correct `commit_count`), `CoChangePairSymmetry` (`(A, B)` pair stored with `A < B`, reverse lookup returns same row), `HotspotScoreFormula` (hand-computed score matches `0.6 churn + 0.4 LOC` blend within `1e-6`), `ConditionalGateSweep` (fixture corpus produces expected `with_macro` + `bhas_probe` + `release_bypass` rows), `MonsterCommitSuppression` (commit with `MaxCommitFileCount+1` files contributes zero pairs). Plus tests under `Monolith.ReflectionIntel.ModuleDepReality.*` covering Build.cs parsing, `EngineSource.db` symbol resolution, implicit-deps whitelist filtering, and template-argument extraction. Fixture corpus under `Source/MonolithReflectionIntel/Private/Tests/Fixtures/RiskCorpus/`. Disposable test DBs at `FPaths::AutomationTransientDir()`; the real `EngineSource.db` is never touched.

- **Reflection Intelligence Phase 3a — new `cppreflect_query` namespace (5 actions) for UE 5.7 reflection-edge queries.** Layered onto the same `MonolithReflectionIntel` module that ships Phase 1's `decision_query` and Phase 2's `risk_query`. Actions: `get_uclass` (UHT-derived UCLASS record — parent class, specifiers, source path), `list_uproperties` (cursor-paginated UPROPERTY surface for a UCLASS), `list_ufunctions` (cursor-paginated UFUNCTION surface, with raw `EFunctionFlags` bitfield + return type + per-param JSON), `find_interface_impls` (every C++ UCLASS implementing a given UINTERFACE — Blueprint impls are handled separately through the asset graph), `find_class_specifier` (every UCLASS carrying a given specifier — `BlueprintType`, `Blueprintable`, `Abstract`, `Deprecated`, etc., substring match against `class_specifiers`). Drives a regex sweep over UHT artefacts (`Intermediate/Build/Win64/.../Inc/<Module>/UHT/*.gen.cpp`) cross-joined with `IAssetRegistry::GetDependencies` for the asset side. **No tree-sitter dependency, no ThirdParty vendoring** — the substrate is deterministic file IO plus 8 regex patterns derived from real `.gen.cpp` inspection (`BeginClassPattern`, `ClassParentPattern`, `ClassSpecifiersPattern`, `PropInfoPattern`, `PropMetaPattern`, `FuncInfoPattern` with three-capture validator, `FuncParamPattern`, `InterfaceImplPattern`). All five actions carry `readOnlyHint + idempotentHint`; the four list-style actions are cursor-paginated via the v0.17.0 codec pattern.

- **New SQLite tables on `EngineSource.db` (Reflection Intelligence Phase 3a):** `reflect_uclasses` (class name → module + parent + specifiers + UHT source path/line), `reflect_uproperties` (per-class UPROPERTY surface with type, blueprint_visibility, specifiers — visibility/specifier fields empty in Phase 3a, fully populated in Phase 3b), `reflect_ufunctions` (per-class UFUNCTION surface with `EFunctionFlags` + return type + params JSON), `reflect_uinterfaces` (UINTERFACE declaration sites), `reflect_uinterface_impls` (UINTERFACE → implementing UCLASS edges, C++ only), `cpp_asset_edges` (asset path → C++ class via `/Script/<Module>` package dependency, coarse `edge_kind='package_dep'` in Phase 3a). All six follow Phase 1's wipe-and-rewrite semantics per `Run()` invocation inside a single `BEGIN TRANSACTION ... COMMIT`. New indexer worker: `FCppReflectIndexer` (background thread). Cross-joining the UE class graph with `IAssetRegistry` lets agents answer "what assets reference this C++ class?" without manual reference-viewer walks.

- **`UMonolithReflectionIntelSettings` extended with CppReflect properties (Reflection Intelligence Phase 3a).** `bIndexEnginePluginReflection` (default `false`) — when false, the Phase 3a sweep restricts to project + project-plugin UHT artefacts; engine-side surface area floods low-signal hits. `UHTArtefactRoot` (default empty / auto-discover) — when empty, resolves to `FPaths::ProjectIntermediateDir() / TEXT("Build")`; set explicitly only for non-standard intermediate layouts. INI section: `[/Script/MonolithReflectionIntel.MonolithReflectionIntelSettings]` in `Config/MonolithSettings.ini`.

- **`MonolithReflectionIntel.Build.cs` gains one `PrivateDependencyModuleNames` entry: `AssetRegistry`.** Required by `FCppReflectIndexer` for `IAssetRegistry::GetDependencies` joins. UHT artefact reader itself uses only `Core` (`FRegexPattern`, `FFileHelper`, `IFileManager`) — already linked. No other Phase 3a Build.cs delta. No new conditional-gate `WITH_*` macros — Phase 3a loads unconditionally.

- **+4 automation tests** under `Monolith.ReflectionIntel.CppReflect.*` — `CppReflectSchemaBootstrap` (all 6 Phase 3a tables present after empty-artefact-root `Run()`), `UClassFixtureExtraction` (`sample.gen.cpp.fixture` produces expected `reflect_uclasses` rows + parent-class linkage), `InterfaceImplResolution` (`IMPLEMENTS_INTERFACE` lines produce expected `reflect_uinterface_impls` rows; `find_interface_impls` round-trips), `AssetGraphJoin` (fixture asset-registry stub with two `/Script/<Module>` package dependencies produces 2 `cpp_asset_edges` rows). Fixture corpus under `Source/MonolithReflectionIntel/Private/Tests/Fixtures/CppReflectCorpus/`. Disposable test DBs at `FPaths::AutomationTransientDir()`; the real `EngineSource.db` is never touched.

- **Phase 3a known limitations documented in spec.** `source_path` on the new tables is UHT's `ModuleRelativePath`, not the canonical project-relative path the Phase 1+2 tables use. `source_line` is `0` everywhere — UHT discards the original-header line during code generation; callers needing per-line precision pair Phase 3a with `source_query("search_source")`. `cpp_asset_edges.edge_kind` is coarse `'package_dep'`. `reflect_uproperties.blueprint_visibility` / `.specifiers` are empty strings — Phase 3b populates fully. See `Docs/specs/SPEC_MonolithReflectionIntel.md` §5.7 for the full caller-contract details.

- **Reflection Intelligence — new `cppreflect_query("list_class_specifiers")` discovery action + `find_class_specifier` forgiveness pass.** `list_class_specifiers` (no params) returns the distinct universe of tokens stored in the `flags` column of `reflect_uclasses`, each with a per-token class count. Those tokens are UHT metadata keys (`IsBlueprintBase`, `BlueprintType`, `Abstract`), NOT raw C++ UCLASS specifiers — so it's the discovery companion that tells you what `find_class_specifier` can actually match, instead of guessing. Same pass makes `find_class_specifier` more forgiving (no count delta): an alias map translates well-known C++ specifiers (`Blueprintable` → `IsBlueprintBase`); specifiers UHT drops entirely (`MinimalAPI`, `NotBlueprintable`) now return an honest not-captured note instead of a silent empty result; matching is case-insensitive. Brings `cppreflect` to 6 actions; public in-tree action count 1384 → 1385 (namespace count unchanged — `cppreflect` already existed). Module spec: [`Docs/specs/SPEC_MonolithReflectionIntel.md`](Docs/specs/SPEC_MonolithReflectionIntel.md) §5.5.

- **Reflection Intelligence Phase 4a — new `network_query` namespace (4 actions) for UE 5.7 replication inspection.** Layered onto the same `MonolithReflectionIntel` module that ships Phase 1's `decision_query`, Phase 2's `risk_query`, and Phase 3a's `cppreflect_query`. Actions: `list_replicated_classes` (UCLASSes carrying at least one `ReplicatedUsing` property, sortable by replicated-property count, cursor-paginated), `list_rpc_functions` (filter `reflect_ufunctions` by `Server_*` / `Client_*` / `Multicast_*` / `NetMulticast_*` name-prefix convention, cursor-paginated), `list_onrep_handlers` (every `OnRep_*` UFUNCTION paired with the property it covers via `reflect_replicated_properties.rep_notify_func` join, cursor-paginated), `audit_unbalanced_onreps` (catches typo + rename drift between property declaration and handler definition — `ReplicatedUsing=OnRep_X` declarations whose `OnRep_X` function does not exist, cursor-paginated). Drives a second UHT-artefact regex sweep (independent of Phase 3a's `FUHTArtefactReader` for separation of concerns) focused on per-property `MetaData` blocks carrying `ReplicatedUsing` tags. All four actions carry `readOnlyHint + idempotentHint`.

- **Reflection Intelligence network completeness — new `reflect_query` namespace (1 action) + replicated/RPC detection improvements.** A new `reflect` namespace ships from `MonolithReflectionIntel` with one WRITE/maintenance action: `reflect_query("rebuild_reflection_index")` (no params) — a project-only force-rebuild of the RI reflection tables (`reflect_uclasses` / `reflect_uproperties` / `reflect_ufunctions` / `reflect_uinterfaces` / `reflect_uinterface_impls` / `cpp_asset_edges` + `reflect_replicated_properties`). It re-runs the RI indexers over PROJECT UHT artefacts only (`bIncludeEnginePlugins=false`, engine excluded), and exists because after an indexer code change there's no other clean repopulation trigger — lazy bootstrap only fires on table-absence, `OnReloadComplete` only on Live Coding, and `source_query("trigger_reindex")` is the full-engine reindex. It is a WRITE/maintenance action (not read-only), idempotent (wipe-and-rewrite per indexer in a single transaction), and non-destructive (regenerates deterministically from on-disk artefacts). Two network-detection improvements land in the same workstream (no count delta): `list_replicated_classes` now captures bare `UPROPERTY(Replicated)` + `DOREPLIFETIME` via the `CPF_Net` property flag, in addition to `ReplicatedUsing` — **verified end-to-end** against a host project's replicated character/attribute classes, so the prior "bare-Replicated not captured" Phase 4b limitation is cleared for replicated-class detection. And `list_rpc_functions` switched to specifier-based detection (`reflect_ufunctions.specifiers` parsed from `EFunctionFlags`) instead of function-name-prefix matching. With the plugin scan-scope expansion (see the dedicated bullet below), `list_rpc_functions` now covers project plugins by default and returns the project's actual RPCs — verified E2E against project-plugin Server RPCs. The earlier "empty because the scan is the game module only" status is resolved. Module spec: [`Docs/specs/SPEC_MonolithReflectionIntel.md`](Docs/specs/SPEC_MonolithReflectionIntel.md) §6 + §6b. Public in-tree action count 1385 → 1386; in-tree namespace count 24 → 25 (`reflect` is brand new).

- **Reflection Intelligence — plugin-aware scan scope (no new actions, no count change).** The RI UHT-artefact indexers (CppReflect + Network) no longer scan the project game module alone. Scope is now driven by `IPluginManager::GetEnabledPlugins()` and walks each matching plugin's own `Intermediate/Build/Win64/UnrealEditor` UHT artefacts following a game-module → project-plugin → marketplace ladder. Two new `UMonolithReflectionIntelSettings` properties (Category "CppReflect") control it: **`bIndexProjectPluginReflection`** (default **`true`**) scans every enabled `LoadedFrom == Project` plugin; **`bIndexMarketplacePluginReflection`** (default **`false`**) ALSO scans enabled engine-installed marketplace plugins (`LoadedFrom == Engine` whose base dir is under `/Plugins/Marketplace/`). Epic engine built-in plugins stay excluded — that remains governed by the separate `bIndexEnginePluginReflection` (still default `false`). This is the change that fixes RPC/replicated-class detection in real projects: replicated classes and RPCs declared in project plugins are now in scope by default, so `list_rpc_functions` and `list_replicated_classes` cover project plugins (and marketplace plugins when enabled), not just the game module. Verified E2E via a project-only force-reindex — game module alone ~30 artefacts → with project plugins on **337** (project-plugin Server RPCs + replicated classes) → with the marketplace flag also on **927 artefacts / 745 UClasses**. No engine source-symbol reindex is triggered by any of this — the ladder only widens the UHT-artefact scan. Settings + scan-scope ladder: [`Docs/specs/SPEC_MonolithReflectionIntel.md`](Docs/specs/SPEC_MonolithReflectionIntel.md) §5.2. **Action count unchanged — this is settings + scan-scope behavior only.**

- **Reflection Intelligence Phase 4a — new `pipeline_query` namespace (2 composer actions).** `pr_review` (changed-files PR review composer — for each path in `changed_files[]`, fans out `risk_query("get_hotspot_score")` + `risk_query("get_cochange_pairs")` + `decision_query("list_decisions", path_filter=path)` + `source_query("audit_module_dep_reality")` + optional `blueprint_query("audit_cdo_drift")`, aggregates per-path; hard cap 100 paths per call) and `release_readiness` (release pre-flight composer — bundles `monolith_status()` + `decision_query("list_stale")` + `risk_query("get_release_window_hotspots")` + the sentinel-list audit + CHANGELOG completeness audit specced in `.claude/rules/scoped/monolith-release.md`). Both composers are read-only end-to-end — every action they invoke is itself `readOnlyHint: true`. Both fan out serially on the game thread; no `ParallelFor`, no async dispatch.

- **Reflection Intelligence Phase 4a — 4 audit actions on existing namespaces.** Each is registered cross-module from `MonolithReflectionIntel` onto its host namespace's adapter for caller ergonomics (agents already discover the host namespace first). `material_query("audit_orphan_materials")` — `/Game/` path scan via `IAssetRegistry::GetReferencers` for materials with zero inbound references; `niagara_query("audit_cross_asset_refs")` — broken/stale asset reference scan over Niagara systems/emitters joined against Phase 3a's `cpp_asset_edges`; `blueprint_query("audit_cdo_drift")` — Blueprint child CDOs that override a native C++ parent's default value (catches drift when a native default changes upstream and BP children silently keep the stale override); `project_query("audit_orphan_assets")` — project-wide zero-reference scan across `IAssetRegistry::GetReferencers` cross-validated against Phase 3a's `cpp_asset_edges` (surfaces assets referenced only from C++ but not from BP/asset graph); params are `asset_class_filter` (optional bare class-name filter), `limit`, and `cursor`. All four are read-only + cursor-paginated.

- **New SQLite table on `EngineSource.db` (Reflection Intelligence Phase 4a):** `reflect_replicated_properties` (per-property replication record — `class_name`, `module_name`, `property_name`, `rep_kind`, `rep_notify_func`, `source_path`, `source_line`). `rep_kind` is `'rep_notify'` in Phase 4a; `'bare_replicated'` is Phase 4b. Wipe-and-rewrite semantics per `Run()` invocation inside a single `BEGIN TRANSACTION ... COMMIT`, same pattern as Phases 1+2+3a. New indexer worker: `FNetworkIndexer` (background thread).

- **`UMonolithReflectionIntelSettings` extended with two Phase 4a properties.** `bEnableNetworkReplicationAudit` (default `true`) — when false, `FNetworkIndexer::Run` short-circuits at entry and `network_query` returns a status string per call. `bEnablePipelineComposers` (default `true`) — when false, both pipeline composer actions return a status string instead of fanning out. The four cross-namespace audit actions on `material` / `niagara` / `blueprint` / `project` are not gated by either flag — they continue to function. INI section unchanged: `[/Script/MonolithReflectionIntel.MonolithReflectionIntelSettings]`.

- **`MonolithReflectionIntel.Build.cs` unchanged from Phase 3a → Phase 4a.** The network indexer reuses `Core` (`FRegexPattern`, `FFileHelper`, `IFileManager`) — the same surface Phase 3a uses. The four cross-namespace audit handlers use `IAssetRegistry::GetReferencers` / `GetAssetsByPath` (already linked via Phase 3a's `AssetRegistry` dep). The two pipeline composers fan out other registered actions via `FMonolithToolRegistry::ExecuteAction` (available via `MonolithCore`). No conditional-gate `WITH_*` macros — Phase 4a loads unconditionally.

- **Phase 4a known limitations documented in spec.** Bare `UPROPERTY(Replicated)` without `ReplicatedUsing` not detected — signal lives in UHT's `PropPointers[]` emission block which Phase 4a's metadata-block sweep does not parse. Multi-condition replication (`COND_OwnerOnly`, `COND_SkipOwner`, etc.) not surfaced — `rep_kind` only carries `'rep_notify'` in Phase 4a. RPC kind derived from name-prefix convention only — the canonical signal lives in `reflect_ufunctions.function_flags` (`FUNC_NetServer` / `FUNC_NetClient` / `FUNC_NetMulticast`); callers needing flag-level fidelity should query `cppreflect_query("list_ufunctions")` directly. All three gaps deferred to Phase 4b alongside tree-sitter substrate landing — see `Docs/specs/SPEC_MonolithReflectionIntel.md` §6.5 + §9.

- **Action counts (live):** the running in-tree total lands at **1386 actions across 25 in-tree namespaces** — v0.17.0 RI brought 1358 → 1384 (Phases 1+2+3a = +16 actions / +3 namespaces; Phase 4a = +10 actions / +2 namespaces over Phase 3a's 1374 / 22), then the [Unreleased] `cppreflect_query("list_class_specifiers")` follow-up adds +1 (1384 → 1385, no new namespace), then the network-completeness `reflect_query("rebuild_reflection_index")` adds +1 plus a brand-new `reflect` namespace (1385 → 1386, 24 → 25 in-tree namespaces), then the `config_query("set_developer_setting")` dev-setter adds +1 (1386 → 1387, `config` 6 → 7, no new namespace). Live `monolith_discover()` returns **1610 total across 29 namespaces** with all four host-project sibling plugins loaded (1387 in-tree + 11 `claudedesign` + 158 `inventory` + 28 `steam` + 26 `substance` = 1610). Sibling action counts remain advisory and never affect the public Monolith release headline.

- **MCP LLM ergonomics — `config_query("set_developer_setting")` dev-gated CDO setter (`config` 6 → 7).** A `#if WITH_EDITOR`-gated write action that sets a property on any `UDeveloperSettings` CDO at runtime — params `class` (short-name or full path), `property`, `value` (parsed via `UProperty::ImportText_Direct`), optional `save_config` to persist back to INI. Lets an agent flip a setting like `bIndexMarketplacePluginReflection` and re-run a query in the same session instead of hand-editing INI + restarting the editor; returns `{class, property, old_value, new_value, saved}`. New `DeveloperSettings` dependency on `MonolithConfig.Build.cs`.

- **Phase 4b (tag-graph queries + animation thread-safety audit) deferred alongside Phase 3b.** Two follow-on audit families remain blocked on Phase 3b's tree-sitter substrate landing: `gas_query("find_tag_consumers"/"find_grant_paths"/"find_revoke_paths")` needs native-tag declaration tracking (`reflect_native_tag_decls`) to resolve native-side tag references; `animation_query("audit_thread_safety")` needs `reflect_uproperties.specifiers` populated (Phase 3a leaves these as empty strings). Both deferred together — combined value is real but not blocking the Steam-build readiness work Phase 4a directly addresses. (Bare `UPROPERTY(Replicated)` detection, previously a Phase 4b item, LANDED in the [Unreleased] network-completeness workstream — `list_replicated_classes` now captures it via `CPF_Net`.) See `Docs/specs/SPEC_MonolithReflectionIntel.md` §9.

- **Phase 3b (tree-sitter native gameplay-tag tracking) deferred to a future release.** The remaining workflow Phase 3a does not cover is native-tag declaration tracking (`UE_DEFINE_GAMEPLAY_TAG_*` / `UE_DECLARE_GAMEPLAY_TAG_EXTERN`) — UHT does not emit native-tag metadata, so the only honest substrate is source-level parsing. Vendoring `tree-sitter-cpp` (~1.1M-line `parser.c` after grammar generation, ~50MB release-zip footprint) was material against the current ~12MB Monolith release-zip baseline; the cost / benefit ratio favoured shipping Phase 3a without it. Phase 3b would add 2 SQLite tables (`reflect_native_tag_decls`, `reflect_native_tag_externs`), 1 new action (`cppreflect_query("list_native_tags")`), and backfill the empty `reflect_uproperties.blueprint_visibility` / `.specifiers` fields plus a finer-grained `cpp_asset_edges.edge_kind`. See `Docs/specs/SPEC_MonolithReflectionIntel.md` §5b for the full deferral rationale.

- **Niagara stateless-emitter factory — 1 new `niagara::` action (`create_stateless_emitter`, Phases 0-3 of `Docs/plans/2026-05-28-niagara-stateless-timer.md`).** Creates a standalone `UNiagaraStatelessEmitter` (Lightweight Emitter) asset for programmatic test-asset setup. Uses `FindObject<UClass>(nullptr, "/Script/Niagara.NiagaraStatelessEmitter")` + type-erased `NewObject` to avoid coupling to Niagara's `Internal/Stateless/` headers (intentionally not exposed to dependent modules). Pairs with the stateless-aware branches added to `set_emitter_loop_profile` and `get_emitter_timing_summary` (see Changed below) for end-to-end Lightweight Emitter authoring without going through the `UNiagaraSystem` wrapper. Spec: [`Docs/specs/SPEC_MonolithNiagara.md`](Docs/specs/SPEC_MonolithNiagara.md) § Stateless Emitters. Niagara namespace 118 → 119.

- **Niagara temporal-control surface — 9 new `niagara::` actions (Phases 1-4 of `Docs/plans/2026-05-28-niagara-timing-actions.md`).** Collapses scattered timing edits (per-property `set_system_property` + `set_simulation_stage_property` + `set_module_input_value` against `EmitterState` / `InitializeParticle`) into composite, intent-named writers. **System-level (4):** `get_system_timing` (bundled read of `WarmupTime` / `WarmupTickCount` / `WarmupTickDelta` / `bFixedTickDelta` / `FixedTickDeltaTime` / `bRequireCurrentFrameData` in one call), `set_warmup_profile` (composite write calling `UNiagaraSystem::SetWarmupTime` + `SetWarmupTickDelta`; response returns the engine-resolved triple so callers observe the `ResolveWarmupTickCount` snap), `set_fixed_tick_delta` (toggle `bFixedTickDelta` with optional `fixed_delta_time`), `set_require_current_frame_data` (toggle `bRequireCurrentFrameData`). **Emitter (2):** `set_emitter_loop_profile` (composite write of EmitterState loop topology — `loop_behavior` / `loop_duration` / `loop_delay` / `loop_count` / `loop_delay_enabled`; internally dispatches `set_static_switch_value` for Loop Behavior + UseLoopDelay and `set_module_input_value` for Duration/Delay/Count; stateless emitters early-out with a hint), `get_emitter_timing_summary` (read aggregator pulling loop topology + `sim_stages[]` (name/`NumIterations`/`ExecuteBehavior`) + `InitializeParticle` lifetime fields into a single response; optional `emitter` filter). **Sim-stage aliases (2):** `set_sim_stage_iteration_count` and `set_sim_stage_execute_behavior` — both alias atop the existing `set_simulation_stage_property` action's `stage_index` / `stage_name` selector convention; iteration-count internally formats the int as the `FNiagaraParameterBindingWithValue` struct-literal `(Value=N)`. **Particle lifetime (1):** `set_particle_lifetime` (convenience write to `InitializeParticle` — `min` only → Direct mode with constant `Lifetime`; `min` + `max` → Random mode with `Lifetime Min` + `Lifetime Max`). Zero new module dependencies, zero sibling-plugin surface. Design: [`Docs/plans/2026-05-28-niagara-timing-actions-design.md`](Docs/plans/2026-05-28-niagara-timing-actions-design.md). Spec: [`Docs/specs/SPEC_MonolithNiagara.md`](Docs/specs/SPEC_MonolithNiagara.md) § Temporal Control. Niagara namespace 109 → 118. Credit to **@middle233** — this timing + sim-stage direction was inspired by their community work in PR #65; direct HLSL script editing from that PR is slated for a future release.

- **MCP LLM ergonomics — universal response-shaping params on every action (Phase 1.0, Survivor B).** Every registered action now accepts three opt-in universal params that reshape the JSON response post-dispatch: `_fields: string[]` (whitelist — keep only these top-level response keys), `_omit: string[]` (blacklist — drop these top-level keys, mutually exclusive with `_fields`; collision appends a warning and `_fields` wins), and `_compact_json: bool` (drop top-level keys whose value is `null` / `""` / `{}` / `[]`). Underscore prefix is mandatory — unprefixed `fields` collides with `MonolithBlueprintStructActions::create_struct_with_fields`. K3 strict-params allowlist co-admits the three keys at registry level (`MonolithToolRegistry.cpp` around line 115) so no per-action schema migration was required. **Top-level keys only in Phase 1** — JSONPath / JMESPath grammar is deferred (WISHLIST). The post-filter (`ApplyResponseShaping` in `MonolithCore/Public/MonolithJsonUtils.h`) appends mutual-exclusion warnings to the same `warnings[]` channel that K3 unknown-param soft-warns and Survivor D AssetPath rewrites populate.

- **MCP LLM ergonomics — schema-tagged param kinds via `EMonolithParamKind` (Phase 1.0, Survivor D).** New enum on `FParamSchema` entries with four variants: `Other` (default — no path semantics, never rewritten), `AssetPath` (dispatcher rewrites `\` → `/` at dispatch time, never silent — emits a surfaced warning), `DiskPath` (native OS path; explicit opt-OUT for clarity, never rewritten), `GameplayTag` (`A.B.C` reserved; never rewritten in Phase 1). The enum is serialised onto the per-param schema JSON as a string field `"kind"`, emitted only when non-default to keep `tools/list` bytes lean. New `FParamSchemaBuilder` sugar overloads `RequiredAssetPath` / `OptionalAssetPath` / `RequiredDiskPath` / `OptionalDiskPath` opt a param into a non-`Other` kind. The `AssetPath` rewrite block in `FMonolithToolRegistry::ExecuteAction` runs AFTER K2 alias rewrite and BEFORE the K3 unknown-key check. **Back-compat preserved** — every existing `.Required(...)` / `.Optional(...)` call site defaults to `Kind == Other` and opts OUT of path normalisation. **Per-namespace param tagging audit is a follow-up (Phase 1.1+)** — ~929 occurrences across 30 `*Actions.cpp` files remain untagged; deferred and tracked in `Docs/plans/2026-05-27-mcp-llm-ergonomics.md` §3.D.

- **MCP LLM ergonomics — `did_you_mean` fuzzy match on dispatch errors (Phase 2).** Unknown-action and unknown-namespace dispatch failures now carry `error.data.suggestions` — top-3 closest registry keys with normalised scores. UE `Algo::LevenshteinDistance` over the registry keyspace, snapshot-then-unlock pattern keeps the hot dispatch lock held only for the snapshot (scoring runs lock-free off a local copy — no read-path latency spike). `error.data.kind` field disambiguates `"action"` vs `"namespace"` suggestions. Asset-path and property-name fuzzy matching explicitly out of scope (O(N·L²) over 10K+ registry entries kills it; property-name overlap with the K3 unknown-key warning produces noise). Implementation: `MonolithCore/Public/MonolithFuzzyMatch.h` + `MonolithFuzzyMatch.cpp`. Spec: SPEC_CORE.md §14.4.

- **MCP LLM ergonomics — MCP-spec tool annotations on `tools/list` (Phase 2).** Five audited read-only / idempotent tools now serialise `readOnlyHint` / `destructiveHint` / `idempotentHint` / `title` hints so LLM clients can pre-filter destructive vs read-only calls without dispatching them first: `monolith_discover`, `monolith_status`, `monolith_guide`, `monolith_reindex`, `source_query`. Per-action annotations live on `FMonolithActionInfo`; per-dispatcher annotations live on the new `FMonolithDispatcherAnnotations` struct via `FMonolithToolRegistry::SetDispatcherAnnotations` / `GetDispatcherAnnotations` (because per-action annotation is semantically wrong when sibling actions inside the same dispatcher disagree on `destructive` / `read-only`). Annotations are only emitted when at least one field is non-default — avoids ~1567 × 4 bytes of boilerplate per session init. Per-action annotation across the full 1567-action surface is architecturally blocked by the 15-dispatcher collapse pattern and intentionally not pursued. Spec: SPEC_CORE.md §14.3.

- **Offline CLI — full Reflection Intelligence parity, byte-identical to the live MCP server.** `monolith_query.exe` (`Tools/MonolithQuery/monolith_query.cpp`) and the stdlib-only dev fallback `Scripts/monolith_offline.py` now implement ALL 20 RI actions — `cppreflect` (6), `network` (4), `decision` (5), `risk` (5) — and emit JSON byte-identical to live: same field names, types, key ordering, row data, float formatting (`%.17g`), and cursor tokens (base64 of UE pretty-printed `{\r\n\t"qh":N,...}` with UE-matching `Strihash_DEPRECATED` filter-hash). Prior builds covered only 4 of the 20 offline, with divergent shapes. The exe is canonical; the `.py` is a dev-only fallback kept in lockstep. Two intentional differences vs the live payload are documented (not bugs): the offline CLI adds a top-level `success: true|false` (its in-band status channel — live carries success/error out-of-band via MCP, so live has no `success` key; the nested DATA payload is byte-identical), and wall-clock fields (`cutoff_unix` / `since_unix` and the `risk.get_release_window_hotspots` cursor whose filter-hash includes them) differ by the run-time gap across separate process invocations on both live and offline.

- **New offline-parity guard infrastructure.** `Scripts/verify_offline_parity.py` (HARD-GATE — byte-diffs exe vs py across all 20 RI actions, exits non-zero on any diff; run as a ship-blocking gate by `make_release.ps1`), `Scripts/check_offline_exe_fresh.py` (staleness guard — compares the exe's `--version` `source_hash` against a fresh hash of `monolith_query.cpp`), and `Tools/MonolithQuery/build.bat` now injecting `SOURCE_HASH` (certutil SHA256 of the source) into the exe via `/DSOURCE_HASH`.

- **MCP LLM ergonomics — `source_query("search_source")` cursor pagination (Phase 3).** New opaque base64+JSON `cursor` in-param and `next_cursor` / `total_estimate` out-params. Page 0 emits `total_estimate` via a server-side SQLite FTS5 `MATCH COUNT(*)` against the symbol + source FTS tables (~50–200 ms cold cache; cached inside the cursor for subsequent pages). Cursor envelope shape: `{ query_hash, symbol_page, source_page, cached_total_estimate }`. Hard cap 1000 rows total per query; a request with `limit=2000` is clamped. Query-hash mismatch returns a clean `INVALID_CURSOR` error rather than silently serving the wrong slice; fallible decode (empty / garbage / malformed input) returns a clean error too — never panics. **Rerun-slice scheme, not keyset** — FTS5 `bm25()` rank is unstable under inserts / deletes, so the only honest pagination over a moving FTS index is to rerun-and-slice. `project_query("search")` cursor pagination is deferred (WISHLIST) — architecturally blocked by `FMonolithIndexDatabase::FullTextSearch` at `MonolithIndexDatabase.cpp:1017-1079`, which performs a UNION-and-resort across multiple FTS tables without deterministic ordering; requires a CTE / `UNION ALL` refactor with stable ordering first. Implementation: `MonolithSource/Public/MonolithCursorCodec.h` + `MonolithCursorCodec.cpp` and count helpers on `FMonolithSourceDatabase`. Spec: SPEC_CORE.md §14.5.

- **MCP LLM ergonomics — proxy-side JSONL call log (Phase 4).** Both proxies now emit one line per call to `Saved/Logs/MonolithCalls.jsonl` (project-root-relative). Eight-field schema: `ts` (ISO-8601 UTC), `namespace`, `action`, `params_hash` (SHA-1 hex over canonicalised params JSON — sorted keys, no whitespace; **not** `FCrc::StrCrc32`), `duration_ms`, `ok`, `error_code` (omitted on success), `result_bytes`. Native proxy (`Tools/MonolithProxy/monolith_proxy.cpp`) and Python fallback (`Scripts/monolith_proxy.py`) implement the same schema. Opt-out: set `MONOLITH_CALL_LOG=0`. Local-only, no phone-home. User-managed rotation (delete the file to reset). Native proxy requires `build_proxy.bat` rebuild + Claude Code MCP reconnect to engage; Python picks up on next Claude Code start. Spec: SPEC_CORE.md §14.6.

### Changed

- **`make_release.ps1` now builds the offline exe fresh and gates on parity.** The release script builds `monolith_query.exe` (vcvars + `build.bat`) before the Binaries copy, then runs `Scripts/verify_offline_parity.py` as a ship-blocking gate — a stale or drifted exe can never ship.

- **Niagara `set_emitter_loop_profile` and `get_emitter_timing_summary` now handle `UNiagaraStatelessEmitter` assets natively (Phases 0-3 of `Docs/plans/2026-05-28-niagara-stateless-timer.md`).** The previous stateless-detection points (which early-outed with the placeholder "set_stateless_loop_profile coming" hint) now dispatch into real reflection-based read/write paths against the protected `EmitterState` (`FNiagaraEmitterStateData`) UPROPERTY. Detection uses `StaticLoadObject` + class-name match; the stateful module-routed path is unchanged. New optional `loop_duration_mode` param on `set_emitter_loop_profile` accepts `"Fixed"` / `"Infinite"` (maps to `ENiagaraLoopDurationMode` — meaningful only on stateless; stateful path emits a warning when supplied since the `EmitterState` module has no equivalent input). Responses on the stateless branch include a `stateless: true` flag; `get_emitter_timing_summary` additionally returns `null` for all 4 `InitializeParticle` lifetime fields and `sim_stages: []` (stateless emitters have no sim stages by design). Same action names, same call surface — purely an additive dispatch branch. Spec: [`Docs/specs/SPEC_MonolithNiagara.md`](Docs/specs/SPEC_MonolithNiagara.md) § Temporal Control, § Stateless Emitters.

- **Symmetric params-string unwrap on the `monolith_` branch of `HandleToolsCall`** — mirrors the pre-existing `_query` branch behaviour so meta-namespace tools (`monolith_discover`, `monolith_status`, `monolith_guide`, etc.) accept the same nested-`params` shape that domain tools already handled.

- **Both proxies advertise the universal params on every tool's inputSchema.** `Tools/MonolithProxy/monolith_proxy.cpp` (native) and `Scripts/monolith_proxy.py` (Python fallback) now emit `_fields` / `_omit` / `_compact_json` on every tool descriptor returned from `tools/list`. **The native proxy requires a `build_proxy.bat` rebuild + Claude Code MCP reconnect** to engage end-to-end; the Python fallback picks up the change on next restart.

- **(LLM ergonomics, Phase 1.1) Per-namespace `AssetPath` / `DiskPath` tagging sweep across 87 `*Actions.cpp` files.** Realises Phase 1.0 survivor D's value: ~913 builder call sites tagged `AssetPath` (the dispatcher now silently rewrites backslash→forward-slash on those params with a surfaced warning), ~18 tagged `DiskPath` (handler expects native paths — opt OUT of rewrite), ~50 left as `Other` (class identifiers, dotted property/struct paths, Outliner folders, glob filters, default-bearing optionals, ambiguous semantics). Counter-example handlers explicitly DiskPath where audit found native-backslash dependence: source `read_file`, source-image / GAS-tag import `source_path`, FBX export `file_path`, debug-view PNG output paths, LogicDriver `json_path_or_data`, Material `include_file_paths` HLSL virtuals. `MonolithGASCueActions save_path_prefix` left as `Other` (trailing-slash directory-join semantics). Pure call-site refactor atop Phase 1.0 sugar overloads — no `*.Build.cs` / `.uplugin` / module-dep delta. UBT exit 0, 0 errors / 0 warnings.

### Fixed

- **(Reflection Intelligence) RI was non-functional in every prior build — now works (commit a5fd582).** Each RI read-path adapter opened a SECOND handle to `EngineSource.db`, which UE 5.7's single-open `unreal-fs` VFS (`SQLITE_OS_OTHER=1`) rejects with `SQLITE_IOERR` — so every `decision_query` / `risk_query` / `cppreflect_query` / `network_query` call failed with "EngineSource.db not available" and the feature never returned data. Fixed by borrowing `UMonolithSourceSubsystem`'s already-open handle: `FMonolithSourceDatabase` now exposes `GetRawHandle()` (the open `FSQLiteDatabase*`) and `GetLock()` (`FCriticalSection&`); read-path adapters borrow the handle under a game-thread-only contract (`ensure(IsInGameThread())`) and write-path bootstrap indexers run under `FScopeLock(&GetLock())`. `MonolithReflectionIntel.Build.cs` now depends on `MonolithSource` + `UnrealEd` + `EditorSubsystem` (RI → MonolithSource is non-circular). E2E-verified: decision (556 records), risk (conditional gates), cppreflect (full UCLASS reflection), network (OnRep handlers + balance audit). Earlier commit 4ab4587 laid the groundwork with a retry-friendly bootstrap path that checks PRAGMA return codes before serving a query.

- **Removed the phantom `risk.list_hotspots` offline action.** It existed only in the offline CLI (the live MCP server never registered it) and is gone — `risk` offline now matches the live namespace exactly.

- **(LLM ergonomics, Phase 1.0 follow-up) Response-shaping post-filter (`_fields` / `_omit` / `_compact_json`) now engages on the MCP wire.** Two-investigator bug-investigator consensus identified the root cause: Claude Code's MCP client serialises array-valued top-level arguments as JSON-encoded strings rather than native arrays. `MonolithJsonUtils::ReadStringArrayParam` (and the `_compact_json` bool read inside `ApplyResponseShaping`) now carry a string-fallback mirroring the existing `params`-key special-case in `MonolithHttpServer.cpp:691-701`. Native-array path remains FIRST (back-compat with automation tests and offline CLI). Live verification: `monolith_status({_fields:["version","total_actions"]})` returns exactly `{"version":"0.16.0","total_actions":1571}`; `editor_query("get_recent_logs", _fields:["count"])` returns `{"count":1}` instead of the prior 412-byte full envelope. Diagnostic `UE_LOG(LogMonolith, Verbose)` line fires only inside the recovery branches.

### Internal

- **+11 automation tests** under `Monolith.ResponseShaping.*` and `Monolith.ParamKind.*` (Phase 1.0) — all passing. Cover whitelist / blacklist / mutual-exclusion / compact-drop / non-`AssetPath` pass-through / K3 strict-params interaction.

- **+10 automation tests** under `Monolith.FuzzyMatch.*` (Phase 2 — 5/5 passing) and `Monolith.CursorPagination.*` (Phase 3 — 5/5 passing). Phase 1.0 brought 11; Monolith total across Phases 1.0–4 is +21 tests.

- **No new MCP actions registered across Phases 1.0 / 1.1 / 2 / 3 / 4** — the response-shaping surface is universal post-dispatch, the param-kind enum is a schema-tag opt-in (87-file sweep tagged registrations only — no new actions), fuzzy match piggybacks on dispatch errors, cursor pagination extends an existing action surface in-place, and the proxy call log is proxy-side. Action count is unchanged from v0.16.0 baseline. No `*.Build.cs` changes. No `.uplugin` changes. No new module dependencies.

## [0.16.0] - 2026-05-27

A small, focused release between the v0.15.0 ergonomics framework and the next major surface. Headline work: a five-action **preview & inspection surface** expansion in the `editor::` namespace (renders + structural-data reads for tech-art and AI agents), one MCP-introspection hint that points agents at the schema-discovery surface before they guess parameter names, and two component-persistence fixes from a community-reported regression.

### Added

- **Preview & inspection surface expansion (`editor::` namespace):** extended
  `editor::capture_scene_preview` `asset_type` enum to support `static_mesh`,
  `skeletal_mesh` (with optional `animation_path` + `seek_time` for posed
  capture), and `widget` (UMG via `FWidgetRenderer` with `scale` DPI multiplier).
  New `editor::capture_material_grid` (N material instances side-by-side under
  shared lighting; auto-grid layout via `ceil(sqrt(N))` with optional `columns`
  override). New `editor::capture_with_overlay` (5 engine debug-view modes:
  wireframe, normals, uv_density, lightmap_density, shader_complexity). New
  `editor::inspect_material_pbr` (PBR texture parameter classification +
  ORM/ARM/MRA channel-packing detection — pure reflection, no rendering). New
  `editor::inspect_texture_channels` (per-channel R/G/B/A min/max/mean
  statistics + optional per-channel split PNGs). All editor-only. AI
  discoverability via new `monolith_guide` recipe entries.

- **Schema-discovery hint in MCP `initialize` instructions (Issue #62, @middle233).**
  Both the C++ HTTP server (`HandleInitialize`) and the Python proxy now point AI agents
  at `monolith_discover`, `describe_query("action_schema")`, and `monolith_guide` from the
  initial handshake — so clients read schemas instead of trial-and-erroring parameter names.
  No new action; widens the existing introspection surface's discoverability.

### Fixed

- **Component persistence (Issue #63, @Heiselisha):** `mesh.convert_to_hism`, `mesh.place_spline`, and
  `ai.place_smart_object_actor` now call `AActor::AddInstanceComponent` on every component they create, so
  the components survive level save/reload. Convert-to-HISM also gains a pre-destroy guard that preserves
  source actors if HISM instance creation reports a count mismatch. `mesh.place_spline` follow-up: root +
  spline components now spawn with `EComponentMobility::Static` so saved spline data round-trips through
  the level's Static-mobility persistence path.

### Contributors

Big thanks to **@middle233** for [Issue #62](https://github.com/tumourlove/monolith/issues/62) (MCP schema-discovery guidance gap) and to **@Heiselisha** for [Issue #63](https://github.com/tumourlove/monolith/issues/63) (HISM / spline / SmartObject component persistence regression). Both issues drove fixes that ship in this release.

## [0.15.0] - 2026-05-23

This release rolls up everything since v0.14.10 (20 commits). Headline work: the **MCP ergonomics framework** (`bulk_fill_query` + `describe_query` routing to 12 per-namespace adapters), a **dataset read/edit ergonomics pack** (17 new `blueprint` actions for round-tripping DataTables / CurveTables / StringTables + `seed_data_asset`), the **UI/Blueprint MCP gap-closure** sweep (9 new actions across 4 phases + the 0516 UI gap-audit Tier 1–4), the new **`monolith_guide`** editorial action, and two community PRs (#58, #60). Two reflection/enum correctness fixes, plus a sibling-plugin-reference scrub in shipping comments.

Public in-tree action count is approximate. Per the Action Count Discipline this EXCLUDES sibling-plugin actions — sibling plugins are not in the public release zip, so their actions are not counted here. With the experimental town-gen registration (`bEnableProceduralTownGen=true`) the in-tree total rises by +45. Authoritative per-namespace breakdown lives in [`Docs/SPEC_CORE.md` §12](Docs/SPEC_CORE.md#12-action-count-summary).

### Added

- **MCP ergonomics framework — `bulk_fill_query` + `describe_query` + 12 per-namespace adapters.** Two new top-level Monolith MCP namespaces land in `MonolithCore`: `bulk_fill_query` (2 actions: `apply`, `list_namespaces`) and `describe_query` (2 actions: `schema`, `list_targets`). Framework primitives ship as `BlueprintType` USTRUCTs in `MonolithCore`: `FBulkFillSpec` (input shape — `target_namespace`, `target`, nested JSON `tree`, `dry_run`, `strict`), `FDryRunReport` (per-field `FieldWrites` / `SilentDrops` / `Clamps` / `Errors`), `FSchemaDescriptor` (recursive descriptor tree — type names, ImportText sample forms, `range_min` / `range_max`, `enum_values`, `conditional_on` discriminators). `FMonolithReflectionWalker` is the single source of truth for UE 5.7 `FProperty` / `FStructProperty` / `FArrayProperty` / `FMapProperty` / `FSetProperty` / `FObjectProperty` / `FSoftObjectProperty` / `FEnumProperty` recursive descent — `InspectTree(...)` returns a dry-run report without mutation, `ApplyTree(...)` performs writes under a caller-owned transaction. `FMonolithBulkFillRegistry` is the string-keyed singleton dispatcher; **zero compile-time linkage from `MonolithCore` into adapter modules** (per-namespace adapters self-register from their owning module's `StartupModule`) preserves the Issue #30 / #32 hard-import hazard class. `FMonolithDryRunGuard` is the RAII helper that opts an adapter into the framework's `dry_run:true` preview-without-persist semantics. 12 per-namespace adapters ship in this release: **blueprint** (`set_cdo_properties`, `describe_cdo_schema` aliases registered for symmetry with the existing `set_cdo_property` / `get_cdo_properties` reads), **gas** (`AttributeInitDataTable` `fill_kind`, `#if WITH_GBA` stub-pattern — `Register()` runs unconditionally, body switches on the gate), **inventory** (an optional sibling-plugin adapter, conditionally gated), **ui** (Input-action DataTable as single transaction, slot-property scoped describe), **ai** (`EQSTests`, `BlackboardKeys`, `SmartObjectSlots` fill_kinds), **niagara**, **material**, **audio** (MetaSound paths `#if WITH_METASOUND`), **mesh**, **animation**, **logicdriver** (`#if WITH_LOGICDRIVER` stub-pattern), **combograph** (`#if WITH_COMBOGRAPH` stub-pattern). H5 stub-adapter pattern uniform across all conditional-gate adapters: `RegisterAdapter` always runs; adapter body conditionally compiled; release-build `#else` returns a clean typed error rather than silently no-op'ing. Net surface: ~50 new C++ files, ~8276 LOC, **zero new module dependencies**. Per-namespace `fill_kind` catalogues live in each per-module SPEC's new "Bulk Fill & Describe Surface (2026-05-11)" section.

- **MonolithUI Tier 2 close-the-loop primitives — 8 new `ui_query` actions.** `rename_widget` (rename a widget tree entry by path), `add_widget_variable` (author a new variable on a Widget Blueprint), `audit_focus_chain` (walks the navigation graph and reports orphans / dead-ends / cycles), `apply_token_binding` (Tokenforge → widget property binding — MVP-STUB; issue [#2-10b](https://github.com/tumourlove/monolith/issues/2) tracks full BP-graph node-write completion), `list_widget_property_enums` (returns the legal enum value list for a UMG property — feeds the property-allowlist authoring loop), `convert_textblock_to_common` (in-place `UTextBlock` → `UCommonTextBlock` upgrade preserving text/font/color), `set_action_bar_button_class` (re-targets a `UCommonBoundActionBar`'s button class without rebuilding the bar), `dump_blueprint_compile_log` (returns the structured compile-log warnings/errors for a Widget Blueprint by path — closes the gap where `compile_widget` returned only success/failure boolean). `MonolithUI.Build.cs` gains `BlueprintGraph` + `Projects` dependencies to support the graph-walk and project-file paths.

- **MonolithUI Tier 3 headline scaffolders — 4 new `ui_query` actions.** `scaffold_main_menu` (full main-menu Widget BP: title, vertical button stack, focus chain wired, reduce-motion gate, hardware-visibility border), `scaffold_settings_panel_with_tabs` (TabList + ActivatableSwitcher with one tab per Settings DataTable row, focus return on close), `scaffold_pause_menu` (push-to-stack ActivatableWidget with Resume/Settings/Quit, input mode swap, blur backdrop), `build_menu_from_spec` (data-driven menu builder from a JSON spec — MVP; issue [#3-18b](https://github.com/tumourlove/monolith/issues/3) tracks multi-screen aggregation). All four return the saved asset path + a verification structure listing the widgets / animations / styles created. New file `Source/MonolithUI/Private/CommonUI/MonolithCommonUITemplateActions.cpp` (993 lines).

- **MonolithUI Tier 4 polish + docs.** `convert_button_to_common` gains Tokenforge auto-detect (when a project has the Tokenforge plugin installed, `convert_button_to_common` will auto-apply the matching design token to the new `UCommonButtonBase` if a binding can be inferred from the source button's style). `parent_class` lookup-by-name doc improvements (clarifies which fallback resolution paths are tried). `set_initial_focus_target` UPROPERTY contract documented (the action authors a `UPROPERTY` reference on the widget, not a transient binding — survives BP recompile). `compile_widget` `errors[]` surface documented (the array is always present, empty on success, populated with structured warning + error entries on failure). New "CommonUI Property Allowlist Coverage" section in `SPEC_MonolithUI.md`. `-32011 ErrTokenforgeProviderAbsence` error contract confirmed parity with `-32010 ErrOptionalDepUnavailable` — the two codes share semantics (optional sibling-plugin runtime absent) but distinct discriminators so AI clients can route token-related failures separately from EffectSurface failures.

- **`mesh.import_mesh` skeletal mesh + animation import support** — PR [#58](https://github.com/tumourlove/monolith/pull/58) by **@4698to**. The action gains two optional parameters that widen the import surface without breaking existing static-mesh callers: `import_as_skeletal: bool=false` (when true, the FBX importer is configured to produce a `USkeletalMesh` instead of a `UStaticMesh` — auto-resolves rig + skeleton hierarchy from the FBX), and `import_animations: bool=false` (when true and the source FBX contains animation tracks, additionally imports the bundled `UAnimSequence` assets alongside the mesh). Schema-only widening — no new action registered, the existing single `mesh.import_mesh` handler dispatches on the new params. Silent-promote behaviour: if `import_animations=true` is requested but the FBX carries no anim tracks, the action succeeds and reports `animations_imported: 0` rather than erroring — callers needing strict animation presence must inspect the count post-call. Full per-param semantic table lives in [`specs/SPEC_MonolithMesh.md`](Docs/specs/SPEC_MonolithMesh.md) §Import. Closes the prior workaround of authoring a transient FBX import factory via `editor.run_python` for cross-skeleton retarget flows.

- **`niagara.{get_system_summary,get_emitter_summary}` semantic-detail surface + `niagara.validate_system` event-chain reasoning** — PR [#60](https://github.com/tumourlove/monolith/pull/60) by **@middle233**. Both summary actions gain an optional `detail_level: "compact" | "full"` parameter (default `"compact"`). Compact returns the existing terse payload plus per-emitter `role_hint`, `spawn_location_mode`, `requires_persistent_ids`, `consumed_events[]` / `generated_events[]` summary fields; full additionally emits per-emitter `incoming_events[]`, `outgoing_links[]`, `event_generators[]`, `location_modules[]`, `semantic_notes[]`, plus system-level `inter_emitter_link_count` + `independent_burst_emitters[]` + `inter_emitter_topology[]`. `validate_system` now reasons about inter-emitter event chains: walks the system topology once via `CollectTopologyEdges`, classifies each emitter's role from a name-substring + module-graph topology heuristic (`event_source` / `event_receiver` / `burst_receiver` / `independent_burst` / `shell_event_source` / `trail_follower` / `trail_or_ribbon` / `independent_emitter`), surfaces unresolved `SourceEmitterID` warnings, warns when a receiver consumes an event its named source does not generate (`GenerateDeathEvent` / `GenerateLocationEvent` / `GenerateCollisionEvent` module-name probes), and emits `requires_persistent_ids` guidance for any emitter participating in an event chain. Schema-only changes for all three actions — no count delta (niagara stays 109). Maintainer hardening on top of the PR: (1) **O(N) `AnalyzeEmitterSemantic` cache** in `HandleValidateSystem` — the PR called `AnalyzeEmitterSemantic` once per emitter in the outer validation loop AND once per source-emitter in the inner event-chain lookup, producing O(N^2) graph walks on event-heavy many-emitter systems. Replaced with a single pre-pass that populates a `TMap<FGuid, FMonolithNiagaraEmitterSemantic>` keyed by emitter handle GUID (same key used by the serialised `incoming_events[].source_emitter_id` field, so the lookup matches the payload contract exactly); inner loop is now `SemanticCache.Find(SourceGuid)`. (2) **Dead-branch drop** in the new `GetGraphForHandleUsage` anon-ns helper — the PR included a `SystemSpawnScript / SystemUpdateScript` switch case that fell through to `GetSystemSpawnScript()` (never returning the Update graph), but the only caller `CollectEmitterModules` iterates emitter-stage usages only — removed the dead branch to keep the helper honest. Design-notes block landed in [`SPEC_CORE.md` §14](Docs/SPEC_CORE.md#14-niagara-summary--validation-semantics).

- **`monolith_guide` MCP action — section-keyed editorial cross-namespace guide for AI agents.** New `guide` action in the `monolith` meta-namespace (`monolith` namespace 4 → 5: `discover`, `status`, `update`, `reindex`, `guide`), backed by a new `FMonolithGuideTool` static class in `MonolithCore`. Serves a hybrid payload: hand-authored `Docs/MONOLITH_GUIDE.md` (loaded via `FFileHelper`, cached behind a session-lived `FCriticalSection`-guarded `TOptional<FString>`, split on `^## ` H2 headers) plus a **live registry overlay** — per-namespace action counts from the running `FMonolithToolRegistry` and the plugin version, so counts always match the live build. Six sections: `onboarding`, `recipes`, `decisions`, `errors`, `skills_map`, `gotchas`. Call `monolith_guide()` for the full index + all sections, or `monolith_guide(section="recipes")` to return a single section and bound context cost; an unknown section returns a validation error listing the valid keys. Audience is **external public-Monolith users** with no project `CLAUDE.md`, private skills, or agent registry. Deliberately omits a pipelines section (cross-linked to [`SPEC_CORE.md` §13](Docs/SPEC_CORE.md#pipelines), not re-authored) and a per-namespace action table (that is `SPEC_CORE.md` §12 + `Docs/references/MCP.md`); `skills_map` points at `Skills/<topic>/SKILL.md` rather than inlining bodies. **Pull-only, zero per-success-call cost** — no success response carries guide content; the only breadcrumb is a single `guide_hint` string on the no-filter `monolith_discover()` response. Offline parity via `monolith_query.exe monolith guide`. The markdown cache refreshes on editor restart (no live file-watcher).

- **Dataset read/edit ergonomics — 17 new `blueprint` actions for round-trip authoring of DataTables, CurveTables, and StringTables.** LLM-friendly read → edit → write loop for the three table asset types, all engine-generic (reflection + string class/struct resolution), zero sibling-plugin coupling, zero new module deps. Reuses the MonolithCore reflection framework (`DescribeStruct` for inline schema, the `FDryRunReport` shape for write previews). **DataTable (8):** `read_data_table` (rows plus the `RowStruct` schema inline), `describe_data_table_schema` (schema only), `set_data_table_rows` (bulk upsert/add/update with `dry_run` + `strict`, per-field `{path, current, proposed, ok, reason}` reporting, one `BroadcastPostChange` per call), `remove_data_table_row` / `rename_data_table_row` / `duplicate_data_table_row` (row CRUD via `FDataTableEditorUtils`), `export_data_table` (JSON or CSV via `GetTableAsJSON` / `GetTableAsCSV`), `import_data_table` (JSON or CSV via `CreateTableFromJSONString` / `CreateTableFromCSVString`, REPLACE semantics with a `RowStruct` guard). **CurveTable (5):** `read_curve_table` (rich or simple key dump — time/value/interp/tangents), `set_curve_table_keys` (replace or merge keys, respects the empty-table rich/simple mode lock), `add_curve_table_row` / `remove_curve_table_row` / `rename_curve_table_row`. **StringTable (3):** `read_string_table` (namespace + key/source enumeration), `set_string_table_entries` (upsert/replace + namespace), `remove_string_table_entry`. **DataAsset (1):** `seed_data_asset` (create + `bulk_fill` populate in one atomic call). DataAssets otherwise round-trip through the existing `bulk_fill_query` (`apply`) + `describe_query` (`schema`) — no extra read/write action needed, documented in the per-module SPEC. New files: `MonolithBlueprintDataTableActions.{h,cpp}`, `MonolithBlueprintCurveTableActions.{h,cpp}`, `MonolithBlueprintStringTableActions.{h,cpp}`; `seed_data_asset` in `MonolithBlueprintStructActions`.

- **UI/Blueprint MCP gap-closure — 9 new actions across four phases.** Engine-generic; foreign classes resolve by string/reflection, zero sibling-plugin coupling. **`ui` (5):** `set_widget_navigation_bulk` (apply N navigation-rule writes then compile once, vs the per-call compile of `set_widget_navigation`; per-entry failures non-fatal), `dump_widget_navigation` (read-only dump of per-direction `UWidget::Navigation` rules including Wrap/Stop/Escape that the Explicit-edge-only `audit_focus_chain` can't see), `convert_border_to_common` (in-place `UBorder` → `UCommonBorder` preserving variable identity, parent slot, and content child), `reparent_widget_root` (replace a Widget Blueprint root with a new `UPanelWidget`-derived class resolved by string, migrating children), `set_widget_is_variable` (first-class flip of `UWidget::bIsVariable` to mark/unmark a tree widget as an exposed Blueprint variable). The four CommonUI-surface actions are `#if WITH_COMMONUI`-gated. **`blueprint` (3):** `add_property_access` (author a `UK2Node_VariableGet/Set` reading/writing a UPROPERTY on an arbitrary foreign class resolved by string via `FMemberReference::SetExternalMember` + `AllocateDefaultPins`, so the value pin resolves to the property's real type instead of a wildcard — closes the cross-class variable get/set gap), `override_parent_function` (author a Blueprint override of an overridable parent function — including ones that RETURN a value, e.g. `UCommonActivatableWidget::BP_GetDesiredFocusTarget` → `UWidget*`, which `add_function` and the event-node form can't do), `save_dirty_assets` (save all currently-dirty Blueprint + Widget Blueprint packages in one sweep with a `path_prefix` filter — closes the data-loss window after a batch of dirtying edits). **`describe` (1):** `action_schema` (return a registered action's full param schema — names, types, required, defaults, aliases, descriptions — by `target_namespace` + `action`, so callers stop trial-and-erroring param names). Plus ergonomics that don't change the count: `get_variables include_bind_widgets` now enumerates BOTH C++ `BindWidget`/`BindWidgetOptional` refs AND pure-Blueprint `bIsVariable` tree widgets (deduped), widget-context `CallFunction` resolution, NodeGuid-on-create across every node-creation path, `add_function`/`add_node` param aliases (`function_name`, `function_class`, `member_class`, `pos`), and a clear `get_widget_tree` error when `asset_path` is empty/missing.

### Fixed

- **MonolithUI Tier 1 correctness fixes — 6 fixes across the always-on + CommonUI surface.** (1) `MonolithUIStyleService` hash-cache mis-keying — the dedup cache was keying on object identity instead of content hash, so two structurally-identical style payloads got cached under different keys and Style Service stats over-reported `unique_styles` while under-reporting cache-hit ratio. (2) CommonUI button allowlist additions — `UCommonButtonBase` missing properties (`MinPaddingDesired`, `bAutoCollapse`, `HoveredAudio`) now writable through `set_widget_property`. (3) Reduce-motion gate diagnostic improvement — when the project's reduce-motion setting is unset, `wrap_with_reduce_motion_gate` now surfaces a structured `reason` instead of failing with a generic "setting not found" error. (4) `create_bound_action_bar` gains an optional `action_button_class` parameter — callers can now author a bound action bar with a custom button class in one call instead of authoring with the default class and patching afterward. (5) `compile_widget` now surfaces `errors[]` and `warnings[]` arrays in the response — the older boolean-only return shape masked recoverable compile warnings that callers needed to see. (6) `set_widget_property` accepts `value` as an alias for `property_value` — closes the foot-gun where callers swapped between read (`get_widget_property` returns under `value`) and write (which required `property_value`) and silently got nothing written.

- **UserDefinedEnum fields inside a UserDefinedStruct now surface `enum_values` in schema and accept display-name writes — were previously reported as a bare `int32`.** A UserDefinedEnum field inside a UserDefinedStruct compiles to a plain numeric `FProperty` with no `Enum` association (`UUserDefinedEnum` is `ECppForm::Namespaced`; the KismetCompiler only emits `FEnumProperty` for `EnumClass`), so the reflection walker reported it as `int32` with no enumerators and writes required the raw integer index. The walker now recovers the `UEnum` from editor-only UDS metadata (`FStructureEditorUtils::GetVarDescByGuid` → `SubCategoryObject`): `DescribeStruct` emits the recovered enumerators (friendly display names) and the enum's name as `type_name`, improving the schema for `read_data_table`, `describe_data_table_schema`, every `bulk_fill`/`describe` adapter, and `describe_query("schema")`. A shared resolver (`ResolveUserDefinedEnumToken`) maps an incoming display or authored name to the enum's integer value before `ImportText`, with bare-int back-compat, wired into the `set_data_table_rows` and `add_data_table_row` write paths. Robust `_MAX` sentinel handling (`IsAutoMaxSentinel`) so enums without a sentinel never drop a real value. Native `FEnumProperty` / `FByteProperty`-with-enum paths untouched (no regression). Editor-only (`#if WITH_EDITOR`), no new module deps.

- **`blueprint.create_user_defined_enum` dropped the last enumerator of every enum it created — now authors all N.** A freshly created `UUserDefinedEnum` starts with ZERO user enumerators (index 0 is already the auto `_MAX` sentinel; `NumEnums() == 1`). The old authoring loop ran `i = 1..N-1`, calling `AddNewEnumeratorForUserDefinedEnum` only N-1 times, then the display-name loop wrote the Nth name onto the `_MAX` slot — `SetEnumeratorDisplayName` bounds-checks only `idx < NumEnums()`, so it silently produced an orphaned DisplayName. Net effect: every enum created via this action lost its LAST value — present only as an orphan display name, invisible at runtime (`NumEnums`, `ImportText`, the reflection walker). Fixed to author N enumerators (`i = 0..N-1`) so real entries occupy `0..N-1` and the `_MAX` sentinel lands at N; display-name and read-back loops now address all N real entries. Verified live: a 3-value enum reports `internal_name` for all three and resolves a display-name write of the last value.

- **NodeGuid correctness — every MCP node-creation path now assigns a valid `NodeGuid` ([gap #15](https://github.com/tumourlove/monolith/issues/15)).** `add_node`, `add_event_node`, `add_timeline`, `promote_pin_to_variable`, `add_comment_node`, and `add_property_access` now call `UEdGraphNode::CreateNewGuid()` so authored nodes carry valid GUIDs and no longer risk invalid-GUID warnings on compile/save.

- **C2011 struct redefinition under unity builds in MonolithBABridge** ([PR #61](https://github.com/tumourlove/monolith/pull/61) by **@tc-imba**). `DECLARE_LOG_CATEGORY_EXTERN` for `LogMonolithBABridge` was duplicated at file scope across two `.cpp` files; the declaration is hoisted into `MonolithBAFormatterImpl.h` (outside the `WITH_BLUEPRINT_ASSIST` guard) so both the BA-enabled and empty-shell log paths share a single declaration.

### Changed

- **`animation.add_anim_graph_node` now supports arbitrary concrete custom AnimGraph node classes** via an optional `node_class` parameter, while preserving the existing built-in `node_type` aliases. The action resolves loaded `UAnimGraphNode_Base` subclasses by full path or unique short/class name, rejects abstract/non-AnimGraph/ambiguous/schema-incompatible classes before mutation, and keeps stock node aliases available for release builds.

- **`niagara.get_emitter_summary` `event_handlers[]` array removed — breaking shape change** ([PR #60](https://github.com/tumourlove/monolith/pull/60) by **@middle233**). The legacy `event_handlers[]` payload (one entry per `FNiagaraEventScriptProperties` carrying `{event_name, source_emitter_id}`) is superseded by two richer surfaces emitted by the new semantic-detail pipeline: `consumed_events[]` (canonicalised event-name list — `DeathEvent` / `LocationEvent` / `CollisionEvent` — emitted at both `compact` and `full` detail levels) and `incoming_events[]` (full per-edge topology array with `source_emitter_name`, `source_emitter_id`, `execution_mode`, `spawn_number`, `max_events_per_frame`, etc. — `full` detail level only). Callers reading `event_handlers[].source_emitter_id` migrate to `incoming_events[].source_emitter_id` (`full` only) or `consumed_events[]` for the canonicalised list. All other existing `get_emitter_summary` fields preserved (additive elsewhere).

- **15 central error messages in MonolithCore (HTTP server + tool registry) now carry inline recovery guidance.** The recovery hints fold directly into the existing `error.message` / `ErrorMessage` text at each of the 15 central error sites (HTTP request parse/dispatch + registry namespace/action lookup) — e.g. an unknown-namespace error now suggests checking the `WITH_*` compile-time gate, a malformed-request error names the expected shape. **No schema change, no envelope change**: no new field on `FMonolithActionResult` / `FMonolithActionInfo`, no `RegisterAction(...)` signature change, no per-success-call cost. MCP clients parsing the error text simply see longer, more actionable messages. Per-handler error text across the in-tree modules is unchanged — only the 15 central sites were salvaged.

### Internal

- **Phase 5 SPEC backfill: 8 per-namespace SPECs gained a "Bulk Fill & Describe Surface (2026-05-11)" section** documenting their adapter's `fill_kind` catalogue, sample tree, adapter-specific quirks, and v1.1 follow-ups: `SPEC_MonolithAI.md`, `SPEC_MonolithNiagara.md`, `SPEC_MonolithMaterial.md`, `SPEC_MonolithAudio.md`, `SPEC_MonolithMesh.md`, `SPEC_MonolithAnimation.md`, `SPEC_MonolithLogicDriver.md`, `SPEC_MonolithComboGraph.md`. The pre-existing `SPEC_MonolithGAS.md`, `SPEC_MonolithBlueprint.md`, and `SPEC_MonolithUI.md` sections renamed to match the canonical heading.

- **Neutralized private sibling-plugin references in shipping comments.** Three comments in `MonolithUI` source named private sibling plugins by path/name (sibling-adapter paths plus an internal dev context) — these files are new in this release window and would publish for the first time. Replaced the specific private references with generic per-namespace-adapter phrasing; the technical rationale is preserved. No code change.

- **Action count: ~1344 in-tree actions across 19 namespaces, closing the v0.15.0 window.** A mid-window live snapshot via `monolith_discover()` on 2026-05-11 read 1540 total across 23 namespaces (in-tree 1317 across 19; sibling-plugin total 223 = `claudedesign` 11 + `inventory` 158 + `steam` 28 + `substance` 26). The late-window additions land another +27 in-tree to reach ~1344: `blueprint` +20 (the 17 dataset read/edit actions plus `add_property_access` / `override_parent_function` / `save_dirty_assets`), `ui` +5 (the Phase 3/4 gap actions plus `set_widget_is_variable`), `describe` +1 (`action_schema`), and `monolith` meta +1 (`guide`). Per the Action Count Discipline, the advertised public count EXCLUDES the sibling-plugin namespaces above — they are NOT in the public release zip. Counts re-verified against the release-candidate build; authoritative per-namespace breakdown in [`Docs/SPEC_CORE.md` §12](Docs/SPEC_CORE.md#12-action-count-summary). (A pre-existing `ui`/GAS-alias double-count in the headline `19-namespace` figure is out of scope here; a holistic count audit is deferred.)

### Contributors

Big thanks to **@4698to** for [PR #58](https://github.com/tumourlove/monolith/pull/58) (`mesh.import_mesh` skeletal-mesh + animation import params), to **@middle233** for [PR #60](https://github.com/tumourlove/monolith/pull/60) (event-aware Niagara summary semantics + `validate_system` event-chain reasoning), and to **@tc-imba** for [PR #61](https://github.com/tumourlove/monolith/pull/61) (MonolithBABridge unity-build C2011 fix + root-cause analysis). Author attribution preserved on every cherry-picked commit.

## [0.14.10] - 2026-05-09

### Added

- **MetaSound document introspection action pack — 12 new `audio_query` actions** ([PR #18](https://github.com/tumourlove/monolith/pull/18) by **@alakangas**, refactored into the existing `MonolithAudio` module). Read-only walk of `IMetaSoundDocumentInterface::GetConstDocument()` → `FMetasoundFrontendDocument`. Distinct from the existing 25 MetaSound Builder API actions: those read **live builder state** during graph mutation; these read **on-disk document state** for arbitrary assets without an active builder session. Action list (all conditional on `WITH_METASOUND`): `list_metasounds` (project-wide enum), `list_metasound_documents` (per-asset page list), `get_metasound_document` (full doc walk), `get_metasound_summary` (lightweight counts), `inspect_metasound_node_instance` (per-node pin/edge details), `get_metasound_document_connections` (edge enumeration), `get_metasound_document_variables` (graph variables), `get_metasound_user_parameters` (public inputs/outputs), `search_metasound_document_nodes` (substring search), `get_metasound_info` (asset metadata), `get_metasound_dependencies` (external class deps + subgraphs), `validate_metasound` (lint pass). PR's original architecture proposed a separate `MonolithMetaSound` module + `metasound_query` namespace — landed refactored into existing `audio_query` per maintainer architectural preference (no new module, no new namespace). Action names disambiguate from the existing Builder-side actions (`get_metasound_document` vs Builder-side `get_metasound_graph`; `inspect_metasound_node_instance` vs Builder-side `get_metasound_node_info`; `get_metasound_document_connections` vs Builder-side `list_metasound_connections`; `get_metasound_user_parameters` vs Builder-side `get_metasound_input_names`). All 12 actions PIE-smoke-tested at port time. By **@alakangas**.

- **`FMetaSoundIndexer` deep indexer in MonolithIndex** ([PR #18](https://github.com/tumourlove/monolith/pull/18) by **@alakangas**). New `Source/MonolithIndex/Public/Indexers/MetaSoundIndexer.h` + `.cpp`. Walks `UMetaSoundSource` + `UMetaSoundPatch` assets at reindex time, opens via `IMetaSoundDocumentInterface::GetConstDocument()`, iterates root-graph pages via `FMetasoundFrontendGraphClass::IterateGraphPages` (const overload), and writes nodes / edges / variables / dependencies into `ProjectIndex.db` for cross-asset query via `project_query`. Sentinel-class registration mirrors `FNiagaraIndexer`. Throttled via `FMonolithMemoryHelper::ShouldThrottle` / `ForceGarbageCollection` / `YieldToEditor` (per-batch, GC every N batches). New setting `bIndexMetaSounds` (default true) under Editor Preferences → Plugins → Monolith → Indexing → Deep Indexers. `MonolithIndex.Build.cs` gains a 3-location Metasound probe (engine `Plugins/Runtime/Metasound`, marketplace, top-level fallback) honouring `MONOLITH_RELEASE_BUILD=1` for binary-release safety (Issue #30 defense). Conditional on `WITH_METASOUND`. By **@alakangas**.

- **`animation.list_bone_tracks` action** — PR [#54](https://github.com/tumourlove/monolith/pull/54) by **@MaxenceEpitech**. Returns `{ count, bone_names: [..] }` for a `UAnimSequence` by walking `IAnimationDataModel::GetBoneTrackNames(TArray<FName>&)`. Closes the discovery gap before `get_bone_track_keys` — Skeleton bone listings include unanimated bones, so they're not a substitute. Wired into `batch_execute` alongside the other animation read actions.

- **`editor.run_console_command` action** — PR [#54](https://github.com/tumourlove/monolith/pull/54) by **@MaxenceEpitech**. Dispatches a console command via the first `APlayerController` of the active PIE world (so exec UFUNCTIONs on the possessed pawn fire correctly), falling back to `GEngine->Exec` on the editor world when no PC is available. Returns which world type was used (`pie` / `editor`) and whether the PC path was taken. Maintainer hardening on top of the PR: `GEngine` null-guard added to the fallback branch (returns clean error instead of dereferencing a null engine pointer); description string clarified that multi-client PIE routes to the first PlayerController found (no disambiguation).

- **`editor.start_pie` + `editor.stop_pie` actions** — PR [#54](https://github.com/tumourlove/monolith/pull/54) by **@MaxenceEpitech**. `start_pie` queues a Play-In-Editor session and refuses to queue a duplicate when a PIE world is already alive. `stop_pie` calls `RequestEndPlayMap` when a PIE world exists, no-op (with `stopped: false`) otherwise. Pairs with the existing `run_python` / `load_level` (v0.14.9 Issue #50) actions for fully automated in-game test flows: load level → start PIE → run console cmds → stop. Maintainer hardening on top of the PR: `start_pie` rewritten to pin to **in-viewport mode** via `FLevelEditorModule::GetFirstActiveViewport()` + `FRequestPlaySessionParams::DestinationSlateViewport` + `EPlaySessionWorldType::PlayInEditor` (canonical pattern from `LevelEditorSubsystem::EditorRequestBeginPlay` at `LevelEditorSubsystem.cpp:264-277`). Without this pin, the action would inherit the user's last-used PIE flavour (`Simulate` / `NewWindow` / etc.) via `ULevelEditorPlaySettings::LastExecutedPlayModeType` — surprise factor for MCP callers expecting "start PIE" to mean "spawn player in active level viewport". Response now includes `mode: 'in_viewport'` for caller verification.

- **`animation.get_skeleton_preview_attached_assets` action** — PR [#55](https://github.com/tumourlove/monolith/pull/55) by **@MaxenceEpitech**. Reads `USkeleton::PreviewAttachedAssetContainer` (the editor-only `[Preview Only]` list shown in Persona's bone tree). Returns `{ asset_path, attached_objects: [{ attach_point, attached_object, attached_object_class }, ...], count, transforms_stored: false }`. The `transforms_stored: false` flag documents that the container does NOT carry per-asset relative transforms (Persona attaches at the socket origin with the asset's natural pivot). Closes the gap where `[Preview Only]` attachments were only readable by parsing the `.uasset` binary. Includes the UE 5.7 surface fix (commit `42e771e`) for `FPreviewAssetAttachContainer::Num()` / `operator[](int32)` — the older `GetNumAttachedObjects` / `GetAttachedObjectByIndex` / `GetAttachNameByIndex` accessors no longer exist in UE 5.7.

- **`animation.get_bone_ref_pose` action** — PR [#55](https://github.com/tumourlove/monolith/pull/55) by **@MaxenceEpitech**. Returns reference (bind) pose transforms for a skeleton's bones in BOTH parent-relative AND component-space. Walks `FReferenceSkeleton` once to compute component-space via parent-index accumulation. Accepts a `bone_names: array` filter (default: all bones). Works on either a `USkeleton` or `USkeletalMesh` asset path — `source_type` field in the response indicates which. Replaces the prior workaround of spawning a temporary `SkeletalMeshActor` to call `GetSocketTransform()` at bind pose.

- **`animation.{get,add,remove}_compatible_skeleton` actions** — PR [#56](https://github.com/tumourlove/monolith/pull/56) by **@MaxenceEpitech**. Three new actions wrapping `USkeleton::CompatibleSkeletons` — the canonical UE5 mechanism that lets anims authored on one skeleton play on another (typical case: UE4 mannequin animation packs on UE5 `SK_Mannequin`). Idempotent semantics: `add_compatible_skeleton` returns disjoint `added` / `already_compatible` booleans and the resulting `count`; `remove_compatible_skeleton` returns disjoint `removed` / `was_compatible` booleans. Self-compat rejected with a clean error (`"Cannot mark a skeleton compatible with itself"`). `save: bool=true` controls whether `UEditorAssetLibrary::SaveAsset` runs after the mutation. Closes the prior `editor_query.run_python` workaround for cross-skeleton retarget setup.

### Fixed

- **MCP proxy startup tool-list stability for clients that do not refresh after `tools/list_changed`.** The native proxy and Python fallback now return a cached tool list when the editor is down, or a seed list of the stable namespace/meta tools on first run. This preserves Claude Code's normal auto-reconnect behavior and also keeps Codex-style deferred tool catalogs from starting with an empty Monolith surface when the AI session launches before Unreal Editor.

- **`animation.get_bone_track_keys` rewritten to use non-deprecated `IAnimationDataModel` API** — PR [#54](https://github.com/tumourlove/monolith/pull/54) by **@MaxenceEpitech**. Old code read raw `FRawAnimSequenceTrack` via the deprecated `IAnimationDataModel::GetBoneAnimationTracks()` accessor (wrapped in `PRAGMA_DISABLE_DEPRECATION_WARNINGS`). That path returns the **uncompressed source tracks** which are missing on AnimSequences that have already been baked / compressed — so callers got `Bone track not found: <bone>` even when the bone was clearly animated and visible in the asset. Switched to the public, non-deprecated pair: `IsValidBoneTrackName()` to validate the bone (no false-positive on missing source tracks) and `GetBoneTrackTransforms(FName, TArray<FTransform>&)` to evaluate per-key `FTransform`s (works regardless of underlying compressed storage). Adds an empty-track guard so `AllTransforms.Num() == 0` returns a clean error instead of producing `num_keys=0` and a misleading `start_frame > end_frame` message.

### Changed

- **`animation.get_bone_track_keys` `scales` array semantics — now always populated regardless of source compression.** Old code emitted the `scales` JSON field only when the underlying `FRawAnimSequenceTrack::ScaleKeys` had entries (silently dropped scales when key counts diverged across pos/rot/scale arrays). New code emits scales for every keyframe in the requested range because `FTransform::GetScale3D()` is always defined. **Behaviour shift for downstream callers:** any tooling that used `len(scales) == 0` as a sentinel for "no scale animation" will mis-classify identity-scale tracks. Inspect the actual `FVector` values to detect identity (`{1, 1, 1}`) instead. PR [#54](https://github.com/tumourlove/monolith/pull/54) by **@MaxenceEpitech**.

- **`blueprint.get_cdo_properties` gains 3 optional filters** — PR [#57](https://github.com/tumourlove/monolith/pull/57) by **@MaxenceEpitech**: `owner_class_filter` (case-insensitive substring on owner class name — skips inherited `AActor` / `APawn` / `ACharacter` props in one parameter), `name_pattern` (case-insensitive substring on property name), `exclude_categories` (case-insensitive exact match on `Category` metadata, e.g. `["Replication", "Cooking", "HLOD"]`). All additive; default `null` keeps the previous full-list output. Composes with the pre-existing `category_filter` and `include_parent_defaults` options. Cuts JSON payload by ~90% in typical AActor-subclass inspection flows where most properties are inherited generic actor scaffolding rather than the asset's own surface.

### Internal

- **PR #17 disposition: substantively superseded.** PR [#17](https://github.com/tumourlove/monolith/pull/17) by **@alakangas** introduced `FMonolithMemoryHelper` (memory-budget probe + GC-throttle primitives) plus retrofits to 6 indexers (Animation, Level, MeshCatalog, Niagara, DataTable, GAS). Master independently reached the same shape for the helper + 4 of the 6 retrofits (Animation/Level/MeshCatalog/Niagara) before the PR landed; remaining gaps are the `FAssetCompilingManager::Get().FinishAllCompilation()` guard hunks for `DataTableIndexer.cpp` (line ~24) and `GASIndexer.cpp` (line ~78), which will be cherry-picked from PR #17 with @alakangas authorship preserved at release time. The PR's settings additions (`MemoryBudgetMB`, `DeepIndexBatchSize`, `PostPassBatchSize`, `GCFrequencyBatches`, `YieldTimeSeconds`, `bDeferFirstTimeIndex`, `bLogMemoryStats`) are already on master with matching field shapes.

- **Action count delta: `audio` 86 → 98, total 1274 → 1286, distinct 1270 → 1282** (with-town-gen 1319 → 1331). Verified live at v0.14.9 + Phase 3 build via `monolith_status` + `monolith_discover("audio")`. The `WITH_METASOUND` gate keeps the new actions inert (and the indexer unregistered) when MetaSound is absent.

- **Naming convention reinforcement.** The non-uniform spelling in the UE 5.7 Metasound API (`FMetaSound...` capital S for builder/asset/interface, `FMetasound...` lowercase s for document/graph/node/edge/vertex/variable/literal/class structs, `EMetasound...` lowercase s for enums) is preserved exactly in the ported code per Iron Law 1 source-verification. Documented in the v0.14.10 implementation plan at `Plugins/Monolith/Docs/plans/2026-05-03-metasound-indexer-integration.md` § 8.

- **Action count delta for PR #54: animation +1, editor +3, registrations 1290 → 1294 / in-tree active default 1286 → 1290 / distinct 1282 → 1286 / with town gen 1335 → 1339.** Verified at v0.14.10 candidate build via `monolith_status` + `monolith_discover("animation")` + `monolith_discover("editor")`. PR #54's 4 new actions are unconditional (no `WITH_*` gate) and not aliased — they bump all three count metrics by exactly +4.

- **Action count delta for PRs #55 + #56: animation +5 (`get_skeleton_preview_attached_assets`, `get_bone_ref_pose`, `get_compatible_skeletons`, `add_compatible_skeleton`, `remove_compatible_skeleton`).** Cumulative v0.14.10 deltas vs v0.14.9: registrations 1290 → 1299 (+9), in-tree active default 1286 → 1295 (+9), distinct 1282 → 1291 (+9), with town gen 1335 → 1344 (+9). MonolithAnimation row 120 → 125 (+5 from #55/#56 layered on top of +1 from #54 baseline). PR #57 does NOT change the action count (purely additive optional params on the existing `get_cdo_properties` handler), but the schema reported by `monolith_discover("blueprint")` now exposes 3 additional optional fields. All counts will be re-verified live at release-candidate build time via `monolith_status` + `monolith_discover("animation")` + `monolith_discover("blueprint")`.

- **PR #54 source-verified against UE 5.7 API surface.** `IAnimationDataModel::IsValidBoneTrackName` (`IAnimationDataModel.h:243`), `IAnimationDataModel::GetBoneTrackTransforms` (`IAnimationDataModel.h:192`, 2-arg overload), `IAnimationDataModel::GetBoneTrackNames` (`IAnimationDataModel.h:257`), `FRequestPlaySessionParams` constructor defaults (`PlayInEditorDataTypes.h:130`: `SessionDestination=InProcess`, `WorldType=PlayInEditor`), `ULevelEditorSubsystem::EditorRequestBeginPlay` canonical PIE pattern (`LevelEditorSubsystem.cpp:264-277`). Maintainer hardening uses `FLevelEditorModule::GetFirstActiveViewport` + `GUnrealEd->RequestPlaySession` instead of `GEditor->RequestPlaySession` to keep the in-viewport pin authoritative.

- **PRs #55 / #56 / #57 source-verified against UE 5.7 API surface.** PR #55: `FPreviewAssetAttachContainer::Num()` + `operator[](int32)` (`PreviewAssetAttachComponent.cpp:62/71`, returns `const FPreviewAttachedObjectPair&`), `FPreviewAttachedObjectPair::GetAttachedObject()` + `AttachedTo` field, `FReferenceSkeleton::GetRefBonePose` / `GetParentIndex` / `GetNum` / `GetBoneName` / `FindBoneIndex` (canonical hierarchy walk, mirrors `ClothingSimulation.cpp:111` + `IKRetargetDetails.cpp:71`). The deprecation note at `SkeletalMesh.h:1811` applies only to the `USkeletalMesh`-side `PreviewAttachedAssetContainer` mirror — PR #55 reads from `USkeleton`, the canonical (non-deprecated) home. PR #56: `USkeleton::AddCompatibleSkeleton(const USkeleton*)` (`Skeleton.h:741`, `ENGINE_API`-exported, impl `Skeleton.cpp:276`), `USkeleton::RemoveCompatibleSkeleton` raw-ptr overload (`Skeleton.cpp:286`), `USkeleton::GetCompatibleSkeletons()` returning iterable `TSoftObjectPtr<USkeleton>` container. PR #57: `TFieldIterator<FProperty>` (canonical `EFieldIterationFlags` walker at `CoreUObject/Public/UObject/UnrealType.h:7023`, mirror of `NiagaraNodeConvert.cpp:801`), `FProperty::GetMetaData(TEXT("Category"))` (canonical pattern `PropertyHandleImpl.cpp:3111`), `FString::Contains(..., ESearchCase::IgnoreCase)` (engine-stable since UE 4.x). No deprecated symbols touched.

### Contributors

Huge thanks to **@alakangas** for both PRs ([#17](https://github.com/tumourlove/monolith/pull/17) memory helper + indexer retrofits, [#18](https://github.com/tumourlove/monolith/pull/18) MetaSound indexer + introspection actions). Quadruple thanks to **@MaxenceEpitech** for shipping [PR #54](https://github.com/tumourlove/monolith/pull/54), [PR #55](https://github.com/tumourlove/monolith/pull/55), [PR #56](https://github.com/tumourlove/monolith/pull/56), and [PR #57](https://github.com/tumourlove/monolith/pull/57) — the bone-track discovery action + deprecated-API rewrite of `get_bone_track_keys` + PIE/console action triplet (PR #54), the two skeleton-introspection actions (PR #55, including the UE 5.7 `FPreviewAssetAttachContainer` API surface fix), the three `CompatibleSkeletons` actions (PR #56), and the three `get_cdo_properties` filter parameters (PR #57). Author attribution preserved on every cherry-picked commit.

## [0.14.9] - 2026-05-03

### Added

- **`editor.run_python` + `editor.load_level` actions** — Issue [#50](https://github.com/tumourlove/monolith/issues/50), ported from @JCSopko's fork. `run_python` wraps `IPythonScriptPlugin::Get()->ExecPythonCommandEx(FPythonCommandEx&)`, supporting all three execution modes (`execute_file`, `execute_statement`, `evaluate_statement`) and the `EPythonFileExecutionScope` Private/Public split. Returns success status, captured Python log output (typed: info/warning/error), and the evaluated result for `evaluate_statement` mode. `load_level` wraps `ULevelEditorSubsystem::LoadLevel(AssetPath)` — single-arg map swap with native semantics (closes current persistent level without saving). Together these replace common fallback-to-other-MCP patterns; agents now stay inside Monolith for Python escape-hatch + map swapping in integration tests / automation flows. `Monolith.uplugin` enables `PythonScriptPlugin` (engine-shipped Experimental plugin requires explicit enable). By **@JCSopko**.
- **`animation.copy_bone_pose_between_sequences` action** — PR [#51](https://github.com/tumourlove/monolith/pull/51) by **@MaxenceEpitech**. Reads the evaluated pose (track + ref-pose fallback) from a source `UAnimSequence` at a given time and writes it as keys to a destination sequence for a list of bones. Closes the workflow gap where `get_bone_track_keys` returned "not found" for bones imported with sparse keys (no explicit track). Per-bone skip with structured `reason` rather than hard-fail. Maintainer follow-ups on top of the PR: (1) replaced the UE 5.6-deprecated `GetBoneTransform(FTransform&, FSkeletonPoseBoneIndex, double, bool)` overload with the non-deprecated `FAnimExtractContext(SourceTime)` form (drops `PRAGMA_DISABLE_DEPRECATION_WARNINGS` shim that was masking a real warning); (2) added `bone_names` array element-type guard — non-string entries now return `-32602` with index in the message, instead of silently skipping via `Val->AsString()` returning empty; (3) added `SourceTime` clamp to `[0, GetPlayLength()]` with `original_source_time` + `clamped_source_time` surfaced in the response when the input was adjusted (out-of-range values previously sampled undefined positions).

### Fixed

- **`blueprint.set_pin_default` now writes `Pin->DefaultObject` for class-typed (`PC_Class`) and object-typed (`PC_Object`) pins** — previously wrote the value string into `Pin->DefaultValue` only, never touching `Pin->DefaultObject`. UE's reflection reads `DefaultObject` for ref-typed pins, so authored class/object values silently reverted to the pin's static base type at compile/load. Fix introduces `MonolithBlueprintInternal::ResolveDefaultObjectForPin` (header-only inline helper) accepting native class names with `A`/`U` prefix retry (PC_Class only), object/class paths via `StaticLoadObject`, and Blueprint class paths with auto `_C`-suffix retry. Type-constraint enforced against `Pin->PinType.PinSubCategoryObject`. Cross-category mismatch (class pin given an instance, object pin given a UClass) returns an error. `set_pin_defaults_bulk` and `batch_execute` inherit the fix automatically (already delegate to `HandleSetPinDefault`). Soft refs (`PC_SoftObject` / `PC_SoftClass`) and `PC_Interface` fall through to the existing primitives path; deferred until concrete demand surfaces. PR [#52](https://github.com/tumourlove/monolith/pull/52), Issue [#53](https://github.com/tumourlove/monolith/issues/53), by **@danielandric**.

### Internal

- **`AbilityTags` reflection lookup future-proofed against the engine's gradual rename to `AssetTags`** (Issue [#31](https://github.com/tumourlove/monolith/issues/31)) — `MonolithGAS::FindAbilityAssetTagsProperty` header-only helper tries the modern `AssetTags` name first, falls back to the legacy `AbilityTags`. Both names work at UE 5.7, but a future engine version may complete the removal — the helper logs a one-time warning if neither is found, so the next break is loud rather than silent. Replaces the two direct `FindPropertyByName(TEXT("AbilityTags"))` call sites in `MonolithGASInspectActions.cpp` and `MonolithGASScaffoldActions.cpp`. No behavioural change at UE 5.7.
- **macOS build CI workflow scaffold** (Issue [#25](https://github.com/tumourlove/monolith/issues/25)) — `.github/workflows/macos-build.yml` triggers on `v*` tag pushes and dispatches a macOS build job to a self-hosted runner labelled `[self-hosted, macOS, monolith]`. Mirrors `make_release.ps1`'s release-build env (`MONOLITH_RELEASE_BUILD=1`, `-DisableUnity`, sibling-strip, `Installed: true` patch, SHA256 emit, `softprops/action-gh-release` attach). Two known gaps documented in workflow-header comments: (1) self-hosted runner provisioning (Mac with UE 5.7 + Xcode CLT + ~150GB disk + `UE_57` env var), (2) project-shell gap — `MonolithEditor.Target.cs` does not exist in the plugin source tree, so the workflow fails-fast with two resolution paths (commit a CI-only project shell into the plugin repo, OR point a parent-project env var at a parent project on the runner). Workflow does not fire until the runner is online.

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
- **Baseline vitals AttributeSet** (F4) — a project AttributeSet with six `FGameplayAttributeData` (Health/MaxHealth plus two more stat pairs), `PreAttributeChange` clamps, `PostGameplayEffectExecute` re-clamps, REPNOTIFY_Always replication. Additional resistance attributes deferred.
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
