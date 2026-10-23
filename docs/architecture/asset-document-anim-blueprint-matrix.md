# AnimBlueprint 支持矩阵

上位规则：[设计规则](asset-document-design-rules.md)
§3（回滚 = 功能等价）、§4（编辑器等价的原始编辑放在独立文件并有行为测试）、§5（支持矩阵、能力不倒退）。
图原子操作契约：[graph primitives v1](asset-document-graph-primitives.md)（§AnimBlueprint 增补）。
UBlueprint 部分（变量、接口、事件图 / 函数 / 宏、Class Defaults、Class Settings）与
[UBlueprint 矩阵](asset-document-ublueprint-matrix.md) 相同，本文只列出差异。

`/Script/Engine.AnimBlueprint` 由 Python Profile `profile.anim_blueprint.v1`
（`Content/Python/asset_document/profiles/anim_blueprint_runtime.py`，图编解码
`Content/Python/asset_document/graphs/schemas/anim.py`）在 `blueprint_profile` 路由上承担
读 / 新建 / 修改。旧 C++ `FAnimBlueprintAssetDocumentProfile` 代码保留但不再注册。

图原子操作来自通用图 provider（`graph.*`）；AnimGraph / 状态机 / ABP host / 设置适配器在
动画侧模块 `Source/AssetDocumentAnimation/Private/AnimBlueprint/` 注册到公共适配器注册表，Core 不含动画代码。
成员原子操作复用 `blueprint.member.*`，新增 `blueprint.member.setting.set.v1`（ABP 设置）。

列说明：**旧 C++** = 被替换的 `FAnimBlueprintAssetDocumentProfile`；**Python** = 当前 Profile。
分类：**Supported**（Extract + Apply + Diff）、**Read-only**（只 Extract / Diff，写入不同值返回稳定 diagnostic）、
**Unsupported**（不在文档中，或只接受 `[]`；给出原因）。✅ 支持，➖ 不支持，部分 = 有限支持。

## 1. 资产与类设置

