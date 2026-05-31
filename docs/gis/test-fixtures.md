# 测试 Fixtures

本文件定义地球引擎开发中应固定使用的测试样例。AI 写测试时优先使用这些 fixtures，不要每次临时编数据。

## 坐标转换 Fixtures

### WGS84 赤道本初子午线

```text
input:
  longitude: 0 degree
  latitude: 0 degree
  height: 0 meter
expected:
  ECEF.x ~= 6378137
  ECEF.y ~= 0
  ECEF.z ~= 0
```

### WGS84 北极

```text
input:
  longitude: 0 degree
  latitude: 90 degree
  height: 0 meter
expected:
  ECEF.x ~= 0
  ECEF.y ~= 0
  ECEF.z ~= 6356752.314245
```

### 北京近似点

```text
input:
  longitude: 116.397 degree
  latitude: 39.908 degree
  height: 50 meter
purpose:
  用于 degree/radian、GCJ-02/WGS84 标注和国内 provider 控制点测试。
```

## XYZ Tile Fixtures

### z=0

```text
tile:
  z: 0
  x: 0
  y: 0
expected:
  covers Web Mercator world bounds
```

### z=1

```text
XYZ:
  1/0/0: northwest
  1/1/0: northeast
  1/0/1: southwest
  1/1/1: southeast
```

### TMS y flip

```text
z: 3
xyzY: 2
tmsY: 5
formula:
  tmsY = 2^z - 1 - xyzY
```

## Picking Fixtures

```text
viewport:
  width: 800
  height: 600
  dpr: 1
case:
  center screen pick should hit ellipsoid when camera looks at earth center.
```

```text
viewport:
  cssWidth: 800
  cssHeight: 600
  drawingBufferWidth: 1600
  drawingBufferHeight: 1200
  dpr: 2
purpose:
  DPR picking correction.
```

## 反经线 Fixtures

```text
bbox:
  west: 170
  east: -170
  south: -10
  north: 10
meaning:
  crosses antimeridian
expected:
  split or normalized explicitly; never assume west < east.
```

## 高纬 Fixtures

```text
point:
  longitude: 0
  latitude: 85
purpose:
  Web Mercator clamp、tile range、camera high latitude behavior.
```

## GeoJSON Fixtures

### Point

```json
{
  "type": "Feature",
  "properties": { "id": "p1", "status": "normal" },
  "geometry": { "type": "Point", "coordinates": [116.397, 39.908] }
}
```

### Polygon With Hole

```json
{
  "type": "Feature",
  "properties": { "id": "poly-hole" },
  "geometry": {
    "type": "Polygon",
    "coordinates": [
      [[0,0],[10,0],[10,10],[0,10],[0,0]],
      [[2,2],[8,2],[8,8],[2,8],[2,2]]
    ]
  }
}
```

## Tile Failure Fixtures

- 404 tile。
- timeout tile。
- invalid image body。
- CORS blocked tile。
- out-of-bounds tile。

这些 fixtures 用于验证失败降级、重试上限和 parent fallback。

## 长时间运行 Fixture

```text
scenario:
  rotate globe for 5 minutes
  zoom in/out 50 times
  switch basemap 20 times
expected:
  texture/buffer count returns near baseline after cleanup
```
