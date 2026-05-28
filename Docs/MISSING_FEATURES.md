# Monolith — Missing Features / MCP Action Gaps

**Purpose:** Running inbox of empirically-discovered Monolith MCP capability gaps — actions that are missing, broken, or that forced a manual in-editor workaround during real project work. Each entry proposes the action to add. Distinct from `ROADMAP.md` (milestones / ship-status); this is the raw gap log, fed from the field.

**Internal planning doc.** Lives in `Docs/` — if it should NOT ship in the public release zip, add it to the `make_release.ps1` strip list alongside `ROADMAP.md`.

---

## 2026-05-23 — AnimBP / Blueprint (Leviathan PFP first-person framework work)

Surfaced finishing the PFP weapon framework in `ABP_SandboxCharacter_CMC_LayeringWrapper`. Each gap blocked an action or forced a manual Details-panel workaround. Source refs are UE 5.7 engine paths.

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