| 编辑操作（人在编辑器中常做） | Body 位置 | 旧 C++ | Python | 分类 | 机制 / 说明 |
|---|---|---|---|---|---|
| 新建 ABP（选父类 + 目标骨骼） | `ParentClass` + `TargetSkeleton` + `Action: Create` | ✅ | ✅ | Supported | lifecycle `anim_blueprint_exact.v1`：反射式 `UAnimBlueprintFactory`（ParentClass / BlueprintType / TargetSkeleton），与 Content Browser 相同；父类须为 `UAnimInstance` 子类；骨骼通过可选 create-context 句柄 `factory_context` 传入。一次 apply 新建并填充 |
| 新建模板 ABP（无骨骼） | `Template: true`，无 `TargetSkeleton` | ✅（对象形式） | ✅ | Supported | `bTemplate = Skeleton == nullptr`；`Template` 现在是 bool；模板带骨骼 → `AnimBlueprint.InvalidTemplateSkeleton` |
| 改 Template | `Template` | ➖ | ➖ | Unsupported | 创建后不可变（编辑器也不提供）：`AnimBlueprint.TemplateImmutable` |
| 改目标骨骼（Class Settings → Target Skeleton） | `TargetSkeleton`（兼容顶层 `Properties.TargetSkeleton`） | ✅ | ✅ | Supported | `blueprint.member.property.set`；非模板置空 → `AnimBlueprint.MissingTargetSkeleton` |
| 改父类（Reparent） | `ParentClass` | ✅ | ✅ | Supported | `blueprint.member.parent.set`，遵守资产的 reparent 规则 |
| 实现 / 移除接口（含 Anim Layer Interface） | `ImplementedInterfaces` | ✅ | ✅ | Supported | 与 UBlueprint 相同；实现 ALI 后其 layer 图出现在 `AnimLayers` |
| 变量（My Blueprint） | `Variables` | ✅ | ✅ | Supported | 与 UBlueprint 相同 |
| Class Defaults | `ClassDefaults` | ✅ | ✅ | Supported | 与 UBlueprint 相同 |
| Class Settings：描述 / 分类 / 命名空间 / 编译模式等 | 顶层 `Properties` | ✅（`Body.BlueprintClassSettings`） | ✅ | Supported | `blueprint.member.property.set`；旧 key 被拒并提示新位置 |
| Class Settings：优化项（多线程更新、Blueprint 使用警告、Linked Layer 实例共享） | 顶层 `Properties`（`bUseMultiThreadedAnimationUpdate` 等） | ✅（`Body.Optimization`） | ✅ | Supported | 同上 |
| Class Settings：Default Binding Class | 顶层 `Properties.DefaultBindingClass` | ✅（`Body.DefaultBindingClass`） | ✅ | Supported | 同上 |
| 预览网格 / 预览 ABP / 应用方式 / Tag（Preview Scene Settings） | `Preview {Mesh, AnimationBlueprint, ApplicationMethod, Tag}` | ✅（字段名不同） | ✅ | Supported | `blueprint.member.setting.set`；兼容旧字段名 `PreviewSkeletalMesh` 等；默认值不输出 |
| Sync Groups（Class Settings） | `SyncGroups: ["GroupA", ...]` | ✅ | ✅ | Supported | 兼容旧 `{"Name"}` 形式 |
| Sync Group 颜色 | `SyncGroups[].Color` | ➖ | ➖ | Unsupported | UE 5.7 无语义读取方（旧实现同样拒绝）：`AnimBlueprint.UnsupportedRegion` |
| 子 ABP 的父资产覆盖（Asset Override Editor） | `ParentAssetOverrides: [{ParentNodeGuid, Asset}]` | ✅ | ✅ | Supported | `blueprint.member.setting.set` |
| 缩略图相机 | `Thumbnail {OrbitPitch, OrbitYaw, OrbitZoom}` | ✅ | ✅ | Supported | `null` = 保持现状（编辑器无法删除缩略图信息） |
| Anim Layer Interface 资产的创建与修改（Content Browser → Animation Layer Interface） | `ParentClass: "/Script/Engine.AnimLayerInterface"` + `AnimLayers` | ➖ | ✅ | Supported | factory `BlueprintType = BPTYPE_Interface`（无 TargetSkeleton）；layer 只含 `Output`（`LayerGroup`）与输入姿势，不得有连线、变量、接口、K2 图（`AnimBlueprint.InvalidLayerInterface`）；ABP ↔ ALI 类型创建后不可变（`AnimBlueprint.BlueprintTypeImmutable`）；E2E `AnimGraph.LayerInterfaceRoundTrip` |
| 组件 | `Components` | ➖（只接受 `[]`） | ➖（只接受 `[]`） | Unsupported | ABP 没有组件 |
| Timeline | `Timelines` | ➖（只接受 `[]`） | ➖（只接受 `[]`） | Unsupported | 编辑器只在 Actor 系 Blueprint 中提供 Timeline（`FBlueprintEditorUtils::DoesSupportTimelines`），非空 → `AnimBlueprint.UnsupportedRegion` |
| 事件分发器：增删、签名、Category、Tooltip；Call / Bind / Unbind / Assign / 分发器事件节点 | `EventDispatchers`（旧 key `DelegateSignatureGraphs` 作为别名，行可写分发器形状或旧图形状 `{"Name","Signature":{"Inputs"}}`） | ➖（只接受 `[]`） | ✅ | Supported | 与 UBlueprint 相同（`blueprint.member.dispatcher.*`）；`AnimBlueprint.EventDispatchers` |
| 仅大小写的改名（变量、事件分发器、函数/图、参数、自定义事件） | 同一条目，名字只差大小写 | ➖ | ✅ | Supported | 与 UBlueprint 相同（`blueprint.member.rename.v1` 等），同一成员、引用保留 |
| 变量改名（不同名字）并保留引用 | — | ➖ | ➖ | Unsupported | 文档按名字识别；改名 = 删除 + 新增 |

## 2. 图

