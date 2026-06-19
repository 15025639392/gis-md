#pragma once

#include "../core/math/Mat4.h"
#include "../core/math/Vec3.h"
#include "Frustum.h"

#include <cstdint>
#include <vector>

namespace earth_engine {

class Camera;

/// 每帧渲染上下文。
/// 由 Scene::update() 计算，传递给各 Layer 和 Renderer。
struct FrameState {
    /// 渲染模式（当前仅 3D Globe）
    enum class Mode { Mode3D };

    uint64_t frameId = 0;
    double timeSeconds = 0.0;   // 自引擎启动以来的秒数
    double deltaSeconds = 0.0;  // 上一帧到本帧的秒数
    Mode mode = Mode::Mode3D;

    const Camera* camera = nullptr;

    /// cesium-native TilesetFrameState::frustums equivalent for selection.
    /// Scene populates the default main-camera view. Empty means no frustums,
    /// matching cesium-native's no-selection update path.
    struct SelectorView {
        Vec3 position = Vec3::zero();
        Vec3 direction = Vec3::zero();
        Frustum frustum;
        Mat4 projectionMatrix;
        int viewportHeightPixels = 0;
    };
    std::vector<SelectorView> selectorViews;

    /// 太阳方向（ECEF 单位向量，地心→太阳），环境系统填充
    struct { float x = 0.35f; float y = 0.45f; float z = 0.82f; } lightDir;

    /// 天空 clear 颜色（RGBA），环境系统填充
    float clearR = 0.02f, clearG = 0.02f, clearB = 0.08f, clearA = 1.0f;

    int viewportWidthPixels = 0;
    int viewportHeightPixels = 0;
    float devicePixelRatio = 1.0f;

    bool hasInteractionFocus = false;
    Vec3 interactionFocusDirection = Vec3::zero();
};

} // namespace earth_engine
