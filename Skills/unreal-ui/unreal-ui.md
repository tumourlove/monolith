---
name: unreal-ui
description: Use when working with Unreal Engine UI via Monolith MCP — creating widget blueprints, building HUDs, menus, settings panels, styling, animations, data binding, save systems, and accessibility. Triggers on UI, HUD, widget, menu, settings, save game, accessibility, font, anchor, toast, dialog, loading screen.
---

# Unreal UI Workflows

**92 UI actions** via `ui_query()` (42 UMG baseline + 50 CommonUI conditional on CommonUI plugin). Discover with `monolith_discover({ namespace: "ui" })`. Filter to CommonUI only: `monolith_discover({ namespace: "ui", category: "CommonUI" })`.

**CommonUI actions require the CommonUI engine plugin** (stock UE 5.7, `Engine/Plugins/Runtime/CommonUI/`). When absent, 50 actions silently unregister. Detect via `monolith_discover`.

## Key Parameters

- `asset_path` -- Widget Blueprint path (e.g. `/Game/UI/WBP_MyWidget`)
- `save_path` -- destination for new WBP assets
- `widget_name` / `widget_class` -- widget name in tree / type (`TextBlock`, `Image`, `Button`, etc.)
- `parent_name` -- parent panel (omit for root)
- `anchor_preset` -- `center`, `top_left`, `stretch_fill`, etc.

## Action Reference

| Action | Key Params | Purpose |
|--------|-----------|---------|
| **Widget CRUD (7)** | | |
| `create_widget_blueprint` | `save_path`, `parent_class`?, `root_widget`? | Create new WBP |
| `get_widget_tree` | `asset_path` | Full hierarchy as JSON |
| `add_widget` | `asset_path`, `widget_class`, `widget_name`?, `parent_name`?, `anchor_preset`?, `position`?, `size`? | Add widget to panel |
| `remove_widget` | `asset_path`, `widget_name` | Remove from tree |
| `set_widget_property` | `asset_path`, `widget_name`, `property_name`, `value` | Set any UPROPERTY |
| `compile_widget` | `asset_path` | Compile WBP |
| `list_widget_types` | `filter`? | Available widget classes |
| **Slot & Layout (3)** | | |
| `set_slot_property` | `asset_path`, `widget_name`, `anchors`?, `offsets`?, `position`?, `size`?, `z_order`?, `padding`? | Any slot property |
| `set_anchor_preset` | `asset_path`, `widget_name`, `preset` | Named anchor preset |
| `move_widget` | `asset_path`, `widget_name`, `new_parent_name` | Reparent widget |
| **Templates (8)** | | |
| `create_hud_element` | `asset_path`, `element_type` | crosshair, health_bar, ammo_counter, stamina_bar, interaction_prompt, damage_indicator, compass, subtitles, flashlight_battery |
| `create_menu` | `save_path`, `menu_type`, `buttons`? | main_menu, pause_menu, death_screen, credits |
| `create_settings_panel` | `save_path`, `tabs`? | Tabbed settings (graphics, audio, controls, gameplay, accessibility) |
| `create_dialog` | `save_path`, `title`?, `body`?, `confirm_text`?, `cancel_text`? | Confirmation dialog |
| `create_notification_toast` | `save_path`, `position`? | Toast widget |
| `create_loading_screen` | `save_path`, `show_progress`?, `show_tips`?, `show_spinner`? | Loading screen |
| `create_inventory_grid` | `save_path`, `columns`?, `rows`?, `slot_size`? | Inventory grid |
| `create_save_slot_list` | `save_path`, `max_slots`? | Save slot selector |
| **Styling (6)** | | |
| `set_brush` | `asset_path`, `widget_name`, `property_name`, `draw_type`?, `tint_color`?, `corner_radius`?, `texture_path`? | Configure brush |
| `set_font` | `asset_path`, `widget_name`, `font_size`?, `typeface`?, `outline_size`?, `outline_color`? | Font on text widgets |
| `set_color_scheme` | `colors` | EStyleColor User1-16 palette |
| `batch_style` | `asset_path`, `widget_class`, `property_name`, `value` | Apply to all widgets of class |
| `set_text` | `asset_path`, `widget_name`, `text`, `text_color`?, `font_size`?, `justification`? | Convenience text setter |
| `set_image` | `asset_path`, `widget_name`, `texture_path`?, `material_path`?, `tint_color`?, `size`? | Convenience image setter |
| **Animation (5)** | | |
| `list_animations` | `asset_path` | List UWidgetAnimations |
| `get_animation_details` | `asset_path`, `animation_name` | Tracks, sections, keyframes |
| `create_animation` | `asset_path`, `animation_name`, `duration`, `tracks` | Create with keyframed tracks |
| `add_animation_keyframe` | `asset_path`, `animation_name`, `widget_name`, `property`, `time`, `value` | Add keyframe |
| `remove_animation` | `asset_path`, `animation_name` | Remove animation |
| **Data Binding (4)** | | |
| `list_widget_events` | `asset_path`, `widget_name`? | Bindable events |
| `list_widget_properties` | `asset_path`, `widget_name` | Settable properties with types |
| `setup_list_view` | `asset_path`, `list_widget_name`, `entry_widget_class`?, `entry_height`? | Configure ListView/TileView |
| `get_widget_bindings` | `asset_path` | All property bindings |
| **Settings & Save Scaffolding (5)** | | |
| `scaffold_game_user_settings` | `class_name`, `module_name`, `features`? | UGameUserSettings subclass C++ |
| `scaffold_save_game` | `class_name`, `module_name`, `properties`? | ULocalPlayerSaveGame subclass C++ |
| `scaffold_save_subsystem` | `class_name`, `module_name`, `save_game_class` | Save management subsystem C++ |
| `scaffold_audio_settings` | `categories`? | Audio settings wiring info |
| `scaffold_input_remapping` | `actions`? | Keybinding remapping setup |
| **Accessibility (4)** | | |
| `scaffold_accessibility_subsystem` | `class_name`, `module_name` | Accessibility settings subsystem C++ |
| `audit_accessibility` | `asset_path` | Audit font size, focus, navigation, tooltips |
| `set_colorblind_mode` | `mode`, `severity`?, `correct`? | Colorblind correction (runtime) |
| `set_text_scale` | `scale` | UI text scale (runtime) |

