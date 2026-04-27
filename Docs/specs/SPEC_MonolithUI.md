# Monolith — MonolithUI Module

**Parent:** [SPEC_CORE.md](../SPEC_CORE.md)
**Engine:** Unreal Engine 5.7+
**Version:** 0.14.7 (Beta) — architecture expansion Phase A–L landed 2026-04-26 (plan: [`Docs/plans/2026-04-25-monolith-ui-architecture-expansion.md`](../../../../Docs/plans/2026-04-25-monolith-ui-architecture-expansion.md)); optional EffectSurface provider decouple (Wave 1/2 + Final.1) landed 2026-04-27.

---

## MonolithUI

**Dependencies:** Core, CoreUObject, Engine, MonolithCore, UnrealEd, UMGEditor, UMG, Slate, SlateCore, Json, JsonUtilities, KismetCompiler, MovieScene, MovieSceneTracks, DeveloperSettings, AssetTools, ImageWrapper, ImageCore, Kismet, MaterialEditor, EditorSubsystem (Public — `UMonolithUIRegistrySubsystem` is exported), CommonUI (optional — `#if WITH_COMMONUI`)

**The optional EffectSurface provider is NOT a build-system dependency** (decoupled 2026-04-27). EffectSurface support is delivered via UClass-by-name reflection through `MonolithUI::GetEffectSurfaceClass()` — see § "Optional Dep Probe API" and § "Error Contract — Optional EffectSurface Provider Absence (-32010)". External providers may depend on MonolithUI for registry/spec structs, but MonolithUI must not depend on them.
**Total actions in `ui::` namespace:** **117** when `WITH_COMMONUI=1` (66 always-on owned by this module + 50 CommonUI owned by this module conditional on `WITH_COMMONUI` + 1 inline diagnostic `dump_style_cache_stats` registered from `MonolithUIModule.cpp` under the same gate) + **4 GAS UI binding aliases owned by `MonolithGAS`** (also registered into `ui::`, conditional on `WITH_GBA`). Without `WITH_COMMONUI`, the namespace registers **66** actions; without `WITH_GBA` the four bridge aliases are absent.
**Settings toggle:** `bEnableUI` (default: True)
**MCP tool:** `ui_query`
**Namespace:** `ui`

### Action category roll-up (post Phase A–L)

| Category | Count | Source file(s) | Conditional? |
|----------|-------|----------------|--------------|
| Widget CRUD | 7 | `MonolithUIActions.cpp` | always |
| Slot | 3 | `MonolithUISlotActions.cpp` | always |
| Templates | 8 | `MonolithUITemplateActions.cpp` | always |
| Styling | 6 | `MonolithUIStylingActions.cpp` | always |
| Animation v1 (deprecated) | 5 | `MonolithUIAnimationActions.cpp` | always — `create_animation` + `add_animation_keyframe` tagged `[DEPRECATED]` Phase L (2026-04-26) |
| Animation v2 / hoisted core | 3 | `Actions/Hoisted/AnimationCoreActions.cpp` | always — `create_animation_v2`, `add_bezier_eased_segment`, `bake_spring_animation` |
| Animation v2 / hoisted events | 2 | `Actions/Hoisted/AnimationEventActions.cpp` | always — `add_animation_event_track`, `bind_animation_to_event` |
| Bindings | 4 | `MonolithUIBindingActions.cpp` | always |
| Settings scaffolds | 5 | `MonolithUISettingsActions.cpp` | always |
| Accessibility (non-CommonUI) | 4 | `MonolithUIAccessibilityActions.cpp` | always |
| Hoisted Design Import | 5 | `Actions/Hoisted/{TextureIngest,FontIngest,RoundedCorner,Shadow,Gradient}Actions.cpp` | always — `import_texture_from_bytes`, `import_font_family`, `set_rounded_corners`, `apply_box_shadow`, `create_gradient_mid_from_spec` |
| Effect Surface Actions | 10 | `Actions/MonolithUIEffectActions.cpp` | always |
| Spec Builder + Serializer | 3 | `Actions/MonolithUISpecActions.cpp` | always — `build_ui_from_spec`, `dump_ui_spec_schema`, `dump_ui_spec` |
| Type Registry diagnostic | 1 | `MonolithUIRegistryActions.cpp` | always — `dump_property_allowlist` |
| **Always-on subtotal** | **66** | | |
| CommonUI Activatables | 8 | `CommonUI/MonolithCommonUIActivatableActions.cpp` | `WITH_COMMONUI` |
| CommonUI Buttons + Styling | 9 | `CommonUI/MonolithCommonUIButtonActions.cpp` | `WITH_COMMONUI` |
| CommonUI Input | 7 | `CommonUI/MonolithCommonUIInputActions.cpp` | `WITH_COMMONUI` |
| CommonUI Navigation/Focus | 5 | `CommonUI/MonolithCommonUINavigationActions.cpp` | `WITH_COMMONUI` |
| CommonUI Lists/Tabs/Groups | 7 | `CommonUI/MonolithCommonUIListActions.cpp` | `WITH_COMMONUI` |
| CommonUI Content widgets | 4 | `CommonUI/MonolithCommonUIContentActions.cpp` | `WITH_COMMONUI` |
| CommonUI Dialogs | 2 | `CommonUI/MonolithCommonUIDialogActions.cpp` | `WITH_COMMONUI` |
| CommonUI Audit + Lint | 4 | `CommonUI/MonolithCommonUIAuditActions.cpp` | `WITH_COMMONUI` |
| CommonUI Accessibility | 4 | `CommonUI/MonolithCommonUIAccessibilityActions.cpp` | `WITH_COMMONUI` |
| Style Service Diagnostics | 1 | inline lambda in `MonolithUIModule.cpp` | `WITH_COMMONUI` — `dump_style_cache_stats` |
| **CommonUI subtotal** | **51** | | conditional |
| **MonolithUI total** | **117** | | full configuration |
| GAS UI binding aliases | 4 | `MonolithGAS/Private/MonolithGASUIBindingActions.cpp` | `WITH_GBA` — registered cross-namespace into `ui::` |

Counts re-verified against `RegisterAction(TEXT("ui"), ...)` call sites on 2026-04-26 (Phase L). Production registration sites only — Tests/ excluded.

### Classes

| Class | Responsibility |
|-------|---------------|
| `FMonolithUIModule` | Registers the 66 always-on actions owned by this module + 51 CommonUI actions when `WITH_COMMONUI`. Logs the live `ui` namespace action count at startup. Owns the `OnPostEngineInit` re-scan delegate that catches late-loading marketplace widget UClasses |
| `FMonolithUIActions` | Widget blueprint CRUD: create, inspect, add/remove widgets, property writes, compile |
| `FMonolithUISlotActions` | Layout slot operations: slot properties, anchor presets, widget movement |
| `FMonolithUITemplateActions` | High-level HUD/menu/panel scaffold templates (8 templates) |
| `FMonolithUIStylingActions` | Visual styling: brush, font, color scheme, text, image, batch style |
| `FMonolithUIAnimationActions` | UMG widget animation v1 CRUD: list, inspect, create, add/remove keyframes (the latter two are Phase L `[DEPRECATED]` — prefer the hoisted v2 surface in `AnimationCoreActions`) |
| `FMonolithUIBindingActions` | Event/property binding inspection, list view setup, widget binding queries |
| `FMonolithUISettingsActions` | Settings/save/audio/input remapping subsystem scaffolding (5 scaffolds) |
| `FMonolithUIAccessibilityActions` | Accessibility subsystem scaffold, audit, colorblind mode, text scale |
| `FMonolithUIRegistryActions` | Registers `dump_property_allowlist` (Phase B diagnostic) |
| `MonolithUI::FTextureIngestActions` / `FFontIngestActions` / `FAnimationCoreActions` / `FAnimationEventActions` / `FRoundedCornerActions` / `FShadowActions` / `FGradientActions` | Hoisted Design Import + Animation v2 verbs (Phase D, 2026-04-26) |
| `MonolithUI::FEffectSurfaceActions` | EffectSurface sub-bag setters + preset (Phase F, 2026-04-26) |
| `MonolithUI::FSpecActions` | `build_ui_from_spec` + `dump_ui_spec_schema` + `dump_ui_spec` (Phases H + J, 2026-04-26) |
| `UMonolithUIRegistrySubsystem` (UEditorSubsystem) | Live type registry + per-type property allowlist (Phase B) |
| `FUITypeRegistry` / `FUIPropertyAllowlist` / `FUIPropertyPathCache` / `FUIReflectionHelper` | Registry data model + safe reflection write surface (Phases B + C) |
| `FUISpecValidator` / `FUISpecBuilder` / `FUISpecSerializer` | Spec-driven UI generator + inverse roundtrip (Phases A + H + J) |
| `FUIAnimationMovieSceneBuilder` | Editor backend that turns `FUISpecAnimation` into `UWidgetAnimation` MovieScene tracks (Phase I — runtime counterparts may live in an external provider module) |
| `FMonolithUIStyleService` | CommonUI style asset dedup + canonical-library lookup (Phase G — `WITH_COMMONUI` only) |
| `UMonolithUISettings` (UDeveloperSettings) | Project Settings → Plugins → Monolith UI: `GeneratedStylesPath`, `CanonicalLibraryPath`, `StyleCacheCap`, `MaxNestingDepth`, `PathCacheCap` |

---

## Actions — UMG Baseline (42 — namespace: "ui")

**Widget CRUD (7)**
| Action | Params | Description |
|--------|--------|-------------|
| `create_widget_blueprint` | `save_path`, `parent_class` | Create a new Widget Blueprint asset |
| `get_widget_tree` | `asset_path` | Get the full widget hierarchy tree |
| `add_widget` | `asset_path`, `widget_class`, `parent_slot` | Add a widget to the widget tree |
| `remove_widget` | `asset_path`, `widget_name` | Remove a widget from the widget tree |
| `set_widget_property` | `asset_path`, `widget_name`, `property_name`, `value` | Set a property on a widget via reflection |
| `compile_widget` | `asset_path` | Compile the Widget Blueprint and return errors/warnings |
| `list_widget_types` | none | List all available widget classes that can be instantiated |

**Slot Operations (3)**
| Action | Params | Description |
|--------|--------|-------------|
| `set_slot_property` | `asset_path`, `widget_name`, `property_name`, `value` | Set a layout slot property (padding, alignment, size, etc.) |
| `set_anchor_preset` | `asset_path`, `widget_name`, `preset` | Apply an anchor preset to a Canvas Panel slot |
| `move_widget` | `asset_path`, `widget_name`, `new_parent`, `slot_index` | Move a widget to a different parent slot |

**Templates (8)**
| Action | Params | Description |
|--------|--------|-------------|
| `create_hud_element` | `save_path`, `element_type` | Scaffold a common HUD element (health bar, crosshair, ammo counter, etc.) |
| `create_menu` | `save_path`, `menu_type` | Scaffold a menu Widget Blueprint (main menu, pause menu, etc.) |
| `create_settings_panel` | `save_path` | Scaffold a settings panel with common option categories |
| `create_dialog` | `save_path`, `dialog_type` | Scaffold a dialog Widget Blueprint (confirmation, info, input prompt) |
| `create_notification_toast` | `save_path` | Scaffold a notification/toast Widget Blueprint |
| `create_loading_screen` | `save_path` | Scaffold a loading screen Widget Blueprint with progress bar |
| `create_inventory_grid` | `save_path`, `columns`, `rows` | Scaffold a grid-based inventory Widget Blueprint |
| `create_save_slot_list` | `save_path` | Scaffold a save slot list Widget Blueprint |

**Styling (6)**
| Action | Params | Description |
|--------|--------|-------------|
| `set_brush` | `asset_path`, `widget_name`, `brush_property`, `texture_path` | Set a brush/image property on a widget |
| `set_font` | `asset_path`, `widget_name`, `font_asset`, `size` | Set the font and size on a text widget |
| `set_color_scheme` | `asset_path`, `color_map` | Apply a color scheme (name->LinearColor map) across the widget |
| `batch_style` | `asset_path`, `style_operations` | Apply multiple styling operations in a single transaction |
| `set_text` | `asset_path`, `widget_name`, `text` | Set display text on a text widget |
| `set_image` | `asset_path`, `widget_name`, `texture_path` | Set the texture on an image widget |

**Animation v1 (5 — `create_animation` and `add_animation_keyframe` `[DEPRECATED]` Phase L; prefer the hoisted `create_animation_v2` / `add_bezier_eased_segment` / `bake_spring_animation` surface documented under "Design Import Actions")**

| Action | Params | Description |
|--------|--------|-------------|
| `list_animations` | `asset_path` | List all UMG animations on a Widget Blueprint |
| `get_animation_details` | `asset_path`, `animation_name` | Get tracks and keyframes for a named animation |
| `create_animation` **[DEPRECATED]** | `asset_path`, `animation_name`, `duration`, `tracks?` | Create a new UMG widget animation. Phase L deprecation marker — response payload is tagged `{deprecated: true, use_action: "ui::create_animation_v2"}`. Removal scheduled one major release out. |
| `add_animation_keyframe` **[DEPRECATED]** | `asset_path`, `animation_name`, `widget_name`, `property`, `time`, `value` | Add a keyframe to a widget animation track. Same deprecation tag as `create_animation`. Use `ui::create_animation_v2` (multi-key authoring) or `ui::add_bezier_eased_segment` (CSS bezier) instead. |
| `remove_animation` | `asset_path`, `animation_name` | Remove a UMG widget animation |

