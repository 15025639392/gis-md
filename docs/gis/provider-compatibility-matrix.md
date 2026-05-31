# Provider 兼容矩阵

本文件记录真实数据源/provider 的兼容信息。接入任何 provider 前必须补充矩阵；不得只靠 URL 模板猜测 tile scheme 或 CRS。

## 矩阵字段

```text
Provider:
  name:
  type:
  url/template:
  tileScheme:
  crsProfile:
  tileSize:
  minZoom/maxZoom:
  yDirection:
  attribution:
  auth/token:
  cors:
  rateLimit:
  coordinateOffset:
  knownIssues:
  controlPoints:
  maturity:
```

## OSM XYZ

```text
name: OpenStreetMap XYZ
type: raster/vector basemap
tileScheme: XYZ Web Mercator
crsProfile: EPSG:3857 tile / WGS84 geographic input
tileSize: 256
yDirection: down
attribution: required
maturity: candidate
notes:
  Must follow OSM tile usage policy.
```

## GeoServer WMTS

```text
name: GeoServer WMTS
type: raster/vector tile service
tileScheme: WMTS TileMatrixSet
crsProfile: depends on TileMatrixSet
required:
  capabilities document
  matrix identifiers
  topLeftCorner
  scaleDenominator
  supported CRS
maturity: candidate
```

## 天地图

```text
name: Tianditu
type: basemap / imagery / annotation
tileScheme: provider-specific, confirm by official docs
crsProfile: commonly has geographic and WebMercator variants; must verify
auth/token: required
attribution: required
maturity: unknown until tested
```

## 高德

```text
name: Gaode/Amap
type: web map tiles
tileScheme: provider-specific XYZ-like
crsProfile: GCJ-02 aligned, must verify
coordinateOffset: yes, do not treat as WGS84
controlPoints: required
maturity: unknown until tested
```

## 腾讯地图

```text
name: Tencent Maps
type: web map tiles
tileScheme: provider-specific XYZ-like
crsProfile: GCJ-02 aligned, must verify
coordinateOffset: yes
controlPoints: required
maturity: unknown until tested
```

## 百度

```text
name: Baidu Maps
type: web map tiles
tileScheme: Baidu-specific
crsProfile: BD-09 / BaiduMercator, not EPSG:3857
coordinateOffset: yes
controlPoints: required
maturity: unknown until tested
```

## Mapbox / MapLibre

```text
name: Mapbox/MapLibre compatible
type: raster/vector tiles
tileScheme: usually XYZ Web Mercator
crsProfile: EPSG:3857 tile
auth/token: provider dependent
style: may affect tile URL/cache key
maturity: candidate
```

## Cesium ion / 3D Tiles

```text
name: Cesium ion / 3D Tiles
type: terrain / 3D Tiles
tileScheme: dataset dependent
crsProfile: tileset transform/boundingVolume dependent
auth/token: often required
maturity: candidate
```

## 兼容等级

- `unknown`：未验证。
- `candidate`：文档清楚，有基本接入计划。
- `mvp-tested`：MVP 示例可加载。
- `integration-tested`：有控制点、错误处理和截图测试。
- `production-ready`：有压测、限流、版权、缓存和回归验证。

Provider 不得在 `unknown` 或 `candidate` 状态下被宣传为生产可用。
