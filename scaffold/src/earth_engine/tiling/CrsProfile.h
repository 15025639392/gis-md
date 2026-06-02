#pragma once

#include <string>
#include <memory>
#include <array>

namespace earth_engine {

/// CRS（Coordinate Reference System）体系描述。
/// 定义坐标轴方向、bounds、单位，供 TileScheme 关联。
/// 不实现坐标转换——转换由 Transforms 和 TileScheme 负责。
class CrsProfile {
public:
    virtual ~CrsProfile() = default;

    /// 唯一标识（如 "EPSG:3857"、"EPSG:4326"）
    virtual std::string id() const = 0;

    /// 简短别名（WebMercator、GeographicWGS84 等）
    virtual std::string name() const = 0;

    /// 坐标单位
    enum class Unit { Degree, Meter };
    virtual Unit unit() const = 0;

    /// 是否为地理坐标系（经纬度）
    bool isGeographic() const { return unit() == Unit::Degree; }

    /// 是否为投影坐标系（米）
    bool isProjected() const { return unit() == Unit::Meter; }

    /// 有效经度/X 范围（native units）
    virtual std::array<double, 2> xRange() const = 0;

    /// 有效纬度/Y 范围（native units）
    virtual std::array<double, 2> yRange() const = 0;

    /// X/Y 轴正方向：true = 东/北递增，false = 西/南递增
    virtual bool xIncreasesEast() const { return true; }
    virtual bool yIncreasesNorth() const { return true; }

    // ---- 预置 CRS ----

    /// EPSG:3857 Web Mercator（米，球面 Mercator 投影）
    static const CrsProfile& webMercator();

    /// EPSG:4326 WGS84 地理坐标系（度）
    static const CrsProfile& wgs84Geographic();
};

} // namespace earth_engine
