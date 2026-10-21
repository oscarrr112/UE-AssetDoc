# AssetDocument 通用图原子操作（Graph primitives v1）

**状态：** Phase 1 冻结接口（2026-10-03）。C++ provider（`Source/AssetDocumentGraph`）与 Python 图模型（`Content/Python/asset_document/graphs/**`）都以本文为准。修改本文需要 Root 同意，并同时更新两侧与 `Content/Python/asset_document/tests/fixtures/graph_dto/*.json`。

上位规则：[设计规则](asset-document-design-rules.md)。回滚由 Core 统一提供（整包快照 + 派生状态重建），本文的原子操作**不写 typed inverse**。

## 1. 模块与 provider

- 新模块 `Source/AssetDocumentGraph`（`PostDefault`），provider id `graph`；依赖 AssetDocument、AssetDocumentEditor（如需复用 handle/codec）、UnrealEd、Engine、BlueprintGraph、Kismet、GraphEditor。
- 不同图类型的差异放在适配器里（§5），不按资产类型新增原子操作。
- 所有写操作的 mutation footprint 统一声明为目标资产所在整包。

## 2. 调用约定

Python 侧：

```python
context.invoke(requirement=<graph.xxx.v1>,
               value_arguments={...},
               handle_arguments=(NamedOpaqueHandleArgument("asset", asset_handle),))
```

- 只传一个 opaque handle：`asset`（ReadOnly 用于查询，Mutable 用于写）。图、节点、pin 一律按值寻址。
- 所有值 DTO 带 `"schema_version": 1`：输入可省略（给出时必须为 1，否则 `Graph.UnsupportedSchemaVersion`）；**所有结果值**（含嵌套的 GraphSnapshot）都带 `"schema_version": 1`。未知字段报 `Graph.UnknownField`（context `allowed_fields`）。
- GUID 一律 32 位**大写**十六进制、无连字符；输入大小写不敏感。

寻址：

| 对象 | 写法 |
|---|---|
| 图 | `{"graph_guid": "..."}` 或 `{"graph_path": "AddHealth"}`。`path` = 从顶层图起按 `/` 拼接的图名，**不带** Body 分区前缀（`EventGraph`、`AddHealth`、`AnimGraph/Locomotion/Idle`）；同名歧义由 `role` 区分 |
| 节点 | `"node_guid": "..."` |
| pin | `{"node_guid": "...", "pin_name": "then", "direction": "output"}`（`direction` ∈ `input`/`output`，即该 pin 在其节点上的方向） |

PinId 不作为身份使用（reconstruct 后会变）。

## 3. 原子操作清单

