#include "RenderCommand.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace earth_engine {
namespace {

bool fail(size_t index,
          const RenderCommand& cmd,
          const std::string& message,
          std::optional<RenderCommandValidationError>& out) {
    out = RenderCommandValidationError{index, cmd.owner, message};
    return false;
}

bool requireColorPass(size_t index,
                      const RenderCommand& cmd,
                      std::optional<RenderCommandValidationError>& out) {
    if (cmd.pass == "color") return true;
    return fail(index, cmd, "MVP surface/basemap commands must use color pass", out);
}

bool requireState(size_t index,
                  const RenderCommand& cmd,
                  bool depthTest,
                  bool depthWrite,
                  bool cullFace,
                  bool blend,
                  const char* label,
                  std::optional<RenderCommandValidationError>& out) {
    if (cmd.depthTest != depthTest) {
        return fail(index, cmd, std::string(label) + " depthTest violates MVP fixed state", out);
    }
    if (cmd.depthWrite != depthWrite) {
        return fail(index, cmd, std::string(label) + " depthWrite violates MVP fixed state", out);
    }
    if (cmd.cullFace != cullFace) {
        return fail(index, cmd, std::string(label) + " cullFace violates MVP fixed state", out);
    }
    if (cmd.blend != blend) {
        return fail(index, cmd, std::string(label) + " blend violates MVP fixed state", out);
    }
    return true;
}

float uniformScalar(const RenderCommand& cmd, const std::string& name, float fallback) {
    if (cmd.kind == RenderCommandKind::SurfaceTile && cmd.hasSurfaceTileUniforms) {
        if (name == "u_tileOpacity") return cmd.surfaceTileOpacity;
        if (name == "u_transitionOpacity") return cmd.surfaceTransitionOpacity;
    }
    if (cmd.hasGltfUniforms) {
        if (name == "u_alphaMode") return cmd.gltfUniforms.alphaMode;
        if (name == "u_transmissionFactor") {
            return cmd.gltfUniforms.transmissionFactor;
        }
        if (name == "u_renderOpacity") return cmd.gltfUniforms.renderOpacity;
    }
    auto it = cmd.uniforms.find(name);
    if (it == cmd.uniforms.end() || it->second.empty()) {
        return fallback;
    }
    return it->second.front();
}

bool surfaceTileBlendAllowed(const RenderCommand& cmd) {
    if (cmd.instanceCount > 0) {
        return true;
    }
    if (cmd.hasSurfaceTileUniforms) {
        return cmd.surfaceTileOpacity < 0.999f ||
               cmd.surfaceTransitionOpacity < 0.999f;
    }
    const float tileOpacity = uniformScalar(cmd, "u_tileOpacity", 1.0f);
    const float transitionOpacity = uniformScalar(cmd, "u_transitionOpacity", 1.0f);
    return tileOpacity < 0.999f || transitionOpacity < 0.999f;
}

bool gltfPrimitiveBlendAllowed(const RenderCommand& cmd) {
    const bool blendMaterial = uniformScalar(cmd, "u_alphaMode", 0.0f) > 1.5f;
    const bool transmissiveMaterial =
        uniformScalar(cmd, "u_transmissionFactor", 0.0f) > 0.0f;
    return blendMaterial ||
           transmissiveMaterial ||
           uniformScalar(cmd, "u_renderOpacity", 1.0f) < 0.999f;
}

bool isGltfPrimitiveCommand(const RenderCommand& cmd) {
    return cmd.kind == RenderCommandKind::GltfPrimitive ||
           cmd.kind == RenderCommandKind::GltfPrimitiveInstanced;
}

bool isTranslucentGltfPrimitiveCommand(const RenderCommand& cmd) {
    return isGltfPrimitiveCommand(cmd) && cmd.blend;
}

bool mvpCommandLess(const RenderCommand& a, const RenderCommand& b) {
    const int orderA = mvpRenderOrder(a.kind);
    const int orderB = mvpRenderOrder(b.kind);
    if (orderA != orderB) {
        return orderA < orderB;
    }

    const bool translucentA = isTranslucentGltfPrimitiveCommand(a);
    const bool translucentB = isTranslucentGltfPrimitiveCommand(b);
    if (translucentA != translucentB) {
        return !translucentA && translucentB;
    }
    if (!translucentA) {
        return false;
    }

    if (a.hasTranslucentSortDepth != b.hasTranslucentSortDepth) {
        return a.hasTranslucentSortDepth;
    }
    if (!a.hasTranslucentSortDepth) {
        return false;
    }
    return a.translucentSortDepth > b.translucentSortDepth;
}

bool surfaceTileCullStateAllowed(const RenderCommand& cmd) {
    // Terrain-primary Quantized Mesh tiles may include skirts and mixed LOD
    // borders whose visibility relies on two-sided rendering. Other passes keep
    // their fixed cull state; only SurfaceTile permits this opt-out.
    return cmd.cullFace || cmd.hasSurfaceTileUniforms;
}

bool terrainPrimaryOverlayStateAllowed(const RenderCommand& cmd) {
    return cmd.owner == "terrain_primary_surface" &&
           cmd.hasSurfaceTileUniforms &&
           !cmd.depthTest &&
           !cmd.depthWrite &&
           !cmd.cullFace &&
           !cmd.blend;
}

} // namespace

