#pragma once

#include "../core/math/Vec3.h"
#include "SelectorView.h"

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

    std::vector<SelectorView> selectorViews;

    /// 太阳方向（ECEF 单位向量，地心→太阳），环境系统填充
    struct { float x = 0.35f; float y = 0.45f; float z = 0.82f; } lightDir;

    /// 天空 clear 颜色（RGBA），环境系统填充
    float clearR = 0.02f, clearG = 0.02f, clearB = 0.08f, clearA = 1.0f;

    /// 天空环境光颜色（RGB），SkyGradient 求解，喂给地表/glTF 着色作填充光。
    /// 消除只有单方向光时背光面死黑。
    struct { float r = 0.0f; float g = 0.0f; float b = 0.0f; } ambient;

    int viewportWidthPixels = 0;
    int viewportHeightPixels = 0;
    float devicePixelRatio = 1.0f;

    bool hasInteractionFocus = false;
    Vec3 interactionFocusDirection = Vec3::zero();

    /// 程序化飞行契约(CameraSystem 填)。
    ///
    /// ⚠️ **这两个字段存在的唯一理由**:`TileFrameInteractionTracker` 判
    /// `cameraMoving` 纯按逐帧位移、不区分驱动源,飞行期每帧位移上千米 ⇒ 恒真
    /// ⇒ `cullRequestsWhileMoving` 全程延迟请求 ⇒ **飞到目的地画面是空的**。
    /// 减速段据 `cameraFlightProgress` 关掉 cull,让目的地瓦片提前进队。
    /// Cesium 用 `Camera.canPreloadFlight()` 解同一件事。
    bool cameraFlightActive = false;
    /// 归一化时间进度 [0,1](未过缓动)。
    double cameraFlightProgress = 0.0;
};

} // namespace earth_engine
