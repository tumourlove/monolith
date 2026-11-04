---
name: unreal-gameplay-tags
description: Use when working with Unreal gameplay tags via Monolith MCP - finding which assets use a tag, listing the tags an asset uses, browsing the tag tree and its declaring sources, auditing unused and broken tags, and declaring, commenting, renaming, moving or removing ini tags. Works with GAS turned off. Triggers on gameplay tag, FGameplayTag, tag container, GameplayTagList, DefaultGameplayTags.ini, Config/Tags, tag redirect, unused tags, broken tags, rename tag, native tag, UE_DEFINE_GAMEPLAY_TAG.
---

# Unreal Gameplay Tag Workflows

**14 actions** via `gameplay_tags_query()`. Discover with `monolith_discover({ namespace: "gameplay_tags" })`.

References come from the live Asset Registry `SearchableName` graph - the same data the editor uses for "Cannot delete tag, still referenced by". No reindex is involved, but only saved assets are seen, and a reference names a package, not the object inside it.

## Action Reference

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `find_references` | `tag`, `include_children`?, `limit`? | Packages using a tag (asset name and class per row). A reference is recorded against the exact tag, so pass `include_children` for a subtree |
| `get_asset_tags` | `asset_path` | Tags one package references, nested objects included |
| `list` | `prefix`?, `limit`? | Tags with parent, child count, declaring sources, dev comment |
| `get_tag_info` | `tag` | One tag: sources, children, direct vs subtree reference counts |
| `find_unused` | `prefix`?, `scan_source`? | Tags no asset, project C++ literal or project config setting uses. `branch_root` marks where a dead branch starts; native tags are listed apart |
| `find_broken` | `path_prefix`? | Assets referencing tags that no longer exist; redirected ones reported apart |
| `list_sources` | - | Declaring sources: ini, restricted ini, DataTable, native |
| `add_source` | `name` | New `Config/Tags/<name>.ini` |
| `add` | `tags`, `comment`?, `source`?, `restricted`? | Declare tags; invalid names come back with the engine's reason and a fix |
| `update` | `tag`, `comment`?, `allow_non_restricted_children`? | Edit a declaration |
| `remove` | `tags`, `dry_run`? | Delete; refused while assets, C++ or config still use the tag |
| `rename` | `old_tag`, `new_tag`, `rename_children`?, `dry_run`? | Rename with a redirect; lists what still spells the old name |
| `move` | `tags`, `target_source` | Move between ini sources |

## Workflows

**Who uses this tag?** `find_references` with `include_children: true` for a parent. Then `blueprint_query` / `level_sequence_query` on the returned packages to find the node or property.

**Clean up dead tags.** `find_unused`, delete the `branch_root` rows with `remove` (run `dry_run: true` first). A tag that only appears in a string built at runtime has no literal to find - check before deleting.

**Rename safely.** `rename` with `dry_run: true` shows every asset, C++ literal and config setting holding the old name. Assets load through the redirect until resaved; C++ and config need editing by hand. Restart the editor afterwards.

**Add tags in the right file.** `list_sources` to pick an ini, `add` with `source`. Without `source`, tags go to `DefaultGameplayTags.ini` (or the developer tag file when Project Settings sets one).

## Gotchas

- Native tags (`UE_DEFINE_GAMEPLAY_TAG`, `AddNativeGameplayTag`) and DataTable tags cannot be changed through an ini; write actions refuse them and say where to edit.
- Spaces are legal in tag names; which characters are invalid is a per-project setting (`InvalidTagCharacters`). A leading or trailing period is always invalid.
- After a rename, looking up the old name returns the new tag (redirects are followed).
- `gas` has its own tag actions, active only with GAS enabled; for references prefer this namespace.
