# 贴地线 stencil 化设计(P6d)——终态替换方案 A 参数档

日期:2026-07-31 | 状态:设计定稿 → 实现
前置:P6a 面 fill stencil 双 pass(`appendFillVolume`/ClassifyVolume/ClassifyColor)已真机点亮;
方案 A 过渡档 = 8m 细分 + 2.5m 抬升(commit 588e5afde),陡崖/粗 DEM 仍可能露头且抬升带视差。

## 0. 调研结论:Cesium GroundPolylinePrimitive 其实不是 stencil 方案

精读 `.ref/cesiumjs/groundpolyline/`(GroundPolylineGeometry.js / GroundPolylinePrimitive.js /
PolylineShadowVolumeVS/FS.glsl / ClassificationPrimitive.js)后的关键事实:

- Cesium 贴地线 = **单 pass、无深度测试、FS 读全局深度纹理**(`czm_globeDepthTexture`),
  逐 fragment 重建地形点 eye 坐标,对"右平面 + 起止 miter 平面"三张解析平面做距离测试
  实现像素级贴地与像素线宽(PolylineShadowVolumeFS.glsl:13-37)。stencil 只用于 3D Tiles
  掩码(只读,op 全 KEEP)。
- 真正的 z-fail 双面 stencil 计数是**面**的 ClassificationPrimitive(两命令:stencil-depth
  + color,与我们 P6a 完全同构)。
- Cesium 的墙体 CPU 侧是**零厚度**(±1e-5m nudge),真实宽度在 VS 按
  `width * czm_metersPerPixel(positionEC)` 沿 miter 法线挤出(VS:149-159),体积做成
  2× 目标宽度作保守裕度,FS 再按地形点深度裁到精确像素宽。

## 1. 选型:复用 P6a z-fail stencil,不走 per-fragment 深度重建

两条可行路线:

| | A. Cesium-faithful(FS 读深度) | B. z-fail stencil 复用(选定) |
|---|---|---|
| 贴地精度 | 像素级 | 像素级(与面 fill 同机制) |
| 线宽精度 | 精确像素宽(按地形点深度) | 近似像素宽(按墙顶点深度换算米,见 §3) |
| 边缘 AA | 可做 smoothstep 羽化 | 硬边(与 fill 一致) |
| 新增基础设施 | 场景 pass 中读**本帧**地形深度纹理:需新增深度拷贝时序 + pass 依赖;reverse-Z 深度重建(高危区,近期三次踩坑同区);天空哨兵值与 Cesium 相反 | 无:StencilPhase 两态、renderState、校验、FBO stencil 附件全部现成 |
| 每像素成本 | 深度采样 + unproject(2× 宽保守体,overdraw 高) | 固定功能 stencil,无 FS 采样 |
| 失效模式 | 深度约定写反 → 全画/全不画;pass 顺序错 → 线滞后地形一帧 | 与 fill 相同(已知已验) |

选 B 的核心理由:满足同样的"像素级贴地"目标,增量全部落在已真机验证的管线上;
路线 A 的三重强耦合(深度重建/FBO 附件/pass 顺序)恰是本项目最近连续踩坑的区域。
潜在缺点(诚实列出):① 线宽换算用墙顶点深度而非地形点深度,掠视大深度差时有
亚像素级宽度偏差;② 边缘无羽化(靠场景 FXAA);③ Metal 后端无 stencil 分类,继续
回落方案 A(与面 fill 现状一致,范围决策另议)。若未来要羽化/精确宽,可在 B 之上
增量叠加 FS 深度读,不推翻本设计。

## 2. 几何:连续横截面墙带(优于 Cesium 的逐段 8 顶点盒)

Cesium 每段独立 8 顶点盒 + 起止 miter 平面,是为 FS 解析裁剪服务的;我们不做 FS
裁剪,不需要逐段平面,因此改用**共享横截面的连续棱柱带**,天然水密、无段间接缝:

- 输入:clamp 预变换后的细分折线(`prepareClampedMeters` 细分点,每点已带采样高)。
- 每个细分点一个**横截面 4 顶点**:bottom±side、top±side。
  - bottom/top 高度 = 该点采样高 ± `kVolumeMarginMeters`(120m,与 fill 共用常量;
    细分点逐点采样比 fill 的 8×8 粗网格密得多,但渲染地形 LOD 网格与采样 DEM 的
    偏差仍在,余量不减)。体高随线走 → 240m 高的"贴地飘带",不是全线一个大盒。
  - CPU 侧**零宽度**:4 顶点 lng/lat 相同,仅高度不同(对齐 Cesium 的零厚墙思路)。
- 顶点属性 24B:`pos(3f, 相对桶原点) + extrude(3f)`。
  `extrude = miterDir * miterScale * sideSign`——挤出方向、miter 缩放、左右符号
  全部烘进一个向量,VS 只做 `pos + extrude * halfWidthMeters`。
  - `miterDir`:切平面内正交化的 to-prev/to-next 角平分线(对齐 Cesium
    computeVertexMiterNormal:396-438;端点退化为 `cross(forward, up)`)。
  - `miterScale = 1/max(dot(miterDir, rightNormal), 0.25)`,miter-limit=4,与现有
    屏幕空间线 shader 的 kMiterMin 同款。极尖角不做 Cesium 的 breakMiter(那是为
    平面裁剪服务的);横截面共享保证水密,尖角只会宽度过冲,不会破洞。
- 拓扑:相邻横截面之间 left/right/top/bottom 四个 quad(8 tri),首尾两个端 cap。
  顶点全共享 → 任意边恰好被 2 个三角形引用(可写机器可查的流形断言)。
- 闭合环(polygon outline):首尾横截面 wrap 共享,无端 cap。

## 3. 线宽:VS 按眼深换算世界米挤出

- CPU 每帧(appendBucketCommands)算 `u_halfWidthPerEyeZ = lineWidthPx * 0.5 *
  (2*tan(fovy/2) / viewportH)`,即"单位眼深对应的半宽米数"。lineWidthPx 沿用 P6b
  zoom 表达式求值结果,保持样式语义不变。
- VS:`ec = u_modelView * pos; halfW = u_halfWidthPerEyeZ * max(abs(ec.z), znear);
  world = pos + extrude * halfW; gl_Position = u_mvp * world`。
- 像素语义近似:半宽按**未挤出顶点**的眼深换算,同一横截面上下顶点眼深不同 →
  墙微呈梯形,视觉可忽略;地形交线处的实际像素宽 ≈ 目标宽(墙竖直,交点深度 ≈
  墙深度)。无需 Cesium 的 2× 保守裕度(那是给 FS 裁剪留余量的)。
- 挤出量上限:halfW clamp 到桶尺度上限(如 5km),防近平面附近数值发散
  (对齐现有线 shader kMaxExtrudeNdc 的防御思路)。

## 4. Pass 组织:逐色组命令对,完全复用两态 StencilPhase

- `tessellateFeatureInto` 分流:`stencilLine = clamp && renderDevice_ &&
  supportsStencilClassification()`;LineString 与 polygon 外环 outline 都改走
  `appendLineVolume`,不再产出方案 A 线网格;不支持时回落现状。
- 按解析线色归组(`lineVolumeGroups`,与 fill 的 `volumeGroups` 同结构**分开存**,
  防止同色 fill/line 误并组)。每组一对相邻命令:
  - ClassifyVolume:depthTest=on / depthWrite=off / blend=off / cull=off,
    双面 INCR/DECR_WRAP z-fail 计数(后端现成,不改);
  - ClassifyColor:depthTest=off / blend=on,NOTEQUAL 0 覆盖 + op ZERO 清零。
  两 pass 用**同一份挤出几何 + 同一新 shader**(vectorLineStencilShader,见 §5);
  色 pass 画的是体的屏幕投影 ∩ stencil 标记像素,与面 fill 同理。
- 排序:仍走 `RenderCommandKind::VectorStencil`(order 29)+ 插入序紧邻契约;
  fill 组对与 line 组对可任意先后(每个色 pass 都顺手清零 stencil,组间不串)。
- `validateMvpRenderCommands` 的两态硬校验原样适用(状态组合不变,shader 无关)。
- polygon offset 隐式耦合:色 pass blend=true → 自动套 `(1,1)` bias,与面一致;
  线更细是否需要不同幅度,留真机 A/B(§7)。