| primitive | handle | value_arguments → result values |
|---|---|---|
| `graph.list.v1` | asset(RO) | `{offset?, limit?}` → `{graphs:[{graph_guid,name,path,role,schema_class,schema_tag,parent_graph_guid?,owner_node_guid?,caps:{spawn,rename,remove}}], total?, next_offset?}`（无 `offset`：全部图，超出 runtime 值预算时 `Graph.ResultTooLarge`；有 `offset`：按预算切成整条目页，`next_offset` 为 null 即末页。Python `list_graphs` 先整取，被拒再分页） |
| `graph.query.v1` | asset(RO) | `{graphs?:[addr], include_properties?:bool, node_offset?:0, node_limit?}` → `{graphs:[GraphSnapshot], truncated}`（§4；结果受 runtime 值预算约束，超出时按节点分页：`next_node_offset` 非 null 时用它续查；每个所请求的图都带表头，数百个图一次请求会只因表头超预算，Python `query_graphs` 对全部图按 64 个一批请求） |
| `graph.actions.enumerate.v1` | asset(RO) | `{graph:addr, context?:{from_pin?:pin, parent_node_guid?}, filter?:{text?,key_prefix?,keys?:[...]}, limit?:200（≤400）}` → `{actions:[{action_key,display_name,category,tooltip,node_class,requires_context,stable?}], unresolved:[{key,suggestions:[...5]}], total, truncated}`（按 action_key 排序；`stable:false` 仅出现在兜底 key 上） |
| `graph.node.spawn.v1` | asset(M) | `{graph:addr, action_key, position?:{x,y}, context?:{from_pin?,parent_node_guid?}}` → `{node_guid, created_node_guids:[], created_graph_guids:[], reused, node:NodeSnapshot}`（`reused=true`：复用了已有/ghost 事件节点） |
| `graph.node.remove.v1` | asset(M) | `{node_guid}` → `{removed_node_guids:[], removed_graph_guids:[]}` |
| `graph.pin.connect.v1` | asset(M) | `{from:pin, to:pin, allow_conversion_node?:false, keep_from_types?:{pin:PinType}, keep_to_types?:{pin:PinType}}` → `{response, created_node_guids:[], broken_links:[Link], reconstructed_node_guids:[]}`（`make_with_promotion`（类型提升运算符的操作数随连线提升类型，如 vector × float）与编辑器拖线一样直接连接；只有需要插入转换节点的 `make_with_conversion_node` 要求 `allow_conversion_node`，否则 `Graph.ConnectNeedsConversion`；`keep_from_types`/`keep_to_types`：该端节点由调用方陈述的引脚类型；该端是类型提升运算符时，连线使其重选函数并改写引脚类型后，只要全部连线适配所陈述类型，就恢复这些类型并按编辑器规则（FindBestMatchingFunc）为其选函数（作者的 Pin Conversions 不被连线顺序冲掉）：连到本次连线另一端的连线按该端陈述的类型判断是否适配；类型未变时，先前连线留下的函数不是编辑器为这些类型所选的也改正（否则 vector × vector 展开时 double 连线无法构建）；所选函数须使加载（UpdatePinsFromFunction）保持这些类型与连线，否则取保持它们的第一个同运算符函数（Add 的 int 操作数与 double 结果用 Add (double, double)）；本次连线使该端陈述引脚断开的原有连线，若适配恢复后的类型则重新连上；两端恢复后 from 端再按 to 端的类型重选一次） |
| `graph.pin.break.v1` | asset(M) | `{from:pin, to?:pin}`（无 `to` 时断开该 pin 全部连线）→ `{broken_links:[Link]}` |
| `graph.pin.default.set.v1` | asset(M) | `{pin, kind:"value"\|"object"\|"text"\|"reset", value?}` → `{default_value, default_object, default_text}`（返回 UE 实际存储的规范字符串，即 `TrySetDefaultValue` 之后编辑器保存的值） |
| `graph.node.property.get.v1` | asset(RO) | `{node_guid, container:"node"\|"template"\|<适配器容器>, property_path?}` → `{properties:{...}}`（typed JSON，同 Editor typed value codec） |
| `graph.node.property.set.v1` | asset(M) | `{node_guid, container, property_path, value}` → `{value}`；只开放 `CPF_Edit` 且非 `EditConst` 属性 |
| `graph.node.layout.set.v1` | asset(M) | `{node_guid, position?:{x,y}, comment?, comment_bubble_visible?}` → `{}` |
| `graph.node.rename.v1` | asset(M) | `{node_guid, name}` → `{name}`（`OnRenameNode`；CustomEvent/State/Comment 等；先交给 schema 适配器 `RenameNode`，见 §5 / §10） |
| `graph.node.user_pins.set.v1` | asset(M) | `{node_guid, pins:[{name, direction, type:PinType, default?}]}` → `{node:NodeSnapshot}`（K2 EditablePinBase：Entry/Result/CustomEvent/Tunnel；`direction` = 该 pin 在此节点上的方向：Entry 用户 pin 为 `output`、Result 为 `input`、CustomEvent 参数为 `output`；整表替换，按给定顺序） |
| `graph.node.dynamic_pin.add.v1` / `.remove.v1` | asset(M) | `{node_guid, pin_name?}` → `{node:NodeSnapshot}`（`IK2Node_AddPinInterface`；add 每次加一个 pin，返回更新后的 NodeSnapshot，Python 循环直到目标 pin 出现，最多 32 次；remove 需要 `pin_name`） |
| `graph.node.pin_type.set.v1` | asset(M) | `{node_guid, pin_name, type:PinType}` → `{node:NodeSnapshot}`（节点 pin 上自带的类型选择器：K2 Select 的 Index（`SGraphPinIndex::OnTypeChanged`：设 pin 类型 → `PinTypeChanged` → `ReconstructNode`）。仅 `extras.typed_pins` 列出的 pin，且该 pin 仍是未连线的 wildcard（编辑器只在此时显示选择器），否则 `Graph.PinTypeNotEditable`；类型须为 bool / byte / 枚举（`byte` + 枚举路径）/ int / int64、非容器，否则 `Graph.InvalidArgument`。已选定类型的 Select 不能改类型：换类型 = 新节点） |
| `graph.function.flags.set.v1` | asset(M) | `{graph:addr, flags:{pure?,const?,call_in_editor?,access?:"public"\|"protected"\|"private",category?,tooltip?,keywords?}}` → `{flags}`（部分更新，返回完整 flags；等价于编辑器 Details 面板：`K2Node_FunctionEntry` ExtraFlags 与 MetaData） |
| `graph.create.v1` | asset(M) | `{role, name, parent?:addr}` → `{graph_guid, path}` |
| `graph.remove.v1` | asset(M) | `{graph:addr}` → `{removed_graph_guids:[]}` |
| `graph.rename.v1` | asset(M) | `{graph:addr, name}` → `{path}` |
| `graph.finalize.v1` | asset(M) | `{}` → `{status:"ok"\|"warnings"\|"errors", messages:[{severity,message,node_guid?,pin?}]}`；Repair 阶段调用（Blueprint compile、BT UpdateAsset、ABP compile）。有 error 时不返回 `status:"errors"`，而是以 `Graph.FinalizeFailed` 失败（message 列出 `[node_guid] 错误`，context `messages` 为同结构数组） |

