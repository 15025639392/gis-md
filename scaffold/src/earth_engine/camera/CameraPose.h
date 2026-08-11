#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace earth_engine {

/// 位姿值类型 + 与 (heading, pitch, roll, range) 的互转。
///
/// 纯数学,不认识相机对象、地形、手势。`CameraSystem` 用它实现
/// `setViewpoint`/`currentViewpoint`,`TetheredController`(阶段 4)用它把
/// 载体系下的 localHPR 换成世界位姿。
///
/// **约定**(与既有 `headingRadians()`/`pitchRadians()` 逐字一致,换约定会让这两个
/// 已上线的读数在极点静默改变含义):
/// - 参考系三列 = (east, north, up);
/// - heading:0 = 正北,顺时针(向东)为正;
/// - pitch:0 = 水平,+π/2 = 仰视天顶,-π/2 = 正俯视;
/// - roll:绕视线轴,右手正向(拇指指向 direction);
/// - range:eye 到参考系原点的距离,eye = origin − direction·range。
struct CameraPose {
    glm::dvec3 eye{0.0};
    glm::dvec3 direction{0.0, 0.0, 1.0};
    glm::dvec3 up{0.0, 1.0, 0.0};

    /// 某 ECEF 点处的局部 ENU 基(三列 = east/north/up)。
    ///
    /// ⚠️ 极点退化时 east 回退 (1,0,0) —— 这是**故意与
    /// `Transforms::eastNorthUpToFixedFrame` 不同**的:那份在极点取 east=(0,1,0),
    /// 而 `headingFromFrame` 从上线起就用 (1,0,0)。两者在极点外完全等价
    /// (cross(z,up) ≡ normalize(-y,x,0)),只有极点分岔。跟已上线的读数走。
    static glm::dmat3 enuFrameAt(const glm::dvec3& originEcef);

    /// 由参考系内的 (heading, pitch, roll, range) 构造世界位姿。
    /// @param frame 三列 = (右, 前, 上) 的正交基。世界系下传 `enuFrameAt(origin)`。
    static CameraPose fromFrame(const glm::dvec3& originEcef,
                                const glm::dmat3& frame,
                                double headingRadians,
                                double pitchRadians,
                                double rollRadians,
                                double rangeMeters);

    /// 反解到参考系内的 (heading, pitch, roll, range)。
    ///
    /// ⚠️ **万向节**:|pitch| → π/2 时 direction 不再含 heading 信息(绕天顶转不改
    /// 视线),此时改用 `up` 的水平分量定 heading 并令 roll = 0 —— 与
    /// `headingFromFrame` 近正俯视时的兜底同一套。代价是那里 (heading, roll) 本就
    /// 只有和是可观测的,任何分解都得挑一个;挑 roll=0 让「正俯视 + 转指北针」这条
    /// 主路径的往返是恒等的。
    void toFrame(const glm::dvec3& originEcef,
                 const glm::dmat3& frame,
                 double& outHeadingRadians,
                 double& outPitchRadians,
                 double& outRollRadians,
                 double& outRangeMeters) const;
};

} // namespace earth_engine
