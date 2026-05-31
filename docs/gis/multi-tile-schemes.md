# 多瓦片体系与无偏移叠加

地球引擎通常需要同时承载不同来源的瓦片：OSM、WMTS、XYZ、TMS、天地图、高德、百度、腾讯、离线包、自定义私有服务等。它们可能看起来都是 `z/x/y`，但背后的 CRS、坐标偏移、瓦片矩阵、原点、分辨率序列和 URL 规则可能完全不同。

目标不是“都能加载”，而是“在同一个地球空间中可解释地叠加，并能证明没有系统性偏移”。

## 核心原则

- 不要把所有 `z/x/y` 服务都当成标准 XYZ Web Mercator。
- 不要把 provider 的 URL 模板当成 tile scheme。
- 不要在渲染层临时加魔法偏移量。
- 每个图层必须声明自己的 `TileScheme` 和 `CrsProfile`。
- 所有图层进入引擎前，必须能转换到统一的世界表达，例如 WGS84 cartographic、ECEF 或引擎认可的局部坐标。
- 国内互联网地图常见 GCJ-02、BD-09 或厂商私有投影/瓦片规则。AI 不得假设它们与 WGS84/标准 Web Mercator 可直接无偏移叠加。

## 必须区分的四层坐标

1. 地理坐标：经纬度，例如 WGS84、GCJ-02、BD-09。
2. 投影坐标：例如 EPSG:3857 Web Mercator、百度墨卡托、局部投影。
3. 瓦片矩阵坐标：z/x/y、TileMatrix/TileRow/TileCol、原点、分辨率。
4. 渲染世界坐标：ECEF、ENU、相机相对坐标或引擎内部坐标。

任何图层偏移问题，都必须先定位是哪一层坐标不一致。

## TileScheme 抽象

每个瓦片体系都应建模为结构化配置或类：

```text
TileScheme {
  id: string
  provider?: string
  matrixSet: "XYZ" | "TMS" | "WMTS" | "Baidu" | "Gaode" | "Tencent" | "Geographic" | "Custom"
  crsProfile: string
  origin: "top-left" | "bottom-left" | "custom"
  tileSize: 256 | 512 | number
  minZoom: number
  maxZoom: number
  bounds: GeographicBounds | ProjectedBounds
  resolutions?: number[]
  scales?: number[]
  yDirection: "down" | "up"
  xRangeAt(z): [minX, maxX]
  yRangeAt(z): [minY, maxY]
  tileToBounds(z, x, y): Bounds
  positionToTile(position, z): TileKey
}
```

`TileScheme` 只负责瓦片矩阵和范围，不负责业务图层样式。

## CrsProfile 抽象

每个图层还必须声明 CRS profile：

```text
CrsProfile {
  id: "WGS84" | "WebMercator" | "GCJ02" | "BD09" | "BaiduMercator" | "Custom"
  geographicCrs?: string
  projectedCrs?: string
  datum?: string
  axisOrder: "lng-lat" | "lat-lng" | "custom"
  units: "degree" | "meter" | "pixel" | "custom"
  toEngineCartographic(input): Cartographic
  fromEngineCartographic(input): ProviderCoordinate
}
```

`CrsProfile` 负责把 provider 坐标转换到引擎统一空间。不能把转换逻辑散落在 URL 拼接、shader 或 UI 事件里。

## 常见瓦片体系

### 标准 XYZ Web Mercator

常见于 OSM 和很多互联网地图底图。通常特征：

- z/x/y
- y 轴从北向南递增
- tile size 通常 256 或 512
- 投影通常为 EPSG:3857
- 覆盖纬度通常裁剪到 Web Mercator 可表示范围

仍需验证 provider 是否使用 retina tile、512 tile、style scale、token 参数或自定义 bounds。

### TMS

TMS 与 XYZ 常见差异是 y 轴方向。不能只替换 URL，必须在 TileScheme 中明确 `yDirection`，并测试 y 翻转：

```text
tmsY = (2^z - 1) - xyzY
```

### WMTS

WMTS 不能简单视为 XYZ。必须读取或配置：

- TileMatrixSet
- TileMatrix identifier
- TopLeftCorner
- ScaleDenominator
- TileWidth / TileHeight
- MatrixWidth / MatrixHeight
- supported CRS

