# 离线与打包策略

地球引擎不能假设用户始终在线。移动端用户可能在飞行模式、地铁、偏远地区或弱网环境下使用地图。离线能力是区分原型和生产级引擎的关键特征。

本文件定义离线数据包格式、缓存策略、同步机制和在线/离线过渡。必须与 `data-provider-contracts.md` 和 `basemap-tile-rendering.md` 配合使用。

## 离线支持等级

| 等级 | 描述 | 适用场景 |
|------|------|----------|
| L0：在线优先 | 无离线支持。网络断开时显示缓存中已有的瓦片，不做预加载。 | MVP 初期 |
| L1：被动缓存 | 用户浏览过的区域自动缓存。重新访问已浏览区域时从缓存加载，无需网络。 | 日常使用，减少流量消耗 |
| L2：区域预加载 | 用户指定一个或多个区域提前下载。支持后台下载和下载进度显示。 | 出差、野外作业、应急场景 |
| L3：全离线包 | 预制的离线数据包（MBTiles、PMTiles、GeoPackage），可批量导入。 | 专业用户、企业部署 |

## 离线数据包格式

### MBTiles

- 格式：SQLite 数据库，单文件。
- 适用：底图瓦片（XYZ raster tiles）。
- 标准：https://github.com/mapbox/mbtiles-spec
- 必须支持：`tiles` 表（`zoom_level`, `tile_column`, `tile_row`, `tile_data`）。
- 可选支持：`metadata` 表（bounds、minzoom、maxzoom、attribution）。
- 注意事项：MBTiles 使用 TMS y 轴（与 XYZ 的 y 翻转关系需在 TileScheme 中处理）。

### PMTiles

- 格式：单文件归档，HTTP range request 友好。
- 适用：不需要完整下载即可按需读取（partial download）。
- 标准：https://github.com/protomaps/PMTiles
- 注意事项：本地 PMTiles 文件可以直接 mmap 读取，无需先解压到目录。

### GeoPackage

- 格式：SQLite 数据库，OGC 标准。
- 适用：矢量数据（feature table）、栅格瓦片、高程数据。
- 标准：https://www.geopackage.org/
- 必须支持：`gpkg_contents`、`gpkg_tile_matrix`、`gpkg_tile_matrix_set` 表。
- 注意事项：GeoPackage 通常使用 geographic tiling（EPSG:4326），与 Web Mercator XYZ 的 tile scheme 不同。

### 自定义离线包

如果上述标准格式不能满足需求（例如压缩策略、加密或增量更新），可自定义格式。必须记录：

- 文件布局和版本。
- tile index 结构（如何从 z/x/y 定位到文件偏移）。
- 压缩算法（gzip、brotli、lz4）。
- checksum 算法（SHA-256）。
- 加密（如有）。

## 离线缓存架构

```text
缓存层级（从快到慢）：

1. 内存 LRU Cache
   - 当前帧需要的 tile raw data + decoded pixels
   - 上限：~50 MB（移动端）
   - 淘汰策略：LRU + 可见 tile 保护

2. 磁盘 SQLite Cache
   - 存储 raw response bytes（PNG/JPEG/WebP bytes）
   - 上限：~200 MB（可配置）
   - 淘汰策略：LRU per provider，全局配额管理

3. 离线包（MBTiles / PMTiles / GeoPackage）
   - 只读或可增量更新
   - 存储配额由用户管理（下载即占用直到手动删除）

4. GPU Texture Cache
   - RenderDevice 管理的 GPU 纹理
   - 上限由 RenderDevice 显存预算决定
   - 淘汰策略：不可见 tile 优先释放
```

## 离线 Provider

离线数据通过 `OfflineProvider` 桥接入引擎：

```cpp
class OfflineImageryProvider : public ImageryProvider {
public:
    // 从磁盘缓存或离线包提供瓦片
    std::vector<uint8_t> loadTile(const TileKey& key, bool& fromCache) override;

    // 离线包元数据
    OfflinePackageInfo packageInfo() const;
};

struct OfflinePackageInfo {
    std::string name;
    std::string format;      // "mbtiles" | "pmtiles" | "geopackage" | "custom"
    std::string crsProfile;
    int minZoom;
    int maxZoom;
    Rectangle bounds;        // 覆盖范围
    int64_t tileCount;       // 瓦片总数
    int64_t sizeBytes;       // 文件大小
    std::string version;
    std::string checksumSha256;
    std::string attribution;
};
```

