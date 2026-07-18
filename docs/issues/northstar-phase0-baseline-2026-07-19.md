# 北极星 Phase 0：耦合态基线 + 测量台（可复现硬数字）

> 成文 2026-07-19。对应 [northstar-texture-geometry-decoupling-2026-07-18.md](northstar-texture-geometry-decoupling-2026-07-18.md) 的 **Phase 0**。
> 目标：立 release 可复现测量台、编译期钉死相机、采**耦合态**"之前"硬数字 + 参考画质，填 §5 闸门表"现状"列。
> 环境：设备 7e045e39 (Adreno)，**release(-O2)**，重庆 lon106.508/lat29.617，heightmap grid65 z0-12（本地 8091）+ 高德卫星 z0-18。

---

## 1. 测量台（可复现的三根支柱）

### 1.1 相机可复现性 —— 本项目历史最弱环节，本次坐实解法
`MinimalGlobeDemoConfig.h` 新增 `kMeasure*` 常量（编译期钉相机），配套两条铁律：

- **必须用 `obliqueElevationDegrees ∈ (0,90)`（free-look 模式）**：`CameraController::update()` 在 free-look 下早退不动相机，静止无 clamp → **逐位可复现**。实测 elev=45/height2500 两次启动 CamPose 完全一致（center/camH/pitch/heading 逐位相同）。**推荐 pitch=-45（elev=45）。**
- **禁用 `obliqueElevationDegrees=0`（orbit 模式）测量**：每帧重建 orbit + 地形 clamp，settled 位姿**随地形加载态漂移**（同 config 4000m 两次跑出 camH3094/pitch-25 vs camH2258/pitch-11）。这正是 [[ge-loading-experience-gap-2026-07-17]] 反复警告的"相机跨重启不可复现"。
- `elev=90` 退化（up∥viewDir 基座塌陷）——别用。
- **残留**：重载耦合态（高空 5000m + 深影像 churn）free-look 偶尔仍漂（见 §3 far stop）。彻底稳需"测量冻结相机"开关（冻 `CameraController::update`），**Phase 2 前值得补**（Phase 2 去耦对拍需要 far 也能钉）。

### 1.2 位姿无关指标（instrumentation，已加）
`EarthPerf` head 行（`scope=Tileset.update`，frameId≤3‖%120 采样，`frame=` join）新增三段常驻字节：

| 字段 | 含义 | 取值源（O(1)/O(overlays)，每帧廉价） |
|---|---|---|
| `memTotalKB` | 常驻总字节 | `Tileset::totalBytesUsed()` |
| `memContentKB` | 内容缓存（**地形几何 VBO/索引/heightmap 源**主导） | `contentBytesUsed()` = `contentCache_.totalBytesUsed()` |
| `memImageryKB` | 影像栅格 overlay 纹理 | `imageryTextureBytesUsed()` = Σ overlay `tileTextureBytesUsed()` |

其余闸门指标本就已打：`render=`（渲染瓦片数）、`visited=`（遍历数）、`selector=`（selector ms）、`reqClass=a/b/c`（第 3 段 c = **rasterDetailUpsample = 影像驱动的地形假细分请求数，耦合的直接指纹**）；`cmds=`（draw call，在 `scope=Tileset.buildRenderCommands` 行）。

> 注：`memTotalKB` 未含 baseVertices/terrainGpuVertex 双拷贝（[[cache-10x-compression-investigation]]），是"缓存计量口径"绝对值；跨 stop **一致**，可比。

### 1.3 采集法
`CamPose` logcat 行（GLESView 每帧采样打 center/camH/pitch/heading）= 位姿真值，每次采集读它校验钉死。采一个 stop = 改 `kMeasure*` → `./build_apk.sh release` → 装 → settle（`adb shell sleep`）→ 读 settled 末帧 head/render 行 + 网络 z 深度 + 截图。脚本见 scratchpad（`run_stop.sh` / `capture_baseline.sh`）。

**测量纪律**：settled = notReady≈0；selector/visited 静止被 selection-reuse 归 0（`reused=1`），故 selector 的耦合成本取**初始细化期确定性 peak**；分析排除 frame≤3 启动帧。

---

## 2. 耦合根因 —— 位姿无关的铁证（S1 单相机 settle 轨迹）

同一钉死相机，从启动到 settled 的轨迹本身就证伪不了地形被影像拖着假细分：

| 阶段 | render | memContent | memImagery | 说明 |
|---|---|---|---|---|
| frame 120（粗，合法细化到 z12） | 54 | 72 MB | 31 MB | 地形 8091 到 z12 为止 |
| settled（影像驱动细化完） | **257** | **428 MB** | **115 MB** | 地形假细分到 z13-18 托 z18 高德影像 |

- 地形瓦片请求 8091 **最深 z=12**（native cap），而高德影像请求到 **z=18**；两者之差全靠**上采样（clip 父 z12）**假细分。
- `reqClass=0/0/N`（rasterDetailUpsample）帧坐实是**影像驱动**的地形上采样，非几何自身需要。
- 这条与相机是否 nadir/oblique 无关，是耦合的结构性指纹。

---

## 3. §5 闸门"现状（耦合）"列 —— 可复现同位姿对拍

**方法**：固定相机位姿（elev=45 free-look，逐位可复现），同一位姿下 `imageryMaxZoom=18`（耦合态）vs `=12`（=地形 native cap，影像不再驱动上采样=**去耦下界代理**）对拍。z18−z12 之差 = **纯耦合浪费**（同位姿，零 confound）。