**Bindings (4)**
| Action | Params | Description |
|--------|--------|-------------|
| `list_widget_events` | `asset_path` | List all bindable events on a Widget Blueprint |
| `list_widget_properties` | `asset_path`, `widget_name` | List all bindable properties on a widget |
| `setup_list_view` | `asset_path`, `list_view_name`, `entry_widget_path` | Configure a List View widget with an entry widget class |
| `get_widget_bindings` | `asset_path` | Get all active property and event bindings on a Widget Blueprint |

**Settings Scaffolding (5)**
| Action | Params | Description |
|--------|--------|-------------|
| `scaffold_game_user_settings` | `save_path`, `class_name` | Scaffold a UGameUserSettings subclass with common settings properties |
| `scaffold_save_game` | `save_path`, `class_name` | Scaffold a USaveGame subclass with save slot infrastructure |
| `scaffold_save_subsystem` | `save_path`, `class_name` | Scaffold a save game subsystem (UGameInstanceSubsystem) |
| `scaffold_audio_settings` | `save_path`, `class_name` | Scaffold an audio settings manager with volume/mix controls |
| `scaffold_input_remapping` | `save_path`, `class_name` | Scaffold an input remapping system backed by Enhanced Input |

**Accessibility (4)**
| Action | Params | Description |
|--------|--------|-------------|
| `scaffold_accessibility_subsystem` | `save_path`, `class_name` | Scaffold a UGameInstanceSubsystem implementing accessibility features |
| `audit_accessibility` | `asset_path` | Audit a Widget Blueprint for common accessibility issues (missing tooltips, low contrast, small text) |
| `set_colorblind_mode` | `asset_path`, `mode` | Apply a colorblind-safe palette mode (deuteranopia, protanopia, tritanopia) |
| `set_text_scale` | `asset_path`, `scale` | Apply a global text scale factor to all text widgets in the blueprint |

---

## Actions — CommonUI (50 — namespace: "ui", conditional on `WITH_COMMONUI`)

Shipped M0.5, v0.14.0 (2026-04-19). Tested M0.5.1 (2026-04-25): 50/50 editor-time actions PASS, 8 bugs found and fixed. 11 actions marked [RUNTIME] need PIE testing.

### Conditional Compilation

- **Build.cs detection:** 3-location probe (project `Plugins/`, engine `Plugins/Marketplace/`, engine `Plugins/Runtime/`) for the CommonUI plugin
- **Compile guard:** `#if WITH_COMMONUI` — wraps all 50 CommonUI actions
- **Release escape hatch:** `MONOLITH_RELEASE_BUILD=1` forces `WITH_COMMONUI=0`, stripping CommonUI dependency from the released DLL

### Style Pattern

Class-as-data: style creators (`create_common_button_style`, `create_common_text_style`, `create_common_border_style`) produce Blueprint subclasses via `FKismetEditorUtilities::CreateBlueprint`. These return `_C` class paths. Consumed by `TSubclassOf<UStyle>` properties on CommonUI widgets via `apply_style_to_widget` and `batch_retheme`.

### Default Button Class

`convert_button_to_common` and other button-targeting actions auto-create a persistent Blueprint at `/Game/Monolith/CommonUI/MonolithDefaultCommonButton` (session-cached, saveable). This avoids requiring callers to pre-create a button subclass for simple operations.

### Category A: Activatable Lifecycle (8 actions)

| Action | Params | Description |
|--------|--------|-------------|
| `create_activatable_widget` | `save_path`, `parent_class` | Create a Widget Blueprint parented to `UCommonActivatableWidget` |
| `create_activatable_stack` | `save_path` | Scaffold a WBP containing a `UCommonActivatableWidgetStack` |
| `create_activatable_switcher` | `save_path` | Scaffold a WBP containing a `UCommonActivatableWidgetSwitcher` |
| `configure_activatable` | `asset_path`, `widget_name`, `properties` | Set activatable-specific properties (auto-activate, input config) |
| `push_to_activatable_stack` | `stack_widget`, `widget_class` | [RUNTIME] Push a widget onto an activatable stack |
| `pop_activatable_stack` | `stack_widget` | [RUNTIME] Pop the top widget from an activatable stack |
| `get_activatable_stack_state` | `stack_widget` | [RUNTIME] Query the stack: active widget, depth, transition state |
| `set_activatable_transition` | `asset_path`, `transition_type`, `duration` | Configure push/pop transition animations |

### Category B: Buttons + Styling (9 actions)

| Action | Params | Description |
|--------|--------|-------------|
| `convert_button_to_common` | `asset_path`, `widget_name` | Replace a UButton with UCommonButtonBase. Does NOT auto-transfer children; the old child subtree is retired and child variable GUIDs are pruned before compile. |
| `configure_common_button` | `asset_path`, `widget_name`, `properties` | Set button-specific properties (triggering input action, selection state, etc.) |
| `create_common_button_style` | `save_path`, `style_name`, `style_spec` | Create a Blueprint subclass of `UCommonButtonStyle` |
| `create_common_text_style` | `save_path`, `style_name`, `style_spec` | Create a Blueprint subclass of `UCommonTextStyle` |
| `create_common_border_style` | `save_path`, `style_name`, `style_spec` | Create a Blueprint subclass of `UCommonBorderStyle` |
| `apply_style_to_widget` | `asset_path`, `widget_name`, `style_class` | Apply a style class to a CommonUI widget |
| `batch_retheme` | `asset_path`, `style_map` | Retheme multiple widgets in a single transaction |
| `configure_common_text` | `asset_path`, `widget_name`, `properties` | Set `UCommonTextBlock` properties (style, scroll speed, auto-collapse) |
| `configure_common_border` | `asset_path`, `widget_name`, `properties` | Set `UCommonBorder` properties (style, opacity, etc.) |

### Category C: Input/Actions/Glyphs (7 actions)

| Action | Params | Description |
|--------|--------|-------------|
| `create_input_action_data_table` | `save_path`, `table_name` | Create a DataTable for CommonUI input action definitions |
| `add_input_action_row` | `table_path`, `row_name`, `action_spec` | Add a row to an input action DataTable |
| `bind_common_action_widget` | `asset_path`, `widget_name`, `action_row` | Bind a `UCommonActionWidget` to display a specific input glyph |
| `create_bound_action_bar` | `save_path` | Scaffold a WBP containing a `UCommonBoundActionBar` |
| `get_active_input_type` | none | [RUNTIME] Query the current active input type (gamepad, keyboard, touch) |
| `set_input_type_override` | `input_type` | [RUNTIME] Force a specific input type for glyph display |
| `list_platform_input_tables` | none | List all registered platform input DataTables |

### Category D: Navigation/Focus (5 actions)

| Action | Params | Description |
|--------|--------|-------------|
| `set_widget_navigation` | `asset_path`, `widget_name`, `nav_rules` | Configure explicit navigation rules (up/down/left/right targets) |
| `set_initial_focus_target` | `asset_path`, `target_name` | Set the initial focus target for an activatable widget |
| `force_focus` | `widget_name` | [RUNTIME] Force focus to a specific widget |
| `get_focus_path` | none | [RUNTIME] Query the current focus path (widget chain) |
| `request_refresh_focus` | none | [RUNTIME] Request CommonUI to recalculate focus |

### Category E: Lists/Tabs/Groups (7 actions)

| Action | Params | Description |
|--------|--------|-------------|
| `setup_common_list_view` | `asset_path`, `list_view_name`, `entry_widget_path` | Configure a `UCommonListView` with an entry widget class |
| `create_tab_list_widget` | `save_path` | Scaffold a WBP containing a `UCommonTabListWidgetBase` |
| `register_tab` | `tab_list_widget`, `tab_id`, `tab_widget` | [RUNTIME] Register a tab with a tab list |
| `create_button_group` | `group_name` | [RUNTIME] Create a `UCommonButtonGroupBase` for radio-style selection |
| `configure_animated_switcher` | `asset_path`, `widget_name`, `properties` | Configure `UCommonAnimatedSwitcher` transition settings |
| `create_widget_carousel` | `save_path` | Scaffold a WBP containing a `UCommonWidgetCarousel` |
| `create_hardware_visibility_border` | `save_path`, `visibility_tags` | Scaffold a WBP with `UCommonHardwareVisibilityBorder` (platform-gated visibility) |

### Category F: Content Widgets (4 actions)

| Action | Params | Description |
|--------|--------|-------------|
| `configure_numeric_text` | `asset_path`, `widget_name`, `properties` | Configure `UCommonNumericTextBlock` (format, interpolation speed, etc.) |
| `configure_rotator` | `asset_path`, `widget_name`, `properties` | Configure `UCommonRotator` widget properties |
| `create_lazy_image` | `asset_path`, `parent_slot` | Add a `UCommonLazyImage` widget (async texture loading) |
| `create_load_guard` | `asset_path`, `parent_slot` | Add a `UCommonLoadGuard` widget (loading state display) |

### Category G: Dialogs (2 actions)

| Action | Params | Description |
|--------|--------|-------------|
| `configure_modal_overlay` | `asset_path`, `widget_name`, `properties` | Configure modal overlay behavior (dismiss on click, input block) |
| `show_common_message` | `message_spec` | [RUNTIME] Show a CommonUI message dialog (fire-and-forward, no async result binding) |

### Category H: Audit + Lint (4 actions)

| Action | Params | Description |
|--------|--------|-------------|
| `audit_commonui_widget` | `asset_path` | Audit a CommonUI WBP for best-practice violations (missing styles, nav gaps, focus issues) |
| `export_commonui_report` | `asset_paths` | Export an audit report across multiple WBPs |
| `hot_reload_styles` | none | [RUNTIME, EXPERIMENTAL] Force-reload all CommonUI style assets |
| `dump_action_router_state` | none | [RUNTIME, EXPERIMENTAL] Dump the CommonUI action router state (cannot read `CurrentInputLocks` — engine-private) |

### Category I: Accessibility (4 actions)

| Action | Params | Description |
|--------|--------|-------------|
| `enforce_focus_ring` | `asset_path`, `widget_name`, `ring_spec` | Configure a visible focus ring on a CommonUI widget |
| `wrap_with_reduce_motion_gate` | `asset_path`, `widget_name` | Wrap a widget's animations with a reduce-motion accessibility gate |
| `set_text_scale_binding` | `asset_path`, `widget_name`, `binding_spec` | Bind text scale to an accessibility setting |
| `apply_high_contrast_variant` | `asset_path`, `style_map` | Apply high-contrast color overrides to CommonUI styled widgets |

---

## GAS Bridge Aliases (4 — namespace: "ui", source: `MonolithGAS`)

The MonolithGAS module also registers four cross-namespace aliases under `ui::` so that GAS-aware UI authoring tools see them in `ui_query` results without learning a separate namespace. The aliases dispatch to the same handlers as their `gas::` originals — there is no behavioural difference.

| Action (alias) | Canonical | Description |
|---|---|---|
| `ui::bind_widget_to_attribute` | `gas::bind_widget_to_attribute` | Bind a widget's display property to a GAS attribute via `UMonolithGASAttributeBindingClassExtension`. See [SPEC_MonolithGAS.md](SPEC_MonolithGAS.md) for full param schema |
| `ui::unbind_widget_attribute` | `gas::unbind_widget_attribute` | Remove a binding by widget+target_property |
| `ui::list_attribute_bindings` | `gas::list_attribute_bindings` | List all bindings on a Widget Blueprint |
| `ui::clear_widget_attribute_bindings` | `gas::clear_widget_attribute_bindings` | Clear all bindings on a Widget Blueprint |

**Registration site:** `Plugins/Monolith/Source/MonolithGAS/Private/MonolithGASUIBindingActions.cpp:571-577`. Both registrations call the same handler functor — `unbind_widget_attribute` is registered as the no-`s` form in both namespaces.

> **Why this is here, not in MonolithUI source:** the binding logic depends on GAS types (`FGameplayAttribute`, `UAttributeSet`) which would force MonolithUI to take a hard `WITH_GBA` dep. Aliasing keeps MonolithUI buildable in non-GAS projects while still letting `ui_query` callers find the action.

---

## Known Limitations (CommonUI)

1. `convert_button_to_common` does NOT auto-transfer UButton children to UCommonButtonBase — UCommonButtonBase uses an internal widget tree, not AddChild. The action retires the old button child subtree, removes child bindings/GUIDs, and then reconciles after structural modification without using UE's broader widget-deletion path, so the replaced button can keep its variable identity. Callers must rewire CommonButton content manually.
2. `set_initial_focus_target` requires the target WBP to expose a `DesiredFocusTargetName` or `InitialFocusTargetName` FName UPROPERTY and override `NativeGetDesiredFocusTarget`. Action errors out if neither property exists.
3. `show_common_message` is fire-and-forward — async result-binding (Yes/No) requires the dialog WBP to handle internally. No MCP-side delegate routing yet.
4. `dump_action_router_state` cannot read `UCommonUIActionRouterBase::CurrentInputLocks` (private-transient in engine). Engine PR candidate (M0.7).

