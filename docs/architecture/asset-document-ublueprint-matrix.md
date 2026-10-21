# UBlueprint 支持矩阵（Step 2 切换）

**状态：** 2026-10-03 切换切片。上位规则：[设计规则](asset-document-design-rules.md) §3（回滚 = 功能等价）、§5（支持矩阵、能力不倒退）。
图原子操作契约：[graph primitives v1](asset-document-graph-primitives.md)。

`/Script/Engine.Blueprint`（精确类；Widget/Anim Blueprint 不在本矩阵内）由 Python Profile
`profile.ublueprint.v2`（`Content/Python/asset_document/profiles/ublueprint_runtime.py`）承担读 / 新建 / 修改。
旧 C++ `FUBlueprintAssetDocumentProfile` 代码保留但不再注册（同一类型只有一个可写 runtime）。

列说明：**旧 C++** = 被替换的 `FUBlueprintAssetDocumentProfile`；**Python** = 本切片交付；
✅ 支持，➖ 不支持（返回稳定 diagnostic），— 不适用。

## 1. Body key 与常见编辑

| 编辑操作（人在编辑器中常做） | Body 位置 | 旧 C++ | Python | 说明 |
|---|---|---|---|---|
| 新建 Blueprint（指定父类） | `ParentClass` + `Action:Create/CreateOrUpdate` | ✅ | ✅ | 一次 ApplyFile 新建并填充全部 Body；与 Content Browser 相同走 `UBlueprintFactory`（lifecycle `blueprint_exact.v1` / `blueprint_factory.v1`） |
| 新建宏库 / Blueprint 接口 / 函数库 | `BlueprintType`: `"MacroLibrary"` \| `"Interface"` \| `"FunctionLibrary"`（缺省 `"Normal"`） | ➖ | ✅ | 与 Content Browser 相同用各自的 factory（`UBlueprintMacroFactory` / `UBlueprintInterfaceFactory` / `UBlueprintFunctionLibraryFactory`，lifecycle 可选 handle `factory_class`），新资产没有事件图 / 构造脚本。宏库父类按 Blueprint Class 的类选择器选（不能是 LevelScriptActor / AnimInstance / Interface 子类）；接口父类只能是 `/Script/CoreUObject.Interface`、函数库只能是 `/Script/Engine.BlueprintFunctionLibrary`（Create 省略 `ParentClass` 时即取此值）。各类型只有编辑器提供的区域：宏库只有 `MacroGraphs`，接口只有 `FunctionGraphs`（签名）与 `ClassDefaults`，函数库只有 `FunctionGraphs`；其它区域非空 → `UBlueprint.BlueprintTypeRegionUnsupported`（成员原语对变量 / 接口同样拒绝：`BlueprintMember.BlueprintTypeUnsupported`；图原语按 `NewDocument_IsVisibleForType` 拒绝宏库的函数图、非 Normal 类型的宏图）。类型创建后不可改：Update 写了不同类型 → `UBlueprint.BlueprintTypeImmutable`；省略 = 保留。Extract 对 Normal / const（父类为 const 类）省略该 key |
| 改父类（Reparent） | `ParentClass` | ✅ | ✅ | `ParentClass` 可写类路径字符串，兼容旧 `{"Kind":"ClassRef","Class":...}` |
| 实现 / 移除接口 | `ImplementedInterfaces` | ✅ | ✅ | 元素可写类路径字符串，兼容旧 `{"Interface":{ClassRef}}` |
| 变量：增删、改类型、默认值、Category、Tooltip | `Variables` | ✅（仅标量 / object / class） | ✅ | 类型复用图 pin 类型写法（`float`、`object:/Script/Engine.Actor`、`array:int`、完整对象）；兼容旧 `{"PinCategory",...}` |
| 变量：容器（Array/Set/Map）、struct、enum、soft 引用 | `Variables[].Type` | ➖ | ✅ | |
| 变量：Instance Editable / Read Only / Expose on Spawn / Private | `Variables[]` 布尔字段 | ➖ | ✅ | |
| 仅大小写的改名（变量、组件、事件分发器、Timeline、函数/宏/事件图、函数与分发器参数、自定义事件） | 同一条目，名字只差大小写 | ➖ | ✅ | 按名字识别，FName 不区分大小写；差异按精确字符串检测。成员是同一个对象（GUID、签名图、SCS 节点、Timeline 模板、节点引用保留），Extract 显示新大小写，再次 Apply 为 no-op。原语 `blueprint.member.rename.v1`（`RenameMemberVariable` / `RenameComponentMemberVariable` / `RenameTimeline`），图用 `graph.rename.v1`，参数用 `SetUserPins` 的原地改名。其它名字的改名仍是删除 + 新增（变量改名并保留引用不支持，需在编辑器里做后重新 Extract） |
| 事件图：任意可从菜单放置的节点（Branch、数学运算、宏、Cast…） | `UbergraphPages` | 部分（5 种 Tier-1 节点类） | ✅ | 节点按 action key 放置（`K2:Branch`、`K2:PromotableOperator:Multiply`…） |
| 自定义事件（含参数） | `UbergraphPages[].Nodes` | ➖ | ✅ | `K2:CustomEvent` + `Name` |
| 函数：新建、参数 / 返回值、Pure/Const/Access/Category | `FunctionGraphs[].Signature` | 图体部分支持，签名 ➖ | ✅ | |
| 宏图 | `MacroGraphs` | 部分 | ✅ | |
| Select 节点的 Index 类型（pin 上的类型选择器：bool / byte / 枚举 / int / int64） | 节点 `PinTypes: {"Index": "byte:/Game/E_Mode.E_Mode"}` | ➖ | ✅ | `graph.node.pin_type.set.v1`（`SGraphPinIndex::OnTypeChanged`），在 spawn 之后、默认值与连线之前设置，枚举的选项 pin（`NewEnumerator0`…）随之出现；Extract 写出已选定（非 wildcard）的类型；live Select 已是另一类型时重建节点（编辑器不能改已选定的类型） |
| 组件：新增 / 删除 / 挂接（OwnedSCS） | `Components` | ✅ | ✅ | `{"Name","Class","AttachTo","Properties"}`；兼容旧 `Key/Scope/Root`（多个 Root、Root+AttachTo、Inherited/Native 带 AttachTo 均拒绝：`UBlueprint.InvalidAttach`） |
| 组件属性覆盖（自有模板） | `Components[].Properties` | ✅ | ✅ | |
| 父 Blueprint 组件覆盖（Inherited） | `Components[]` + `Scope:"Inherited"` | ✅ | ✅ | |
| 原生组件覆盖（Native，如 CharacterMovement） | `Components[]` + `Scope:"Native"` | ✅ | ✅ | |
| Class Defaults（CDO 属性） | `ClassDefaults` | ✅（无 struct/容器） | ✅ | 值为 typed JSON（struct → 对象，容器 → 数组） |
| Class Settings（Description / Category / Namespace…） | `Properties` | ✅ | ✅ | UBlueprint 可编辑反射属性 |
| 事件分发器：增删、签名（Inputs）、Category、Tooltip | `EventDispatchers` | ➖ | ✅ | `[{"Name","Inputs":[{"Name","Type"}],"Category","Tooltip"}]`；成员变量（`PC_MCDelegate`）与签名图同建同删（`FBlueprintEditor::OnAddNewDelegate` / `SMyBlueprint::OnDeleteDelegate`）；无复制设置（编辑器对分发器禁用）；不同名字的改名 = 删除 + 新增，仅大小写不同是同一分发器的改名（见 §1 大小写改名）；与变量同名 → `UBlueprint.DuplicateName` |
| 分发器节点：Call / Bind / Unbind / Unbind all / Assign / 分发器事件 | `UbergraphPages[].Nodes` | ➖ | ✅ | `K2:CallDelegate:OnDied`、`K2:AddDelegate:…`、`K2:RemoveDelegate:…`、`K2:ClearDelegate:…`、`K2:AssignDelegate:…`（编辑器随之放置的 `<名>_Event` 自定义事件由文档中连到其 `Delegate` pin 的节点接管，文档未列出则删除）、`K2:CustomEvent:OnDied`（拖放 “Event” = 带分发器签名、名为 `OnDied_Event` 的自定义事件）；extract 统一写成 `K2:CustomEvent` + `Name` + `Inputs` |
| 自定义事件参数 | `UbergraphPages[].Nodes[].Inputs` | ➖ | ✅ | 仅 `K2:CustomEvent`；`[{"Name","Type"}]`，无默认值；只在文档写出时比较 |
| Timeline：增删、Length / Use Last Keyframe / AutoPlay / Loop / Replicated / Ignore Time Dilation / Tick Group | `Timelines` | ➖（非空拒绝） | ✅ | 模板（`UTimelineTemplate`）与 `K2Node_Timeline` 同建同删；新节点按文档 `UbergraphPages` 中同名 `K2:Timeline` 节点放置，否则放在第一个事件图右侧；两区都给出时名字必须一致（`UBlueprint.TimelineNodeMismatch`）；`K2:Timeline` 节点必须写 `Name` |
| Timeline 轨道：Float / Vector / LinearColor / Event，关键帧，外部曲线资产，显示顺序 | `Timelines[].Tracks` | ➖ | ✅ | `Keys` 为数组（Float/Event）或按通道 `{X,Y,Z}` / `{R,G,B,A}`；或 `Curve` 指向外部曲线（内外切换同 `STimelineEdTrack`）；关键帧字段同 CurveFloat；Cubic + Auto/SmartAuto 的切线由曲线编辑器推导，不提取不比较；Constant 邻接的切线清零与 `FRichCurve::AutoSetTangents` 一致 |
| 函数局部变量：增删、类型、默认值、Category、Tooltip、顺序 | `FunctionGraphs[].LocalVariables` | ➖ | ✅ | `[{"Name","Type","Default","Category","Tooltip"}]`；省略 = 保留，`[]` = 清空；按 `FBlueprintEditorUtils` 局部变量 API 编辑，其 Get / Set 节点为 `K2:Get:<名>` / `K2:Set:<名>`；map 写作 `{"Category": <键类型>, "Container": "map", "ValueType": <值类型>}`（同成员变量） |
| 类型提升运算符的 Pin Conversions（vector × float、int64 操作数…） | 节点 `PinTypes` | ➖ | ✅ | 只在操作数 / 结果类型不全相同或与另一运算符相连时写出；`UK2Node_PromotableOperator::ConvertPinType`，在连线之前按顺序设置；连线的类型提升（`make_with_promotion`）与编辑器拖线一样直接连接 |
| Variable Get 的 Validated Get / Branch、函数调用的 Show Exec pins | 节点 `Properties: {"Pure": false}` | ➖ | ✅ | 只在不同于默认时写出；`SetNodeSetting("Pure")` |
| 折叠图（Collapse Nodes） | `K2:Composite` 节点 + 同名 `Subgraphs[]`（`Owner`） | ➖ | ✅ | 子图 `Signature` 即折叠节点的 pin，先于父图连线建立 |
| Split pin | — | ➖ | ➖ | 图原子操作 v1 不支持 |
| 读取（Extract / Inspect / Diff） | 全部上述 key | ✅ | ✅ | extract → apply → extract 为空操作；编辑器自动生成、只含隐式节点的图（空 EventGraph、构造脚本、接口桩）在 Diff 中被容忍，文档无需列出 |

