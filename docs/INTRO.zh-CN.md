# UE-AssetDoc 的思路

[English](INTRO.md)

UE-AssetDoc 让 Unreal Editor 成为供 AI 客户端使用的 MCP 服务器。许多编辑器 MCP 服务器为每个编辑器操作提供一个工具：
加一个节点、设一个引脚、连两个引脚、编译。UE-AssetDoc 则以整个资产为单位：AI 把一个资产读成一份 JSON 文档，编辑这份
文档，查看会改变什么，再交还给编辑器，由编辑器用它自己的编辑操作来应用。本页说明这个思路，以及你和你的 AI 可以依赖什么。
安装见 [README](../README.md)；客户端配置和工具列表见 [MCP/README.md](../MCP/README.md)。

## 1. 一个资产就是一份文档

AI 编辑的每个资产都是一份 **AssetDocument**：一个描述资产全部作者状态的 JSON 对象。

```jsonc
{"SchemaVersion": 1, "Target": "/Game/AI/BT_Enemy", "Class": "/Script/AIModule.BehaviorTree",
 "Action": "CreateOrUpdate", "Definitions": {}, "Properties": {}, "Body": {"BlackboardAsset": ..., "Tree": ...}}
```

- `Target` 是资产的包路径，`Class` 是精确的类路径，`Action` 是 `Create`、`Update` 或 `CreateOrUpdate`。
- `Properties` 存放反射属性，与 Details 面板一致。`Body` 存放 Details 面板不显示的内容：Behavior Tree 的节点、
  Data Table 的行、Blueprint 的图、Niagara 系统的发射器。

工作是一个五步循环，每一步对应一个工具：

| 步骤 | 工具 | 是否改动资产 |
| --- | --- | --- |
| 提取 | `extract_asset_document` | 否 |
| 编辑 | （AI 编辑 JSON） | 否 |
| 校验 | `validate_asset_document` | 否 |
| 对比 | `diff_asset_document` | 否 |
| 应用 | 写入 sidecar 文件，`apply_asset_document_file` | 是 |

文档是声明式的：它写的是资产应当是什么样子，而不是通过哪些点击到达那里。两条规则让这一点变得精确：

- **省略**的 `Body` 键保持资产的这一部分不变，所以一份文档可以只列出它要改的部分。
- **出现**的键是这一部分完整的目标状态：没有列出的条目会被删除。Data Table 的 `Rows` 列表就是整张表；Behavior Tree
  的 `Tree` 就是整棵树。

顶层 `Properties` 始终是稀疏的：只写入列出的属性。原样应用一份提取结果不会改变任何东西。

文档作为 sidecar 文件放在资产旁边：`/Game/AI/BT_Enemy` 对应 `<Content dir>/AI/BT_Enemy.assetdoc.json`。写入这个文件
就是应用：编辑器监视 sidecar 文件，在文件写入约半秒后应用它。`apply_asset_document_file` 返回结果（或文件监视器那次
应用的结果）。

## 2. 编辑器保真

应用走的是编辑器自己的编辑操作：Content Browser 使用的工厂、图的 schema action、与 Details 面板相同的属性写入路径、
编辑器自己的结构体和枚举工具。结果就是一个人手工做同样修改得到的资产。几个例子：

- 带 `RenamedFrom` 的 Data Table 行通过 Rename Row 重命名，而不是先删除再添加。
- 新建 Mirror Data Table 时，提供 New Mirror Data Table 对话框会要求的 Skeleton（`FactoryContext`），工厂像在编辑器
  中一样填充它的行。
- `float` 属性存储最接近的 32 位浮点数，与 Details 面板一致。
- 用户定义结构体改变时，编辑器会更新使用它的 Data Table 和 Blueprint，与手工编辑该结构体时一样。

编辑器做不出来的东西会被拒绝并给出原因，绝不近似处理：

- 会打开模态对话框的工厂被取消，不留下任何东西（`FactoryCreate.InteractiveFactory`）。
- 编辑器会改写的值（被钳制、被改名的 socket）在任何改动之前被拒绝（`Generic.PropertyValueNotStoredExactly`），诊断
  会给出资产实际会存下的值。
- Data Table 创建后不能更换行结构体（`DataTable.RowStructImmutable`）；消息会提示新建一张表。

引擎本身从不被修改，插件也不使用引擎的私有头文件。

## 3. 先检查再修改

`validate_asset_document` 运行应用时的全部检查，但不改变任何东西。成功时，它的消息说明应用会做什么：
`would create '<target>'`、`would change N value(s)`（以及会删除哪些条目），或 `would change nothing`。

`diff_asset_document` 把每处改动列为 `{path, kind, before, after}`：`path` 是指向文档内部的 JSON Pointer，`kind` 是
`Add`、`Remove` 或 `Replace`，`before` 是资产中的值，`after` 是文档中的值。payload 把结果分放在几个容器里：改动在
`changed` 中；`unchanged`、`skipped`（未比较的区域）和 `failed` 通常为空；Behavior Tree profile 还有 `added` 和
`removed`。`changed` 为空（所有容器都为空）表示资产已经一致。