---

## Spec System (M5)

**Status:** Phases A–L all landed 2026-04-26 (plan: [`Docs/plans/2026-04-25-monolith-ui-architecture-expansion.md`](../../../../Docs/plans/2026-04-25-monolith-ui-architecture-expansion.md)). Phase L (this pass) marked v1 animation actions deprecated, refreshed action counts and roll-ups across all SPEC docs, and preserved one-major-release compatibility for legacy sibling UI aliases.

The Spec System promotes MonolithUI from a flat action toolbox into a schema-driven UI generator. A single MCP action `ui::build_ui_from_spec` accepts a typed JSON document (`FUISpecDocument`) and produces a fully-built `UWidgetBlueprint` transactionally — validate-then-mutate semantics guarantee the asset is either fully replaced or untouched.

### Spec Builder (M5 — Phase H, landed 2026-04-26)

**Action surface:** `ui::build_ui_from_spec` + `ui::dump_ui_spec_schema`.

**`build_ui_from_spec` pipeline (per plan §1.4):**

1. Parse JSON `spec` -> `FUISpecDocument` (manual walk; FUISpecNode recurses through TSharedPtr).
2. Validate via `FUISpecValidator` (structural + animation/style ref checks).
3. Resolve `parentClass` token -> `UClass*` (UserWidget, CommonActivatableWidget, CommonUserWidget, or full path).
4. **Dry-walk** the spec tree: type-resolve every node, depth-check, cycle-detect (visited-set keyed by `FName` id). FAIL HERE means no asset mutation runs.
5. Get-or-create `UWidgetBlueprint` at `asset_path`. Refuses on parent-class mismatch when `overwrite=true`.
6. Open `FScopedTransaction` (in-place edit sub-case rolls back via Cancel).
7. **Pre-create referenced styles** before any widget construction (closes recon takeaway §8 BP-recompile-invalidates-template trap).
8. Walk the tree depth-first; dispatch per node to `PanelBuilder` / `LeafBuilder` / `CommonUIBuilder` / `EffectSurfaceBuilder` sub-builders.
9. Compile the blueprint, then **rebuild widget-id -> UWidget* map** by walking the post-compile WidgetTree (closes recon takeaway §8 — pointers cached pre-compile go stale).
10. Wire animations via `FUIAnimationMovieSceneBuilder::BuildSingle` (Phase I integration).
11. Save (or cancel transaction + rollback created asset on `dry_run=true` / failure).

**Atomicity strategy (split per H2 + R3):** `FScopedTransaction` wraps property writes on existing WBPs (where `Modify()` calls undo cleanly). For NEW WBPs, the create-package + factory-create side effects survive transaction cancel — so we dry-walk first (validate every node, resolve every UClass) and only call `CreatePackage` after dry-walk passes. On post-create failure we manually `Package->MarkAsGarbage()` + `ObjectTools::DeleteSingleObject` to undo the disk artefact.

**Per-pass FUIBuildContext (NOT generator-as-stateful-object).** The builder is stateless; each Build call constructs a fresh `FUIBuildContext` on the stack. Sub-builders borrow the context by reference and never cache pointers across calls. Closes recon avoid §10/4.

**Modes:**

| Mode | Effect |
|---|---|
| `dry_run: true` | Validate + walk + report a diff; no asset mutation. Transaction cancelled at end. |
| `treat_warnings_as_errors: true` | Validator warnings (missing styleRef / animationRef etc.) escalate to errors and abort the build. |
| `raw_mode: true` | Bypass the per-write allowlist gate (legacy compat for callers that pre-date the gate). |
| `overwrite: false` | Refuse if an asset already exists at `asset_path`. |

**Limits & guards:**

- `MaxNestingDepth = 32` (Project Settings -> Plugins -> Monolith UI). Spec trees deeper than this are rejected before any mutation.
- Cycle detection via visited-set keyed by FName-id. Duplicate ids in the spec tree are rejected as structural errors.
- The path cache survives Live Coding / hot-reload by re-finding root structs by name on every cache hit (delegates to `FCoreUObjectDelegates::ReloadCompleteDelegate`).

**Action signature:**

```
ui::build_ui_from_spec({
  spec: <FUISpecDocument JSON>,
  asset_path: "/Game/UI/MyWidget",
  overwrite: true,             // default
  dry_run: false,              // default
  request_id: "<caller-uuid>", // optional, echoed back
  treat_warnings_as_errors: false,
  raw_mode: false              // default — bypass allowlist gate
})
```

**Response:**

```
{
  bSuccess: bool,
  asset_path: str,
  request_id?: str,
  validation: { is_valid: bool, llm_report: str },
  node_counts: { created: N, modified: N, removed: N },
  errors?: [{ category, widget_id, message, json_path, suggested_fix }],
  warnings?: [{ category, widget_id, message, suggested_fix }],
  diff?: { lines: [str], dry_run: bool }
}
```

**`ui::dump_ui_spec_schema`** returns a JSON-Schema-style description of `FUISpecDocument` plus the live allowlist projection per widget type. LLMs use it to build valid spec inputs without crawling our headers.

### Spec Serializer (M5 — Phase J, landed 2026-04-26)

`FUISpecSerializer` is the inverse of `FUISpecBuilder`. The MCP action `ui::dump_ui_spec(asset_path)` reads a live `UWidgetBlueprint` and produces a `FUISpecDocument` JSON suitable for round-tripping back through `ui::build_ui_from_spec`. Combined with the builder, this gives a true Build -> Dump -> Build identity for every property surfaced through the Phase B/C allowlist.

**Pipeline:**

1. Load the WBP at `asset_path` (with `/Game/` prefix fallback for relative paths).
2. Capture document-level metadata (`Name`, `ParentClass`, `Version`, `Metadata.AuthoringTool = "MonolithUI.SpecSerializer"`, `Metadata.SourceFile = AssetPath`).
3. Walk the `WidgetTree->RootWidget` recursively, building `FUISpecNode` per widget with:
   - **Type token** -- registry-mapped via `FUITypeRegistry::FindByClass`, falling back to `MakeTokenFromClassName` (strips the engine `U` prefix) when the registry hasn't classified the type.
   - **Slot** -- per-`UPanelSlot` derived class branch table covering ALL stock UMG slot types (Canvas / Vertical / Horizontal / Overlay / ScrollBox / Grid / UniformGrid / SizeBox / ScaleBox / WrapBox / WidgetSwitcher / Border). Closes the §6.3.3 surface-map gap (the legacy `SerializeSlotProperties` covered only Canvas/Vertical/Horizontal/Overlay -- 4 of 12+).
   - **Style** -- widget-level `RenderOpacity` + `Visibility` always, plus `SizeBox` width/height overrides, `Border` brush colour + padding, `ProgressBar` fill colour. Reads via the engine's public getters (`GetBrushColor`, `GetFillColorAndOpacity`, etc.) -- the deprecated direct UPROPERTY accessors are avoided.
   - **Content** -- per-widget-class branches for `UTextBlock` (text, font size, font color, AutoWrapText -> WrapMode token), `URichTextBlock`, `UImage` (BrushPath via `B.GetResourceObject()->GetPathName()`), `UEditableText` / `UEditableTextBox` (text + hint).
   - **Effect** (`bHasEffect`) -- when the widget IS-A `UEffectSurface`, capture the entire `FEffectSurfaceConfig`: corner radii, smoothness, solid colour, backdrop blur strength, and the **DropShadow / InnerShadow arrays** via a hand-rolled array path (the Phase H `ApplyJsonPath` helper deliberately does not navigate `TArray` container properties; Phase J implements both the symmetric read here AND the matching write).
   - **CommonUI** (`bHasCommonUI`, `WITH_COMMONUI`) -- detect `UCommonButtonBase` / `UCommonActivatableWidget` / `UCommonUserWidget`; capture the button's style class via `GetStyleCDO()->GetClass()->GetPathName()` into `StyleRefs`.
   - **CustomClassPath** -- when the registry does not recognise the widget type, the full UClass path is captured so the rebuild side can resolve via `LoadObject<UClass>`.