## 5. Shader(GLSL + MSL 对齐版)

新增 vectorLineStencil shader(两 pass 共用):
- attributes:`a_position(3f)` `a_extrude(3f)`;
- uniforms:`u_modelViewProjection` `u_modelView` `u_halfWidthPerEyeZ` `u_color`;
- VS 如 §3;FS 直接输出 `u_color`(volume pass 颜色被 colorMask 关掉,无所谓)。
- MSL 同步写(语义对齐)但 Metal 矢量路径不出货、不验真机,与现有线 shader 注释
  约定一致。

## 6. 派生影响

- **heightOffset 语义**:stencil 线是"给地形表面像素染色",抬升不存在也不需要——
  视差问题自然消失。方案 A 回落路径仍用 heightOffset(参数档保留)。
- **细分密度**:stencil 路径的细分只服务①线形跟随大圆曲率②高度采样密度,不再
  服务"贴地不露头",8m 过密。已解耦(P6d 收尾):stencil 线路径用
  `max(clampDensifyMeters, 100m)`(用户显式更粗时尊重更粗值),方案 A 回落
  仍用原值。
- **重钳节流**:地形代次变化触发的 120 帧节流重镶原样适用(体高依赖采样高)。
  注意 stencil 线对重钳的敏感度远低于方案 A(±120m 余量内 LOD 变化免重钳也不
  断线),重钳只为收紧体高。
- **dash/lengthSoFar**:方案 A 线 shader 的 dash 留口本就未接;stencil 路径 v1 不带,
  如需 dash 需在顶点带 lengthSoFar 并在 FS 做纹理坐标(Cesium FS:68-70 有对齐
  平面方案可抄),列 TODO。
- **pick**:纯 CPU 几何拾取,不读 GPU 资源,零影响。
- **内存**:24B × 4 顶点/细分点;8m 细分下 1km 线 ≈ 12KB 顶点 + 索引,demo 规模
  无压力;放宽细分后再降一档。

## 7. 验证标准(目标驱动)

1. host 构建 + ctest:新增流形断言测试(线体积任意边被恰好 2 三角引用;闭环
   wrap 正确),全绿;既存 2 红(raster_overlay_details/scene_frame_state)不劣化。
2. 真机(GLES,重庆 demo,本地 heightmap 8091 + adb reverse):
   - RESET 视角拉近一档,示范区 A 东缘道路切坡段——此前断线位置**连续无断裂**;
   - 拉远/拉近线宽像素观感与方案 A 一致(zoom 表达式仍生效);
   - 低空斜视贴崖段无"抬升浮空"视差(对比方案 A 的 2.5m 抬升);
   - 面 fill stencil 与线 stencil 同屏共存无互相染色(组间清零契约)。
3. 像素判断归用户,机制信号(命令对数、stencil 附件、组划分)走日志自查。

## 8. 移植陷阱对照(来自 Cesium 精读,已在设计中消解)

| Cesium 陷阱 | 本设计的消解 |
|---|---|
| 深度重建对深度约定强依赖(reverse-Z 下天空哨兵相反) | 不读深度纹理,固定功能 stencil |
| 地形超出 min/max 体高 → 静默断线 | 逐细分点采样 ±120m 余量;比 Cesium 粗矩形查表精细 |
| miter 平面 cross 顺序写反 → 接缝丢块 | 无逐段平面;横截面共享水密 + 流形断言 |
| 掠视宽度失真(FS 用地形深度 vs VS 用墙深度) | 不做 FS 裁剪,宽度即墙宽;偏差亚像素级 |
| `dot(miter, right)` 除零 | miterScale clamp(kMiterMin=0.25)+ 挤出量上限 |
| FBO 无 stencil 附件静默失效 | P6a 已修(hasStencil + DEPTH32F_STENCIL8) |
| winding/cull 后端约定冲突 | 双 pass cull=off,双面计数对绕向翻转免疫(P6a 同款) |
| 宽度 UNSIGNED_BYTE 截断 | 宽度走 float uniform(P6b 通路),不进顶点 |