`role` 取值：`ubergraph`、`function`、`macro`、`delegate_signature`、`anim_graph`、`anim_layer`、`state_machine`、`state`、`transition`、`custom_transition`、`blend_space_sample`、`conduit`、`bt_root`、`other`（`anim_layer` / `custom_transition` / `blend_space_sample` 见 §9）。

## 4. 快照结构

```jsonc
// GraphSnapshot
{"graph_guid":"...", "path":"EventGraph", "role":"ubergraph", "schema_tag":"K2",
 "nodes":[NodeSnapshot...],
 "links":[{"from":{"node_guid":"...","pin_name":"then"},"to":{"node_guid":"...","pin_name":"execute"}}]}

// NodeSnapshot
{"node_guid":"...", "class":"/Script/BlueprintGraph.K2Node_IfThenElse",
 "action_key":"K2:IfThenElse",          // 能反推时给出
 "title":"Branch", "implicit_role":null, // "entry"/"result"/"root"/"sm_entry" 等
 "position":{"x":0,"y":0}, "comment":"", "enabled_state":"enabled",   // "enabled"|"disabled"|"development_only"
 "parent_node_guid":null, "subnode_index":null, "subgraph_guids":[],
 "can_add_pin":false, "extras":{},
 "pins":[{"name":"Condition","direction":"input",
          "type":{"category":"bool","sub_category":"","sub_category_object":"","container":"none","is_ref":false,"is_const":false},
          "default_value":"true","default_object":"","default_text":"",
          "is_autogenerated_default":false,"hidden":false,"advanced":false,"orphaned":false,
          "linked_to":[{"node_guid":"...","pin_name":"..."}]}],
 "properties":{"node":{...},"template":{...}}}  // 仅 include_properties=true；无 template 容器时省略 template
```

GraphSnapshot 还带 `schema_version`、`node_count`、`next_node_offset`（分页，未截断为 null）。

`extras`（schema 相关事实，缺省为 `{}`）：

| 键 | 何时出现 | 含义 |
|---|---|---|
| `ghost: true` | 新建资产自带、未被使用的禁用默认事件节点 | 连线、设 pin 默认值、改属性或 spawn 同一事件时按编辑器方式启用（ghost 消失） |
| `name` | 可重命名节点（CustomEvent、Comment、State…） | 作者给的名字（不是 title） |
| `user_pins: [names]` | K2 EditablePinBase（Entry/Result/CustomEvent/Tunnel） | 用户定义 pin 名，按顺序 |
| `signature_flags` | K2 函数 Entry | `{pure, const, call_in_editor, access:"public"\|"protected"\|"private", category, tooltip, keywords}` |
| `typed_pins: [names]` | K2 Select；K2 类型提升运算符 | 类型在 pin 上选择的 pin（Select 的 `Index`；运算符的操作数与结果，右键 “Pin Conversions”）；文档节点 `PinTypes` 只写已选定（非 wildcard）的类型，由 `graph.node.pin_type.set.v1` 在 spawn 之后、属性 / 连线 / 默认值之前按文档顺序设置；live Select 该 pin 已是另一类型时 reconcile 不复用它（重建节点），运算符则原地转换并重新连接它的连线 |
| `promotable: true`、`wired_to_operator` | K2 类型提升运算符 | 操作数 / 结果类型不全相同，或与另一个运算符相连（两个 wildcard 运算符的类型取决于连线顺序）时 Extract 才写 `PinTypes`；其余同类型的由连线得到 |
| （连线执行） | 所有图 | 连到 wildcard pin 或类型提升运算符的连线之后重读快照（pin 类型随连线确定）；连到 wildcard pin 的拆分成员（`Array Element_StartMs`）的连线等到之后为该 pin 定型的连线完成再连 |
| `properties` / `property_defaults` | K2 Variable Get（bool / object / class 变量且图允许 exec）、可切换纯度的函数调用 | 适配器自管的节点设置 `{"Pure": bool}` 与其默认值；文档 `Properties.Pure` 只在不同于默认时写出，经 `SetNodeSetting` 设置 |
| `local_variables` | K2 函数 Entry（支持局部变量的图） | `[{name, type:PinType（map 带 `value_type`）, default, category（默认分类为空串）, tooltip}]`，My Blueprint 的局部变量，按列表顺序 |

