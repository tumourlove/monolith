---
name: unreal-ui
description: Use when working with Unreal Engine UI via Monolith MCP — creating widget blueprints, building HUDs, menus, settings panels, styling, animations, data binding, save systems, and accessibility. Triggers on UI, HUD, widget, menu, settings, save game, accessibility, font, anchor, toast, dialog, loading screen.
---

# Unreal UI Workflows

You have access to **Monolith** with 42 UI actions via `ui_query()`.

## Discovery

Always discover available actions first:
```
monolith_discover({ namespace: "ui" })
```

## Key Parameter Names

- `asset_path` — Widget Blueprint asset path (e.g. `/Game/UI/WBP_MyWidget`)
- `save_path` — path for creating new Widget Blueprint assets
- `widget_name` — name of a widget in the tree
- `widget_class` — widget type: `TextBlock`, `Image`, `Button`, `VerticalBox`, etc.
- `parent_name` — parent panel widget name (omit for root)
- `anchor_preset` — named preset: `center`, `top_left`, `bottom_right`, `stretch_fill`, etc.
- `compile` — boolean, whether to compile after modification (default varies)

## Action Reference

### Widget Blueprint CRUD (7)

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `create_widget_blueprint` | `save_path`, `parent_class`?, `root_widget`? | Create new WBP with optional parent class and root panel |
| `get_widget_tree` | `asset_path` | Get full widget hierarchy as JSON tree with slot properties |
| `add_widget` | `asset_path`, `widget_class`, `widget_name`?, `parent_name`?, `anchor_preset`?, `position`?, `size`? | Add widget to a panel |
| `remove_widget` | `asset_path`, `widget_name` | Remove widget from tree |
| `set_widget_property` | `asset_path`, `widget_name`, `property_name`, `value` | Set any UPROPERTY via reflection |
| `compile_widget` | `asset_path` | Compile widget blueprint |
| `list_widget_types` | `filter`? | List available widget classes by category |

### Slot & Layout (3)

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `set_slot_property` | `asset_path`, `widget_name`, `anchors`?, `offsets`?, `position`?, `size`?, `z_order`?, `h_align`?, `v_align`?, `padding`? | Set any slot property |
| `set_anchor_preset` | `asset_path`, `widget_name`, `preset` | Apply named anchor preset |
| `move_widget` | `asset_path`, `widget_name`, `new_parent_name` | Reparent widget to different panel |

### Templates (8)

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `create_hud_element` | `asset_path`, `element_type`, `widget_name_prefix`? | Create HUD element: crosshair, health_bar, ammo_counter, stamina_bar, interaction_prompt, damage_indicator, compass, subtitles, flashlight_battery |
| `create_menu` | `save_path`, `menu_type`, `buttons`? | Create menu: main_menu, pause_menu, death_screen, credits |
| `create_settings_panel` | `save_path`, `tabs`? | Create tabbed settings panel (graphics, audio, controls, gameplay, accessibility) |
| `create_dialog` | `save_path`, `title`?, `body`?, `confirm_text`?, `cancel_text`? | Create confirmation dialog |
| `create_notification_toast` | `save_path`, `position`? | Create notification toast widget |
| `create_loading_screen` | `save_path`, `show_progress`?, `show_tips`?, `show_spinner`? | Create loading screen |
| `create_inventory_grid` | `save_path`, `columns`?, `rows`?, `slot_size`? | Create inventory grid layout |
| `create_save_slot_list` | `save_path`, `max_slots`? | Create save slot selector |

### Styling (6)

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `set_brush` | `asset_path`, `widget_name`, `property_name`, `draw_type`?, `tint_color`?, `corner_radius`?, `texture_path`?, `material_path`? | Configure brush on any widget |
| `set_font` | `asset_path`, `widget_name`, `font_size`?, `typeface`?, `outline_size`?, `outline_color`? | Set font on text widgets |
| `set_color_scheme` | `colors` | Set EStyleColor User1-16 palette slots |
| `batch_style` | `asset_path`, `widget_class`, `property_name`, `value` | Apply property to all widgets of a class |
| `set_text` | `asset_path`, `widget_name`, `text`, `text_color`?, `font_size`?, `justification`? | Convenience text setter |
| `set_image` | `asset_path`, `widget_name`, `texture_path`?, `material_path`?, `tint_color`?, `size`? | Convenience image setter |

### Animation (5)

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `list_animations` | `asset_path` | List all UWidgetAnimations on a WBP |
| `get_animation_details` | `asset_path`, `animation_name` | Get tracks, sections, keyframe counts |
| `create_animation` | `asset_path`, `animation_name`, `duration`, `tracks` | Create animation with keyframed property tracks |
| `add_animation_keyframe` | `asset_path`, `animation_name`, `widget_name`, `property`, `time`, `value` | Add keyframe to existing track |
| `remove_animation` | `asset_path`, `animation_name` | Remove animation from WBP |

