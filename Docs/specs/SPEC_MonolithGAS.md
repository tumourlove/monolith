# Monolith — MonolithGAS Module

**Parent:** [SPEC_CORE.md](../SPEC_CORE.md)
**Engine:** Unreal Engine 5.7+
**Version:** 0.14.7 (Beta)

---

## MonolithGAS

**Dependencies:** Core, CoreUObject, Engine, MonolithCore, GameplayAbilities, GameplayTags
**Namespace:** `gas` (135 actions) + 4 cross-namespace aliases into `ui` | **Tool:** `gas_query(action, params)` | **Actions:** 135 (Phase J F8: +`grant_ability_to_pawn`)
**Conditional:** GBA (Blueprint Attributes) features wrapped in `#if WITH_GBA`. Core GAS engine modules (GameplayAbilities, GameplayTags, GameplayTasks) are always available. When GBA is absent, Blueprint AttributeSet creation is disabled but all 135 actions still register and compile cleanly. When `bEnableGAS` is disabled in settings, 0 actions registered.
**Settings toggle:** `bEnableGAS` (default: True)

MonolithGAS provides full MCP coverage of the Gameplay Ability System. It covers ability CRUD, attribute set management, gameplay effect authoring, ASC (Ability System Component) inspection and manipulation, gameplay tag operations, gameplay cue management, target data, input binding, runtime inspection, scaffolding of common GAS patterns, and Widget→Attribute binding via class-extension authoring.

### Action Categories

| Category | Actions | Description |
|----------|---------|-------------|
| Abilities | 28 | Create, edit, delete, list, grant, activate, cancel, query gameplay abilities. Includes spec handles, instancing policy, tags, costs, cooldowns |
| Attributes | 20 | Create/edit attribute sets, get/set attribute values, define derived attributes, attribute initialization, clamping, replication config |
| Effects | 26 | Create/edit gameplay effects, duration policies, modifiers, executions, stacking, conditional application, period, tags granted/removed |
| ASC | 14 | Inspect/configure Ability System Components, list granted abilities, active effects, attribute values, owned tags, replication mode |
| Tags | 10 | Query gameplay tag hierarchy, check tag matches, add/remove loose tags, tag containers, tag queries |
| Cues | 10 | Create/edit gameplay cue notifies (static and actor), cue tags, cue parameters, handler lookup |
| Targets | 5 | Target data handles, target actor selection, target data confirmation, custom target data types |
| Input | 5 | Bind abilities to Enhanced Input actions, input tag mapping, activation on input |
| Inspect | 6 | Runtime inspection of active abilities, applied effects, attribute snapshots, ability task state, prediction keys |
| Scaffold | 7 | Scaffold common GAS setups: init_attribute_set, init_asc_actor, init_ability_set, init_damage_pipeline, init_cooldown_system, init_stacking_effect, **`grant_ability_to_pawn`** (Phase J F8 — author-time append to ASC startup-abilities array via reflection) |
| UI Binding | 4 | `bind_widget_to_attribute`, `unbind_widget_attribute`, `list_attribute_bindings`, `clear_widget_attribute_bindings`. Authored via `UMonolithGASAttributeBindingClassExtension`. **Also registered as aliases in the `ui` namespace** (so `ui::bind_widget_to_attribute` and `gas::bind_widget_to_attribute` dispatch to the same handler — see `MonolithGASUIBindingActions.cpp:561-577`). The `ui::` aliases are documented in [SPEC_MonolithUI.md](SPEC_MonolithUI.md) "GAS Bridge Aliases" section |

**Total:** 28 + 20 + 26 + 14 + 10 + 10 + 5 + 5 + 6 + 7 + 4 = **135**.

### Phase J fixes touching this module

- **F2 (2026-04-26)** — `gas::bind_widget_to_attribute` rejects unknown `owner_resolver` (`ParseOwner` no longer silently coerces to `OwningPlayerPawn`).
- **F3 (2026-04-26)** — `gas::bind_widget_to_attribute` rejects malformed `format=format_string` templates (new `ValidateFormatStringPayload` helper enforces `{0}` slot, `{1}` when `max_attribute` bound).
- **F5 (2026-04-26)** — Response shape & error-message drift cleanup (`index` → `binding_index`, composite `attribute`/`max_attribute` strings, `widget_class`, `removed_binding_index`, enriched valid-options enumerations).
- **F6 (2026-04-26)** — J1 spec relaxed to match impl (`warnings` omitted-when-empty, AttributeSet enumeration dropped, full-valid-list replaces Levenshtein "did you mean").
- **F8 (2026-04-26)** — `gas::grant_ability_to_pawn` added (+1).
- **F9 logging (2026-04-26)** — Observability adds + `LogMonolithGASUIBinding` / `LogMonolithGASUIBindingExt` retired into parent `LogMonolithGAS` category.