### Geographic Tiling

有些服务使用经纬度矩形切片，不是 Web Mercator。必须明确每个 tile 的经纬度 bounds 和分辨率，不得套用 EPSG:3857 公式。

### 高德、腾讯等 GCJ-02 体系

国内互联网地图常见图层使用 GCJ-02 相关坐标体系或与 GCJ-02 对齐的瓦片。原则：

- 不得直接与 WGS84 数据叠加后声称无偏移。
- 业务数据若是 WGS84，应在叠加到 GCJ-02 底图前经过合法、明确、可测试的坐标转换，或反向把底图纳入引擎统一空间。
- 转换函数必须集中在 `CrsProfile`，并标明适用范围和误差。
- 中国境外、边界附近或数据源未授权转换时，必须保守处理并记录限制。

### 百度 BD-09 / 百度墨卡托

百度地图体系通常与标准 Web Mercator/GCJ-02 不能直接等同。原则：

- `BD-09`、`BaiduMercator` 和标准 `EPSG:3857` 必须作为不同 `CrsProfile`。
- 百度瓦片的 x/y、zoom、投影和坐标转换必须独立建模。
- 与 WGS84、GCJ-02、Web Mercator 图层叠加时，必须用控制点验证偏移。

### 私有瓦片系统

私有服务可能使用：

- 非 256 tile size
- 非 2 的幂矩阵
- 局部坐标原点
- 自定义分辨率序列
- 自定义 y 轴方向
- 自定义加密、签名、token、style 参数
- 非全球 bounds

接入前必须要求 provider 给出 tile matrix 文档或通过样例瓦片和控制点反推并记录。

## 无偏移叠加策略

地球引擎应选择一个统一世界空间。推荐：

- 权威分析数据：WGS84 / ECEF / ENU。
- Web 底图图层：通过 `TileScheme` 和 `CrsProfile` 转入统一世界空间。
- 国内偏移底图：作为独立 CRS profile，不得和 WGS84 混称。
- UI 点击、picking、测量：输出时必须说明返回的是 WGS84、GCJ-02、BD-09 还是业务 CRS。

叠加流程：

1. Provider 声明 `TileScheme` 和 `CrsProfile`。
2. Scheduler 只处理 tile key，不做坐标偏移。
3. Provider 根据 tile key 请求和解析数据。
4. Layer 把 tile bounds 转换到引擎统一世界空间。
5. Renderer 只渲染已转换到统一空间的数据。
6. Picking 输出通过目标 `CrsProfile` 反转换。

## 控制点验收

无偏移叠加必须用控制点验证。控制点可以来自：

- 已知地标点
- 道路交叉口
- 行政边界角点
- 高精度测绘点
- 同源数据中的参考点

验收要求：

- 至少覆盖不同 zoom。
- 至少覆盖图层 bounds 的中心和边缘。
- 国内图层至少覆盖一个城市中心、城市边缘和跨 provider 对照点。
- 记录允许误差，例如像素误差或米级误差。
- 控制点要注明坐标来源和 CRS。

不要只用一张截图判断“无偏移”。截图只能作为辅助证据。

## 测试要求

必须有纯函数测试：

- `tileToBounds(z, x, y)`
- `positionToTile(position, z)`
- y 轴方向
- zoom 边界
- bounds 边界
- provider 坐标到引擎坐标
- 引擎坐标到 provider 坐标
- 控制点偏移误差

必须有视觉或集成测试：

- 标准 XYZ + WGS84 矢量叠加
- WMTS + WGS84 矢量叠加
- GCJ-02 图层 + 对应 GCJ-02 数据叠加
- BD-09/百度瓦片 + 对应百度坐标数据叠加
- WGS84 业务数据转换后与国内底图叠加

## 禁止做法

- 在 shader 中直接写 provider 偏移补偿。
- 用一个全局 `isChinaMap` 开关混改所有图层。
- 把 GCJ-02 或 BD-09 坐标伪装成 WGS84。
- 用 `EPSG:3857` 名称承载百度墨卡托或其他私有投影。
- 只靠 URL 模板判断 tile scheme。
- 只测一个 zoom 或一个城市点就声称支持某 provider。
- 忽略服务条款、license、attribution 或坐标转换合规边界。