### Data Binding (4)

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `list_widget_events` | `asset_path`, `widget_name`? | List bindable events on widgets |
| `list_widget_properties` | `asset_path`, `widget_name` | List settable properties with types and values |
| `setup_list_view` | `asset_path`, `list_widget_name`, `entry_widget_class`?, `entry_height`?, `entry_width`? | Configure ListView/TileView |
| `get_widget_bindings` | `asset_path` | Get all property bindings on a WBP |

### Settings & Save Scaffolding (5)

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `scaffold_game_user_settings` | `class_name`, `module_name`, `features`? | Generate UGameUserSettings subclass C++ |
| `scaffold_save_game` | `class_name`, `module_name`, `properties`? | Generate ULocalPlayerSaveGame subclass C++ |
| `scaffold_save_subsystem` | `class_name`, `module_name`, `save_game_class` | Generate save management subsystem C++ |
| `scaffold_audio_settings` | `categories`? | Get audio settings wiring info (USoundMix/USoundClass) |
| `scaffold_input_remapping` | `actions`? | Get keybinding remapping setup info |

### Accessibility (4)

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `scaffold_accessibility_subsystem` | `class_name`, `module_name` | Generate accessibility settings subsystem C++ |
| `audit_accessibility` | `asset_path` | Audit WBP for font size, focus, navigation, tooltip issues |
| `set_colorblind_mode` | `mode`, `severity`?, `correct`? | Set colorblind correction (runtime) |
| `set_text_scale` | `scale` | Set UI text scale (runtime) |

## Anchor Presets

| Preset | Anchors (minX, minY, maxX, maxY) |
|--------|----------------------------------|
| `top_left` | 0, 0, 0, 0 |
| `top_center` | 0.5, 0, 0.5, 0 |
| `top_right` | 1, 0, 1, 0 |
| `center_left` | 0, 0.5, 0, 0.5 |
| `center` | 0.5, 0.5, 0.5, 0.5 |
| `center_right` | 1, 0.5, 1, 0.5 |
| `bottom_left` | 0, 1, 0, 1 |
| `bottom_center` | 0.5, 1, 0.5, 1 |
| `bottom_right` | 1, 1, 1, 1 |
| `stretch_fill` | 0, 0, 1, 1 |
| `stretch_horizontal` | 0, 0.5, 1, 0.5 |
| `stretch_vertical` | 0.5, 0, 0.5, 1 |

## Typical Workflows

### Build a HUD from scratch
```
1. ui_query("create_widget_blueprint", {"save_path": "/Game/UI/WBP_GameHUD"})
2. ui_query("create_hud_element", {"asset_path": "/Game/UI/WBP_GameHUD", "element_type": "health_bar"})
3. ui_query("create_hud_element", {"asset_path": "/Game/UI/WBP_GameHUD", "element_type": "ammo_counter"})
4. ui_query("create_hud_element", {"asset_path": "/Game/UI/WBP_GameHUD", "element_type": "crosshair"})
5. ui_query("set_font", {"asset_path": "/Game/UI/WBP_GameHUD", "widget_name": "ammo_counter_Current", "font_size": 28, "typeface": "Bold"})
```

### Create a pause menu
```
1. ui_query("create_menu", {"save_path": "/Game/UI/WBP_PauseMenu", "menu_type": "pause_menu", "buttons": ["Resume", "Settings", "Main Menu", "Quit"]})
2. ui_query("set_brush", {"asset_path": "/Game/UI/WBP_PauseMenu", "widget_name": "Background", "draw_type": "RoundedBox", "tint_color": "#0A0A14CC", "corner_radius": {"top_left": 8, "top_right": 8, "bottom_right": 8, "bottom_left": 8}})
```

### Scaffold a save system
```
1. ui_query("scaffold_save_game", {"class_name": "ULeviathanSaveGame", "module_name": "Leviathan", "properties": [{"name": "PlayerHealth", "type": "float", "default_value": "100.0f"}, {"name": "PlayerLocation", "type": "FVector", "default_value": "FVector::ZeroVector"}]})
2. ui_query("scaffold_save_subsystem", {"class_name": "ULeviathanSaveSubsystem", "module_name": "Leviathan", "save_game_class": "ULeviathanSaveGame"})
```

## Horror UI Guidelines

- **No health bar** — use vignette + desaturation + heartbeat audio via post-process
- **Minimal HUD** — only show info when relevant (auto-hide stamina when full)
- **Subtitles ON by default** — caption ALL sounds, not just dialogue
- **Diegetic where possible** — flashlight battery on the prop, not screen overlay
- **No minimap** — navigation uncertainty = horror
- **Short interaction trace** — player must get close to see prompts

## Accessibility (Critical — players with accessibility needs)

- **Atkinson Hyperlegible** font at `Content/UI/Fonts/Atkinson/` — designed for low vision
- Minimum font size: 18pt body, 22pt subtitles
- Always offer: text scale, colorblind mode, reduced motion, hold-vs-toggle
- Focus indicators must be visually distinct (not just color)
- Run `audit_accessibility` on every WBP before shipping