## Anchor Presets

`top_left`(0,0,0,0) `top_center`(0.5,0,0.5,0) `top_right`(1,0,1,0) `center_left`(0,0.5,0,0.5) `center`(0.5,0.5,0.5,0.5) `center_right`(1,0.5,1,0.5) `bottom_left`(0,1,0,1) `bottom_center`(0.5,1,0.5,1) `bottom_right`(1,1,1,1) `stretch_fill`(0,0,1,1) `stretch_horizontal`(0,0.5,1,0.5) `stretch_vertical`(0.5,0,0.5,1)

## Typical Workflow: Build a HUD

```
1. ui_query("create_widget_blueprint", {"save_path": "/Game/UI/WBP_GameHUD"})
2. ui_query("create_hud_element", {"asset_path": "/Game/UI/WBP_GameHUD", "element_type": "health_bar"})
3. ui_query("create_hud_element", {"asset_path": "/Game/UI/WBP_GameHUD", "element_type": "crosshair"})
4. ui_query("set_font", {"asset_path": "/Game/UI/WBP_GameHUD", "widget_name": "ammo_counter_Current", "font_size": 28, "typeface": "Bold"})
```

## Horror UI + Accessibility Guidelines

**Horror:**
- No health bar -- use vignette + desaturation + heartbeat audio via post-process
- Minimal HUD -- auto-hide when full (stamina, etc.)
- Subtitles ON by default -- caption ALL sounds
- Diegetic where possible -- flashlight battery on prop, not overlay
- No minimap -- navigation uncertainty = horror
- Short interaction trace -- must get close

**Accessibility (critical -- hospice patients):**
- **Atkinson Hyperlegible** font at `Content/UI/Fonts/Atkinson/`
- Minimum: 18pt body, 22pt subtitles
- Always offer: text scale, colorblind mode, reduced motion, hold-vs-toggle
- Focus indicators must be visually distinct (not just color)
- Run `audit_accessibility` on every WBP before shipping

---

## CommonUI Actions (50, v0.14.0, conditional)

Require the CommonUI engine plugin. Stock in UE 5.7 at `Engine/Plugins/Runtime/CommonUI/`. Build.cs detects via 3-location scan; missing plugin → actions silently unregister.

Filter the listing: `monolith_discover({ namespace: "ui", category: "CommonUI" })`. Runtime-phase actions marked `[RUNTIME]` require a PIE session.