## 2. 省略语义

| Body key | 旧 C++ | Python |
|---|---|---|
| 省略 `Variables` / `ImplementedInterfaces` / `Components` / `ClassDefaults` / `UbergraphPages` | 视为空（删除全部） | **保留现状** |
| 省略 `EventDispatchers` / `Timelines` | — | 保留现状（`[]` 删除全部） |
| 省略 `FunctionGraphs` / `MacroGraphs` | 保留现状 | 保留现状 |
| 给出某个 key | 该区域的完整期望状态 | 同左（未列出的变量 / 组件 / 图被删除，`ClassDefaults` 未列出的属性恢复为父类值） |
| `Variables[]` 某行省略 Category / Tooltip / 布尔标志 | 恢复默认 | 恢复默认（与 extract 只输出非默认值一致）；省略 `DefaultValue` 保留现值 |
| `EventDispatchers[]` / `Timelines[]` 某行省略字段 | — | 恢复默认（行 = 完整的分发器 / Timeline；省略 `Tracks` = 无轨道，省略 `Inputs` = 无参数） |
| 文档顶层 `Properties`（Class Settings） | 合并 | 合并：只写列出的属性 |
| `Inherited` / `Native` 组件行省略 | 覆盖被清除 | 覆盖被清除（Inherited 删除 ICH 记录，Native 属性恢复父类值） |

