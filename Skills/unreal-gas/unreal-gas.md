---
name: unreal-gas
description: Use when working with Unreal Engine Gameplay Ability System (GAS) via Monolith MCP — creating and editing abilities, attribute sets, gameplay effects, ASC setup, gameplay tags, gameplay cues, targeting, input binding, runtime inspection, and project scaffolding. Triggers on GAS, ability, attribute, gameplay effect, gameplay tag, gameplay cue, ASC, ability system, cooldown, modifier, stacking, ability task.
---

# Unreal GAS Workflows

You have access to **Monolith** with **130 GAS actions** across 10 categories via `gas_query()`.

## Discovery

Always discover available actions first:
```
monolith_discover({ namespace: "gas" })
```

## Key Parameter Names

- `asset_path` — the Blueprint or asset path (NOT `asset`). For Gameplay Effects, Abilities, Attribute Sets
- `attribute_set` — attribute set asset path (for attribute operations)
- `effect_path` — gameplay effect asset path (for effect operations when distinct from `asset_path`)
- `ability_path` — gameplay ability asset path
- `tag` — gameplay tag string (e.g., `"Ability.Combat.Attack"`)
- `template` — template name (from `list_templates` or documented below)

## Action Reference

### Abilities (28)

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `create_ability` | `save_path`, `parent_class`?, `ability_name`? | Create a new Gameplay Ability Blueprint |
| `get_ability_info` | `asset_path` | Read ability details: tags, costs, cooldowns, flags |
| `list_abilities` | `path_filter`?, `tag_filter`? | List all GA assets in project |
| `compile_ability` | `asset_path` | Compile a Gameplay Ability Blueprint |
| `set_ability_tags` | `asset_path`, `tags` | Set ability tags (cancel, block, owned, required) |
| `get_ability_tags` | `asset_path` | Read all tag containers on an ability |
| `set_ability_policy` | `asset_path`, `instancing`?, `net_execution`? | Set instancing and net execution policies |
| `set_ability_cost` | `asset_path`, `effect_path` | Assign a cost Gameplay Effect |
| `set_ability_cooldown` | `asset_path`, `effect_path` | Assign a cooldown Gameplay Effect |
| `set_ability_triggers` | `asset_path`, `triggers` | Configure ability trigger events |
| `set_ability_flags` | `asset_path`, flags | Set flags: server-only, retry on activation fail, etc. |
| `add_ability_task_node` | `asset_path`, `task_class`, `graph_name`? | Add an Ability Task node to ability graph |
| `add_commit_and_end_flow` | `asset_path`, `graph_name`? | Scaffold CommitAbility -> logic -> EndAbility flow |
| `add_effect_application` | `asset_path`, `effect_class`, `target`? | Add ApplyGameplayEffectToSelf/Target node |
| `add_gameplay_cue_node` | `asset_path`, `cue_tag`, `type`? | Add ExecuteGameplayCue or AddGameplayCueWithParams |
| `create_ability_from_template` | `save_path`, `template` | Create ability from a preset template |
| `build_ability_from_spec` | `save_path`, `spec` | Declarative one-shot ability builder (nodes + connections) |
| `batch_create_abilities` | `abilities` | Create multiple abilities at once |
| `duplicate_ability` | `asset_path`, `new_path` | Duplicate an ability to a new path |
| `list_ability_tasks` | `class_filter`? | List available Ability Task classes |
| `get_ability_task_pins` | `task_class` | Get input/output pins for an Ability Task |
| `wire_ability_task_delegate` | `asset_path`, `task_node_id`, `delegate_name`, `target_node`? | Wire an AT delegate output |
| `get_ability_graph_flow` | `asset_path`, `graph_name`? | Trace execution flow through ability graph |
| `validate_ability` | `asset_path` | Lint ability: missing cost, orphaned tasks, tag conflicts |
| `find_abilities_by_tag` | `tag`, `match_type`? | Find abilities by gameplay tag |
| `get_ability_tag_matrix` | `path_filter`? | Cross-reference tag usage across all abilities |
| `validate_ability_blueprint` | `asset_path` | Deep validation: graph integrity, task wiring, delegates |
| `scaffold_custom_ability_task` | `save_path`, `task_name`, `delegates`?, `params`? | Scaffold a custom AbilityTask subclass Blueprint |

