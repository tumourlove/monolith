# Monolith — MonolithBlueprint Module

**Parent:** [SPEC_CORE.md](../SPEC_CORE.md)
**Engine:** Unreal Engine 5.7+
**Version:** 0.14.1 (Beta)

---

## MonolithBlueprint

**Dependencies:** Core, CoreUObject, Engine, MonolithCore, UnrealEd, BlueprintGraph, Json, JsonUtilities

### Classes

| Class | Responsibility |
|-------|---------------|
| `FMonolithBlueprintModule` | Registers 88 blueprint actions |
| `FMonolithBlueprintActions` | Static handlers. Uses `FMonolithAssetUtils::LoadAssetByPath<UBlueprint>` |
| `MonolithBlueprintInternal` | Helpers: AddGraphArray, FindGraphByName, PinTypeToString, SerializePin/Node, TraceExecFlow, FindEntryNode |

### Actions (88 — namespace: "blueprint")

**Read Actions (13)**
| Action | Params | Description |
|--------|--------|-------------|
| `list_graphs` | `asset_path` | List all graphs with name/type/node_count. Graph types: event_graph, function, macro, delegate_signature |
| `get_graph_summary` | `asset_path`, `graph_name` | Lightweight graph overview: node id/class/title + exec connections only (~10KB vs 172KB for full data) |
| `get_graph_data` | `asset_path`, `graph_name`, `node_class_filter` | Full graph with all nodes, pins (17+ type categories), connections, positions. Optional class filter |
| `get_variables` | `asset_path` | All NewVariables: name, type (with container prefix), default (from CDO), category, flags (editable, read_only, expose_on_spawn, replicated, transient) |
| `get_execution_flow` | `asset_path`, `entry_point` | Linearized exec trace from entry point. Handles branching (multiple exec outputs). MaxDepth=100 |
| `search_nodes` | `asset_path`, `query` | Case-insensitive search by title, class name, or function name |
| `get_components` | `asset_path` | List all components in the component hierarchy |
| `get_component_details` | `asset_path`, `component_name` | Full property reflection for a named component |
| `get_functions` | `asset_path` | List all functions with signatures, access, and purity flags |
| `get_event_dispatchers` | `asset_path` | List all event dispatchers with parameter signatures |
| `get_parent_class` | `asset_path` | Return the parent class of the Blueprint |
| `get_interfaces` | `asset_path` | List all implemented interfaces |
| `get_construction_script` | `asset_path` | Get the construction script graph |

**Variable CRUD (7)**
| Action | Params | Description |
|--------|--------|-------------|
| `add_variable` | `asset_path`, `variable_name`, `variable_type` | Add a new variable to the Blueprint |
| `remove_variable` | `asset_path`, `variable_name` | Remove a variable by name |
| `rename_variable` | `asset_path`, `old_name`, `new_name` | Rename a variable |
| `set_variable_type` | `asset_path`, `variable_name`, `variable_type` | Change a variable's type |
| `set_variable_defaults` | `asset_path`, `variable_name`, `default_value` | Set a variable's default value |
| `add_local_variable` | `asset_path`, `function_name`, `variable_name`, `variable_type` | Add a local variable inside a function graph |
| `remove_local_variable` | `asset_path`, `function_name`, `variable_name` | Remove a local variable from a function graph |

**Component CRUD (6)**
| Action | Params | Description |
|--------|--------|-------------|
| `add_component` | `asset_path`, `component_class`, `component_name` | Add a component to the Blueprint |
| `remove_component` | `asset_path`, `component_name` | Remove a component by name |
| `rename_component` | `asset_path`, `old_name`, `new_name` | Rename a component |
| `reparent_component` | `asset_path`, `component_name`, `new_parent` | Change a component's parent in the hierarchy |
| `set_component_property` | `asset_path`, `component_name`, `property_name`, `value` | Set a property on a component via reflection |
| `duplicate_component` | `asset_path`, `component_name`, `new_name` | Duplicate a component with all its settings |

