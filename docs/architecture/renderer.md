# 架构沉淀:渲染子系统 (renderer + RenderDevice)

> **架构/方案**文档。质量标尺(渲染管线/HDR 相关)看 `docs/northstar/lighting.md`;行号看 `AI_INDEX.md` §1/§12/§13/§14/§19/§20——**但该子系统迭代快于文档,本文以实际读到的源码为准,AI_INDEX 只作路标**。行号随重构漂移,以符号名为准。引擎重心三大件之一。

**规模**:`renderer/` 12k 行 + `platform/` 9.7k 行。

> ⚠️ **兼容路径提示：MVT 道路线场即将废弃。** `TerrainPageStore` 中的路网 SDF/D2 场页及其
> `kGltfRoadFieldTextureSlot`/`Indir` 绑定暂时保留，用于兼容和历史回归；渲染器新增页化平面或新线表示时不得把这组场槽位当作推荐扩展范例。
> 本提示不涉及影像页、高度页、MVT 面/点或 `VectorLine` 几何命令。

---

## 职责边界

- **`renderer/`**:平台无关渲染中间层。核心产物是 `RenderCommand`(一条命令是 backend-neutral 的"胖"绘制描述符)、`Renderer`(拥有 shader/占位纹理/地形页存储等共享 GPU 资源)、`RenderDevice`(抽象接口)。收纳不依赖具体图形 API 但渲染语义强相关的子系统:地形页存储 `TerrainPageStore`、深度 prepass `TerrainDepthPrepass`、地形实例合批 `TerrainInstanceBatcher`、离屏后处理 `OffscreenPostProcess`、字形/图标图集 `GlyphAtlas`/`IconAtlas`、以及两条**单一事实来源**契约头 `DepthConvention.h`/`BackendWindingContract.h`。
- **`platform/`**:把 `RenderDevice` 落到具体图形 API——`RenderDeviceGLES`(OpenGL ES 3.0)、`RenderDeviceMetal`(MSL/Metal 2)、`PlatformBridge`(curl/NSURLSession 网络桥)、`RenderThreadPlacementAndroid`(渲染线程调度到大核)。
- **边界原则**:`core/`/`scene/` 之上一切渲染逻辑只认 `RenderCommand` 和 `RenderDevice` 接口,不 `#include` 任何 GL/Metal 头。`Engine` 持有注入的 `RenderDevice*`,由宿主(Android/iOS demo)决定用哪个后端构造。

---

## 核心设计决策 + 理由

### 1. `RenderCommand` 中间层为何存在,且刻意做"胖"★
`RenderCommand` 是所有绘制的唯一载体——不是"最小公倍数"式的裸描述符,而是内联了 surface/glTF PBR/raster-overlay 全部 uniform 槽位(固定容量数组)+ `RenderCommandTextureList`(固定容量避免逐命令堆分配)。
- **理由**:命令拷贝是**每帧热路径**——常驻命令缓存把命令逐可见瓦片拷进帧列表,`std::vector<Uniform>` 式通用 map 每次拷贝都付一次堆分配,固定容量数组把这笔成本消掉。
- **代价**:新增一个纹理槽必须同步三处(`RenderCommand.h` 槽位常量、GLES `currentTextures` 容量与 sampler 注册、Metal 绑定循环上限)——注释明确点名这个"孪生同步点"是漏改就永远采样不到的坑。
- `RenderCommand` 同时是**双后端解耦点**:由 `Renderer` 工厂产出(`makeGltfPrimitiveCommand`/`makeTerrainPrimitiveCommand`),被 `RenderDevice::submit` 消费;命令只含中性字段(`vertexStride`/`depthTest`/`blend`…),**同一份命令驱动两条完全不同的绑定代码路径**。

