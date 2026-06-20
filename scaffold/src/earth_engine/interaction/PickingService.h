#pragma once

#include "../core/geodesy/Cartographic.h"
#include "../core/math/Vec3.h"
#include <string>
#include <vector>
#include <functional>
#include <optional>

namespace earth_engine {

class Camera;
class VectorLayer;

/// 拾取命中结果。
struct PickResult {
    /// 屏幕坐标（像素）
    float screenX = 0.0f;
    float screenY = 0.0f;

    /// 命中类型
    enum class HitType {
        None,
        Ellipsoid,
        Terrain,
        VectorFeature
    } hitType = HitType::None;

    /// 地形高度（HitType::Terrain 时有效）
    float terrainHeight = 0.0f;

    /// 命中位置（大地坐标，radian/meter）
    Cartographic cartographic;

    /// 命中位置（ECEF，meter）
    Vec3 worldPosition = Vec3::zero();

    /// 图层 ID
    std::string layerId;

    /// Feature ID（VectorLayer 命中时）
    std::string featureId;

    /// 命中距离（从相机到命中点，meter）
    double distance = 0.0;

    bool isValid() const { return hitType != HitType::None; }
};

/// 射线拾取服务。
///
/// 接收屏幕坐标，使用 Camera::getPickRay() 生成射线，
/// 对 VectorLayer features 做相交测试。
///
/// 命中优先级：
///   1. VectorFeature（最近命中优先）
///   2. Ellipsoid（地球表面）
class PickingService {
public:
    PickingService() = default;

    /// 执行拾取。
    /// @param screenXPixels 屏幕 x（像素，左上角原点）
    /// @param screenYPixels 屏幕 y（像素）
    /// @param camera 当前相机
    /// @param viewportWidthPixels 视口宽度
    /// @param viewportHeightPixels 视口高度
    /// @param vectorLayers 参与拾取的矢量图层（只读指针）
    /// @return 最近的命中结果
    PickResult pick(float screenXPixels, float screenYPixels,
                    const Camera& camera,
                    double viewportWidthPixels,
                    double viewportHeightPixels,
                    const std::vector<const VectorLayer*>& vectorLayers) const;

    /// 仅测试椭球命中（无矢量图层）
    PickResult pickEllipsoid(float screenXPixels, float screenYPixels,
                              const Camera& camera,
                              double viewportWidthPixels,
                              double viewportHeightPixels) const;

    /// 带地形高度的椭球拾取。
    /// 命中椭球后，通过 terrainSampler 查询高度。
    /// @param terrainSampler 地形高度查询函数（lngRad, latRad）→ heightMeters
    PickResult pickTerrain(float screenXPixels, float screenYPixels,
                            const Camera& camera,
                            double viewportWidthPixels,
                            double viewportHeightPixels,
                            std::function<float(double,double)> terrainSampler) const;

    /// 射线-三角形相交（cesium-native IntersectionTests::rayTriangle 语义）
    static bool rayTriangleIntersection(const Vec3& rayOrigin,
                                         const Vec3& rayDirection,
                                         const Vec3& v0,
                                         const Vec3& v1,
                                         const Vec3& v2,
                                         double& t);

private:
    /// 射线-椭球相交（WGS84）
    /// @return 交点 ECEF 坐标，无交点返回 std::nullopt。
    static std::optional<Vec3> rayEllipsoidIntersection(
        const Vec3& rayOrigin,
        const Vec3& rayDirection);
};

} // namespace earth_engine
