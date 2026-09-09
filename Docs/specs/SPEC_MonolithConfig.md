# Monolith — MonolithConfig Module

**Parent:** [SPEC_CORE.md](../SPEC_CORE.md)
**Engine:** Unreal Engine 5.7+
**Version:** 0.23.0 (Beta)

---

## MonolithConfig

**Dependencies:** Core, CoreUObject, Engine, MonolithCore, UnrealEd, Json, JsonUtilities

### Classes

| Class | Responsibility |
|-------|---------------|
| `FMonolithConfigModule` | Registers 6 config actions + 4 localization actions |
| `FMonolithConfigActions` | Static handlers. Helpers: ResolveConfigFilePath, GetConfigHierarchy (5 layers: Base -> Default -> Project -> User -> Saved). Uses GConfig API for reliable resolution |
| `FMonolithLocalizationActions` | Static handlers for the `localization` namespace. Culture resolution via `FInternationalization`; StringTable discovery via `IAssetRegistry`, entry reads through the const `UStringTable::GetStringTable()` (`FStringTableConstRef`) |

### Actions (6 — namespace: "config")

| Action | Description |
|--------|-------------|
| `resolve_setting` | Get effective value via `GConfig->GetString`. Params: `file` (category), `section`, `key` |
| `explain_setting` | Show where value comes from across Base->Default->User layers. Auto-searches Engine/Game/Input/Editor if only `setting` given |
| `diff_from_default` | Compare config layers using GConfig API. Supports 5 INI layers (Base, Default, Project, User, Saved). Reports modified + added. Optional `section` filter |
| `search_config` | Full-text search across all config files. Max 100 results. Optional `file` filter |
| `get_section` | Read entire config section from a file |
| `get_config_files` | List all .ini files with hierarchy level and sizes. Optional `category` filter |

---

## Localization Namespace (4 — namespace: "localization")

**New in v0.23.0.** Read-only culture and StringTable inspection, registered from `MonolithConfig`. **Registered AHEAD of the `bEnableConfig` gate** — these actions depend on nothing that gate controls, so they stay available when config authoring is switched off (`FMonolithConfigModule::StartupModule` calls `FMonolithLocalizationActions::RegisterActions` before the early return). The dispatcher is annotated read-only + idempotent.

Every action requires a **canonical mounted package path** (or the matching top-level object path) and **never transacts, saves, mutates or dirties a package**. StringTable *authoring* is not here — it remains on the `blueprint` namespace.

| Action | Description |
|--------|-------------|
| `list_cultures` | Bounded culture discovery. Without `culture_names` it pages every known culture; with them it resolves those roots and reports the ones Unreal could not resolve under `unresolved_names`. Params: `culture_names` (array, max 256), `include_derived` (bool, default `true`, only meaningful with `culture_names`), `offset` (default 0), `limit` (1-500, default 100). Returns `current_culture` / `current_language` / `current_locale`, `unresolved_names`, and per-culture `name`, `native_name`, `english_name`, `display_name`, `two_letter_iso`, `three_letter_iso`, plus `total` / `offset` / `limit` / `count` / `has_more` |
| `list_string_tables` | AssetRegistry `UStringTable` discovery under a mounted package root. Params: `path` (canonical root, default `/Game`), `offset`, `limit` (1-1000, default 200), `include_details` (default `false`). `include_details` loads **only the returned page** and adds `namespace`, `table_id`, `is_internal` and `entry_count` per row. Discovery itself stays registry-only |
| `get_string_table` | One bounded page of entries in stable key order, resumed by an **exclusive** `after_key` cursor. Params: `asset_path` (required), `after_key` (max 4096 chars), `entry_limit` (1-1000, default 200), `include_metadata` (default `false`), `metadata_limit` (0-4096, default 512 — a budget **shared across the whole page**, not per entry), `text_limit` (1-65536, default 4096, per source string or metadata value). Entry rows carry `key`, `source_string`, `source_string_length`, `source_string_truncated` and optional `metadata`. Readback separates three completeness facts: `has_more_entries` (this page), `all_entries_covered` (the table), and `metadata_complete` (the metadata budget) |
| `validate_string_table` | Deterministic bounded key/source validation in stable key order, with paginated issues. Params: `asset_path` (required), `scan_limit` (1-10000, default 4096), `issue_offset`, `issue_limit` (1-1000, default 200). **Errors** are empty key, empty table, and `scan_limit` exceeded; **warnings** are edge whitespace and empty source string. `valid=true` ONLY when the scan covered every entry AND found zero errors — a truncated scan is itself an error, so a partial pass can never read as a clean one. Returns `entry_count`, `entries_scanned`, `complete`, `valid`, `errors`, `warnings`, `issues[{code, severity, message, key?}]`, `issue_total`, `has_more_issues` |

**Bounding contract.** Every response is finite, and every cutoff is reported through an explicit flag rather than being left to look like a complete answer. That is the reason `get_string_table` splits page-completeness from table-completeness from metadata-completeness instead of emitting a single `complete` bool: a caller paging a large table needs to know *which* budget stopped it.

---
