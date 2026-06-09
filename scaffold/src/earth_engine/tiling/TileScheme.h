#pragma once

#include "TileKey.h"
#include "../core/math/Rectangle.h"
#include <memory>
#include <string>
#include <vector>

namespace earth_engine {

class CrsProfile;

/// 瓦片体系抽象。
/// 定义 tile ↔ bounds 转换、zoom 范围、y 轴方向、关联 CRS。
/// 具体实现：XYZWebMercator、TMS、WMTS、Geographic、Baidu 等。
class TileScheme {
public:
    virtual ~TileScheme() = default;

    virtual std::string id() const = 0;

    /// 关联的 CRS 体系
    virtual const CrsProfile& crs() const = 0;

    /// CRS profile 标识（便捷方法，等价于 crs().id()）
    virtual std::string crsProfile() const = 0;
    virtual int tileSize() const = 0;
    virtual int minZoom() const = 0;
    virtual int maxZoom() const = 0;

    /// y 轴方向："down"（XYZ，北→南递增）或 "up"（TMS，南→北递增）
    virtual std::string yDirection() const = 0;

    /// tile key → 地理矩形（radian）
    virtual Rectangle tileToRectangle(const TileKey& key) const = 0;

    /// 地理坐标 → tile key
    virtual TileKey positionToTile(double lngRad, double latRad, int zoom) const = 0;

    /// 给定 zoom，计算覆盖矩形所需的 tile 范围
    virtual void tileRange(const Rectangle& rect, int zoom,
                           int& minX, int& minY, int& maxX, int& maxY) const = 0;

    /// 给定 zoom 的分辨率（弧度/tile）
    virtual double levelResolution(int zoom) const = 0;

    /// 创建标准 XYZ Web Mercator 瓦片体系
    static std::unique_ptr<TileScheme> createXYZWebMercator();

    /// 创建 TMS（Tile Map Service）Web Mercator 瓦片体系
    /// y 轴方向为 "up"（南→北），与 XYZ 相反
    static std::unique_ptr<TileScheme> createTMS();

    /// 创建 OpenGlobus Earth 三分区瓦片体系。
    /// y 编码：
    ///   main Mercator group: [0, 2^z)
    ///   north polar LonLat group: [2^z, 2*2^z)
    ///   south polar LonLat group: [2*2^z, 3*2^z)
    static std::unique_ptr<TileScheme> createOpenGlobusEarth();

    /// Geographic (EPSG:4326) TMS tiling.
    /// x = floor((lng + 180) / 360 * 2^z)
    /// y = floor((90 - lat) / 180 * 2^z)  (TMS: y=0 at south pole)
    static std::unique_ptr<TileScheme> createGeographicTMS();
};

} // namespace earth_engine