### A: Activatable Lifecycle (8)

| Action | Params | Purpose |
|---|---|---|
| `create_activatable_widget` | `save_path`, `root_widget`? | New WBP subclass of UCommonActivatableWidget |
| `create_activatable_stack` | `wbp_path`, `widget_name`, `parent_widget`? | Add UCommonActivatableWidgetStack into existing tree |
| `create_activatable_switcher` | `wbp_path`, `widget_name`, `parent_widget`? | Add UCommonActivatableWidgetSwitcher |
| `configure_activatable` | `wbp_path`, `bAutoActivate`?, `bIsModal`?, `bIsBackHandler`?, `activated_visibility`?, `deactivated_visibility`?, `input_mapping`?, `input_mapping_priority`? | Stamp CDO flags |
| `push_to_activatable_stack` `[RUNTIME]` | `container_name`, `widget_class` | Push widget class onto named container |
| `pop_activatable_stack` `[RUNTIME]` | `container_name`, `mode`? (top/all) | Pop top or clear |
| `get_activatable_stack_state` `[RUNTIME]` | `container_name` | Depth + top widget |
| `set_activatable_transition` | `wbp_path`, `widget_name`, `transition_type`?, `transition_duration`?, `transition_curve_type`? | Tune transition |

### B: Buttons + Styling (9)

| Action | Params | Purpose |
|---|---|---|
| `convert_button_to_common` | `wbp_path`, `widget_name` | Replace UButton with UCommonButtonBase (child NOT auto-transferred — see Limitations) |
| `configure_common_button` | `wbp_path`, `widget_name`, `is_toggleable`?, `requires_hold`?, `min/max_width/height`?, `click_method`?, `disabled_reason`? | Set button behavior |
| `create_common_button_style` | `package_path`, `asset_name`, `properties`? | Create UCommonButtonStyle asset |
| `create_common_text_style` | same | Create UCommonTextStyle asset |
| `create_common_border_style` | same | Create UCommonBorderStyle asset |
| `apply_style_to_widget` | `wbp_path`, `widget_name`, `style_asset` | Assign style class to button/text/border |
| `batch_retheme` | `folder_path`, `old_style`, `new_style` | Swap style class refs across WBPs |
| `configure_common_text` | `wbp_path`, `widget_name`, `wrap_text_width`?, `line_height_percentage`?, `mobile_font_size_multiplier`?, `scrolling_enabled`?, `text_case`? | UCommonTextBlock props |
| `configure_common_border` | `wbp_path`, `widget_name`, `reduce_padding_by_safezone`?, `minimum_padding`? | UCommonBorder props |

### C: Input / Actions / Glyphs (7)

| Action | Params | Purpose |
|---|---|---|
| `create_input_action_data_table` | `package_path`, `asset_name` | UDataTable of FCommonInputActionDataBase |
| `add_input_action_row` | `table_path`, `row_name`, `display_name`, `hold_display_name`?, `nav_bar_priority`?, `keyboard_key`?, `gamepad_key`?, `touch_key`? | Add action row (struct fields as UE text format) |
| `bind_common_action_widget` | `wbp_path`, `widget_name`, `table_path`, `row_name` | Point UCommonActionWidget at DataTable row |
| `create_bound_action_bar` | `wbp_path`, `widget_name`, `parent_widget`? | Add UCommonBoundActionBar to tree |
| `get_active_input_type` `[RUNTIME]` | — | Current ECommonInputType from LocalPlayer subsystem |
| `set_input_type_override` `[RUNTIME]` | `input_type` (MouseAndKeyboard/Gamepad/Touch) | Force input type for test |
| `list_platform_input_tables` | — | UCommonInputSettings.ControllerData entries |

### D: Navigation / Focus (5)

| Action | Params | Purpose |
|---|---|---|
| `set_widget_navigation` | `wbp_path`, `widget_name`, `direction` (Up/Down/Left/Right/Next/Previous), `rule` (Escape/Stop/Wrap/Explicit/Custom/CustomBoundary), `explicit_target`? | Set per-direction nav rule |
| `set_initial_focus_target` | `wbp_path`, `target_widget` | Stamp DesiredFocusTargetName on UCommonActivatableWidget CDO (WBP must expose property) |
| `force_focus` `[RUNTIME]` | `widget_name` | SetFocus on named live widget |
| `get_focus_path` `[RUNTIME]` | — | Slate focus chain leaf→root |
| `request_refresh_focus` `[RUNTIME]` | `widget_name` | Trigger RequestRefreshFocus on activatable |

