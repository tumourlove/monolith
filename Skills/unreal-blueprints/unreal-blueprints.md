---
name: unreal-blueprints
description: Use when working with Unreal Engine Blueprints via Monolith MCP — reading, creating, modifying, compiling Blueprints. Covers variables, components, functions, nodes, pins, interfaces, graph management, DataTables, structs, enums, templates, layout, timelines, level blueprints, CDO properties, and graph export/import. Triggers on Blueprint, BP, event graph, node, variable, function graph, component, compile, interface, DataTable, struct, enum, template, layout, timeline, level blueprint, CDO.
---

# Unreal Blueprint Workflows

**89 Blueprint actions** via `blueprint_query()`. Discover first: `monolith_discover({ namespace: "blueprint" })`

Also works on: Level Blueprints (map path or `$current`), Widget Blueprints.

## Key Parameters

- `asset_path` -- Blueprint path (NOT `asset`). Level BPs: map path or `"$current"`
- `graph_name` -- from `list_graphs`
- `node_id` -- from `get_graph_data` or `add_node` response
- `component_name` -- from `get_components`

## Action Reference

### Read (19)

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `list_graphs` | `asset_path` | All event/function/macro graphs |
| `get_graph_summary` | `asset_path`, `graph_name` | Node counts by type |
| `get_graph_data` | `asset_path`, `graph_name`, `node_class_filter`? | Full topology: pins, connections, positions |
| `get_variables` | `asset_path` | Variables with types, defaults, replication |
| `get_execution_flow` | `asset_path`, `graph_name`, `entry_point` | Trace execution wires |
| `search_nodes` | `asset_path`, `query` | Find by class/name/comment |
| `get_components` | `asset_path` | Component hierarchy |
| `get_component_details` | `asset_path`, `component_name` | Full property dump |
| `get_functions` | `asset_path` | Functions with I/O, metadata |
| `get_event_dispatchers` | `asset_path` | Dispatchers with pins |
| `get_parent_class` | `asset_path` | Parent, type, status |
| `get_interfaces` | `asset_path` | Implemented interfaces |
| `get_construction_script` | `asset_path` | Construction script data |
| `get_node_details` | `asset_path`, `node_id`, `graph_name`? | Full pin dump |
| `search_functions` | `query`, `class_filter`?, `pure_only`?, `limit`? | Find BP-callable functions |
| `get_interface_functions` | `interface_class` | Interface required functions |
| `get_function_signature` | `asset_path`, `function_name` | Inputs, outputs, flags |
| `get_blueprint_info` | `asset_path` | Comprehensive overview |
| `get_event_dispatcher_details` | `asset_path`, `dispatcher_name` | Signature + referencing nodes |

### CDO (2)

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `get_cdo_properties` | `asset_path`, `category_filter`? | Read UPROPERTY defaults |
| `set_cdo_property` | `asset_path`, `property_name`, `value` | Write CDO property (ImportText) |

### Discovery (1)

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `resolve_node` | `node_type`, `function_name`?, `target_class`? | Dry-run: returns resolved type + pins |

### Variable CRUD (8)

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `add_variable` | `asset_path`, `name`, `type`, `default_value`?, `category`? | Add member var |
| `remove_variable` / `rename_variable` | `asset_path`, `name`/`old_name`, `new_name`? | Remove/rename |
| `set_variable_type` | `asset_path`, `name`, `type` | Change type |
| `set_variable_defaults` | `asset_path`, `name`, flags | Update metadata |
| `add_local_variable` / `remove_local_variable` | `asset_path`, `function_name`, `name`, `type`? | Function locals |
| `add_replicated_variable` | `asset_path`, `variable_name`, `type`, `replication_condition`? | With OnRep stub |

**Type strings:** `bool`, `int`, `int64`, `float`, `double`, `string`, `name`, `text`, `byte`, `object:Class`, `class:Class`, `struct:Struct`, `enum:Enum`, `exec`, `wildcard`, `array:T`, `set:T`, `map:K:V`

### Component CRUD (6)

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `add_component` | `asset_path`, `component_class`, `name`?, `parent`? | Add SCS component |
| `remove_component` | `asset_path`, `component_name`, `promote_children`? | Remove |
| `rename_component` / `reparent_component` | `asset_path`, `component_name`, `new_name`/`new_parent` | Rename/reparent |
| `set_component_property` | `asset_path`, `component_name`, `property_name`, `value` | Set via reflection |
| `duplicate_component` | `asset_path`, `component_name`, `new_name`? | Duplicate |