| pose（elev45, heading0） | 影像 | render | visited | memTotal | memContent | memImagery | selector peak | Gaode z 请求 |
|---|---|---|---|---|---|---|---|---|
| **mid** camH1500 pitch-45 | z18 耦合 | **68** | churn | 124 MB | 84 MB | **39 MB** | 5.5 ms | 16 |
| **mid** camH1500 pitch-45 | z12 capped | **3** | 0 | 85 MB | 76 MB | **8 MB** | 4.2 ms | 12 |
| **near** camH600 pitch-45 | z18 耦合 | **63** | churn | 123 MB | 80 MB | **43 MB** | 6.2 ms | 18 |
| **near** camH600 pitch-45 | z12 capped | **3** | 0 | 83 MB | 73 MB | **10 MB** | 2.9 ms | 12 |
| far camH5000 pitch-45 | z12 capped | 6 | 0 | 99 MB | 86 MB | 13 MB | 4.7 ms | 12 |
| far（config 5000）**⚠位姿漂到 camH1852/pitch-5.8** | z18 耦合 | 237 | 373 | 576 MB | 470 MB | 106 MB | 22.5 ms | 17 |

**同位姿耦合倍数（mid / near，位姿逐位一致，干净）**：

| 指标 | mid ×倍 | near ×倍 | 归属 |
|---|---|---|---|
| **地形渲染瓦片数** render | 3→68 = **23×** | 3→63 = **21×** | draw call / selector 遍历 / clip 主线程尖刺 全随它涨 |
| **影像纹理字节** memImagery | 8→39 MB = **4.8×** | 10→43 MB = **4.3×** | 每假瓦片各自托一张 z13-18 影像纹理 |
| selector peak | 4.2→5.5 ms | 2.9→6.2 ms = **2.1×** | 遍历更深的树 |
| 常驻总字节 memTotal | 85→124 MB = 1.5× | 83→123 MB = 1.5× | 未触 192MB 缓存预算上限 |
| **地形几何字节** memContent | 76→84 MB = **+8 MB** | 73→80 MB = **+7 MB** | ← **注意：只涨一点点** |

### 3.1 一条必须诚实记下的发现（细化北极星论点）
**耦合的资源成本集中在"瓦片数"与"影像纹理字节"，不在"地形几何字节"。** 上采样瓦片是父 z12 网格的小块 clip，几何 VBO 字节增量很小（+7~8MB）。真正爆的是：
- **瓦片数量 ~22×**（3→65）→ draw call、selector 遍历、每片一次 clip 主线程尖刺、每片一次上传；
- **影像纹理 ~4.5×**（每假瓦片托一张自己的深 z 影像）；
- **churn / 峰值**：耦合态 settled 前长时间 churn（notReady 高、selector peak 翻倍）。

`memTotal` 只 1.5×（且被 192MB 预算兜着不无限涨）——所以"峰值内存爆炸"在**静止 settled 态**不是最锋利的指标；最锋利的是**瓦片数（22×）+ 影像纹理字节（4.5×）+ churn 期 selector/clip 尖刺**。这不推翻北极星（解耦仍砍掉这 22× 瓦片与全部假细分 churn），但把"内存爆炸"的措辞精确化为"**瓦片数 + 影像纹理 + CPU churn 爆炸；地形几何字节其次；总内存被缓存预算封顶**"。

---

## 4. 参考画质（艺术地板，Phase 2 不得回退）

同一 near 位姿（camH600, elev45），耦合 z18 vs capped z12 截图（`docs/issues/northstar-phase0-shots/`）：

- **`m_near_600_z18.png`（耦合，z18）** = 清晰的重庆城区卫星图（道路/楼宇/树/车可辨）。**这是必须守住的近景画质地板。**
- **`m_near_600_z12.png`（capped，z12）** = **糊成一片不可用**（z12 影像 ~10m/px 拉到 600m 近景）。

⟹ **决定性结论**：naive 地砍地形 LOD 到 z12 会把影像一起拖成右图这种糊——**不可接受**。解耦的全部意义 = **拿到左图（z18）的清晰度，只花右图（z12 地形）的资源**。这就是 Universal Texture 要解、而"直接 cap 几何"解不了的洞。

其它参照：`s1_shallow_4000m.png` = 斜视地平线，底边 artifact（锯齿地形边 + skirt 裙墙 drape + 露天）——[[terrain-continuous-lod-redesign-2026-07-17]] 记的斜视底边 artifact，settled 后为暂态。

---

## 5. 交付物清单

- **测量台代码**（本次提交）：引擎 `memTotal/memContent/memImagery` 上 EarthPerf 行（`Tileset`/`TileCacheOwnershipManager`/`TileUpdateDebugLogInput`/`TileFrameDebugLogFormatter`/`TilesetUpdateFrameRuntime`）；demo `kMeasure*` 钉相机 + `kMeasureImageryMaxZoom` 对拍开关 + `CamPose` 位姿真值日志。148/148 native 绿。
- **可复现基线数据**：本文 §2/§3 表；原始 logcat/截图在 scratchpad `baseline/`，关键截图入库 `docs/issues/northstar-phase0-shots/`。
- **§5 闸门"现状"列已填**（§3）。**Phase 2 验收 = 同一 elev45 位姿、去耦后 z18 影像照清，而 render/memImagery/selector/churn 回落到接近 capped-z12 那列**（即"斜率≈0"）。

## 6. 待补（非 Phase 0 阻塞）
- **测量冻结相机**开关（冻 `CameraController::update`）：让重载耦合 far stop 也可复现。Phase 2 去耦对拍前补。
- Phase 2 用同一批 `kMeasure*` 位姿重测 = 去耦列，与本文对拍即出"斜率"。