### Attributes (20)

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `create_attribute_set` | `save_path`, `parent_class`?, `attributes`? | Create a new Attribute Set Blueprint |
| `add_attribute` | `asset_path`, `attribute_name`, `type`?, `default`? | Add attribute to set |
| `get_attribute_set` | `asset_path` | Read attribute set: attributes, defaults, clamping |
| `set_attribute_defaults` | `asset_path`, `defaults` | Set default values for attributes |
| `list_attribute_sets` | `path_filter`? | List all attribute set assets |
| `configure_attribute_clamping` | `asset_path`, `attribute`, `min`?, `max`?, `clamp_source`? | Set attribute clamping rules |
| `configure_meta_attributes` | `asset_path`, `meta_attribute`, `target_attribute`, `operation` | Set up meta attribute for damage/heal pipeline |
| `create_attribute_set_from_template` | `save_path`, `template` | Create attribute set from preset template |
| `create_attribute_init_datatable` | `save_path`, `attribute_set`, `rows`? | Create DataTable for attribute initialization |
| `duplicate_attribute_set` | `asset_path`, `new_path` | Duplicate attribute set |
| `configure_attribute_replication` | `asset_path`, `attribute`, `replicate`?, `condition`? | Set replication for individual attributes |
| `link_datatable_to_asc` | `asc_asset`, `datatable_path` | Link init DataTable to ASC defaults |
| `bulk_edit_attributes` | `asset_path`, `edits` | Edit multiple attributes in one call |
| `validate_attribute_set` | `asset_path` | Lint: orphan attributes, missing clamping, replication gaps |
| `find_attribute_modifiers` | `attribute` | Find all GEs that modify a specific attribute |
| `diff_attribute_sets` | `asset_path_a`, `asset_path_b` | Compare two attribute sets |
| `get_attribute_dependency_graph` | `asset_path` | Map attribute dependencies (meta, clamping, derived) |
| `remove_attribute` | `asset_path`, `attribute_name` | Remove attribute from set |
| `get_attribute_value` | `actor_path`, `attribute` | Read runtime attribute value (PIE only) |
| `set_attribute_value` | `actor_path`, `attribute`, `value` | Set runtime attribute value (PIE only) |

### Effects (26)

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `create_gameplay_effect` | `save_path`, `duration_policy`?, `parent_class`? | Create a new Gameplay Effect Blueprint |
| `get_gameplay_effect` | `asset_path` | Read full GE details: modifiers, duration, stacking, components |
| `list_gameplay_effects` | `path_filter`?, `duration_filter`? | List all GE assets |
| `add_modifier` | `asset_path`, `attribute`, `operation`, `magnitude`? | Add attribute modifier |
| `set_modifier` | `asset_path`, `index`, `attribute`?, `operation`?, `magnitude`? | Edit existing modifier by index |
| `remove_modifier` | `asset_path`, `index` | Remove modifier by index |
| `list_modifiers` | `asset_path` | List all modifiers on a GE |
| `add_ge_component` | `asset_path`, `component_class`, `config`? | Add a GE Component (5.3+ model) |
| `set_ge_component` | `asset_path`, `component_index`, `config` | Edit GE Component config |
| `set_effect_stacking` | `asset_path`, `type`, `limit`?, `duration_refresh`?, `period_reset`? | Configure stacking behavior |
| `set_duration` | `asset_path`, `policy`, `duration`? | Set duration policy (instant/duration/infinite) |
| `set_period` | `asset_path`, `period`, `execute_on_apply`? | Set periodic execution |
| `create_effect_from_template` | `save_path`, `template` | Create GE from preset template |
| `build_effect_from_spec` | `save_path`, `spec` | Declarative one-shot GE builder |
| `batch_create_effects` | `effects` | Create multiple GEs at once |
| `add_execution` | `asset_path`, `execution_class` | Add a Gameplay Effect Execution Calculation |
| `duplicate_gameplay_effect` | `asset_path`, `new_path` | Duplicate GE |
| `delete_gameplay_effect` | `asset_path` | Delete GE asset |
| `validate_effect` | `asset_path` | Lint: missing attributes, orphan modifiers, stacking conflicts |
| `get_effect_interaction_matrix` | `path_filter`? | Cross-reference all GE interactions (stacking, immunity) |
| `remove_ge_component` | `asset_path`, `component_index` | Remove a GE Component |
| `get_active_effects` | `actor_path` | List active GEs on an actor (PIE only) |
| `get_effect_modifiers_breakdown` | `asset_path` | Detailed modifier analysis with calculated values |
| `apply_effect` | `actor_path`, `effect_path`, `level`? | Apply GE to actor at runtime (PIE only) |
| `remove_effect` | `actor_path`, `handle` | Remove active GE by handle (PIE only) |
| `simulate_effect_stack` | `effect_path`, `count`?, `base_value`? | Simulate stacking results without PIE |