### Graph Management (14)

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `add_function` | `asset_path`, `name`, `inputs`?, `outputs`?, `replication`?, `reliable`? | Create function |
| `remove_function` / `rename_function` | `asset_path`, `name`/`old_name` | Remove/rename |
| `add_macro` / `remove_macro` / `rename_macro` | `asset_path`, `name`/`macro_name` | Macro CRUD |
| `add_event_dispatcher` / `remove_event_dispatcher` | `asset_path`, `name`/`dispatcher_name` | Dispatcher CRUD |
| `set_event_dispatcher_params` | `asset_path`, `dispatcher_name`, `params` | Set signature |
| `set_function_params` | `asset_path`, `function_name`, `inputs`?, `outputs`? | Set signature |
| `implement_interface` / `remove_interface` | `asset_path`, `interface_class` | Add/remove interface |
| `scaffold_interface_implementation` | `asset_path`, `interface_class` | Add + create stubs |
| `reparent_blueprint` | `asset_path`, `new_parent_class` | Change parent |

### Node & Pin (7)

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `add_node` | `asset_path`, `node_type`, `graph_name`?, `position`?, `replication`?, `reliable`? | Add node |
| `remove_node` | `asset_path`, `node_id`, `graph_name`? | Remove node |
| `connect_pins` | `asset_path`, `source_node`, `source_pin`, `target_node`, `target_pin` | Wire pins (case-insensitive) |
| `disconnect_pins` | `asset_path`, `node_id`, `pin_name` | Break connections |
| `set_pin_default` | `asset_path`, `node_id`, `pin_name`, `value` | Set default |
| `set_node_position` | `asset_path`, `node_id`, `position` | Move to [x,y] |
| `promote_pin_to_variable` | `asset_path`, `node_id`, `pin_name`, `variable_name`? | Pin to member var |

#### `add_node` Types (~25)