`links` 每条连线只出现一次，方向固定为 output → input。

## 5. 适配器（C++ 公共头 `Source/AssetDocumentGraph/Public/AssetDocumentGraphAdapters.h`）

- `IAssetDocumentGraphSchemaAdapter`（按 `UEdGraphSchema` 子类匹配，最深继承者优先）：`Tag()`、`EnumerateActions`、`ResolveAndSpawn(action_key)`、`DescribeNode`（反推 action_key、implicit_role、extras）、`ResolvePropertyContainer(node, "template")`、`RemoveNode`，可选 `SetUserPins`、`AddDynamicPin`/`RemoveDynamicPin`、`SetPinType`（`graph.node.pin_type.set.v1`，默认 `Graph.PinTypeNotEditable`）。
- `IAssetDocumentGraphHostAdapter`（按资产类匹配）：`EnumerateGraphs`、`CreateGraph`、`RemoveGraph`、`RenameGraph`、`Finalize`；`Finalize` 同时注册为 Core 的派生状态重建钩子。
- Blueprint host 的 `EnumerateGraphs` 按编辑器实现接口时创建的顺序（`ImplementNewInterface`：接口函数的迭代顺序）报告接口图：存储顺序还带着接口的修改历史（接口后加的函数被 conform 追加到末尾），而编辑器不能调整接口图的顺序（`MoveGraphBeforeOtherGraph` 只移动函数图与宏图）。
- 注册：各模块 `StartupModule` 调 `RegisterAssetDocumentGraphSchemaAdapter` / `RegisterAssetDocumentGraphHostAdapter`。
- 本阶段实现：K2 schema、Blueprint host、通用 fallback（`GetGraphContextActions` + `FEdGraphSchemaAction_NewNode`）；BT（AssetDocumentAI）；AnimGraph / 状态机 / ABP host / ABP 设置（AssetDocumentAnimation，见 §9）。
- 可选 `SetNodeSetting(asset, node, path, value, bOutHandled, error)`（schema 适配器）：`graph.node.property.set.v1`（container `node`）先交给适配器；不是普通反射属性的节点设置（如 state alias 指向的状态、动画节点属性绑定）在此处理并置 `bOutHandled`，否则走反射属性路径。默认不处理。K2：`Pure`（Variable Get 的 “Convert to Branch / Validated Get / pure Get”，函数调用的 “Show / Hide Exec pins”；菜单项绑定私有成员，按其函数体执行：Modify → 反射写 `CurrentVariation` / `NodePurityOverride` → `ReconstructNode`）、`LocalVariables`（函数 Entry：`FBlueprintEditorUtils::AddLocalVariable` / `RemoveLocalVariable` / `ChangeLocalVariableType`，默认值 / 分类 / 说明写入变量描述，按列表排序；完成后刷新 action 数据库，使 `K2:VariableGet:<类>:<函数>:<变量>` 可 spawn；Python 计划在签名之后、节点之前以 `graph.local_variables.set` 执行）。
- `IAssetDocumentBlueprintSettingsAdapter`（按 Blueprint 类匹配，最深者优先）：不是可编辑反射属性的类级设置（如 ABP 预览网格）。`blueprint.member.query.v1` 以 `settings` 返回 `QuerySettings`；`blueprint.member.setting.set.v1` `{name, value}` → `{settings}` 调 `SetSetting`；无适配器 → `BlueprintMember.SettingNotFound`。
- `graph.node.property.set.v1` 的 Details 编辑事件为 `PostEditChangeChainProperty`（链 = 顶层成员 + 叶属性，与 Details 面板相同），叶属性专属处理（如 `Node.InstanceClass`）会执行。
- 可选 `GetOwnedSubgraphs(node, out)`（schema 适配器，默认 `UEdGraphNode::GetSubGraphs()`）：`graph.list.v1` 与节点快照 `subgraph_guids` 报告的“节点拥有的图”。适配器可跳过编辑器内部的容器图而报告其中的图（Blend Space Graph 节点报告各采样图，而非其 `UBlendSpaceGraph`）；此时 `path` 含中间容器图名（`AnimGraph/BlendSpace/Idle`），`parent_graph_guid` 仍为 owner 节点所在的图。
- `IAssetDocumentBlueprintRebuildAdapter`（按 Blueprint 类匹配）：让 Graph 的派生状态重建器（`RefreshAllNodes` + 编译 + 还原类默认值）也接管该 Blueprint 类的包（`GetBlueprintClass`），并声明该类编译产生的派生对象（`IsDerivedObject`，如 Widget 生成类下的 widget tree 原型 / `*_INST` 动画）。`IAssetDocumentBoundEventSourceAdapter`：K2 适配器除 `UActorComponent` 外，还为 `GetBoundObjectClass` 的成员对象（Widget Blueprint：`UWidget`）枚举 / 生成 bound event（Details 面板的 “On Clicked [+]”）。`AssetDocumentWidget` 模块在 `StartupModule` 注册这两个适配器，`umg` provider 经 `CreateAssetDocumentGraphTablePrimitiveProvider` 复用 Graph 的调用管线；`AssetDocumentGraph` 不依赖 UMG / MovieScene。
- 可选 `RenameNode(asset, node, name, bOutHandled, error)`（schema 适配器，默认不处理）：`graph.node.rename.v1` 先交给适配器；节点身份不是可编辑标题的 schema（SoundCue：sound node 对象名）在此改名并置 `bOutHandled`，返回的 `name` 取自 `DescribeNode` 的 `extras.name`；否则走编辑器的标题改名（`GetCanRenameNode` / `OnRenameNode`）。
- 公共头导出 `EncodeAssetDocumentGraphPropertyValue` / `DecodeAssetDocumentGraphPropertyValue`（`graph.node.property.*` 的 typed JSON 编解码），供自行报告 / 写入反射值的适配器使用。

