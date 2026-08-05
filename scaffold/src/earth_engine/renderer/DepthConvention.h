#pragma once

// 深度约定 —— 单一事实来源。
//
// 本引擎用 reverse-Z:投影矩阵把近平面映到深度 1、远平面映到 0(见
// Transforms::createPerspectiveMatrix,对齐 cesium-native)。float 的尾数在
// 0 附近最密,把"远"放在 0 让远景拿到最高精度,是高空 z-fighting 的根治手段。
//
// 问题在于这个约定有**六个散落的消费者**,两个后端各三个:深度清除值、深度比较
// 函数、polygon offset 符号。此前它们全是各自硬编码的字面量,只靠平行注释互相
// 解释 —— 与 winding 收归前(见 BackendWindingContract.h)完全同构的形态。
//
// 而且这个坑真的踩过:blend 路径的 glPolygonOffset(-1,-1) 是 pre-reverse-Z
// (传统 LEQUAL)时代的遗留,切到 reverse-Z 后方向恰好反了 —— 本该把描边拉向
// 观察者,实际把它推得更远,于是贴地线被地形埋掉。当时靠真机 A/B 才翻出来,
// 因为没有任何机制能指出"这个符号与当前深度约定不一致"。
//
// 所以这里的关键不是把常量集中,是**让下游全部派生而非各自声明**:
// 比较方向与 offset 符号都是 kNearDepth/kFarDepth 的 constexpr 函数,翻转约定
// 只需改这两个值,六个消费者自动跟随,不存在"grep 漏一个"的可能。
namespace earth_engine::depth_convention {

/// 近平面映到的深度值。reverse-Z 下 = 1。
inline constexpr float kNearDepth = 1.0f;
/// 远平面映到的深度值。reverse-Z 下 = 0。
inline constexpr float kFarDepth = 0.0f;

/// 深度缓冲的清除值 = 最远处。清成"近"会让所有片元被深度测试拒绝(整屏空)。
inline constexpr float kClearDepth = kFarDepth;

/// 约定方向:近处的深度值是否大于远处。下面两项全部由它派生。
inline constexpr bool kNearIsGreaterDepth = kNearDepth > kFarDepth;

/// 深度比较函数。"通过 = 新片元比已有的更近"在两种约定下是相反的比较符。
enum class DepthCompare { GreaterEqual, LessEqual };
inline constexpr DepthCompare kDepthCompare = kNearIsGreaterDepth
    ? DepthCompare::GreaterEqual
    : DepthCompare::LessEqual;

/// 把片元**拉向观察者**所需的 polygon offset 符号。
///
/// reverse-Z 下"更近"= 深度值更大 → 正 offset。传统 LEQUAL 下反之。消费方写
/// glPolygonOffset(kTowardViewerOffsetSign * factor, kTowardViewerOffsetSign *
/// units),不要再自己写正负号。
inline constexpr float kTowardViewerOffsetSign = kNearIsGreaterDepth ? 1.0f : -1.0f;

// 派生量之间的一致性由构造保证,这里只锁住"两个源值本身没退化成相等"——
// 相等会让 kNearIsGreaterDepth 变成 false 并静默翻转下游全部三项。
static_assert(kNearDepth != kFarDepth,
              "near/far depth values must differ: they are the only inputs "
              "from which the compare direction and the polygon-offset sign "
              "are derived, and making them equal silently flips both.");

}  // namespace earth_engine::depth_convention
