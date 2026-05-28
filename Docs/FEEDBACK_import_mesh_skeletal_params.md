# Feedback: `mesh::import_mesh` — explicit skeletal / animation import parameters

**Audience:** Monolith maintainers (upstream)  
**Origin:** Downstream fork / integration testing (Unreal 5.7, MCP + `mesh_query` → `import_mesh`)  
**File touched:** `Source/MonolithMesh/Private/MonolithMeshTechArtActions.cpp` (`FMonolithMeshTechArtActions::RegisterActions`, `ImportMesh`)

---

## 中文说明（给原作者 / 社区的中文摘要）

**读者：** Monolith 维护者与上游仓库贡献者。  
**来源：** 下游工程在 UE 5.7 上通过 MCP（`mesh_query` → `import_mesh`）做自动化导入时的实践反馈。  
**改动文件：** `Source/MonolithMesh/Private/MonolithMeshTechArtActions.cpp`（`RegisterActions` 与 `ImportMesh`）。

### 一、动机

通过 Monolith MCP 驱动 FBX 导入时，我们希望对**带骨架的角色 FBX**（骨骼网格 + Skeleton）有**可预期、可在参数里声明的一等支持**，而不是完全依赖引擎启发式行为。

**改动前的逻辑：**

- 文案上提到可配置「静态 vs 骨骼」，但 `ImportMesh` 里对 `UFbxImportUI` **始终**写成：
  - `bImportAsSkeletal = false`
  - `MeshTypeToImport = FBXIT_StaticMesh`
  - `bImportAnimations = false`
- 实际上部分带骨架的 FBX 仍可能被导入成 `SkeletalMesh`（引擎/管线行为），对 MCP 调用方**不透明**，且**未在参数 schema 中体现**。

**具体问题：**

1. 智能体与使用者无法通过 JSON **明确表达**「这是角色骨架 FBX」。
2. Monolith 对未知参数会校验失败：客户端若自行加字段，在未更新服务端 schema 时会出现 `Unknown param ...` 警告，且**无法保证**与 `UFbxImportUI` 绑定。
3. **文档与实现不一致：** 仓库内 `Skills/unreal-mesh/unreal-mesh.md` 仍写 `file_path` / `save_path`，而实现要求的是 `files`（数组）与 `destination`（字符串）。（建议上游单独整理文档。）

### 二、API 变更（向后兼容）

在 `mesh::import_mesh` 上新增两个**可选**参数：

| 参数 | 类型 | 默认值 | 含义 |
|------|------|--------|------|
| `import_as_skeletal` | boolean | `false` | 为 `true` 时走骨骼 FBX 分支：`bImportAsSkeletal = true`，`MeshTypeToImport = FBXIT_SkeletalMesh`。 |
| `import_animations` | boolean | `false` | 为 `true` 时在 `UFbxImportUI` 上启用 `bImportAnimations`；**同时会强制**按骨骼分支处理（本实现中动画导入不与纯静态分支混用）。 |

**必填参数不变：** `files`（绝对路径数组）、`destination`（内容路径，例如 `/Game/Characters/Skeleton/npc_02`）。

**默认行为与旧版一致：** 不传上述两个布尔量时，仍走原先面向静态网格的 FBX 配置。

### 三、实现要点

**注册（`RegisterActions`）：**

- 更新 action 描述字符串，说明角色骨架与可选动画导入。
- 在 schema 中增加 `import_as_skeletal`、`import_animations` 两个 `.Optional(...)`。

**`ImportMesh`（仅当检测到 FBX、即 `bHasFbx` 时）：**

- 用 `TryGetBoolField` 读取 `import_as_skeletal`、`import_animations`。
- 若 `import_animations == true`，则强制 `bImportAsSkeletal = true`。
- 分支：
  - **骨骼路径：** `bImportAsSkeletal = true`，`MeshTypeToImport = FBXIT_SkeletalMesh`，`bImportAnimations` 取自参数。
  - **静态路径（兼容旧行为）：** `bImportAsSkeletal = false`，`MeshTypeToImport = FBXIT_StaticMesh`，`bImportAnimations = false`。

`material_import`（`create_new` / `find_existing` / `skip`）逻辑未改。

### 四、MCP 调用示例（`mesh_query`）

在 `tools/call` 中调用 `mesh_query`：`arguments.action` 为 `import_mesh`，`arguments.params` 中提供 `files`、`destination`，并按需设置 `import_as_skeletal`、`import_animations`。若 FBX 内含需一并导入的动画片段，将 `import_animations` 设为 `true`。

```json
{
  "name": "mesh_query",
  "arguments": {
    "action": "import_mesh",
    "params": {
      "files": ["C:/absolute/path/to/character.fbx"],
      "destination": "/Game/Characters/Skeleton/MyCharacter",
      "replace_existing": true,
      "combine_meshes": false,
      "import_as_skeletal": true,
      "import_animations": false
    }
  }
}
```

### 五、建议的后续工作（本补丁未实现）

1. **返回体增强：** 当导入结果为 `USkeletalMesh` 时，可在 JSON 中附带骨骼数、Skeleton 路径等（当前仅对 `UStaticMesh` 写顶点/三角面/材质槽统计）。
2. **glTF：** 若 glTF 走另一套工厂，建议与 FBX 使用同一套布尔语义，便于 MCP 统一描述。
3. **文档：** 将 `Skills/unreal-mesh/unreal-mesh.md` 中 `import_mesh` 一行改为与实现一致的参数名（`files`、`destination` 及新开关）。
4. **测试：** 增加最小编辑器侧用例：带 `import_as_skeletal=true` 导入极简绑骨 FBX，断言 `USkeletalMesh` 与包路径。

### 六、许可与贡献方式