## 6. action_key

规范 key（C++ 只认规范 key）：

```
K2:<NodeClass 去掉 K2Node_ 前缀>[:<OwnerPath>][:<FieldName>][@<Binding>]
```

- `K2:CallFunction:/Script/Engine.KismetSystemLibrary:PrintString`
- `K2:Event:/Script/Engine.Actor:ReceiveBeginPlay`
- `K2:IfThenElse`、`K2:CustomEvent`、`K2:ExecutionSequence`
- `K2:DynamicCast:/Script/Engine.Pawn`
- `K2:MacroInstance:/Engine/EditorBlueprintResources/StandardMacros.StandardMacros:ForEachLoop`
- `K2:VariableGet:/Game/BP_Foo.BP_Foo_C:Health`、`K2:VariableSet:...`
- `K2:PromotableOperator:Multiply`（类型提升运算符按运算符名寻址：UE 对每个运算符只注册一个共享 spawner，其 UFunction 取决于模块加载顺序，因此 key 不带 owner/函数名；被提升的具体重载如 `Multiply_DoubleDouble` 不再以 `K2:CallFunction` 出现在菜单中，连线后由节点自行确定类型。类型提升之前的内容里仍有直接调用重载的节点，如 `K2:CallFunction:/Script/Engine.KismetMathLibrary:Less_DoubleDouble`、`K2:CommutativeAssociativeBinaryOperator:/Script/Engine.KismetMathLibrary:Add_DoubleDouble`：这类 key 可 spawn（编辑器关闭 “Enable Type Promotion” 时数据库为该函数注册的 spawner），但不出现在 enumerate 中；交换律运算符函数的普通 `K2:CallFunction` 节点（旧内容，如 StandardMacros 里的 `BooleanAND`、`Add_IntInt`）同样可 spawn）
- `K2:ComponentBoundEvent:/Script/Engine.PrimitiveComponent:OnComponentBeginOverlap@CollisionComp`
- 非 K2：`<Tag>:<Kind>:<TemplateClass>`，如 `BT:Task:/Script/AIModule.BTTask_MoveTo`、`SM:State`。兜底 `<Tag>:Action:<TypeId>:<Menu>` 不保证稳定，enumerate 结果中标 `"stable": false`。
- 结构体/枚举节点（`UBlueprintFieldNodeSpawner`：MakeStruct、BreakStruct、SetFieldsInStruct、EnumLiteral、SwitchEnum、ForEachElementInEnum、GetNumEnumEntries、CastByteToEnum）以字段路径作 owner：`K2:EnumLiteral:/Script/Engine.EMovementMode`、`K2:ForEachElementInEnum:/Script/Engine.EMovementMode`、`K2:BreakStruct:/Game/S_Item.S_Item`（用户结构体与无原生 make/break 函数的原生结构体；有原生函数的是 `K2:CallFunction`）。
- `K2:Composite`：折叠图（“Collapse Nodes”）节点。编辑器没有面板 action，spawn 按 `FBlueprintEditor::CollapseNodes` 放置空的折叠节点（可改名，绑定图名 `CollapseGraph` 唯一化）；节点 `Name` 即图名（改名 = `OnRenameNode` → `RenameGraph`）；其子图的 entry / exit tunnel 的用户 pin 镜像到折叠节点上，K2 的拥有子图因此在父图连线之前处理。
- 仍冲突时追加 `#<FBlueprintNodeSignature GUID 前 8 位>`（签名由 key 主体字符串哈希得到，跨重启稳定；当前主要是旧版输入 InputKey/InputDebugKey/InputAxisKeyEvent、GetSubsystem 系列等每个 FKey/类注册一个普通 spawner 的节点，通过 enumerate 发现）；用无后缀 key spawn 命中多个时报 `Graph.ActionAmbiguous` 并给候选。
- spawn 解析：按 owner 在 `FBlueprintActionDatabase` 直接查候选 → 用 `FBlueprintActionFilter` 和当前上下文过滤 → 恰好一个才执行。不带 `from_pin` 时，上下文菜单滤掉、但编辑器关闭 “Context Sensitive” 后的完整面板仍提供的 action 也可 spawn（例如构造脚本里对组件变量调用 `PrimitiveComponent:CreateDynamicMaterialInstance`：上下文菜单只在从该类引脚拖出时才列出它，目标由连线接上）；带 `from_pin` 时按该引脚的上下文菜单过滤。
- 别名（`K2:Branch` → `K2:IfThenElse`、`K2:PrintString` 等）只在 Python `graphs/actions.py` 中处理。
- `<OwnerPath>`：类写权威（非 skeleton）类路径（转换到 Blueprint 类也写类路径 `K2:DynamicCast:/Game/BP.BP_C`；以 Blueprint 对象路径写的旧 key 仍可 spawn）；Blueprint 宏库写 Blueprint 对象路径；局部变量写 `<GeneratedClass>:<FunctionScope>`。
- 函数调用 / 事件节点的 key 写数据库列出的声明：覆写的函数写最上层被覆写的函数，实现接口的事件写接口函数（内容里对本类覆写或事件桩的 self 引用也归到声明）；`CallParentFunction` 写它调用的父类实现。
- 函数图名为父类（或接口）可覆写函数时，`graph.create.v1` 按 My Blueprint 面板的 Override 菜单创建覆写图（继承签名）。
- 父类或已实现接口的可覆写函数作为事件放入事件图时，即使面板滤掉它（如非编辑器 Blueprint 中编辑器模块接口的事件），也按 My Blueprint 面板的 “Implement event” 放置（`SMyBlueprint::ImplementFunction`）。
- 连线 / 设置默认值指向结构体引脚的成员（`ReturnValue_X`）而该引脚尚未拆分时，先按编辑器 “Split Struct Pin”（`UEdGraphSchema::SplitPin`）拆分。
- owner 为资产（非 `/Script/`）而未加载时（如宏库、其它 Blueprint 类），spawn 会先加载该资产并刷新 action 数据库再解析；enumerate 只列出已加载资产的 action（与编辑器菜单一致）。
- 节点反推 key 的边界：`K2:FunctionEntry` 等不能从菜单创建的节点 `action_key` 为 null（用 `implicit_role` 识别）；`K2:FunctionResult`（“Add Return Node”）可 spawn；函数图可有多个 `result` 节点（文档中第一个是签名自带的，其余由 reconcile 以 `K2:FunctionResult` 放置）。
- key 只依赖 action 本身，不依赖图上下文；`requires_context:true` 表示该 action 存在但当前上下文的编辑器菜单会过滤掉它；不带 `from_pin` 的 spawn 只有在完整面板（Context Sensitive 关闭）也滤掉它时才报 `Graph.ActionRequiresContext`，带 `from_pin` 时该引脚的上下文菜单滤掉它就报。调用编辑器专用函数（编辑器模块的函数，或父类为编辑器专用类的蓝图类的函数）的节点，编辑器不允许放进运行时蓝图的图（面板与粘贴都拒绝：UK2Node_CallFunction::CanPasteHere），报 `Graph.EditorOnlyFunction`。折叠图（collapsed graph）的 outer 不是蓝图，引擎的这项检查在其中总是失败；编辑器可以在顶层图放置后折叠，所以折叠图里以其顶层图（FBlueprintEditorUtils::GetTopLevelGraph）的粘贴检查为准。