提取结果带有 `BaseRevision`。如果有人在提取之后改了资产，校验和应用都会拒绝这份文档（`Document.StaleBase`），这样
AI 不会覆盖它没有看到的修改。

## 4. 要么全部成功，要么什么都不变

一次应用要么整体成功，要么让资产保持原样：

- 失败的应用会被回滚：资产提取出来与之前相同，编译状态不会变差，磁盘上的文件不会被重写，也不会留下新的包。
- 应用后编译出错的 Blueprint 以 `Graph.FinalizeFailed` 失败并被回滚。编译警告作为成功结果中的警告返回。
- 超时或连接中断后，sidecar 可能已经被应用。此时 `diff_asset_document` 返回空，就说明已经应用了。

AI 的应用不是编辑器 Undo 历史中的一项；Ctrl+Z 撤销不了它。要退回，就应用之前的文档，或使用版本控制。为了避免之后的
Ctrl+Z 让资产处于改了一半的状态，应用会移除涉及该资产的 Undo 条目，并在结果中说明（`Undo.HistoryTrimmed`）。关卡编辑
则不同（第 8 节）。

## 5. 为 AI 读者设计

每个结果的形状相同：`{success, message, target, asset_path, sidecar_file_path, saved_asset, wrote_sidecar,
diagnostics[], compile?, payload?}`。一条诊断是 `{path, code, message, severity?, details?}`：

- `path` 是指向 AI 所写文档中某个字段的 JSON Pointer，AI 因此知道该改哪里。
- `code` 是稳定的，前缀说明是哪一层拒绝的：`Meta.*` 文档头，`Generic.*` 通用 profile，`DataTable.*` 或 `Widget.*`
  某个精确 profile，`Graph.*` Blueprint 图，`PropertyWrite.*` 编辑器拒绝的值。
- 失败结果把根本原因放在最前：`diagnostics[0]`。许多“找不到”类的消息会给出相近的名称。

工具描述很短，细节在 AI 通过工具读取的指南里：`get_asset_document_schema` 返回核心指南以及每个 profile 的一节（它接受
的 `Body` 形状、它的诊断、它的易错点）；`get_level_editing_guide` 涵盖关卡编辑、试玩、导入和设置。
`inspect_asset_document_target` 列出一个类的可写属性，或一个现有资产可供构建的内容；`find_assets` 和 `list_types`
用来查找资产、类、结构体和枚举。

## 6. Profile

文档的 `Class` 决定使用哪个 profile，profile 负责该资产家族 `Body` 的含义。精确 profile 覆盖 Blueprint 和 Widget
Blueprint、Animation Blueprint 和动画资产、Behavior Tree 和 Blackboard、StateTree、EQS 查询、Data Table 以及用户定义
结构体和枚举、曲线、Material 和 Material Instance、Niagara、MetaSound 和 Sound Cue、IK Rig 和重定向器、Control Rig、
Physics Asset、Level Sequence 等。`inspect_asset_document_profile` 可以查出某个类归哪个 profile。

其他任何类都使用**通用 profile**：它的反射属性按 Details 面板的方式写入，用 Content Browser 使用的工厂创建。例如
Data Asset、Input Action 和 Input Mapping Context 就使用这个 profile。

[`docs/architecture/`](architecture/) 中的支持矩阵按资产家族记录哪些内容可以读取、创建和修改。人们在编辑器里常改的
东西都应当在其中；不支持的少见形式会被拒绝，并给出说明替代做法的诊断。

## 7. 编辑器就是服务器

AI 和编辑器之间没有单独的进程。插件在编辑器内部以 `http://127.0.0.1:8559/mcp` 提供 MCP；一个小的 stdio 桥
（`ue-assetdoc`，Node.js）为只能启动 stdio 服务器的客户端做转发。插件的模块全部是编辑器模块。

这个端点能以运行编辑器的用户身份操控编辑器，包括 `execute_python` 和 `delete_assets`，所以默认只限本机：

- 它只监听回环接口，并拒绝 `Host` 不是 `localhost`、`127.0.0.1` 或 `[::1]` 的请求，以及 `Origin` 不是 localhost 页面
  的请求。
- 发送数据的请求必须是 `application/json`；网页不经 CORS 预检就无法向其他源发送这种请求，而服务器不响应任何预检。
- 可以要求 bearer token，也可以在 Project Settings 中关闭 `execute_python` 和 `delete_assets`。二者默认开启。这两个开关不是沙箱：能连上端点的客户端仍能让编辑器运行代码（例如它编写的 Blueprint 在 Play In Editor 中运行），所以安全边界是谁能连上端点。
- 远程连接默认关闭；开启后总是要求 token。

请把对这个端点的访问当作编辑器用户的一个 shell 会话来对待。[SECURITY.md](../SECURITY.md) 说明暴露了什么以及如何
报告漏洞。

## 8. 文档之外