### E: Lists / Tabs / Groups / Switchers / Carousel / HW Visibility (7)

| Action | Params | Purpose |
|---|---|---|
| `setup_common_list_view` | `wbp_path`, `widget_name`, `entry_class`, `entry_spacing`?, `pool_size`? | Configure UCommonListView / UCommonTileView |
| `create_tab_list_widget` | `save_path` | New WBP subclass of UCommonTabListWidgetBase |
| `register_tab` `[RUNTIME]` | `tab_list_name`, `tab_id`, `button_class`, `tab_index`? | RegisterTab on live instance |
| `create_button_group` `[RUNTIME]` | `button_names` (array), `selection_required`? | UCommonButtonGroupBase wrapping PIE widgets |
| `configure_animated_switcher` | `wbp_path`, `widget_name`, `transition_type`?, `transition_duration`?, `transition_curve_type`? | UCommonAnimatedSwitcher props |
| `create_widget_carousel` | `wbp_path`, `widget_name`, `parent_widget`? | Add UCommonWidgetCarousel to tree |
| `create_hardware_visibility_border` | `wbp_path`, `widget_name`, `parent_widget`?, `visibility_query`? | Add UCommonHardwareVisibilityBorder with optional FGameplayTagQuery |

### F: Content (4)

| Action | Params | Purpose |
|---|---|---|
| `configure_numeric_text` | `wbp_path`, `widget_name`, `numeric_type`?, `current_value`?, `formatting_specification`?, `ease_out_exponent`?, `post_interpolation_shrink_duration`? | UCommonNumericTextBlock |
| `configure_rotator` | `wbp_path`, `widget_name`, `labels`?, `selected_index`? | UCommonRotator labels |
| `create_lazy_image` | `wbp_path`, `widget_name`, `parent_widget`? | Add UCommonLazyImage |
| `create_load_guard` | `wbp_path`, `widget_name`, `parent_widget`? | Add UCommonLoadGuard |

### G: Dialog / Modal (2)

| Action | Params | Purpose |
|---|---|---|
| `show_common_message` `[RUNTIME]` | `container_name`, `dialog_class` | Push dialog WBP onto named container (fire-and-forward — result binding in dialog WBP) |
| `configure_modal_overlay` | `wbp_path`, `parent_widget`, `blur_widget_name`?, `blur_strength`? | Add UBackgroundBlur behind a parent panel |

### H: Audit + Lint (4)

| Action | Params | Purpose |
|---|---|---|
| `audit_commonui_widget` | `wbp_path` | Per-WBP lint: missing styles, unbound action widgets, missing focus target |
| `export_commonui_report` | `folder_path`? | Project-wide coverage report: activatable count, button styling ratio, action-widget binding ratio |
| `hot_reload_styles` `[RUNTIME, EXPERIMENTAL]` | — | Re-apply styles to all PIE buttons |
| `dump_action_router_state` `[RUNTIME, EXPERIMENTAL]` | — | Input subsystem + activatable container states |

### I: Accessibility Bridge (4)

| Action | Params | Purpose |
|---|---|---|
| `enforce_focus_ring` | `folder_path` | Audit: report unstyled UCommonButtonBase widgets |
| `wrap_with_reduce_motion_gate` | `folder_path` | Stamp bRespectReduceMotion on WBP CDOs (author must expose property + branch animation on subsystem) |
| `set_text_scale_binding` | `folder_path` | Stamp bHonorAccessibilityTextScale on WBP CDOs |
| `apply_high_contrast_variant` | `folder_path`, `normal_style`, `high_contrast_style` | Swap style class refs to HC variant |

### CommonUI Known Limitations (v0.14.0)

- `convert_button_to_common` does NOT auto-transfer UButton children — UCommonButtonBase uses internal widget tree. Rewire manually.
- `set_initial_focus_target` requires the WBP to expose a `DesiredFocusTargetName` FName UPROPERTY.
- `show_common_message` is fire-and-forward — async result-binding belongs in the dialog WBP.
- `dump_action_router_state` cannot read private `CurrentInputLocks` (engine PR candidate).
- 50 CommonUI actions require PIE session for functional testing — M0.5.1 test-coverage pass pending.
