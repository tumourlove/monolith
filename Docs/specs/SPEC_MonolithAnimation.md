# Monolith — MonolithAnimation Module

**Parent:** [SPEC_CORE.md](../SPEC_CORE.md)
**Engine:** Unreal Engine 5.7+
**Version:** 0.14.10 (Beta)

---

## MonolithAnimation

**Dependencies:** Core, CoreUObject, Engine, MonolithCore, UnrealEd, AnimGraph, AnimGraphRuntime, BlueprintGraph, AnimationBlueprintLibrary, PoseSearch, AnimationModifiers, EditorScriptingUtilities, Json, JsonUtilities

### Classes

| Class | Responsibility |
|-------|---------------|
| `FMonolithAnimationModule` | Registers 125 animation actions across `MonolithAnimationActions.cpp` (103), `MonolithPoseSearchActions.cpp` (13), `MonolithAbpWriteActions.cpp` (5), `MonolithControlRigWriteActions.cpp` (3), `MonolithAnimLayoutActions.cpp` (1) |
| `FMonolithAnimationActions` | Static handlers organized in 15 groups (the original action handlers) |
| `FMonolithAbpWriteActions` | ABP graph write actions (Phase v0.14.3 PR #34): `add_anim_graph_node` (built-in aliases plus generic `UAnimGraphNode_Base` class path/name resolution, with TwoBoneIK / ModifyBone helpers and auto-pin exposure), `connect_anim_graph_pins`, `set_state_animation`, `add_variable_get`, `set_anim_graph_node_property` |
| `FMonolithControlRigWriteActions` | ControlRig write actions: 3 actions (graph node creation, pin configuration, variable management) |
| `FMonolithAnimLayoutActions` | `auto_layout` for AnimBP graphs |

### Actions (125 — namespace: "animation")

**Note (2026-04-26 audit):** The detailed per-category tables below cover the 103 baseline actions. The remaining **22 actions** (5 ABP write + 13 PoseSearch + 3 ControlRig + 1 layout) are documented in their own sections at the bottom of this spec. The ABP write actions landed in v0.14.3 (PR #34 by @MaxenceEpitech). No Phase J changes touched this module. v0.14.9 added `copy_bone_pose_between_sequences` (PR #51 by @MaxenceEpitech). v0.14.10 added `list_bone_tracks` (PR #54 by @MaxenceEpitech) and rewrote `get_bone_track_keys` to use the non-deprecated `IsValidBoneTrackName` + `GetBoneTrackTransforms` API path. v0.14.10 also added `get_skeleton_preview_attached_assets` + `get_bone_ref_pose` (PR #55 by @MaxenceEpitech) and the three `CompatibleSkeletons` actions (`get_compatible_skeletons` / `add_compatible_skeleton` / `remove_compatible_skeleton` — PR #56 by @MaxenceEpitech), bringing the module total to 125 (103 baseline + 22 wave-actions).

**Sequence Info (5) — read-only**
| Action | Description |
|--------|-------------|
| `get_sequence_info` | Get sequence metadata (duration, frames, root motion, compression, etc.) |
| `get_sequence_notifies` | Get all notifies on an animation asset (sequence, montage, composite) |
| `get_bone_track_keys` | Get position/rotation/scale keys for a bone track (with optional frame range) (rewritten v0.14.10 to use non-deprecated `IsValidBoneTrackName` + `GetBoneTrackTransforms`; emits scales unconditionally — see CHANGELOG behaviour note) |
| `get_sequence_curves` | Get float and transform curves on an animation sequence |
| `list_bone_tracks` | List all bone tracks present on an animation sequence (returns `count` + `bone_names: [..]`). Discovery action for `get_bone_track_keys`. (PR #54, v0.14.10) |

**Bone Track Editing (4)**
| Action | Description |
|--------|-------------|
| `set_bone_track_keys` | Set position/rotation/scale keys (JSON arrays) |
| `add_bone_track` | Add a bone track to an animation sequence |
| `remove_bone_track` | Remove a bone track (with optional `include_children`) |
| `copy_bone_pose_between_sequences` | Read evaluated pose from source `UAnimSequence` at a time and write as keys to a destination sequence for a list of bones. Per-bone skip with structured `reason`. (PR #51 v0.14.9 by @MaxenceEpitech) |

**Notify Operations (6)**
| Action | Description |
|--------|-------------|
| `add_notify` | Add a point notify to an animation asset |
| `add_notify_state` | Add a state notify (with duration) to an animation asset |
| `remove_notify` | Remove a notify by index |
| `set_notify_time` | Set trigger time of an animation notify |
| `set_notify_duration` | Set duration of a state animation notify |
| `set_notify_track` | Move a notify to a different track |

**Curve Operations (5)**
| Action | Description |
|--------|-------------|
| `list_curves` | List all animation curves on a sequence (optional `include_keys`) |
| `add_curve` | Add a float or transform curve to an animation sequence |
| `remove_curve` | Remove a curve from an animation sequence |
| `set_curve_keys` | Set keys on a float curve (replaces existing keys) |
| `get_curve_keys` | Get all keys from a float curve |

**BlendSpace Operations (5)**
| Action | Description |
|--------|-------------|
| `get_blend_space_info` | Get blend space samples and axis settings |
| `add_blendspace_sample` | Add a sample to a blend space |
| `edit_blendspace_sample` | Edit sample position and optionally its animation |
| `delete_blendspace_sample` | Delete a sample by index |
| `set_blend_space_axis` | Configure axis (name, range, grid divisions, snap, wrap) |

**ABP Graph Reading (8) — read-only**
| Action | Description |
|--------|-------------|
| `get_abp_info` | Get ABP overview (skeleton, graphs, state machines, variables, interfaces) |
| `get_state_machines` | Get all state machines with full topology |
| `get_state_info` | Detailed info about a state in a state machine |
| `get_transitions` | All transitions (supports empty machine_name for ALL state machines) |
| `get_blend_nodes` | Blend nodes in an ABP graph |
| `get_linked_layers` | Linked animation layers |
| `get_graphs` | All graphs in an ABP |
| `get_nodes` | Animation nodes with optional class and graph_name filters |

**Montage Operations (8)**
| Action | Description |
|--------|-------------|
| `get_montage_info` | Get montage sections, slots, blend settings |
| `add_montage_section` | Add a section to an animation montage |
| `delete_montage_section` | Delete a section by index |
| `set_section_next` | Set the next section for a montage section |
| `set_section_time` | Set start time of a montage section |
| `set_montage_blend` | Set blend in/out times and auto blend out |
| `add_montage_slot` | Add a slot track to a montage |
| `set_montage_slot` | Rename a slot track by index |

**Skeleton Operations (11)**
| Action | Description |
|--------|-------------|
| `get_skeleton_info` | Skeleton bone hierarchy, virtual bones, and sockets |
| `get_skeletal_mesh_info` | Mesh info: morph targets, sockets, LODs, materials |
| `get_skeleton_sockets` | Get sockets from a skeleton or skeletal mesh |
| `get_skeleton_curves` | Get all registered animation curve names from a skeleton |
| `get_skeleton_preview_attached_assets` | Read `USkeleton::PreviewAttachedAssetContainer` (the `[Preview Only]` list shown in Persona's bone tree). Returns `{ asset_path, attached_objects: [{ attach_point, attached_object, attached_object_class }, ...], count, transforms_stored: false }`. The `transforms_stored: false` flag documents that the container does NOT carry per-asset relative transforms (Persona attaches at the socket origin with the asset's natural pivot). (PR #55, v0.14.10) |
| `get_bone_ref_pose` | Reference (bind) pose transforms for skeleton bones in BOTH parent-relative AND component-space. Walks `FReferenceSkeleton` once for component-space accumulation. Accepts a `bone_names: array` filter (default: all bones). Works on either a `USkeleton` or `USkeletalMesh` asset path — `source_type` field in response indicates which. (PR #55, v0.14.10) |
| `add_virtual_bone` | Add a virtual bone to a skeleton |
| `remove_virtual_bones` | Remove virtual bones (specific names) |
| `add_socket` | Add a socket to a skeleton |
| `remove_socket` | Remove a socket from a skeleton |
| `set_socket_transform` | Set the transform of a skeleton socket |

**Skeleton Compatibility (3)**
Wraps `USkeleton::CompatibleSkeletons` — the canonical UE5 mechanism that lets anims authored on one skeleton play on another (typical case: UE4 mannequin animation packs on UE5 `SK_Mannequin` meshes). Closes the prior `editor_query.run_python` workaround for cross-skeleton retarget setup. All three actions accept `save: bool = true` controlling whether `UEditorAssetLibrary::SaveAsset` runs after the mutation. (PR #56 by @MaxenceEpitech, v0.14.10.)

| Action | Description |
|--------|-------------|
| `get_compatible_skeletons` | Returns `{ asset_path, compatible_skeletons: [..], count }`. Lists the soft-pointer paths currently registered in `USkeleton::CompatibleSkeletons`. |
| `add_compatible_skeleton` | Marks a target skeleton compatible with the source. Idempotent: returns disjoint `added` / `already_compatible` booleans + resulting `count`. Self-compat rejected with `"Cannot mark a skeleton compatible with itself"`. |
| `remove_compatible_skeleton` | Removes a previously-registered compatible skeleton. Idempotent: returns disjoint `removed` / `was_compatible` booleans + resulting `count`. |

**Root Motion (1)**
| Action | Description |
|--------|-------------|
| `set_root_motion_settings` | Configure root motion settings (enable, lock mode, force root lock) |

**Asset Creation (3)**
| Action | Description |
|--------|-------------|
| `create_sequence` | Create a new empty animation sequence |
| `duplicate_sequence` | Duplicate an animation sequence to a new path |
| `create_montage` | Create a new animation montage with skeleton |

**Anim Modifiers (2)**
| Action | Description |
|--------|-------------|
| `apply_anim_modifier` | Apply an animation modifier class to a sequence |
| `list_anim_modifiers` | List animation modifiers applied to a sequence |

**Composites (3)**
| Action | Description |
|--------|-------------|
| `get_composite_info` | Get segments and metadata from an animation composite |
| `add_composite_segment` | Add a segment to an animation composite |
| `remove_composite_segment` | Remove a segment from an animation composite by index |

**PoseSearch (13)**
| Action | Description |
|--------|-------------|
| `get_pose_search_schema` | Get PoseSearch schema config and channels |
| `get_pose_search_database` | Get PoseSearch database sequences and schema reference |
| `add_database_sequence` | Add an animation sequence to a PoseSearch database |
| `remove_database_sequence` | Remove a sequence from a PoseSearch database by index |
| `get_database_stats` | Get PoseSearch database statistics (pose count, search mode, costs) |
| `create_pose_search_schema` | Create a new PoseSearch schema asset |
| `create_pose_search_database` | Create a new PoseSearch database asset |
| `set_database_sequence_properties` | Set per-sequence properties (looping, mirror option, sample range) |
| `add_schema_channel` | Add a channel to a PoseSearch schema |
| `remove_schema_channel` | Remove a channel from a PoseSearch schema |
| (3 additional PoseSearch actions registered — see `MonolithPoseSearchActions.cpp` for the full list; this section is approximate while the per-action audit catches up) |

**ABP Write (5) — v0.14.3 PR #34 by @MaxenceEpitech**
| Action | Description |
|--------|-------------|
| `add_anim_graph_node` | Place an animation graph node. `node_type` still accepts the existing aliases (`SequencePlayer`, `BlendSpacePlayer`, `TwoWayBlend`, `BlendListByBool`, `LayeredBoneBlend`, `MotionMatching`, `TwoBoneIK`, `ModifyBone`, `LocalToComponentSpace`, `ComponentToLocalSpace`) and may also be a class path/name for legacy clients. New `node_class` accepts any loaded non-abstract `UAnimGraphNode_Base` subclass by class path or name. Rejects missing, ambiguous, non-`UAnimGraphNode_Base`, abstract, and unresolved classes with actionable errors. TwoBoneIK auto-exposes `EffectorLocation`, `JointTargetLocation`, `Alpha` as input pins; `expose_pins` manually controls optional pins on any node type |
| `connect_anim_graph_pins` | Wire two pins inside an ABP graph |
| `set_state_animation` | Assign an animation asset to a state machine state |
| `add_variable_get` | Place a `K2Node_VariableGet` in an ABP anim graph for reading AnimInstance member variables. Validates the variable exists on the skeleton class before spawning |
| `set_anim_graph_node_property` | Set a property on a previously-placed anim graph node via reflection |

**ControlRig Write (3)**
| Action | Description |
|--------|-------------|
| (3 actions in `MonolithControlRigWriteActions.cpp` — graph node creation, pin configuration, variable management) |

**Layout (1)**
| Action | Description |
|--------|-------------|
| `auto_layout` | Auto-arrange nodes in an Animation Blueprint graph. `formatter`: `"auto"` (default) — uses Blueprint Assist if available, falls back to built-in hierarchical layout; `"blueprint_assist"` — requires BA; `"builtin"` — built-in only. Optional `graph_name` to target a specific graph |

### Bulk Fill & Describe Surface (2026-05-11)

`MonolithAnimationBulkFillAdapter` registers under `FMonolithBulkFillRegistry` for the `animation` namespace, exposed via the framework-level `bulk_fill_query("apply", ...)` and `describe_query("schema", ...)` dispatchers. Phase 5 of the MCP ergonomics rollout (design spec `Docs/plans/2026-05-11-monolith-mcp-ergonomics-design.md`).

**Surface summary.** `bulk_fill_query("apply", target_namespace="animation", target="<asset_path>", tree={...})` covers PoseSearch database bulk-populate (the 60-300+ entry pain) and a v1 audit-only notify-apply-template scan. `describe_query("schema", target_namespace="animation", target="<asset_path>")` returns the PoseSearch entry schema and the notify/curve track schema for the target sequence.

**fill_kind catalogue (2 — enumerated against `MonolithAnimationBulkFillAdapter.cpp`):**

| `fill_kind` | Target shape | Walks |
|---|---|---|
| `PoseSearchDatabase` | `UPoseSearchDatabase` | `entries:[]` walked as `FPoseSearchDatabaseAnimationAsset` rows. Discriminated 5.7-unified shape: `sequence` / `blendspace` / `composite` / `montage` per entry, plus `looping` / `mirror_option` / `sample_range` |
| `NotifyApplyTemplate` | Folder + name_glob | **v1 audit-only.** Scans the folder via name glob (e.g. `name_glob: "A_Walk_*"`) and surfaces which sequences a template would apply to. Commit still through existing per-asset notify CRUD actions |

**Sample tree (PoseSearchDatabase, design spec Appendix B.3):**

```json
{
  "target": "/Game/AnimGraph/PSD_Locomotion",
  "tree": {
    "fill_kind": "PoseSearchDatabase",
    "entries": [
      {"animation": "/Game/Anim/A_Idle",     "looping": true,  "mirror_option": "UnmirroredOnly"},
      {"animation": "/Game/Anim/A_Walk_F",   "looping": true,  "sample_range": {"min": 0.0, "max": 1.5}},
      {"animation": "/Game/Anim/A_Run_F",    "looping": true},
      {"blendspace": "/Game/Anim/BS_Strafe", "mirror_option": "UnmirroredAndMirrored"}
    ]
  },
  "dry_run": true
}
```

**Adapter-specific quirks.**

- **`FPoseSearchDatabaseAnimationAsset` is a unified 5.7 shape.** UE 5.7 collapsed prior per-asset-type containers into a single discriminated struct. The adapter routes per-row writes via the discriminator (the first of `sequence` / `blendspace` / `composite` / `montage` present in each row). Schema surfaces the discriminator under `entries[].asset_type` with the four valid values.
- **`IAnimationDataController` requires bracket transactions.** Sequence-level writes (notify CRUD, curve CRUD, bone-track CRUD) must open / close an `IAnimationDataController` transaction. The PoseSearchDatabase fill_kind does NOT touch sequence-level transactions (it writes to the database asset directly); notify/curve fill_kinds would, hence they remain `(WISHLIST v1.1)`.
- **CHT_ chooser-table has NO MCP surface — `(WISHLIST)`.** Chooser tables are not in the animation_query action surface and not in bulk_fill v1. Per the first-fanout Appendix A row "Chooser table (CHT_) population not in animation_query".
- **`v1 NotifyApplyTemplate fill_kind is audit-only.** Cited from the design spec. The handler scans the folder + glob and returns matched sequences with their existing notify / curve state, plus the template that would be applied. No writes commit. Real per-asset notify writes still flow through the existing `add_notify` / `add_curve` / `set_notify_time` actions.
- **Skeleton compatibility surface in v0.14.10.** Schema surfaces `CompatibleSkeletons` via the existing `get_compatible_skeletons` action — bulk_fill of compatible-skeleton lists is a separate `(WISHLIST v1.1)` fill_kind.

**Limitations / v1.1 follow-ups.**

- Notify/curve glob-template real-write fill_kind — `(v1.1)` — blocked on per-asset `IAnimationDataController` transaction surface enumeration.
- `set_blend_space_axis` / `set_section_next` / `add_montage_section` dry-run integration — `(WISHLIST v1.1)` — dry_run integration on existing actions.
- CHT_ chooser-table population fill_kind — `(WISHLIST)` — entire chooser-table action surface absent from animation_query.
- CSV ingest of PoseSearch entries via folder + naming convention — `(WISHLIST v1.1)` per Q2.

---