节点写法见 `graphs/schemas/anim.py` 文档字符串：`Anim:<Kind>`（菜单中的任意动画节点）、
`Anim:SaveCachedPose:<Cache>`、`Anim:UseCachedPose:<Cache>`、`Anim:LinkedAnimLayer:<Layer>`、
`SM:State` / `SM:Conduit` / `SM:StateAlias` / `SM:Transition`；蓝图节点仍为 `K2:...`。
`Anim:BlendSpaceGraph:<BlendSpace|BlendSpace1D>`、`Anim:RotationOffsetBlendSpaceGraph:<AimOffset...>`、`Anim:LinkedInputPose`。
保留 id：`Output`（输出姿势 / 状态结果 / 自定义混合结果 / 采样结果）、`Entry`（状态机入口）、`Result`（转换 / conduit 规则结果）、
`SourcePose` / `DestinationPose`（自定义混合图的两个姿势求值节点）。
节点拥有的子图写在节点的 `Graph` 中（嵌套），转换可用 `From` / `To` 简写；转换的自定义混合图写在 `BlendGraph`，
Blend Space Graph 的采样与采样图写在 `Samples[{Name, X, Y?, Graph?}]`。

| 编辑操作 | Body 位置 | 旧 C++ | Python | 分类 | 机制 / 说明 |
|---|---|---|---|---|---|
| 事件图 / 函数 / 宏 | `UbergraphPages` / `FunctionGraphs` / `MacroGraphs` | ✅ | ✅ | Supported | 与 UBlueprint 相同（K2 适配器） |
| AnimGraph：放置任意动画节点、连线、pin 默认值 | `AnimGraph {Nodes, Links}` | ✅（反射 Class/Fields） | ✅ | Supported | Anim 适配器：节点按菜单 spawner 放置 |
| 动画节点 Details（节点结构体字段，如 Sequence、PlayRate、BlendTime） | `Properties["Node.<Field>"]` | ✅ | ✅ | Supported | `graph.node.set_setting`；只输出非默认值 |
| Details 中“作为 pin 暴露”（Show Pin） | `Properties["ShowPin.<Field>"]` | ✅（反射字段） | ✅ | Supported | 与 Details 勾选相同，pin 重建 |
| 属性绑定（Details → Bind，绑定到变量 / 属性路径） | `Properties.Bindings {Pin: {Path, OnlyUpdateWhenActive}}` | 部分（反射字段） | ✅ | Supported | 原始编辑 `AssetDocumentAnimGraphNodeBindingRawEdits.cpp`（与绑定控件相同：暴露 pin、断开连线、编译后生效），行为测试 `AnimBlueprint.Adapters.NodeBindingRawEdit` |
| 函数类型绑定（绑定到纯函数） | `Properties.Bindings {Pin: {Path: "<函数名>"}}` | ➖ | ✅ | Supported | 路径末段为函数时绑定类型为 Function（与绑定控件 `OnAddBinding` 相同）；须为无参数、返回值兼容的纯函数（`OnCanBindFunction`），否则 `Graph.InvalidArgument`；同一 apply 新建的函数可直接绑定；E2E `Graphs.FunctionBinding` |
| 状态机：新增 / 删除 / 改名 | `Anim:StateMachine` 节点 + `Name` + `Graph` | ✅（`Body.StateMachines`） | ✅ | Supported | 状态机图嵌套在 AnimGraph / layer 中；旧 `StateMachines` key 被拒并提示新位置 |
| 状态：新增 / 删除、状态姿势图、状态 Details（事件通知、Always Reset On Entry） | `SM:State` + `Graph` + `Properties` | ✅ | ✅ | Supported | |
| 入口状态 | `Entry` 的连线 | ✅ | ✅ | Supported | |
| 转换：新增 / 删除、规则图、Details（CrossfadeDuration、PriorityOrder、BlendMode、自动规则等） | `SM:Transition` + `From`/`To` + `Graph` + `Properties` | 部分（规则图不支持自定义节点） | ✅ | Supported | 同一对状态间可有多条转换（编辑器允许）。身份：Extract 写出的 `NodeGuid`；手写文档（无 `NodeGuid`）按 `From`/`To` + 序号与现有转换一一对应（第 n 条对第 n 条，`SchemaAdapter.matches_in_order`），子图名按 `A->B #2` 编号。手写 / Extract 重放为 no-op；改第二条只改它；用 Extract（带 `NodeGuid`）删掉第一条时保留第二条的节点。E2E `StateMachine.DuplicateTransitions` |
| Conduit（含规则图） | `SM:Conduit` + `Graph` | ➖（`UnsupportedStateMachineNodeClass`） | ✅ | Supported | |
| State Alias（别名覆盖的状态、Global Alias） | `SM:StateAlias` + `Properties.AliasedStates` / `bGlobalAlias` | ➖ | ✅ | Supported | |
| 自定义混合图（转换 Details → Blend Logic = Custom） | `SM:Transition` + `BlendGraph {Nodes, Links}` | ➖ | ✅ | Supported | `BlendGraph` 隐含 `LogicType = TLT_Custom`（编辑器 PostEditChangeProperty 建图）；改回 `TLT_StandardBlend` 删除该图；E2E `Graphs.CustomTransitionBlendGraph` |
| Blend Space Graph（ABP 内嵌 blend space）：轴 / 插值设置、采样、采样姿势图 | `Anim:BlendSpaceGraph:<Class>` + `Properties["BlendSpace.*"]` + `Samples` | ➖ | ✅ | Supported | 原始编辑 `AssetDocumentAnimBlendSpaceGraphRawEdits.cpp`（Blend Space Graph 页签的 Details 与采样增 / 删 / 移，含采样图），行为测试 `AnimBlueprint.Adapters.BlendSpaceGraphRawEdit`；采样按名字识别；E2E `Graphs.BlendSpaceGraph` |
| Layer 输入姿势（layer Details → Inputs “+”） | `AnimLayers[].Nodes[{Action: "Anim:LinkedInputPose", Properties: {"Node.Name"}}]` | ➖ | ✅ | Supported | 与 `FAnimGraphDetails::OnAddNewInputPoseClicked` 相同（随后重建 self layer 节点）；按姿势名识别 |
| Layer Group（layer Details → Group） | `AnimLayers[].Nodes[{Id: "Output", Properties: {"LayerGroup"}}]` | ➖ | ✅ | Supported | `FBlueprintEditorUtils::SetAnimationGraphLayerGroup`；`""` = 不分组，默认组不输出 |
| Cached Pose：Save / Use | `Anim:SaveCachedPose:<C>` / `Anim:UseCachedPose:<C>` | ✅（反射 Class/Fields） | ✅ | Supported | Use 节点按缓存名引用 Save 节点 |
| 自有 Anim Layer：新增 / 删除 / 图体 | `AnimLayers: [{Name, Nodes, Links}]` | ✅ | ✅ | Supported | host 适配器 `anim_layer` 角色；先于 AnimGraph 应用并编译（Linked Anim Layer 节点按编译后的签名解析，与编辑器一致） |
| 实现 ALI 后填写其 layer | `ImplementedInterfaces` + `AnimLayers[{Name: 接口 layer 名}]` | ✅ | ✅ | Supported | 接口 layer 图也是 `anim_layer` 角色 |
| Linked Anim Layer 节点（自有 layer 或接口 layer） | `Anim:LinkedAnimLayer:<Layer>` | ✅（有签名 bug） | ✅ | Supported | 修复 `IncompatibleLinkedAnimLayerSignature`（旧路径用未编译 class 校验） |
| Linked Anim Graph 节点 | `Anim:LinkedAnimGraph` + `Node.InstanceClass` | ✅ | ✅ | Supported | |
| 节点位置、注释、NodeGuid | `Position` / `Comment` / `NodeGuid` | ✅ | ✅ | Supported | 与 K2 相同 |
| 读取（Extract / Inspect / Diff） | 全部上述 key | ✅ | ✅ | Supported | extract → apply → extract 为空操作 |