### ASC Setup (14)

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `add_asc_to_actor` | `asset_path`, `component_class`? | Add AbilitySystemComponent to a Blueprint |
| `configure_asc` | `asset_path`, `replication_mode`?, `avatar_actor`? | Configure ASC settings |
| `setup_asc_init` | `asset_path`, `init_location`? | Scaffold InitAbilityActorInfo call |
| `setup_ability_system_interface` | `asset_path` | Implement IAbilitySystemInterface (C++ required) |
| `apply_asc_template` | `asset_path`, `template` | Apply full ASC template (player, AI, boss) |
| `set_default_abilities` | `asset_path`, `abilities` | Set abilities granted on init |
| `set_default_effects` | `asset_path`, `effects` | Set effects applied on init |
| `set_default_attribute_sets` | `asset_path`, `attribute_sets` | Set attribute sets created on init |
| `set_asc_replication_mode` | `asset_path`, `mode` | Full/Minimal/Mixed replication |
| `validate_asc_setup` | `asset_path` | Lint: missing interface, init call, avatar mapping |
| `grant_ability` | `actor_path`, `ability_class`, `level`? | Grant ability at runtime (PIE only) |
| `revoke_ability` | `actor_path`, `ability_class` | Revoke ability at runtime (PIE only) |
| `get_asc_snapshot` | `actor_path` | Full ASC state dump: abilities, effects, tags (PIE only) |
| `get_all_ascs` | — | List all actors with ASCs in current level |

### Tags (10)

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `add_gameplay_tags` | `tags`, `source`? | Register gameplay tags (INI or DataTable) |
| `get_tag_hierarchy` | `root_tag`? | Display tag tree from root |
| `search_tag_usage` | `tag`, `search_scope`? | Find all assets referencing a tag |
| `scaffold_tag_hierarchy` | `template`? | Generate full tag hierarchy from preset |
| `rename_tag` | `old_tag`, `new_tag` | Rename tag across all assets |
| `remove_gameplay_tags` | `tags` | Remove tags from registry |
| `validate_tag_consistency` | — | Check for orphan tags, naming violations |
| `audit_tag_naming` | `path_filter`? | Audit tag naming conventions |
| `export_tag_hierarchy` | `format`? | Export tag tree as JSON/CSV |
| `import_tag_hierarchy` | `source_path`, `format`? | Import tags from file |

### Cues (10)

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `create_gameplay_cue_notify` | `save_path`, `cue_tag`, `type`? | Create GameplayCue Notify (Static or Actor) |
| `link_cue_to_effect` | `effect_path`, `cue_tag` | Add cue tag to a GE's cue list |
| `unlink_cue_from_effect` | `effect_path`, `cue_tag` | Remove cue tag from GE |
| `get_cue_info` | `asset_path` | Read cue details: tag, type, params |
| `list_gameplay_cues` | `path_filter`? | List all cue assets |
| `set_cue_parameters` | `asset_path`, `params` | Configure cue parameters (magnitude, location, etc.) |
| `find_cue_triggers` | `cue_tag` | Find all GEs and abilities that trigger a cue |
| `validate_cue_coverage` | `path_filter`? | Check for GEs missing cue notifies |
| `batch_create_cues` | `cues` | Create multiple cues at once |
| `scaffold_cue_library` | `template`?, `save_path`? | Generate a starter cue set (hit, heal, buff, debuff) |

### Targeting (5)

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `create_target_actor` | `save_path`, `type`?, `parent_class`? | Create a TargetActor Blueprint |
| `configure_target_actor` | `asset_path`, `config` | Set targeting params (range, radius, shape) |
| `add_targeting_to_ability` | `ability_path`, `target_actor_class` | Wire WaitTargetData task into ability |
| `scaffold_fps_targeting` | `save_path`, `config`? | Scaffold FPS-style line trace targeting |
| `validate_targeting` | `asset_path` | Lint targeting setup in an ability |