### 2. 双后端抽象边界:能力查询是"运行时告知点"而非开关
`RenderDevice` 纯虚接口分四组:能力查询、资源创建、帧操作、Surface 生命周期。**关键设计**:能力查询不是"要不要支持"的布尔开关,而是**后端能力差异的运行时告知点**——调用方据此显式拒绝并回报,而不是静默调用空实现:
- `supportsStencilClassification()` 默认 `false`,GLES 覆写 `true`,Metal **未覆写**。P6 贴地矢量 stencil 分类方案因此只在 GLES 可用,Metal 侧必须回落 CPU 高程钳制方案 A。
- `supportsOffscreenPostProcess()` 默认 `true`,Metal 显式覆写 `false`(注释:"MSL 入口 + submit 侧接线待补",不是能力上限)。

### 3. 两份"单一事实来源"契约头(编译期锁死)★
- `DepthConvention.h`:reverse-Z 的近/远深度值、比较方向、polygon-offset 符号全部从 `kNearDepth`/`kFarDepth` 派生(`static_assert` 锁死不能相等)。注释直接记录一次真实事故:blend 路径 `glPolygonOffset` 符号切到 reverse-Z 时忘翻,贴地描边被地形埋掉,只能真机 A/B 才发现。
- `BackendWindingContract.h`:GLES 用 CCW 前向面,Metal 用 CW(屏幕绕序反转);`static_assert(kGlesFrontFace != kMetalFrontFace)` 编译期锁死"必须相反",避免单侧改动静默反转另一侧背面剔除。

这两份文件的存在本身就是对"双后端各自硬编码同一物理量、靠对称注释互相解释"失败模式的直接补救——注释都点名了"此前"的坏状态。

### 4. 命令构建的严格顺序(order 整数 + 校验器主动拒绝)
`SceneRenderPipeline::render()` 按固定顺序构建:prepareTerrainOcclusion → buildSky → buildAtmosphere → buildLayer(地形/glTF + 矢量) → assembleTerrainBatches → applyMvpUniforms → sortAndValidate → submit → releaseRenderReferences。
- 顺序硬编码为整数 `mvpRenderOrder(kind)`(实测:`SkyBackground=0` < `GltfPrimitive=15` < `AtmosphereBackground=20` < `VectorStencil=29` < `VectorOverlay/Fill/Line/Point=30` < `VectorLabel=31`),由 `validateMvpRenderCommands` 硬性拒绝任何逆序命令(`std::runtime_error`,不是静默丢帧)。
- **约束靠校验器主动维持,不靠调用者小心翼翼按顺序 push**。`sortAndValidate` 只在检测到逆序或存在半透明 glTF 才真正 `stable_sort`,平时零排序开销。
- **注意**:大气背景 order=20 实际排在地形(15)**之后**画,与 AI_INDEX 正文叙事的"sky→atmosphere→surface"文字不严格一致(正文是简化表述,order 表才是真相)。

### 5. reverse-Z
投影矩阵把近平面映到深度 1、远平面映到深度 0,对齐 cesium/openglobus `reverseDepth:true`。理由:float 尾数在 0 附近最密,把"远"放在 0 把精度预算让给远景,是高空 z-fighting 的根治而非缓解。派生量:`kClearDepth=kFarDepth`、`kDepthCompare=GreaterEqual`、polygon-offset 符号。GLES `glClearDepthf(0)`+`GL_GEQUAL`;Metal `MTLCompareFunctionGreaterEqual`+预建三态。

### 6. streaming/stable 命令集划分 —— **该机制已被完全移除**
AI_INDEX §13 记录的 `RenderCommandStreamingSet`(stable-key 长期槽位 diff)**文件已从磁盘整个消失**(`find` 零命中),不是"去引用类还在"。当前等价物是两层缓存,都**不在 renderer/ 层**而在 tiling 层:`tiling/TileRenderCommandManager`(常驻命令缓存,跨帧复用绘制资源句柄)+ `renderer/TerrainInstanceBatcher`(每帧合成 instanced 命令,非跨帧 diff)。
- 即:**命令列表本身每帧从零重建**(`context.commands.clear()` 在 render 开头);"稳定性"只体现在瓦片侧绘制资源(VBO/IBO/纹理句柄)跨帧复用,不体现在命令对象本身跨帧身份。