## 7. 诊断码

`Graph.UnknownField`、`Graph.InvalidArgument`（类型/取值错误，path 指向字段）、`Graph.UnsupportedSchemaVersion`、`Graph.ResultTooLarge`（超出 runtime 值预算，改用分页/limit）、`Graph.GraphNotFound`、`Graph.NodeNotFound`、`Graph.PinNotFound`、`Graph.ActionNotFound`（附 suggestions）、`Graph.ActionAmbiguous`、`Graph.ActionRequiresContext`、`Graph.EditorOnlyFunction`、`Graph.ConnectDisallowed`（附 schema 给出的原因）、`Graph.ConnectNeedsConversion`、`Graph.PinDefaultInvalid`、`Graph.PropertyNotEditable`、`Graph.NodeNotDeletable`、`Graph.UserPinsUnsupported`、`Graph.DynamicPinUnsupported`、`Graph.PinTypeNotEditable`、`Graph.GraphOperationUnsupported`、`Graph.FinalizeFailed`（messages 带 node_guid）、`Graph.PieActive`。

## 8. 注意事项

- 连线或改属性可能触发 `ReconstructNode`，pin 名可能变化：每次调用都按名字重新解析；结构性操作后 Python 重新 query。
- pin 默认值在连线之后设置（wildcard pin 连线后才确定类型）；类 pin 上选的类（Create Widget / Add Component by Class / Spawn Actor 的 `Class`，决定输出类型）在连线之前也设置一次（编辑器里作者先选类再连线；此时不是类 pin 的则跳过）；`PinTypes` 声明了类型的 pin 上的默认值同样在连线之前设置（类型提升运算符连线时把未定型的附加 pin 归一到函数类型，默认值使其保持已定型）。函数的 `Result` 在文档没有声明输出时：新建图已带返回节点（覆写有输出的父函数）则取用它，否则按 `K2:FunctionResult` 放置。
- 同一次 apply 新建的函数/变量：spawn 引用它们的节点前先 `MarkBlueprintAsStructurallyModified` + `FBlueprintActionDatabase::RefreshAssetActions`。
- PIE 运行中拒绝写操作（`Graph.PieActive`）。
- 新建 Blueprint 自带的 ghost 事件节点应复用，不重复创建。
- split pin 暂不支持。