4. Capture every `UWidgetAnimation` in `WBP->Animations` into a `FUISpecAnimation` entry: name (display label), duration (derived from the MovieScene playback range + tick resolution), target widget id (first binding's `WidgetName`), plus a synthetic two-keyframe Linear envelope.
5. Pack into a `FUISpecDocument` with non-null `Root` and return.

**Action surface:**

```jsonc
ui::dump_ui_spec({
  asset_path: "/Game/UI/MyMenu",
  emit_defaults?: false,
  request_id?: "uuid"
})
=>
{
  bSuccess: bool,
  asset_path: str,
  request_id?: str,
  nodes_visited: int,
  animations_captured: int,
  spec: <FUISpecDocument JSON ready to feed back into ui::build_ui_from_spec>,
  errors?: [...],
  warnings?: [...]
}
```

**Lossy boundary catalogue.** The following do not roundtrip perfectly; the rebuild side falls back to defaults or warns:

| Field | Behaviour | Rationale |
|---|---|---|
| Style asset class references (CommonUI button styles, text styles) | Serialised as the asset's full path; resolved via `LoadObject` on rebuild. A manual rename / move of the style asset between dump and build will break the link. | UMG style classes are referenced by `TSubclassOf<UCommonButtonStyle>`; we capture the class path, not the inline configuration. |
| Native widget bindings (graph-bound delegates, widget native ticks) | Captured by NAME ONLY in `animationRefs`. The graph wiring itself is NOT roundtripped. | UISpec is a UMG layout format, not a Blueprint graph format. |
| Editor-only metadata (`DisplayLabel`-vs-`Name` divergence, designer canvas size) | Dropped intentionally. | Does not drive runtime behaviour. |
| Per-property animation curve TANGENT data | Keyframes serialise with `{time, scalarValue, easing}`; rich Bezier tangents (`ArriveTangent` / `LeaveTangent` / `ArriveWeight` / `LeaveWeight`) only populated when the source MovieScene track exposes them via the v1 reader. Curve shapes beyond Linear / Cubic / Constant fall back to Linear with a warning. | Full MovieScene curve capture is a Phase J+1 enhancement; v1 captures the envelope. |
| Slot fields outside the per-slot-class subset documented above | Silently default. | Validator on the rebuild side warns rather than errors -- the spec is intentionally permissive here. |
| Default-valued property fields | Omitted unless `emit_defaults=true`. | Keeps the resulting JSON small and lets the validator's missing-key warnings actually fire on the rebuild side. |
| Document-scoped `Styles` map | Empty for fresh dumps from existing WBPs. | UMG has no direct counterpart for free-standing named styles outside the CommonUI style class table. |

**Slot-type coverage matrix:**

| Slot Class | Phase J Coverage | Captured fields |
|---|---|---|
| `UCanvasPanelSlot` | Full | anchors (preset + raw min/max), offsets (Position, Size), alignment, autoSize, zOrder |
| `UVerticalBoxSlot` | Full | hAlign, vAlign, padding |
| `UHorizontalBoxSlot` | Full | hAlign, vAlign, padding |
| `UOverlaySlot` | Full | hAlign, vAlign, padding |
| `UScrollBoxSlot` | Full | hAlign, vAlign, padding |
| `UGridSlot` | Full | hAlign, vAlign, padding, row/column (in Position), rowSpan/columnSpan (in Size), layer (in zOrder) |
| `UUniformGridSlot` | Full | hAlign, vAlign, row/column (in Position) |
| `USizeBoxSlot` | Full | hAlign, vAlign, padding |
| `UScaleBoxSlot` | Partial | hAlign, vAlign (no padding -- `UScaleBoxSlot` deprecated padding in 5.1) |
| `UWrapBoxSlot` | Full | hAlign, vAlign, padding |
| `UWidgetSwitcherSlot` | Full | hAlign, vAlign, padding |
| `UBorderSlot` | Full | hAlign, vAlign, padding |
| Unknown / future slot types | Silently default | Lossy boundary; documented above |

**Files:**

- `Source/MonolithUI/Public/Spec/UISpecSerializer.h` -- `FUISpecSerializer::Dump` entry + `FUISpecSerializerInputs` / `FUISpecSerializerResult` structs.
- `Source/MonolithUI/Private/Spec/UISpecSerializer.cpp` -- per-slot/-content/-style/-effect/-commonui readers + recursive widget walker.
- `Source/MonolithUI/Private/Actions/MonolithUISpecActions.cpp` -- `ui::dump_ui_spec` action handler + the inverse `NodeToJson` / `SlotToJson` / `EffectToJson` / `CommonUIToJson` packers.
- `Source/MonolithUI/Public/Registry/UIReflectionHelper.h` (+ `.cpp`) -- new `ReadJsonPath` method (symmetric counterpart to `ApplyJsonPath`); read-side `ReadValueFromProperty` dispatch table mirrors the existing write-side `WriteValueToProperty`.
- `Source/MonolithUI/Private/Tests/UISpecRoundtripTests.cpp` -- automation tests J1 (`MonolithUI.SpecSerializer.RoundtripIdentity`), J4 (`MonolithUI.SpecSerializer.RoundtripCorpus` -- 5 representative WBP shapes), and a per-slot-type coverage test (`MonolithUI.SpecSerializer.SlotCoverage`).

### LLM Error Reporting (M5 — Phase K, landed 2026-04-26)

`FUISpecError` and `FUISpecValidationResult` both carry dual-audience formatters:

| Method | Audience | Shape |
|---|---|---|
| `FUISpecError::ToString()` | Humans (logs, automation test failures) | One-liner: `[ERR ] /path: message  hint: <fix>  valid_options: a, b, c, ...` |
| `FUISpecError::ToLLMReport()` | LLM consumers (re-prompt loops) | Stable key:value block with `category:` / `severity:` / `json_path:` / `widget_id:` / `message:` / `suggested_fix:` / `valid_options:` lines |
| `FUISpecValidationResult::ToString()` | Humans | Multi-line summary header + per-finding lines |
| `FUISpecValidationResult::ToLLMReport()` | LLM consumers | Aggregate header (`validation_status` / `error_count` / `warning_count`) + per-finding blocks delegated verbatim to `FUISpecError::ToLLMReport()` (two-space indented under `- kind: error|warning` discriminator) |

**Stability contract:** the field labels (`json_path`, `category`, `valid_options`, `suggested_fix`, etc.) are part of the public contract. Adding new fields is allowed; renaming or removing existing ones is a breaking change. External automation harnesses may route validation errors back to upstream LLMs by grepping these labels — drift here breaks that round-trip.

**`request_id` echo.** `ui::build_ui_from_spec` echoes the caller-supplied `request_id` back in the response on every path (parse-fail / validate-fail / builder-fail / success). The field is omitted entirely when the caller did not supply one — this lets consumers distinguish "unset" from "set-to-empty-string".

**Action handler error shape (Phase K audit).** The 10 most-common-action error paths now emit `FMonolithActionResult::Error` with the message body equal to `FUISpecError::ToLLMReport()`. Consumers can dispatch on `R.bSuccess == false` and parse the message body using the same key:value labels they already use for `build_ui_from_spec` validation blocks. Audited actions:

| Action | Notable enum payloads in `valid_options` |
|---|---|
| `ui::create_widget_blueprint` | `UserWidget`, `CommonActivatableWidget`, `CommonUserWidget` |
| `ui::add_widget` | 16-entry common widget-class starter list (full set behind `ui::list_widget_types`) |
| `ui::set_widget_property` | Live allowlist entries for the widget type (when the registry has classified the type); `raw_mode=true` named in `suggested_fix` as the bypass |
| `ui::compile_widget` | n/a — surfaces `BS_Error` as a `Compile`-category structured failure instead of success-with-status=error |
| `ui::set_slot_property` | The 10 legal slot-property keys |
| `ui::set_anchor_preset` | All 16 anchor-preset tokens (canonical list at `MonolithUIInternal::GetAnchorPresetNames`) |
| `ui::set_brush` | The 5 `ESlateBrushDrawType` tokens; the 9 brush-property keys |
| `ui::set_font` | `TextBlock`, `RichTextBlock`; the 6 font-property keys |
| `ui::set_text` | `TextBlock`, `RichTextBlock`; the 4 text-property keys; the 3 `ETextJustify` tokens |
| `ui::set_image` | `Image`; the 4 image-property keys |

JSON-RPC error code: `-32602` (invalid-params) for caller-input issues, `-32603` (internal error) for asset-creation / compile failures the caller cannot fix from their input.

**Sub-builders:**

| Builder | Handles | Notes |
|---|---|---|
| `PanelBuilder` | UVerticalBox / UHorizontalBox / UCanvasPanel / UOverlay / UWrapBox / UScrollBox / UGridPanel / UUniformGridPanel | Slot writes via typed casts (CanvasPanelSlot / VerticalBoxSlot / etc.). |
| `LeafBuilder` | UTextBlock / UImage / UButton / URichTextBlock / UEditableText / UEditableTextBox / USpacer / single-child content widgets (UBorder / USizeBox / UScaleBox) | FUISpecContent text/font/brush. |
| `CommonUIBuilder` | Any widget with `bHasCommonUI` set | Wires StyleRef -> pre-created style class. WITH_COMMONUI gated; stub-warns when CommonUI absent. |
| `EffectSurfaceBuilder` | UEffectSurface (optional provider) | Routes Effect.* JSON paths through `FUIReflectionHelper::ApplyJsonPath` (the Phase H hoist of the per-handler translation). |

**JSON path -> engine path translation (hoisted in Phase H):** `FUIReflectionHelper::ApplyJsonPath` performs the allowlist gate on the LLM-facing JSON path, then translates JSON path -> engine path via the registry's curated mapping for that widget token, then calls the standard `Apply` with raw_mode internally. The Phase F per-handler translation pattern (`FEffectSurfaceActions::ApplyPath`) becomes a thin wrapper around this hoisted helper — the spec builder and the per-MCP-call paths share one translation routine.

### Folder convention

| Path | Contents |
|------|----------|
| `Source/MonolithUI/Public/Spec/` | Document USTRUCTs (`UISpec.h`), validator interface (`UISpecValidator.h`) |
| `Source/MonolithUI/Private/Spec/` | Validator implementation, JSON parser (M5), builder pipeline (M5) |
| `Source/MonolithUI/Public/Registry/` | Type registry, property allowlist, reflection cache (M6) |
| `Source/MonolithUI/Private/Registry/` | Registry/cache implementations (M6) |
| `Source/MonolithUI/Public/Style/` | CommonUI style service public API (M7) |
| `Source/MonolithUI/Private/Style/` | Style service implementation (M7) |

### Phase A foundations (landed)

- `MonolithUICommon.h` / `.cpp` — exported `MONOLITHUI_API` helpers (`ParseColor`, `TryParseColor`, `LoadWidgetBlueprint`, `WidgetClassFromName`, anchor presets, alignment parsers, variable-name registration). The legacy `MonolithUIInternal::*` helpers now forward into these exports.
- `LogMonolithUISpec` log category — filterable separately from `LogMonolith` so spec-builder iteration noise is isolated.
- `Spec/UISpec.h` — full USTRUCT cluster: `FUISpecDocument`, `FUISpecNode`, `FUISpecSlot`, `FUISpecStyle`, `FUISpecContent`, `FUISpecEffect`, `FUISpecEffectShadow`, `FUISpecCommonUI`, `FUISpecAnimation`, `FUISpecKeyframe`, `FUISpecMetadata`, `FUISpecError`, `FUISpecValidationResult`. `FUISpecNode::Children` is `TArray<TSharedPtr<FUISpecNode>>` (NOT a UPROPERTY) — UHT rejects USTRUCT recursion through reflected members. `FUISpecNode` and `FUISpecDocument` are `USTRUCT()` (not `BlueprintType`) for the same reason; leaf, non-recursive structs are `BlueprintType`.
- `Spec/UISpecValidator.{h,cpp}` — stateless validator skeleton. Phase A implements the missing-root structural check; subsequent phases extend with type-registry and allowlist validation.

### Shared UI helper consolidation (landed in Phase A)

Downstream UI bridge modules can declare `MonolithUI` as a private dependency and consume `MonolithUI::TryParseColor` instead of carrying local color parsers. The shared helper preserves the no-degamma byte-to-float path required by UI gradient/shadow materials (see `MonolithUICommon.h` for the `ParseColor` vs `TryParseColor` distinction). MonolithUI remains the provider of generic helpers; downstream bridge modules are not public Monolith dependencies.

---

## Effect Surface Actions (M5 — Phase F, landed 2026-04-26)

**Status:** Shipped. 10 actions in `ui::` namespace targeting `UEffectSurface` supplied by an optional provider. Composes against the allowlist-gated JSON path surface seeded in Phase E (`Effect.Shape.CornerRadii`, `Effect.Fill.Mode`, etc.). Each sub-bag setter ALSO ORs the matching feature bit into `Effect.FeatureFlags` so the shader permutation key stays in sync with the live data.

### Composition pattern

The handlers do NOT call into `UEffectSurface`'s typed C++ setters (`SetCornerRadii`, `SetFill`, etc.) and do NOT include any optional-provider header. They:

1. Probe `MonolithUI::IsEffectSurfaceAvailable()` (R3b) — when false, return `-32010 ErrOptionalDepUnavailable` with a structured payload (see § "Error Contract" below). Single point of truth at the resolver entry, never per-handler.
2. Resolve the widget through `MonolithUI::GetEffectSurfaceClass()` + `IsA(EffectSurfaceClass)` — no `Cast<UEffectSurface>`.
3. Route every property write through `FUIReflectionHelper::Apply` against the curated JSON paths.
4. The two C++-method calls that remain (`ForceMIDRefresh`, `SetBaseMaterial` inside `apply_effect_surface_preset`) go through `UClass::FindFunctionByName` + `ProcessEvent` — no typed function pointer.
5. Feature-flag bits use raw `int32` constants (`EffectFeature_RoundedCorners = 1<<0`, ...) declared at the top of `MonolithUIEffectActions.cpp`; the provider's source-of-truth header is cross-referenced in a comment but never `#include`d.

Invariants this keeps aligned:

- The allowlist gate is the single source of truth for what's writable on an EffectSurface widget.
- The Phase C `FUIPropertyPathCache` stays hot for both the action surface AND the Phase H spec-builder pipeline (which reuses the same path strings).
- External emitters are loosely coupled to UEffectSurface's compile-time API surface — only the JSON path schema is contractual.
- The public Monolith release zip ships standalone — no optional-provider symbol references in `MonolithUI.dll` import table. Verified via `make_release.ps1` `$LeakSentinels` dumpbin /imports check.

**Drift-detection cadence:** the raw flag constants in `MonolithUIEffectActions.cpp` and the provider's canonical enum MUST stay aligned. If the provider renumbers its feature enum, the constants here become stale silently. Manual diff on every preset-touching PR; tracked as a documentation/audit concern.

### Actions (10 — namespace: `ui`)

| Action | Required params | Optional params | FeatureFlag bit |
|--------|-----------------|-----------------|------------------|
| `set_effect_surface_corners` | `asset_path`, `widget_name`, `corner_radii=[TL,TR,BR,BL]` | `smoothness`, `compile` | `RoundedCorners` |
| `set_effect_surface_fill` | `asset_path`, `widget_name`, `mode=solid|linear|radial` | `color`, `stops=[{position,color}]`, `angle`, `radial_center=[x,y]`, `compile` | `SolidFill`/`LinearGradient`/`RadialGradient` |
| `set_effect_surface_border` | `asset_path`, `widget_name`, `width` | `color`, `offset`, `glow`, `glow_color`, `compile` | `Border` |
| `set_effect_surface_dropShadow` | `asset_path`, `widget_name`, `layers=[{offset,blur,spread,color}, ...]` (cap 4) | `compile` | `DropShadow` (skipped on empty list) |
| `set_effect_surface_innerShadow` | `asset_path`, `widget_name`, `layers=[{offset,blur,spread,color}, ...]` (cap 4) | `compile` | `InnerShadow` (skipped on empty list) |
| `set_effect_surface_glow` | `asset_path`, `widget_name`, `radius` | `color`, `intensity`, `inner_outer_mix`, `compile` | `Glow` |
| `set_effect_surface_filter` | `asset_path`, `widget_name`, ≥1 of `saturation`/`brightness`/`contrast` | `compile` | `Filter` |
| `set_effect_surface_backdropBlur` | `asset_path`, `widget_name`, `strength` (0 disables) | `compile` | `BackdropBlur` (skipped at strength=0) |
| `set_effect_surface_insetHighlight` | `asset_path`, `widget_name`, ≥1 of `offset`/`blur`/`color`/`intensity`/`edge_mask` | `compile` | `InsetHighlight` |
| `apply_effect_surface_preset` | `asset_path`, `widget_name`, `preset_name` | `parent_material`, `compile` | (overwrite — preset is authoritative) |

### Preset names (apply_effect_surface_preset)

OURS — derived from CSS Backgrounds and Borders L3 + CSS Filter Effects L1 vocabulary, no third-party preset library consulted.

| Name | Look |
|------|------|
| `rounded-rect` | 12px corners + soft drop shadow + white solid fill |
| `pill` | Fully rounded ends (sentinel radius 999) + light grey fill |
| `circle` | Same sentinel as pill — only meaningful when host slot is square |
| `glass` | 16px corners + faint translucent fill + thin border + backdrop blur + top inset highlight |
| `glowing-button` | 8px corners + linear gradient fill + outer glow + soft drop shadow |
| `neon` | 4px corners + transparent fill + magenta border with intense border-glow halo |

### MID lifecycle inside `apply_effect_surface_preset`

When `parent_material` is supplied, the handler calls `UEffectSurface::SetBaseMaterial(...)` BEFORE the sub-bag writes so the MID is recreated against the new parent. Otherwise the existing MID is reused. Either way, `FinalizeWriteAndPush` calls `ForceMIDRefresh()` once at the end of the batch so the live preview reflects the new look without waiting on a Slate paint pass.

### External emitter consumption (Phase F4)

Downstream emitters can promote Border/RoundedBorder nodes that combine 3+ of {rounded corners, box shadow, gradient, border outline} to `UEffectSurface` and emit 3-4 `set_effect_surface_*` calls instead of a longer effect-card chain (gradient MID create + bind + box-shadow material composite + rounded-corners reflection + outline x2). Single- and two-effect nodes should stay on the cheap RoundedBorder path -- EffectSurface's per-paint shader cost is only worth eating when the call-saving threshold is met.

### Action count refresh (closed, Phase L — 2026-04-26)

The Phase A foundation added zero MCP actions. Phase B added the diagnostic `dump_property_allowlist`. Phase D hoisted 10 generic UI-design verbs into `ui::` (5 are direct registrations under `ui::`; the other 5 are the AnimationCore + AnimationEvent surface — `create_animation_v2`, `add_bezier_eased_segment`, `bake_spring_animation`, `add_animation_event_track`, `bind_animation_to_event`). Phase F added 10 EffectSurface actions. Phase G added 1 inline `dump_style_cache_stats` (`#if WITH_COMMONUI`). Phase H added 2 (`build_ui_from_spec`, `dump_ui_spec_schema`). Phase J added 1 (`dump_ui_spec`).

Live total against the registration sites in `Plugins/Monolith/Source/MonolithUI/Private/`: **117** when `WITH_COMMONUI=1`, **66** without. The **42** baseline that prior revisions of this document quoted is the pre-expansion figure and is no longer accurate — see the action category roll-up at the top of this file for the current category breakdown.

---

## Type Registry (M5)

**Status:** Phase B landed; subsequent Phase C (`FUIPropertyPathCache` + safe `FUIReflectionHelper`), Phase E (EffectSurface curated mappings), and Phase L (count refresh) all landed 2026-04-26. See [`Docs/plans/2026-04-25-monolith-ui-architecture-expansion.md`](../../../../Docs/plans/2026-04-25-monolith-ui-architecture-expansion.md).

The Type Registry catalogues every UMG widget UClass that the spec/builder pipeline can construct, plus the per-type allowlist of property paths that the safe reflection helper (Phase C) will accept. It is the source-of-truth lookup that backs `build_ui_from_spec` validation, the `set_widget_property` allowlist gate, and the diagnostic action `ui::dump_property_allowlist`.

### Components

| Class | Responsibility |
|-------|---------------|
| `UMonolithUIRegistrySubsystem` | UEditorSubsystem owning the live registry + allowlist. Performs the initial reflection walk in `Initialize`, re-scans on `OnPostEngineInit` (registered from `FMonolithUIModule::StartupModule` because editor subsystems initialise after that delegate fires), and refreshes weak-pointer-stale entries on `FCoreUObjectDelegates::ReloadCompleteDelegate` for hot-reload survival. |
| `FUITypeRegistry` | Catalogue keyed by short token (`VerticalBox`, `TextBlock`, `RoundedBorder`). Each entry holds `WidgetClass` (TWeakObjectPtr — null after hot-reload, refreshed on the next `RefreshAfterReload` call), `ContainerKind` (Leaf / Content / Panel), `MaxChildren` (0 / 1 / -1), `SlotClass` (from `UPanelWidget::GetSlotClass()` on the CDO), and `PropertyMappings` (curated `(JsonPath, EnginePath)` pairs). |
| `FUIPropertyAllowlist` | Lazy projection over `FUITypeRegistry::PropertyMappings`. Per-type `TSet<FString>` populated on first lookup, invalidated whenever the registry mutates. NOT a parallel store — the registry is canonical. |
| `FMonolithUIRegistryActions` | Registers the `ui::dump_property_allowlist` diagnostic action. |

### Token naming convention

Token = UClass display name with the leading `U` engine prefix stripped. `UVerticalBox` → `"VerticalBox"`. `URoundedBorder` → `"RoundedBorder"`. Custom widgets that don't follow the `U` prefix convention keep their full class name. The convention is OURS — derived from CLAUDE.md naming rules; not lifted from any reference plugin.

### Container kind classification

Order-sensitive — `UContentWidget` extends `UPanelWidget`, so the classifier checks Content FIRST:

1. `IsChildOf(UContentWidget::StaticClass())` → `Content`, `MaxChildren=1`.
2. Else `IsChildOf(UPanelWidget::StaticClass())` → `Panel`. `MaxChildren = -1` if `CanHaveMultipleChildren()` (the default), else `1`.
3. Otherwise `Leaf`, `MaxChildren=0`.

Slot class comes from `UPanelWidget::GetSlotClass()` on the CDO (virtual; abstract-base default returns `UPanelSlot::StaticClass()`).

### Filtering rules during the reflection walk

The `TObjectIterator<UClass>` walk skips:
- `CLASS_Abstract` (e.g. `UPanelWidget`, `UContentWidget`, `USafeZone`)
- `CLASS_Deprecated`
- `CLASS_NewerVersionExists` (hot-reload-orphan UClasses — refreshed on the next reload-complete event)
- `CLASS_CompiledFromBlueprint` (Blueprint widget subclasses are project-specific authoring assets, not registry types)

### Curated property mappings

Mappings are explicit, NOT auto-generated from `UPROPERTY` reflection. Auto-walking would surface engine-internal transient flags that should never be writable from a JSON spec — the allowlist is a security gate as well as a correctness one. Phase B ships mappings for: `Border` (Background, BrushColor, Padding, ContentColorAndOpacity), `SizeBox` (Width/HeightOverride, Min/MaxDesired*), `TextBlock` (Text, Font, ColorAndOpacity, Justification, AutoWrapText, Shadow*), `Image` (Brush, ColorAndOpacity), `Button` (ColorAndOpacity, BackgroundColor, Style, ClickMethod, IsFocusable), `ProgressBar` (Percent, FillColorAndOpacity, BarFillType), and the plugin widget `RoundedBorder` (CornerRadii, OutlineColor, OutlineWidth, FillColor + inherited Border properties).

Slot.* paths (`Slot.Padding`, `Slot.HAlign`, `Slot.VAlign`, `Slot.Anchors`, `Slot.Position`, `Slot.Size`, `Slot.Alignment`, `Slot.AutoSize`, `Slot.ZOrder`) are attached to the COMMON child widgets (TextBlock, Image, Button, Border, SizeBox, ProgressBar, the box panels, ScrollBox, RoundedBorder). The reflection helper performs per-parent-slot validation at write time — the allowlist is the generous outer envelope.

### Diagnostic action

| Action | Params | Description |
|--------|--------|-------------|
| `dump_property_allowlist` | `widget_type` (string) | Returns `{type, registered, container_kind, max_children, widget_class, allowed_paths:[...], allowed_path_count}`. Unknown types return `registered:false` with a hint that the type isn't in the registry. |

### Hot-reload behaviour

`FCoreUObjectDelegates::ReloadCompleteDelegate` (single param `EReloadCompleteReason`) triggers `RefreshAfterReload`, which walks every registry entry and re-resolves `WidgetClass` weak-pointers via `FindFirstObject<UClass>(Token)` for any that went null. Allowlist cache is invalidated as a side-effect so the next lookup rebuilds against the refreshed UClasses.

### Late-loading plugin widgets

`FMonolithUIModule::StartupModule` registers an `OnPostEngineInit` lambda that calls `UMonolithUIRegistrySubsystem::RescanWidgetTypes()` once the engine has finished plugin initialisation. This catches marketplace widget packs (CommonUI, third-party UMG plugins) whose UClasses load after stock UMG. The lambda is owned at module scope so `ShutdownModule` can cleanly unregister it.

---

## Effect Surface (M5 — Phase E + Optional Provider Decouple)

**Status:** Phase E widget shipped 2026-04-26; **optional provider decouple Wave 1/2 + Final.1 shipped 2026-04-27**. `M_EffectSurface_Base` material asset still deferred (tracked in `Plugins/Monolith/Docs/ROADMAP.md`).

`UEffectSurface` is the SDF-driven UMG widget that supersedes `URoundedBorder`. One Slate `FSlateDrawElement::MakeBox` per paint pass, all visual maths in the material domain (User Interface), MID-driven by a typed config struct. The widget lives in an optional provider plugin outside the public Monolith release. MonolithUI accesses it via reflective `UClass*` lookup only — there is no compile-time `#include` of any provider header from MonolithUI source. The curated mappings, allowlist coverage, and Phase F MCP action wrappers all live in MonolithUI; they reach `UEffectSurface` through `MonolithUI::GetEffectSurfaceClass()` + `FUIReflectionHelper::ApplyJsonPath` against the curated paths.

### Layout

| Component | Where | Responsibility |
|-----------|-------|----------------|
| `UEffectSurface : public UContentWidget` | Optional provider module | UMG wrapper, single content child, exposes `Config` UPROPERTY, owns the MID, opt-in `SBackgroundBlur` wrap when `Config.BackdropBlur.Strength > 0`. |
| `SEffectSurface : public SLeafWidget` | Optional provider module | Slate paint leaf, one `FSlateDrawElement::MakeBox` per `OnPaint`, brush `ResourceObject` points at the MID. |
| `FEffectSurfaceConfig` USTRUCT cluster | Optional provider module | Typed config: Shape, Fill, Border, DropShadow stack (cap 4), InnerShadow stack (cap 4), Glow, Filter, BackdropBlur, InsetHighlight, ContentPadding, FeatureFlags bitmask. |
| `M_EffectSurface_Base` content asset | **DEFERRED** — provider-specific material path | SDF material that consumes the MID parameters and renders the surface. Authored via Monolith `material_query` at edit time. Until landed, `BaseMaterial.LoadSynchronous()` returns nullptr and the SDF surface paints invisibly (widget still compiles + runs). |

### Hit-testing

`UEffectSurface` sets its own visibility to `SelfHitTestInvisible` so the SOverlay's hit-test passes through to the content child. The `SEffectSurface` leaf inside the overlay uses default `SLeafWidget` semantics — the overlay's pass-through covers it.

### MID lifecycle

- Created lazily on first `RebuildWidget` (or on the first setter call after an asset swap).
- `UPROPERTY()` on the widget so GC keeps it alive between paints.
- Released and re-created on `SetBaseMaterial(...)` — the parent material change invalidates the MID's parameter bindings.
- `#if WITH_EDITOR PostEditChangeProperty` flips `bMIDDirty=true` so editor preview updates without a full rebuild. A change to the `BaseMaterial` field forces a fresh MID.

### Curated mapping coverage (Phase E7)

Allowlist tokens registered for `EffectSurface` in `UMonolithUIRegistrySubsystem::RegisterCuratedMappings`:

| JSON path | Engine path | Sub-bag |
|-----------|-------------|---------|
| `Effect.Shape.CornerRadii`, `Effect.Shape.Smoothness` | `Config.Shape.*` | Shape |
| `Effect.Fill.Mode`, `Effect.Fill.SolidColor`, `Effect.Fill.Stops`, `Effect.Fill.Angle`, `Effect.Fill.RadialCenter` | `Config.Fill.*` | Fill |
| `Effect.Border.Width`, `Effect.Border.Color`, `Effect.Border.Offset`, `Effect.Border.Glow`, `Effect.Border.GlowColor` | `Config.Border.*` | Border |
| `Effect.DropShadow`, `Effect.InnerShadow` | `Config.DropShadow`, `Config.InnerShadow` | Shadow stacks |
| `Effect.Glow.Radius`, `Effect.Glow.Color`, `Effect.Glow.Intensity`, `Effect.Glow.InnerOuterMix` | `Config.Glow.*` | Glow |
| `Effect.Filter.Saturation`, `Effect.Filter.Brightness`, `Effect.Filter.Contrast` | `Config.Filter.*` | Filter |
| `Effect.BackdropBlur.Strength` | `Config.BackdropBlur.Strength` | Backdrop blur |
| `Effect.InsetHighlight.{Offset,Blur,Color,Intensity,EdgeMask}` | `Config.InsetHighlight.*` | Inset highlight |
| `Effect.ContentPadding`, `Effect.FeatureFlags`, `Effect.BaseMaterial` | top-level | Top-level |
| `Slot.*` (Canvas + Box mappings) | `Slot.*` | Parent-slot envelope |

### Backwards compatibility

`URoundedBorder` is marked `UCLASS(meta=(DeprecatedNode, DeprecationMessage="Use UEffectSurface instead."))` in Phase E8 but **NOT** removed. Existing WBPs that reference `URoundedBorder` continue to compile and render unchanged. A future automation action `ui::migrate_rounded_border_to_effect_surface(asset_path)` will convert WBP nodes in-place — out of scope for v1, tracked in Monolith ROADMAP.

### Material asset deferral

`M_EffectSurface_Base` is the SDF material that turns the MID parameter set into pixels. Its authoring requires the editor (Monolith `material_query("build_material_graph", ...)` only works when the editor is running), and Phase E was implemented offline. Therefore: this phase ships the C++ widget code with `TSoftObjectPtr<UMaterialInterface> BaseMaterial` defaulted to the planned asset path, but **does NOT** include the material asset itself. The widget compiles, links, and runs; the SDF surface paints invisibly until the asset lands. Material authoring is tracked as a follow-up task in `Plugins/Monolith/Docs/ROADMAP.md`.

### Tests

EffectSurface typed tests live with the optional provider module. Typed `UEffectSurface*` access is legal inside the module that defines the class, so assertion-side test code stays compact. MonolithUI exposes only the registry/spec/reflection APIs needed by provider-side tests; the reverse compile-time dependency is gone.

| Test | Location | Verifies |
|------|----------|----------|
| `MonolithUI.EffectSurface.SetCornerRadiiDirtiesMID` | Optional provider tests | E3 acceptance: `SetCornerRadii` flips `bMIDDirty`; next push writes matching `CornerRadii` vector parameter onto the MID. |
| `MonolithUI.EffectSurface.RegistryKnowsEffectSurface` | Optional provider tests | Reflection walk picks up `UEffectSurface` as a Content widget (single child). |
| `MonolithUI.EffectSurface.AllowlistCoversCuratedPaths` | Optional provider tests | Curated mapping coverage — every sub-bag has at least one allowlisted JSON path. |
| `MonolithUI.EffectSurfaceActions.*` (4 action-layer tests) | Optional provider tests | Action-handler smoke against a fixture WBP, with reflective post-state assertions through `FProperty::GetValue_InContainer` walks (not typed `Config.*` reads). All handlers route through `MonolithUI::IsEffectSurfaceAvailable()` first per § "Error Contract" — every test starts with the canonical skip-with-warning preamble. |
| `MonolithUI.AnimationRuntime.FadeInPlays` (was I4) | Optional provider tests | Runtime driver Tick + sample-at-time + stop-after-end. Moved out of `UIAnimationBuilderTests.cpp` (R5) so the MonolithUI-side builder test file no longer types any provider runtime symbol. Display name preserved for Session Frontend filter ergonomics. |

The Session Frontend display names are unchanged across the move; existing CI filters pinned to `MonolithUI.EffectSurface.*` / `MonolithUI.EffectSurfaceActions.*` / `MonolithUI.AnimationRuntime.FadeInPlays` keep working — the test display string is the public filter key, not the file path. The skip-with-warning pattern (per § "Error Contract — Test Surface (5.5.5)") means these tests **PASS-via-skip with `AddWarning`** when the optional provider is absent, never `return false`.

---

## Optional Dep Probe API (optional provider decouple — 2026-04-27)

**Status:** Shipped 2026-04-27. The single source of truth for "is the optional `UEffectSurface` provider reachable?" used by every action handler, the spec serializer, provider-side tests, and the spec-builder (via the upstream type-resolver).

### Surface

Declared in `Plugins/Monolith/Source/MonolithUI/Public/MonolithUICommon.h` inside `namespace MonolithUI`; implemented in `Plugins/Monolith/Source/MonolithUI/Private/MonolithUICommon.cpp`:

| Function | Returns | Behaviour |
|----------|---------|-----------|
| `MONOLITHUI_API UClass* GetEffectSurfaceClass()` | `UClass*` (or nullptr) | First call scans currently loaded widget classes for an `EffectSurface` provider class; subsequent calls hit a function-static `TWeakObjectPtr<UClass>` cache. Reset on `FCoreUObjectDelegates::ReloadCompleteDelegate` so Live Coding patches against the provider re-resolve cleanly. |
| `MONOLITHUI_API bool IsEffectSurfaceAvailable()` | `bool` | Convenience: `GetEffectSurfaceClass() != nullptr`. Shares the same function-static cache. |

### Boundary preservation

ZERO optional-provider headers in `MonolithUICommon.h` / `.cpp`. `UClass*` is a UE-core type; provider discovery is by loaded UClass name only. The hot-reload bind happens during `FMonolithUIModule::StartupModule` so the cache always reflects the current load state.

### Consumer surfaces

- **Action handlers** (`MonolithUIEffectActions.cpp`) — `ResolveEffectSurface` calls `IsEffectSurfaceAvailable()` first; on false, returns the canonical structured error (see § "Error Contract" below) before parsing `asset_path` / `widget_name`.
- **Serializer** (`UISpecSerializer.cpp::SerializeEffect`) — calls `GetEffectSurfaceClass()`; on null, silent no-op (`bOutHasEffect=false`, no entry in `Context.Errors`).
- **Tests** (the 7 EffectSurface tests + the moved I4 driver test, all in the optional provider module) — call `IsEffectSurfaceAvailable()` at the top of each `RunTest`; on false, `AddWarning(...)` + `return true` (PASS-via-skip).
- **Builder** (`UISpecBuilder.cpp` → `EffectSurfaceBuilder.cpp`) — already-clean per planner verification: when `type:"EffectSurface"` is unresolved at the type-resolver layer (`UISpecBuilder.cpp:375-386`), a structured `FUISpecError` (Category="Type") fires and `Constructed` is null; the per-property `EffectSurfaceBuilder::ApplyEffect` never runs.

### Non-conflict verification

The names `MonolithUI::GetEffectSurfaceClass` and `MonolithUI::IsEffectSurfaceAvailable` are unique across the entire MonolithUI source tree (verified via Grep during R3b.4 implementation). The `MonolithUI::` namespace exports `ParseColor`, `TryParseColor`, `LoadWidgetBlueprint`, `WidgetClassFromName`, `GetAnchorPreset`, `ParseHAlign`, `ParseVAlign`, `MakeTokenFromClassName`, `RegisterVariableName`, `RegisterCreatedWidget`, `ReconcileWidgetVariableGuids`, plus the new probe pair — no `Get*EffectSurface*`, `Is*EffectSurface*`, or `*Available*` collision.

---

## Error Contract — Optional EffectSurface Provider Absence (-32010)

**Status:** Shipped 2026-04-27 as §5.5 of the decouple plan. **Load-bearing** runtime contract for what happens when `MonolithUI::IsEffectSurfaceAvailable() == false` (the optional provider is physically absent, OR present but its module has not loaded yet, OR loaded but its `UEffectSurface` UClass cannot be resolved). This is the public-release reality.

### Error code allocation

`-32010` is allocated from the JSON-RPC server-defined range (`-32000..-32099`) for "an optional sibling/marketplace plugin the action depends on is not present in this build". Distinct from `-32603 ErrInternalError` so telemetry / LLM error counters do not conflate "feature gracefully unavailable" with "server choked".

| Constant | Value | Header |
|----------|-------|--------|
| `FMonolithJsonUtils::ErrOptionalDepUnavailable` | `-32010` | `Plugins/Monolith/Source/MonolithCore/Public/MonolithJsonUtils.h:53` |

The reserved range `-32011..-32019` is left open for future sibling-plugin codes (e.g. `ErrFeatureGated` if "missing plugin" later needs to split from "feature flag off"). Existing "X not available" sites in MonolithAI / MonolithAudio / MonolithGAS still default to `-32603` via `Error(const FString&)`; their migration to `-32010` is OUT OF SCOPE for the decouple — the rule is "new code uses `-32010`; existing code stays `-32603` until a separate hygiene pass".

### Action-handler contract (5.5.3) — LOUD

Trigger: any of the 10 EffectSurface action handlers (`set_effect_surface_corners`, `set_effect_surface_fill` (solid / linear / radial modes), `set_effect_surface_border`, `set_effect_surface_dropShadow`, `set_effect_surface_innerShadow`, `set_effect_surface_glow`, `set_effect_surface_filter`, `set_effect_surface_backdropBlur`, `set_effect_surface_insetHighlight`, `apply_effect_surface_preset`) invoked when `IsEffectSurfaceAvailable()` is false.

Behaviour: `ResolveEffectSurface` probes FIRST (before parsing `asset_path` / `widget_name` — fail-fast). On unavailable, returns the structured error built by `MonolithUIInternal::MakeOptionalDepUnavailableError` (single point of truth — never inlined per call site).

Canonical response shape:

```jsonc
{
  "bSuccess": false,
  "ErrorCode": -32010,
  "ErrorMessage": "EffectSurface widget unavailable — optional provider plugin not loaded. Install the provider or use ui::set_widget_property with a different widget type.",
  "Result": {
    "dep_name":     "EffectSurfaceProvider",
    "widget_type":  "EffectSurface",
    "alternative":  "ui::set_widget_property",
    "category":     "OptionalDepUnavailable"
  }
}
```

The `category` value mirrors the `FUIReflectionApplyResult::FailureReason` taxonomy elsewhere in the action surface (`PropertyNotFound | NotInAllowlist | ParseFailed | TypeMismatch`) — adds `OptionalDepUnavailable` as a sibling. LLM-greppable. Implementation: `Plugins/Monolith/Source/MonolithUI/Private/MonolithUIInternal.h:113`.

Invariants: NEVER a crash. NEVER a silent success. NEVER a different code. The structured `Result` payload is present even on failure (matches the `MakeErrorFromSpecError` Phase K convention). The literal substring `"EffectSurface widget unavailable"` is byte-stable across releases — tests assert that substring only, not the full message.

### Serializer contract (5.5.4) — SILENT

Trigger: `ui::dump_ui_spec` traverses a WBP and calls `SerializeEffect(Widget, OutEffect, bOutHasEffect)` for every node.

Behaviour: `SerializeEffect` calls `MonolithUI::GetEffectSurfaceClass()` first. When the class is null OR the widget is not an instance of it, `bOutHasEffect = false; return;` — silent no-op. NO entry added to `Context.Errors` or `Context.Warnings`. NO metadata field saying "skipped". The absence of the `effect` key in the resulting JSON IS the signal.

Why silent: most widgets in any tree are NOT EffectSurfaces. Emitting a "not an EffectSurface" warning per non-effect widget would flood the warnings list and break the `dump_ui_spec` LLM signal. A vanilla-sandbox WBP genuinely has zero EffectSurface widgets; the truthful serializer output is "no `effect` keys anywhere", regardless of optional-provider status. The action-layer contract (5.5.3) already surfaces "provider missing" loudly when the user tries to OPERATE on an EffectSurface — the serializer's job is to faithfully report current state.

Implementation reads via `HasAnyEffectConfigured` + `GetEffectSurfaceSpecPayload` `ProcessEvent` calls (R3b accessors on the provider side), then deserialises the returned JSON document into `FUISpecEffect` via `ReadJsonPath` + manual `FProperty` descent. Source: `Plugins/Monolith/Source/MonolithUI/Private/Spec/UISpecSerializer.cpp:540` onwards.

### Test contract (5.5.5) — SOFT (skip-with-warning)

Trigger: any automation test that types `UEffectSurface*` or asserts post-action EffectSurface state, when `IsEffectSurfaceAvailable() == false`.

Canonical pattern (verbatim — match `Plugins/Monolith/Source/MonolithUI/Private/Tests/Hoisted/SetRoundedCornersTests.cpp:98-105`):

```cpp
bool FFooTest::RunTest(const FString& Parameters)
{
    if (!MonolithUI::IsEffectSurfaceAvailable())
    {
        AddWarning(TEXT("Skipping: optional EffectSurface provider not present (UEffectSurface class unresolvable)."));
        return true;  // PASS-via-skip, NOT FAIL
    }
    // ... rest of test ...
}
```

`return true;` reports the test as PASSED-with-warning to the automation runner. Session Frontend filter discovery still finds the test by display name. NEVER `return false` — that would be a regression of the public release contract. Use the literal `TEXT("Skipping: ...")` form (no `FString::Printf` needed).

### Builder contract (5.5.6) — LOUD via existing type-resolver

Trigger: `ui::build_ui_from_spec` includes a node with `type:"EffectSurface"` when the optional provider is absent.

Behaviour (existing — verified at `UISpecBuilder.cpp:353-386` + `:419-423`): `ResolveWidgetClass(Node, *RegistryPtr)` returns nullptr because no entry exists for the absent UClass. The builder emits a structured `FUISpecError` with `Severity=Error, Category="Type", WidgetId=Node.Id, Message="Could not resolve UClass for node '<id>' (type='EffectSurface', customClass='')."`. The `if (!Constructed)` guard short-circuits — `EffectSurfaceBuilder::ApplyEffect` is NEVER called when construction fails. NO change required to the builder — the existing structured-error path IS the contract.

### Probe staleness mitigation

The weak-ptr cache could go stale if the optional provider is loaded mid-session AFTER the cache populated to nullptr (e.g., user enables the plugin in editor settings without restart). Mitigated by binding `FCoreUObjectDelegates::ReloadCompleteDelegate` at MonolithUI module startup — the plugin-enable path triggers a reload-complete cycle which resets the cache. Belt-and-suspenders: `TWeakObjectPtr` semantics mean if the UClass were ever GC'd, `Cached.Get()` returns null and re-resolution kicks in.

### Summary table — contract by surface

| Surface | Trigger | Behaviour | Code | Visibility |
|---------|---------|-----------|------|-----------|
| Action handler | Any `set_effect_surface_*` / `apply_effect_surface_preset` | Return structured error via `MakeOptionalDepUnavailableError` | `-32010 ErrOptionalDepUnavailable` | LOUD — explicit error response with `dep_name`/`widget_type`/`alternative` payload |
| Serializer | `dump_ui_spec` traverses a node | Silent no-op (`bOutHasEffect=false`) | n/a | SILENT — absence of `effect` field IS the signal |
| Test | Test body runs without optional provider | `AddWarning` + `return true` | n/a | SOFT — passes with warning, automation runner sees SKIPPED-via-warning |
| Builder | `build_ui_from_spec` with `type:"EffectSurface"` node | Existing Type-category error from type-resolver (shipped pre-decouple) | n/a (Builder uses `FUISpecError`, not action codes) | LOUD — structured `validation` payload with category="Type" |
| Probe | `MonolithUI::IsEffectSurfaceAvailable()` | Returns `false` | n/a | Direct caller-visible — the truth-source |

### Pattern reusability for future siblings

The probe / error-code / handler / serializer / test / builder sextuple is the canonical pattern for any future MonolithUI optional dep. To onboard a new sibling-plugin widget type:

1. Add a `MonolithUI::GetXClass()` + `MonolithUI::IsXAvailable()` pair to `MonolithUICommon.h/.cpp` — same function-static `TWeakObjectPtr<UClass>` cache + `ReloadCompleteDelegate` reset.
2. Reuse `FMonolithJsonUtils::ErrOptionalDepUnavailable` (`-32010`) — only allocate a new code when the contract semantics change.
3. Add a `MakeOptionalDepUnavailableError` call site at the resolver entry of every action handler — single point of truth.
4. Serializer silent-no-op on null UClass (no `Context.Errors` entry).
5. Tests skip-with-warning + `return true`.
6. Builder leans on the type-resolver's existing `FUISpecError` Category="Type" path.

---

## Design Import Actions (M5 — Phase D Hoist)

**Status:** Phase D shipped 2026-04-26.

The 10 generic verbs below were hoisted into MonolithUI under the `ui::` namespace from an earlier private bridge implementation. Legacy sibling namespaces may keep deprecated alias entries for one release; those aliases should re-dispatch into `ui::` and tag the response with `{ "deprecated": true, "use_action": "ui::<Name>" }`. Removal is scheduled for Phase L.

The actions are generic UE-UI verbs: ingesting an image as a UTexture2D, importing a font family as a UFont composite, authoring weighted-tangent UMG animations, baking a damped spring into linear keyframes, attaching a master event track to a UWidgetAnimation, wiring a widget interaction event to play an animation, writing reflection-based rounded-corner properties, compositing sibling box-shadow widgets, and producing a UMaterialInstanceConstant from a parametrised gradient spec. None of them carries caller-specific bridge semantics; the parent material / asset paths flow in via params.

| Action | Params (summary) | Description |
|--------|------------------|-------------|
| `import_texture_from_bytes` | `destination`, `bytes_b64`, `format_hint`, `settings?`, `save?` | Decode a base64-encoded image (PNG / JPEG / BMP / EXR / TGA / HDR / TIFF / DDS) and import as a UTexture2D at `/Game/...`. Source data captured for editor save-to-disk; `bytes_b64` MUST be base64-encoded compressed bytes (NOT raw pixels). Returns `{asset_path, width, height, size_bytes}`. |
| `import_font_family` | `destination`, `family_name`, `faces[]`, `loading_policy?`, `hinting?`, `save?` | Import a font family as a UFont composite + one UFontFace per typeface entry. Per-face errors don't abort the batch — failed faces appear in the `warnings` array. Uses `UFont::GetMutableInternalCompositeFont()` (UE 5.7 deprecation-safe accessor for the composite-font field). |
| `create_animation_v2` | `asset_path`, `animation_name`, `duration_sec`, `tracks[]`, `compile_once?` | Authors a UWidgetAnimation with multi-track, multi-key data. Cubic / linear / constant interpolation; cubic supports tangent weights for CSS bezier easing reproduction. Idempotent — re-runs against the same `(asset_path, animation_name)` reset existing channel keys. |
| `add_bezier_eased_segment` | `asset_path`, `animation_name`, `widget_name`, `property`, `from_value`, `to_value`, `start_time`, `end_time`, `bezier[4]` | Convenience wrapper that converts a CSS `cubic-bezier(x1,y1,x2,y2)` control-point pair into UE weighted tangents and inserts a 2-key segment. Returns `{tangent_info: {key0_leave_*, key1_arrive_*}}` for diagnostics. |
| `bake_spring_animation` | `asset_path`, `animation_name`, `widget_name`, `property`, `from_value`, `to_value`, `stiffness`, `damping`, `mass`, `fps?`, `duration?`, `compile_once?` | Bakes a damped harmonic spring into dense linear keyframes via semi-implicit Euler integration with convergence early-out. `early_settled` is true when the simulation converged before `duration`. |
| `add_animation_event_track` | `asset_path`, `animation_name`, `events[]` | Adds a master `UMovieSceneEventTrack` to an existing UWidgetAnimation and inserts timed `FMovieSceneEvent` keys. The animation must already exist (created by `create_animation_v2` or equivalent). |
| `bind_animation_to_event` | `asset_path`, `animation_name`, `widget_event`, `animation_event?` | Creates a `UWidgetAnimationDelegateBinding` entry that wires a widget interaction event (`OnHovered` / `OnUnhovered` / `OnPressed` / `OnReleased` / `OnFocusReceived` / `OnFocusLost`) to play a named animation on Started or Finished. Stored on the WBP's generated class. |
| `set_rounded_corners` | `asset_path`, `widget_name`, `corner_radii[4]?`, `outline_color?`, `outline_width?`, `fill_color?`, `compile?` | Reflection-based writer for `CornerRadii` (FVector4) / `OutlineColor` (FLinearColor) / `OutlineWidth` (float) / `FillColor` (FLinearColor) UPROPERTYs on a named widget. ZERO compile-time dep on any specific widget class — works on any widget exposing compatible UPROPERTY names + types. Partial success is OK; missing/incompatible properties surface as warnings. |
| `apply_box_shadow` | `asset_path`, `widget_name`, `shadow_material_path`, `shadow?` OR `shadows[]` (capped at 2), `shadow_mid_destination?`, `target_size?`, `compile?` | Inserts sibling UImage widget(s) BEHIND the target in an authored UMG tree, each backed by a transient (or saved-MID) MaterialInstance parented to a caller-supplied shadow material. Required shadow-material params: `ShadowColor` vector + `BlurRadius` scalar. Optional: `Offset` vector, `Spread` scalar, `Inset` scalar, `ShadowSize` vector. Multi-layer cap is 2; extra layers are dropped with a warning (CSS-equivalent rendering order is preserved). If insertion fails, the transient shadow widget is retired before returning so WBP compile validation is not poisoned by orphan variables. |
| `create_gradient_mid_from_spec` | `parent_material`, `destination`, `spec`, `save?` | Parameter-driven gradient MID factory. Creates a UMaterialInstanceConstant from a caller-supplied parent material exposing `StopNPos` scalar + `StopNColor` vector + `UseStopN` static-switch params (and optionally `Angle` scalar, `CornerRadii`/`WidgetSize` for material-level SDF rounding). Supports 1–8 stops. Validates parent exposes `Stop0Pos`/`Stop0Color` BEFORE creating any asset — returns -32602 on incompatible parent without leaving a half-built MIC on disk. |

### Source layout

| Path | Contents |
|------|----------|
| `Source/MonolithUI/Private/Actions/Hoisted/TextureIngestActions.{h,cpp}` | `MonolithUI::FTextureIngestActions` |
| `Source/MonolithUI/Private/Actions/Hoisted/FontIngestActions.{h,cpp}` | `MonolithUI::FFontIngestActions` |
| `Source/MonolithUI/Private/Actions/Hoisted/AnimationCoreActions.{h,cpp}` | `MonolithUI::FAnimationCoreActions` (3 actions) |
| `Source/MonolithUI/Private/Actions/Hoisted/AnimationEventActions.{h,cpp}` | `MonolithUI::FAnimationEventActions` (2 actions) |
| `Source/MonolithUI/Private/Actions/Hoisted/RoundedCornerActions.{h,cpp}` | `MonolithUI::FRoundedCornerActions` |
| `Source/MonolithUI/Private/Actions/Hoisted/ShadowActions.{h,cpp}` | `MonolithUI::FShadowActions` |
| `Source/MonolithUI/Private/Actions/Hoisted/GradientActions.{h,cpp}` | `MonolithUI::FGradientActions` |
| `Source/MonolithUI/Private/Tests/Hoisted/*Tests.cpp` | Test category `MonolithUI.<ActionName>.*` |
| `Source/MonolithUI/Private/Tests/Hoisted/MonolithUITestFixtureUtils.h` | Throwaway-WBP construction helpers under `MonolithUI::TestUtils` |

### Build.cs deps added by Phase D

`ImageWrapper`, `ImageCore` (texture decode), `AssetTools` (`CreateUniqueAssetName`), `Kismet` (`MarkBlueprintAsStructurallyModified`), `MaterialEditor` (`UMaterialEditingLibrary::UpdateMaterialInstance`).

### Backwards-compatibility window

Legacy sibling UI aliases re-dispatch into the canonical `ui::<NAME>` action via `FMonolithToolRegistry::Get().ExecuteAction("ui", ...)` (NOT a direct static-handler call — the registry path is the single validated dispatch site). Their response payload is tagged `{ "deprecated": true, "use_action": "ui::<NAME>" }`. A future cleanup will remove those aliases from their owning sibling plugin.

---

## Animation v3 (M5 — Phase I)

**Status:** Phase I shipped 2026-04-26. Editor backend + runtime backend + preserve-on-rebuild policy.

The Phase D-hoisted action surface (`create_animation_v2`, `add_bezier_eased_segment`, `bake_spring_animation`, `add_animation_event_track`, `bind_animation_to_event`) is preserved verbatim — every existing caller and every existing automation test stays green. Phase I adds a **declarative** `FUISpecAnimation` driven by two backends so the same author intent can drive editor MovieScene **and** pure-runtime playback without an authoring round-trip.

### Backend split

| Backend | Module | Trigger | Output |
|---------|--------|---------|--------|
| `FUIAnimationMovieSceneBuilder` | MonolithUI (editor-only) | `Build(FUISpecDocument, UWidgetBlueprint*, FUIAnimationBuildOptions)` | `UWidgetAnimation` on `UWidgetBlueprint::Animations`, full MovieScene tree (binding → float track → section → channel) |
| `FUIAnimationRuntimeDriver` | Optional provider runtime | `SetTracks(...)`, `Play(name)`, `Tick(dt)` | Live `UWidget` property writes via UE reflection (FFloatProperty / FDoubleProperty) |

The editor backend is the preferred path for cooked games — `UWidgetAnimation` survives compile and replicates Slate's tick. The runtime driver exists for the niche where a spec arrives at runtime (data-driven UI) without an editor compile loop.

### Spec-side surface

`FUISpecAnimation` (in `Spec/UISpec.h`) carries one widget-id + one property + ordered `FUISpecKeyframe`s OR the `from`/`to`/`Duration` sugar shape. The MovieScene builder honours both: explicit Keyframes always win; sugar expands to a 2-key cubic CSS-ease-in-out segment when Keyframes is empty and `Duration > 0`.

`FUIAnimationBuildOptions` (in `Animation/UISpecAnimation.h`) controls reconciliation:

| Field | Default | Effect |
|-------|---------|--------|
| `Policy` | `Recreate` | Existing same-name UWidgetAnimation is destroyed + rebuilt. `MergeAdditive` reserved for v2. |
| `PreserveAnimationsNamed` | `[]` | Each name in this list is a hard veto — the existing animation stays untouched even if the spec also defines it. The opt-in for incremental authoring. |
| `bCompileBlueprint` | `true` | Run `FKismetEditorUtilities::CompileBlueprint` once at end of Build. Set false to batch multiple builders. |
| `bDryRun` | `false` | Validate-only — no UWidgetAnimation is mutated. The result bag still reports what *would* have happened. |

### Reuse of Phase D helpers

The MovieScene builder calls a public `MonolithUI::` helper surface in `Actions/Hoisted/AnimationCoreActions.h` (lifted from the .cpp's anonymous namespace by Phase I). Single source of truth for: CSS-bezier-to-weighted-tangent math (`ComputeBezierWeightedTangents`), find-or-create animation (`FindOrCreateWidgetAnimation`), possessable binding (`EnsureWidgetPossessableBinding`), track + section + channel ensure (`EnsureFloatTrackSectionChannel`), key insertion (`InsertFloatChannelKey`). The action handlers continue to call the same helpers via the .cpp-internal namespace; the public surface is just a forwarding mirror.

### Runtime driver design

`FUIAnimationRuntimeDriver` is a `USTRUCT(BlueprintType)` deliberately not `FCurveSequence`-backed: `FCurveSequence` is Slate-tick-driven and opaque about its time cursor (the curve handle exposes only `GetLerp`). A standalone time-accumulator is more testable, more explicit, and trivially extensible to vector / color tracks later. Sampling: linear / Hermite cubic / constant — same trio as the editor backend, math is OUR derivation from the standard Hermite basis (`h00`, `h10`, `h01`, `h11`) with tangents scaled by segment span (UE stores tangents as value-per-second).

The driver writes resolved scalars to the host widget's named property via UE reflection (`UClass::FindPropertyByName` → `FFloatProperty::SetPropertyValue_InContainer`). Vector / color tracks: deferred to a future expansion.

### Source layout

| Path | Contents |
|------|----------|
| `Plugins/Monolith/Source/MonolithUI/Public/Animation/UISpecAnimation.h` | `FUIAnimationBuildOptions`, `FUIAnimationRebuildResult`, `EUIAnimationRebuildPolicy` |
| `Plugins/Monolith/Source/MonolithUI/Public/Animation/UIAnimationMovieSceneBuilder.h` / `Private/Animation/UIAnimationMovieSceneBuilder.cpp` | `FUIAnimationMovieSceneBuilder` |
| `Plugins/Monolith/Source/MonolithUI/Private/Actions/Hoisted/AnimationCoreActions.{h,cpp}` | Phase I lifted the CSS-bezier math + binding/track helpers from `AnimationInternal::` (.cpp) into a public `MonolithUI::` surface (.h) so the builder can reuse them. Existing action handlers still call the .cpp helpers; the public surface forwards. |
| Optional provider runtime driver files | `FUIAnimationRuntimeDriver`, `FUIAnimationRuntimeTrack`, `FUIAnimationRuntimeKey`, `EUIAnimationRuntimeInterp` |
| `Plugins/Monolith/Source/MonolithUI/Private/Tests/UIAnimationBuilderTests.cpp` | Test category `MonolithUI.AnimationBuilder.*` and `MonolithUI.AnimationRuntime.*` |

### Build.cs deps added by Phase I

None new. `MovieScene` and `MovieSceneTracks` were already in MonolithUI's PrivateDependencyModuleNames (Phase D). The runtime driver lives outside public Monolith; MonolithUI reaches optional runtime surfaces only through exported generic structs and reflection.

### `build_ui_from_spec` integration

Phase I deliberately defers the wiring of the spec-driven builder into `build_ui_from_spec` to **Phase H** — that action does not exist yet. The MovieScene builder is ready (`Build(...)` is callable from any action handler that lands an `FUISpecDocument`); Phase H will register the public action that consumes both the widget-tree and animation surfaces in one call. Until then, `create_animation_v2` and the rest of the v2 action surface remain the LLM-facing entry points.

### Test coverage

| Test | Verifies |
|------|----------|
| `MonolithUI.AnimationBuilder.MovieSceneOpacityTrack` | Build produces a `UWidgetAnimation` with a float track on `RenderOpacity`, 2 keys, correct values |
| `MonolithUI.AnimationRuntime.FadeInPlays` | Runtime driver advances opacity 0 → 1 over 10 ticks of 0.1s, mid-curve sample at 0.5s ≈ 0.5, IsPlaying false after end |
| `MonolithUI.AnimationBuilder.PreserveNamedSurvivesRebuild` | After two builds with the same animation name (5x duration delta), `PreserveAnimationsNamed=["Preserved"]` keeps UObject identity + key count unchanged |

---

## Style Service (M5 — Phase G)

**Status:** Phase G shipped 2026-04-26. CommonUI-only (`#if WITH_COMMONUI`).

`FMonolithUIStyleService` is a process-singleton (held by `TUniquePtr` in `MonolithUIStyleService.cpp`, lifecycle anchored to `FMonolithUIModule`) that dedupes CommonUI style assets — `UCommonButtonStyle` / `UCommonTextStyle` / `UCommonBorderStyle` Blueprints created via the class-as-data pattern. Every call to `create_common_button_style` / `create_common_text_style` / `create_common_border_style` now flows through the service before any disk I/O.

### Resolution chain

The service walks three steps for every `ResolveOrCreate(StyleClass, AssetName, PackagePath, Properties)` call:

1. **Cache by name** — instant hit when the same `(name, content-hash)` pair was returned before.
2. **Canonical library lookup** — scans `UMonolithUISettings::CanonicalLibraryPath` (default `/Game/UI/Library/`) for a same-typed asset with the same name. Pre-authored library styles ALWAYS win over auto-generated ones.
3. **Cache by hash** — instant hit when the same property bag came in before under a different name. This is the dedup that matters for LLM-driven workflows: two specs that say "button: red, 14pt" produce ONE style asset.

Misses fall through to creation. The new asset is created at `UMonolithUISettings::GeneratedStylesPath` (default `/Game/UI/Styles/`) using `IAssetTools::CreateUniqueAssetName` to dodge name collisions. The created `_C` class is what `apply_style_to_widget` / `batch_retheme` consume.

### Hash function

`FMonolithUIStyleService::ComputeContentHash` canonicalises the JSON property bag (sort field names, `%.6f` for floats, UTF-8 byte serialisation) and feeds the result into `FCrc::MemCrc32`. The class name is mixed into the canonical buffer so identical bags against different style types (e.g. Button{ColorAndOpacity:white} vs Text{ColorAndOpacity:white}) hash distinctly.

CRC32's ~1-in-2^32 collision probability is comfortably acceptable below ~10k unique styles. If a project's shipped style library crosses that threshold, swap to `FXxHash64::HashBuffer` and widen `HashIndex` to `TMap<uint64, ...>` — pre-emptive migration is cheaper than retrofitting after style names embed the hex.

### Settings (`UMonolithUISettings`)

Project Settings → Plugins → Monolith UI surfaces the following config entries (stored under `[/Script/MonolithUI.MonolithUISettings]` in `DefaultGame.ini`):

| Field | Default | Used by |
|-------|---------|---------|
| `GeneratedStylesPath` | `/Game/UI/Styles/` | Style service — folder for new auto-generated assets |
| `CanonicalLibraryPath` | `/Game/UI/Library/` | Style service — folder scanned in resolution step 2 |
| `StyleCacheCap` | 200 | Style service — LRU eviction trigger |
| `MaxNestingDepth` | 32 | Phase H validator — caps spec tree depth |
| `PathCacheCap` | 256 | Phase C path cache — TODO: cache constructor reads this value (currently hardcoded) |

`UDeveloperSettings::OnSettingChanged` live-refresh wiring is **deferred** until Phase E's `MonolithUIRegistrySubsystem.cpp` curated-mapping work lands (Phase G could not safely touch the same .cpp file in parallel). Until that follow-up patch, edits in Project Settings require an editor restart to take effect.

### Action surface preserved

The four migrated actions retain their input and response shapes — additive fields only:

| Action | Input | Response (additive fields in **bold**) |
|--------|-------|----------------------------------------|
| `create_common_button_style` | `package_path`, `asset_name`, `properties?` | `asset_path`, `asset_name`, `class`, **`resolved_via`**, **`was_created`** |
| `create_common_text_style` | same | same |
| `create_common_border_style` | same | same |
| `apply_style_to_widget` | `wbp_path`, `widget_name`, `style_asset` | unchanged — consumes resolved class path |
| `batch_retheme` | `folder_path`, `old_style`, `new_style` | unchanged — operates on pre-resolved paths |

`resolved_via` is one of `name_cache` / `library` / `hash_cache` / `created`. `was_created` is true only on the fall-through path.

### Diagnostics

`ui::dump_style_cache_stats` (zero params) returns:

```json
{
  "cache_size": 12,
  "hits": 47,
  "misses": 12,
  "evictions": 0,
  "by_type": { "Button": 5, "Text": 4, "Border": 3 }
}
```

### Singleton ownership rationale

The service is a static singleton (not a `UEditorSubsystem` member), held by `TUniquePtr` in `MonolithUIStyleService.cpp` and torn down from `FMonolithUIModule::ShutdownModule`. This avoids the parallel-conflict on `MonolithUIRegistrySubsystem.cpp` (Phase E owns that file for B7 curated-mapping additions). The trade-off: no automatic registry-driven lifecycle, but the service has no GC-tracked state beyond `TStrongObjectPtr<UClass>` entries (which the explicit `Shutdown()` releases cleanly), so there's nothing the subsystem framework would have done for free.

### Source layout

| Path | Contents |
|------|----------|
| `Source/MonolithUI/Public/MonolithUISettings.h` / `Private/MonolithUISettings.cpp` | `UMonolithUISettings` (UDeveloperSettings) |
| `Source/MonolithUI/Public/Style/MonolithUIStyleService.h` / `Private/Style/MonolithUIStyleService.cpp` | `FMonolithUIStyleService`, `FUIStyleEntry`, `FUIStyleResolution`, `FUIStyleCacheStats` |
| `Source/MonolithUI/Private/Tests/UIStyleServiceTests.cpp` | Test category `MonolithUI.StyleService.*` |

### Build.cs deps added by Phase G

`DeveloperSettings` (UDeveloperSettings parent class — lives in its own module, NOT `Engine`).

---

## Known Follow-ups (post Phase L — 2026-04-26)

These are tracked drift / deferred work that did not block Phase L closure:

| # | Item | Notes |
|---|------|-------|
| 1 | `M_EffectSurface_Base` material asset (WISHLIST) | Phase E shipped the C++ widget against `TSoftObjectPtr<UMaterialInterface> BaseMaterial` defaulted to a provider-specific material path. Authoring requires editor + Monolith `material_query("build_material_graph", ...)`. Until the asset lands, the SDF surface paints invisibly (widget compiles + runs). Tracked in `Plugins/Monolith/Docs/ROADMAP.md`. |
| 2 | `URoundedBorder` removal | Marked `UCLASS(meta=(DeprecatedNode))` Phase E8. Not removed yet — existing WBPs continue to render unchanged. A future `ui::migrate_rounded_border_to_effect_surface(asset_path)` action will convert nodes in-place. |
| 3 | ~~optional provider .uplugin dep declaration~~ | **RESOLVED 2026-04-27** — superseded by the optional provider decouple. The pragmatic fix landed: zero compile-time dep on the provider + reflective `MonolithUI::GetEffectSurfaceClass()` probe. The missing-plugin dependency warning no longer fires because there is no warrant for the dep — MonolithUI does not include any provider header. See § "Optional Dep Probe API" + § "Error Contract — Optional EffectSurface Provider Absence (-32010)". |
| 4 | Legacy sibling UI alias removal | 10 aliases in a private bridge module re-dispatch into `ui::` and tag responses `{deprecated: true}`. Plan: keep one major release for downstream catch-up, then remove. Phase L marker present in code + spec. |
| 5 | `ui::create_animation` / `ui::add_animation_keyframe` removal | Same one-major-release window as the legacy sibling UI aliases. Description prefixed `[DEPRECATED]`; response payload tagged `{deprecated: true, use_action: "ui::create_animation_v2"}`. |
| 6 | `UDeveloperSettings::OnSettingChanged` live-refresh wiring | Deferred from Phase G to avoid a parallel-edit conflict with the Phase E curated-mapping work in `MonolithUIRegistrySubsystem.cpp`. Until landed, edits in Project Settings → Plugins → Monolith UI require an editor restart. |
| 7 | `FUIPropertyPathCache::Cap` honours `UMonolithUISettings::PathCacheCap` | Currently hardcoded; the settings field exists but the cache constructor does not yet read it. Trivial follow-up. |
| 8 | Vector / Color tracks for `FUIAnimationRuntimeDriver` | Phase I shipped scalar-only. Vector / color is a deliberate v2 expansion. |
| 9 | JSON-path → engine-path helper migration | Phase F per-handler `FEffectSurfaceActions::ApplyPath` is now a thin wrapper around `FUIReflectionHelper::ApplyJsonPath` (the Phase H hoist). Other handlers can migrate at leisure — the wrapper-and-hoist pattern is canonical going forward. |
| 10 | Preset-literal drift detector (post-decouple R2.9) | The R2.9 metadata table in `MonolithUIEffectActions.cpp` (6 named presets — `rounded-rect`, `pill`, `circle`, `glass`, `glowing-button`, `neon`) and the provider's canonical presets MUST stay byte-aligned. Currently relies on manual diff in PR review. Future automation: a provider-side unit test that round-trips each preset name through both code paths and asserts pixel-identical render. Tracked here so the next post-decouple audit picks it up. |
| 11 | EEffectSurfaceFeature drift detector (post-decouple R2.5) | Raw `int32` constants `EffectFeature_RoundedCorners = 1<<0` … `EffectFeature_InsetHighlight = 1<<10` in `MonolithUIEffectActions.cpp` shadow the provider's canonical feature enum. Same drift class as #10 — manual diff today; would be neat to gate with a provider-side static assert. |

Items 4 + 5 are the only Phase L deprecation markers; items 10 + 11 were added by the optional provider decouple (2026-04-27). The rest are drift the audit caught while sweeping the spec but predate the relevant phase.

---