**Graph Management (9)**
| Action | Params | Description |
|--------|--------|-------------|
| `add_function` | `asset_path`, `function_name` | Add a new function graph |
| `remove_function` | `asset_path`, `function_name` | Remove a function graph |
| `rename_function` | `asset_path`, `old_name`, `new_name` | Rename a function graph |
| `add_macro` | `asset_path`, `macro_name` | Add a new macro graph |
| `add_event_dispatcher` | `asset_path`, `dispatcher_name` | Add a new event dispatcher |
| `set_function_params` | `asset_path`, `function_name`, `params` | Set input/output parameters on a function |
| `implement_interface` | `asset_path`, `interface_class` | Add an interface to the Blueprint |
| `remove_interface` | `asset_path`, `interface_class` | Remove an interface from the Blueprint |
| `reparent_blueprint` | `asset_path`, `new_parent_class` | Change the Blueprint's parent class |

**Node & Pin Operations (6)**
| Action | Params | Description |
|--------|--------|-------------|
| `add_node` | `asset_path`, `graph_name`, `node_class`, `position` | Add a node to a graph. Accepts common aliases (e.g. `CallFunction`, `VariableGet`) and tries `K2_` prefix fallback for function calls |
| `remove_node` | `asset_path`, `graph_name`, `node_id` | Remove a node by ID |
| `connect_pins` | `asset_path`, `graph_name`, `source_node`, `source_pin`, `target_node`, `target_pin` | Connect two pins |
| `disconnect_pins` | `asset_path`, `graph_name`, `source_node`, `source_pin`, `target_node`, `target_pin` | Disconnect two pins |
| `set_pin_default` | `asset_path`, `graph_name`, `node_id`, `pin_name`, `default_value` | Set a pin's default value |
| `set_node_position` | `asset_path`, `graph_name`, `node_id`, `x`, `y` | Set a node's position in the graph |

**Compile & Create (5)**
| Action | Params | Description |
|--------|--------|-------------|
| `compile_blueprint` | `asset_path` | Compile the Blueprint and return errors/warnings |
| `validate_blueprint` | `asset_path` | Validate Blueprint without full compile — checks for broken references and missing overrides |
| `create_blueprint` | `save_path`, `parent_class` | Create a new Blueprint asset |
| `duplicate_blueprint` | `asset_path`, `new_path` | Duplicate a Blueprint asset to a new path |
| `get_dependencies` | `asset_path` | List all hard and soft asset dependencies |

**Layout (1)**
| Action | Params | Description |
|--------|--------|-------------|
| `auto_layout` | `asset_path`, `graph_name`?, `formatter`? | Auto-arrange nodes in a Blueprint graph. `formatter`: `"auto"` (default) — uses Blueprint Assist if available, falls back to built-in hierarchical layout; `"blueprint_assist"` — requires BA, errors if not present; `"builtin"` — built-in layout only |

**Spawn (2)**
| Action | Params | Description |
|--------|--------|-------------|
| `spawn_blueprint_actor` | `blueprint`, `location`?, `rotation`?, `scale`?, `label`?, `folder`?, `properties`?, `tags`?, `sublevel`?, `mobility`?, `select`? | Spawn a Blueprint actor into the editor world with full transform, property reflection, tags, sublevel targeting, and mobility control. Uses `GEditor->AddActor` for proper editor integration (undo/redo). Default folder: `"Blueprints"` |
| `batch_spawn_blueprint_actors` | `blueprint`, `count`, `pattern`?, `origin`?, `spacing`?, `columns`?, `direction`?, `rotation`?, `scale`?, `label_prefix`?, `folder`?, `properties`?, `tags`?, `sublevel`?, `mobility`?, `select`? | Spawn multiple Blueprint actors in a grid or linear pattern. Partial failure semantics — continues on per-actor failure, reports successes and failures separately. Single undo transaction. Max 1000 |

---
