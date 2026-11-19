# Changelog

## [Unreleased]

## [0.25.0] - 2026-11-20

> **Action count is approximate.** **~1,500+ actions across 27 in-tree namespaces** - query `monolith_discover()` for the live figure.

**Behaviour changes in this release** (details in the bullets below):
- `blueprint set_property_at_path` now refuses to write into an object outside the target asset's package unless `allow_external: true` is passed.
- `editor run_python` keeps its top-level globals by default; the post-call cleanup is opt-in via `clean_globals: true`.
- `blueprint set_function_params` now reconstructs the function's entry/result nodes and every loaded call site after adding parameters.
- `DatabasePathOverride` is now honoured; anyone who had set it will see `ProjectIndex.db` open at that path.
- `ui mvvm_add_viewmodel` defaults to the `CreateInstance` creation type when the class allows it (the stock MVVM panel defaults to the first allowed type, usually `Manual`).
- New optional plugin reference `ModelViewViewModel` in `Monolith.uplugin`. The MVVM actions exist only in source builds of a project that enables that plugin; release binaries ship without them.
- New plugin reference `GameplayTagsEditor` in `Monolith.uplugin` (for the `gameplay_tags` write actions). It is an engine plugin enabled by default, so nothing changes unless a project has turned it off.

### Added