本改动在下游工程中为生产向 MCP 工作流而加。若上游愿意合并，可直接采用；若更希望用单个 `fbx_options` 对象承载更多导入选项，也可用同等语义替换这两个扁平布尔字段。需要时我们可以向 `tumourlove/monolith` 单独开 PR 提交该补丁。

### 七、附：`Monolith.uplugin` 编码（与 `import_mesh` 无关，但影响本地 UBT）

磁盘上的 `Monolith.uplugin` 曾为 **UTF-16 LE**（`FF FE` BOM），导致 **UnrealBuildTool**（UE 5.7，`System.Text.Json`）解析插件清单失败。转为 **UTF-8** 后命令行编译恢复正常。建议上游约定 `.uplugin` 使用 UTF-8，避免各区域工具链差异。

---

## 1. Motivation

When driving FBX import through Monolith MCP (`mesh_query` / `import_mesh`), we needed **predictable, first-class support for rigged character FBX** (SkeletalMesh + skeleton), not only reliance on engine heuristics.

**Prior behavior (before this change):**

- The action description mentioned configuring “static vs skeletal”, but `ImportMesh` always configured `UFbxImportUI` as:

  - `bImportAsSkeletal = false`
  - `MeshTypeToImport = FBXIT_StaticMesh`
  - `bImportAnimations = false`

- In practice, some rigged FBX files still imported as `SkeletalMesh` (engine / pipeline behavior), which was **opaque to MCP callers** and did not surface in the param schema.

**Pain points:**

1. Agents and humans cannot **declare intent** (“this FBX is a character rig”) via JSON params.
2. Monolith’s **parameter validation** rejects unknown keys — so adding skeletal flags in the client without updating the server schema produces `Unknown param ...` warnings and no guaranteed linkage to `UFbxImportUI`.
3. **Documentation drift:** in-repo skill text (`Skills/unreal-mesh/unreal-mesh.md`) still mentions `file_path` / `save_path`, while the implementation requires `files` (array) + `destination` (string). (Separate cleanup suggestion.)

---

## 2. Proposed API change (backward compatible)

Two **optional** parameters on `mesh::import_mesh`:

| Parameter | Type | Default | Semantics |
|-----------|------|---------|-------------|
| `import_as_skeletal` | boolean | `false` | When `true`, set skeletal FBX import path: `bImportAsSkeletal = true`, `MeshTypeToImport = FBXIT_SkeletalMesh`. |
| `import_animations` | boolean | `false` | When `true`, set `bImportAnimations = true` on `UFbxImportUI`. **Also forces** `import_as_skeletal` logic on (animations require skeletal import in this path). |

**Unchanged required params:** `files` (array of absolute paths), `destination` (content path, e.g. `/Game/Characters/Skeleton/npc_02`).

**Defaults preserve old behavior:** omitting both flags keeps the previous static-mesh-oriented FBX branch.

---

## 3. Implementation summary

**Registration (`RegisterActions`):**

- Extended the action description string to mention rigged characters and optional animation import.
- Appended two `.Optional(...)` schema entries for `import_as_skeletal` and `import_animations`.

**`ImportMesh` (FBX branch only, when `bHasFbx`):**

- Read `import_as_skeletal` and `import_animations` via `TryGetBoolField`.
- If `import_animations` is true, force `bImportAsSkeletal = true` (so animation import is not paired with the static-mesh-only branch).
- Branch:

  - **Skeletal path:** `bImportAsSkeletal = true`, `MeshTypeToImport = FBXIT_SkeletalMesh`, `bImportAnimations` from param.
  - **Static path (legacy):** `bImportAsSkeletal = false`, `MeshTypeToImport = FBXIT_StaticMesh`, `bImportAnimations = false`.

Material import (`material_import`: `create_new` / `find_existing` / `skip`) is unchanged.

---

## 4. Example MCP payload (`mesh_query`)

```json
{
  "name": "mesh_query",
  "arguments": {
    "action": "import_mesh",
    "params": {
      "files": ["C:/absolute/path/to/character.fbx"],
      "destination": "/Game/Characters/Skeleton/MyCharacter",
      "replace_existing": true,
      "combine_meshes": false,
      "import_as_skeletal": true,
      "import_animations": false
    }
  }
}
```

Set `import_animations` to `true` when FBX contains clips that should be imported alongside the skeletal mesh.

---

## 5. Suggested follow-ups (optional, not implemented in our patch)

1. **Response enrichment:** when `UObject` is `USkeletalMesh`, optionally emit bone count / skeleton path in the JSON result (today only `UStaticMesh` gets vertex/triangle/material slot stats).
2. **glTF:** if glTF import uses a different factory path, mirror the same boolean semantics there for consistency.
3. **Docs:** align `Skills/unreal-mesh/unreal-mesh.md` “Tech Art Pipeline” row for `import_mesh` with the real parameter names (`files`, `destination`, new flags).
4. **Tests:** add an editor automation or scripted test that imports a minimal rigged FBX with `import_as_skeletal=true` and asserts `USkeletalMesh` + expected package path.

---

## 6. License / contribution

This change was made in a downstream project for production MCP workflows. We are happy for upstream to adopt, adapt, or replace with a more general FBX import options struct if you prefer a single `fbx_options` object instead of flat booleans.

If useful, we can open a proper GitHub PR against `tumourlove/monolith` with this patch isolated on a branch.

---

## 7. Related note (unrelated to `import_mesh`, but blocked local UBT)

`Monolith.uplugin` on disk was **UTF-16 LE** (`FF FE` BOM), which caused **UnrealBuildTool** (UE 5.7, `System.Text.Json`) to fail parsing the plugin manifest. Converting the file to **UTF-8** fixed local command-line builds. Worth verifying encoding policy for `.uplugin` in the upstream repo.
