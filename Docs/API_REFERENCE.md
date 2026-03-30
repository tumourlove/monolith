# Monolith API Reference

**Total Actions: 815** across 12 namespaces (13 modules; MonolithBABridge is integration-only with 0 MCP actions)

> Auto-generated from action registration code. Each action is called via HTTP POST to `http://localhost:<port>` with JSON body `{ "namespace": "<ns>", "action": "<action>", "params": { ... } }`.
>
> **Note:** This reference covers all action signatures as of v0.11.0. Some sections below still use the detailed per-action format from earlier versions. For the most current param schemas, call `monolith_discover("namespace")` at runtime -- it returns live schemas directly from the plugin.

---

## Table of Contents

| Namespace | Actions | Description |
|-----------|---------|-------------|
| [monolith](#monolith) | 4 | Core server tools (discover, status, update, reindex) |
| [blueprint](#blueprint) | 86 | Blueprint read/write, variable/component/graph CRUD, node operations, compile, auto-layout |
| [material](#material) | 57 | Material graph editing, inspection, CRUD, material functions |
| [animation](#animation) | 115 | Animation curves, bone tracks, sync markers, root motion, compression, blend spaces, ABPs, montages, skeletons, PoseSearch, IKRig, Control Rig |
| [niagara](#niagara) | 96 | Niagara VFX system editing (emitters, modules, params, renderers, HLSL, dynamic inputs, event handlers, sim stages, NPC, effect types) |
| [editor](#editor) | 19 | Live Coding builds, compile output capture, editor log capture, scene capture, texture import |
| [config](#config) | 6 | INI config file inspection and search |
| [project](#project) | 7 | Project-wide asset index (SQLite + FTS5) |
| [source](#source) | 11 | Unreal Engine C++ source code navigation |
| [mesh](#mesh) | 242 | Mesh inspection, scene manipulation, spatial queries, level blockout, GeometryScript, procedural geometry, lighting, audio, performance, town gen (experimental) |
| [ui](#ui) | 42 | UI widget Blueprint CRUD, templates, styling, animation, settings scaffolding, accessibility |
| [gas](#gas) | 130 | Gameplay Ability System: abilities, attributes, effects, ASC, tags, cues, targeting, input, inspect, scaffold |

---

## monolith

Core server management and introspection tools.

### `monolith_discover`

List available tool namespaces and their actions. Pass namespace to filter.

> `discover` returns per-action param schemas for all 815 actions. AI clients also receive these schemas in `tools/list` at session start, so full param documentation is available without calling `discover` first.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `namespace` | string | optional | Filter to a specific namespace |

---

### `monolith_status`

Get Monolith server health: version, uptime, port, registered action count, module status.

*No parameters.*

---

### `monolith_update`

Check for or install Monolith updates from GitHub Releases.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `action` | string | optional | `"check"` to compare versions, `"install"` to download and stage update. Default: `"check"` |

---

### `monolith_reindex`

Trigger a full project re-index of the Monolith project database.

*No parameters.*

---

## blueprint

Full read/write access to Blueprint graphs, variables, components, functions, nodes, pins, and interfaces.

### `blueprint.list_graphs`

List all graphs in a Blueprint asset.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the Blueprint asset |

**Returns:** Array of graphs with name, type (event_graph/function/macro/delegate_signature), and node count.

---

### `blueprint.get_graph_summary`

Get a lightweight graph overview with node id/class/title and exec connections only. Much smaller payload than `get_graph_data` (~10KB vs ~172KB for complex graphs).

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the Blueprint asset |
| `graph_name` | string | optional | Graph name. Defaults to first UbergraphPage |

**Returns:** Array of nodes (id, class, title) and exec-only connections. No pin details or positions.

---

### `blueprint.get_graph_data`

Get full graph data with all nodes, pins, and connections.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the Blueprint asset |
| `graph_name` | string | optional | Graph name. Defaults to first UbergraphPage |
| `node_class_filter` | string | optional | Filter nodes by class name (case-insensitive substring match) |

**Returns:** Full node list with IDs, classes, titles, positions, pin details, and connections.

---

### `blueprint.get_variables`

Get all variables defined in a Blueprint.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the Blueprint asset |

**Returns:** Array of variables with name, type, default value, category, and flags (instance_editable, blueprint_read_only, expose_on_spawn, replicated, transient).

---

### `blueprint.get_execution_flow`

Get linearized execution flow from an entry point.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the Blueprint asset |
| `entry_point` | string | **required** | Event name, function name, or node title to trace from |

**Returns:** Recursive flow tree with nodes, branches, and execution paths.

---

### `blueprint.search_nodes`

Search for nodes in a Blueprint by title or function name.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the Blueprint asset |
| `query` | string | **required** | Search string matched against node title, class, and function name |

**Returns:** Matching nodes with graph, type, node ID, class, title, and function name.

---

### `blueprint.get_components`

Get the component hierarchy of a Blueprint.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the Blueprint asset |

**Returns:** Component tree with parent-child relationships, component class, and attach socket for each component.

---

### `blueprint.get_component_details`

Get full property reflection for a single component.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the Blueprint asset |
| `component_name` | string | **required** | Name of the component |

**Returns:** All reflected properties for the component with their current values and metadata.

---

### `blueprint.get_functions`

Get all functions defined in a Blueprint.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the Blueprint asset |

**Returns:** Array of functions with name, signature (inputs/outputs), access level, and purity/const flags.

---

### `blueprint.get_event_dispatchers`

Get all event dispatchers defined in a Blueprint.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the Blueprint asset |

**Returns:** Array of event dispatchers with name and parameter signatures.

---

### `blueprint.get_parent_class`

Get the parent class and Blueprint type information.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the Blueprint asset |

**Returns:** Parent class name, Blueprint type (Normal/Interface/Macro/FunctionLibrary), and Blueprint status.

---

### `blueprint.get_interfaces`

Get all interfaces implemented by a Blueprint.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the Blueprint asset |

**Returns:** Array of implemented interface class names.

---

### `blueprint.get_construction_script`

Get the construction script graph data for a Blueprint.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the Blueprint asset |

**Returns:** Full graph data for the construction script graph (nodes, pins, connections).

---

### `blueprint.add_variable`

Add a new variable to a Blueprint.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the Blueprint asset |
| `name` | string | **required** | Variable name |
| `type` | string | **required** | Variable type (e.g. `bool`, `int`, `float`, `FString`, `FVector`, `UObject`) |
| `default_value` | string | optional | Default value as string |
| `category` | string | optional | Category for editor organization |
| `instance_editable` | bool | optional | Expose in Details panel. Default: `false` |
| `blueprint_read_only` | bool | optional | Prevent Blueprint writes. Default: `false` |
| `expose_on_spawn` | bool | optional | Expose as spawn parameter. Default: `false` |
| `replicated` | bool | optional | Replicate across network. Default: `false` |
| `transient` | bool | optional | Do not serialize. Default: `false` |
| `save_game` | bool | optional | Include in SaveGame serialization. Default: `false` |

---

### `blueprint.remove_variable`

Remove a variable from a Blueprint.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the Blueprint asset |
| `name` | string | **required** | Name of the variable to remove |

---

### `blueprint.rename_variable`

Rename a variable in a Blueprint.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the Blueprint asset |
| `old_name` | string | **required** | Current variable name |
| `new_name` | string | **required** | New variable name |

---

### `blueprint.set_variable_type`

Change the type of an existing variable.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the Blueprint asset |
| `name` | string | **required** | Variable name |
| `type` | string | **required** | New variable type |

---

### `blueprint.set_variable_defaults`

Set default value and metadata flags on an existing variable.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the Blueprint asset |
| `name` | string | **required** | Variable name |
| `default_value` | string | optional | Default value as string |
| `category` | string | optional | Category for editor organization |
| `instance_editable` | bool | optional | Expose in Details panel |
| `blueprint_read_only` | bool | optional | Prevent Blueprint writes |
| `expose_on_spawn` | bool | optional | Expose as spawn parameter |
| `replicated` | bool | optional | Replicate across network |
| `transient` | bool | optional | Do not serialize |
| `save_game` | bool | optional | Include in SaveGame serialization |

---

### `blueprint.add_local_variable`

Add a local variable scoped to a specific function.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the Blueprint asset |
| `function_name` | string | **required** | Name of the function to add the local variable to |
| `name` | string | **required** | Variable name |
| `type` | string | **required** | Variable type |
| `default_value` | string | optional | Default value as string |

---

### `blueprint.remove_local_variable`

Remove a local variable from a specific function.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the Blueprint asset |
| `function_name` | string | **required** | Name of the function containing the variable |
| `name` | string | **required** | Name of the local variable to remove |

---

### `blueprint.add_component`

Add a component to a Blueprint's component hierarchy.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the Blueprint asset |
| `component_class` | string | **required** | Component class name (e.g. `StaticMeshComponent`, `PointLightComponent`) |
| `name` | string | optional | Name for the new component |
| `parent` | string | optional | Name of parent component. Defaults to root |
| `attach_socket` | string | optional | Socket name on parent to attach to |

---

### `blueprint.remove_component`

Remove a component from a Blueprint.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the Blueprint asset |
| `component_name` | string | **required** | Name of the component to remove |
| `promote_children` | bool | optional | Re-parent children to the removed component's parent. Default: `false` |

---

### `blueprint.rename_component`

Rename a component in a Blueprint.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the Blueprint asset |
| `component_name` | string | **required** | Current component name |
| `new_name` | string | **required** | New component name |

---

### `blueprint.reparent_component`

Change the parent of a component in the component hierarchy.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the Blueprint asset |
| `component_name` | string | **required** | Name of the component to reparent |
| `new_parent` | string | **required** | Name of the new parent component |
| `attach_socket` | string | optional | Socket name on the new parent to attach to |

---

### `blueprint.set_component_property`

Set a property value on a Blueprint component.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the Blueprint asset |
| `component_name` | string | **required** | Name of the component |
| `property_name` | string | **required** | Name of the property to set |
| `value` | string | **required** | Value to set as string |

---

### `blueprint.duplicate_component`

Duplicate an existing component within a Blueprint.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the Blueprint asset |
| `component_name` | string | **required** | Name of the component to duplicate |
| `new_name` | string | optional | Name for the duplicate. Auto-generated if omitted |

---

### `blueprint.add_function`

Add a new function graph to a Blueprint.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the Blueprint asset |
| `name` | string | **required** | Function name |
| `is_pure` | bool | optional | Mark as pure (no exec pins). Default: `false` |
| `is_const` | bool | optional | Mark as const. Default: `false` |
| `is_static` | bool | optional | Mark as static. Default: `false` |
| `call_in_editor` | bool | optional | Expose as callable in editor. Default: `false` |
| `category` | string | optional | Category for editor organization |
| `description` | string | optional | Tooltip description |
| `access` | string | optional | Access level: `public`, `protected`, or `private`. Default: `public` |

---

### `blueprint.remove_function`

Remove a function graph from a Blueprint.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the Blueprint asset |
| `name` | string | **required** | Name of the function to remove |

---

### `blueprint.rename_function`

Rename a function in a Blueprint.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the Blueprint asset |
| `old_name` | string | **required** | Current function name |
| `new_name` | string | **required** | New function name |

---

### `blueprint.add_macro`

Add a new macro graph to a Blueprint.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the Blueprint asset |
| `name` | string | **required** | Macro name |

---

### `blueprint.add_event_dispatcher`

Add a new event dispatcher to a Blueprint.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the Blueprint asset |
| `name` | string | **required** | Event dispatcher name |

---

### `blueprint.set_function_params`

Set input and output parameters on a function.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the Blueprint asset |
| `function_name` | string | **required** | Name of the function |
| `inputs` | array | optional | Array of `{ name, type, default_value? }` input parameter objects |
| `outputs` | array | optional | Array of `{ name, type }` output parameter objects |

---

### `blueprint.implement_interface`

Add an interface implementation to a Blueprint.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the Blueprint asset |
| `interface_class` | string | **required** | Interface class name to implement |

---

### `blueprint.remove_interface`

Remove an interface implementation from a Blueprint.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the Blueprint asset |
| `interface_class` | string | **required** | Interface class name to remove |
| `preserve_functions` | bool | optional | Keep generated function graphs as regular functions. Default: `false` |

---

### `blueprint.reparent_blueprint`

Change the parent class of a Blueprint.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the Blueprint asset |
| `new_parent_class` | string | **required** | New parent class name |

---

### `blueprint.add_node`

Add a node to a Blueprint graph.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the Blueprint asset |
| `node_type` | string | **required** | Node type: `CallFunction`, `VariableGet`, `VariableSet`, `CustomEvent`, `Branch`, `Sequence`, `MacroInstance`, `SpawnActorFromClass` |
| `graph_name` | string | optional | Target graph. Defaults to first UbergraphPage |
| `position` | array | optional | `[x, y]` node position |
| `function_name` | string | optional | For `CallFunction` — function to call (e.g. `ClassName::FunctionName`) |
| `variable_name` | string | optional | For `VariableGet`/`VariableSet` — variable name |
| `event_name` | string | optional | For `CustomEvent` — event name |
| `class_name` | string | optional | For `SpawnActorFromClass` — actor class |
| `macro_path` | string | optional | For `MacroInstance` — asset path of macro library |

**Returns:** Node ID of the newly created node.

---

### `blueprint.remove_node`

Remove a node from a Blueprint graph.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the Blueprint asset |
| `node_id` | string | **required** | ID of the node to remove |
| `graph_name` | string | optional | Graph containing the node |

---

### `blueprint.connect_pins`

Connect two pins in a Blueprint graph.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the Blueprint asset |
| `source_node` | string | **required** | ID of the source node |
| `source_pin` | string | **required** | Name of the output pin on the source node |
| `target_node` | string | **required** | ID of the target node |
| `target_pin` | string | **required** | Name of the input pin on the target node |
| `graph_name` | string | optional | Graph containing the nodes |

---

### `blueprint.disconnect_pins`

Disconnect a pin connection in a Blueprint graph.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the Blueprint asset |
| `node_id` | string | **required** | ID of the node whose pin to disconnect |
| `pin_name` | string | **required** | Name of the pin to disconnect |
| `target_node` | string | optional | Disconnect only the link to this specific target node |
| `target_pin` | string | optional | Disconnect only the link to this specific target pin |
| `graph_name` | string | optional | Graph containing the node |

---

### `blueprint.set_pin_default`

Set the default value on a pin.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the Blueprint asset |
| `node_id` | string | **required** | ID of the node |
| `pin_name` | string | **required** | Name of the pin |
| `value` | string | **required** | Default value as string |
| `graph_name` | string | optional | Graph containing the node |

---

### `blueprint.set_node_position`

Move a node to a new position in a graph.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the Blueprint asset |
| `node_id` | string | **required** | ID of the node to move |
| `position` | array | **required** | `[x, y]` new position |
| `graph_name` | string | optional | Graph containing the node |

---

### `blueprint.compile_blueprint`

Compile a Blueprint asset.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the Blueprint asset |

**Returns:** Compile result with success/failure status and any compiler messages.

---

### `blueprint.validate_blueprint`

Validate a Blueprint for errors without a full compile.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the Blueprint asset |

**Returns:** Validation result with any errors or warnings found.

---

### `blueprint.create_blueprint`

Create a new Blueprint asset.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `save_path` | string | **required** | Package path where the Blueprint will be saved (e.g. `/Game/Blueprints/BP_MyActor`) |
| `parent_class` | string | **required** | Parent class name (e.g. `Actor`, `Character`, `ActorComponent`) |
| `blueprint_type` | string | optional | Blueprint type: `Normal`, `Interface`, `MacroLibrary`, `FunctionLibrary`. Default: `Normal` |

**Returns:** Asset path of the created Blueprint.

---

### `blueprint.duplicate_blueprint`

Duplicate an existing Blueprint asset.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the source Blueprint |
| `new_path` | string | **required** | Package path for the duplicate |

**Returns:** Asset path of the duplicated Blueprint.

---

### `blueprint.get_dependencies`

Get asset dependencies for a Blueprint.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the Blueprint asset |
| `direction` | string | optional | `depends_on` (what this Blueprint uses), `referenced_by` (what uses this Blueprint), or `both`. Default: `both` |

**Returns:** Dependency lists with asset paths and types.

---

## material

Material graph editing and inspection tools.

### `material.get_all_expressions`

Get all expression nodes in a base material.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the material asset |

---

### `material.get_expression_details`

Get full property reflection, inputs, and outputs for a single expression.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the material asset |
| `expression_name` | string | **required** | Name of the expression node |

---

### `material.get_full_connection_graph`

Get the complete connection graph (all wires) of a material.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the material asset |

---

### `material.disconnect_expression`

Disconnect inputs or outputs on a named expression.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the material asset |
| `expression_name` | string | **required** | Name of the expression node |
| `input_name` | string | optional | Specific input to disconnect |
| `target_expression` | string | optional | Disconnect only links from this source expression |
| `output_index` | number | optional | Disconnect only this output index on the source expression |
| `disconnect_outputs` | bool | optional | Also disconnect outputs. Default: `false` |

---

### `material.build_material_graph`

Build entire material graph from JSON spec in a single undo transaction. Automatically recompiles the material on success.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the material asset |
| `graph_spec` | object/string | **required** | JSON spec with `nodes`, `custom_hlsl_nodes`, `connections`, `outputs` arrays |
| `clear_existing` | bool | optional | Clear existing graph before building. Default: `false` |

**Returns:** Build result including `"recompiled": true` when compilation succeeded. Emits blend mode validation warnings (e.g. Opacity on Opaque, OpacityMask on non-Masked).

---

### `material.begin_transaction`

Begin a named undo transaction for batching edits.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `transaction_name` | string | **required** | Name for the undo transaction |

---

### `material.end_transaction`

End the current undo transaction.

*No parameters.*

---

### `material.export_material_graph`

Export complete material graph to JSON (round-trippable with build_material_graph).

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the material asset |
| `include_properties` | bool | optional | Include node property details. Default: `true` |
| `include_positions` | bool | optional | Include node X/Y positions. Default: `true` |

---

### `material.import_material_graph`

Import material graph from JSON string. Mode: overwrite or merge.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the material asset |
| `graph_json` | string | **required** | JSON string of the graph to import |
| `mode` | string | optional | `"overwrite"` or `"merge"`. Default: `"overwrite"` |

---

### `material.validate_material`

Validate material graph health and optionally auto-fix issues.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the material asset |
| `fix_issues` | bool | optional | Auto-fix detected issues. Default: `false` |

---

### `material.render_preview`

Render material preview to PNG file.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the material asset |
| `resolution` | number | optional | Preview resolution in pixels. Default: `256` |

---

### `material.get_thumbnail`

Get material thumbnail as base64-encoded PNG.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the material asset |
| `resolution` | number | optional | Thumbnail resolution in pixels. Default: `256` |
| `save_to_file` | bool | optional | Save thumbnail to `Saved/Monolith/thumbnails/` instead of returning base64. Default: `false` |

---

### `material.create_custom_hlsl_node`

Create a Custom HLSL expression node with inputs, outputs, and code.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the material asset |
| `code` | string | **required** | HLSL code for the custom node |
| `description` | string | optional | Node description |
| `output_type` | string | optional | Output type name |
| `pos_x` | number | optional | Node X position. Default: `0` |
| `pos_y` | number | optional | Node Y position. Default: `0` |
| `inputs` | array | optional | Array of `{ "name": "..." }` input definitions |
| `additional_outputs` | array | optional | Array of `{ "name": "...", "type": "..." }` output definitions |

---

### `material.get_layer_info`

Get Material Layer or Material Layer Blend info.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the material layer asset |

---

### `material.create_material`

Create a new UMaterial asset at the specified path.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path for the new material |
| `blend_mode` | string | optional | Blend mode (Opaque, Masked, Translucent, etc.). Default: `"Opaque"` |
| `shading_model` | string | optional | Shading model (DefaultLit, Unlit, etc.). Default: `"DefaultLit"` |
| `material_domain` | string | optional | Material domain (Surface, DeferredDecal, etc.). Default: `"Surface"` |

---

### `material.create_material_instance`

Create a UMaterialInstanceConstant from a parent material.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path for the new material instance |
| `parent_material` | string | **required** | Package path of the parent material |
| `parameters` | object | optional | Parameter overrides to set on creation |

---

### `material.set_material_property`

Set material properties (blend_mode, shading_model, two_sided, etc.).

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the material asset |
| `property` | string | **required** | Property name to set |
| `value` | any | **required** | Value to set |

---

### `material.delete_expression`

Delete an expression node by name from a material graph.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the material asset |
| `expression_name` | string | **required** | Name of the expression to delete |

---

### `material.get_material_parameters`

List all parameter types (scalar, vector, texture, static switch) with current values. Works on both UMaterial and UMaterialInstanceConstant.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the material or material instance |

---

### `material.set_instance_parameter`

Set a parameter value on a UMaterialInstanceConstant.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the material instance |
| `parameter_name` | string | **required** | Name of the parameter |
| `value` | any | **required** | Value to set |
| `type` | string | **required** | Parameter type: `"scalar"`, `"vector"`, `"texture"`, `"static_switch"` |

---

### `material.recompile_material`

Force a material recompile.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the material asset |

---

### `material.duplicate_material`

Duplicate a material asset to a new path.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the source material |
| `destination_path` | string | **required** | Package path for the duplicate |

---

### `material.get_compilation_stats`

Get material compilation statistics: vertex shader instruction count, pixel shader instruction count, sampler count, texture estimates, UV scalars, blend mode, expression count.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the material asset |

---

### `material.set_expression_property`

Set a property on an expression node (e.g., DefaultValue on a scalar parameter). Also accepts `property_name` as an alias for the `property` param.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the material asset |
| `expression_name` | string | **required** | Name of the expression node |
| `property` | string | **required** | Property name to set (alias: `property_name`) |
| `value` | any | **required** | Value to set |

---

### `material.connect_expressions`

Wire an expression output to another expression input or a material property input. Emits blend mode validation warnings when connecting to Opacity/OpacityMask outputs on mismatched blend modes.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the material asset |
| `from` | string | **required** | Source: `"ExpressionName.OutputName"` |
| `to` | string | **required** | Target: `"ExpressionName.InputName"` or `"Material.PropertyName"` |

---

## animation

Animation asset editing -- curves, bone tracks, sync markers, root motion, compression, blend spaces, ABPs, montages, notifies, skeletons, batch operations, and PoseSearch.

### Curve Operations

#### `animation.get_curves`

Get all curves in an animation sequence.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the animation sequence |

**Returns:** `curves` array with `name` and `type` for each curve.

---

#### `animation.add_curve`

Add a curve to an animation sequence.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the animation sequence |
| `curve_name` | string | **required** | Name for the new curve |
| `curve_type` | string | optional | Curve type. Default: `"float"` |

**Returns:** Added curve info (name, type).

---

#### `animation.remove_curve`

Remove a curve from an animation sequence.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the animation sequence |
| `curve_name` | string | **required** | Name of the curve to remove |

**Returns:** Removed curve name.

---

#### `animation.set_curve_keys`

Set keys on a curve in an animation sequence.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the animation sequence |
| `curve_name` | string | **required** | Name of the curve |
| `keys` | array | **required** | Array of key objects: `{ "time": float, "value": float }` |

**Returns:** `key_count` — number of keys set.

---

#### `animation.get_curve_keys`

Get all keys from a curve in an animation sequence.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the animation sequence |
| `curve_name` | string | **required** | Name of the curve |

**Returns:** `keys` array with `time` and `value` for each key.

---

#### `animation.rename_curve`

Rename a curve in an animation sequence.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the animation sequence |
| `old_name` | string | **required** | Current curve name |
| `new_name` | string | **required** | New curve name |

**Returns:** `old_name` and `new_name`.

---

#### `animation.get_curve_data`

Get all curves with their keys and metadata from an animation sequence.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the animation sequence |

**Returns:** `curves` array, each with `name`, `type`, and `keys` (array of `{ time, value }`).

---

### Bone Track Inspection

#### `animation.get_bone_tracks`

Get all bone tracks in an animation sequence with key counts.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the animation sequence |

**Returns:** `tracks` array with `bone_name`, `num_pos_keys`, `num_rot_keys`, `num_scale_keys`.

---

#### `animation.get_bone_track_data`

Get position, rotation, and scale key data for a specific bone track.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the animation sequence |
| `bone_name` | string | **required** | Name of the bone |
| `max_keys` | number | optional | Maximum number of keys to return per channel |

**Returns:** `position_keys`, `rotation_keys`, `scale_keys` arrays with time and value data.

---

#### `animation.get_animation_statistics`

Get animation sequence statistics.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the animation sequence |

**Returns:** `compressed_size`, `num_curves`, `num_bone_tracks`, `duration`, `num_frames`, `frame_rate`, and other metadata.

---

### Sync Markers

#### `animation.get_sync_markers`

Get all sync markers in an animation sequence.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the animation sequence |

**Returns:** `markers` array with `name` and `time`.

---

#### `animation.add_sync_marker`

Add a sync marker to an animation sequence.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the animation sequence |
| `marker_name` | string | **required** | Name for the sync marker |
| `time` | number | **required** | Time position in seconds |

**Returns:** Added marker info (name, time).

---

#### `animation.remove_sync_marker`

Remove a sync marker from an animation sequence by name.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the animation sequence |
| `marker_name` | string | **required** | Name of the marker to remove |

**Returns:** Removed marker name.

---

### Root Motion

#### `animation.get_root_motion_info`

Get root motion settings and accumulated totals for an animation sequence.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the animation sequence |

**Returns:** `enable_root_motion`, `force_root_lock`, `root_motion_root_lock`, `total_translation` (vector), `total_rotation` (rotator).

---

#### `animation.extract_root_motion`

Extract root motion translation and rotation over a time range.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the animation sequence |
| `start_time` | number | **required** | Start time in seconds |
| `end_time` | number | **required** | End time in seconds |

**Returns:** `translation` (vector) and `rotation` (rotator) for the specified range.

---

### Animation Compression

#### `animation.get_compression_settings`

Get compression codec information for an animation sequence.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the animation sequence |

**Returns:** `codec_name`, `codec_description`, `compression_scheme`.

---

#### `animation.apply_compression`

Apply compression to an animation sequence.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the animation sequence |
| `codec_class` | string | optional | Codec class name to use. Default: engine default codec |

**Returns:** `codec` used and `compressed_size`.

---

### BlendSpace Operations

#### `animation.get_blendspace_info`

Get blend space axis info, dimensions, and sample count.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the blend space asset |

**Returns:** `axis_x` (name, min, max, grid_divisions), `axis_y`, `dimensions`, `sample_count`, `skeleton`.

---

#### `animation.add_blendspace_sample`

Add a sample to a blend space.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the blend space asset |
| `animation_path` | string | **required** | Package path of the animation sequence |
| `x` | number | **required** | X-axis sample position |
| `y` | number | **required** | Y-axis sample position |

**Returns:** `index` and `sample_count`.

---

#### `animation.remove_blendspace_sample`

Remove a sample from a blend space by index.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the blend space asset |
| `sample_index` | number | **required** | Index of the sample to remove |

**Returns:** `removed_index` and `remaining_count`.

---

#### `animation.set_blendspace_axis`

Set axis properties on a blend space.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the blend space asset |
| `axis` | string | **required** | Axis to modify: `"X"` or `"Y"` |
| `min` | number | optional | Minimum axis value |
| `max` | number | optional | Maximum axis value |
| `name` | string | optional | Axis display name |
| `grid_divisions` | number | optional | Number of grid divisions |

**Returns:** Updated axis info.

---

#### `animation.get_blendspace_samples`

Get all samples in a blend space with their positions and animations.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the blend space asset |

**Returns:** `samples` array with `index`, `animation`, `x`, `y`.

---

#### `animation.edit_blendspace_sample`

Edit a blend space sample position and optionally its animation.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the blend space asset |
| `sample_index` | number | **required** | Index of the sample to edit |
| `x` | number | **required** | New X-axis position |
| `y` | number | **required** | New Y-axis position |
| `anim_path` | string | optional | New animation sequence path |

---

#### `animation.delete_blendspace_sample`

Delete a sample from a blend space by index (legacy alias).

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the blend space asset |
| `sample_index` | number | **required** | Index of the sample to delete |

---

### ABP Graph Reading

#### `animation.get_anim_blueprint_info`

Get animation blueprint overview: target skeleton, parent class, graph count, and variables.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the anim blueprint |

**Returns:** `target_skeleton`, `parent_class`, `num_graphs`, `variables` array.

---

#### `animation.get_state_machines`

Get all state machines in an animation blueprint.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the anim blueprint |

**Returns:** `machines` array with `name`, `num_states`, `num_transitions`.

---

#### `animation.get_state_info`

Get detailed info about a state in a state machine.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the anim blueprint |
| `machine_name` | string | **required** | State machine name |
| `state_name` | string | **required** | State name |

**Returns:** State details including linked anim graph info.

---

#### `animation.get_transitions`

Get all transitions in a state machine.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the anim blueprint |
| `machine_name` | string | **required** | State machine name (empty string for ALL state machines) |

**Returns:** `transitions` array with `from`, `to`, `from_type`, `to_type`, `priority`, `duration`.

---

#### `animation.get_anim_graph_nodes`

Get animation graph nodes with optional graph name filter.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the anim blueprint |
| `graph_name` | string | optional | Filter to a specific graph by name |

**Returns:** `nodes` array with `class`, `title`, `position`.

---

#### `animation.get_blend_nodes`

Get blend nodes in an animation blueprint graph.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the anim blueprint |
| `graph_name` | string | optional | Specific graph name to search |

---

#### `animation.get_linked_layers`

Get linked animation layers in an animation blueprint.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the anim blueprint |

---

#### `animation.get_graphs`

Get all graphs in an animation blueprint.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the anim blueprint |

---

#### `animation.get_nodes`

Get animation nodes with optional class and graph filters.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the anim blueprint |
| `node_class_filter` | string | optional | Filter by node class name |
| `graph_name` | string | optional | Filter to a specific graph by name |

---

### Montage Operations

#### `animation.get_montage_info`

Get montage overview: sections, slots, blend settings, and sequence info.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the montage asset |

**Returns:** `sections` array, `slots` array, `blend_in`, `blend_out`, sequence info.

---

#### `animation.add_montage_section`

Add a section to an animation montage.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the montage asset |
| `section_name` | string | **required** | Name for the new section |
| `start_time` | number | **required** | Start time in seconds |

**Returns:** Added section info.

---

#### `animation.delete_montage_section`

Delete a section from an animation montage by name.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the montage asset |
| `section_name` | string | **required** | Name of the section to delete |

**Returns:** Removed section name.

---

#### `animation.set_montage_section_link`

Set the next section link for a montage section.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the montage asset |
| `section_name` | string | **required** | Source section name |
| `next_section` | string | **required** | Target next section name |

**Returns:** Linked section names.

---

#### `animation.get_montage_slots`

Get montage slot and section information.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the montage asset |

**Returns:** `slots` array with `name` and `num_segments`, `sections` array.

---

#### `animation.set_section_next`

Set the next section for a montage section (legacy).

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the montage asset |
| `section_name` | string | **required** | Source section name |
| `next_section_name` | string | **required** | Target next section name |

---

#### `animation.set_section_time`

Set the start time of a montage section (legacy).

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the montage asset |
| `section_name` | string | **required** | Section name |
| `new_time` | number | **required** | New start time in seconds |

---

### Notify Editing

#### `animation.set_notify_time`

Set the trigger time of an animation notify.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the animation asset |
| `notify_index` | number | **required** | Index of the notify |
| `new_time` | number | **required** | New trigger time in seconds |

---

#### `animation.set_notify_duration`

Set the duration of a state animation notify.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the animation asset |
| `notify_index` | number | **required** | Index of the notify |
| `new_duration` | number | **required** | New duration in seconds |

---

### Bone Track Editing

#### `animation.set_bone_track_keys`

Set position, rotation, and scale keys on a bone track.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the animation sequence |
| `bone_name` | string | **required** | Name of the bone |
| `positions_json` | string | **required** | JSON array of position keys |
| `rotations_json` | string | **required** | JSON array of rotation keys |
| `scales_json` | string | **required** | JSON array of scale keys |

---

#### `animation.add_bone_track`

Add a bone track to an animation sequence.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the animation sequence |
| `bone_name` | string | **required** | Name of the bone to add a track for |

---

#### `animation.remove_bone_track`

Remove a bone track from an animation sequence.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the animation sequence |
| `bone_name` | string | **required** | Name of the bone track to remove |
| `include_children` | bool | optional | Also remove child bone tracks. Default: `false` |

---

### Skeleton Operations

#### `animation.get_skeleton_info`

Get skeleton bone hierarchy, virtual bones, and sockets.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the skeleton asset |

**Returns:** `bone_count`, `bones` array (name, parent, depth), `virtual_bones`, `sockets`.

---

#### `animation.add_virtual_bone`

Add a virtual bone to a skeleton.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the skeleton asset |
| `source_bone` | string | **required** | Source bone name |
| `target_bone` | string | **required** | Target bone name |
| `name` | string | optional | Custom name for the virtual bone |

**Returns:** Virtual bone name.

---

#### `animation.remove_virtual_bones`

Remove virtual bones from a skeleton.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the skeleton asset |
| `bone_names` | array | **required** | Array of virtual bone names to remove |

**Returns:** Removed bone names list.

---

#### `animation.get_socket_info`

Get socket details from a skeleton.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the skeleton asset |
| `socket_name` | string | optional | Filter to a specific socket by name |

**Returns:** `sockets` array with `name`, `bone`, `position`, `rotation`, `scale`.

---

#### `animation.add_socket`

Add a socket to a skeleton.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the skeleton asset |
| `socket_name` | string | **required** | Name for the new socket |
| `parent_bone` | string | **required** | Parent bone name |
| `position` | object | optional | Socket position `{ "x", "y", "z" }` |
| `rotation` | object | optional | Socket rotation `{ "pitch", "yaw", "roll" }` |
| `scale` | object | optional | Socket scale `{ "x", "y", "z" }` |

**Returns:** Socket info (name, bone, position, rotation, scale).

---

### Skeleton Mesh Info

#### `animation.get_skeletal_mesh_info`

Get skeletal mesh info including morph targets, sockets, LODs, and materials.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the skeletal mesh asset |

---

### Batch & Modifiers

#### `animation.batch_get_animation_info`

Get basic info for multiple animation assets in a single call.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_paths` | array | **required** | Array of animation sequence package paths |

**Returns:** `results` array (each with `path`, `length`, `frames`, `rate`), `failed` array.

---

#### `animation.run_animation_modifier`

Apply an animation modifier class to an animation sequence.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the animation sequence |
| `modifier_class` | string | **required** | Class name of the animation modifier to apply |

**Returns:** Modifier applied info.

---

### PoseSearch

#### `animation.get_pose_search_schema`

Get PoseSearch schema configuration, channels, and skeleton reference.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the PoseSearch schema asset |

**Returns:** Schema config, `channels` array, `skeleton` reference.

---

#### `animation.get_pose_search_database`

Get PoseSearch database sequences and schema reference.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the PoseSearch database asset |

**Returns:** `sequences` array, `schema` reference.

---

#### `animation.add_database_sequence`

Add an animation sequence to a PoseSearch database.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the PoseSearch database asset |
| `anim_path` | string | **required** | Package path of the animation sequence to add |
| `enabled` | bool | optional | Whether the sequence is enabled. Default: `true` |

**Returns:** Added sequence info.

---

#### `animation.remove_database_sequence`

Remove a sequence from a PoseSearch database by index.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the PoseSearch database asset |
| `sequence_index` | number | **required** | Index of the sequence to remove |

**Returns:** Removed sequence info.

---

#### `animation.get_database_stats`

Get PoseSearch database statistics.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the PoseSearch database asset |

**Returns:** `pose_count`, `search_mode`, cost biases.

---

## niagara

Niagara VFX system editing -- emitters, modules, parameters, renderers, and batch operations.

> **Note:** All Niagara actions accept `asset_path` (preferred) or `system_path` (backward compatible) for the system asset path parameter.
>
> **Param aliases:** Several params accept multiple names for convenience:
> - Module identifier: `module`, `module_name`, `module_node` (all interchangeable)
> - Input name: `input`, `input_name` (interchangeable)
> - Property name: `property`, `property_name` (interchangeable)
> - Renderer class: `class`, `renderer_class`, `renderer_type` (interchangeable)

### Emitter Actions

#### `niagara.add_emitter`

Add an emitter to a Niagara system.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `system_path` | string | **required** | Package path of the Niagara system |
| `emitter_asset` | string | **required** | Package path of the emitter asset to add |
| `name` | string | optional | Custom name for the emitter handle |

---

#### `niagara.remove_emitter`

Remove an emitter from a Niagara system.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `system_path` | string | **required** | Package path of the Niagara system |
| `emitter` | string | **required** | Emitter handle ID |

---

#### `niagara.duplicate_emitter`

Duplicate an emitter within a Niagara system.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `system_path` | string | **required** | Package path of the Niagara system |
| `source_emitter` | string | **required** | Source emitter handle ID |
| `new_name` | string | optional | Name for the duplicated emitter |

---

#### `niagara.set_emitter_enabled`

Enable or disable an emitter.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `system_path` | string | **required** | Package path of the Niagara system |
| `emitter` | string | **required** | Emitter handle ID |
| `enabled` | bool | **required** | Enable state |

---

#### `niagara.reorder_emitters`

Reorder emitters in a system.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `system_path` | string | **required** | Package path of the Niagara system |
| `order` | array | **required** | Array of emitter handle IDs in desired order |

---

#### `niagara.set_emitter_property`

Set an emitter property.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `system_path` | string | **required** | Package path of the Niagara system |
| `emitter` | string | **required** | Emitter handle ID |
| `property` | string | **required** | Property name |
| `value` | any | optional | Property value |

---

#### `niagara.set_system_property`

Set a system-level property via snake_case alias or reflection fallback.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the Niagara system |
| `property` | string | **required** | Property name (snake_case alias or reflected property name) |
| `value` | any | **required** | Property value |

Supported aliases: `warmup_time`, `b_determinism`, `b_fixed_tick_delta`, `random_seed`, `max_pool_size`, etc.

---

#### `niagara.request_compile`

Request compilation of a Niagara system.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `system_path` | string | **required** | Package path of the Niagara system |

---

#### `niagara.create_system`

Create a new Niagara system.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `save_path` | string | **required** | Package path to save the new system |
| `template` | string | optional | Template system asset path |

---

#### `niagara.list_emitters`

List all emitters in a Niagara system with summary info.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the Niagara system |

**Returns:** Array of emitters with name, index, enabled, sim_target, and renderer_count.

---

#### `niagara.list_renderers`

List all renderers across emitters in a Niagara system.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the Niagara system |
| `emitter` | string | optional | Filter to a specific emitter |

**Returns:** Array of renderers with emitter, `type` (short class name), index, enabled, and material.

---

#### `niagara.list_module_scripts`

Search available Niagara module scripts by keyword.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `query` | string | **required** | Keyword to search for in module script asset names and paths |

**Returns:** Array of matching module script asset paths.

---

#### `niagara.list_renderer_properties`

List editable properties on a renderer.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the Niagara system |
| `emitter` | string | **required** | Emitter handle ID |
| `renderer` | string | **required** | Renderer index or class name |

**Returns:** Array of property names and their current values.

---

### Module Actions

#### `niagara.get_ordered_modules`

Get ordered modules in a script stage.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `system_path` | string | **required** | Package path of the Niagara system |
| `emitter` | string | **required** | Emitter handle ID |
| `usage` | string | **required** | Script usage (e.g. `"EmitterSpawnScript"`, `"ParticleUpdateScript"`) |

---

#### `niagara.get_module_inputs`

Get inputs for a module node.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `system_path` | string | **required** | Package path of the Niagara system |
| `emitter` | string | **required** | Emitter handle ID |
| `module_node` | string | **required** | Module node GUID |

---

#### `niagara.get_module_graph`

Get the node graph of a module script.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `script_path` | string | **required** | Package path of the Niagara script |

---

#### `niagara.add_module`

Add a module to a script stage.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `system_path` | string | **required** | Package path of the Niagara system |
| `emitter` | string | **required** | Emitter handle ID |
| `usage` | string | **required** | Script usage stage |
| `module_script` | string | **required** | Module script asset path |
| `index` | number | optional | Insertion index. `-1` to append |

---

#### `niagara.remove_module`

Remove a module from a script stage.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `system_path` | string | **required** | Package path of the Niagara system |
| `emitter` | string | **required** | Emitter handle ID |
| `module_node` | string | **required** | Module node GUID |

---

#### `niagara.move_module`

Move a module to a new index.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `system_path` | string | **required** | Package path of the Niagara system |
| `emitter` | string | **required** | Emitter handle ID |
| `module_node` | string | **required** | Module node GUID |
| `new_index` | number | **required** | Target index |

---

#### `niagara.set_module_enabled`

Enable or disable a module.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `system_path` | string | **required** | Package path of the Niagara system |
| `emitter` | string | **required** | Emitter handle ID |
| `module_node` | string | **required** | Module node GUID |
| `enabled` | bool | **required** | Enable state |

---

#### `niagara.set_module_input_value`

Set a module input value.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `system_path` | string | **required** | Package path of the Niagara system |
| `emitter` | string | **required** | Emitter handle ID |
| `module_node` | string | **required** | Module node GUID |
| `input` | string | **required** | Input parameter name |
| `value` | any | optional | Value (number, bool, string, vector `{x,y,z}`, color `{r,g,b,a}`) |

---

#### `niagara.set_module_input_binding`

Bind a module input to a parameter.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `system_path` | string | **required** | Package path of the Niagara system |
| `emitter` | string | **required** | Emitter handle ID |
| `module_node` | string | **required** | Module node GUID |
| `input` | string | **required** | Input parameter name |
| `binding` | string | **required** | Parameter path to bind to |

---

#### `niagara.set_module_input_di`

Set a data interface on a module input.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `system_path` | string | **required** | Package path of the Niagara system |
| `emitter` | string | **required** | Emitter handle ID |
| `module_node` | string | **required** | Module node GUID |
| `input` | string | **required** | Input parameter name |
| `di_class` | string | **required** | Data interface class name (auto-resolves UNiagara/UNiagaraDataInterface prefix) |
| `config` | string | optional | JSON configuration for DI properties |

---

#### `niagara.set_static_switch_value`

Set a static switch value on a module input.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the Niagara system |
| `emitter` | string | **required** | Emitter handle ID |
| `module_node` | string | **required** | Module node GUID |
| `input` | string | **required** | Static switch input name |
| `value` | any | **required** | Switch value: bool (`true`/`false`), enum value name (string), or int |

---

#### `niagara.create_module_from_hlsl`

Create a standalone `UNiagaraScript` asset (module usage) with a CustomHlsl node and typed ParameterMap I/O pins. Supports CPU and GPU sim targets.

Input/output pin names must be bare identifiers — no dots. Dotted names like `Module.Color` are rejected; use `Color`.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Destination package path for the new script asset (e.g. `/Game/VFX/Modules/MyModule`) |
| `hlsl` | string | **required** | HLSL body. ParameterMap is handled automatically — write only the logic inside the function body |
| `inputs` | array | optional | Input pin declarations. Each entry: `{ "name": "InValue", "type": "float" }` |
| `outputs` | array | optional | Output pin declarations. Each entry: `{ "name": "OutResult", "type": "float" }` |
| `sim_target` | string | optional | `"cpu"` (default) or `"gpu"` |

**Example:**
```json
{
  "asset_path": "/Game/VFX/Modules/ScaleByAge",
  "hlsl": "float Age = Particles.NormalizedAge;\nParticles.Scale = float3(1,1,1) * (1.0 - Age);",
  "sim_target": "cpu"
}
```

---

#### `niagara.create_function_from_hlsl`

Create a standalone `UNiagaraScript` asset (function usage) with a CustomHlsl node. Same as `create_module_from_hlsl` but with function usage context — for reusable HLSL logic called from other modules.

Input/output pin names must be bare identifiers — no dots.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Destination package path for the new script asset |
| `hlsl` | string | **required** | HLSL function body |
| `inputs` | array | optional | Input pin declarations. Each entry: `{ "name": "InValue", "type": "float" }` |
| `outputs` | array | optional | Output pin declarations. Each entry: `{ "name": "OutResult", "type": "float" }` |
| `sim_target` | string | optional | `"cpu"` (default) or `"gpu"` |

**Example:**
```json
{
  "asset_path": "/Game/VFX/Functions/Remap01",
  "hlsl": "OutValue = saturate((InValue - InMin) / (InMax - InMin));",
  "inputs": [
    { "name": "InValue", "type": "float" },
    { "name": "InMin",   "type": "float" },
    { "name": "InMax",   "type": "float" }
  ],
  "outputs": [
    { "name": "OutValue", "type": "float" }
  ]
}
```

---

### Parameter Actions

#### `niagara.get_all_parameters`

Get all parameters in a system.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `system_path` | string | **required** | Package path of the Niagara system |

---

#### `niagara.get_user_parameters`

Get user-exposed parameters.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `system_path` | string | **required** | Package path of the Niagara system |

---

#### `niagara.get_parameter_value`

Get a parameter value.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `system_path` | string | **required** | Package path of the Niagara system |
| `parameter` | string | **required** | Parameter name (with or without `"User."` prefix) |

---

#### `niagara.get_parameter_type`

Get info about a Niagara type.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `type` | string | **required** | Niagara type name (e.g. `"float"`, `"FVector3f"`) |

---

#### `niagara.trace_parameter_binding`

Trace where a parameter is used.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `system_path` | string | **required** | Package path of the Niagara system |
| `parameter` | string | **required** | Parameter name |

---

#### `niagara.add_user_parameter`

Add a user parameter.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `system_path` | string | **required** | Package path of the Niagara system |
| `name` | string | **required** | Parameter name |
| `type` | string | **required** | Niagara type |
| `default` | any | optional | Default value |

---

#### `niagara.remove_user_parameter`

Remove a user parameter.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `system_path` | string | **required** | Package path of the Niagara system |
| `name` | string | **required** | Parameter name (with or without `"User."` prefix) |

---

#### `niagara.set_parameter_default`

Set a parameter default value.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `system_path` | string | **required** | Package path of the Niagara system |
| `parameter` | string | **required** | Parameter name (with or without `"User."` prefix) |
| `value` | any | optional | New default value |

---

#### `niagara.set_curve_value`

Set curve keys on a module input.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `system_path` | string | **required** | Package path of the Niagara system |
| `emitter` | string | **required** | Emitter handle ID |
| `module` | string | **required** | Module name |
| `input` | string | **required** | Input name |
| `keys` | string | **required** | JSON array of curve keys (`{ "time", "value", "arrive_tangent"?, "leave_tangent"? }`) |

---

### Renderer Actions

#### `niagara.add_renderer`

Add a renderer to an emitter.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `system_path` | string | **required** | Package path of the Niagara system |
| `emitter` | string | **required** | Emitter handle ID |
| `class` | string | **required** | Renderer class (`"Sprite"`, `"Mesh"`, `"Ribbon"`, `"Light"`, `"Component"`) |

---

#### `niagara.remove_renderer`

Remove a renderer from an emitter.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `system_path` | string | **required** | Package path of the Niagara system |
| `emitter` | string | **required** | Emitter handle ID |
| `renderer_index` | number | **required** | Renderer index |

---

#### `niagara.set_renderer_material`

Set renderer material.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `system_path` | string | **required** | Package path of the Niagara system |
| `emitter` | string | **required** | Emitter handle ID |
| `renderer_index` | number | **required** | Renderer index |
| `material` | string | **required** | Material asset path |

---

#### `niagara.set_renderer_property`

Set a renderer property.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `system_path` | string | **required** | Package path of the Niagara system |
| `emitter` | string | **required** | Emitter handle ID |
| `renderer_index` | number | **required** | Renderer index |
| `property` | string | **required** | Property name |
| `value` | any | optional | Property value |

---

#### `niagara.get_renderer_bindings`

Get renderer attribute bindings.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `system_path` | string | **required** | Package path of the Niagara system |
| `emitter` | string | **required** | Emitter handle ID |
| `renderer_index` | number | **required** | Renderer index |

---

#### `niagara.set_renderer_binding`

Set a renderer attribute binding.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `system_path` | string | **required** | Package path of the Niagara system |
| `emitter` | string | **required** | Emitter handle ID |
| `renderer_index` | number | **required** | Renderer index |
| `binding_name` | string | **required** | Binding property name |
| `attribute` | string | **required** | Attribute path to bind |

---

### Batch & Utility Actions

#### `niagara.batch_execute`

Execute multiple operations in one transaction.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `system_path` | string | **required** | Package path of the Niagara system |
| `operations` | string | **required** | JSON array of operation objects, each with `"op"` field and action-specific params |

---

#### `niagara.create_system_from_spec`

Create a full system from JSON spec.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `spec` | string | **required** | JSON specification with `save_path`, optional `template`, `user_parameters`, `emitters` |

---

#### `niagara.get_di_functions`

Get data interface function signatures.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `di_class` | string | **required** | Data interface class name |

---

#### `niagara.get_compiled_gpu_hlsl`

Get compiled GPU HLSL for an emitter.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `system_path` | string | **required** | Package path of the Niagara system |
| `emitter` | string | **required** | Emitter handle ID (must be a GPU emitter) |

---

### Dynamic Input Actions (new in v0.10.0)

#### `niagara.add_dynamic_input`

Add a dynamic input to a module's input slot.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the Niagara system |
| `emitter` | string | **required** | Emitter handle ID |
| `module` | string | **required** | Module name |
| `input` | string | **required** | Input name to attach the dynamic input to |
| `dynamic_input` | string | **required** | Dynamic input script name or path |

---

#### `niagara.remove_dynamic_input`

Remove a dynamic input from a module's input slot.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the Niagara system |
| `emitter` | string | **required** | Emitter handle ID |
| `module` | string | **required** | Module name |
| `input` | string | **required** | Input name to remove the dynamic input from |

---

#### `niagara.set_dynamic_input_value`

Set a value on a dynamic input module.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the Niagara system |
| `emitter` | string | **required** | Emitter handle ID |
| `module` | string | **required** | Module name |
| `input` | string | **required** | Input name on the dynamic input |
| `value` | any | **required** | Value to set |

---

#### `niagara.get_dynamic_input_info`

Get info about dynamic inputs attached to a module.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the Niagara system |
| `emitter` | string | **required** | Emitter handle ID |
| `module` | string | **required** | Module name |

---

#### `niagara.search_dynamic_inputs`

Search available dynamic input scripts by keyword.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `query` | string | **required** | Search keyword (supports multi-word) |

---

### Event Handler Actions (new in v0.10.0)

#### `niagara.add_event_handler`

Add an event handler stage to an emitter.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the Niagara system |
| `emitter` | string | **required** | Emitter handle ID |
| `event_name` | string | **required** | Event name to handle |
| `source_emitter` | string | optional | Source emitter for the event |

---

#### `niagara.remove_event_handler`

Remove an event handler from an emitter.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the Niagara system |
| `emitter` | string | **required** | Emitter handle ID |
| `handler_index` | number | **required** | Index of the event handler to remove |

---

#### `niagara.list_event_handlers`

List all event handlers on an emitter.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the Niagara system |
| `emitter` | string | **required** | Emitter handle ID |

---

### Simulation Stage Actions (new in v0.10.0)

#### `niagara.add_simulation_stage`

Add a simulation stage to a GPU emitter.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the Niagara system |
| `emitter` | string | **required** | Emitter handle ID |
| `stage_name` | string | **required** | Name for the new simulation stage |

---

#### `niagara.remove_simulation_stage`

Remove a simulation stage from an emitter.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the Niagara system |
| `emitter` | string | **required** | Emitter handle ID |
| `stage_index` | number | **required** | Index of the simulation stage to remove |

---

#### `niagara.list_simulation_stages`

List all simulation stages on an emitter.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the Niagara system |
| `emitter` | string | **required** | Emitter handle ID |

---

### NPC System Actions (new in v0.10.0)

#### `niagara.create_npc_system`

Create a Niagara system configured for NPC particle behaviors.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `save_path` | string | **required** | Package path to save the new system |
| `template` | string | optional | NPC template name |

---

#### `niagara.add_npc_behavior` / `niagara.get_npc_info` / `niagara.set_npc_property` / `niagara.list_npc_templates`

NPC particle system management actions. Use `monolith_discover("niagara")` for full param schemas.

---

### Effect Type Actions (new in v0.10.0)

#### `niagara.create_effect_type` / `niagara.get_effect_type_info` / `niagara.set_effect_type_property`

Effect type CRUD. Use `monolith_discover("niagara")` for full param schemas.

---

### Renderer Helper Actions (new in v0.10.0)

#### `niagara.list_available_renderers`

List all available renderer types.

*No parameters.*

---

#### `niagara.set_renderer_mesh`

Set the mesh on a mesh renderer.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the Niagara system |
| `emitter` | string | **required** | Emitter handle ID |
| `renderer_index` | number | **required** | Renderer index |
| `mesh` | string | **required** | Package path of the static mesh asset |

---

#### `niagara.configure_ribbon` / `niagara.configure_subuv`

Configure ribbon renderer or SubUV settings. Use `monolith_discover("niagara")` for full param schemas.

---

### Advanced Niagara Actions (new in v0.10.0)

#### `niagara.diff_systems`

Diff two Niagara systems side-by-side.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `system_a` | string | **required** | Package path of the first system |
| `system_b` | string | **required** | Package path of the second system |

---

#### `niagara.save_emitter_as_template` / `niagara.clone_module_overrides` / `niagara.preview_system` / `niagara.get_available_parameters` / `niagara.get_module_output_parameters` / `niagara.rename_emitter` / `niagara.get_emitter_property`

Advanced system management actions. Use `monolith_discover("niagara")` for full param schemas.

---

## editor

Live Coding build management and editor log capture.

### `editor.trigger_build`

Trigger a Live Coding compile.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `wait` | bool | optional | Block until compile finishes |

---

### `editor.live_compile`

Trigger a Live Coding compile (alias for trigger_build).

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `wait` | bool | optional | Block until compile finishes |

---

### `editor.get_build_errors`

Get build errors and warnings from the last compile.

*No parameters.*

---

### `editor.get_build_status`

Check if a build is currently in progress.

*No parameters.*

---

### `editor.get_build_summary`

Get summary of last build (errors, warnings, time).

*No parameters.*

---

### `editor.search_build_output`

Search build log output by pattern.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `pattern` | string | **required** | Search pattern |
| `limit` | number | optional | Maximum results |

---

### `editor.get_recent_logs`

Get recent editor log entries.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `count` | number | optional | Number of entries to return |

---

### `editor.search_logs`

Search log entries by category, verbosity, and text pattern.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `pattern` | string | **required** | Text pattern to search |
| `category` | string | **required** | Log category filter |
| `verbosity` | string | **required** | Max verbosity level (`"fatal"`, `"error"`, `"warning"`, `"log"`, `"verbose"`) |
| `limit` | number | optional | Maximum results |

---

### `editor.tail_log`

Get last N log lines.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `count` | number | optional | Number of lines to return |

---

### `editor.get_log_categories`

List active log categories.

*No parameters.*

---

### `editor.get_log_stats`

Get log statistics by verbosity level.

*No parameters.*

---

### `editor.get_compile_output`

Get structured compile report: result, time, log lines from compile categories (LogLiveCoding, LogCompile, LogLinker), error/warning counts, patch status. Time-windowed to the last compile event via OnPatchComplete delegate.

*No parameters.*

**Returns:** `last_result`, `last_compile_time`, `last_compile_end_time`, `patch_applied`, `compiling`, `error_count`, `warning_count`, `log_line_count`, `compile_log` (array of log entries).

---

### `editor.get_crash_context`

Get last crash/ensure context information.

*No parameters.*

---

## config

INI config file inspection and search across the full Unreal Engine config hierarchy.

### `config.resolve_setting`

Get effective value of a config key across the full INI hierarchy.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `file` | string | **required** | Config category (e.g. `"Engine"`, `"Game"`, `"Input"`) |
| `section` | string | **required** | INI section (e.g. `"/Script/Engine.GarbageCollectionSettings"`) |
| `key` | string | **required** | Config key name |

---

### `config.explain_setting`

Show where a config value comes from across Base->Default->User layers.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `file` | string | **required** | Config category |
| `section` | string | **required** | INI section |
| `key` | string | **required** | Config key name |
| `setting` | string | optional | Shortcut: search for this key across common categories (instead of file/section/key) |

---

### `config.diff_from_default`

Show project config overrides vs engine defaults for a category.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `file` | string | **required** | Config category |
| `section` | string | optional | Filter to a specific section |

---

### `config.search_config`

Full-text search across all config files.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `query` | string | **required** | Search text |
| `file` | string | optional | Filter to a specific config category |

---

### `config.get_section`

Read an entire config section from a specific file.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `file` | string | **required** | Config file short name (e.g. `"DefaultEngine"`, `"BaseEngine"`) |
| `section` | string | **required** | INI section name |

---

### `config.get_config_files`

List all config files with their hierarchy level.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `category` | string | optional | Filter to a specific config category |

---

## project

Project-wide asset index powered by SQLite with FTS5 full-text search. Requires `bEnableIndex` in Monolith settings.

### `project.search`

Full-text search across all indexed project assets, nodes, variables, and parameters.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `query` | string | **required** | FTS5 search query (supports `AND`, `OR`, `NOT`, `prefix*`) |
| `limit` | number | optional | Maximum results. Default: `50` |

---

### `project.find_references`

Find all assets that reference or are referenced by the given asset.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the asset (e.g. `/Game/Characters/BP_Hero`) |

---

### `project.find_by_type`

Find all assets of a given type (e.g. Blueprint, Material, StaticMesh).

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_type` | string | **required** | Asset class name (e.g. `Blueprint`, `Material`, `StaticMesh`, `Texture2D`) |
| `limit` | number | optional | Maximum results. Default: `100` |
| `offset` | number | optional | Pagination offset. Default: `0` |

---

### `project.get_stats`

Get project index statistics -- total counts by table and asset class breakdown.

*No parameters.*

---

### `project.get_asset_details`

Get deep details for a specific asset -- nodes, variables, parameters, dependencies.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the asset (e.g. `/Game/Characters/BP_Hero`) |

---

## source

Unreal Engine C++ source code navigation powered by a pre-built SQLite index.

### `source.read_source`

Get the implementation source code for a class, function, or struct.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `symbol` | string | **required** | Symbol name (class, function, or struct) |
| `include_header` | bool | optional | Include header declaration. Default: `true` |
| `max_lines` | number | optional | Maximum lines to return. Default: `0` (unlimited) |
| `members_only` | bool | optional | Show only member signatures (skip function bodies). Default: `false` |

---

### `source.find_references`

Find all usage sites of a symbol (calls, includes, type references).

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `symbol` | string | **required** | Symbol name |
| `ref_kind` | string | optional | Filter by reference kind |
| `limit` | number | optional | Maximum results. Default: `50` |

---

### `source.find_callers`

Find all functions that call the given function.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `symbol` | string | **required** | Function name |
| `limit` | number | optional | Maximum results. Default: `50` |

---

### `source.find_callees`

Find all functions called by the given function.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `symbol` | string | **required** | Function name |
| `limit` | number | optional | Maximum results. Default: `50` |

---

### `source.search_source`

Full-text search across Unreal Engine source code and shaders.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `query` | string | **required** | Search query |
| `scope` | string | optional | Search scope. Default: `"all"` |
| `limit` | number | optional | Maximum results. Default: `20` |
| `mode` | string | optional | Search mode (`"fts"` or `"regex"`). Default: `"fts"` |
| `module` | string | optional | Filter to a specific module |
| `path_filter` | string | optional | Filter by file path substring |
| `symbol_kind` | string | optional | Filter by symbol kind |

---

### `source.get_class_hierarchy`

Show the inheritance tree for a class.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `class_name` | string | **required** | Class name |
| `direction` | string | optional | `"up"`, `"down"`, or `"both"`. Default: `"both"` |
| `depth` | number | optional | Hierarchy depth. Default: `1` |

---

### `source.get_module_info`

Get module statistics: file count, symbol counts by kind, and key classes.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `module_name` | string | **required** | Module name |

---

### `source.get_symbol_context`

Get a symbol definition with surrounding context lines.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `symbol` | string | **required** | Symbol name |
| `context_lines` | number | optional | Number of context lines. Default: `20` |

---

### `source.read_file`

Read source lines from a file by path.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `file_path` | string | **required** | File path (relative to engine or absolute) |
| `start_line` | number | optional | Start line. Default: `1` |
| `end_line` | number | optional | End line. Default: `0` (end of file) |

---

### `source.trigger_reindex`

Trigger Python indexer to rebuild the engine source DB.

*No parameters.*

---

## ui

UI widget Blueprint CRUD, templates, styling, animation, settings scaffolding, and accessibility. 42 actions covering the full UMG widget pipeline.

> For full param schemas, call `monolith_discover("ui")` at runtime.

The UI module provides actions for creating and editing Widget Blueprints, managing widget hierarchies, configuring styling and animations, scaffolding settings menus, and implementing accessibility features. Actions include widget CRUD, slot configuration, brush/style management, widget animation keyframing, settings generation from config, and accessibility annotation.

Use `monolith_discover("ui")` for the complete action list with parameter schemas.

---

## mesh

Mesh inspection, scene manipulation, spatial queries, level blockout, GeometryScript operations, horror/accessibility features, lighting, audio/acoustics, performance, decals, level design, tech art, context-aware props, procedural geometry, blueprint prefabs, genre presets, encounter design, and accessibility reports. **242 actions total** (197 core + 45 experimental town gen).

> **New in v0.11.0.** For full param schemas, call `monolith_discover("mesh")` at runtime.

The mesh module is the largest in Monolith, covering the full level authoring pipeline from blockout primitives to furnished interiors. Core actions (197) are always registered. The 45 experimental Procedural Town Generator actions (floor plans, facades, roofs, city blocks, spatial registry, terrain adaptation, room furnishing, debug views) are disabled by default via `bEnableProceduralTownGen` in Editor Preferences — known geometry issues with wall alignment and room separation.

**Action categories (core):** mesh inspection, primitive creation, batch operations, import/export, scene hierarchy, spatial queries, level blockout, GeometryScript ops, lighting (point/spot/rect/sky/HDRI), audio/acoustics (emitters, attenuation, reverb, occlusion), performance (HLOD, Nanite, LOD, draw call), decals, level design (volumes, sublevels, streaming), tech art (import, LOD config, texel density, collision), context-aware props (surface scatter, disturbance, physics), procedural geometry (parametric furniture, structures, mazes, terrain, sweep walls, auto-collision, proc mesh caching), blueprint prefabs, genre presets (horror, survival), encounter design, accessibility reports.

**Action categories (experimental town gen):** `generate_floor_plan`, `create_building_from_grid`, `generate_facade`, `generate_roof`, `create_city_block`, `register_building`, `query_spatial_registry`, `create_auto_volumes`, `adapt_terrain`, `generate_arch_features`, `furnish_room`, `validate_building`, debug views, and more.

Use `monolith_discover("mesh")` for the complete action list with parameter schemas.

---

## gas

Gameplay Ability System integration. **130 actions** across 10 categories covering the full GAS authoring pipeline. Conditional on `#if WITH_GBA` — requires GameplayAbilities plugin.

> **New in v0.11.0.** For full param schemas, call `monolith_discover("gas")` at runtime.

The GAS module provides complete CRUD and configuration for all GAS asset types, plus runtime inspection in PIE. Categories:

| Category | Actions | Description |
|----------|---------|-------------|
| Scaffold | 6 | Bootstrap GAS foundation, validate setup, scaffold tag hierarchies, scaffold damage pipeline |
| Attributes | 20 | Attribute set CRUD (Blueprint + C++ modes), templates, get/list/add attributes, clamping, validation |
| Effects | 26 | Gameplay Effect CRUD, modifiers, components, templates, stacking, duration, period, validate, duplicate |
| Abilities | 28 | Gameplay Ability CRUD, tags, activation policy, costs, cooldowns, info, compile, templates, find by tag |
| Graph Building | — | Add commit/end flow, ability task nodes, effect application, get ability graph flow |
| ASC | 14 | Add ASC to actor, configure ASC, validate ASC setup, granted abilities, active effects, replication mode |
| Cues | 10 | Create gameplay cue notify (static + actor), link cue to effect, validate cue coverage, cue params |
| Tags | 10 | Add gameplay tags, search tag usage, validate tag consistency, rename tag, tag hierarchy/matching |
| Targeting | 5 | Target data handles, actor selection, confirmation |
| Input | 5 | Enhanced Input binding, input tags |
| Inspect (PIE) | 6 | Runtime inspection: get all ASCs, ASC snapshot, GAS state snapshot, get/set attribute value, apply effect |

Use `monolith_discover("gas")` for the complete action list with parameter schemas.