### 7. 地形页存储在渲染侧的角色
`TerrainPageStore` 是地形表面影像的常驻纹理池:把地形瓦切成 `gridN²` cell,每 cell 独立按 LRU 拿 `texture2DArray` 一层,着色器经间接纹理查层号,取代"每瓦片一张合成影像"。渲染帧内 `updateVisiblePages` 每帧驱动(枚举可见 cell→缺页 fetch→写间接纹理),`applyToTerrainCommand` 把层号/页参数写进地形 `RenderCommand`——地形命令绑的不是"一张贴图",而是间接纹理 + array,shader 做一次 `texelFetch` 定位。槽位常量 `kGltfPageStoreArrayTextureSlot`/`Indir`(影像页)、`kGltfHeightTextureSlot`(高度页)、`kGltfRoadFieldTextureSlot`/`Indir`(**路网 SDF/D2 场页，⚠️即将废弃的兼容路径**)是它在 `RenderCommand` 层的落点。

#### PageStore mixed-scheme 规则

`TerrainPageStore` 是**单 canonical page domain 的多内容合成器**，不是任意瓦片体系之间的重投影器。一个实例同一时刻只有一个兼容键：

```text
PageStoreCompatibilityKey = {
  canonical page-facing TileScheme semantics,
  effective RasterOverlayProjection
}
```

- `providers[0]` 定义 canonical 页网格、投影、页 zoom、placement、UV 与几何仿射；同一 compose group 的后续 provider 必须消费相同逻辑 `PageKey`，并返回同一目标页空间的像素。
- 允许 source 的内容、`maximumLevel` 和源图尺寸不同；祖先钳制与页内重采样负责吸收这些差异。
- 不允许 XYZ/TMS、Mercator/Geographic、Standard/GCJ 等 page-facing 语义直接混在一个 group。异构原生 source 必须在 provider adapter 内完成选瓦、重投影和重采样，最终对外声明并输出 canonical page image；MVT drape provider 就属于这种 adapter，而不是 PageStore 自己理解 MVT 原生网格。
- 当前真实地形 tile 的 scheme 必须与 canonical scheme 相同；一个 PageStore 也不允许多个 canonical domain 并存。
- 不兼容时整组 PageStore fail-closed，清空 scheme-less 页账本和间接纹理绑定，继续以 Direct raster mapping/composite 为权威回退；不能静默丢掉某个 overlay，因为有序 `alphaOver` 少一层就已改变画面语义。
- `packKey(z/x/y)` 只是 canonical domain 内部的紧凑 key，不是跨 scheme 全局身份。provider/domain 变化会推进 `pageDomainGeneration`，影像 compose、场页与 GPU upload 的异步结果都必须匹配该 generation，防止旧回调在相同 `z/x/y + layer` 上串入新 domain。

这条边界与 Cesium 的“geometry scheme 可和单个 overlay provider scheme 不同”不冲突：Cesium 是每个 provider 在自己的 tiling/projection 空间独立建立映射；它没有定义多个异构 provider 直接共用一个 PageStore 页键。本规则是 gis-md 自研 PageStore 的运行期契约。

---

## 数据流(关键路径)