- **`gameplay_tags` namespace (new `MonolithGameplayTags` module, 14 actions; PR #170 by @yashabogdanoff).** Tag references are read from the Asset Registry `SearchableName` graph, the data the editor itself uses for "still referenced by" - live, no reindex, and covering tags wherever they are serialized (properties, graph pins, level components, containers). On Lyra 5.7 that is 743 references on 197 of 448 tags, against 33 on 29 from the Asset Registry metadata that `gas search_tag_usage` reads; on Lyra 5.8 every answer matches the engine's experimental `GameplayTagsToolset` package for package. Read: `ping`, `find_references`, `get_asset_tags`, `list`, `get_tag_info`, `find_unused` (dead tags and dead branches, checked against assets, project C++ string literals and `(TagName="...")` config settings, native tags reported apart), `find_broken`. Write, through `IGameplayTagsEditorModule`: `list_sources`, `add_source`, `add`, `update`, `remove` (refused while anything uses the tag, `dry_run`), `rename` (redirect-based, reports what still spells the old name, `dry_run`), `move`. Every refusal comes back with its reason, which the engine module only shows as a toast. Works around two engine behaviours: `UpdateTagInINI` cannot change a dev comment, and `RenameTagInINI` with children leaves the renamed parent undeclared. Gated by the new `bEnableGameplayTags` setting and independent of `bEnableGAS`. Builds on 5.7 and 5.8 with no version gate. Automation tests `Monolith.GameplayTags.*`. The overlapping `gas` actions (`add_gameplay_tags`, `remove_gameplay_tags`, `rename_tag`, `get_tag_hierarchy`) and their `gameplay_tags` counterparts now cross-reference each other in their descriptions.
- **UMG MVVM support in the `ui` namespace (#173, requested by @Maxmystere).** Eight actions, registered only when the ModelViewViewModel ("UMG Viewmodel") plugin is enabled in the project: `mvvm_get_view` reads a Widget Blueprint's viewmodels, bindings (mode, source/destination paths, conversion functions and their argument pins, enabled/compile flags, compiler messages), events, conditions and view settings, optionally filtered to one `widget`; `mvvm_add_viewmodel` / `mvvm_remove_viewmodel`, `mvvm_add_binding` (optional execution mode and forward conversion function) / `mvvm_remove_binding`, and `mvvm_add_event` / `mvvm_remove_event` author them through `UMVVMEditorSubsystem`, transacted and refused during PIE; `mvvm_set_field_notify` toggles FieldNotify on a Blueprint variable so Blueprint viewmodels can be built end to end. `mvvm_add_viewmodel` without `creation_type` uses `CreateInstance` when the class allows it, otherwise the engine's first allowed type, and reports `creation_type_source`. `get_widget_bindings` output is unchanged.
  - **Build:** MonolithUI gates MVVM on `.uproject` enablement (`WITH_MVVM`), never disk presence, and `MONOLITH_RELEASE_BUILD=1` forces it off, so release binaries carry no MVVM actions and no MVVM imports. `ModelViewViewModel` is an `Optional` plugin reference in `Monolith.uplugin`, and `ModelViewViewModel`, `ModelViewViewModelBlueprint` and `ModelViewViewModelEditor` are release import-smoke sentinels.
- **`editor pie_inject_key` (#162 item 9, reported by @altarfinch).** Injects raw key events into a running PIE player controller through `APlayerController::InputKey`. Each event carries the primary input device of the controller's platform user, so Enhanced Input mappings (chorded actions, modifiers, triggers), legacy key bindings and Blueprint Debug Key events receive it. Supports `pressed`, `released`, `tap` (the release goes out on a later frame, after `hold_seconds`) and `axis` (analog keys; with `hold_seconds` the value is re-sent every frame and then set back to zero). Chord `modifiers` go down before the key and come up after it, at least two frames apart. Pending steps are dropped when PIE ends.
- **`blueprint measure_graph_nodes` (#161, reported by @altarfinch; follows #132).** Real node sizes and pin positions, read from the Blueprint editor's node widgets at zoom 1:1: per-node `w`/`h` (with the old `est_w`/`est_h` estimate for comparison) and per-pin row centres relative to the node. Opens the graph in the Blueprint editor and restores the view afterwards. Fails cleanly without a UI (`-nullrhi`, commandlets).
- **`material measure_graph_nodes` (#161, @altarfinch).** The same measurement for material and material-function graphs, through the material editor's graph; node ids are expression names.
- **Blueprint function parameter and pin-structure actions (#162 items 1-4, reported by @altarfinch).** All seven are also `batch_execute` ops.
  - `remove_function_param` removes a function input or output; entry/return nodes and call sites in every loaded Blueprint are rebuilt, and call-site pins orphaned by the removal are dropped so the compile no longer fails on a stale pin. Get/Set nodes of the removed input left inside the function are reported, with the connected ones (which block compilation) listed separately.
  - `reorder_function_params` reorders a function's inputs or outputs; call sites follow.
  - `refresh_all_nodes` runs Refresh All Nodes with an optional compile.
  - `split_pin` / `recombine_pin` split and recombine struct pins.
  - `add_pin` / `remove_pin` drive Add Pin / Remove Pin on `IK2Node_AddPinInterface` nodes (Multiply, Add, AND/OR, Append, Sequence, Select, Make Array, ...) and Switch on Int/String/Name, with a `count` param so `A * B * C` is one node.
- **Blueprint event, delegate and async node authoring (#162 items 5-7, reported by @altarfinch).**
  - `add_custom_event_input` adds an input to a Custom Event, typed from a type token or copied from an existing pin (`like_pin: {node, pin}`), and reconstructs the Blueprint's own call sites.
  - `add_event_for_delegate` creates a Custom Event with the signature of a Bind Event / Assign / Unbind node's delegate (or of any delegate input pin), already wired to the delegate pin.
  - `add_create_delegate_node` places a Create Event node bound to a named function in one call; it links the delegate output before setting the function, so `HandleAnyChange` cannot clear the selection, and reads the binding back.
  - `add_async_action_node` places async action nodes from a static factory via `UK2Node_AsyncAction::InitializeProxyFromFunction`, including `BlueprintInternalUseOnly` factories; other `UK2Node_BaseAsyncTask` subclasses via `node_class`.
  - `add_construct_object_node` places Construct Object from Class / Create Widget (or any `UK2Node_ConstructObjectFromClass` subclass) with the class set and exposed-on-spawn pins rebuilt. Classes the node cannot compile (abstract, non-`BlueprintType`, `DontUseGenericSpawnObject`, Actor / ActorComponent for Construct Object) are refused up front with the reason. `node_id` retargets an existing node; unlinked spawn pins left over from the previous class are removed, linked ones are kept and reported.
- **`blueprint swap_component_class` (#162 item 8, reported by @altarfinch).** Changes the class of a Blueprint-added (SCS) component in place, to a subclass or (with `allow_unrelated`) another class of the same kind (scene/non-scene). The variable name, variable GUID, child nodes and attach socket are kept, so graph references keep working. Properties the designer changed on the old template are copied onto the new one, and the rest take the new class's defaults. Inheritable Component Handler overrides in child Blueprints, loaded or not, are rebuilt for the new class with their per-child values instead of being silently discarded on the next compile. The Blueprint and its children are compiled, and the response includes each compile report. `preserve_level_instances=true` re-applies per-instance values on actors in the open level.
- **`blueprint dump_instance_component_values` / `restore_instance_component_values` (#162 item 8, @altarfinch).** Dump the per-instance component property overrides of an actor placed in the editor level (editable properties that differ from the component's archetype, as export text), and re-apply them later, for example after `swap_component_class` reinstances the actor. Values are applied in one batch, so the construction script reruns once, and every value is verified on the live component afterwards; anything that did not stick is reported as `did_not_persist`. Transient and non-editable properties are skipped. Values that cannot round-trip (instanced subobjects, fixed-size arrays, EditConst, DisableEditOnInstance) are listed rather than silently dropped.
- **Curve assets: `blueprint create_curve_asset`, `get_curve_asset_keys`, `set_curve_asset_keys`.** Create a standalone CurveFloat, CurveVector or CurveLinearColor asset (`curve_type: float|vector|linear_color`), optionally seeded with keys; the path and every key are validated before anything is created, an existing asset is never overwritten, and the save goes through the central save helper. Per-channel key read/write for CurveFloat (`value`), CurveVector (`x|y|z`) and CurveLinearColor (`r|g|b|a`, long names accepted), with the same key JSON as `set_curve_float_keys` plus a per-key `channel`; `replace` clears only the channels being written; writes are undoable.
- **`animation copy_sequence_curves` / `audit_sequence_curves`.** Copy float curves (keys, curve flags, editor colour) from one AnimSequence to many targets (`targets[]` and/or `target_folder`) through the animation data controller, with an optional name filter, `overwrite`, `retime: scale` to the target play length, `dry_run`, and per-target added/overwritten/skipped reporting, in one undo transaction per call. The read-only audit checks AnimSequences (`assets[]` and/or `folder`) against `required_curves[]` and/or a `reference` sequence's curve set and reports per-sequence missing / empty / extra curves, plus how many sequences miss each required curve.
- **`animation` skeleton authoring: slots, blend profiles, curve metadata (12 actions).**
  - Slots: `get_skeleton_slots`, `add_skeleton_slot` (creates the slot group if missing, moves an existing slot when a group is given), `remove_skeleton_slot` (one slot, or a whole group with its slots; the engine default group and slot are protected), `set_slot_group`. Montage slots used by slot nodes can be registered on a skeleton without the Persona Anim Slot Manager.
  - Blend profiles: `get_blend_profiles`, `create_blend_profile` (`time_factor` | `weight_factor` | `blend_mask`), `set_blend_profile_bones` (per-bone scale with `recurse` / `remove`, optional `clear` and `mode` change with the same entry re-normalisation the skeleton editor does), `remove_blend_profile`.
  - Curve metadata: `get_skeleton_curve_metadata` (material / morph-target flags, linked bones, max LOD), `add_skeleton_curve` / `remove_skeleton_curve` (single name or bulk `curves` array), `set_skeleton_curve_metadata` (flags, `linked_bones` for LOD culling, `max_lod`; registers the curve if missing). `get_skeleton_curves` stays as the names-only listing.
- **`animation` physics asset structural authoring (6 actions).** `create_physics_asset` generates bodies and constraints from any skeletal mesh with the editor's generator (`body_params` exposes min bone size, primitive type, vertex weighting, constraint creation and collision defaults; the asset is assigned to the mesh only with `assign_to_mesh: true`). `add_physics_body` fits a capsule/sphere/box/convex body to one bone's skinned vertices, constrains it to the nearest physical ancestor and re-parents descendant constraints, as the Physics Asset Editor does. `remove_physics_body` removes attached constraints too. `add_physics_constraint` / `remove_physics_constraint` work by index, name or body pair. `set_body_collision` enables or disables collision for one body pair. Ragdoll and hit-reaction setups can be built without opening the Physics Asset Editor. New private deps: `PhysicsUtilities`, `MeshUtilitiesEngine` (engine Developer modules, always present).
- **`animation` Control Rig graph editing (4 actions).** `remove_control_rig_node`, `break_control_rig_link` (one link by source/target pin, or every link on a pin), `set_control_rig_pin_default` (edit defaults on existing nodes, not only at creation), `add_control_rig_variable` (member variables of primitive, math-struct, `RigElementKey`, any struct/enum path, or arrays of those; unsupported types are refused before they reach the engine's fatal type check). All accept an optional `graph_name` and go through the RigVM controller with undo.
- **`blueprint set_property_at_path` edits properties on sub-objects inside an asset (requested by @k-s-s on the Ideas board).** For example editor graph nodes such as Mutable `CustomizableObject` nodes (`Source.Nodes[3].GroomData.GroomComponentName`); Monolith has no Mutable dependency, the walker follows object references generically.
  - New array-element selectors for arrays of object references: `[@ObjectName]` matches by object name (case-insensitive), and `[@class=ClassName]` matches by exact class, with or without the `U` prefix. The class match must be unique.
  - When the leaf lives on a sub-object, that object gets `Modify()`, `PreEditChange`, `PostEditChangeChainProperty` and `MarkPackageDirty`, and the result reports it as `leaf_owner`.
  - New params `allow_external` (see Changed) and `new_element_value` (see Fixed).

### Changed

- **`editor run_python` can clean up its top-level globals; the cleanup is opt-in (PR #166 by @itismyfield).** With `clean_globals=true`, the names a call creates in the shared console globals are removed afterwards, `builtins._` is cleared and `gc.collect()` runs, so UObjects (and their worlds) referenced only from those names can be garbage collected. Inline code runs in the editor's shared console globals whatever `file_scope` says, and `FPyReferenceCollector` keeps every UObject a live Python wrapper points at reachable. The default is `false`: globals persist across calls as before, which multi-call workflows and self-unregistering Slate tick callbacks rely on. `keep_globals=false` is accepted as an alias and is ignored when `clean_globals` is given. The `python_script` and `stages` Python hooks of `run_pie_smoke` / `capture_pie_movement_clip` and the in-session probes always clean up. Values stored outside the top-level globals (module attributes, closures, registered callbacks) are not touched.
- **Behaviour change: `blueprint set_property_at_path` stays inside the target asset's package.** Writes refuse to follow a reference into an object in another package (for example a referenced material or another data asset). A path such as `SomeMaterialRef.SomeProp`, which used to write into the referenced asset without saying so, now errors unless `allow_external: true` is passed; with the flag the external package is marked dirty and listed in `external_objects`, and `save=true` still saves only the target asset's package. Dry runs follow such references, list them in `external_objects`, and set `would_refuse_external`.
- **Behaviour change: `blueprint set_function_params` refreshes call sites (#162, @altarfinch).** After adding parameters it reconstructs the entry/result nodes and every call site in loaded Blueprints (the details-panel path), so callers show the new pins without a manual refresh. Previously it only marked the Blueprint structurally modified.
- **`blueprint auto_layout` gained opt-in `measured: true` (#161, @altarfinch).** With the built-in formatter it lays out from measured node sizes instead of the 300 x (50 + 24/pin) estimate, widens columns to fit wide nodes, and lines up exec pins instead of node tops; it falls back to the estimate with a warning when measuring is unavailable. The response gains `sizes_source`. Without the flag, layout is unchanged.
- **Node and pin not-found errors name where to look (requested by @k-s-s on the Ideas board).** `connect_pins`, `disconnect_pins`, `set_pin_default` and the new pin actions say which graph was searched, name the graph(s) that actually contain the node ID, and list input and output pins separately.
- **Docs: `127.0.0.1:9316` everywhere (#163 follow-up).** `SECURITY.md`, `Docs/SIBLING_PLUGIN_GUIDE.md` and the wiki pages (`Connecting-Your-AI.md`, `Installation.md`, `FAQ.md`, `Sibling-Plugin-Guide.md`) use `127.0.0.1` instead of `localhost`.
- **Docs: Reflection Intel settings file (#165 follow-up).** `SPEC_MonolithReflectionIntel.md` names `<Project>/Config/DefaultMonolithSettings.ini` (the class is `config=MonolithSettings, defaultconfig`, a separate branch from `DefaultMonolith.ini`) and Project Settings > Engine as its location.

### Fixed

- **The level indexer no longer crashes the editor on Linux and macOS (PR #167 by @PumpkinSpicePanda; #169 by @whalemenace independently found and fixed the same crash, and the merged code carries #169's fuller explanation).** `FMonolithMemoryHelper::GetCurrentMemoryUsageMB` called `FPlatformProcess::GetApplicationMemoryUsage`, which only Windows implements; on every other platform the generic version is a fatal error (`not implemented on this platform`), so the first memory snapshot of the level pass took the editor down. Non-Windows platforms now read the process's resident memory from `FPlatformMemory::GetStats().UsedPhysical`, the same figure `ShouldThrottle` already uses.
- **Per-project index location and `DatabasePathOverride` (PR #168 by @whalemenace).** `ProjectIndex.db` stays in `Plugins/Monolith/Saved/` when the plugin sits inside the project, and moves to `<Project>/Saved/Monolith/ProjectIndex.db` when the plugin is mounted from outside it (engine plugin or a shared `AdditionalPluginDirectories` folder), so two projects sharing one plugin copy no longer share one index. **Behaviour change:** `DatabasePathOverride` was previously ignored and is now honoured, so anyone who had set it will see the index open at that path instead of the plugin `Saved/` folder. Known limitation: the offline `monolith_query.exe` and `monolith_offline.py` still read only the plugin `Saved/` DB; `monolith_query.exe --project-db <path>` points it at a relocated index, `monolith_offline.py` has no override yet (documented in `SPEC_MonolithIndex.md`).
- **`include_ui` PIE clip frames read the game viewport's own widget (PR #171 by @whalemenace).** With `include_ui=true`, `capture_pie_movement_clip` frames are now captured from the PIE game viewport widget with `FSlateApplication::TakeScreenshot` rather than a global `FScreenshotRequest`, which whichever viewport drew first serviced - on a seated editor the level editor viewport, saving its own picture under the requested name. UI frames get the same uniformity accounting as scene-only frames, and a window Slate will not draw (minimized, hidden, modal, zero size) is refused.
- **`blueprint set_property_at_path`: four problems with map entries (#172, reported by @dlikas).**
  - A dry run now reports the live value of an existing entry's leaf as `current` (it was reading a default-initialised scratch value, so `-0.45` showed as `0.000000`, `True` as `False`, a name as `None`).
  - Numeric text the engine import only partly parses is now checked before the write. With `strict: true`, `"abc"` for a float or `"12abc"` for an int is an error, and a strict dry run that would fail returns an error. Without `strict` the write behaves as before, and the result carries a warning.
  - A dry run now honours `create_missing_keys`: it walks the rest of the path inside a scratch entry, reports `would_create_key: true`, and leaves the map alone.
  - A leaf-only create of a new map entry now lists the entry's fields that stayed at their defaults (`defaulted_fields`, dotted, e.g. `Value.Type`) and adds a warning. The new `new_element_value` param initialises the created entry (ImportText or a JSON object) before the leaf write. `created_missing_key` now reports whether a key was actually created; it used to echo the flag.
- **Bulk-fill dry runs report the live value.** `InspectTree`, used by every `*_bulk_fill` / `set_cdo_properties` dry run, now reports the live value as `current` instead of the default.
- **`blueprint add_node` `VariableGet` / `VariableSet` on a function input binds to the parameter (#159, reported by @altarfinch).** The node is scoped to the function (`SetLocalMember`, as the editor's parameter drag does) instead of coming out as a pinless "Get" node.
- **`blueprint connect_pins` handles nodes with a same-named input and output (#160, reported by @altarfinch).** The source is resolved among output pins and the target among input pins, so a node such as an async action with a `Stream` input and output can be wired; ends passed the wrong way round are detected and linked correctly. `disconnect_pins` and `set_pin_default` resolve same-named pins by direction too.
- **Save failures name what was kept (#164 follow-up).** Niagara `add_emitter`, `create_system`, `create_stateless_emitter`, `create_script_from_hlsl`, `create_npc`, `create_effect_type`, `save_emitter_as_template` and `save_system`, and the UI font/gradient/shadow/texture-ingest actions and UI spec builder, now say the edit or asset exists in memory but was not saved to disk, followed by the save helper's reason, instead of the stale "UPackage::SavePackage failed" text.
- **`gameplay_tags update` reports ini write failures.** The bool from `TryUpdateDefaultConfigFile` was ignored, so a read-only or not-checked-out tag ini reported success. It now returns an error naming the files that could not be written (and any that were).
- **`MonolithGameplayTags` full-unity collision.** `TagsNamespace` was defined in an anonymous namespace in three `.cpp` files of the module, which collides under full unity; it now lives once in `MonolithGameplayTagsInternal.h`.
- **The MCP proxies default to `http://127.0.0.1:9316/mcp` instead of `http://localhost:9316/mcp` (#163, @itismyfield).** On Windows `localhost` resolves to `::1` first, and the editor's listener does not accept on `::1`, so each connection waited about two seconds for the refused IPv6 attempt before falling back to `127.0.0.1`. `Scripts/monolith_proxy.py` (urllib) paid that on every request, including the health check; `Tools/MonolithProxy` (WinHTTP) at least on the first request of each process. `Templates/.mcp.json.example` and the docs use the same address. `MONOLITH_URL` still overrides the default.
- **The plugin's default settings file is read (#165, @itismyfield).** `UMonolithSettings` is `config=Monolith`, and the engine loads a plugin's own branch from `Config/Base<Name>.ini` / `Config/Default<Name>.ini`; any other file in the plugin's `Config/` folder only modifies a branch of the same name, and `MonolithSettings` names no branch, so `Config/MonolithSettings.ini` was skipped and editing it did nothing. It is now `Config/DefaultMonolith.ini`. Its module toggles used names that match no property (`bBlueprintEnabled` and so on; the properties are `bEnableBlueprint` and so on), `LogVerbosity=3` would have parsed as `VeryVerbose` under the current four-value enum, and `DatabasePathOverride=` was not a valid `FDirectoryPath` value; these are corrected so the file restates the C++ defaults and loading it changes no behaviour. The docs now point at Project Settings, where `UMonolithSettings` actually appears.
- **Write actions check the asset out before saving (#164, @itismyfield).** About sixty save sites across every namespace called `UPackage::SavePackage` directly, which never talks to source control: under a provider that keeps files read-only until checked out the save failed, and without one the file changed on disk while source control still reported it unmodified. They now go through `FMonolithAssetUtils::SaveAssetPackage`, which wraps `UEditorLoadingAndSavingUtils::SavePackages` (the call `UEditorAssetSubsystem::SaveLoadedAsset` uses): it checks the package out, saves without dialogs, marks a new file for add, and returns whether the package reached disk. Map packages save through the editor's world path, so their external packages are saved and checked out with them. `editor save_packages` keeps its own handling from #147, and the UI test fixtures keep the direct save so automation runs do not touch source control.

## [0.24.0] - 2026-10-18

> **Action count is approximate.** **~1,400+ actions across 26 in-tree namespaces** — query `monolith_discover()` for the live figure.

A queue-clearing release: every open PR and issue was triaged, ten issues fixed, seven PRs landed, and a set of tooling gaps hit during real project work closed. The theme is actions that reported success while doing nothing, and editor crashes behind ordinary failures.

### Added

- **`-NoMonolithMcp` command-line switch** skips the MCP HTTP listener for one process (tools still register). Lets automated test runs (`UnrealEditor-Cmd ... -ExecCmds="Automation RunTests ..."`) stay off the port a working editor or an MCP client is using. From PR #154 by **@whalemenace**.
- **`ui create_widget_blueprint` can create Editor Utility Widget Blueprints.** `editor_utility=true` creates a `UEditorUtilityWidgetBlueprint` (default parent `EditorUtilityWidget`) with the requested `root_widget`; it is enabled automatically when `parent_class` derives from `EditorUtilityWidget`. Previously the action always produced a plain `UWidgetBlueprint`, which the editor does not treat as an editor utility even with an EUW parent.
- **`blueprint set_function_flags` (#150, reported by @altarfinch).** Edits pure, const, Call In Editor, access specifier, category and description on an existing function graph; only the fields passed change. Writes the function entry node's extra flags and metadata plus the skeleton `UFunction` (as the My Blueprint details panel does), marks the Blueprint structurally modified, and reconstructs the Blueprint's own call sites when purity changes. Also available as a `batch_execute` op.
- **`blueprint add_event_graph` (#150, reported by @altarfinch).** Adds a new Event Graph (ubergraph page), matching **Add New > Event Graph**. Optional `name` (validated with the Kismet name validator, default a unique `NewEventGraph`); the page reuses the schema of the Blueprint's existing event graph.
- **`input add_key_mapping` / `remove_key_mapping` (#148, reported by @grzegorzkarwalski-del).** Add a new action + key row to an Input Mapping Context (via `UInputMappingContext::MapKey`; returns the new `mapping_index` and any `duplicate_indices`), or remove one row by `mapping_index` or `(input_action, key)`. Removal keeps the remaining rows in order rather than using `UnmapKey`, which swap-removes. Both take `save`. The `input` namespace is now 15 actions.
- **By-reference and const function parameters.** `blueprint set_function_params` inputs accept optional `is_reference` / `is_const` (a `const FFoo&` parameter is both; `is_const` requires `is_reference`, and both are rejected on outputs). `blueprint add_function` takes optional `prototype_class` + `prototype_function` (or a full `/Script/Module.Class:Function` path in `prototype_function`) and seeds the entry/result pins from that `UFunction`, copying reference and const flags exactly; an unresolved prototype is an error, never a silent fallback to an empty signature. The response lists the seeded `params`.
- **`blueprint set_property_access_path`.** Re-points an existing Property Access node (`UK2Node_PropertyAccess`) in place: writes the path the same way `add_property_access_node` does, refreshes the display path, and reconstructs the node so `Value` re-resolves and keeps its links where it can. Finds nodes in nested graphs such as state-machine transitions; `node_guid` disambiguates a `node_id` that occurs in several graphs. Returns `old_path` and `path`.
- **`blueprint get_curve_float_keys` / `set_curve_float_keys`.** Read and write the keys of a standalone `UCurveFloat` asset, whose `FloatCurve` is not reachable from Python. Keys carry `time`, `value`, `interp` (`linear` / `constant` / `cubic`), `tangent_mode` and optional `arrive_tangent` / `leave_tangent`. `replace` (default true) clears the curve first; otherwise keys at matching times are updated. All keys are validated before the curve is modified; the write is undoable and marks the package dirty.
- **`blueprint get_cdo_properties` takes `property_names`** (case-insensitive exact names; unmatched names come back in `missing_property_names`) and accepts `name_filter` as an alias of `name_pattern`.

### Fixed

- **The PIE pre-flight also reports Dirty Blueprints whose last compile failed.** `ScanErroredBlueprints` (behind `list_errored_blueprints`, `start_pie`, `run_pie_smoke`, `sample_pie_timeseries`) only counted `BS_Error`, but the engine also recompiles every non-data-only `BS_Dirty` Blueprint before play and raises the blocking error dialog when that fails, so `on_compile_errors="refuse"` could still let PIE hang the editor. Those Blueprints are now reported with `status: "dirty_with_errors"`. From PR #151 by **@whalemenace**.
- **`source search_source` validates the cursor before checking the source DB**, so a bad cursor is `INVALID_CURSOR` even on a machine without `EngineSource.db`. From PR #152 by **@whalemenace**.
- **ReflectionIntel tests stage fixtures under an absolute path**, so they scan their fixtures on layouts where the project and engine share a root (Linux/Mac source builds, same-drive Windows). From PR #153 by **@whalemenace**.
- **`MonolithChooserReadActions.cpp` includes `EnumProperty.h` and `TextProperty.h`**, so it compiles without unity or a shared PCH. From PR #144 by **@itismyfield**.
- **A missing required parameter is reported as invalid params (`-32602`).** The dispatcher's required-param check returned its error with no code, so it surfaced as `-32603` (internal error) for every action; clients that branch on the code treated a caller mistake as a server fault.
- **Auto-updater source and unrelated ZIP archives are rejected.** Update discovery now accepts only explicit `Monolith-*.zip` release assets and fails closed when a release exposes only GitHub's `zipball_url` or another ZIP, because repository source archives and unrelated artifacts do not carry the precompiled plugin binaries required by Blueprint-only installations. Thanks **@kunkunGames** (#104).
- **`animation set_anim_node_pin_binding` keeps `ArrayIndex` on array-element pins.** Every write reset `FAnimGraphNodePropertyBinding::ArrayIndex` to `-1`, so pins such as `BlendWeights_N` / `DesiredAlphas_N` bound the whole array, compiled with `Cannot copy property (float -> TArray)` warnings, and never drove their element. New optional `array_index`; when omitted, a rebind keeps the existing index and otherwise it is derived from the pin name as the engine binding widget does. Binding an element now drops a whole-array binding of the same property and vice versa, as in the editor. `get_anim_node_pin_bindings` reports `array_index` per binding.
- **`material build_material_graph` / `clear_graph` no longer convert `FromMaterialExpression` materials to DefaultLit.** Both restored the shading model from `GetShadingModels().GetFirstShadingModel()`, which is the graph-derived model, not the authored one. They now save and restore the raw `UMaterial::ShadingModel`. Setting `shading_model: FromMaterialExpression` through `set_material_property`, `batch_set_material_property`, `create_material` or `create_pbr_material_from_disk` no longer hits the engine's `check(InShadingModel < MSM_NUM)` in `UMaterial::SetShadingModel`; the hidden `MSM_NUM` value is rejected.
- **Scripted connections to a material output now compile.** `connect_expressions`, `build_material_graph`, `replace_expression` and `create_pbr_material_from_disk` wired material property inputs without clearing `UseConstant`, so an output never wired in the editor kept compiling its constant while the wire showed in the graph. The flag is now cleared on connect, as the material editor does.
- **`niagara add_module` no longer crashes the editor on a broken stack chain.** The guard only checked that the output node had a linked input, so a chain left dangling by earlier edits reached `AddScriptModuleToStack`, which creates the node and then asserts `StackNodeGroups.Num() >= 2`. The stack's parameter-map chain is now validated the way the engine walks it, and an invalid stack returns an error. `move_module` builds its node groups the way the engine does, refuses an invalid stack, puts the module back if the detach goes wrong, and re-validates after the move; `remove_module` reports `stack_valid`.
- **Offline `monolith_query.exe source read_file` honours positional line numbers and engine-relative paths.** `read_file <path> <start> <end>` silently ignored the numbers and read the first 200 lines; they are now used, and a non-integer or a value given both positionally and as `--start`/`--end` is an error. Path lookup no longer converts to backslashes before querying the index (which stores forward slashes), so `Engine/Source/...` and other suffix paths resolve. `monolith_offline.py` matches.
- **Live `source read_file` resolves relative and engine-relative paths.** The handler converted the requested path to backslashes before querying the index, which stores forward slashes, so only absolute paths that existed on disk ever resolved. It now uses the offline tool's order: forward-slash exact match, forward-slash suffix match anchored at a separator (`Engine/Source/...`, `Runtime/Engine/...`), the legacy backslash forms for older indexes, then an unindexed `Engine/...` path joined onto the engine root of an indexed engine file.
- **Source index now contains the member functions of UCLASS / USTRUCT / UINTERFACE types and allman-style definitions (#156, reported by @445052020).** The class parser started member extraction for reflected types on the line holding the body's `{`, so its brace depth never returned to zero and every member was skipped; this is why types like `UCommonUserWidget` had a class row and no function rows. Separately, out-of-line definitions were only recognised with the `{` on the same line, so allman-style `.cpp` files (the Epic standard), including constructors with initializer lists, yielded no symbols, and the call-reference pass had the same limitation, so those bodies never produced callers for `find_callers` / `find_callees`. Multi-line member declarations and multi-line `UFUNCTION(...)` specifier lists are handled too. Deprecation rows now link to the declaration they annotate in the same file. A file reachable from two module roots is indexed once per run, which removes its duplicated rows and makes `meta.total_files` equal the `files` row count. The index records `meta.parser_version` (now 2) and the editor logs a warning when the loaded index is older; run one full `source trigger_reindex` to pick up the fix.
- **`source check_deprecations` handles `Class::Method` and reports unknown names (#155, reported by @445052020).** `symbol_deprecations` stores bare names only, so a qualified name always came back `deprecated: false`, including for deprecated APIs. A qualified name is now retried with its trailing identifier; the result carries `match: "qualified"` when the linked symbol's owner matches and `match: "name"` when only the bare name matched. Every verdict includes `found`: `false` means the name is not in the source index at all, so `deprecated: false` there is not a verdict. `find_callers`, `find_callees` and `find_references` also accept `Class::Method`, narrowing to that owner when it is indexed and otherwise falling back to every match with a note.
- **`blueprint set_pin_default` now writes Text pins (#145, reported by @altarfinch).** The value went into `Pin->DefaultValue`, but the editor and compiler read `DefaultTextValue` for PC_Text pins, so the call reported success while the pin stayed empty. Text pins now go through `UEdGraphSchema_K2::TrySetDefaultValue` (plain strings become a text literal; `INVTEXT` / `NSLOCTEXT` / `LOCTEXT` literals are parsed), and the response reports `default_text`. `get_node_details`, `get_graph_data` and `resolve_node` expose a `default_text` field on text pins so the read-back matches what compiles.
- **`blueprint set_pin_default` can clear object and class pins (#146, reported by @altarfinch).** `None`, `null`, an empty string or JSON null now clears PC_Object, PC_Class, PC_Interface, soft object and soft class pins through the K2 schema (`DefaultObject` and `DefaultValue` reset together) and returns `cleared: true`. Previously `None` was rejected as a bare name and an empty value failed with "Missing required parameter: value". Empty values are also accepted on text, string and name pins; other pin types still require a value.
- **`blueprint get_blueprint_info` and `get_components` agree on `component_count` (#158, reported by @445052020).** `get_blueprint_info` subtracted the default scene root whenever `GetDefaultSceneRootNode()` was non-null, including the orphaned root UE leaves behind once the first scene component replaces it, so a Blueprint with one component reported 0. Both actions now share one definition: SCS-added components, excluding the default scene root only when it is actually in the SCS node list. `get_components` no longer counts a live `DefaultSceneRoot` (it still appears in the `components` tree).
- **`blueprint rename_function` keeps function flags and metadata (#150, reported by @altarfinch).** The entry node's extra flags (pure, const, access, ...) and metadata (Call In Editor, category, tooltip, thread safe) are saved before the rename and restored after, and the response echoes them. A rename the engine silently refused because of a name collision now returns an error instead of success under the old name.
- **Incremental project source reindex no longer leaves stale or duplicated rows.** `source trigger_project_reindex`, and the automatic reindex after Live Coding / hot reload, appended to the project's existing rows instead of replacing them: files deleted from disk kept every symbol, reference, include and source chunk, and files still on disk had their symbols, includes, source-search chunks and deprecations duplicated, with symbols removed from a file lingering beside the new ones. The pass now purges everything previously indexed under the project's `Source/` and `Plugins/` trees in a single transaction before re-indexing, which also clears modules and plugins deleted outright. Engine rows are untouched, and a full `trigger_reindex` behaves as before.
- **Source indexer file and module ids resolved to unrelated rows.** Re-inserting an existing file or module used `INSERT OR IGNORE` followed by the connection's last insert rowid, which SQLite does not reset when the insert is ignored, so an existing file could resolve to the id of whatever was inserted last (usually a symbol). The row is now looked up first, and an existing row has its metadata (line count, timestamp, module, `Build.cs` path) refreshed.
- **Asset index kept the old asset name after a rename detected at editor startup.** The startup incremental asset index recorded the new package path for a moved asset but left `asset_name` at its old value; it now passes the name the asset registry reports for the new path.
- **`editor save_packages` no longer crashes the editor on a read-only file (#147, reported by @altarfinch).** `FSavePackageArgs::Error` defaults to `GError`; when the engine refuses to replace a read-only target (`FinalizeTempOutputFiles`: "Cannot remove ... as it is read only!") it reports "Error saving" through that device, and the Windows `GError` device is fatal at any verbosity. A read-only target is now checked out through source control when it is enabled; otherwise the package is skipped with `"read-only on disk, not checked out"` and nothing is written. Rows report `read_only` / `checked_out`, and saves in `save_packages`, `create_empty_map`, `import_texture` and `stitch_flipbook` route save errors to `GWarn`, so any other failed save returns `saved: false` instead of crashing.
- **Failed asset saves in other namespaces no longer crash the editor (#147, reported by @altarfinch).** The same `GError` default applied to about 60 other direct `UPackage::SavePackage` / `UPackage::Save` calls in the AI, audio, ComboGraph, GAS, Logic Driver, material, mesh, Niagara and UI actions; Niagara also set `Error = GError` explicitly at six sites. All of them now route save errors to `GWarn`, so a read-only or otherwise unwritable target is logged as a warning and reported through each action's existing save result instead of terminating the editor.
- **`editor capture_pie_movement_clip` no longer reads an undrawn PIE viewport, and a stalled clip no longer reports success (#149, reported by @renatokuurstra).** The frame read checks the viewport's render-thread texture before `ReadSurfaceData`, so a viewport that has not drawn yet (or a minimized window) is skipped and counted under `capture_validity.not_drawn_frames` instead of hitting `ensure(InRHITexture)` in D3D12. `capture_validity` now reports `expected_frames`, `captured_frames`, `frame_shortfall` and `max_capture_gap_seconds`, and `captured_ok` is false when fewer than half the expected frames were saved.
- **`blueprint set_property_at_path` / `set_cdo_property` reported 0 mappings on an Input Mapping Context (#148, reported by @grzegorzkarwalski-del).** The writers were operating on the right asset; `UInputMappingContext::Mappings` is deprecated since UE 5.7 and `PostLoad` moves its contents into `DefaultKeyMappings.Mappings`, so on any loaded context it is empty and writes to it are never read. Paths through a deprecated property (`CPF_Deprecated` or `meta=(DeprecatedProperty)`), `set_cdo_property` on one, and `set_cdo_properties` top-level keys naming one now fail with the property's deprecation message and, for this case, point at `DefaultKeyMappings.Mappings[N]` and the new `input` actions.
- **A missing `asset_path` is now rejected before dispatch (#157, reported by @445052020).** The dispatcher skipped `asset_path` in its required-param check for every action, citing a `system_path` alias that only three Niagara helpers implement, so elsewhere a missing or misnamed path reached the handler as an empty string and came back as e.g. `"Blueprint not found: "`. `asset_path` is now validated like any other required param. The two aliases handlers actually read are still accepted, each only in its namespace: `system_path` in `niagara` and `package_path` in `project`; the alias value is moved onto `asset_path`, and the missing-param error names it (`asset_path (or alias: system_path)`).
- **Level indexing loads and releases one map at a time, and replaces actor rows atomically (#123).** Each game-thread dispatch loads at most one root map, finishes compilation, tears the world down, and runs GC plus a render/RHI flush before the next map is admitted. Actor rows are assembled first and replaced in one SQLite transaction per world, so a failed write keeps the world's previous rows and a rerun no longer duplicates them. Before and after each load the pass checks process memory, free system RAM and GPU budget; the first pressure result triggers one cleanup, and sustained critical pressure stops the level pass. `project get_stats` reports the pass state as `level_index_state`. Thanks **@Hvizeu**. Review changes made while landing it:
  - A level pass stopped by memory pressure is recorded as `level_index_state: degraded` along with the skipped maps. The remaining post-pass indexers (DataTable, config, C++ symbols, animation, tags, Niagara, mesh) still run and the index is still marked complete, so a machine under persistent pressure no longer re-runs the whole index on every launch. A later full reindex retries the skipped maps.
  - When Unreal defers the release GC because the GC lock is held, the release is retried (bounded) instead of ending the level pass.
  - The deep pass and the per-indexer batch throttles keep their previous memory checks. The free-RAM and VRAM checks apply to the level pass only, so low-RAM machines no longer get a full-purge GC on every deep-pass batch.
  - Only packages belonging to the map being indexed (the map, its streaming sub-levels, and their external actor/object packages) are released after a load. Packages loaded by anything else during the map load are left alone.

### Changed

- **`editor delete_assets` explains failures.** `failed_to_delete` now lists only the assets that actually survived (re-resolved after the delete instead of derived from the count), and a new `failures[]` gives each one a `reason` plus its on-disk package referencers from the asset registry (outside the delete set, capped at 20) and its in-memory referencers from the same reference search ObjectTools runs (capped at 20), with totals and `referenced_by_undo`. A holder that leaves no reflected reference, such as native code keeping a raw pointer, cannot be named.
- **Engine source database schema v3.** `symbol_deprecations` gains a nullable `file_id` column so the incremental purge can reach deprecation rows that are not linked to a symbol. Existing databases are migrated in place on the next index pass. Deprecation rows written before v3 that have neither a file nor a symbol link cannot be attributed, and are only cleared by one full `source trigger_reindex`. `meta.schema_version` now records the real schema version instead of `1`.

## [0.23.0] - 2026-09-10

> **Action count is approximate.** **~1,400+ actions across 26 in-tree namespaces** — query `monolith_discover()` for the live figure.

Two new namespaces, `input` and `localization`, and a release built largely out of the gap between what an action *reported* and what it *did*: a Sound Cue edit that asserted instead of failing, an index that hung the editor on exit, a UI rebuild that reset properties it never mentioned, and instanced input modifiers that read back as `None` after a reload with nothing anywhere flagging the loss. Thanks to **@kunkunGames**, **@Alexbeav**, **@whalemenace**, **@Hvizeu**, **@M0rtaI**, **@aggitti**, and **@Nicopara** for the reports and prototypes.

### Added

- **New `input` namespace (13 actions) for Enhanced Input.** `UInputAction` and `UInputMappingContext` inspection and authoring. It lives in `MonolithGAS` for proximity to ability input binding, but it is **not GAS** — its assets are engine assets, and it registers *before* the `bEnableGAS` gate, so Input Actions and Mapping Contexts stay reachable when GAS integration is switched off entirely. Five read-only inspection actions (`list_input_actions`, `get_input_action`, `list_input_mapping_contexts`, `get_input_mapping_context`, `validate_input_mappings`) and eight authoring actions for instanced modifiers and triggers. Mappings are addressed by index or by an `(input_action, key)` pair; an ambiguous pair is refused rather than guessed. Reimplemented from PR #130 by **@kunkunGames**.
- **New read-only `localization` namespace (4 actions).** `list_cultures`, `list_string_tables`, `get_string_table`, `validate_string_table`. Registered by `MonolithConfig` **ahead of** the `bEnableConfig` gate, so culture and StringTable inspection stay available when config authoring is off. Every response is bounded and every cutoff is reported explicitly — `get_string_table` separates "more on this page" from "covered the whole table" from "metadata budget exhausted", so you can tell which limit stopped you. `validate_string_table` reports `valid=true` only when the scan covered every entry and found zero errors; a truncated scan is itself an error. StringTable *authoring* remains on the `blueprint` namespace.
- **6 bounded read-only `chooser` actions** — `list_chooser_tables`, `get_chooser_table`, `list_chooser_columns`, `list_chooser_rows`, `list_chooser_references`, `validate_chooser_table`. Every response is finite, and each cutoff is reported through explicit truncation or `complete` metadata: a short answer is never indistinguishable from a whole one. Column readback reads the active per-row array rather than the deprecated one, so bool columns no longer report zero cells. All six stay registered and discoverable when the optional Chooser plugin is disabled, returning an explicit availability error instead of synthesized data. **`validate_chooser_table` is distinct from the existing `validate_chooser`** — the new one never compiles, mutates, saves or dirties a package; the old one still performs its compile-oriented pass. From PR #129 by **@kunkunGames**.
- **User Defined Structs are editable after creation** — `get_struct_fields`, `add_struct_field`, `remove_struct_field`, `rename_struct_field`, `set_struct_field_type`. `create_user_defined_struct` authored a struct once; there was no way to change one afterwards and no way to read one at all. Fields are targeted by display name (`Mobility`) rather than the serialized `VarName`, and `get_struct_fields` reports types in the grammar the writers parse, so its output feeds straight back in. Each writer reports what it recompiled, what it dirtied, and which assets reference the struct — those are **not** recompiled here, so audit them before a removal or a retype. `set_struct_field_type` additionally reports the default value the engine discards, because a retype is a migration, not a rename.
- **`ui build_ui_from_spec` gained a `mode`: `"rebuild"` (default, unchanged) or `"patch"`.** Patch matches spec nodes to existing widgets by id and class, reuses those objects, and writes only what the spec names — so custom style tints, blur strengths, tooltips and unmodelled slot fields survive an iteration pass. An unchanged `FText` keeps its localization key. `build_menu_from_spec` takes the same mode.
- **`editor start_pie` gained an `on_compile_errors` policy.** Starting PIE with a Blueprint in an unresolved error state raises the engine's blocking compile-error modal, which runs a nested modal loop and starves the in-process MCP server. `"refuse"` (default) returns the offending Blueprint list without starting PIE; `"suppress"` starts anyway with the dialog silenced. Thanks **@Hvizeu**.
- **`editor capture_pie_movement_clip` gained `include_ui`.** The default capture path only ever sees the scene render target, which is why UI iteration used to come back as an empty scene. With `include_ui: true` frames ride the engine screenshot path, so Slate composites UMG into the image. Note that those frames are written asynchronously, so the per-frame validity check does not apply to them, and that a docked PIE session captures the whole editor window — use new-window PIE for a clean clip. Thanks **@whalemenace**.
- **`editor create_empty_map` gained `open` and `dirty_policy`.** `open=true` loads the new map with the same guards as `editor load_level`. Thanks **@whalemenace**.
- **`editor capture_scene_preview` honours scalar `width` / `height`.** They were previously rejected as unknown params while the capture silently stayed 512x512. Use them to match a widget's design frame; a square capture yields false layout conclusions. Reported by **@Alexbeav** (#141).
- **`project search` gained an `asset_class` filter.** Takes `"Blueprint"` or `["Blueprint","WidgetBlueprint"]`. **The filter is applied inside the query, not over the results**, so `limit` counts matching rows rather than returning the top N of everything and then culling — searching a common token in a project with a large third-party content folder no longer buries the target. A graph-node text hit inside a Blueprint still survives a `Blueprint` filter, since node hits are filtered by their owning asset's class.

### Fixed

- **Instanced Enhanced Input modifiers and triggers could not be authored from script (#140, reported by @Alexbeav).** Modifiers nested in a mapping context's mapping array were silently dropped on save: the mapping slots kept their count, but every instanced subobject read back as `None` after a reload, with nothing in the editor or the log flagging the loss. The new actions construct each modifier with the outer and flags the details panel itself uses, so the subobject is harvested into the mapping context's package instead of being written as an unresolvable import.
- **`blueprint add_node` no longer piles every unplaced node on the origin (#132, reported by @Nicopara, diagnosed by @Alexbeav).** Omitting `position` defaulted to `[0,0]`, so a graph authored through MCP came out as one unreadable stack. Unpositioned nodes are now placed in clear space; an explicit position is still authoritative.
- **`blueprint auto_layout(layout_mode: "new_only")` now moves piles, not just nodes at the origin.** Its rule was "unpinned only if at (0,0)", which made it depend on the bug above. A node now counts as new if it is at the origin *or* piled on another node, measured by overlap area so edge contact in a hand-laid graph is left alone. **Consequence:** nodes that `add_node` placed in clear space are no longer `new_only` candidates — use `layout_mode: "all"` to fold them into the flow.
- **`ui build_ui_from_spec` rebuild mode now audits what it is about to destroy (#139).** Every editable property that differs from its class default and is not restored by the spec is reported as a warning naming the widget and dotted path, plus one per localization key the rebuild would reassign — and the audit runs on `dry_run: true`, so you can see the loss before it happens.
- **`editor create_empty_map` no longer implies it changed the open world.** It never did — it saves a `UWorld` asset and leaves the editor on whatever level was loaded, so chaining create -> populate silently edited the wrong world. The response now always reports `current_world` and `opened`.
- **`niagara` module inputs set in the stack editor no longer read back as `(default)` (#143).** Those values live in the script's rapid-iteration parameter store rather than on a graph override pin. Reads now fall back to that store and report `source: "rapid_iteration"`.
- **`niagara set_module_input_value` now writes LinearColor correctly (#122, #143).** The JSON object form emitted a literal the engine's colour pin parser rejects, silently replacing it with black-with-alpha-1 — the "only the alpha applied" symptom.
- **Sound Cue edits no longer crash the editor when the cue's asset editor is closed (#125, reported by @M0rtaI).** A cue whose editor was never opened has no editor graph, and the engine's graph-link call asserts on it rather than failing softly. Edits now rebuild a missing graph before touching it, and skip the visual link entirely when it still cannot be linked safely — the runtime node chain is updated either way. All twelve Sound Cue mutation actions report `graph_link_skipped: true` when that happens.
- **Quitting the editor while a project index was running hung the process forever.** The indexing worker waited on work only the game thread can run, while shutdown blocked the game thread joining that worker. A force-stopped index now exits in well under a second. A user-initiated cancel is unaffected and still runs to a clean finish. This also unmasks and fixes an `"AsyncTaskNotification was still pending when destroyed"` assert that the deadlock had been hiding.
- **Cleared the UE 5.8 deprecation warnings (#121, reported by @aggitti) while keeping UE 5.7 compilation intact** — object iteration, post-engine-init delegates, mirror tables, retarget op settings, PoseSearch database entries, and the `Rename` flag set.

### Changed

- **`editor capture_scene_preview` validates capture size.** Non-positive dimensions and sides beyond 8192 px are rejected as invalid params.

## [0.22.0] - 2026-08-01

> **Action count is approximate.** **~1,400+ actions across 24 in-tree namespaces** — query `monolith_discover()` for the live figure.

A reliability release: the project index now survives a crash instead of starting over, several read actions that were quietly answering with the wrong object are fixed, and `monolith_discover` can finally search across namespaces. Thanks to **@kunkunGames**, **@DanaFo**, **@Hvizeu**, **@aggitti**, **@nanbarada**, and **@Thomasbehan** for the reports.

### Added

- **Animation layers can be authored without an interface asset.** New `animation add_anim_layer_graph` creates an ABP-native animation layer — a `UAnimationGraph` on the animation graph schema in the Animation Blueprint's own function graphs, which is what the editor's My Blueprint → **+** → Animation Layer button produces. The layer belongs to the ABP, so variant ABPs no longer have to share a `UAnimLayerInterface` signature just to have a layer. The schema is the load-bearing part: it is what makes the animation compiler emit a real `FAnimBlueprintFunction`, which `blueprint add_function` cannot do — it produces an inert K2 graph. The Output Pose root node is created automatically. Optional `input_poses` declares input pose pins by name (capped at 16). Refuses a duplicate graph name outright instead of renaming the incumbent, plus the reserved name `AnimGraph`, child Animation Blueprints, macro libraries, and interface Blueprints.
- **`monolith_discover` can search every namespace at once.** A `filter` passed without a `namespace` used to be silently ignored — you got the plain namespace inventory back and had to walk all of them by hand. It now searches the whole registry, tags each row with its owning namespace, and reports `matched_namespaces` before pagination so "which namespace owns this" stays answerable even when rows are capped. An absent `limit` caps at 50 here (the whole registry is a much bigger haystack than one namespace); `limit: 0` still means all. Thanks **@kunkunGames** (#112).
- **`risk get_mining_status` says why a risk query came back empty.** Reports which repositories were mined, which candidates were rejected and why, whether mining ran this session, and the row counts of the four risk tables. The git-backed `risk` actions also attach a `diagnostics` block when they return nothing, with a hint naming the setting to change.
- **`project get_stats` reports assets skipped by deep indexing** — `skipped_assets` and `skipped_asset_paths` (up to 50), populated when the index dropped an asset after repeated interrupted attempts.
- **`Monolith.StartIndex force`** — the console command takes an optional `force` argument. Bare `Monolith.StartIndex` resumes an interrupted index; `force` wipes and starts over.

### Fixed

- **An interrupted full index no longer throws away the work it already did.** A full index wiped the database up front and wrote its completion marker only at the very end, so any crash or kill left no marker and the next launch erased every batch the previous run had finished. On a large project one crash cost hours. The index now checkpoints per asset, resumes on the next launch, and drops (never silently) an asset that twice crashed the editor mid-index. Recover with `Monolith.StartIndex force`. Thanks **@Hvizeu** (#117, #120).
- **A project with fewer than 500 assets no longer re-indexes from scratch on every launch.**
- **Blueprint component reads returned the C++ parent's defaults, presented as your Blueprint's values.** `get_component_details` and `get_components` read the native parent's class-default object while `set_component_property` wrote the Blueprint's own — so on any Character Blueprint a capsule half-height overridden to 96 read back 88, and the same applied to transforms, collision, movement settings and mesh/anim-class wiring. Both reads now use the Blueprint's own CDO through one shared resolver. `get_component_details` also reports `source`, `resolved_component` and a `note`; `get_components` gains `inherited_components[]`. Thanks **@DanaFo** (#116).
- **The `Mesh` alias resolved to the capsule on Character Blueprints.** Aliases now carry their own target class, an exact component name always beats an alias, and an ambiguous alias reports the candidates instead of picking the first. Thanks **@DanaFo** (#116).
- **`set_component_property` can now write a component inherited from a parent Blueprint**, by creating an Inheritable Component Handler override on the child instead of failing with "component not found". Thanks **@Thomasbehan** (#102).
- **Enum widget variables compile to enum properties instead of `int`.** `ui add_widget_variable` built `enum:<Name>` pins with a type-*picker* category that is never valid on a real pin, so the variable fell through to an `int` property and every Get/Set node refused a real enum connection. The tell was a "not compatible" message naming a *spaced* enum (`ESlate Visibility Enum`). Already-broken assets are repairable in place with `blueprint set_variable_type`. Thanks **@aggitti** (#115).
- **`enum:` accepts a full object path.** `enum:/Script/UMG.ESlateVisibility` never resolved on any action, on any version — lookup flattened the whole argument into a single name, so a path could only ever miss.
- **The `risk` namespace mined no repositories on almost every project.** The git roots were a hardcoded list of nested-plugin paths that never probed the project root, so for anyone whose project root *is* the git repository — the ordinary layout — `risk_query` returned empty permanently with nothing to say why. Roots are now discovered at runtime, with two new settings under **Editor Preferences → Plugins → Monolith Reflection Intel → Risk**. Thanks **@nanbarada** (#119).
- **Hotspot scores were `0` for every file**, because churn paths and complexity paths were keyed in different path spaces and never met. Existing risk tables rebuild once on the first query after upgrading.
- **Editing a risk setting did nothing until the editor was restarted.**
- **`project search` no longer reports query errors as zero results.** A malformed FTS5 query came back as a successful empty result set, indistinguishable from a genuine no-match. Caller errors are now `-32602` and storage failures `-32603`, with `LIMIT` bound as a parameter. A column filter naming an unknown column now names it and lists the valid ones. Thanks **@kunkunGames** (#113).
- **Corrected the documented `NEAR` syntax and scope for `project search`** — the working form is `NEAR(BP_Enemy Health, 3)`, and the index covers asset and graph-node fields, not variables and parameters.
- **`monolith_discover` pagination no longer overflows into an empty page with a negative `next_offset`** on a very large `limit`. Thanks **@kunkunGames** (#112).
- **Stopping or restarting the Monolith server no longer takes every other plugin's HTTP routes down with it.** Teardown called a process-wide stop, so Web Remote Control, PerfCounters and anything else holding a route lost service whenever Monolith restarted. Monolith now unbinds only its own six routes. **One consequence:** the listener survives `Stop()`, so a TCP probe of port 9316 is no longer a valid liveness check — use `monolith_status`. Thanks **@kunkunGames** (#114).
- **Closing the editor during a project index could crash on shutdown**, via game-thread callbacks that outlived the task object they captured.
- **A failed project index left the asset index silently stale for the rest of the session** — the live Asset Registry callbacks were re-armed only on success, so after a failure every later asset change was dropped until a reindex or restart.
- **A reindex that never started no longer reports success**, and **an indexer that failed to start no longer refuses every request afterwards** until the editor is restarted.
- **Editor startup no longer stalls on the port probe** — the bind check used a blocking socket, costing roughly two seconds on the game thread per retry on Windows.
- **The animation indexing pass commits per batch** instead of once at the end, and **a failed database reset now aborts the index** instead of being ignored.
- **`animation set_anim_graph_node_property` no longer writes to a different node when its scope fails to resolve.** `graph_name` / `state_name` were advisory: an unresolved scope fell through to an all-graphs search by `node_id`, and node ids are not unique across graphs — so the write landed in an unrelated animation layer, and the asset still compiled and still validated, it just played the wrong animation. A supplied scope is now authoritative. The resolver also reaches nested state machines at any depth.

### Changed

- **`animation add_linked_anim_layer` now finds ABP-native layers too.** When no implemented `UAnimLayerInterface` declares the requested `layer_name`, the action looks for a layer graph on the Animation Blueprint itself and binds it as a self layer — mirroring the engine, which stores no "is self" flag and treats a failed interface lookup as the self signal. Interface layers still win when the name exists in both places, passing `interface_class` disables the native fallback, and `instance_class` is rejected for a self layer. A self bind reports `interface_class: "<self>"` and `guid_resolved: false`.
- Ordering note: a self layer's pose pins resolve against the Animation Blueprint's generated skeleton class, so creating a layer with `compile: false` and immediately placing its node yields a node with no pose pins. Keep the default `compile: true`, or recompile before placing.
- **`animation set_anim_graph_node_property` reports where the write landed**, via `resolved_graph_path` — the root-to-leaf graph path of the node actually written.
- **Pin-type strings are parsed strictly by the actions that create pins.** A typo'd `type` in `blueprint set_function_params` used to produce a `bool` pin, and an unresolvable `enum:Whatever` produced a variable with no type object at all. Both now return an error naming the token and the reason, and the signature-rewriting actions validate every type *before* touching a single pin, so a rejected call leaves the existing signature untouched.
- **`blueprint get_inherited_component_override` reports a wider set of `source` values** — `scs`, `cdo_native`, `ich_override`, `inherited_scs`, `parent_cdo_fallback`. Callers matching on the literal `"ich"` need updating.
- **`project search` validates its parameters before touching the index** — `query` is trimmed, must be non-empty and is capped at 4096 characters; `limit` is clamped to 1-1000.
- **The Project Settings re-index button is greyed out when the index database is closed**, instead of being enabled and doing nothing.
- **`Monolith.StartIndex` resumes rather than wipes.** Use `Monolith.StartIndex force` for the old behaviour; `monolith_reindex force=true` is unchanged.

### Internal

- Three release gates that reported green while the path they defend was open are now real: the offline CLI build gate (a `cmd.exe` variable-expansion bug made its failure branch unreachable, so a failed compile published a stale `monolith_query.exe` and exited 0), the offline-exe freshness guard (documented but invoked by no script), and the pre-v2 SHA marker detector (anchored more narrowly than the deployed parser it defends). See [[Auto-Updater]] for why the marker names matter.

## [0.21.3] - 2026-07-26

> **Action count is approximate.** **~1,500+ actions across 25+ in-tree namespaces** — query `monolith_discover()` for the live figure.

This release closes out the open pull-request queue. Thanks to **@Thomasbehan**, **@whalemenace**, and **@kunkunGames** for the reports.

### Added

- **`niagara get_module_graph` can emit link topology.** Pass `links: true` for a top-level `edges` array plus a `pin_id` on every pin, so branch tracing through static switches no longer requires a T3D export. Output is unchanged when the flag is absent; edges are keyed by the `(node_guid, pin_id)` pair, since pin IDs alone are not unique within a graph. Thanks **@whalemenace** (#109).

### Fixed

- **`ui set_widget_property` can now set Margin and Vector4 properties.** `value` was declared as a string, so every documented shape arrived as text and was rejected — while the error message listed those exact shapes as valid. Thanks **@Thomasbehan** (#110).
- **Malformed destination paths are rejected before anything is created**, across seventeen actions in `blueprint`, `material`, and `niagara`. In the compound PBR workflow, a bad option or an existing-material collision now fails before the first texture import. Thanks **@kunkunGames** (#106, #107, #108).
- **Material creation no longer risks overwriting a `.uasset` that is on disk but not yet in the Asset Registry.**
- **A spurious `LogEditorAssetSubsystem: Error` on every successful asset creation is gone.**

### Changed

- **Paths containing characters Unreal forbids in a package name are now errors** — `\ : * ? " < > | ' <space> , . & ! ~ @ #`. Previously they passed a weaker check and produced a mangled asset. If a path you rely on is rejected, please open an issue rather than working around it.
- **Object-path destinations such as `/Game/Foo/M_Bar.M_Bar` are accepted and normalised**, since the forbidden set includes `.` and that form is what you get copying a path out of a search result.
- **`blueprint seed_data_asset` with `dry_run: true` reports a malformed path as an error** rather than returning a dry-run report.

## [0.21.2] - 2026-07-22

> **Action count is approximate.** **~1,500+ actions across 25+ in-tree namespaces** — query `monolith_discover()` for the live figure.

### Fixed

- **`blueprint` wildcard array pins now resolve** — tool-created `Array_*` nodes spawn the palette-correct `UK2Node_CallFunction` subclass, so their wildcard pins take a type from your connections; the schema-mediated disconnect path resets wildcard pins. Thanks **@Alexbeav** (#95).
- **`blueprint` reference-audit blind spots closed** — `list_graphs` / `search_nodes` / `find_variable_references` / `get_graph_data` now see collapsed-composite subgraphs, input-pin default values, string-bound timer callers, and inherited-scope variables; nested graphs are classified via a shared helper. Thanks **@Alexbeav** (#96).
- **`blueprint` local-variable data pins materialize** — `VariableGet` / `VariableSet` nodes for locals now bind via `SetLocalMember` when the graph declares a matching local. Thanks **@Alexbeav** (#97).
- **`blueprint` enum member variables are enum-typed** — `VariableGet` / `VariableSet` nodes now use the schema's byte + subobject convention, so their pins connect to enum consumers. Thanks **@Alexbeav** (#98).
- **UE 5.8 / Linux source-build compatibility** — build fixes for the 5.8 `FJsonObject` shared-string keys and new `-Werror` sites, plus Reflection Intelligence support for the 5.8 UHT codegen format (replicated properties, RPCs, `OnRep`, interfaces, parent chains). Thanks **@gregorygmwhite** (#99).
- **MetaSound build error on UE 5.8** — fixed the 5.8 MetaSound compile break, plus broader cross-version hardening of `FJsonObject` key iteration (GAS / LogicDriver / ComboGraph bulk-fill and MetaSound) via the `MonolithKeyToString` helper. Reported by **@Matt-Makes** (#100).

### Changed

- **`blueprint` `find_variable_references` defaults `include_inherited` to `true`** (was `false`) — a deletion audit over-reports rather than returning a false zero for parent-class variables. Pass `include_inherited: false` for the strict own-class view. Thanks **@Alexbeav** (#96).

## [0.21.1] - 2026-07-20

> **Action count is approximate.** **~1,500+ actions across 25+ in-tree namespaces** — query `monolith_discover()` for the live figure.

### Fixed

- **CRITICAL — auto-updater no longer crashes the editor on Install (Windows)** (#90, #94). The integrity check added in v0.14.7 called `FPlatformMisc::GetSHA256Signature`, which has no Windows implementation — the engine's generic fallback is a fatal assert, so clicking **Install** killed the editor whenever the target release carried a checksum marker. The updater now uses a portable, NIST-vector-tested SHA-256. Release-note checksum markers moved to new `Monolith-SHA256-v2-*` names so pre-fix updaters fail safe (a "missing integrity marker" notification on 0.20.3+, an unverified-but-working install on older versions) instead of crashing — see [Auto-Updater](Auto-Updater). Thanks **@Alexbeav** (#90, #91) and **@SilkroadLabs** (#94).
- **`blueprint` `add_event_dispatcher`** creates the multicast delegate member variable, so `CallDelegate`/`AddDelegate`/`RemoveDelegate` can bind the dispatcher; `remove_event_dispatcher` cleans it up. Thanks **@Alexbeav** (#84).
- **`blueprint` `add_node`/`resolve_node`** no longer crash the editor on `node_type: "SpawnActor"` (legacy-node title null-deref); generic-fallback nodes report a safe title. Thanks **@Alexbeav** (#85).
- **`blueprint` `add_timeline_track`** — the new track's output pin now actually appears on the timeline node (display-track registration + node reconstruction). Thanks **@Alexbeav** (#86).
- **`blueprint` `batch_execute`** honors a top-level `graph_name` default for all ops. Thanks **@Alexbeav** (#87).
- **`blueprint` `resolve_node`** delegate-node dry-runs resolve real signature pins. Thanks **@Alexbeav** (#92).
- **String-encoded array/object params recovered centrally** — MCP clients that serialize complex arguments to JSON strings no longer hit "array is required" errors on `delete_assets`, `save_packages`, `set_function_params`, and friends. Thanks **@Alexbeav** (#93).

### Added

- **`editor` `load_level` fail-closed guards** — refuses on a dirty current map (override with `dirty_policy:"discard"`) and on a stale rooted in-memory copy of the target world, instead of silent data loss or a fatal engine assert. Thanks **@Alexbeav** (#89).
- **Paired `MODAL_OPEN`/`MODAL_CLOSE` log telemetry** with window identity + slow-task flag on UE 5.8+. Thanks **@Alexbeav** (#88).

## [0.21.0] - 2026-07-19

> **Action count is approximate.** **~1,500+ actions across 25+ in-tree namespaces** — query `monolith_discover()` for the live figure.

### Added

Ergonomics upgrades to the `ui` and `blueprint` action packs from the Ideas board (Discussion #74, thanks @k-s-s). No new actions — existing ones just got easier to call.

- **`ui` `add_widget`** accepts `parent` as an alias for `parent_name`, so you can drop a widget into a non-root panel directly.
- **`ui` `set_widget_property`** now allowlists common `UWidget` properties by default (`Visibility`, `RenderOpacity`, `ToolTipText`, `bIsEnabled`, `RenderTransform.Angle` / `.Scale` / `.Translation`) — no more `raw_mode=true` for the everyday ones.
- **`ui` `set_brush`** makes `property_name` optional (auto-resolves Image → `Brush`, Border → `Background`) and takes `color` as an alias for `tint_color`.
- **`ui` `set_slot_property`** supports grid slots (`row`, `column`, `row_span`, `column_span` on `UUniformGridSlot` and `UGridSlot`).
- **`blueprint` `describe_cdo_schema`** emits the correct positional `TMap` ImportText hint, including the struct literal for struct-valued maps.
- **`blueprint` `add_variable` / `set_variable_type`** accept prefixed key **and** value types in map strings (e.g. `map:enum:ESlateVisibility:struct:LinearColor`), so enum-keyed and struct-valued maps are authorable.
- **`blueprint` `connect_pins`** disambiguates node IDs that exist in multiple graphs and tells you to pass `graph_name`.
- **`blueprint` `add_node` / `resolve_node`** resolve user-defined enums on `K2Node_SwitchEnum` (short name, `/Script` path, or unloaded asset; plus `enum` / `enum_path` aliases) and Blueprint-defined functions on `K2Node_CallFunction` (self and external Blueprints).

> Issue #82 (deferred MCP context loading) was already handled in v0.20.3 by terse-by-default `monolith_discover` + `describe_query` — nothing new needed. Thanks @aggitti.

### Fixed

- **BlueprintAssist bridge vs. BA 4.9.0+** — `MonolithBABridge` no longer C2039s against BlueprintAssist 4.9.0+ (`RequestFormatAll` / `GetNumberOfPendingNodesToCache` changed); `__has_include` keeps pre-4.9 building too. PR #78 — thanks @tc-imba (parallel fix @mewliks, #76).
- **GeometryScripting delay-load DLLs gated to Win64** — fixes `BuildEnvironment.Unique` and the macOS source link. PR #77 — thanks @itismyfield.
- **UE 5.8 source builds** — `FJsonObject` keys are now routed through a `MonolithKeyToString` shim (they became `FSharedString` in 5.8), so the same `MonolithAnimation` source compiles on 5.7 and 5.8. Issue #80 — thanks @Baba-Ramsi (also @dulanw, #79).
- **Linux / clang builds** — removed nested `/*` in doc comments that tripped `-Werror,-Wcomment` at four `MonolithReflectionIntel` sites. Issue #83 — thanks @daschatten-tb.
- **Indexer data-loss guard** — the source indexer no longer strips `RF_Standalone` from assets that were already loaded and referenced; a load-time residency gate across seven `TryUnloadPackage` sites (plus the landscape cleanup branch) means it only unloads packages it brought in. Issue #81 — thanks @Alexbeav.
- **Optional-plugin detection** no longer false-positives on plugins that share a name prefix (e.g. `BlueprintRetarget` being read as BlueprintAssist). Disk-presence globs now match full plugin names. Reported by @k-s-s (#66).

## [0.20.3] - 2026-06-20

> **Action count is approximate.** **~1,500+ actions across 25+ in-tree namespaces** — query `monolith_discover()` for the live figure.

### Added

- **Dual-engine releases — per-engine prebuilt zips for UE 5.7 and UE 5.8.** Each release now ships one flat plugin zip per Unreal Engine version (`Monolith-vX.Y.Z-UE5.7.zip`, `Monolith-vX.Y.Z-UE5.8.zip`), plus a legacy `Monolith-vX.Y.Z.zip` (the 5.7 build) for older auto-updaters. Download the zip matching your engine; a prebuilt binary is locked to one engine's ABI, so 5.7 and 5.8 builds are not interchangeable (source builds work on both). The auto-updater is now engine-aware — it detects the running engine, downloads the matching zip, verifies it against a per-engine `Monolith-SHA256-UE5.x:` checksum, and refuses to install if no build exists for your engine.

- **`monolith_discover` is terse by default — major token reduction.** A per-namespace `discover(namespace)` now returns each action's name plus a **one-line description** (first sentence, else hard-capped at 150 chars on a word boundary with a `...` suffix), instead of the full per-action param JSON-Schema. This cuts roughly 70-84% of the tokens on the heavy namespaces (e.g. `blueprint`) and about 77% overall across all 26 namespaces, verified universal. Full param schemas are still available two ways: the existing `describe_query("action_schema")` returns a single action's full schema (~54 tokens) on demand, and a new `detail=true` (alias `verbose=true`) param on `discover` reproduces the pre-change shape inline. New `discover` params: `filter` (case-insensitive substring on action name OR full description), `offset` (default 0), and `limit` (default 0 = ALL; pagination is opt-in — the default still returns the COMPLETE list). The response carries `total` always, `next_offset` only when a positive `limit` leaves more remaining, and a `schema_hint` in terse mode. Backward-compatible: `discover(ns, detail=true)` is byte-for-byte the old output, and full `discover()` with no namespace is unchanged.

### Fixed

## [0.20.2] - 2026-06-15

> **Action count is approximate.** **~1,500+ actions across 25+ in-tree namespaces** — query `monolith_discover()` for the live figure.

### Fixed

- **From-source rebuilds still hard-linked optional plugins MonolithAI couldn't load without.** v0.20.1 rebuilt the shipped release binaries clean, but it didn't fix the source path — so anyone who rebuilt Monolith from source on a stock engine kept hitting the same `GetLastError 126` load failure on `MonolithAI.dll`. The root cause: the optional-plugin gates in `MonolithAI`, `MonolithMesh`, `MonolithIndex`, `MonolithAudio`, and `MonolithAnimation` only checked whether a plugin was **present on disk**. Engine plugins like MassSpawner / ZoneGraph are always on disk, so the gates linked them in even when they weren't enabled in the project, and a stock engine doesn't load them at runtime. Those five `Build.cs` files now read the `.uproject` ProjectDescriptor and gate on whether the plugin is actually **enabled in the project**, not merely present. A from-source build with those plugins disabled now produces a `MonolithAI.dll` with zero MassSpawner / ZoneGraph imports; builds that do enable them keep the features. This is the source-side complement to v0.20.1's binary fix. (#71, reported by @aggitti)

### Internal

- Dropped `GameplayAbilities` from the release import-leak sentinel list — it's a hard dep in `Monolith.uplugin` (auto-enable contract guarantees load order), so it's functionally safe to hard-link and was triggering a false-positive ship block.

## [0.20.1] - 2026-06-15

> **Action count is approximate.** **~1,500+ actions across 25+ in-tree namespaces** — query `monolith_discover()` for the live figure.

### Fixed

- **MonolithAI failed to load on stock UE 5.7.4 (Epic Launcher) with `GetLastError 126`.** The v0.20.0 release binary hard-linked the optional `MassSpawner` (MassEntity / MassGameplay) and `ZoneGraph` plugin DLLs, which aren't enabled in a stock engine build, so on a clean install the plugin could not load. The release binary is now correctly gated and MonolithAI loads without those experimental plugins. (#71, reported by @aggitti)

## [0.20.0] - 2026-06-14

> **Action count is approximate.** **~1,500+ actions across 25+ in-tree namespaces** — query `monolith_discover()` for the live figure.

### Added

- **Blend space baking + interpolation control (`animation`).** Two new actions for getting blend spaces runtime-correct.
  - `bake_blend_space` — rebuild a blend space's triangulation (`FBlendSpaceData`) via `ResampleData()` and mark it dirty, for blend spaces authored externally or before this release's auto-bake fix. Params: `asset_path`. Returns `has_blendspace_data`, `sample_count`, `baked`, and a `warning` when a 2D blend space has fewer than 3 samples (triangulation needs at least 3). Works on 1D and 2D blend spaces.
  - `set_blend_space_interpolation` — set a blend space's input-interpolation settings, then resample + dirty. `use_grid` toggles `bInterpolateUsingGrid` (true = runtime uses the grid, false = the triangulation); `preferred_triangulation_direction` chooses the edge direction (`None` / `Tangential` / `Radial`). Returns the resulting flags including `has_blendspace_data`. In grid mode the triangulation is intentionally empty, so `has_blendspace_data` is `false` — that is correct, not a failure.

- **State machine editing — remove states, remove transitions, re-point the entry (`animation`).** Three new actions completing the anim-blueprint state-machine authoring surface. Previously you could create states and transitions and set the entry only at creation time; now you can edit a state machine after the fact.
  - `remove_anim_state` — remove a state from a state machine and tear down its inner anim graph. Params: `asset_path`, `machine_name`, `state_name`, and `remove_dependent_transitions` (default `true`, also removes transitions that referenced the state). Refuses to remove the state machine's current entry state — re-point the entry with `set_anim_entry_state` first, then remove.
  - `set_anim_entry_state` — re-point a state machine's Entry node at an existing state. Params: `asset_path`, `machine_name`, `state_name`. Returns the previous entry target, and reports `unchanged` when the named state is already the entry.
  - `remove_anim_transition` — remove the transition from one state to another. Params: `asset_path`, `machine_name`, `from_state`, `to_state`. Reports how many matching transitions were removed.

- **Remove an IK Rig solver (`animation`).** `remove_ik_solver` removes a solver from an IK Rig's solver stack by index. Params: `asset_path`, `solver_index` (0-based). Validates the index against the current solver count — an out-of-range index returns a clear error naming the valid range — and reports `removed_index` and `solver_count_after`.

- **AnimGraph-authoring pack (`animation`) — 14 new actions.** Pose-composition, slot, cached-pose, output-wiring, blend, sync, layered-blend, Control Rig, and linked-layer anim-graph node authoring, composing with the existing `add_anim_graph_node` / `connect_anim_graph_pins` write surface: `add_apply_additive`, `add_apply_mesh_space_additive` (Apply Additive / mesh-space additive nodes), `add_slot_node` (slot name validated against the skeleton's slot groups), `add_save_cached_pose` / `add_use_cached_pose` (paired by `cache_name`), `set_output_pose_source` (wire a node into the AnimGraph Output / Root result pin), `set_state_result_source` (wire a node into a state machine state's result pin), `add_blend_by_int` (grown to `num_poses` pins), `add_blend_by_enum` (Blend Poses by Enum bound to a `UEnum` via `enum_path`; one pose pin per exposed enumerator plus a Default/else pin, skipping the auto `_MAX` sentinel and `Hidden` enumerators), `set_sync_group` (player node sync group name / role / method), `set_layered_blend_bones` (per-bone branch filters on a Layered Blend Per Bone node), `add_anim_control_rig_node` (`control_rig_class`; IO pins regenerate), `add_linked_anim_layer` (`layer_name`, optional `interface_class`), `add_conduit` (a state-machine conduit whose bound graph is a transition-logic graph, not an anim graph).

- **`set_anim_node_pin_binding` bootstraps the binding object (`animation`).** It now constructs a node's binding object when it has none, so it works on previously-unbound AnimGraph nodes instead of refusing them.

- **`auto_layout` built-in formatter (`animation`).** A new `formatter:"builtin"` (also the `"auto"` fallback) lays out anim graphs by dependency-aware layering WITHOUT Blueprint Assist, so layout works in release builds where Blueprint Assist is compiled out.

- **`set_transition_rule` gains an `expression` kind (`animation`).** Compound multi-term transition conditions, extending the existing `bool` / `auto` / `compare` kinds: `terms: [{ lhs, op, rhs, abs?, negate? }]` combined with `combine: "and" | "or"`. Each term builds one comparison sub-node — optional per-term `abs` wraps the left-hand side, optional per-term `negate` inverts the term result — and all terms fold through Boolean AND/OR into the transition result. `get_transition_rule` decodes it back.

### Fixed

- **MCP-authored blend spaces played bind/A-pose at runtime (`animation`).** The four blend space mutators — `add_blendspace_sample`, `edit_blendspace_sample`, `delete_blendspace_sample`, `set_blend_space_axis` — mutated samples without rebuilding the triangulation (`FBlendSpaceData`), so a blend space authored over MCP shipped with empty triangulation and evaluated to the bind/reference pose at runtime. The asset-editor preview recomputed the triangulation live, which masked the problem in-editor. Each of the four actions now calls `ResampleData()` after the edit and marks the package dirty, so every sample/axis change re-bakes correctly. Existing broken blend spaces can be repaired in place with the new `bake_blend_space` action.

- **`add_ik_solver` failed to add solvers, Full Body IK especially (`animation`).** The action resolved the solver type through a hardcoded reflected struct path that does not resolve in UE 5.7, so the add silently failed. `solver_type` is now resolved against the live solver-struct table the engine registers — a friendly alias (`fullbodyik`/`fbik`, `limb`, `pole`, `bodymover`, `settransform`, `stretchlimb`) or the exact struct name, falling back to a unique substring; an ambiguous value returns the candidate solvers and an unknown value returns the available list. Solvers, including Full Body IK, now add correctly.

## [0.19.0] - 2026-06-13

> **Action count is approximate.** **~1,400+ actions across 25+ in-tree namespaces** — query `monolith_discover()` for the live figure.

An LLM-C++-ergonomics release: an eight-action `source` pack so your AI resolves an include, signature, deprecation, Build.cs deps, header lint, or a UCLASS stub in one round-trip, plus a parser fix that tripled the engine source index. On top: live-PIE introspection + driving (`editor`), anim-node binding read/write and time-series PIE sampling (`animation`), Blueprint variable census + contract reconciliation (`blueprint`), and T3D asset-text export (`project`). Two first-launch crash/load fixes (issue #70, thanks @aggitti) and a ~40% smaller `tools/list` manifest round it out.

### Added

- **LLM C++ authoring ergonomics (`source`) — 8 actions across three phases.** Read-only, offline-served lookups so an agent resolves an include, signature, or deprecation in one round-trip instead of reading raw source.
  - **Phase 1:** `get_include_path` (canonical `#include` for a symbol, with `includable` + owning module + `build_cs_note`), `get_signature` (exact overload signature(s), inline bodies/macro continuations stripped), `check_deprecations` (batch `UE_DEPRECATED` status; returns `index_state:"empty"` before the first reindex rather than a false clean bill). Adds a `symbol_deprecations` index (schema v1→v2) populated on the next full `trigger_reindex`, plus a `modules.build_cs_path` backfill.
  - **Phase 2:** `verify_symbols` (batch pre-flight — existence + include + signature + deprecation per symbol), `find_example_usage` (ranked real call-site examples via source-line FTS, `prefer` engine/project, cursor-paginated), `suggest_build_cs_deps` (required + missing Build.cs deps for a file or symbol list; reads the module's Build.cs from disk so it works on uncommitted files).
  - **Phase 3:** `lint_header` (regex-level UHT-gotcha lint that works on unindexed headers you just wrote), `generate_class_stub` (UCLASS-derived `.h`/`.cpp` pair returned as TEXT — never writes to disk), and `editor.get_build_errors` gains an additive deterministic `fix_hints[]` array (`LNK2019` on `Z_Construct_*` / `UDeveloperSettings`, `C4996` deprecation, `generated.h`-must-be-last).

- **Live-PIE object read/call + driving (`editor`).** `pie_get_object_properties` (read UPROPERTY values off a resolved PIE object by dotted, UDS-friendly path — read-only), `pie_call_function` (call a BlueprintCallable function/event on it — mutates live PIE state, rejects replicated/latent), `pie_set_control_rotation` (with best-effort `hold_frames`), `pie_inject_input_action` (Enhanced Input injection with shape-mapped values), `pie_possess_spectator_free` (free-fly spectator toggle). Adds an always-enabled `EnhancedInput` Build.cs dep — release-build safe, no `WITH_*` gate.

- **Stat-group counter readout (`editor`).** `get_stat_group_values` reads a stats group (`STATGROUP_Anim` or short `Anim`) into a structured response — counter values + cycle-stat timing in ms; `sample_frames>1` aggregates min/avg/max. `#if STATS`-gated (Development editor only, not Shipping/Test).

- **Anim-node function + pin bindings read/write (`animation`).** `get`/`set_anim_node_function_bindings` (the per-node On Initial Update / On Become Relevant / On Update function slots, with the engine's thread-safe + signature validation on write), `get`/`set_anim_node_pin_binding(s)` (the per-pin property-access bindings). `get_nodes` also emits compact additive `bindings` / `pin_bindings` per node.

- **Time-series PIE sampling (`animation`).** `sample_pie_timeseries` — an async PIE session that samples a target's dotted, UDS-friendly variables each tick and fires typed provocations (`set_control_rotation` / `add_movement_input` / `jump` / `console_command`) on a timeline. Poll with `poll_pie_smoke`, force-end with `stop_pie_smoke`. Plus a `compare_to_actor` lockstep parity option on `sample_pie_anim_instance`.

- **Variable reference census + contract reconciliation (`blueprint`).** `find_variable_references` finds every graph node that reads or writes a member variable (incl. thread-safe Property Access nodes), classifying each as read/write/other. `compare_class_variable_contract` and `promote_variables_to_parent` reconcile a Blueprint's local variables against a native parent during nativization.

- **First-class asset text (T3D) export (`project`).** `export_asset_text` exports an asset to its native T3D text dump (or grepped excerpts) and returns it directly — the escape hatch for surfaces no typed read exposes. Scope with `object_filter` / `grep_pattern`, bounded by `max_bytes`.

### Changed

- **`tools/list` manifest ~40% smaller — duplicated action list dropped from dispatcher descriptions.** Each domain dispatcher (`source_query`, `material_query`, etc.) previously inlined its full `"Available actions: a, b, c..."` list into the tool `description` field — a copy of names already carried authoritatively by the `action` enum in the schema. The prose join is gone; the description now points clients at `monolith_discover("<namespace>")`. `tools/list` drops from ~77.8K to ~46.7K bytes (~16k tokens) with no capability loss. The `action` enum is unchanged — no dispatch behaviour changes.

### Fixed

- **MonolithMesh first-launch load failure — GeometryScripting DLLs now delay-loaded (`mesh`, issue #70, thanks @aggitti).** On a clean build, `MonolithMesh.dll` carried a load-time hard import on `UnrealEditor-GeometryScriptingCore.dll` (+ GeometryFramework / GeometryCore), surfacing as `CouldNotBeLoadedByOS` (`LoadLibrary` null, `GetLastError=126`) on first editor launch and failing the whole Monolith plugin load. The three GeometryScripting module DLLs are now in `PublicDelayLoadDLLs`, so the Windows loader binds them lazily on first Tier-5 call instead of at module load. The full Tier-5 (`WITH_GEOMETRYSCRIPT=1`) mesh-op surface is preserved, and the `.uplugin` Plugins array is unchanged — no new mandatory dependency.
- **Deep indexer no longer asserts on UserDefinedStruct fields with unresolved types (`source`, issue #70, thanks @aggitti).** The deep indexer called `FProperty::GetCPPType()` unconditionally on every UserDefinedStruct field. For a field whose inner type can't resolve — e.g. a `TSubclassOf<X>` pointing at a deleted Blueprint, which leaves `MetaClass` null — `GetCPPType()` asserted and took the editor down mid-index. A `SafeGetCPPType()` helper now null-checks the inner type pointer and returns `<unresolved>` for broken fields instead of asserting; well-formed fields are unaffected.
- **Plain-class indexing gap — allman-brace classes/structs now indexed (`source`).** The non-reflected class/struct extractor required the opening `{` on the declaration line, but Epic's coding standard puts it on the next line (allman), which structurally excluded most exported non-reflected engine types (`FCollisionShape`, `FSceneView`, `FScopeLock`, `FPaths`, `FRegexPattern`, and ~40K others). The parser now accepts the allman brace. Engine `symbols` rows went ~301K → ~967K after a full reindex; existing `EngineSource.db` files enrich on the next `trigger_reindex`. The fix also hardened the member extractor against two latent crash classes it exposed.

## [0.18.1] - 2026-06-07

A motion-matching-focused release. A from-scratch **Motion Matching authoring pack** across the `animation`, `chooser`, and `blueprint` namespaces — Pose Search schema / database primitives, mirror data tables, chooser-table authoring, the AnimBP motion-matching graph, foot-IK, thread-safe AnimBP authoring (Property Access, thread-safe function flag, chooser-database-via-threadsafe), character/actor scaffolding, and a retarget create/run pack. Plus a **PIE / profiling harness** (async PIE-smoke sessions, CSV/Insights profiling brackets, clip + anim-frame capture, map authoring, nav rebuild/validate), **state-machine authoring + live anim telemetry**, a generic **AI controller that runs a BehaviorTree on possess** with movement-driving BT task classes, inherited-native-component inspection, and live DataAsset field read-back.

> **Action count is approximate.** The surface is too large to track to the unit — **1,400+ actions across 25+ in-tree namespaces** (public, in-tree only). Query `monolith_discover()` for the exact live figure.

Authoritative per-namespace breakdown lives in [`Docs/SPEC_CORE.md` §12](https://github.com/tumourlove/monolith/blob/master/Docs/SPEC_CORE.md#12-action-count-summary).

## [0.18.0] - 2026-06-01

Niagara HLSL direct-editing plus a round of build + reliability fixes.

Public in-tree action count is approximate — **1,400+ actions across 25+ in-tree namespaces** (query `monolith_discover()` for the live figure). The `niagara` namespace gained a net +9 this release (issue #64 Tranche 2 search & discovery +7, PR #65 HLSL +2). The `editor.delete_assets` hardening below adds no new action — it's a new param on an existing action; issue #64 Tranche 1's Blueprint-callable surface adds zero dispatcher actions.

### Added

- **Niagara HLSL direct-editing + simulation-stage / event-handler authoring (PR #65, by @middle233).** Two net-new `niagara` actions plus a set of module-stack / script / event enhancements:
  - **`get_custom_hlsl_text`** — reads the HLSL source from a `CustomHlsl` node via public UPROPERTY reflection. Params: `script_path` (required), optional `node_guid` to disambiguate multi-node scripts.
  - **`set_custom_hlsl_text`** — overwrites a `CustomHlsl` node's HLSL source under a `Modify()` + transaction with a recompile. Params: `script_path` (required), `hlsl` (required), optional `node_guid`.
  - **Selector-based stage targeting.** `get_ordered_modules` / `add_module` / `move_module` / `duplicate_module` now accept `usage: "particle_simulation_stage"` (selectors `usage_id` / `stage_name` / `stage_index`) and `usage: "particle_event"` (selectors `usage_id` / `handler_index`), so you can target shared-graph simulation-stage and event scripts directly.
  - **`add_simulation_stage`** now materializes the matching `particle_simulation_stage` output node and returns `usage_id`, `stage_id`, `graph_outputs`.
  - **`add_event_handler`** now returns `handler_index` + `usage_id` + `usage` and rejects unresolved inter-emitter source emitters instead of silently creating an empty `SourceEmitterID`.
  - **`create_module_from_hlsl`** now generates a ParameterMap bridge graph (InputMap → ParameterMapGet → CustomHlsl → ParameterMapSet → OutputNode), preserves Data-Interface input types (NeighborGrid3D / Grid3D / ParticleRead), and strictly validates HLSL input/output types — unknown types now hard-fail instead of silently degrading to `float`.
  - See `Docs/NIAGARA_HLSL_GUIDE.md` for the full HLSL workflow.

- **Niagara search & discovery pack — +7 actions (issue #64 Tranche 2).** `search_by_parameter`, `search_by_data_interface`, `search_by_material`, `query_niagara` (deterministic structured-filter DSL — `emitters>N`, `sim_target=GPU`, `has_renderer=<name>`), `find_similar_systems` (structural-similarity ranking), `find_niagara_references` (`IAssetRegistry::GetReferencers`), and `list_system_data_interfaces` (data interfaces actually used by a system). All read-only.

- **Blueprint-callable Niagara inspection/search surface — `UMonolithNiagaraQueryLibrary` (issue #64 Tranche 1).** An editor-only `UBlueprintFunctionLibrary` exposing the read-only `niagara` dispatcher actions as Blueprint-callable nodes for Blueprint utilities and Editor Utility Widgets (24 nodes total — 17 Tranche 1 wrappers + 7 Tranche 2 wrappers). Zero new dispatcher actions, zero cost in packaged / runtime builds (the module is editor-only).

### Fixed

- **`editor.delete_assets` could pop a blocking modal and freeze the editor.** Deleting an asset that was open in an editor tab, or still referenced, would raise a Slate "asset in use" / "save changes" dialog — which hangs an unattended MCP session because no human is there to dismiss it. The action now closes any open asset editor and clears the package dirty flag for each target before deleting, and runs the delete inside an unattended-script guard so the engine never raises a blocking dialog. A new optional `force` bool (default `false`) selects the path: `force=false` soft-deletes after closing editors; `force=true` calls `ForceDeleteObjects`, nulling referencers. Per-asset failures are reported in a `failed_to_delete` array instead of aborting the whole call. Native C++ — no Python required. **Known limitation:** a NiagaraScript created and compiled in the same session can't be deleted until its compile state clears (a transient compilation graph holds a reference — the engine's own Content Browser hits the same wall); it becomes deletable after the state clears, e.g. on editor restart.

- **From-scratch unity editor builds failed to compile** due to duplicated file-local helpers colliding across `.cpp`s in the same module in `MonolithReflectionIntel`, `MonolithNiagara`, `MonolithGAS`, and `MonolithBlueprint`. UE adaptive unity concatenates same-module `.cpp` into one translation unit, so these internal-linkage symbols clashed (C2084/C2011/C2668). Masked from releases by `-DisableUnity` and masked locally because adaptive unity excludes recently-edited files — so it only bit fresh-clone / full-unity (end-user first-compile) builds. Helpers are now unity-safe (hoisted shared units + file-unique names); behaviour-preserving, no API change. A new full-unity collision gate in `make_release.ps1` guards against regressions. (#68, reported by @likeitlotlot-commits)

- **Noisy ensure during full level indexing on landscape-heavy projects.** The level indexer loads each World purely to enumerate placed actors, which lazily initializes a `ULandscapeSubsystem`; tearing the world down then tripped a GC ensure. The indexer now unregisters every landscape proxy's components before driving `UWorld::CleanupWorld`, so the subsystem deinitializes cleanly with zero ensures and no residency cost. (#67, reported by @likeitlotlot-commits)

## [0.17.0] - 2026-05-29 (TBD)

A big one. Two major surfaces land here: the **MCP LLM Ergonomics Pack** (universal response shaping, schema-tagged param kinds, fuzzy-match dispatch errors, MCP tool annotations, cursor pagination, proxy JSONL call log, Niagara temporal-control + stateless-emitter pack) and **Reflection Intelligence** (deterministic reflection layer for architectural-decision lookup, repo-risk signals, UE 5.7 UHT reflection edges, replication inspection, read-only pipeline composers, and index maintenance). Zero LLM calls inside the new surface, zero embeddings, zero network, zero new optional dependencies.

Public in-tree action count: **1387 actions across 25 in-tree namespaces** (1384 at v0.17.0 ship + the [Unreleased] `cppreflect_query("list_class_specifiers")` follow-up + the [Unreleased] network-completeness `reflect_query("rebuild_reflection_index")`). Live `monolith_discover()` reports **1610 across 29 namespaces** when extension plugins are loaded — those advisory totals do not affect the public release headline. With the experimental town-gen registration (`bEnableProceduralTownGen=true`) the in-tree total rises by +45 to 1431. Authoritative per-namespace breakdown lives in [`Docs/SPEC_CORE.md` §12](https://github.com/tumourlove/monolith/blob/master/Docs/SPEC_CORE.md#12-action-count-summary).

**Six new in-tree namespaces:** `decision`, `risk`, `cppreflect`, `network`, `pipeline`, plus the [Unreleased] `reflect` index-maintenance namespace.

### Added — Reflection Intelligence

- **`decision_query` namespace (5 actions, Phase 1) from the new `MonolithReflectionIntel` module.** Deterministic markdown decision-record harvest — specs, plans, `CHANGELOG.md`, `.claude/rules/` — into `decision_records` + `decision_supersedes` SQLite tables on top of `EngineSource.db`. Actions: `list_decisions`, `get_decision`, `list_stale`, `find_supersession_chain`, `find_referent_decisions`. Three heuristic tiers with distinct confidence floors: YAML frontmatter `decision: true` / `status:` (0.90), `## ADR-N` / `## Architectural Decision` headers (0.85), markdown header followed within 8 lines by a paragraph containing `because` / `rationale` / `evidence` / `decision:` (0.65). Lazy bootstrap on first call + `FCoreUObjectDelegates::ReloadCompleteDelegate` refresh on Live Coding / UBT hot-reload. Every action carries the v0.17.0 ergonomics surface: `EMonolithParamKind::DiskPath` on `path_filter`, `readOnlyHint + idempotentHint`, universal response shaping (`_fields` / `_omit` / `_compact_json`), opaque base64+JSON cursor pagination on the two list-style actions.

- **`UMonolithReflectionIntelSettings` UDeveloperSettings** (Editor Preferences → Plugins → "Monolith Reflection Intel"). Surfaces toggles + tuning across all four phases: `bEnableDecisionMining`, `DecisionMinConfidence`, `DecisionMarkdownRoots`, `bIndexProjectPluginReflection` ([Unreleased], default `true`), `bIndexMarketplacePluginReflection` ([Unreleased], default `false`), `bIndexEnginePluginReflection`, `UHTArtefactRoot`, `bEnableGitCoChangeMining`, `MaxCoChangeWindowCommits`, `MaxCommitFileCount`, `GitMiningNoiseFilter`, `bEnableNetworkReplicationAudit`, `bEnablePipelineComposers`. INI section: `[/Script/MonolithReflectionIntel.MonolithReflectionIntelSettings]` in `Config/MonolithSettings.ini`.

- **([Unreleased]) Plugin-aware scan scope for the reflection indexers — no new actions, no count change.** The CppReflect + Network UHT-artefact indexers no longer scan the project game module alone. Scope is driven by `IPluginManager::GetEnabledPlugins()` along a game-module → project-plugin → marketplace ladder. `bIndexProjectPluginReflection` (default `true`) scans every enabled `LoadedFrom == Project` plugin's UHT artefacts; `bIndexMarketplacePluginReflection` (default `false`) also scans enabled engine-installed marketplace plugins (`LoadedFrom == Engine` under `/Plugins/Marketplace/`); Epic engine built-ins stay excluded (governed by `bIndexEnginePluginReflection`, default off). This is the change that makes RPC + replicated-class detection work on real projects — those declarations live in project plugins, which are now in scope by default. Verified E2E via a project-only force-reindex: game module alone ~30 artefacts → with project plugins on 337 (project-plugin Server RPCs + replicated classes) → with marketplace flag also on 927 artefacts / 745 UClasses. No engine source-symbol reindex is triggered — the ladder only widens the UHT-artefact scan.

- **`risk_query` namespace (5 actions, Phase 2) for repo-level risk signals.** Actions: `get_hotspot_score` (per-file blended churn × LOC score), `get_cochange_pairs` (files that frequently change together with a given anchor file), `get_file_churn`, `get_release_window_hotspots`, `list_conditional_gates` (`#if WITH_*` macros + `bHas*` 3-location probes + `MONOLITH_RELEASE_BUILD` bypasses). Mines git via `FPlatformProcess::CreateProc` against each nested repo's `.git/`, skipping any path without `.git/` present. Hotspot score formula is deterministic and traceable: `0.6 * normalised_churn + 0.4 * normalised_loc`. Co-change pair detection caps per-commit file count at `MaxCommitFileCount` (default 50) to suppress tree-wide refactor + initial-import noise. Writes into four new SQLite tables: `git_file_churn`, `git_cochange_pairs`, `risk_hotspot_scores`, `reflect_conditional_gates`.

- **`source_query("audit_module_dep_reality")` action (Phase 2, cross-namespace registration).** Catches the LNK2019 bug class where UPROPERTY (or any reflection-touching declaration) references a foreign-module type whose owning module is missing from the declaring module's `Build.cs` `Private/PublicDependencyModuleNames`. UHT generates `Z_Construct_*_NoRegister` calls that link against the foreign module's API macro at link time; the failure surfaces as a confusing LNK2019. Algorithm: regex-parse every `*.Build.cs` under `Source/` for declared deps, regex-extract type-bearing reflection declarations from every `*.h` / `*.cpp`, resolve each extracted type against `EngineSource.db`'s symbol → owning module mapping, emit a violation when the owning module is not declared and not on the implicit-deps whitelist (`Core`, `CoreUObject`, `Engine`, `Projects`, `RHI`, `RenderCore`). Cursor-paginated with `module_filter` substring scope. The audit handler is owned by `MonolithReflectionIntel` but registers onto the existing `source` namespace for caller ergonomics — agents already discover `source_query` first.

- **`cppreflect_query` namespace (6 actions — 5 in Phase 3a + 1 [Unreleased]) for UE 5.7 reflection-edge queries.** Actions: `get_uclass` (UHT-derived UCLASS record — parent class, specifiers, source path), `list_uproperties` (cursor-paginated UPROPERTY surface for a UCLASS), `list_ufunctions` (cursor-paginated UFUNCTION surface, with raw `EFunctionFlags` bitfield + return type + per-param JSON), `find_interface_impls` (every C++ UCLASS implementing a given UINTERFACE), `find_class_specifier` (every UCLASS carrying a given specifier), and `list_class_specifiers` ([Unreleased] — returns the distinct universe of tokens stored in the `flags` column of `reflect_uclasses`, each with a per-token class count; those tokens are UHT metadata keys like `IsBlueprintBase` / `BlueprintType` / `Abstract`, NOT raw C++ specifiers, so it's the discovery companion telling you what `find_class_specifier` can match). [Unreleased] also made `find_class_specifier` forgiving: alias map (`Blueprintable` → `IsBlueprintBase`), honest not-captured note for specifiers UHT drops (`MinimalAPI` / `NotBlueprintable`), case-insensitive matching. Drives a regex sweep over UHT artefacts (`Intermediate/Build/Win64/.../Inc/<Module>/UHT/*.gen.cpp`) cross-joined with `IAssetRegistry::GetDependencies` for the asset side. **No tree-sitter dependency, no ThirdParty vendoring** — substrate is deterministic file IO plus 8 regex patterns derived from real `.gen.cpp` inspection. Writes into six new SQLite tables: `reflect_uclasses`, `reflect_uproperties`, `reflect_ufunctions`, `reflect_uinterfaces`, `reflect_uinterface_impls`, `cpp_asset_edges` (asset path → C++ class via `/Script/<Module>` package dependency, coarse `edge_kind='package_dep'` in Phase 3a). Cross-joining the UE class graph with `IAssetRegistry` lets agents answer "what assets reference this C++ class?" without manual reference-viewer walks.

- **`network_query` namespace (4 actions, Phase 4a) for UE 5.7 replication inspection.** Actions: `list_replicated_classes` (UCLASSes carrying at least one replicated property, sortable by replicated-property count — **as of the [Unreleased] network-completeness workstream this captures bare `UPROPERTY(Replicated)` + `DOREPLIFETIME` via `CPF_Net` in addition to `ReplicatedUsing`, verified E2E** against the project's replicated character/attribute classes), `list_rpc_functions` (filter `reflect_ufunctions` by replication specifier from `EFunctionFlags` as of [Unreleased] — **covers project plugins by default** via the [Unreleased] scan-scope ladder, so it returns the project's actual RPCs, verified E2E with the project's project-plugin Server RPCs; the prior "empty because game-module-only" status is resolved), `list_onrep_handlers` (every `OnRep_*` UFUNCTION paired with the property it covers via `reflect_replicated_properties.rep_notify_func` join), `audit_unbalanced_onreps` (catches typo + rename drift — `ReplicatedUsing=OnRep_X` declarations whose `OnRep_X` function does not exist). Drives a second UHT-artefact regex sweep (independent of Phase 3a's reader for separation of concerns) over per-property `MetaData` blocks plus the `CPF_Net` property flag. Writes into one new SQLite table: `reflect_replicated_properties`. All four cursor-paginated.

- **`pipeline_query` namespace (2 composer actions, Phase 4a).** `pr_review` — changed-files PR review composer; for each path in `changed_files[]`, fans out `risk_query("get_hotspot_score")` + `risk_query("get_cochange_pairs")` + `decision_query("list_decisions", path_filter=path)` + `source_query("audit_module_dep_reality")` + optional `blueprint_query("audit_cdo_drift")`, aggregates per-path; hard cap 100 paths per call. `release_readiness` — release pre-flight composer; bundles `monolith_status()` + `decision_query("list_stale")` + `risk_query("get_release_window_hotspots")` + the sentinel-list audit + CHANGELOG completeness audit specced in `.claude/rules/scoped/monolith-release.md`. Both composers are read-only end-to-end. Both fan out serially on the game thread; no `ParallelFor`, no async dispatch.

- **`reflect_query` namespace (1 action, [Unreleased] network-completeness workstream).** `rebuild_reflection_index` (no params) — a project-only force-rebuild of the RI reflection tables (`reflect_uclasses` / `reflect_uproperties` / `reflect_ufunctions` / `reflect_uinterfaces` / `reflect_uinterface_impls` / `cpp_asset_edges` + `reflect_replicated_properties`). Re-runs the RI indexers over PROJECT UHT artefacts only (Epic engine built-ins excluded). With the [Unreleased] scan-scope ladder, "project" means the game module plus enabled `LoadedFrom == Project` plugins by default (and marketplace plugins when enabled), so a rebuild repopulates project-plugin reflection — which is why `network_query("list_rpc_functions")` returns the project's RPCs after a rebuild. Exists because after an indexer code change there's no other clean repopulation trigger — lazy bootstrap only fires on table-absence, `OnReloadComplete` only on Live Coding, and `source_query("trigger_reindex")` is the full-engine reindex. WRITE/maintenance action (not read-only), idempotent (wipe-and-rewrite per indexer in a single transaction), non-destructive.

- **Four cross-namespace audit actions (Phase 4a).** Each owned by `MonolithReflectionIntel` but registered onto the host namespace's adapter for caller ergonomics. `material_query("audit_orphan_materials")` — `/Game/` path scan via `IAssetRegistry::GetReferencers` for zero-reference materials. `niagara_query("audit_cross_asset_refs")` — broken/stale asset reference scan over Niagara systems/emitters joined against Phase 3a's `cpp_asset_edges`. `blueprint_query("audit_cdo_drift")` — Blueprint child CDOs that override a native C++ parent's default value (catches drift when a native default changes upstream and BP children silently keep the stale override). `project_query("audit_orphan_assets")` — project-wide zero-reference scan cross-validated against Phase 3a's `cpp_asset_edges` (surfaces assets referenced only from C++ but not from BP / asset graph). All four read-only + cursor-paginated; `path_prefix` carries `EMonolithParamKind::AssetPath` for automatic `\` → `/` rewrite with surfaced warning.

- **Automation tests for Reflection Intelligence.** +4 under `Monolith.ReflectionIntel.Decision.*` (`SchemaBootstrap`, `HeuristicAccuracy`, `SupersessionChain`, `StalenessFlag`). +6 under `Monolith.ReflectionIntel.Risk.*` (`RiskSchemaBootstrap`, `ChurnAggregation`, `CoChangePairSymmetry`, `HotspotScoreFormula`, `ConditionalGateSweep`, `MonsterCommitSuppression`). +tests under `Monolith.ReflectionIntel.ModuleDepReality.*`. +4 under `Monolith.ReflectionIntel.CppReflect.*` (`CppReflectSchemaBootstrap`, `UClassFixtureExtraction`, `InterfaceImplResolution`, `AssetGraphJoin`). Phase 4a (network + pipeline + cross-namespace audits) shipped with manual smoke only — automation tests deferred. All fixture corpora live under `Source/MonolithReflectionIntel/Private/Tests/Fixtures/`. Disposable test DBs at `FPaths::AutomationTransientDir()`; the real `EngineSource.db` is never touched by tests.

### Added — MCP LLM Ergonomics Pack

- **Universal response-shaping params on every action (Phase 1.0).** Three opt-in universal params reshape the JSON response post-dispatch: `_fields: string[]` (whitelist top-level response keys), `_omit: string[]` (blacklist, mutually exclusive with `_fields`), `_compact_json: bool` (drop top-level keys whose value is `null` / `""` / `{}` / `[]`). Underscore-prefixed to avoid collisions. Registry-level allowlist so no per-action schema migration was required. Top-level keys only in Phase 1; JSONPath / JMESPath grammar deferred.

- **Schema-tagged param kinds via `EMonolithParamKind` (Phase 1.0 + 1.1 sweep).** New enum on `FParamSchema` with four variants: `Other` (default, never rewritten), `AssetPath` (dispatcher rewrites `\` → `/` with a surfaced warning), `DiskPath` (native OS path; explicit opt-out for clarity), `GameplayTag` (reserved). 87-file sweep tagged ~913 builder call sites as `AssetPath` and ~18 as `DiskPath`. New `FParamSchemaBuilder` sugar overloads `RequiredAssetPath` / `OptionalAssetPath` / `RequiredDiskPath` / `OptionalDiskPath`. Back-compat preserved — every existing `.Required(...)` / `.Optional(...)` call site defaults to `Kind == Other` and opts OUT of path normalisation.

- **`did_you_mean` fuzzy match on dispatch errors (Phase 2).** Unknown-action and unknown-namespace dispatch failures now carry `error.data.suggestions` — top-3 closest registry keys with normalised scores via UE `Algo::LevenshteinDistance`. Snapshot-then-unlock pattern keeps the hot dispatch lock free of scoring work. `error.data.kind` field disambiguates `"action"` vs `"namespace"` suggestions.

- **MCP-spec tool annotations on `tools/list` (Phase 2).** Five audited read-only / idempotent dispatchers (`monolith_discover`, `monolith_status`, `monolith_guide`, `monolith_reindex`, `source_query`) now serialise `readOnlyHint` / `destructiveHint` / `idempotentHint` / `title` hints so LLM clients can pre-filter destructive vs read-only calls without dispatching them first. Annotations only emitted when at least one field is non-default.

- **`source_query("search_source")` cursor pagination (Phase 3).** New opaque base64+JSON `cursor` in-param and `next_cursor` / `total_estimate` out-params. Page 0 emits `total_estimate` via a server-side SQLite FTS5 `MATCH COUNT(*)`. Rerun-slice scheme (FTS5 `bm25()` rank is unstable under inserts/deletes). Hard cap 1000 rows total per query. Query-hash mismatch returns a clean `INVALID_CURSOR` error rather than silently serving the wrong slice. `project_query("search")` cursor pagination deferred — architecturally blocked by `FMonolithIndexDatabase::FullTextSearch`'s UNION-and-resort without deterministic ordering.

- **Proxy-side JSONL call log (Phase 4).** Both proxies now emit one line per call to `Saved/Logs/MonolithCalls.jsonl` (project-root-relative). Eight-field schema: `ts` (ISO-8601 UTC), `namespace`, `action`, `params_hash` (SHA-1 hex over canonicalised params JSON), `duration_ms`, `ok`, `error_code` (omitted on success), `result_bytes`. Native proxy (`Tools/MonolithProxy/monolith_proxy.cpp`) and Python fallback (`Scripts/monolith_proxy.py`) implement the same schema. Opt-out: set `MONOLITH_CALL_LOG=0`. Local-only, no phone-home. User-managed rotation (delete the file to reset). Native proxy requires `build_proxy.bat` rebuild + Claude Code MCP reconnect to engage; Python picks up on next Claude Code start.

- **Niagara temporal-control surface (+9 `niagara::` actions).** Collapses scattered timing edits (per-property `set_system_property` + `set_simulation_stage_property` + `set_module_input_value` against `EmitterState` / `InitializeParticle`) into composite, intent-named writers. **System-level (4):** `get_system_timing` (bundled read of `WarmupTime` / `WarmupTickCount` / `WarmupTickDelta` / `bFixedTickDelta` / `FixedTickDeltaTime` / `bRequireCurrentFrameData`), `set_warmup_profile`, `set_fixed_tick_delta`, `set_require_current_frame_data`. **Emitter (2):** `set_emitter_loop_profile` (composite write of EmitterState loop topology), `get_emitter_timing_summary` (read aggregator pulling loop topology + sim_stages + `InitializeParticle` lifetime fields into one response). **Sim-stage aliases (2):** `set_sim_stage_iteration_count`, `set_sim_stage_execute_behavior` — both alias atop `set_simulation_stage_property` with `stage_index` / `stage_name` selector convention. **Particle lifetime (1):** `set_particle_lifetime` (convenience write to `InitializeParticle` — Direct mode with constant `Lifetime` or Random mode with min/max).

- **Niagara stateless-emitter factory — `create_stateless_emitter` action.** Creates a standalone `UNiagaraStatelessEmitter` (Lightweight Emitter) asset for programmatic test-asset setup. Uses `FindObject<UClass>(nullptr, "/Script/Niagara.NiagaraStatelessEmitter")` + type-erased `NewObject` to avoid coupling to Niagara's `Internal/Stateless/` headers. Pairs with new stateless-aware branches added to `set_emitter_loop_profile` and `get_emitter_timing_summary` (see Changed).

### Changed

- **Niagara `set_emitter_loop_profile` and `get_emitter_timing_summary` now handle `UNiagaraStatelessEmitter` assets natively.** The previous stateless-detection points (which early-outed with the placeholder "set_stateless_loop_profile coming" hint) now dispatch into real reflection-based read/write paths against the protected `EmitterState` (`FNiagaraEmitterStateData`) UPROPERTY. Detection uses `StaticLoadObject` + class-name match; the stateful module-routed path is unchanged. New optional `loop_duration_mode` param on `set_emitter_loop_profile` accepts `"Fixed"` / `"Infinite"` (maps to `ENiagaraLoopDurationMode` — meaningful only on stateless). Responses on the stateless branch include a `stateless: true` flag; `get_emitter_timing_summary` additionally returns `null` for all 4 `InitializeParticle` lifetime fields and `sim_stages: []`. Same action names, same call surface — purely an additive dispatch branch.

- **Both proxies advertise the universal params on every tool's inputSchema.** Native `Tools/MonolithProxy/monolith_proxy.cpp` and Python `Scripts/monolith_proxy.py` now emit `_fields` / `_omit` / `_compact_json` on every tool descriptor returned from `tools/list`. The native proxy requires a `build_proxy.bat` rebuild + Claude Code MCP reconnect to engage end-to-end; the Python fallback picks up the change on next restart.

- **Symmetric params-string unwrap on the `monolith_` branch of `HandleToolsCall`** — mirrors the pre-existing `_query` branch behaviour so meta-namespace tools (`monolith_discover`, `monolith_status`, `monolith_guide`, etc.) accept the same nested-`params` shape that domain tools already handled.

### Fixed

- **Response-shaping post-filter (`_fields` / `_omit` / `_compact_json`) now engages on the MCP wire.** Two-investigator bug-investigator consensus identified the root cause: Claude Code's MCP client serialises array-valued top-level arguments as JSON-encoded strings rather than native arrays. `MonolithJsonUtils::ReadStringArrayParam` (and the `_compact_json` bool read inside `ApplyResponseShaping`) now carry a string-fallback mirroring the existing `params`-key special-case in `MonolithHttpServer.cpp`. Native-array path remains FIRST. Live verification: `monolith_status({_fields:["version","total_actions"]})` returns exactly `{"version":"0.17.0","total_actions":1609}`; `editor_query("get_recent_logs", _fields:["count"])` returns `{"count":1}` instead of the prior full envelope.

### Deferred to future releases

- **Phase 3b — tree-sitter source-vendored native gameplay-tag tracking.** `cppreflect_query("list_native_tags")` plus `reflect_native_tag_decls` + `reflect_native_tag_externs` tables. UHT does not emit native-tag metadata (`UE_DEFINE_GAMEPLAY_TAG_*` / `UE_DECLARE_GAMEPLAY_TAG_EXTERN`), so source-level parsing is the only honest substrate. Vendoring `tree-sitter-cpp` (~50MB source weight, ~1.1M-line generated `parser.c`) was too much against the current ~12MB Monolith release-zip baseline. Phase 3b would also backfill the currently-empty `reflect_uproperties.blueprint_visibility` / `.specifiers` fields plus a finer-grained `cpp_asset_edges.edge_kind`.

- **Phase 4b — two audit families blocked on Phase 3b substrate.** `gas_query("find_tag_consumers" / "find_grant_paths" / "find_revoke_paths")` needs native-tag declaration tracking. `animation_query("audit_thread_safety")` needs `reflect_uproperties.specifiers` populated. Both deferred together — the combined value is real but not blocking the release-readiness work Phase 4a directly addresses. (Bare `UPROPERTY(Replicated)` detection, previously a Phase 4b item, LANDED in the [Unreleased] network-completeness workstream — `list_replicated_classes` now captures it via `CPF_Net`.)

- **Phase 5+ — cursor codec consolidation.** The base64+JSON cursor envelope is now duplicated five times across Decision / Risk / CppReflect / SourceAudit / Network adapters. Refactor target for the next release window.

### Internal

- **Automation tests added.** Phase 1.0 brought +11 under `Monolith.ResponseShaping.*` and `Monolith.ParamKind.*`. Phase 2 brought +10 under `Monolith.FuzzyMatch.*` and `Monolith.CursorPagination.*`. Reflection Intelligence brought +4 / +6 / +4 across Decision / Risk / CppReflect (Phase 4a deferred to manual smoke). All passing.

- **No `*.Build.cs` changes for the ergonomics surface; one `AssetRegistry` add to `MonolithReflectionIntel.Build.cs` for Phase 3a.** No `.uplugin` changes. No new module dependencies for the ergonomics work. Phase 3a needed `AssetRegistry` for `IAssetRegistry::GetDependencies` joins.

## [0.16.0] - 2026-05-27

A small, focused release between the v0.15.0 ergonomics framework and the next major surface. Headline work: a five-action **preview & inspection surface** expansion in the `editor::` namespace (renders + structural-data reads for tech-art and AI agents), one MCP-introspection hint that points agents at the schema-discovery surface before they guess parameter names, and two component-persistence fixes from a community-reported regression.

Public in-tree action count is approximate (Editor expanded 29 → 33). Per the Action Count Discipline this EXCLUDES sibling-plugin actions — sibling plugins are not in the public release zip. With the experimental town-gen registration (`bEnableProceduralTownGen=true`) the in-tree total rises by +45.

### Added

- **Preview & inspection surface expansion (`editor::` namespace):** extended `editor::capture_scene_preview` `asset_type` enum to support `static_mesh`, `skeletal_mesh` (with optional `animation_path` + `seek_time` for posed capture), and `widget` (UMG via `FWidgetRenderer` with `scale` DPI multiplier). New `editor::capture_material_grid` (N material instances side-by-side under shared lighting; auto-grid layout via `ceil(sqrt(N))` with optional `columns` override). New `editor::capture_with_overlay` (5 engine debug-view modes: wireframe, normals, uv_density, lightmap_density, shader_complexity). New `editor::inspect_material_pbr` (PBR texture parameter classification + ORM/ARM/MRA channel-packing detection — pure reflection, no rendering). New `editor::inspect_texture_channels` (per-channel R/G/B/A min/max/mean statistics + optional per-channel split PNGs). All editor-only. AI discoverability via new `monolith_guide` recipe entries.

- **Schema-discovery hint in MCP `initialize` instructions (Issue #62, @middle233).** Both the C++ HTTP server (`HandleInitialize`) and the Python proxy now point AI agents at `monolith_discover`, `describe_query("action_schema")`, and `monolith_guide` from the initial handshake — so clients read schemas instead of trial-and-erroring parameter names. No new action; widens the existing introspection surface's discoverability.

### Fixed

- **Component persistence (Issue #63, @Heiselisha):** `mesh.convert_to_hism`, `mesh.place_spline`, and `ai.place_smart_object_actor` now call `AActor::AddInstanceComponent` on every component they create, so the components survive level save/reload. Convert-to-HISM also gains a pre-destroy guard that preserves source actors if HISM instance creation reports a count mismatch. `mesh.place_spline` follow-up: root + spline components now spawn with `EComponentMobility::Static` so saved spline data round-trips through the level's Static-mobility persistence path.

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
- **macOS build CI workflow scaffold** (Issue [#25](https://github.com/tumourlove/monolith/issues/25)) — `.github/workflows/macos-build.yml` triggers on `v*` tag pushes and dispatches a macOS build job to a self-hosted runner labelled `[self-hosted, macOS, monolith]`. Mirrors `make_release.ps1`'s release-build env (`MONOLITH_RELEASE_BUILD=1`, `-DisableUnity`, sibling-strip, `Installed: true` patch, SHA256 emit, `softprops/action-gh-release` attach). Two known gaps documented in workflow-header comments: (1) self-hosted runner provisioning (Mac with UE 5.7 + Xcode CLT + ~150GB disk + `UE_57` env var), (2) project-shell gap — `MonolithEditor.Target.cs` does not exist in the plugin source tree, so the workflow fails-fast with two resolution paths (commit a CI-only project shell into the plugin repo, OR point a project-path env var at a parent project on the runner). Workflow does not fire until the runner is online.

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
- **Baseline vitals AttributeSet** (F4) — six `FGameplayAttributeData` (Health/MaxHealth/Sanity/MaxSanity/Stamina/MaxSamina), `PreAttributeChange` clamps, `PostGameplayEffectExecute` re-clamps, REPNOTIFY_Always replication. Additional resistance attributes deferred.
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

- **MonolithAudio module shipped** — 81 actions across Phases 0-2: Sound asset CRUD (15), query/search (10), batch operations (10), Sound Cue graph building (21), MetaSound Builder API integration (25). Power actions: `build_sound_cue_from_spec`, `build_metasound_from_spec`, `apply_audio_template`. MetaSound features gated on `WITH_METASOUND` (graceful degradation when absent). Module had been completed + tested internally on 2026-04-08; v0.13.0 is its public debut.
- **Indexer RAM tier auto-detect** — `FMonolithMemoryHelper` picks memory budget + batch sizes from installed RAM: 64+ GB → 32768 MB / deep=8 / post=4; 32+ GB → 16384 MB / deep=8 / post=4; 16 GB → 6144 MB / deep=4 / post=2; <16 GB → 3072 MB / deep=2 / post=1. Settings defaults changed to `0` (auto-detect sentinel) for `MemoryBudgetMB`, `DeepIndexBatchSize`, `PostPassBatchSize`. Override via Project Settings > Monolith > Indexing > Performance. Tier logged once per editor session on first index run.

### Fixed

- **Indexer OOM + reentrant texture compiler crash on large projects** ([#16](https://github.com/tumourlove/monolith/issues/16)) — deep-index batches could exhaust physical RAM or re-enter `FTextureCompilingManager::ProcessAsyncTasks`, crashing the editor on large projects (>20 GB content). Fix introduces `FAssetCompilingManager::FinishAllCompilation()` guards before each batch, forced GC between batches, Slate-safe yields, emergency pause when available memory drops below 2 GB, and honors the async notification Cancel button. Shipped as PR [#17](https://github.com/tumourlove/monolith/pull/17) from **@alakangas**. Reported by **@MAYLYBY**.

### Changed

- **`bLogMemoryStats` default flipped to `false`** — opt in when debugging indexer memory behavior. Keeps shipped-project logs quiet.

### Credits

- **@alakangas** — PR [#17](https://github.com/tumourlove/monolith/pull/17) (indexer OOM + reentrant compiler crash fix)
- **@MAYLYBY** — issue [#16](https://github.com/tumourlove/monolith/issues/16) (detailed crash report that drove the fix and uncovered the low-spec regression addressed by the RAM tier auto-detect)

Full diff: [v0.12.1...v0.13.0](https://github.com/tumourlove/monolith/compare/v0.12.1...v0.13.0)

## [0.12.1] - 2026-04-03

### Fixed

- **UE 5.7 compatibility** — resolved deprecated API usages causing C2220 in non-unity builds ([#12](https://github.com/tumourlove/monolith/issues/12))
- **Non-unity build** — fixed symbol collisions across 8 files (module-prefixed anonymous helpers)
- **ComboGraph log category** — proper extern declaration, no duplicate defines
- **Uninitialized variables** — zero-initialized `FVector` locals

### Improved

- StateTree schema resolution with multiple fallback paths
- UI animation: binding helpers, transform/color component keyframes, GUID bookkeeping
- UI param handling: safer required/optional field accessors, duplicate-asset guard

## [0.12.0] - 2026-04-01

Biggest release yet: +310 actions (815 to 1125). Two new domain modules (MonolithAI, MonolithLogicDriver), ComboGraph expansion. Python-to-C++ port of standalone tools. 14 skills (up from 12).

### Added

**MonolithAI (229 actions) — AI Asset Manipulation**

The most comprehensive AI tooling available through any MCP server. 229 actions across 15 categories, 24K lines C++, 30 files. Full lifecycle management for Behavior Trees, Blackboards, State Trees, EQS, Smart Objects, AI Controllers, AI Perception, Navigation, Runtime, Scaffolds, Discovery, and Advanced categories.

Crown jewels: `build_behavior_tree_from_spec` and `build_state_tree_from_spec` — hand the AI a JSON description and it builds the entire asset programmatically. Conditional on `#if WITH_STATETREE` and `#if WITH_SMARTOBJECTS` (required, both ship with UE). Optional: `#if WITH_MASSENTITY` and `#if WITH_ZONEGRAPH` for large-scale AI.

**MonolithLogicDriver (66 actions) — Logic Driver Pro State Machines**

Full integration with Logic Driver Pro. SM CRUD, graph read/write, node configuration, runtime/PIE control, JSON spec import/export, scaffolding (door controller, health system, AI patrol, dialogue system, elevator, puzzle, inventory), component management, text graph visualization, discovery. Reflection-only integration (no direct C++ API linkage) — works with any Logic Driver Pro version. Conditional on `#if WITH_LOGICDRIVER` with 3-location Build.cs detection. 2 new skills: `unreal-logicdriver`, `unreal-combograph`.

**MonolithComboGraph expanded (12 to 13 actions)**

Added `auto_layout` action for combo graph node arrangement.

**Standalone C++ Tools (Python-to-C++ port)**

Two standalone C++ executables replace the Python scripts. Zero Python dependency at runtime.

- **`monolith_proxy.exe`** (473KB) — MCP stdio-to-HTTP proxy. Full feature parity with `monolith_proxy.py`: JSON-RPC, health poll, tool dedup, editor query splitting, action allowlist/denylist. Built with WinHTTP + nlohmann/json, zero UE dependency. Source: `Tools/MonolithProxy/monolith_proxy.cpp` (775 lines).
- **`monolith_query.exe`** (1.8MB) — Offline DB query tool. Replaces `monolith_offline.py` AND `MonolithQueryCommandlet`. 14 actions: 9 source + 5 project. Built with sqlite3 amalgamation, zero UE dependency, instant startup. Source: `Tools/MonolithQuery/monolith_query.cpp` (1080 lines).

Python scripts remain as deprecated fallbacks. `MonolithQueryCommandlet` deleted (the standalone exe is faster).

### Changed

- Total: 815 -> 1125 actions across 15 modules (was 13), exposed through 18 MCP tools (was 15)
- Blueprint: 86 -> 88 actions
- ComboGraph: 12 -> 13 actions
- Skills: 12 -> 14 bundled with plugin
- `.mcp.json` proxy config: recommend `monolith_proxy.exe` over Python script
- Python is no longer required for any core functionality (only for optional project C++ source indexing)

---

## [0.11.0] - 2026-03-30

Massive expansion: +372 actions (443 to 815). Three new modules (MonolithMesh, MonolithGAS, MonolithBABridge). MCP auto-reconnect proxy for Claude Code. Optional module system for third-party plugin integrations. 12 skills (up from 9).

### Added

- **MonolithMesh (242 actions)** — 22 capability tiers: mesh inspection, scene manipulation, spatial queries, blockout, GeometryScript, horror spatial analysis, accessibility, lighting, audio/acoustics, performance, decals, level design, tech art, context-aware props, procedural geometry (furniture, horror props, structures, mazes, terrain), genre presets, encounter design, quality/polish. +45 experimental town gen (disabled by default).
- **MonolithGAS (130 actions)** — Complete Gameplay Ability System: abilities, attributes, effects, ASC, tags, cues, targeting, input, inspect, scaffold. 53/53 tests PASS. Optional GBA support for Blueprint AttributeSets.
- **MonolithUI (42 actions)** — Widget Blueprint CRUD, templates (HUDs, menus, settings, inventory), styling, animation, game scaffolding, accessibility.
- **MonolithBABridge** — Blueprint Assist integration via IModularFeatures bridge. Enhanced auto_layout when BA is installed.
- **MCP auto-reconnect proxy** — stdio-to-HTTP proxy keeps Claude Code sessions alive across editor restarts.
- **Optional module system** — `Build.cs` probe pattern for third-party plugins (GeometryScripting, BlueprintAssist, GBA).
- **3 new skills** — `unreal-mesh`, `unreal-ui`, `unreal-gas`.

### Changed

- Total: 443 -> 815 actions across 13 modules, exposed through 15 MCP tools
- Skills: 9 -> 12 bundled with plugin

---

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

---

## [0.9.0] - 2026-03-19

+69 new actions (219 → 290 total), 60 bug fixes, 202 tests all pass.

### Added

**Blueprint (+20, 47 → 67)**

- `batch_execute` — dispatch multiple Blueprint operations in a single call
- `resolve_node` — resolve a node reference to its target (function, variable, etc.)
- `search_functions` — search functions and events by name across a Blueprint
- `get_node_details` — full detail dump for a single node (pins, defaults, metadata)
- `add_nodes_bulk` / `connect_pins_bulk` / `set_pin_defaults_bulk` — bulk graph operations in one call
- `scaffold_interface_implementation` — auto-generate stub event nodes for an unimplemented interface
- `add_timeline` / `add_event_node` / `add_comment_node` — new node types for `add_node`
- `get_function_signature` — return param list and return type for a Blueprint function
- `get_blueprint_info` — comprehensive Blueprint summary (class, interfaces, components, variable count)
- `get_event_dispatcher_details` / `remove_event_dispatcher` / `set_event_dispatcher_params` — event dispatcher management
- `validate_blueprint` (enhanced) — now detects unimplemented interfaces and duplicate events
- `promote_pin_to_variable` — promote a pin's value to a Blueprint variable
- `add_replicated_variable` — add a replicated variable with configurable RepNotify
- `add_node` (extended) — now supports cast node creation (`CastTo<ClassName>`)

**Material (+22, 25 → 47)**

- `auto_layout` — auto-arrange expression nodes in the material graph
- `duplicate_expression` / `replace_expression` — expression node management
- `list_expression_classes` — list all available material expression class names
- `get_expression_connections` / `get_expression_pin_info` — expression inspection
- `move_expression` / `rename_expression` — expression editing
- `get_material_properties` — return material-level properties (blend mode, shading model, etc.)
- `get_instance_parameters` / `set_instance_parameters` / `set_instance_parent` / `clear_instance_parameter` — material instance CRUD
- `list_material_instances` — find all material instances derived from a material
- `save_material` — explicitly save a material asset
- `update_custom_hlsl_node` — update the HLSL code on a CustomHLSL expression
- `create_material_function` / `build_function_graph` / `get_function_info` — material function authoring
- `batch_set_material_property` / `batch_recompile` — bulk material operations
- `import_texture` — import an image file as a UTexture2D asset

**Niagara (+17, 47 → 64)**

- `get_system_summary` / `get_emitter_summary` — high-level overview actions
- `list_emitter_properties` — list all editable UPROPERTY fields on an emitter asset
- `get_module_input_value` — read the current value of a single module input
- `configure_curve_keys` / `configure_data_interface` — data interface configuration
- `duplicate_system` / `create_emitter` — asset management
- `set_fixed_bounds` / `set_effect_type` — system-level configuration
- `export_system_spec` — export a system's full spec as JSON (reverse of `create_system_from_spec`)
- `add_dynamic_input` / `set_dynamic_input_value` / `search_dynamic_inputs` — dynamic input support
- `add_event_handler` — add an event handler stage to an emitter
- `validate_system` — validate for GPU/Light conflicts, missing materials, bounds warnings
- `add_simulation_stage` — add a simulation stage to a GPU emitter

**Animation (+12, 62 → 74)**

- `get_ikrig_info` / `add_ik_solver` — IKRig asset inspection and editing
- `get_retargeter_info` / `set_retarget_chain_mapping` — IK Retargeter support
- `get_control_rig_info` / `get_control_rig_variables` / `add_control_rig_element` — Control Rig support
- `get_abp_variables` / `get_abp_linked_assets` — Animation Blueprint inspection
- `add_state_to_machine` / `add_transition` / `set_transition_rule` — AnimBP structural writes

### Fixed

- **Blueprint** — 21 fixes: 5 crashes (null graph ref, invalid pin access, blueprint-not-compiled guard, interface scaffold on abstract classes, cast node creation), 7 logic bugs, 9 UX improvements
- **Material** — 11 fixes: `build_function_graph` node class resolution, `connect_expressions` direction detection, `get_material_parameters` missing static switch params, and others
- **Niagara** — 16 fixes: 2 crashes (`configure_data_interface` null DI, `add_event_handler` uninitialized receiver), 5 bugs, 9 UX improvements
- **Animation** — 12 fixes: 1 crash (`add_ik_solver` null pointer), 6 bugs, 5 UX improvements

### Changed

- Blueprint: 47 → 67 actions
- Material: 25 → 47 actions
- Niagara: 47 → 64 actions
- Animation: 62 → 74 actions
- Total: 220 → 290 actions

---

## [0.8.0] - 2026-03-15

Native C++ source indexer, marketplace content indexing, CDO properties, and project C++ source indexing. 3 community PRs from NRG-Nad. 220 actions total.

### Added

**Source — Native C++ indexer (no Python required)**
- Completely rewrote the engine source indexer in native C++ (4,119 lines). Engine source indexing now works out of the box — no Python install, no separate indexer script. Two indexing modes: full engine source on startup, incremental project-only via the new `trigger_project_reindex` action.
- New `MonolithQueryCommandlet` for offline source queries from the command line.
- New `trigger_project_reindex` action: triggers an incremental re-index of project C++ source from an MCP session. **(220 total actions)**

**Index — Marketplace plugin content**
- Auto-discovers installed marketplace and Fab plugins and indexes their content alongside project assets. Toggle with `bIndexMarketplacePlugins` in plugin settings.

**Index — Configurable content paths (#4)**
- `AdditionalContentPaths` setting for adding arbitrary extra content paths (e.g. external asset packs) to the project index. Credit: **NRG-Nad** ([#4](https://github.com/tumourlove/monolith/pull/4)).

**Blueprint — CDO property reader (#5)**
- New `get_cdo_properties` action: reads `UPROPERTY` defaults from any Blueprint CDO or `UObject` asset. Credit: **NRG-Nad** ([#5](https://github.com/tumourlove/monolith/pull/5)).
- New `FDataAssetIndexer`: deep-indexes DataAsset subclasses. 15 registered indexers total. `bIndexDataAssets` toggle in settings. Credit: **NRG-Nad** ([#5](https://github.com/tumourlove/monolith/pull/5)).

**Source — Project C++ source indexing (#6)**
- `Scripts/index_project.py`: indexes project C++ source into `EngineSource.db` alongside engine symbols. Incremental pipeline — only changed files are reprocessed. Source DB grows from ~1.8 GB to ~3.4 GB with a full project. Credit: **NRG-Nad** ([#6](https://github.com/tumourlove/monolith/pull/6)).

### Fixed

- **MonolithSource** — Improved error handling throughout the source indexer pipeline.
- **MonolithNiagara** — 5 bugs fixed: DI class auto-prefix, DI curve data in `get_module_inputs`, LinearColor/vector defaults, `disconnect_expression` targeted disconnection, `list_renderers` type short name.

### Changed

- MonolithBlueprint: 46 → 47 actions (`get_cdo_properties`)
- MonolithSource: 10 → 11 actions (`trigger_project_reindex`)
- Total: 219 → 220 actions
- Source indexing no longer requires Python for engine lookups. Python is only needed for optional project C++ source indexing.

---

## [0.7.3] - 2026-03-15

Blueprint module fully realized (6 → 46 actions). Niagara HLSL module creation implemented. Major Niagara, Material, and MCP reliability fixes. 218 actions total.

### Added

**Blueprint — 40 new write actions (6 → 46 total)**
- **Variables (7):** `add_variable`, `remove_variable`, `set_variable_default`, `set_variable_type`, `set_variable_flags`, `rename_variable`, `get_variable_details`
- **Components (6):** `add_component`, `remove_component`, `set_component_property`, `get_components`, `get_component_details`, `reparent_component`
- **Graph management (9):** `add_function_graph`, `remove_function_graph`, `add_macro_graph`, `remove_macro_graph`, `add_event_graph`, `remove_event_graph`, `get_functions`, `get_event_dispatchers`, `get_construction_script`
- **Nodes & pins (6):** `add_node`, `remove_node`, `connect_pins`, `disconnect_pins`, `get_pin_info`, `find_nodes_by_class`
- **Compile & create (5):** `compile_blueprint`, `create_blueprint`, `reparent_blueprint`, `add_interface`, `remove_interface`
- `add_node` now resolves common node class aliases (`CallFunction`, `VariableGet`, `VariableSet`, `Branch`, `Sequence`, `ForEach`) and tries `K2_` prefix automatically

**Niagara — HLSL module authoring (2 new)**
- **`create_module_from_hlsl`** — Creates a `UNiagaraScript` (module usage) with a CustomHlsl node and typed ParameterMap I/O pins. Inputs are exposed as overridable parameters — compatible with `get_module_inputs` and `set_module_input_value`. CPU and GPU sim targets supported.
- **`create_function_from_hlsl`** — Same as above in function usage context (reusable utility, direct typed pin wiring, no ParameterMap wrapper).
- Dot-containing pin names (e.g. `Module.Color`) rejected at creation time with clear guidance.

**Niagara — System controls & discovery (5 new)**
- **`set_system_property`** — Set system-level properties (e.g. `WarmupTime`, `bDeterminism`) via UE reflection. Any `UPROPERTY` on `UNiagaraSystem` is settable.
- **`set_static_switch_value`** — Set static switch inputs on module stack entries for compile-time code path control.
- **`list_module_scripts`** — Keyword search for available Niagara module script assets.
- **`list_renderer_properties`** — Lists all editable `UPROPERTY` fields on a renderer via reflection, with current values.
- **`get_system_diagnostics`** — Compile errors, warnings, renderer/SimTarget incompatibility flags, GPU/dynamic bounds warnings, per-script stats (op count, register count, compile status).

**MCP**
- `tools/list` now embeds per-action param schemas at session start — full documentation without calling `monolith_discover()` first.
- Registry-level required param validation: missing params return a clear error before the handler is called.

**Offline CLI**
- `Saved/monolith_offline.py` — pure Python (stdlib, zero deps) read-only CLI for querying the source and project DBs when the editor is not running. 14 actions across `source` and `project` namespaces.

### Fixed

**Niagara**
- `add_emitter` — emitters were not persisting in the saved asset. Fixed by switching to `FNiagaraEditorUtilities::AddEmitterToSystem()`.
- `create_system_from_spec` — failed with `failed_steps:1` on any spec with modules. Root cause: missing synchronous compile after each emitter add. Failed sub-operations now reported in `"errors"` array.
- `set_emitter_property` — SimTarget change caused "Data missing please force a recompile". Fixed with `PostEditChangeVersionedProperty` + `RebuildEmitterNodes` + `SynchronizeOverviewGraphWithSystem`.
- `set_module_input_value` / `set_module_input_binding` — were using stripped short names where the full `Module.`-prefixed name was required, causing namespace warnings on every Niagara compile.
- `get_module_inputs` — correctly deserializes `LinearColor` and vector defaults; returns real `FRichCurve` key data for curve DI inputs; works with CustomHlsl modules.
- `list_emitters` — now includes emitter GUID in output.
- `list_renderers` — now returns short renderer class name (e.g. `SpriteRenderer`) instead of full UClass path.

**Material**
- `set_expression_property` — now calls `PostEditChangeProperty` with the actual property; changes reflect without a manual recompile.
- `build_material_graph` — now auto-recompiles on success.
- `delete_expression`, `connect_expressions`, `disconnect_expression` — wrapped in `PreEditChange`/`PostEditChange` for correct undo history and editor updates.
- `disconnect_expression` — now accepts optional `input_name`/`output_name` for targeted disconnection.

**Blueprint**
- `add_node` — now resolves node class aliases and `K2_` prefix. Previously failed with class-not-found on all common node types.

**Core**
- `LoadAssetByPath` — queries Asset Registry first to prevent stale `RF_Standalone` ghost objects from shadowing recreated assets.

### Changed

- Blueprint action count 6 → 46
- Niagara action count 41 → 47
- Total action count 177 → 218

---

## [0.7.1] - 2026-03-11

Niagara write testing: all 41 actions verified. 12 bugs found and fixed (4 crashes, 8 logic bugs), plus a major improvement to `get_module_inputs`.

### Fixed

- **CRASH:** `create_system_from_spec` — `GetAssetPath` infinite recursion (stack overflow)
- **CRASH:** `create_system` — raw `NewObject` without `InitializeSystem()` caused array OOB on `AddEmitterHandle`
- **CRASH:** `add_emitter` — emitter with no versions caused array OOB
- **CRASH:** `set_module_input_di` — assertion failure when pin already had links
- `set_module_input_di` — accepted nonexistent input names, non-DI types, and parsed config as string instead of JSON object
- `get_module_inputs` — only returned static switches. Now returns ALL input types via engine's `GetStackFunctionInputs` API
- `GetStackFunctionInputOverridePin` — now walks upstream to ParameterMapSet node for data input overrides
- `get_module_inputs` — stripped `Module.` prefix for consistency with write actions
- `batch_execute` — added 8 missing write ops to dispatch table
- `FindEmitterHandleIndex` — no longer auto-selects when a non-matching name is passed
- `set_module_input_value` / `set_curve_value` — added `BreakAllPinLinks()` guard for overriding bindings

---

## [0.7.0] - 2026-03-10

Animation Wave 2: 44 new actions across animation and PoseSearch, bringing the module from 23 to 67 actions and the plugin total to 177.

### Added

- **Curve Operations (7):** `get_curves`, `add_curve`, `remove_curve`, `set_curve_keys`, `get_curve_keys`, `rename_curve`, `get_curve_data`
- **Bone Track Inspection (3):** `get_bone_tracks`, `get_bone_track_data`, `get_animation_statistics`
- **Sync Markers (3):** `get_sync_markers`, `add_sync_marker`, `remove_sync_marker`
- **Root Motion (2):** `get_root_motion_info`, `extract_root_motion`
- **Compression (2):** `get_compression_settings`, `apply_compression`
- **BlendSpace Operations (5):** `get_blendspace_info`, `add_blendspace_sample`, `remove_blendspace_sample`, `set_blendspace_axis`, `get_blendspace_samples`
- **AnimBP Inspection (5):** `get_anim_blueprint_info`, `get_state_machines`, `get_state_info`, `get_transitions`, `get_anim_graph_nodes`
- **Montage Operations (5):** `get_montage_info`, `add_montage_section`, `delete_montage_section`, `set_montage_section_link`, `get_montage_slots`
- **Skeleton Operations (5):** `get_skeleton_info`, `add_virtual_bone`, `remove_virtual_bones`, `get_socket_info`, `add_socket`
- **Batch & Modifiers (2):** `batch_get_animation_info`, `run_animation_modifier`
- **PoseSearch (5):** `get_pose_search_schema`, `get_pose_search_database`, `add_database_sequence`, `remove_database_sequence`, `get_database_stats`

### Fixed

- `add_virtual_bone` — crash on non-existent source bone (array OOB)
- `set_notify_time/duration` — now works on AnimMontage (was AnimSequence-only)
- `delete_montage_section` — guard against deleting last remaining section
- `add_blendspace_sample` — descriptive error on skeleton mismatch
- `remove_virtual_bones` — no longer reports false success for missing bones

---

## [0.6.1] - 2026-03-10

MCP tool discovery fix — tools now register natively in Claude Code's ToolSearch.

### Fixed

- Tool names changed from dot notation to underscore (`material_query` not `material.query`)
- Protocol version negotiation echoes client's requested version

---

## [0.6.0] - 2026-03-10

Material Wave 2: 11 new write actions for full material CRUD coverage.

### Added

- `create_material`, `create_material_instance`, `set_material_property`, `delete_expression`
- `get_material_parameters`, `set_instance_parameter`, `recompile_material`, `duplicate_material`
- `get_compilation_stats`, `set_expression_property`, `connect_expressions`
- Material actions 14 → 25, total 122 → 133

### Fixed

- Auto-updater hot-swap no longer deletes Saved/ directory
- `build_material_graph` FindObject → FindFirstObject
- `disconnect_expression` missing material output pins

---

## [0.5.2] - 2026-03-10

36/36 read actions verified PASS. Per-action param schemas, new actions, major fixes.

### Added

- **Blueprint `get_graph_summary`** — lightweight graph overview (~10KB vs 172KB). Blueprint 5→6 actions
- **Niagara `list_emitters`** — emitter name, index, enabled, sim_target, renderer_count
- **Niagara `list_renderers`** — renderer class, index, enabled, material. Niagara 39→41 actions
- Per-action param schemas in `monolith_discover()` output — all actions are now self-documenting
- Blueprint `get_graph_data` accepts optional `node_class_filter` param
- Blueprint `get_execution_flow` two-pass entry node search (events before comments)
- Material `export_material_graph` accepts `include_properties` and `include_positions` params
- Material `get_thumbnail` accepts `save_to_file` param
- Niagara `get_all_parameters` accepts optional `emitter` and `scope` filters
- Animation `get_nodes` accepts optional `graph_name` filter
- Animation `get_transitions` includes `from_type`/`to_type` fields (state vs conduit)

### Fixed

- **Project Indexer** — auto-index deferred to `OnFilesLoaded()` (was only indexing 193/9560 assets)
- **Material `validate_material`** — added 7 missing material properties + MaterialAttributeLayers BFS seed (0 false positive islands, was 43)
- **Blueprint `get_variables`** — CDO default values now populated (was always empty)
- **Niagara `get_module_inputs`** — real types via PinToTypeDefinition (no more Vector4f)
- **Niagara `get_renderer_bindings`** — clean JSON output instead of raw UE struct dumps
- **Niagara `get_ordered_modules`** — usage filter with shorthands, error on invalid values
- **Niagara `trace_parameter_binding`** — User. prefix OR fallback
- **Niagara `get_di_functions`** — reversed class name pattern
- **Niagara `batch_execute`** — 3 op name mismatches fixed, old names kept as aliases
- **Animation `get_transitions`** — conduit nodes resolved via UAnimStateNodeBase cast
- **Animation state machines** — `\n` stripped from names, exact matching instead of fuzzy
- **Animation `get_state_info`** — validates required params
- All Niagara actions accept `asset_path` (with `system_path` backward compat)
- Niagara `User.` prefix stripped transparently in 4 param actions
- Niagara `get_compiled_gpu_hlsl` auto-compiles system if HLSL not cached
- Indexer `bIsIndexing` reset in Deinitialize, sanity check <500 assets skips last_full_index
- Index DB changed from WAL to DELETE journal mode

---

## [0.5.0] - 2026-03-08

Auto-updater rewrite — fixes all swap script failures on Windows.

### Fixed

- Swap script now polls `tasklist` for `UnrealEditor.exe` instead of a cosmetic 10-second countdown
- `errorlevel` check after retry rename was unreachable due to cmd.exe resetting `%ERRORLEVEL%`
- Launcher script now uses outer-double-quote trick for `cmd /c` paths with spaces
- Switched from `ren` to `move` for full path support
- Retry now cleans stale backup before re-attempting rename
- Rollback on failed xcopy now removes partial destination before restoring backup
- Added `/h` flag to primary xcopy to include hidden-attribute files
- Enabled `DelayedExpansion` for correct variable expansion inside `if` blocks

### Added

- `Scripts/make_release.ps1` — release zip builder that sets `"Installed": true` for Blueprint-only compatibility
- Conditional post-update message for C++ vs Blueprint-only users

---

## [0.2.0] - 2026-03-08

Source indexer overhaul and auto-updater improvements.

### Fixed

- UE macros (UCLASS, ENGINE_API, GENERATED_BODY) now stripped before tree-sitter parsing
- Class definitions increased from ~0 to 62,059; inheritance links from ~0 to 37,010
- `read_source members_only` now returns class members correctly
- `get_class_hierarchy` ancestor traversal now works
- `get_class_hierarchy` accepts both `symbol` and `class_name` params

### Added

- UE macro preprocessor with balanced-paren stripping
- `--clean` flag for source indexer
- Release notes in update notification and Output Log

---

## [0.1.0] - 2026-03-07

Initial beta release. One plugin, 9 domains, 119 actions.

### Added

- Embedded Streamable HTTP MCP server with JSON-RPC 2.0
- Namespace dispatch pattern (~14 tools instead of ~119)
- 9 domain modules: Core, Blueprint, Material, Animation, Niagara, Editor, Config, Index, Source
- SQLite FTS5 project indexer with 14 asset indexers
- Python tree-sitter engine source indexer
- Auto-updater via GitHub Releases
- 9 Claude Code skills
- Plugin settings via UDeveloperSettings

### Fixed (post-release)

- HTTP body null-termination, Niagara graph traversal crash, emitter lookup failures
- Source DB WAL lock contention, SQL schema creation, asset loading crash
- 8 additional bug fixes across animation, editor, config, and source modules
- Session tracking removed (fully stateless), first-call failures fixed
