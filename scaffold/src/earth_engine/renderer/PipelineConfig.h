#pragma once

namespace earth_engine {

// ============================================================
// 渲染管线编译期开关(T2 HDR 切片)
// ============================================================
//
// kEnableHdrPipeline:场景画进线性 HDR(RGBA16F)离屏靶,末端单次 PBR-Neutral
// tonemap→8bit。**默认 false = 现状零变化**(逐 shader sRGB encode 直绘 8bit)。
//
// 编译期常量而非运行时 flag:它同时驱动 ①场景 shader 的输出空间(HDR 下地形
// 等输出**线性**、不做 per-shader linearToSrgb)与 ②Engine 的离屏靶格式 +
// tonemap 终端 pass —— 二者必须一致(shader 输出空间 ≠ 靶/终端 → 观感全错),
// 用同一 constexpr 保证不分叉。A/B = 翻此值重编。
//
// ⚠️ T2 切片先行 GLES(Metal PSO 像素格式烘死是最大风险,见
//    docs/issues/lighting-color-pipeline-architecture-2026-08-12.md §9.1)。
// ⚠️ 开启需 RGBA16F float-color-renderable(GLES 探 EXT_color_buffer_half_float,
//    缺失 createFramebuffer 回落 RGBA8 → 那时输出会偏暗,属预期回落非 bug)。
constexpr bool kEnableHdrPipeline = false;

} // namespace earth_engine
