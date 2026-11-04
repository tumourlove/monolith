# Monolith - MonolithGameplayTags Module

**Parent:** [SPEC_CORE.md](../SPEC_CORE.md)
**Engine:** Unreal Engine 5.7+
**Version:** Unreleased (Beta)

---

## MonolithGameplayTags

**Dependencies:** Core, CoreUObject, Engine, MonolithCore, AssetRegistry, GameplayTags, GameplayTagsEditor, SourceControl, Projects, Json
**Plugin dependency:** GameplayTagsEditor (engine plugin, enabled by default)
**Namespace:** `gameplay_tags` | **Tool:** `gameplay_tags_query(action, params)` | **Actions:** 14 (13 production + `ping` smoke)
**Settings toggle:** `bEnableGameplayTags` (default True) under `[/Script/MonolithCore.MonolithSettings]`. Independent of `bEnableGAS`: gameplay tags are an engine feature every project has, so turning GAS off does not take tag lookups and tag edits with it.

MonolithGameplayTags answers "what uses this tag" the way the editor's own tag tooling does, audits tags for dead and broken ones, and declares, edits, renames, moves and removes ini tags through the same module the Gameplay Tags page of Project Settings uses.

### Where tag references come from

Serializing an `FGameplayTag` calls `FArchive::MarkSearchableName(FGameplayTag::StaticStruct(), TagName)`, so the Asset Registry holds a `SearchableName` dependency edge from every package to every tag it serializes - an asset property, a graph node pin, a component on a level, a tag inside a container or query. `GameplayTagsEditorModule` resolves referencers this way (it is the check behind "Cannot delete tag, still referenced by"), and so does this module.

Asset Registry *metadata* (`FAssetData::TagsAndValues`) is not a usable source: an `FGameplayTag` property only reaches it when marked `AssetRegistrySearchable`, which almost none are. On Lyra 5.7, over all 448 registered tags, the `SearchableName` graph gives 743 references on 197 tags, while a metadata scan gives 33 references on 29 tags.

Consequences:

- **No reindex needed.** Answers come from the live Asset Registry, not from `ProjectIndex.db`.
- **On-disk only.** An asset edited but not yet saved is not seen.
- **Package granularity.** A reference identifies the package, not the object inside it that holds the tag.
- **Exact tag.** A reference is recorded against the tag written in the asset; `include_children` sweeps a subtree.

### Action Categories

Read actions live in `MonolithGameplayTagsActions.cpp`, write actions in `MonolithGameplayTagsWriteActions.cpp`.

| Category | Actions | Description |
|----------|---------|-------------|
| Smoke | 1 | `ping` - tag count, Asset Registry readiness |
| Lookup | 4 | `find_references` (packages using a tag, optional subtree, asset name and class per row), `get_asset_tags` (tags one asset uses), `list` (tags with parent, child count, sources, dev comment), `get_tag_info` (one tag in detail with direct and subtree reference counts) |
| Audit | 2 | `find_unused` (dead tags and dead branches), `find_broken` (references to tags the manager no longer knows, redirected ones reported apart) |
| Sources | 2 | `list_sources` (default ini, `Config/Tags/*.ini`, restricted lists, tag DataTables, native), `add_source` (new `Config/Tags/<name>.ini`) |
| Write | 5 | `add`, `update`, `remove`, `rename`, `move` |

**Total:** **14** registered.

### find_unused

A tag is reported when nothing uses it and an edit of its declaring source (an ini or a tag DataTable, listed in `sources`) can remove it.

- **Assets.** A tag is a candidate when nothing in its whole subtree is referenced. A parent with a live child is never a candidate.
- **C++.** Evidence is a string literal holding the exact tag name (`"A.B"`, compared without case) in the project's `Source/` and `Plugins/`. Comments and bare identifiers do not count. Monolith and every plugin that depends on it are left out: their sources quote tag-shaped strings (`"Input"`, `"GameplayCue"`) as pin names, examples and presets.
- **Config.** A tag-valued setting is saved as its export text, `(TagName="A.B")`, in the project, platform or plugin ini that holds it (platform traits, surface-type maps, any developer setting). Only that form counts: declarations use `Tag="A.B"` and redirects use `OldTagName` / `NewTagName`.
- **Native tags** are declared in C++ and never land in `unused`. They go to `native_unreferenced_tags`; for an explicitly declared one, `declared_in` names the project file that declares it and stays empty when the engine or a third-party plugin does. A non-native tag with a native tag below it is counted in `native_branches_skipped`.
- **Branches.** Every row's whole subtree is dead, so `branch_root` marks where a dead branch starts.
- `code_referenced_tags` names the file that rescued each tag.