int mvpRenderOrder(RenderCommandKind kind) {
    switch (kind) {
        case RenderCommandKind::SkyBackground:
            return 0;
        case RenderCommandKind::SurfaceTile:
            return 10;
        case RenderCommandKind::GltfPrimitive:
        case RenderCommandKind::GltfPrimitiveInstanced:
            return 15;
        case RenderCommandKind::AtmosphereBackground:
            return 20;
        case RenderCommandKind::VectorStencil:
            return 29;  // 分类 fill 压在其它矢量之下(色 pass 关深度测)
        case RenderCommandKind::VectorOverlay:
        case RenderCommandKind::VectorFill:
        case RenderCommandKind::VectorLine:
        case RenderCommandKind::VectorPoint:
            return 30;
        case RenderCommandKind::VectorLabel:
            return 31;  // 文字压其它矢量之上
        case RenderCommandKind::Unknown:
        default:
            return 100;
    }
}

std::optional<RenderCommandValidationError>
validateMvpRenderCommands(const RenderCommandList& commands,
                          uint64_t expectedFrameId) {
    std::optional<RenderCommandValidationError> error;
    int lastOrder = -1;
    bool sawTranslucentGltf = false;
    double lastTranslucentGltfDepth = std::numeric_limits<double>::infinity();

    for (size_t i = 0; i < commands.size(); ++i) {
        const RenderCommand& cmd = commands[i];
        const int order = mvpRenderOrder(cmd.kind);
        if (order < lastOrder) {
            fail(i, cmd, "RenderCommand order violates MVP pass order", error);
            return error;
        }
        lastOrder = order;

        switch (cmd.kind) {
            case RenderCommandKind::SurfaceTile:
                if (!requireColorPass(i, cmd, error)) return error;
                if (terrainPrimaryOverlayStateAllowed(cmd)) {
                    if (expectedFrameId != 0 && cmd.frameId != expectedFrameId) {
                        fail(i, cmd, "SurfaceTile frameId is stale for current FrameState", error);
                        return error;
                    }
                    if (cmd.generation == 0) {
                        fail(i, cmd, "SurfaceTile generation must be non-zero", error);
                        return error;
                    }
                    break;
                }
                if (cmd.depthTest != true) {
                    fail(i, cmd, "SurfaceTile depthTest violates MVP fixed state", error);
                    return error;
                }
                if (cmd.depthWrite != true) {
                    fail(i, cmd, "SurfaceTile depthWrite violates MVP fixed state", error);
                    return error;
                }
                if (!surfaceTileCullStateAllowed(cmd)) {
                    fail(i, cmd, "SurfaceTile cullFace violates MVP fixed state", error);
                    return error;
                }
                if (cmd.blend != (cmd.blend && surfaceTileBlendAllowed(cmd))) {
                    fail(i, cmd, "SurfaceTile blend violates MVP fixed state", error);
                    return error;
                }
                if (expectedFrameId != 0 && cmd.frameId != expectedFrameId) {
                    fail(i, cmd, "SurfaceTile frameId is stale for current FrameState", error);
                    return error;
                }
                if (cmd.generation == 0) {
                    fail(i, cmd, "SurfaceTile generation must be non-zero", error);
                    return error;
                }
                break;

            case RenderCommandKind::GltfPrimitive:
            case RenderCommandKind::GltfPrimitiveInstanced:
                if (!requireColorPass(i, cmd, error)) return error;
                if (!cmd.blend && sawTranslucentGltf) {
                    fail(i, cmd, "GltfPrimitive opaque command follows translucent command", error);
                    return error;
                }
                if (cmd.blend) {
                    sawTranslucentGltf = true;
                    if (!cmd.hasTranslucentSortDepth ||
                        !std::isfinite(cmd.translucentSortDepth)) {
                        fail(i, cmd, "GltfPrimitive blended command missing translucent sort depth", error);
                        return error;
                    }
                    if (cmd.translucentSortDepth >
                        lastTranslucentGltfDepth + 1e-6) {
                        fail(i, cmd, "GltfPrimitive translucent commands are not back-to-front sorted", error);
                        return error;
                    }
                    lastTranslucentGltfDepth = cmd.translucentSortDepth;
                }
                if (cmd.depthTest != true) {
                    fail(i, cmd, "GltfPrimitive depthTest violates MVP fixed state", error);
                    return error;
                }
                if (cmd.depthWrite == cmd.blend) {
                    fail(i, cmd, "GltfPrimitive depthWrite violates alpha blend state", error);
                    return error;
                }
                if (cmd.blend != (cmd.blend && gltfPrimitiveBlendAllowed(cmd))) {
                    fail(i, cmd, "GltfPrimitive blend violates MVP fixed state", error);
                    return error;
                }
                if (expectedFrameId != 0 && cmd.frameId != expectedFrameId) {
                    fail(i, cmd, "GltfPrimitive frameId is stale for current FrameState", error);
                    return error;
                }
                if (cmd.generation == 0) {
                    fail(i, cmd, "GltfPrimitive generation must be non-zero", error);
                    return error;
                }
                if (cmd.kind == RenderCommandKind::GltfPrimitiveInstanced) {
                    if (cmd.instanceCount <= 0) {
                        fail(i, cmd, "GltfPrimitiveInstanced requires instanceCount", error);
                        return error;
                    }
                    if (!cmd.instanceBuffer) {
                        fail(i, cmd, "GltfPrimitiveInstanced requires instanceBuffer", error);
                        return error;
                    }
                    if (cmd.instanceStride <= 0) {
                        fail(i, cmd, "GltfPrimitiveInstanced requires instanceStride", error);
                        return error;
                    }
                }
                break;

            case RenderCommandKind::VectorOverlay:
                if (!requireColorPass(i, cmd, error)) return error;
                break;

            case RenderCommandKind::VectorFill:
            case RenderCommandKind::VectorLine:
            case RenderCommandKind::VectorPoint:
            case RenderCommandKind::VectorLabel:
                // 矢量 P1/P5 固定状态:压深度测试出图但不写深度(半透明叠加、
                // 顺序即桶序),双面(球面绕向视半球翻转),alpha 混合。
                if (!requireColorPass(i, cmd, error)) return error;
                if (!requireState(i, cmd, true, false, false, true,
                                  "Vector*", error)) {
                    return error;
                }
                break;

            case RenderCommandKind::VectorStencil:
                // P6 分类两 phase 状态锁死(顺序错/状态错 = 整屏染色或全丢):
                // 体 pass 深度测开写关(z-fail 计数)、不混合、双面;
                // 色 pass 关深度测(覆盖面自身别被地形挡)、开混合、双面。
                if (!requireColorPass(i, cmd, error)) return error;
                if (cmd.stencilPhase == StencilPhase::ClassifyVolume) {
                    if (!requireState(i, cmd, true, false, false, false,
                                      "VectorStencil volume", error)) {
                        return error;
                    }
                } else if (cmd.stencilPhase == StencilPhase::ClassifyColor) {
                    if (!requireState(i, cmd, false, false, false, true,
                                      "VectorStencil color", error)) {
                        return error;
                    }
                } else {
                    fail(i, cmd, "VectorStencil requires a stencil phase",
                         error);
                    return error;
                }
                break;

            case RenderCommandKind::AtmosphereBackground:
                if (!requireColorPass(i, cmd, error)) return error;
                if (!requireState(i, cmd, true, false, false, true, "AtmosphereBackground", error)) return error;
                break;

            case RenderCommandKind::Unknown:
            default:
                break;
        }
    }

    return std::nullopt;
}

void sortMvpRenderCommands(RenderCommandList& commands) {
    std::stable_sort(commands.begin(), commands.end(),
        [](const RenderCommand& a, const RenderCommand& b) {
            return mvpCommandLess(a, b);
        });
}

} // namespace earth_engine
