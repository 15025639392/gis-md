#pragma once

#include <string>

namespace earth_engine {

/// 内置矢量符号形状(矢量 P6c 图标,设计 §11)。
///
/// 走解析 SDF(fragment 里按形状 id 算距离场),任意屏幕尺寸都锐利、零外部
/// 资源;应用层要画美术图标则注入位图走 IconAtlas 通道(见 FeatureRenderLayer
/// 的 pointImage 解析:名字命中内置表 → 本枚举,否则查图集 frame)。
///
/// **契约**:这里的整数值必须与 Renderer.cpp 的 vectorPoint fragment shader
/// 分支一一对应,改一处必须改另一处(shader 里同样有此注记)。
enum class SymbolShape : int {
    Circle = 0,    ///< 圆(P5a 既有点符号/编辑手柄的形状,默认值)
    Square = 1,    ///< 正方形
    Triangle = 2,  ///< 正三角(尖朝上)
    Diamond = 3,   ///< 菱形
    Star = 4,      ///< 五角星
    Pin = 5,       ///< 水滴形图钉(**底尖锚定**:整形画在锚点上方)
};

/// 图集通道哨兵:顶点 shape 分量为负 → fragment 走位图采样而非解析 SDF。
constexpr float kSymbolShapeAtlas = -1.0f;

/// 符号图形相对锚点的竖直对齐。
enum class SymbolAnchor {
    Auto,    ///< pin 底部对齐,其余(含位图图标)居中
    Center,  ///< 图形中心压锚点
    Bottom,  ///< 图形底边压锚点(整形画在锚点上方,锚点 = "落点")
};

/// 名字 → 内置形状。未命中返回 false(调用方据此转查 IconAtlas)。
inline bool symbolShapeFromName(const std::string& name, SymbolShape* out) {
    SymbolShape s;
    if (name == "circle") {
        s = SymbolShape::Circle;
    } else if (name == "square") {
        s = SymbolShape::Square;
    } else if (name == "triangle") {
        s = SymbolShape::Triangle;
    } else if (name == "diamond") {
        s = SymbolShape::Diamond;
    } else if (name == "star") {
        s = SymbolShape::Star;
    } else if (name == "pin") {
        s = SymbolShape::Pin;
    } else {
        return false;
    }
    if (out) *out = s;
    return true;
}

/// 该形状是否底尖锚定(quad 画在锚点正上方,而非以锚点为中心)。
inline bool symbolShapeIsBottomAnchored(SymbolShape s) {
    return s == SymbolShape::Pin;
}

} // namespace earth_engine