改为“省略 = 保留”的原因：旧行为对 Ubergraph 与函数图不一致；AI 写只含部分 key 的文档时不会误删事件图。
写 `[]` / `{}` 仍可删除全部，能力不减少。

## 3. 回滚与派生状态

- 已存在目标：Core 整包快照（`FAssetDocumentPackageSnapshot`）+ Blueprint 派生状态重建器
  （`AssetDocumentGraph` 模块注册：`RefreshAllNodes` + `CompileBlueprint`），满足 §3 功能等价。
- 新建目标：factory 回滚 seed 删除新建资产，不留 package / registry 条目。
- Python 侧通过 `SnapshotCoveredMutationV1` 预检投影声明“本请求的写操作由整包快照覆盖”，
  Apply/Repair 中图与成员原子操作按需动态调用，无需逐步预声明。仅 snapshot-covered 请求对 Blueprint 包做快照，
  旧 C++ 路径（若重新注册）仍走自己的 typed rollback。
- `blueprint.member.query.v1` 可带 `offset`：数组 / 对象区域（variables、dispatchers、timelines、components、class_defaults、properties）按整条目切页，每页重复标量头部，返回 `total` 与 `next_offset`（最后一页为 null）；Python 总是分页并合并，大 Blueprint（如 FS_MasterField）不再 `Graph.ResultTooLarge`。单个条目超出预算仍为 `Graph.ResultTooLarge`（消息点名条目）。不带 `offset` 的行为不变。
- 原子操作 provider：成员 `blueprint`（`blueprint.member.*`），图 `graph`（`graph.*`，K2 适配器）；
  编译失败 → `Graph.FinalizeFailed`，整包回滚。