```
Engine::render(dt)
  device->beginFrame()                       # reverse-Z depth clear
  Scene::update() → Tileset::update() → TilePlan
  Scene::render() → SceneRenderPipeline::render(Context):
    reserveCommands
    prepareTerrainOcclusion                   # 地形深度纹理 → Renderer::setTerrainOcclusion
    buildSkyCommands / buildAtmosphereCommands
    buildLayerCommands → TilesetRenderFrameExecutor::buildRenderCommands
        → TileRenderCommandManager::buildTileDrawCommand
        → GltfDrawCommandBuilder::build:
            instanceCount>0 ? makeGltfPrimitiveInstancedCommand
            : useTerrainVertexFormat ? makeTerrainPrimitiveCommand(stride32)
            : makeGltfPrimitiveCommand(stride120)
        + 可见矢量图层(VectorFill/Line/Point/Label/Stencil)
    pool->flushHeightBakes / polarCap_.appendCommands
    assembleTerrainBatches                     # 资格瓦片 → terrainBatcher_ → instanced 合批
    applyMvpUniforms                           # 写 MVP/light-dir
    sortAndValidate                            # needsSort 才 stable_sort; validate 失败抛异常
    (presentable) beforeSubmit / runTerrainDepthPrepass / renderer.submit(commands)
    releaseRenderReferences                    # 契约 SubmitBeforeReleaseRefs
  device->endFrame()                           # eglSwapBuffers / presentDrawable
```

GLES `submit`:program/VBO/IBO/texture 冗余缓存 + 按 `cmd.vertexStride` 分派顶点属性布局(stride 32/120/8/12 各走各分支);Metal `submit`:按命令选 PSO/深度状态,走固定编号 `setVertexBytes`/`setFragmentBytes`。

---

## 关键契约与不变量

| 契约 | 说明 |
|---|---|
| `submit` 必须先于 `releaseRenderReferences` | 现已是**机制守卫**`GE_CONTRACT(SubmitBeforeReleaseRefs)`(注释自陈"在此之前只是一句文档")。命令持裸 `Buffer*/Texture*`+`resourceKeepAlive`,提前释放=绘制途中放掉 GPU 资源 |
| 深度/绕序约定单一事实来源 | `DepthConvention.h`/`BackendWindingContract.h` 的 `static_assert` 编译期锁死"近远深度值不能相等"、"GLES/Metal 前向面必须相反" |
| 命令严格排序 | `mvpRenderOrder`+`validateMvpRenderCommands`,逆序抛 `std::runtime_error`;还校验半透明 glTF 严格 back-to-front、instanced 三字段缺一不可、generation 非零、frameId==当前帧 |
| frameId/generation 是"读本帧状态"强制凭证 | 命令带上一帧 frameId 混入本帧列表会被直接拒绝,不静默渲染陈旧画面 |
| 能力查询是硬边界非配置项 | `supportsStencilClassification`/`supportsOffscreenPostProcess`:Metal 均不覆写为 true,调用方据此显式拒绝并回报,不允许静默 initFailed |

---

## 诚实得失

### ✅ 强项
- **命令模型表达力与校验强度**:固定容量数组换掉逐命令堆分配;`validateMvpRenderCommands` 把渲染顺序、深度/混合、半透明排序这些历史上"改一处漏一处"的不变量做成每帧强制校验(失败即抛,不是日志)。
- **reverse-Z / 绕序单一事实来源治理**:两份契约文件是"双后端各自硬编码同一物理量"失败模式的复盘产物,`static_assert` 把"必须保持某关系"做成编译期不可能违反。
- **`submit` 先于 `releaseRenderReferences` 机制化**:从纯文档约定升级成运行期断言——一次可验证的架构债偿还。

