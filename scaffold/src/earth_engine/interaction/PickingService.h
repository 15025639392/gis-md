#pragma once

#include "../core/geodesy/Cartographic.h"
#include "../core/math/Vec3.h"
#include <string>
#include <vector>
#include <functional>
#include <cstdint>
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

    /// VectorFeature 的生产路径。保留统一 HitType，避免 SelectionManager
    /// 分裂成两套状态机；调用方可用 sourceKind 区分旧 VectorLayer 与
    /// FeatureStore/FeatureRenderLayer。
    enum class FeatureSourceKind {
        None,
        VectorLayer,
        FeatureRenderLayer
    } sourceKind = FeatureSourceKind::None;

    /// 矢量要素的精细命中部位。旧 VectorLayer 当前提供 Edge/Fill，
    /// FeatureRenderLayer 进一步提供 Vertex/Edge/Fill。
    enum class FeaturePart {
        None,
        Vertex,
        Edge,
        Fill
    } featurePart = FeaturePart::None;

    /// 地形高度（HitType::Terrain 时有效）
    float terrainHeight = 0.0f;

    /// 命中位置（大地坐标，radian/meter）
    Cartographic cartographic;

    /// 命中位置（ECEF，meter）
    Vec3 worldPosition = Vec3::zero();

    /// 图层 ID
    std::string layerId;

    /// 统一字符串 Feature ID。旧 VectorLayer 沿用源字符串；
    /// FeatureRenderLayer 使用稳定本地 FeatureId 的十进制字符串。
    std::string featureId;

    /// FeatureRenderLayer 稳定本地 ID；其它命中为 0。
    uint64_t featureNumericId = 0;

    /// 数据源原始 feature.id（若 Feature::sourceId 非空）。它不替代本地
    /// featureId，因为跨瓦片/多几何拆分时源 ID 未必唯一。
    std::string sourceFeatureId;

    /// Vertex/Edge 的环/顶点语义；Fill/旧 VectorLayer 为 -1。
    int ringIndex = -1;
    int vertexIndex = -1;

    /// FeatureRenderLayer 屏幕命中距离（pixel）；Fill 为 0。
    double screenDistancePixels = 0.0;

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

    /// 带地形的射线拾取。
    /// 从相机沿拾取射线做自适应步长行进，返回射线与地形高度场
    /// (terrainSampler: lngRad, latRad → heightMeters) 的第一个交点。命中点
    /// 在射线上且是用户看到的地表——低空朝坡/崖时不再返回"椭球交点+抬升"
    /// 的山后点(那是起手锚点贴眼/拖拽增益崩塌的来源)。射线未碰到地形时
    /// 返回椭球入口点(平地/海面与旧行为一致)；射线碰不到椭球(仰视天空)
    /// 返回 None。
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
