# 独立动画资产支持矩阵（Skeleton、BlendSpace 系、PoseAsset、AnimComposite）

上位规则：[设计规则](asset-document-design-rules.md)
§3（回滚 = 功能等价）、§4（编辑器等价的原始编辑放在独立文件并有行为测试）、§5（支持矩阵、能力不倒退）。

这些类原来走通用反射路由（只有 `Properties`；骨骼没有槽位、Blend Space 没有轴和样本可写）。现在由 Python
Profile（`Content/Python/asset_document/profiles/{skeleton,blend_space,pose_asset,anim_composite}.py`，共享
`anim_asset_common.py`）在路由 `animation_asset_profile` 上承担读 / 新建 / 修改，原子操作来自新 provider
`animation_asset`（`Source/AssetDocumentAnimation/Private/AnimAssets/`）：

| 原子操作 | 作用 |
|---|---|
| `animation_asset.skeleton.read.v1` / `.edit.v1` | 槽位组 / 槽位、socket、虚拟骨骼、blend profile、平移重定向模式、曲线元数据 |
| `animation_asset.blend_space.read.v1` / `.edit.v1` | 轴参数、样本（增删改）、预览网格、Details 修改后的重新验证 / 重采样 |
| `animation_asset.pose_asset.read.v1` / `.edit.v1` | 姿势改名 / 删除、加性 + 基础姿势、Update Source、预览网格 |
| `animation_asset.composite.read.v1` / `.edit.v1` | 片段轨道（按位置整体替换）、预览网格 |

每次 edit 调用在一个 `FScopedTransaction` 里按编辑器的顺序调用编辑器 API（编辑器自身的事务嵌套其中）；回滚由 Core 的
包快照（已有资产）或工厂回滚（新建资产）承担，preflight 返回 `SNAPSHOT_COVERED`。其余 Details 设置走通用
`Properties` 区域（`common.PropertiesRegion`），Body 拥有的属性在 `Properties` 中被拒绝并指出 Body 位置。
新建时的上下文资产（New Asset 对话框选的骨骼 / 源动画）通过 `factory_context` 句柄交给通用工厂生命周期，
与编辑器对话框设置的工厂字段相同（`UBlendSpaceFactoryNew::TargetSkeleton`、`UAnimCompositeFactory::TargetSkeleton`、
`UPoseAssetFactory::SourceAnimation`）。

分类：**Supported**（Extract + Apply + Diff）、**Read-only**（只 Extract / Inspect，写入不同值返回稳定 diagnostic）、
**Unsupported**（不在文档中；给出原因）。E2E：`UEAssetDoc.AnimationAssets.*`
（`Source/AssetDocumentAnimation/Private/Tests/AssetDocumentAnimationAssetsTests.cpp`），Python 单测
`tests/test_animation_assets.py`。

## 1. Skeleton（`/Script/Engine.Skeleton`，`profile.skeleton.v1`）

| 编辑操作 | Body 位置 | 分类 | 机制 / 说明 |
|---|---|---|---|
| 新建 Skeleton | `Action: Create` | Unsupported | 编辑器没有"新建空骨骼"：骨骼随 Skeletal Mesh 导入产生。`Skeleton.CreateUnsupported`（at `/Target`），不留残留 |
| 骨骼层级 | — | Read-only | 导入数据；Inspect 的 Skeleton facts（p6-mcp 的 discovery）列出骨骼 / 槽位 / socket，`SkeletonDetails.BoneCount` |
| 槽位组 / 槽位：增、删、改名、移组（Anim Slot Manager） | `SlotGroups[{Name, Slots[name 或 {Name, RenamedFrom}]}]` | Supported | `IEditableSkeleton::AddSlotGroupName / SetSlotGroupName / RenameSlotName / DeleteSlotName / DeleteSlotGroup`；`DefaultGroup` 必须保留；被 Montage 或 ABP Slot 节点使用的槽位不可改名 / 删除（`Skeleton.SlotInUse` 列出使用者；与 `SSkeletonSlotNames` 相同的资产注册表查询） |
| Socket：增、删、改名、骨骼、相对变换、Force Always Animated | `Sockets[]` | Supported | `IEditableSkeleton::AddSocket / RenameSocket / SetSocketParent`；删除与 Details 写入是编辑器的原始编辑（`AssetDocumentSkeletonRawEdits.cpp`：Modify + 移除 + 卸下预览附件；PreEditChange / PostEditChangeProperty）；未知骨骼 → `Skeleton.BoneNotFound` |
| 虚拟骨骼：增、删、改名 | `VirtualBones[]` | Supported | `USkeleton::AddNewNamedVirtualBone / RemoveVirtualBones`、`IEditableSkeleton::RenameVirtualBone`；名字须以 `VB ` 开头；改 Source / Target = 替换 |
| Blend Profile：增、删、改名、模式、骨骼缩放 | `BlendProfiles[]` | Supported | `IEditableSkeleton::CreateNewBlendProfile / RemoveBlendProfile / RenameBlendProfile / SetBlendProfileMode / SetBlendProfileScale`（设为默认值即移除条目，与编辑器相同） |
| 平移重定向模式（每骨骼） | `BoneRetargeting{bone: mode}` | Supported | `IEditableSkeleton::SetBoneTranslationRetargetingMode`；未列出 = `Animation` |
| 曲线元数据：增、删、改名、Material / MorphTarget 标志 | `Curves[]` | Supported | `USkeleton` 曲线元数据 API（Anim Curves 面板） |
| 曲线元数据的骨骼链接（LinkedBones） | — | Unsupported | 罕用；不在文档中 |
| Compatible Skeletons、Preview Forward Axis 等 Details | 顶层 `Properties` | Supported | 通用 Properties 区域 |
| 资产在 Skeleton 编辑器中打开时 apply | — | Supported | 原地修改后刷新骨骼树（`ISkeletonTree::Refresh`）和预览；测试 `AnimationAssets.OpenEditorApply.Skeleton` |
| 骨骼重定向源（Retarget Sources） | — | Unsupported | 需要网格姿势捕获（交互式），罕用 |

