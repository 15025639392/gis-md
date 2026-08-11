#pragma once

namespace earth_engine {

/// 相机控制器族的基类契约:**一个每帧驱动相机的东西**。
///
/// 成员只有生命周期与时间,**没有输入**。这是刻意的:
///
/// Skybolt 的 `CameraController::setInput` 收一个归一化速率结构
/// (`forwardSpeed`/`yawRate`/`zoomRate`/modifier),对飞行模拟式的速率控制成立,
/// 但对我们**直接操纵**式的锚点钉合是错的——「把抓住的地表点放回手指那个像素」
/// 所需的全部信息就是那个像素坐标,归一化成速率恰好把它丢掉,而那正是我们相对
/// 基准领先的部分(cesium 低空 `pan3D` 是屏幕空间启发式带经验因子)。
///
/// 反过来,各控制器的输入形状本就不同:Free 吃触摸事件、Tethered 吃「绕载体转」
/// 的同名手势但语义不同、桌面吃滚轮/中键/方向键、Flight 根本不吃输入。硬凑一个
/// 公共输入接口只会得到一个谁都要现场解释的联合体。
///
/// ⇒ **输入路由到具体类型**,靠 `CameraControllerSelector::activeAs<T>()`
/// (Skybolt 自己也是这么逃生的:`getControllerOfType<T>()`)。
///
/// 实现者必须保证:`tick` 里对相机的任何写入都已过约束出口
/// (`CameraConstraintSolver::constrainEye`),否则只能靠 `CameraSystem` 的帧末
/// 哨兵兜底——那是兜底,不是设计。
class ICameraController {
public:
    virtual ~ICameraController() = default;

    /// 接管:从**当前相机位姿**初始化自身状态,使切换零跳变。
    ///
    /// ⚠️ 架构文档写的是 `onActivate(currentWorldPose)`,这里不带参:每个控制器
    /// 本就持有 `Camera*`(不持有就没法驱动它),位姿从那里读即可,再传一份是纯
    /// 冗余。契约不变——"接管时把自己对齐到当前位姿"。
    virtual void onActivate() {}

    /// 交出:清掉不该跨控制器存活的瞬时状态(惯性、手势中间量)。
    virtual void onDeactivate() {}

    /// 每帧时间步进。只有**被选中**的控制器会收到。
    virtual void tick(double deltaSeconds) = 0;

    /// 是否仍在自行演进(与外部输入无关的持续变化)。帧级按需渲染据此判定
    /// 「停手之后还得再画几帧」。
    virtual bool isAnimating() const = 0;

    /// 视口尺寸变更。**广播给所有控制器**(不只是活动的)——视口是渲染表面的
    /// 属性,与"谁在驱动"无关,未激活的控制器也得保持正确否则接管瞬间用错增益。
    virtual void setViewport(int widthPixels, int heightPixels) {
        (void)widthPixels;
        (void)heightPixels;
    }
};

} // namespace earth_engine
