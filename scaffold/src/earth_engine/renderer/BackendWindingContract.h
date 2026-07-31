#pragma once

// 前向面绕序契约——单一事实来源。
//
// 两后端用同一份(不做 y-flip)投影矩阵画同一几何;Metal 的 top-left/y-down
// 帧缓冲原点相对 GL 的 bottom-left/y-up 会反转屏幕空间三角形绕序,因此两侧
// 的 front-face 约定必须**相反**,背面剔除才落在同一几何面上。
//
// 此前这对约定散落在 RenderDeviceGLES.cpp / RenderDeviceMetal.mm 两处硬编码
// 字面量上,只靠对称注释互相解释——单侧改动会静默反转另一侧的剔除。现在两个
// 后端都从这里取常量:要改绕序必须改本文件,"必须相反"由 static_assert 在
// 编译期锁死。
namespace earth_engine::backend_contract {

enum class FrontFaceWinding { Clockwise, CounterClockwise };

// GLES: bottom-left / y-up 帧缓冲原点 → 保持 GL 默认 CCW。
inline constexpr FrontFaceWinding kGlesFrontFace =
    FrontFaceWinding::CounterClockwise;

// Metal: top-left / y-down 帧缓冲原点反转屏幕绕序 → CW 抵消。
inline constexpr FrontFaceWinding kMetalFrontFace =
    FrontFaceWinding::Clockwise;

static_assert(kGlesFrontFace != kMetalFrontFace,
              "GLES/Metal front-face windings must stay opposite: the two "
              "backends share one non-y-flipped projection matrix, and "
              "Metal's y-down framebuffer origin reverses on-screen triangle "
              "winding relative to GL. Unifying them inverts backface "
              "culling on one side.");

}  // namespace earth_engine::backend_contract