### ⚠️ 短板 / 已知债
- **Metal 后端能力缺口是结构性的,不是待办清单**:`supportsOffscreenPostProcess()=false` 意味 Metal 完全没有离屏后处理链(FXAA/aerial-fog tonemap/HDR 终端 pass);`supportsStencilClassification()` 默认 false 且 Metal 未覆写意味 P6 贴地矢量 stencil 方案在 Metal 不可用,只能回落 CPU 钳制方案 A。
- **HDR 管线是"默认关、仅 GLES"的半成品,且零自动化守卫**(lighting.md L-P3):①`kEnableHdrPipeline=false`②Metal `supportsOffscreenPostProcess()=false` 砍断 Metal 入口 ③tonemap 常数未对 tonemap 输出重标定。风险=**静默腐烂**:并行改 shader 可能悄改坏 HDR 开启态而无人察觉,因它"编不到、跑不到、测不到"(host ctest 不编译 GLSL/MSL)。2026-08-16 用户拍板长期做、短期挂起。
- **双后端统一维护成本明确存在且已造成过事故**:per-backend uniform binding 重复(GLES 按名字查找、Metal 按固定索引,两份手工同步);两份契约文件本身就是同步失败造成过真机 bug 的证据。
- **天空颜色治理分叉**(L-P1):CPU `SkyGradient`(解析散射,喂 clear/ambient)与 GLSL `computeSkyColor`(经验色板,喂天空背景+雾)语义都是"天空色"却互不知情,改一套不动另一套会让"雾色与天空对不上"。
- **历史包袱**:`RenderCommandKind::SurfaceTile` 已完全移除(AI_INDEX 记录的"vestigial"是更旧一层状态);`RenderCommandStreamingSet` 类型已从磁盘整个消失——该子系统迭代速度明显快于文档更新速度。

---

## 扩展点

- **加新 render pass**(如水面镜面反射):在 `SceneRenderPipeline::render()` 插入新 `buildXxxCommands`,给新 `RenderCommandKind` 分配 `mvpRenderOrder` 里一个整数区间(空档:15-20、20-29、31-100),并在 `validateMvpRenderCommands` 补 `case` 声明固定深度/混合/剔除状态。需独立 FBO 参照 `runTerrainDepthPrepass` 的 `beginPass(depthTarget)`→提交子命令→`beginPass(sceneTarget)` 收尾模式。
- **加新后端**(如 Vulkan,`Backend` 枚举已预留值但无实现):实现 `RenderDevice` 全部纯虚方法;能力查询必须**如实反映**真实支持度而非全 true 占位,否则"显式拒绝并回报"设计被绕过退化为静默失败;深度/绕序必须从两份契约头派生,不重定义字面量。
- **加新命令类型**(矢量近期 `VectorFill/Line/Point/Label/Stencil` 拆分是活样本):`RenderCommandKind` 加枚举 → `mvpRenderOrder` 加 case → `validateMvpRenderCommands` 加校验 → GLES/Metal `submit` 各加分派分支(两处必须同步,是双后端维护债的直接体现)。`VectorStencil` 额外引入 `StencilPhase`(ClassifyVolume/ClassifyColor)双阶段,是"一个 kind 内部再分子阶段"的参考。
- **地形页存储扩展**:已支撑影像页/高度页/路网 SDF 场页三类；其中路网 SDF/D2 场页即将废弃，仅作兼容保留。新增"页化平面"(如法线贴图页)时不要复制该场路径的生命周期或把其槽位视为长期 API；仍需注意"孪生同步点"警告,三处漏一处就是永久采样 0 且无任何报错。

---

## 对照系

- **openglobus 对齐(局部)**:reverse-Z 相机默认、大气/天空渐变模型对齐 openglobus。只对齐相机默认值与环境模型的设计取向,不意味渲染命令模型照搬(openglobus 是 WebGL immediate-mode 单后端,没有这套跨 GLES/Metal 的 `RenderCommand` 抽象层与校验层)。
- **cesium-native 对齐**:`RenderCommand` 是 cesium `DrawCommand`-analogous but far wider;`IPrepareRendererResources` 直接是 cesium 同名接口对应物,渲染层显式**不**是 raster 可见性/祖先回退/纹理绑定的真相来源。tonemap 选型对齐 Cesium 默认(PBR-Neutral、曝光固定、无 auto-exposure),是四引擎调研后的直接产物。
- **自研独有**:双后端统一命令模型 + 编译期契约文件 + 运行期 `contracts::Id` 断言这三层结构,在 cesium/openglobus 都无对应物——是本引擎因**必须同时维护 GLES 与 Metal 两条生产路径**而被迫发展出的治理机制,是双后端约束本身催生的架构投入,不是抄来的。`TerrainPageStore`(几何/纹理解耦)同属自研产物。