### Input Binding (5)

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `setup_ability_input_binding` | `asset_path`, `method`? | Configure ability input binding approach |
| `bind_ability_to_input` | `asset_path`, `ability_class`, `input_action` | Bind specific ability to Enhanced Input action |
| `batch_bind_abilities` | `asset_path`, `bindings` | Bind multiple abilities to inputs |
| `get_ability_input_bindings` | `asset_path` | List current ability-input bindings |
| `scaffold_input_binding_component` | `save_path` | Scaffold an input binding component |

### Inspect / Debug (6)

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `snapshot_gas_state` | `actor_path` | Full runtime state: attributes, effects, abilities, tags |
| `get_tag_state` | `actor_path` | Current tag container state at runtime |
| `get_cooldown_state` | `actor_path`, `ability_class`? | Active cooldown timers |
| `trace_ability_activation` | `actor_path`, `ability_class` | Step-by-step activation trace (can/cannot + why) |
| `compare_gas_states` | `actor_a`, `actor_b` | Diff GAS state between two actors |
| `export_gas_manifest` | `path_filter`?, `format`? | Export full GAS project manifest |

### Scaffold / Bootstrap (6)

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `bootstrap_gas_foundation` | `save_path`, `config`? | Full GAS bootstrap: ASC, AttributeSet, starter GEs, tags |
| `validate_gas_setup` | `path_filter`? | Project-wide GAS validation |
| `scaffold_gas_project` | `save_path`, `template`? | Scaffold complete GAS project structure |
| `scaffold_damage_pipeline` | `save_path`, `config`? | Scaffold damage pipeline: meta attributes, execution calc |
| `scaffold_status_effect` | `save_path`, `effect_name`, `config`? | Scaffold a status effect (DOT, buff, debuff) |
| `scaffold_weapon_ability` | `save_path`, `weapon_type`, `config`? | Scaffold weapon ability with targeting and cues |

## Templates

Templates are preset configurations accessible via `*_from_template` and `scaffold_*` actions.

### Survival Horror Templates (this project's genre)
- `horror_attributes` — Health, Stamina, Sanity, Horror, PainThreshold
- `horror_effects` — Bleeding, Infection, Exhaustion, Panic, Adrenaline
- `horror_abilities` — Sprint, HeavyAttack, Heal, BarricadeDoor, Flashlight
- `horror_asc_player` — Full player ASC: horror attributes, starter abilities, accessibility GEs
- `horror_asc_ai` — AI ASC: minimal attributes, aggro abilities
- `horror_tags` — Full tag hierarchy for survival horror (State, Ability, Effect, Damage, Status)

### Generic Templates
- `basic_attributes` — Health, MaxHealth, Mana, MaxMana, AttackPower, Defense
- `basic_damage_effect` — Instant damage GE with meta attribute pipeline
- `basic_heal_effect` — Instant heal GE
- `basic_dot_effect` — Duration-based damage-over-time
- `basic_buff_effect` — Duration-based stat buff with stacking
- `basic_melee_ability` — Melee attack with commit, montage, damage application
- `basic_projectile_ability` — Ranged attack with targeting and projectile spawn
- `basic_asc_player` — Player ASC on PlayerState with input binding
- `basic_asc_ai` — AI ASC on Actor with behavior tree integration

## Key Technical Notes

1. **UK2Node_LatentAbilityCall** — Ability Task nodes use `UK2Node_LatentAbilityCall`, NOT `UK2Node_CallFunction`. The `add_ability_task_node` action handles this automatically, but if using `blueprint_query("add_node")` directly, specify the correct node type.

2. **GE Component Model (5.3+)** — UE 5.3+ replaces hardcoded GE fields with modular `UGameplayEffectComponent` subclasses. Use `add_ge_component` / `set_ge_component` / `remove_ge_component` instead of legacy field setters. The tools handle both models.

3. **IAbilitySystemInterface is C++ only** — This interface cannot be implemented in Blueprint. `setup_ability_system_interface` will guide you to the C++ implementation or scaffold the header changes needed.

4. **ComboGraph globals chain** — If ComboGraph plugin is active (it is in this project), check `DefaultGame.ini` for `GlobalAbilityList` entries that auto-grant abilities. ComboGraph injects its own abilities here.

5. **GBA conditional** — Gameplay Blueprint Attributes (GBA) plugin allows Blueprint-only attribute sets. Check if installed before recommending C++-only attribute workflows.

6. **Accessibility modes** — When scaffolding abilities, consider infinite-duration GEs for accessibility modes (e.g., invulnerability, infinite stamina, reduced horror intensity). The horror templates include these by default.