**统计：** Supported 39 行，Read-only 0 行，Unsupported 5 行
（改 Template、Sync Group 颜色、组件、Timeline、变量改名）。

## 3. 省略语义

| Body key | 旧 C++ | Python |
|---|---|---|
| 省略 `Variables` / `ImplementedInterfaces` / `ClassDefaults` / `UbergraphPages` / `FunctionGraphs` / `MacroGraphs` | 按区域适配器各自处理 | **保留现状**（与 UBlueprint Python 相同） |
| 省略 `AnimGraph` / `AnimLayers` | 按区域适配器各自处理 | 保留现状 |
| 省略 `Preview` / `SyncGroups` / `ParentAssetOverrides` / `Thumbnail` / `TargetSkeleton` | 按区域适配器各自处理 | 保留现状 |
| 给出某个 key | 该区域完整期望状态 | 同左：未列出的 layer / 节点 / 状态被删除；`Preview` 未列出的字段恢复默认 |
| `Thumbnail: null` | — | 保持现状（编辑器无法删除缩略图信息） |
| 顶层 `Properties` | 合并 | 合并：只写列出的属性 |

写 `[]` / `{}` 仍可删除全部，能力不减少。

## 4. 回滚与派生状态

- 已存在目标：Core 整包快照（`FAssetDocumentPackageSnapshot`）+ Blueprint 派生状态重建器
  （`AssetDocumentGraphBlueprintRebuilder.cpp`，`Supports` 覆盖 `UAnimBlueprint`；编译时重建的瞬态
  `<Graph>__AnimFunc` 桩图等 transient 对象归为派生状态），满足 §3 功能等价。
