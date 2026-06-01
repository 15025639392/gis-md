#pragma once

#include "TileKey.h"
#include "../core/math/Rectangle.h"
#include <memory>
#include <string>
#include <vector>

namespace earth_engine {

/// 瓦片体系抽象。
/// 定义 tile ↔ bounds 转换、zoom 范围、y 轴方向。
/// 具体实现：XYZWebMercator、TMS、WMTS、Geographic、Baidu 等。
class TileScheme {
public:
    virtual ~TileScheme() = default;

    virtual std::string id() const = 0;
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
};

} // namespace earth_engine