## Common Workflows

### Full GAS Bootstrap (new project)
```
gas_query({ action: "bootstrap_gas_foundation", params: {
  save_path: "/Game/GAS",
  config: { template: "horror_asc_player", include_damage_pipeline: true }
}})
```

### Create Attribute Set -> Configure -> Validate
```
gas_query({ action: "create_attribute_set_from_template", params: {
  save_path: "/Game/GAS/Attributes/AS_PlayerVitals", template: "horror_attributes"
}})
gas_query({ action: "configure_attribute_clamping", params: {
  asset_path: "/Game/GAS/Attributes/AS_PlayerVitals",
  attribute: "Health", min: 0, max: "MaxHealth"
}})
gas_query({ action: "validate_attribute_set", params: {
  asset_path: "/Game/GAS/Attributes/AS_PlayerVitals"
}})
```

### Create Gameplay Effect with Modifiers
```
gas_query({ action: "create_gameplay_effect", params: {
  save_path: "/Game/GAS/Effects/GE_Damage_Bleed", duration_policy: "duration"
}})
gas_query({ action: "add_modifier", params: {
  asset_path: "/Game/GAS/Effects/GE_Damage_Bleed",
  attribute: "Health", operation: "Additive", magnitude: { type: "ScalableFloat", value: -5.0 }
}})
gas_query({ action: "set_period", params: {
  asset_path: "/Game/GAS/Effects/GE_Damage_Bleed", period: 1.0, execute_on_apply: true
}})
gas_query({ action: "set_duration", params: {
  asset_path: "/Game/GAS/Effects/GE_Damage_Bleed", policy: "has_duration", duration: 10.0
}})
```

### Create Ability with Tasks
```
gas_query({ action: "create_ability_from_template", params: {
  save_path: "/Game/GAS/Abilities/GA_HeavyAttack", template: "basic_melee_ability"
}})
gas_query({ action: "set_ability_cost", params: {
  asset_path: "/Game/GAS/Abilities/GA_HeavyAttack",
  effect_path: "/Game/GAS/Effects/GE_Cost_Stamina"
}})
gas_query({ action: "set_ability_cooldown", params: {
  asset_path: "/Game/GAS/Abilities/GA_HeavyAttack",
  effect_path: "/Game/GAS/Effects/GE_Cooldown_3s"
}})
```

### Wire Input Binding
```
gas_query({ action: "bind_ability_to_input", params: {
  asset_path: "/Game/Blueprints/BP_PlayerCharacter",
  ability_class: "/Game/GAS/Abilities/GA_HeavyAttack",
  input_action: "/Game/Input/IA_HeavyAttack"
}})
```

### Scaffold Damage Pipeline
```
gas_query({ action: "scaffold_damage_pipeline", params: {
  save_path: "/Game/GAS/DamagePipeline",
  config: { use_meta_attributes: true, damage_types: ["Physical", "Fire", "Poison"] }
}})
```

## Anti-Patterns to Validate

- **Missing InitAbilityActorInfo** — ASC won't work without it. `validate_asc_setup` catches this.
- **Cost GE with wrong duration** — Cost effects must be Instant. `validate_effect` flags duration costs.
- **Cooldown GE without duration** — Cooldown effects must have a duration. Flagged by validation.
- **Ability granted but never bound to input** — Use `validate_gas_setup` to find unbound abilities.
- **Stacking without limit** — Infinite stacking can crash. `validate_effect` warns on unlimited stacks.
- **Meta attribute without execution calc** — Meta attributes need an execution calculation to route damage. `validate_attribute_set` checks this.
- **Orphaned cue tags** — Cue tags on GEs with no matching GCN asset. `validate_cue_coverage` catches this.
- **Tag typos** — `validate_tag_consistency` finds tags used in assets but not in the tag registry.

## Tips

- **Always validate** after creating or modifying GAS assets — the validation actions catch common mistakes early.
- **Use templates** for this project's genre — the horror templates include accessibility features for players with accessibility needs.
- **GE Components** are the modern way — avoid setting legacy GE fields directly on 5.3+.
- **Batch operations** (`batch_create_abilities`, `batch_create_effects`, `batch_create_cues`) save round-trips when scaffolding.
- **Runtime actions** (get/set attribute values, apply/remove effects, grant/revoke abilities) only work during PIE.
- **`export_gas_manifest`** generates a full project GAS report — useful for audits and documentation.