See [SPEC_CORE.md §11 Recent Fixes](../SPEC_CORE.md#recent-fixes-phase-j--shipped-in-0147) for the long-form descriptions.

### Notes

> **Runtime actions (Inspect category) require PIE.** These actions query live game state and return errors if called outside a Play-In-Editor session.
>
> **GBA conditional support:** The `WITH_GBA` define is set automatically by the module's `Build.cs` when GameplayAbilities is found. Projects without GAS get zero compile overhead — the entire module compiles to an empty stub.
>
> **UI Binding cooked-build caveat.** `UMonolithGASAttributeBindingClassExtension` is an editor-only class — content WBPs that reference it will fail to apply bindings in cooked Steam builds. See [COOKED_BUILD_TODO.md](../COOKED_BUILD_TODO.md) for the resolution path (Option A/B/C deferred to pre-Steam-launch checkpoint).

---

### Bulk Fill & Describe Surface (2026-05-11)

The `gas` namespace registers a `FMonolithBulkFillRegistry` adapter (`MonolithGASBulkFillAdapter.cpp`) routed from the central `bulk_fill_query("apply")` and `describe_query("schema")` dispatchers. Phase 2 of the MCP ergonomics rollout (design spec `Docs/plans/2026-05-11-monolith-mcp-ergonomics-design.md`, implementation plan `Docs/plans/2026-05-11-monolith-mcp-ergonomics.md`). This collapses the 20-attr × 10-level ≈ 200-call grind on AttributeInit DataTables into a single transacted call.

**Surface summary.** `bulk_fill_query("apply", target_namespace="gas", target="<asset_path>", tree={...}, dry_run=<bool>, strict=<bool>)` walks the JSON tree against the target asset's reflection schema and either commits atomically or fails with a per-row error map. `describe_query("schema", target_namespace="gas", target="<asset_path>")` returns the settable surface — for AttributeInit DataTables, the `FAttributeMetaData` row schema; for everything else, the modifier-magnitude tagged-union descriptor.

**fill_kind catalogue (1 — enumerated against `MonolithGASBulkFillAdapter.cpp`):**

| `fill_kind` | Target shape | Walks |
|---|---|---|
| `AttributeInitDataTable` | `UCurveTable` / `UDataTable` set up for `FAttributeMetaData` | `rows:{}` keyed by `[GroupName].[Level]` (per the engine's `FAttributeSetInitterDiscreteLevels` convention at `AttributeSet.h:303-318`), values are per-attribute scalars or `{base, min, max}` objects |

**H5 stub-adapter invariant:** the adapter's `Register()` call runs unconditionally from `FMonolithGASModule::StartupModule` regardless of `WITH_GBA`. The adapter BODY switches on `WITH_GBA` — the dev build wires the real handlers; release builds without GAS return a clean `"GAS optional dep not available (WITH_GBA=0)"` error. This guarantees `monolith_discover("gas")` action surface is identical across dev + release builds.

#### `bulk_fill_query("apply", target_namespace="gas", target_asset=..., tree=...)`

Supported `fill_kind` (v1): **`AttributeInitDataTable`**.

Tree shape:

```json
{
  "fill_kind": "AttributeInitDataTable",
  "attribute_set": "ULeviathanVitalsSet",
  "rows": {
    "Player.1": { "MaxHealth": 100.0, "HealthRegenRate": 1.0, "AttackRating": 10 },
    "Player.2": { "MaxHealth": 200.0, "HealthRegenRate": 1.0, "AttackRating": 12 },
    "Player.10": { "MaxHealth": 999.0, "HealthRegenRate": 1.0, "AttackRating": 30 }
  }
}
```

- `attribute_set` accepts either a C++ class name (`"ULeviathanVitalsSet"` / `"LeviathanVitalsSet"`) or a Blueprint asset path (`"/Game/.../BP_VitalsSet"`).
- Each cell may be a bare number (sets `BaseValue` only) OR an object `{ "base": N, "min": N, "max": N }` (sets all three on `FAttributeMetaData`).
- Row names are stored as `[GroupName].[AttributeSetName].[Attribute]` per the engine's `FAttributeSetInitterDiscreteLevels` convention (`AttributeSet.h:303-318`).
- Pre-commit, every column-name in `rows[].*` is resolved against the `attribute_set` class. **A miss surfaces as a `SilentDrops` entry** with a "possible rename hazard" warning — this is the `FGameplayAttribute`-rename-invalidates-GEs quirk from the design's Cross-Cutting Engine Quirks table.
- `dry_run: true` returns the full FieldWrites report without touching the asset.
- `strict: true` rejects the whole batch and cancels the transaction if any cell errors.

#### `describe_query("schema", target_namespace="gas", target_asset=...)`

Returns:

- **`target_asset` is an AttributeInit DataTable** → returns the `FAttributeMetaData` row schema (`BaseValue:float`, `MinValue:float`, `MaxValue:float`, `DerivedAttributeInfo:FString`, `bCanStack:bool`).
- **`target_asset` is anything else** → returns the modifier-magnitude **tagged-union descriptor** (`ScalableFloat` / `AttributeBased` / `SetByCaller` / `CustomCalculationClass`) with per-variant `ConditionalOn` discriminators and ImportText sample forms — the GE describe surface from design Cross-Cutting Engine Quirks row.
- **`target_asset` is empty** → returns both shapes as children of a `gas` root descriptor so callers can introspect the namespace's full surface.

### Files

- `Plugins/Monolith/Source/MonolithGAS/Private/MonolithGASBulkFillAdapter.h` / `.cpp` — the adapter
- `Plugins/Monolith/Source/MonolithGAS/Private/MonolithGASModule.cpp` — `Register()` + `Unregister()` call sites

