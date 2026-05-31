# GIS 可信来源

优先使用官方标准和官方库文档。不要把大段外部文档复制进仓库；本文件只记录可信入口和对本项目的含义。

## 核心标准

- GeoJSON: RFC 7946, "The GeoJSON Format"  
  来源：https://www.rfc-editor.org/rfc/rfc7946  
  项目含义：GeoJSON 坐标位置使用 longitude, latitude 顺序。本项目内部可简写为 `[lng, lat]`。除非另有明确说明，GeoJSON 基于 WGS 84 经纬度。

- OGC Standards  
  来源：https://www.ogc.org/standards/  
  用途：WMS、WMTS、WFS、GeoPackage、Simple Features、OGC API Features 等互操作标准问题。

- EPSG Geodetic Parameter Dataset  
  来源：https://epsg.org/  
  用途：CRS 编号、坐标轴定义、基准面定义、坐标操作元数据。

## 坐标转换与地理空间库

- PROJ documentation  
  来源：https://proj.org/  
  用途：坐标转换、CRS 定义、datum shift、投影行为。

- GDAL/OGR documentation  
  来源：https://gdal.org/  
  用途：栅格/矢量 IO、格式驱动、warping、重投影、命令行为。

- GEOS documentation  
  来源：https://libgeos.org/  
  用途：平面几何谓词和操作，例如 intersection、union、contains、buffer。

- Shapely documentation  
  来源：https://shapely.readthedocs.io/  
  用途：Python 几何操作，底层基于 GEOS。除非文档明确说明，否则把 Shapely 操作视为平面几何操作。

- Turf.js documentation  
  来源：https://turfjs.org/  
  用途：JavaScript GeoJSON 操作。每个函数都要确认单位和球面/平面假设。

## 空间数据库

- PostGIS documentation  
  来源：https://postgis.net/documentation/  
  用途：`geometry` 与 `geography` 区别、空间索引、`ST_Distance`、`ST_DWithin`、`ST_Intersects`、`ST_Transform`、几何合法性函数。

- PostgreSQL documentation  
  来源：https://www.postgresql.org/docs/  
  用途：查询计划、索引、扩展、非 PostGIS 专属的数据库行为。

## Web 地图与瓦片

- Mapbox Vector Tile specification  
  来源：https://github.com/mapbox/vector-tile-spec  
  用途：矢量瓦片编码、tile extent、layer、geometry command。

- Slippy map tilenames / Web Mercator tile scheme reference  
  来源：https://wiki.openstreetmap.org/wiki/Slippy_map_tilenames  
  用途：XYZ 瓦片地址的实用参考。生产行为仍需以当前使用的地图 SDK 或瓦片服务文档为准。

- OpenStreetMap tile usage policy  
  来源：https://operations.osmfoundation.org/policies/tiles/  
  用途：使用公共 OSM 瓦片时的限制和规范。

## 3D 地球、地形与场景

- CesiumJS documentation  
  来源：https://cesium.com/learn/cesiumjs/ref-doc/  
  用途：3D globe、相机、terrain provider、imagery layer、坐标转换、场景渲染 API 的工程参考。

- 3D Tiles specification  
  来源：https://github.com/CesiumGS/3d-tiles  
  OGC 入口：https://www.ogc.org/standard/3dtiles/  
  用途：大规模 3D 地理空间内容的分块、层级、bounding volume、geometric error、tileset JSON、content 格式。

- glTF specification  
  来源：https://github.com/KhronosGroup/glTF  
  用途：3D 模型资产格式、PBR 材质、坐标、buffer、mesh、texture。

- Quantized Mesh terrain format  
  来源：https://github.com/CesiumGS/quantized-mesh  
  用途：地形瓦片网格、顶点量化、高程、边界裙边 skirt、terrain LOD。

- WebGL specification  
  来源：https://registry.khronos.org/webgl/specs/latest/  
  用途：浏览器 GPU 渲染能力、精度限制、shader、texture、buffer、上下文行为。

- WebGPU specification  
  来源：https://www.w3.org/TR/webgpu/  
  用途：新一代浏览器 GPU API。采用前需确认目标浏览器支持情况。

- WGS 84 / NGA geodesy references  
  来源：https://earth-info.nga.mil/  
  用途：WGS 84、地球参考系、椭球参数和大地测量背景资料。

## 本项目的来源使用规则

如果上面的来源和本仓库实际使用的库版本存在差异，优先查“当前安装版本”的官方文档，并把版本差异记录在本文件或相关实现文档中。
