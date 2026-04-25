# Monolith — MonolithUI Module

**Parent:** [SPEC_CORE.md](../SPEC_CORE.md)
**Engine:** Unreal Engine 5.7+
**Version:** 0.14.1 (Beta)

---

## MonolithUI

**Dependencies:** Core, CoreUObject, Engine, MonolithCore, UnrealEd, UMGEditor, UMG, Slate, SlateCore, Json, JsonUtilities, KismetCompiler, MovieScene, MovieSceneTracks
**Total actions:** 42 (UMG baseline)
**Settings toggle:** `bEnableUI` (default: True)

> **Coming soon:** A CommonUI action pack (~50 actions across activatable widgets, buttons + styling, input glyphs, focus/nav, lists/tabs, dialogs, audit/lint, accessibility) is planned for a future release. It will gate on `WITH_COMMONUI` via `MonolithUI.Build.cs` detection of the CommonUI engine plugin, mirroring the conditional-dependency pattern used by `MonolithMesh` (GeometryScripting) and `MonolithBABridge` (Blueprint Assist).

### Classes

| Class | Responsibility |
|-------|---------------|
| `FMonolithUIModule` | Registers 42 UMG baseline actions. Logs the live `ui` namespace action count at startup |
| `FMonolithUIActions` | Widget blueprint CRUD: create, inspect, add/remove widgets, property writes, compile |
| `FMonolithUISlotActions` | Layout slot operations: slot properties, anchor presets, widget movement |
| `FMonolithUITemplateActions` | High-level HUD/menu/panel scaffold templates (8 templates) |
| `FMonolithUIStylingActions` | Visual styling: brush, font, color scheme, text, image, batch style |
| `FMonolithUIAnimationActions` | UMG widget animation CRUD: list, inspect, create, add/remove keyframes |
| `FMonolithUIBindingActions` | Event/property binding inspection, list view setup, widget binding queries |
| `FMonolithUISettingsActions` | Settings/save/audio/input remapping subsystem scaffolding (5 scaffolds) |
| `FMonolithUIAccessibilityActions` | Accessibility subsystem scaffold, audit, colorblind mode, text scale |

### Actions — UMG Baseline (42 — namespace: "ui")

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
| `set_color_scheme` | `asset_path`, `color_map` | Apply a color scheme (name→LinearColor map) across the widget |
| `batch_style` | `asset_path`, `style_operations` | Apply multiple styling operations in a single transaction |
| `set_text` | `asset_path`, `widget_name`, `text` | Set display text on a text widget |
| `set_image` | `asset_path`, `widget_name`, `texture_path` | Set the texture on an image widget |

**Animation (5)**
| Action | Params | Description |
|--------|--------|-------------|
| `list_animations` | `asset_path` | List all UMG animations on a Widget Blueprint |
| `get_animation_details` | `asset_path`, `animation_name` | Get tracks and keyframes for a named animation |
| `create_animation` | `asset_path`, `animation_name` | Create a new UMG widget animation |
| `add_animation_keyframe` | `asset_path`, `animation_name`, `widget_name`, `property`, `time`, `value` | Add a keyframe to a widget animation track |
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