- 快照恢复时，新建的图节点先断开与存活节点的 pin 连线再移除（否则存活节点保留指向被回收 pin 的指针，
  GC 时崩溃）；对所有图资产生效，测试 `PackageSnapshot.UnlinksNewGraphNodesFromSurvivingNodes`。
- 编译产物也归为派生状态：编译器复制到生成类下的内嵌 blend space（`<ABP>_C:BlendSpace_0`）与
  `UAnimBlueprintExtension`（每次编译由 `RefreshExtensions` 按节点增删），否则回滚后的对象集校验失败；由
  `AssetDocumentAnimation` 注册的 `IAssetDocumentBlueprintRebuildAdapter`（`AssetDocumentAnimBlueprintHostAdapter.cpp`）声明。
- 新建目标：factory 回滚 seed 删除新建资产，不留 package / registry 条目。
- 编译失败 → `Graph.FinalizeFailed`，整包回滚；E2E `AnimBlueprint.Python` 覆盖中途失败、编译失败、
  卸载重载、创建失败不留残留、撤销历史。
- 编辑器打开时 apply：同 UBlueprint（原地编辑 + Blueprint open-editor handler 关闭已删除图的标签页并 `RefreshEditors`）；Persona 预览实例运行重新编译后的类。测试 `AnimBlueprint.OpenEditorApply`。

## 5. 未做与原因

- **Anim Layer Interface 的编译失败回滚**：ALI 不能含 K2 图，文档无法构造编译失败；中途失败回滚、撤销与重载已覆盖。
- **Timeline**：编辑器不为 Animation Blueprint 提供（`DoesSupportTimelines` 要求 Actor 系）。
- **变量 / 分发器改名保引用**：与 UBlueprint 矩阵相同。

## 6. 已知限制（Diff）

- 与 UBlueprint 相同的严格 / 宽松两种模式；严格 Diff 中显式写出等于默认值的节点设置会报差异（保守），
  例外：`Bindings: {}` 与转换的 `LogicType: "TLT_StandardBlend"`（`anim_blueprint_runtime.KNOWN_DEFAULTS`）。
- Blend Space 的 `GridNum` / `Min` / `Max` 改动可能使编辑器吸附或移动已有采样（与 Details 相同）；同时写出 `Samples`
  即按文档位置重放。
- 节点设置中的浮点按 5 位小数比较（`0.4` 与读回的 float32 `0.4000000059604645` 视为相等）。
- `FAnimNotifyEvent.Guid`（状态 / 转换通知）不作为设置比较，回滚 / 重载后重新生成不算差异。
- 同一对状态间的重复转换按出现顺序编号（`A->B #2`，Extract 与 Body 解码一致），重排可能使编号对应到不同转换；
  有 `NodeGuid` 时按 GUID 识别。
