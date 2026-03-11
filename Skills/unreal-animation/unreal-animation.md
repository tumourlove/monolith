---
name: unreal-animation
description: Use when inspecting or editing Unreal animation assets via Monolith MCP — sequences, montages, blend spaces, animation blueprints, notifies, curves, sync markers, skeletons. Triggers on animation, montage, ABP, blend space, notify, anim sequence, skeleton.
---

# Unreal Animation Workflows

You have access to **Monolith** with 23 animation actions via `animation_query()`.

## Discovery

```
monolith_discover({ namespace: "animation" })
```

## Asset Path Conventions

All asset paths follow UE content browser format (no .uasset extension):

| Location | Path Format | Example |
|----------|------------|---------|
| Project Content/ | `/Game/Path/To/Asset` | `/Game/Animations/ABP_Player` |
| Project Plugins/ | `/PluginName/Path/To/Asset` | `/CarnageFX/Animations/AM_Hit` |
| Engine Plugins | `/PluginName/Path/To/Asset` | `/Niagara/DefaultAssets/SystemAssets/NS_Default` |

## Key Parameter Names

- `asset_path` — the animation asset path
- `machine_name` — state machine name (returned by `get_state_machines`)
- `state_name` — state name within a machine
- `graph_name` — graph name (optional filter for `get_nodes`)

## Action Categories

### Montage Editing
| Action | Key Params | Purpose |
|--------|-----------|---------|
| `add_montage_section` | `asset_path`, `name`, `time` | Add a named section to a montage |
| `delete_montage_section` | `asset_path`, `name` | Remove a section |
| `set_section_next` | `asset_path`, `section`, `next` | Set section playback order |
| `set_section_time` | `asset_path`, `section`, `time` | Move a section to a specific time |

### Blend Space Samples
| Action | Key Params | Purpose |
|--------|-----------|---------|
| `add_blendspace_sample` | `asset_path`, `animation`, `x`, `y` | Add an animation at X/Y coordinates |
| `edit_blendspace_sample` | `asset_path`, `index`, `x`, `y` | Move an existing sample |
| `delete_blendspace_sample` | `asset_path`, `index` | Remove a sample point |

### Animation Blueprint (ABP) Reading
| Action | Key Params | Purpose |
|--------|-----------|---------|
| `get_state_machines` | `asset_path` | List all state machines in an ABP |
| `get_state_info` | `asset_path`, `machine_name`, `state_name` | Details of a specific state |
| `get_transitions` | `asset_path`, `machine_name` | Transition rules between states |
| `get_blend_nodes` | `asset_path` | Blend node trees |
| `get_linked_layers` | `asset_path` | Linked anim layers |
| `get_graphs` | `asset_path` | All graphs in the ABP |
| `get_nodes` | `asset_path`, `graph_name` (optional) | Nodes within a specific graph (or all graphs) |

### Notify Editing
| Action | Key Params | Purpose |
|--------|-----------|---------|
| `set_notify_time` | `asset_path`, `notify`, `time` | Move a notify to a specific time |
| `set_notify_duration` | `asset_path`, `notify`, `duration` | Set duration of a notify state |

### Bone Tracks
| Action | Key Params | Purpose |
|--------|-----------|---------|
| `set_bone_track_keys` | `asset_path`, `bone`, `keys` | Set keyframes for a bone track |
| `add_bone_track` | `asset_path`, `bone` | Add a new bone track |
| `remove_bone_track` | `asset_path`, `bone` | Remove a bone track |

### Skeleton
| Action | Key Params | Purpose |
|--------|-----------|---------|
| `add_virtual_bone` | `asset_path`, `source`, `target` | Create a virtual bone between two bones |
| `remove_virtual_bones` | `asset_path`, `bones` | Remove virtual bones |
| `get_skeleton_info` | `asset_path` | Bone hierarchy, sockets, virtual bones |
| `get_skeletal_mesh_info` | `asset_path` | Mesh details, LODs, materials |

## Common Workflows

### Inspect an ABP's state machines
```
animation_query({ action: "get_state_machines", params: { asset_path: "/Game/Animations/ABP_Player" } })
animation_query({ action: "get_transitions", params: { asset_path: "/Game/Animations/ABP_Player", machine_name: "Locomotion" } })
```

### Get nodes with optional graph filter
```
animation_query({ action: "get_nodes", params: { asset_path: "/Game/Animations/ABP_Player" } })
animation_query({ action: "get_nodes", params: { asset_path: "/Game/Animations/ABP_Player", graph_name: "EventGraph" } })
```

### Set up montage section flow (intro -> loop -> outro)
```
animation_query({ action: "add_montage_section", params: { asset_path: "/Game/Animations/AM_Attack", name: "Intro", time: 0.0 } })
animation_query({ action: "add_montage_section", params: { asset_path: "/Game/Animations/AM_Attack", name: "Loop", time: 0.5 } })
animation_query({ action: "add_montage_section", params: { asset_path: "/Game/Animations/AM_Attack", name: "Outro", time: 1.2 } })
animation_query({ action: "set_section_next", params: { asset_path: "/Game/Animations/AM_Attack", section: "Intro", next: "Loop" } })
animation_query({ action: "set_section_next", params: { asset_path: "/Game/Animations/AM_Attack", section: "Loop", next: "Outro" } })
```

### Inspect skeleton structure
```
animation_query({ action: "get_skeleton_info", params: { asset_path: "/Game/Characters/SK_Mannequin" } })
animation_query({ action: "get_skeletal_mesh_info", params: { asset_path: "/Game/Characters/SKM_Mannequin" } })
```

## Rules

- Editing tools modify assets **live in the editor** — changes are immediate
- The primary asset param is `asset_path` (not `asset`)
- State machine names are returned clean (no newline artifacts)
- `get_nodes` accepts an optional `graph_name` filter to scope results
- Use `project_query("search", { query: "AM_*" })` to find animation assets first
- ABP reading is read-only — state machine logic must be edited in the BP editor