## 区域预下载

### 用户流程

1. 用户选择区域（矩形或自定义多边形）。
2. 选择 zoom 范围（minZoom ~ maxZoom）。
3. 引擎计算需要下载的 tile 总数和预估大小。
4. 用户确认后开始后台下载。
5. 显示下载进度和剩余时间估算。
6. 下载完成后标记区域为"offline-ready"。

### 后台下载

- iOS: 使用 `BGTaskScheduler`（`BGProcessingTask`）或 `URLSession background configuration`。
- Android: 使用 `WorkManager`（`OneTimeWorkRequest` 或 `PeriodicWorkRequest`）。
- 下载中断（网络切换、应用被杀）后可断点续传。
- 完成通知：本地推送通知。

### Tile 计数和大小预估

```cpp
struct DownloadEstimate {
    int64_t totalTiles;
    int64_t estimatedBytes;  // 假设每 tile 15 KB（压缩后）
    int64_t storageNeeded;   // 含 SQLite 索引开销（+15%）
};
```

下载前调用 `estimateTileCount(rectangle, minZoom, maxZoom, tileScheme)` 计算。

## 在线/离线过渡

### 网络状态检测

- 通过 PlatformBridge 暴露网络状态：
  - `NetworkStatus::Online`（WiFi 或蜂窝网络可用）。
  - `NetworkStatus::Metered`（仅蜂窝网络，限流量场景）。
  - `NetworkStatus::Offline`（无网络连接）。

- Android: `ConnectivityManager` + `NetworkCallback`。
- iOS: `NWPathMonitor`。

### 过渡策略

```text
Online → Offline:
  1. 取消所有待处理的网络请求。
  2. DataProvider 切换到离线模式（从缓存和离线包加载）。
  3. 不在离线包/缓存中的 tile 显示空或 parent fallback。
  4. 显示状态指示"离线模式"。

Offline → Online:
  1. 恢复网络请求队列。
  2. 刷新当前视域中缺失的在线 tile。
  3. 不自动移除离线缓存。
```

### 计量网络策略

当网络类型为 `Metered` 时（Android: `isActiveNetworkMetered()`）：

- 不自动预取（禁止 speculative pre-loading）。
- 降低最大并发请求数（如从 6 降至 2）。
- 降低纹理质量（如使用 256px tile 而非 512px）。
- 不自动更新离线包。
- 用户可手动忽略计量限制。

## 离线包更新

- 版本检查：离线包的 `version` 与远程清单对比。
- 增量更新：仅下载变更的 tile（如果有 delta manifest）。
- 全量重新下载：如果增量不可用，提示用户重新下载完整包。
- 更新不阻塞当前使用：旧版本在下载新版本期间仍可用，下载完成后原子替换。

## 存储配额管理

```cpp
class StorageQuotaManager {
public:
    // 当前使用量
    int64_t totalBytes() const;
    int64_t cacheBytes() const;       // 自动缓存
    int64_t packageBytes() const;     // 离线包

    // 淘汰
    void evictToTarget(int64_t targetBytes);
    void evictOldestPackages(int count);
    void evictByProvider(const std::string& providerId);

    // 平台存储信息
    int64_t availableSpace() const;       // 系统剩余空间
    int64_t quotaLimit() const;           // 应用配额上限
};
```

系统剩余空间 < 500 MB 时停止下载新离线包，并警告用户。

## 验收清单

- 网络断开后，已浏览区域的瓦片从缓存正常加载。
- 区域预下载完整体现在进度条和完成通知中。
- MBTiles 离线包导入后瓦片位置正确（TMS y 翻转验证通过）。
- 在线/离线过渡不出现崩溃或数据丢失。
- 计量网络下预取被禁止，并发请求数降低。
- 存储配额达到上限时触发淘汰，不崩溃。
- 后台下载在应用被杀后从断点恢复。
- 离线包更新不阻塞当前地图使用。