关卡不是资产文档。`query_level` 读取打开的关卡，`edit_level` 按批次修改它：每批先检查（`dry_run: true` 显示计划），
整体应用或整体回滚，且是 Edit > Undo 中的一项。`play` 和 `query_play` 运行 Play In Editor、注入输入、等待条件、读取
运行中的游戏；`get_viewport_screenshot` 显示玩家看到的画面。编辑器工具还能导入和导出文件，保存、重新加载和删除资产，
打开和关闭资产编辑器，修改项目设置，其余的事情则在编辑器里运行 Python。

## 9. 示例：修改一棵 Behavior Tree

资产是指南中的示例树 `/Game/AI/BT_Enemy`：一个 Selector，第一个子节点追击 `TargetActor`，第二个子节点等待。AI 想把
等待时间从 2 秒改为 3.5 秒。

**提取。** `extract_asset_document {"asset_path": "/Game/AI/BT_Enemy"}` 返回文档。相关部分（节选）：

```jsonc
"Children": [
  {"Id": "00000000000000000000000000000003", "Class": "/Script/AIModule.BTComposite_Sequence", ...},
  {"Id": "00000000000000000000000000000006", "Class": "/Script/AIModule.BTTask_Wait", "Editor": {...},
   "Properties": {"WaitTime": {"DefaultValue": 2.0, "Key": "None"}}}
]
```

**编辑。** AI 把 `"DefaultValue": 2.0` 改为 `3.5`，其他内容都保留，包括保持节点身份的各个节点 `Id`。

**校验。** 用编辑后的文档调用 `validate_asset_document`，返回 `AssetDocument valid; Apply would change 1 value(s)`，
payload 中有 `change_count: 1`。

**对比。** `diff_asset_document` 返回一处改动：

```json
{"path": "/Body/Tree/Root/Children/1/Properties/WaitTime/DefaultValue",
 "kind": "Replace", "before": 2.0, "after": 3.5,
 "status": "changed", "current": 2.0, "desired": 3.5}
```

（`status`、`current` 和 `desired` 是 Behavior Tree profile 对同一处改动的自有字段名。）

**应用。** AI 把文档写入 `<Content dir>/AI/BT_Enemy.assetdoc.json`，再用该路径调用 `apply_asset_document_file`。结果是
`BehaviorTree document applied`（如果文件监视器先应用了它，调用会返回那次的结果，并带有
`applied_by: "file_watcher"`）。资产已保存；打开着的 Behavior Tree 编辑器会被关闭，并在结果上重新打开
（`BehaviorTree.EditorReopened`）。此时对同一份文档做 diff 为空。

**写错时。** 如果 AI 在 Move To 节点上写了 `"Key": "TargetActr"`，校验会在任何改动之前拒绝它：

```
[UnknownBlackboardKey] at /Body/Tree/Root/Children/0/Children/0/Properties/BlackboardKey/Key:
The key 'TargetActr' does not exist in the effective BlackboardAsset. Close: TargetActor.
```

资产之外的一点说明：Move To 需要导航网格。没有 `NavMeshBoundsVolume` 的关卡没有导航网格，敌人在 Play In Editor 里会站着不动
（`query_play` 显示 `move_status` 为 `Idle`）。可以用 `edit_level` 生成
`/Script/NavigationSystem.NavMeshBoundsVolume`，缩放到覆盖地面；引擎随后会生成导航网格。

## 10. 它不是什么，以及它的限制

- 它不是运行时或游戏玩法插件；它在编辑器里工作。
- 它不取代编辑器。它以同样的方式编辑同样的资产，所以人可以打开并修改 AI 做出的任何东西，下一次提取会反映这些修改。
- 有些东西没有文档：关卡（用关卡工具）、GameplayTags（编辑其 ini，或通过 `execute_python` 使用
  GameplayTagsManager 编辑器 API），以及向 Data Table 导入 CSV 或 JSON（改为直接编写行）。
- 有文档之外的未保存修改的资产会被拒绝（`DirtyExistingAssetTransactionUnsupported`）：先保存（`save_assets`）再重新
  提取，或重新加载以丢弃这些修改。
- `/Game` 之外的资产（引擎和插件内容）可以读取和引用，但不能编写；提取后把副本应用到 `/Game` 下，或使用
  `duplicate_asset`。
- 大资产对应大文档：一个 Control Rig 的提取结果可达数百 KB。
- 已知的未解决问题及其绕行办法见
  [docs/architecture/asset-document-known-issues.md](architecture/asset-document-known-issues.md)。

## 延伸阅读

- [MCP/schemas/AssetDocument.md](../MCP/schemas/AssetDocument.md)：AI 读取的核心指南（工作流程、文档头、省略与空值、
  校验与对比、诊断）。
- [MCP/schemas/assetdocument/](../MCP/schemas/assetdocument/)：每个 profile 一节。
- [docs/architecture/asset-document-design-rules.md](architecture/asset-document-design-rules.md)：profile 遵循的规则
  （回滚、引擎 API、支持矩阵）。