## 2. BlendSpace / BlendSpace1D / AimOffsetBlendSpace / AimOffsetBlendSpace1D

| 编辑操作 | Body 位置 | 分类 | 机制 / 说明 |
|---|---|---|---|
| 新建（选骨骼） | `Skeleton` + `Action: Create` | Supported | `factory_context` → `TargetSkeleton`；缺少 → `BlendSpace.SkeletonRequired`；一次 apply 新建并填充 |
| 改骨骼 | `Skeleton` | Read-only | 编辑器不提供：`BlendSpace.SkeletonImmutable` |
| 预览网格 | `PreviewMesh` | Supported | `UAnimationAsset::SetPreviewMesh`；骨骼不兼容 → `AnimationAsset.IncompatibleSkeleton` |
| 轴：名称、Min / Max、网格划分、吸附、Wrap Input | `Axes{X, Y}`（1D 只有 X） | Supported | 与 Details 面板相同的 PreEdit / PostEdit；吸附开启时引擎在 PostEditChange 中按旧网格重算样本，因此改范围时先关吸附、写 Min / Max / 网格、再恢复（等价于编辑器逐项编辑的结果） |
| 样本：增、删、移动、换动画、Rate Scale、Single Frame、Analyse All | `Samples[]`（按位置识别） | Supported | `UBlendSpace::AddSample / DeleteSample / ReplaceSampleAnimation`，随后 `ResampleData / ValidateSampleData`（编辑器的拖放流程）；越界 → `BlendSpace.SampleOutOfRange`（AddSample 会扩轴，因此先检查）；所有轴吸附时不在网格点 → `BlendSpace.SampleOffGrid`；Aim Offset 只接受旋转偏移加性动画（`BlendSpace.IncompatibleAnimation`） |
| 插值 / 平滑、目标权重插值速度、通知触发模式等 Details | 顶层 `Properties` | Supported | 通用 Properties 区域；写入后执行编辑器的 `NotifyPostChange`（验证 + 重采样）。Details 写入与 ABP Blend Space Graph 节点共用一份实现（`AssetDocumentBlendSpaceEdits.cpp`） |
| 资产在 Blend Space 编辑器中打开时 apply | — | Supported | 原地修改后重建文档页签并刷新预览；测试 `AnimationAssets.OpenEditorApply.BlendSpace` |
| 分析（Analysis）属性 | 顶层 `Properties` | 部分 | 只能写通用 Properties 区域接受的部分；分析函数对象未单独建模 |

## 3. PoseAsset（`profile.pose_asset.v1`）

| 编辑操作 | Body 位置 | 分类 | 机制 / 说明 |
|---|---|---|---|
| 新建（Create Pose Asset：从动画序列每帧一个姿势） | `SourceAnimation` + `Action: Create` | Supported | `factory_context` → `UPoseAssetFactory::SourceAnimation`；缺少 → `PoseAsset.SourceAnimationRequired` |
| 姿势改名 / 删除 | `Poses[name 或 {Name, RenamedFrom}]` | Supported | `UPoseAsset::ModifyPoseName / DeletePoses` |
| 新增 / 重排姿势 | `Poses` | Unsupported | 编辑器不提供（姿势来自源动画 / 网格捕获）：`PoseAsset.PoseAddUnsupported`（列出现有姿势）、`PoseAsset.PoseOrderUnsupported` |
| 加性 + 基础姿势（Details 的 Apply） | `Additive` + `BasePose` | Supported | `UPoseAsset::ConvertSpace`；基础姿势按索引保存，改名后跟随 |
| 更换源动画（Update Source） | `SourceAnimation` | Supported | 设置 `SourceAnimation`（PreEdit / PostEdit）+ `UpdatePoseFromAnimation` |
| 骨骼 | `Skeleton` | Read-only | 取自源动画：`PoseAsset.SkeletonImmutable` |
| 姿势曲线 / 骨骼轨道数据 | — | Read-only | Inspect `PoseAssetDetails` |
| Retarget Source 等 Details | 顶层 `Properties` | Supported | 通用 Properties 区域 |
| 资产在 Pose Asset 编辑器中打开时 apply | — | Supported | 原地修改后重建文档页签（姿势列表）并刷新预览；测试 `AnimationAssets.OpenEditorApply.PoseAsset` |