| node_type | Extra Params | Aliases/Notes |
|-----------|-------------|-------|
| `CallFunction` | `function_name`, `target_class`? | `call`, `function` |
| `VariableGet`/`VariableSet` | `variable_name` | `get`/`set` |
| `CustomEvent` | `event_name`, `replication`?, `reliable`? | `event`. Server/client/multicast RPC |
| `Branch` / `Sequence` | — | |
| `MacroInstance` | `macro_name`, `macro_blueprint`? | `macro` |
| `SpawnActorFromClass` | `actor_class` | `spawn` |
| `DynamicCast` | `cast_class` | `cast` |
| `Self` / `Return` | — | |
| `MakeStruct` / `BreakStruct` | `struct_type` | |
| `SwitchOnEnum` / `SwitchOnInt` / `SwitchOnString` | `enum_type`? | |
| `FormatText` | `format`? | `"Hello {Name}"` creates arg pins |
| `MakeArray` | `num_entries`? | |
| `Select` / `ForEachLoop` / `ForLoop` / `ForLoopWithBreak` | — | |
| `DoOnce` / `FlipFlop` / `Gate` | — | Engine macros |
| `IsValid` / `Delay` / `RetriggerableDelay` | — | |
| `ComponentBoundEvent` | `component_name`, `delegate_property_name` | "+OnClicked" event entry. Rejects duplicate (component, delegate) BP-wide. Component must be SCS or UMG widget; delegate must be `BlueprintAssignable` multicast on the component class |
| `AddDelegate` | `delegate_property_name`, `target_class`? | "Bind Event to..." for `BlueprintAssignable` multicast. Defaults to self-context (BP's class); `target_class` accepts bare or prefixed forms |
| `RemoveDelegate` | `delegate_property_name`, `target_class`? | "Unbind Event from..." — removes one previously bound event. Same params as `AddDelegate` |
| `ClearDelegate` | `delegate_property_name`, `target_class`? | "Unbind all Events from..." — clears every bound listener. Same params as `AddDelegate` |
| `CallDelegate` | `delegate_property_name`, `target_class`? | "Call ..." — broadcasts a BP-resident multicast delegate to all listeners. Spawned node has one input pin per delegate signature parameter |
| *(any UK2Node_ class)* | — | Generic fallback |

### Compile & Create (6)

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `compile_blueprint` | `asset_path` | Errors include node_id + graph_name |
| `validate_blueprint` | `asset_path` | Lint: unused vars, disconnected nodes |
| `create_blueprint` | `save_path`, `parent_class`, `blueprint_type`? | Create new BP |
| `duplicate_blueprint` | `asset_path`, `new_path` | Duplicate |
| `get_dependencies` | `asset_path`, `direction`? | Asset deps |
| `save_asset` | `asset_path` | Save to disk |

### Timeline (4)

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `add_timeline` | `asset_path`, `timeline_name`?, `auto_play`?, `loop`? | Create timeline |
| `get_timeline_data` | `asset_path`, `timeline_name`? | Read tracks, keys |
| `add_timeline_track` | `asset_path`, `timeline_name`, `track_name`, `track_type`? | float/vector/event/color track |
| `set_timeline_keys` | `asset_path`, `timeline_name`, `track_name`, `keys` | `[{time, value, interp_mode?}]` |

### Struct, Enum & DataTable (6)

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `create_user_defined_struct` | `save_path`, `fields` | `[{name, type, default_value?}]` |
| `create_user_defined_enum` | `save_path`, `values` | `["Value1", "Value2"]` |
| `create_data_table` | `save_path`, `row_struct` | DataTable for struct |
| `create_data_asset` | `save_path`, `class_name`, `skip_save`? | Raw UObject (DataAssets, MPCs, etc.) |
| `add_data_table_row` | `asset_path`, `row_name`, `values` | `{column: value}` |
| `get_data_table_rows` | `asset_path`, `row_name`? | Read rows |

### Build from Spec (1)

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `build_blueprint_from_spec` | `asset_path`, `graph_name`?, `variables`?, `components`?, `nodes`, `connections`?, `pin_defaults`?, `auto_compile`? | One-shot declarative builder |

Nodes use spec IDs (e.g., `"id": "evt"`) mapped to real IDs in connections/pin_defaults.

### Graph Export (3)

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `export_graph` | `asset_path`, `graph_name`? | Export to JSON (build_from_spec compatible) |
| `copy_nodes` | `source_asset`, `source_graph`, `node_ids`, `target_asset`, `target_graph` | Copy via T3D |
| `duplicate_graph` | `asset_path`, `graph_name`, `new_name` | Duplicate within BP |

### Diff, Template, Layout, Batch, Events (11)

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `compare_blueprints` | `asset_path_a`, `asset_path_b` | Structural diff |
| `list_templates` | — | Available templates |
| `apply_template` | `template_name`, `asset_path`, `params`? | Apply template |
| `auto_layout` | `asset_path`, `graph_name`?, `layout_mode`?, `formatter`? | Auto-arrange. Modes: `all`/`new_only`/`selected`. Formatter: `monolith`/`blueprint_assist` |
| `add_event_node` | `asset_path`, `event_name`, `replication`?, `reliable`? | Override/custom event with RPC |
| `add_comment_node` | `asset_path`, `text`, `node_ids`?, `color`? | Comment box |
| `batch_execute` | `asset_path`, `operations`, `compile_on_complete`? | Multiple ops, one round-trip |
| `add_nodes_bulk` | `asset_path`, `nodes` (with `temp_id`) | Place multiple, returns ID map |
| `connect_pins_bulk` | `asset_path`, `connections` | Wire multiple |
| `set_pin_defaults_bulk` | `asset_path`, `defaults` | Set multiple defaults |

## Common Workflows

### Build from Spec (one call)
```
blueprint_query({ action: "build_blueprint_from_spec", params: {
  asset_path: "/Game/Test/BP_Door",
  nodes: [
    {"id": "evt", "type": "CustomEvent", "event_name": "OnInteract", "position": [0, 0]},
    {"id": "print", "type": "CallFunction", "function_name": "PrintString", "position": [300, 0]}
  ],
  connections: [{"source": "evt", "source_pin": "Then", "target": "print", "target_pin": "execute"}],
  pin_defaults: [{"node_id": "print", "pin_name": "InString", "value": "Door opened!"}],
  auto_compile: true
}})
```

### Server RPC
```
blueprint_query({ action: "add_node", params: {
  asset_path: "/Game/BP_Player", node_type: "CustomEvent",
  event_name: "ServerTakeDamage", replication: "server", reliable: true
}})
```

## Rules

- Pin names are case-insensitive. Wrong names show available pins in error.
- Compile errors include `node_id` + `graph_name` for targeted debugging.
- Any `UK2Node_` subclass name works as `node_type` (generic fallback).
- Use `get_graph_summary` first, then `get_graph_data` with `node_class_filter` for specifics.
- Always compile after structural changes.