- 编辑器打开时 apply：原地编辑。编译路径刷新编辑器；之后 open-editor handler（`AssetDocumentGraphBlueprintOpenEditorHandler.cpp`，`RefreshInPlace`，所有 UBlueprint 子类共用）关闭已删除图的标签页（引擎只在用户自己删除图时关闭），并像编辑器 Undo 一样调用 `RefreshEditors`（My Blueprint、Components 树、Details、预览）。失败 apply 回滚后编辑器显示回滚状态，人工编辑与 Undo 正常。测试 `Blueprint.OpenEditorApply`。

## 4. 未做与原因

- **事件分发器 / Timeline 改名（不同名字）**：按名字识别，改名 = 删除 + 新增（引用节点需同时改写）。仅大小写的改名受支持，见 §1。
- **Timeline 仅大小写改名**：`RenameTimeline` 最后的 `UObject::Rename("<Name>_Template")` 与唯一性校验会让编辑器自己把它当作没有变化，原语经一个唯一的中间名两步改名（同一编辑器函数，模板对象不变）。
- **组件仅大小写改名与绑定事件**：引擎的 `UK2Node_ComponentBoundEvent::HandleVariableRenamed` 把“新名 == 绑定名”（仅大小写改名恒成立）当作重复绑定而报错并保留旧大小写；原语在调用期间摘下绑定事件，事后写回新大小写（E2E `UBlueprint.CaseRenameBoundComponent`：`Mesh` → `mesh` 带 `OnComponentBeginOverlap`，同一绑定事件节点、编译后 `UComponentDelegateBinding` 指向新名字、reload 与改回）。
- **改签名后的孤立 pin**（删除分发器输入，而节点仍接着它；其它经 finalize 的孤立 pin 走同一路径）：与编辑器一致，不静默断线。
  编辑器：`FBaseBlueprintGraphActionDetails::OnParamsChanged` 只重建签名节点（关闭 orphan 保存），调用 / 绑定 / 自定义事件节点在
  结构刷新（`RefreshAllNodes`）时经 `UK2Node::RewireOldPinsToNewPins` 把“有连线或非默认值”的旧 pin 保留为 `bOrphanedPin`；
  编译时 `UK2Node::ValidateOrphanPin`：有连线 = **编译错误**（`In use pin ... no longer exists`），无连线但非默认值 = **警告**；
  断开连线后 `UK2Node::PinConnectionListChanged` 自动移除孤立 pin。AssetDocument 同样处理：
  - 仍有连线：Apply 失败并回滚，诊断 `Graph.OrphanedPinLinked`，路径指向文档节点（`/Body/UbergraphPages/0/Nodes/2`），消息点名节点与 pin；
    文档省略图区域（= 保留）时路径与节点 id 取自 Extract 视图。
  - 只剩非默认值：Apply 成功，Result 带 `Graph.OrphanedPinDefault`（警告）同样点名节点与 pin。
  - 同一次 Apply 里同时删除输入和连线（或把 pin 值也写回默认）：无孤立 pin、无诊断。
  - Extract 不写出孤立 pin 的值，但保留指向它的连线；原样重新应用 extract 是 no-op（不编译、不保存）。
  失败诊断来自 `graph.finalize.v1`（编译消息带 `orphaned` 标记）；裸编译（Repair 阶段）通过 executor 的 `binder` 把 GUID 映射回文档节点。
- **变量改名（不同名字）保引用**：需要 `FBlueprintEditorUtils::RenameMemberVariable` 与文档内节点 id 的对应关系，留待后续。
- **split pin**：图原子操作 v1 范围外。

## 5. 已知限制（Diff）

- 图比较按节点重命名不变的结构比较（WL 颜色细化 + 连线多重集 + pin 覆盖 + 函数签名）；
  极少数非同构图可能被判为相同。
- 两种模式：
  - **Diff 操作（严格）**：文档写出的每个值都必须在 live 资产上成立——节点位置、NodeGuid、
    注释、Name、Extras、节点属性包、GraphGuid，以及 extract 中缺失的 Properties /
    组件属性 / ClassDefaults / 变量默认值都算差异。空 Diff ⇒ 应用不会改变任何东西，
    因此 ApplyFile 的“未变更”探针（`AssetDocumentGenericRuntimeRoute.cpp` DispatchApplyFile）
    对 Blueprint 生效：原样重新应用 extract 不编译、不置脏、不保存、不重写文件。
    代价：显式写出等于父类/默认值的属性时 Diff 报差异，应用照常执行（保守）。
  - **应用后校验（宽松）**：不比较上述编辑器会归一化或省略的字段，文档写了但 extract
    缺失的组件 / ClassDefaults 属性按“等于父类值”接受。