## 9. AnimBlueprint 增补（P2）

- **模块分层**：AnimGraph / 状态机 / ABP host / 设置适配器在 `AssetDocumentAnimation`，依赖 `AssetDocumentGraph` 的公共适配器注册表；因此 `AssetDocumentGraph` 是其它 provider（AI、Animation、Blueprint、UMG、Widget）之下的一层，反向依赖仍被 `Architecture.ModuleGraph.NoReverseDependencies` 禁止。
- **action_key**：`Anim:<Kind>`（`UAnimGraphNode_<Kind>`，菜单中任意动画节点）、`Anim:SaveCachedPose:<Cache>`、`Anim:UseCachedPose:<Cache>`、`Anim:LinkedAnimLayer:<Layer>`（自有 layer 或已实现接口的 layer）、`Anim:BlendSpaceGraph:<BlendSpaceClass>` / `Anim:RotationOffsetBlendSpaceGraph:<Class>`（菜单的类变体 spawner，按 blend space 类区分）、`Anim:LinkedInputPose`（只在可编辑 layer 图中提供，等同 layer Details 的 Inputs “+”）；状态机图中 `SM:State`、`SM:Conduit`、`SM:StateAlias`、`SM:Transition`。implicit_role：`root`（Output Pose / 状态结果 / 自定义混合结果 / 采样结果）、`sm_entry`、`result`（转换 / conduit 规则结果）、`source_pose` / `destination_pose`（自定义混合图中按 DataSource 的第一个姿势求值节点）。
- **角色**：`anim_layer` = ABP 或 Anim Layer Interface 的 layer 图（host 适配器可 create / remove）；`custom_transition` = 转换的自定义混合图（Body `BlendGraph`，由 `LogicType = TLT_Custom` 产生）；`blend_space_sample` = Blend Space Graph 节点的采样图（Body `Samples[].Graph`，由 `Samples` 设置产生，按采样名识别：`SchemaAdapter.subgraph_identity`）。节点拥有的子图跟随其 owner 节点识别并以 owner 命名（`owned_subgraphs_follow_owner`）；同名兄弟子图在 Extract 中加 ` #n`。
- **节点设置**：`Node.<Field>`（动画节点结构体）、`ShowPin.<Field>`、`Bindings`（`{Pin: {Path, OnlyUpdateWhenActive?}}`，路径末段为纯函数时为函数绑定；原始编辑 `AssetDocumentAnimGraphNodeBindingRawEdits.cpp`）、`AliasedStates`（state alias）、`LayerGroup`（layer 的 Output Pose）、`Samples` 与 `BlendSpace.<Property>` / `BlendSpace.<BlendParameters|InterpolationParam>.<X|Y>.<Field>`（Blend Space Graph，原始编辑 `AssetDocumentAnimBlendSpaceGraphRawEdits.cpp`）；除 `Node.*` 外均经 `SetNodeSetting`。
- **ABP 设置**（`blueprint.member.setting.set.v1`）：`preview_mesh`、`preview_animation_blueprint`、`preview_animation_blueprint_application_method`、`preview_animation_blueprint_tag`、`sync_groups`、`parent_asset_overrides`、`thumbnail`；query 另报 `is_template`、`blueprint_type`。
- **Python 钩子**（`graphs/schemas/__init__.py` `SchemaAdapter`）：`owned_subgraphs_follow_owner`、`node_matches_live`、`spawn_priority`、`live_properties`、`extract_properties`、`subgraph_name`、`subgraph_identity`（一个 owner 拥有同角色多张图时，文档图名对应的 live 图名；reconcile 与 `graph.subgraph.sync` 据此匹配，GraphGuid 也须名称一致）、`link_leads`（同深度中先连的连线：K2 容器函数的 `TargetArray` / `TargetMap` / `TargetSet` 先于元素连线，由容器而非元素确定 wildcard 类型）；默认值保持 K2 / BT 行为不变。
- **create context**：lifecycle 可声明 `optional_create_context_handle_names`（catalog 标为非必需的句柄，如 ABP 模板没有骨骼 `factory_context`）；`registered_runtime` 按“声明顺序的子集且必需项齐全”校验。
- **比较**：`ublueprint_graph_compare` 可传入节点标签函数（ABP 用 `anim_label`）；严格比较中 `@Property` / `@Extra` 的浮点按 5 位小数取整（float32 读回）。
- **快照**：`FAssetDocumentPackageSnapshot` 恢复时，新建的 `UEdGraphNode` 先对每个 pin `BreakAllPinLinks()` 再移除，避免存活节点留下指向已释放 pin 的连线（对所有图资产生效）。