C++ and config are read from disk on every call (about 0.4 s for 1,100 C++ files, 0.7 s on Lyra). `MonolithSource`'s index is not used: it also holds the engine's source, so a phrase query for a common word walks thousands of engine lines before a project filter applies (7 s on a 1,100-file project), and it is rebuilt by hand, so fresh code would be missed.

A tag assembled at runtime from pieces leaves no literal behind; confirm a candidate before deleting it.

### Writing tags

Every write goes through `IGameplayTagsEditorModule`, so checkout under source control, the ini save and the tag tree refresh happen as they do from the UI. The API is identical in UE 5.7 and 5.8; no version gate.

| Action | Notes |
|--------|-------|
| `add` | Batch; one tag-tree rebuild per call. Each name is validated with `IsValidGameplayTagString`; an invalid one returns the engine's reason and suggested fix. An unknown `source` is an error pointing at `add_source`. A restricted tag needs an explicit restricted source. Restricted-hierarchy rules are checked up front. Adding a name that is a redirect source reports `replaces_redirect_to`. |
| `update` | Dev comment, and `allow_non_restricted_children` for restricted tags. |
| `remove` | Refused while any asset references the tag (the engine's own rule, including implicit parents that would disappear with it), or while a name that would stop existing is still in project C++ or used in project config. `dry_run`. A tag with children of its own stays as an implicit parent (`remains_implicit`). |
| `rename` | Subtree by default (`rename_children`). Writes a `GameplayTagRedirect`; assets are not rewritten. Reports, per renamed tag, the assets, C++ literals and config uses that still spell the old name. `dry_run`. `restart_recommended`. Restricted tags are refused. |
| `move` | Between ini sources, comments kept. Restricted, native and DataTable tags cannot be moved. |

Engine behaviours the write actions work around, all confirmed live:

- **No refusal reason.** `AddNewGameplayTagToINI`, `DeleteTagFromINI` and `MoveTagsBetweenINI` report a refusal only as an editor toast and a log line. The actions validate first and return the reason.
- **Silent fallbacks in `AddNewGameplayTagToINI`.** An unknown source name creates a new source; a restricted tag sent to a source without a restricted list becomes an ordinary tag. `add` refuses both instead.
- **`UpdateTagInINI` cannot edit a comment.** It only toggles `bAllowNonRestrictedChildren` and returns false for an ordinary tag. `update` edits the declaring row and saves it the way the module saves its own edits: checkout, `TryUpdateDefaultConfigFile`, `GConfig->LoadFile`, tree refresh.
- **Rename drops the parent's declaration.** With `bRenameChildren`, `RenameTagInINI` renames children first, so by the time it reaches the parent the new parent name already exists implicitly and it skips declaring it: the parent loses its declaration and dev comment. `rename` re-declares every tag that was explicit before, with its old comment and source (`redeclared`).
- **Lookups follow redirects.** After a rename, `GetTagEditorData` and `FindTagNode` asked about the old name answer for the new tag. Whether a declaration is really gone is read from the ini tag lists themselves.
- **Each delete rebuilds the tree.** `DeleteTagFromINI` invalidates every `FGameplayTagNode`, so `remove` looks each node up fresh.

### Tests

`Monolith.GameplayTags.Write.*` (add, update comment, remove, rename, move) declare tags in throwaway ini sources under `Saved/AutomationTests/MonolithGameplayTags`, registered through `UGameplayTagsManager::AddTagIniSearchPath`, below a root tag unique to the run, and remove them again; the project's tag configuration is never touched. `Monolith.GameplayTags.Evidence.*` cover the C++ literal and config-use parsers.

### Conventions notes

- Native and DataTable declarations are outside what an ini edit can change; write actions refuse them and name the place to edit instead.
- The `gas` namespace keeps its own tag actions (`add_gameplay_tags`, `search_tag_usage`, ...), gated behind `bEnableGAS` and reading Asset Registry metadata. This module does not change them.