## 4. AnimComposite（`profile.anim_composite.v1`）

| 编辑操作 | Body 位置 | 分类 | 机制 / 说明 |
|---|---|---|---|
| 新建（选骨骼） | `Skeleton` + `Action: Create` | Supported | `factory_context` → `UAnimCompositeFactory::TargetSkeleton` |
| 片段：增、删、重排、换动画、起止时间、播放速率、循环次数 | `Segments[]`（按位置） | Supported | 与编辑器相同：`FAnimSegment::SetAnimReference` + `FAnimTrack::IsValidToAdd`，赋值后 `CollapseAnimSegments / SortAnimSegments / UpdateCommonTargetFrameRate / InvalidateRecursiveAsset / SetCompositeLength`；递归引用 → `AnimComposite.IncompatibleAnimation` |
| 片段在时间轴上的位置 | — | Read-only | 片段首尾相接（编辑器 collapse）；Inspect `CompositeDetails` |
| 预览网格 | `PreviewMesh` | Supported | 同 BlendSpace |
| 通知 / 曲线 | — | Unsupported | 现有 AnimSequence 的通知 / 曲线原子操作只接受 AnimSequence；Composite 上很少使用 |
| 其它 Details | 顶层 `Properties` | Supported | 通用 Properties 区域 |
| 资产在动画编辑器中打开时 apply | — | Supported | 与 Montage / Sequence 相同的处理（原地修改后重建文档页签、刷新预览）；测试 `AnimationAssets.OpenEditorApply.AnimComposite` |

## 5. AnimMontage 与骨骼槽位

- 引擎新建的 Montage 带一条 `DefaultSlot` 轨道（`UAnimMontage` 构造函数）；骨骼没有对应槽位组时，"只给 Skeleton"的新建在设置骨骼时会被 `IncompatibleReference` 拒绝。
- Montage profile 在 preflight 通过 `animation_asset.skeleton.read.v1` 读取骨骼槽位，缺失时报
  `AnimMontage.UnknownSlot`，路径为 `/Body/SlotAnimTracks/<i>/SlotName`（未写轨道的新建：`/Body/SlotAnimTracks`；
  改骨骼：`/Body/Skeleton`），消息给出骨骼路径和它的槽位列表，并指向 Skeleton 文档的 `Body.SlotGroups`。C++ 侧
  `AnimationProvider.Montage.InvalidSlot` / `AnimationProvider.Skeleton.IncompatibleReference` 的消息指出
  骨骼、缺少的槽位或不兼容的动画。
- 测试 `AnimationAssets.MontageSlot`：骨骼文档添加 `DefaultSlot` / `UpperBody`（并在 `Properties` 写
  `CompatibleSkeletons`）→ 只给 Skeleton 新建 Montage 成功（得到 `DefaultSlot` 轨道）→ 用引擎教程动画新建
  `UpperBody` 轨道的 Montage 成功（同一文档写 `CompositeSections`）→ 骨骼文档删除该槽位被 `Skeleton.SlotInUse` 拒绝。
- Montage 结构原语按编辑器行为放行：
  - 新建（从未有通知轨道）的 Montage 改变长度时，引擎会建默认通知轨道 "1"（`UAnimSequenceBase::InitializeNotifyTrack`，
    与 Montage 编辑器和每次加载相同），这不算改动未拥有的通知轨道。
  - 没有 `SyncGroup` 时，`SyncSlotIndex` 所指轨道的槽位可以改变（编辑器的槽位下拉框同样可改）；有 `SyncGroup` 时拒绝。
  - 保留的 section / notify 所在槽位被替换时拒绝，消息指出槽位并说明在同一文档写 `CompositeSections`。
- Diff：经 Diff 请求传入的文档里整数可能是浮点（`LoopingCount: 1.0`）；Montage 的 Diff 把整数值浮点视为整数。
- 不做：编辑器的 Montage 面板在打开时会把 Montage 用到的槽位自动注册到骨骼（`RegisterSlotNode`）。AssetDocument
  不在 Montage apply 中顺带修改另一个资产（骨骼），而是给出明确诊断，由骨骼文档显式添加。