## 10. SoundCue 增补（S0 / S1，2026-10-07，Root / 用户已批准，见 [SoundCue 设计](asset-document-soundcue-design.md) §8）

- **适配器**（`AssetDocumentAudio/Private/SoundCue/`）：schema 适配器 tag `SoundCue`（`USoundCueGraphSchema`，按类路径匹配，不包含私有头）；host 适配器 `USoundCue`，一个图（role `other`，path `SoundCue`，不可 create / rename / remove）；`Finalize` 同时注册为派生状态重建器。
- **action_key**：`SoundCue:Node:<USoundNode 类路径>`（调色板列出的非抽象、未隐藏 / 弃用的类，经调色板 action `FSoundCueGraphSchemaAction_NewNode::PerformAction` 生成，反射构造其 UScriptStruct）、`SoundCue:Comment`（`GetCreateCommentAction`）。未知或调色板不提供的类：`Graph.ActionNotFound`。
- **属性容器 `sound_node`**：`graph.node.property.get/set.v1` 的 container `sound_node` = 图节点的 `SoundNode`（Details 面板编辑的对象，`PostEditChangeChainProperty` 同 Details）；`node` 仍是图节点本身（注释框文字 / 颜色等）。注释框 `Size`（`[w, h]`）是适配器设置（`SetNodeSetting`，同拖动角落的 `ResizeNode`）。
- **extras**：`name`（sound node 或注释节点的对象名 = 文档 key）、`input_pins`（输入 pin 名，按子节点槽位顺序）、`child_limits: {min, max}`（`GetMinChildNodes` / `GetMaxChildNodes`，Quality Level 为项目质量级数）、`reachable`（从 Root 可达）。Root 节点 `implicit_role: "root"`。
- **`graph.node.rename.v1` 的 SoundCue 含义**：改的是身份对象名（sound node / 注释节点对象，Material 先例），不是标题；新节点 spawn 后改名为 key。非法名或重名 → `SoundCue.InvalidKey`。
- **输入 pin 别名 `"#<i>"`**（direction input）：第 *i* 个输入 pin（槽位位置），pin 名随 Add / Delete Input 变化时仍按位置解析（`ResolvePinAlias`）。
- **动态 pin**：`dynamic_pin.add` = `USoundCueGraphNode::AddInputPin`（`CanAddInputPin`），`remove` = `RemoveInputPin`（只在多于最小子节点数时）；`pin_name` 为真实 pin 名。
- **remove**：编辑器的删除在私有 `FSoundCueEditor::DeleteSelectedNodes`，适配器在独立 raw 文件中复刻（`BreakAllNodeLinks`、`FBlueprintEditorUtils::RemoveNode`、`AllNodes.Remove`、编译），行为测试与打开的编辑器的 Delete 对比。
- **finalize**：`CompileSoundNodesFromGraphNodes` + 验证；`Graph.FinalizeFailed` 的 context `messages` 为 `[{severity, message, node_guid, pin}]`。
