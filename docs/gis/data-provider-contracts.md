# 数据 Provider 接口契约

地球引擎的数据接入层必须可替换、可测试、可取消。AI 不得把 URL 拼接、缓存、解析、坐标转换和渲染资源创建混在同一个函数里。

## Provider 类型

常见 Provider：

- `ImageryProvider`：影像瓦片，例如 XYZ、WMTS、WMS、离线瓦片。
- `TerrainProvider`：地形瓦片，例如 heightmap、quantized mesh、自定义 DEM。
- `VectorProvider`：矢量数据，例如 GeoJSON、MVT、WFS、本地业务要素。
- `TilesetProvider`：3D Tiles tileset、content、metadata。
- `ModelProvider`：glTF/glb 模型。
- `ElevationProvider`：点位高程查询、地形采样。
- `PointCloudProvider`：点云数据，例如 LAS/LAZ、3D Tiles pnts、实时点云。
- `TimeSeriesProvider`：时序栅格、时序矢量、动态轨迹、气象海洋时序数据。
- `SensorProvider`：实时传感器、AIS/ADS-B、IoT、遥测流。
- `AnnotationProvider`：标注、测量、绘制、用户编辑数据。

## 最小接口

每个 Provider 至少要明确：

- `id`：稳定标识。
- `type`：imagery、terrain、vector、3d-tiles、model、point-cloud、time-series、sensor、annotation 等。
- `crs` 或 `tileMatrixSet`：数据空间参考。
- `availability`：可用范围、层级、时间范围。
- `request(key, signal)`：异步请求，必须支持取消。
- `parse(response)`：解析网络或本地数据。
- `getMetadata()`：版权、attribution、格式、分辨率、版本。
- `dispose()`：释放缓存、worker、临时资源。

## Tile Key

Tile key 必须是结构化对象，不要只用字符串到处传：

```text
TileKey {
  scheme: "XYZ" | "TMS" | "WMTS" | "Geographic" | "Custom"
  z: number
  x: number
  y: number
  lod?: number
  layer?: string
  time?: string
}
```

必须提供：

- tile key 到 bounds 的函数。
- bounds 到 tile key 的函数。
- key 序列化和反序列化。
- y 轴方向测试。
- 世界边界和反经线测试。

## 请求与取消

- 所有请求必须接收 cancellation signal。
- 相机快速移动后，不可见瓦片的请求应取消或降级优先级。
- 旧请求返回后，必须检查它是否仍属于当前可见状态。
- 网络错误、404、超时、解析失败应分开处理。
- 重试必须有上限和退避策略。

## 缓存边界

建议分层缓存：

- HTTP/browser cache：负责网络层缓存。
- raw data cache：缓存 response 或 ArrayBuffer。
- parsed data cache：缓存解码后的 tile 数据。
- GPU resource cache：缓存 texture、buffer、mesh。

GPU cache 不应由 Provider 私自长期持有，除非架构明确允许。Provider 负责数据，Renderer 负责 GPU 资源更清晰。

## Attribution

Provider 必须暴露 attribution。影像、OSM、地形和商业数据源通常有版权显示要求。AI 实现图层时不得忽略 attribution。

## Worker 解析

大瓦片、DEM、MVT、3D Tiles content、glTF 解码应优先考虑 worker。Worker 设计必须说明：

- 输入输出数据结构。
- transferable object 使用方式。
- 错误回传。
- 取消或丢弃过期任务的机制。
