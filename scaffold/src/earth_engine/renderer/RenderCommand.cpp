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

    if (a.vectorPaintOrder != b.vectorPaintOrder) {
        return a.vectorPaintOrder < b.vectorPaintOrder;
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

} // namespace

int mvpRenderOrder(RenderCommandKind kind) {
    switch (kind) {
        case RenderCommandKind::SkyBackground:
            return 0;
        case RenderCommandKind::GltfPrimitive:
        case RenderCommandKind::GltfPrimitiveInstanced:
            return 15;
        case RenderCommandKind::AtmosphereBackground:
            return 20;
        case RenderCommandKind::VectorStencil:
            return 30;  // 与普通矢量共享 paint ordinal；色 pass 关深度测
        case RenderCommandKind::VectorOverlay:
        case RenderCommandKind::VectorFill:
        case RenderCommandKind::VectorLine:
        case RenderCommandKind::VectorExtrusion:
        case RenderCommandKind::VectorPoint:
            return 30;
        case RenderCommandKind::VectorLabel:
            return 31;  // 文字压其它矢量之上
        case RenderCommandKind::Unknown:
        default:
            return 100;
    }
}

bool mvpRenderCommandLess(const RenderCommand& a, const RenderCommand& b) {
    return mvpCommandLess(a, b);
}

bool mvpRenderCommandsNeedSort(const RenderCommandList& commands) {
    for (size_t i = 1; i < commands.size(); ++i) {
        if (mvpCommandLess(commands[i], commands[i - 1])) return true;
    }
    return false;
}

std::optional<RenderCommandValidationError>
validateMvpRenderCommands(const RenderCommandList& commands,
                          uint64_t expectedFrameId) {
    std::optional<RenderCommandValidationError> error;
    int lastOrder = -1;
    int lastVectorPaintOrder = std::numeric_limits<int>::min();
    bool sawTranslucentGltf = false;
    double lastTranslucentGltfDepth = std::numeric_limits<double>::infinity();

    for (size_t i = 0; i < commands.size(); ++i) {
        const RenderCommand& cmd = commands[i];
        // The fixed blocks are alternative command ABIs.  GLES used to apply
        // both in sequence while Metal preferred glTF via else-if, so a
        // malformed dual-flag command rendered differently by backend.  Keep
        // the producer contract explicit and fail before submission.
        if (cmd.hasGltfUniforms && cmd.hasVectorUniforms) {
            fail(i, cmd,
                 "RenderCommand cannot use glTF and vector uniform blocks "
                 "simultaneously",
                 error);
            return error;
        }
        const int order = mvpRenderOrder(cmd.kind);
        if (order < lastOrder) {
            fail(i, cmd, "RenderCommand order violates MVP pass order", error);
            return error;
        }
        if (order == lastOrder &&
            cmd.vectorPaintOrder < lastVectorPaintOrder) {
            fail(i, cmd,
                 "RenderCommand vector paint order is not monotonic",
                 error);
            return error;
        }
        if (order != lastOrder) {
            lastVectorPaintOrder = std::numeric_limits<int>::min();
        }
        lastOrder = order;
        lastVectorPaintOrder = cmd.vectorPaintOrder;

        switch (cmd.kind) {
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
                // 矢量 P1 面/线固定状态:压深度测试出图但不写深度(半透明
                // 叠加、顺序即桶序),双面(球面绕向视半球翻转),alpha 混合。
                // 面/线是**贴在地表上的几何**,像素与 3D 位置一一对应,
                // 逐像素深度测试语义正确 —— 与下面的符号分道正在于此。
                if (!requireColorPass(i, cmd, error)) return error;
                if (!requireState(i, cmd, true, false, false, true,
                                  "VectorFill/Line", error)) {
                    return error;
                }
                break;

            case RenderCommandKind::VectorExtrusion:
                // V6 建筑挤出:不透明实体,深度测+写(楼与楼互遮挡),
                // 双面(墙带绕向免调),不混合。
                if (!requireColorPass(i, cmd, error)) return error;
                if (!requireState(i, cmd, true, true, false, false,
                                  "VectorExtrusion", error)) {
                    return error;
                }
                break;

            case RenderCommandKind::VectorPoint:
            case RenderCommandKind::VectorLabel:
                // 符号(billboard/文字)固定状态:**深度测试关**。四角共用
                // 锚点深度,逐像素比对只会把 quad 切掉一块 —— 而 quad 像素
                // 没有 3D 位置语义,那道切口是不存在的形状边界。遮挡改由
                // 锚点判定整符号决定(shader eeSymbolTerrainVisibility)。
                // 三家引擎同解:maplibre 地形模式关深度测试、osgEarth 默认
                // Depth(ALWAYS)、cesium 默认 depthTestAgainstTerrain=false。
                if (!requireColorPass(i, cmd, error)) return error;
                if (!requireState(i, cmd, false, false, false, true,
                                  "VectorPoint/Label", error)) {
                    return error;
                }
                break;

            case RenderCommandKind::VectorStencil:
                // P6 分类两 phase 状态锁死(顺序错/状态错 = 整屏染色或全丢):
                // 体 pass 深度测开写关(z-fail 计数)、不混合、**双面**
                //   (两侧计数是 z-fail 的定义,单面直接失去分类语义);
                // 色 pass 关深度测(覆盖面自身别被地形挡)、开混合、**只画背面**
                //   (覆盖只需每个 stencil 选中像素被盖一次,水密体的背面即足够;
                //    双面是白烧一倍光栅化。取背面因为相机进体内时正面被近平面切掉)。
                if (!requireColorPass(i, cmd, error)) return error;
                if (cmd.stencilPhase == StencilPhase::ClassifyVolume) {
                    if (!requireState(i, cmd, true, false, false, false,
                                      "VectorStencil volume", error)) {
                        return error;
                    }
                } else if (cmd.stencilPhase == StencilPhase::ClassifyColor) {
                    if (!requireState(i, cmd, false, false, true, true,
                                      "VectorStencil color", error)) {
                        return error;
                    }
                    // 剔正面留背面。写死在校验里:剔错面 = 相机进体内时整片
                    // 消失,而那是最不容易在静态截图里发现的一类回归。
                    if (cmd.cullMode != RenderCommand::CullMode::Front) {
                        fail(i, cmd,
                             "VectorStencil color must cull front faces "
                             "(back faces are the cover surface)", error);
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
    if (commands.size() < 2) return;

    // RenderCommand 是重对象(uniform map、纹理列表、owner/pass 字符串)。
    // 直接 stable_sort 会在 O(n log n) 中反复搬动整个命令。这里先装饰出
    // 紧凑键，std::sort 只随机访问 POD；原始下标作为最终 tie-break，等价
    // 于旧 stable_sort。深度用双向 > 比较，NaN/相等都回落下标，与旧比较器
    // 两向均 false 后保持输入顺序的语义一致。
    struct SortKey {
        size_t index = 0;
        int renderOrder = 0;
        int paintOrder = 0;
        bool translucentGltf = false;
        bool hasDepth = false;
        double depth = 0.0;
    };
    std::vector<SortKey> order;
    order.reserve(commands.size());
    for (size_t i = 0; i < commands.size(); ++i) {
        const RenderCommand& cmd = commands[i];
        order.push_back(SortKey{i, mvpRenderOrder(cmd.kind),
                                cmd.vectorPaintOrder,
                                isTranslucentGltfPrimitiveCommand(cmd),
                                cmd.hasTranslucentSortDepth,
                                cmd.translucentSortDepth});
    }
    std::sort(order.begin(), order.end(),
              [](const SortKey& a, const SortKey& b) {
                  if (a.renderOrder != b.renderOrder) {
                      return a.renderOrder < b.renderOrder;
                  }
                  if (a.paintOrder != b.paintOrder) {
                      return a.paintOrder < b.paintOrder;
                  }
                  if (a.translucentGltf != b.translucentGltf) {
                      return !a.translucentGltf && b.translucentGltf;
                  }
                  if (a.translucentGltf) {
                      if (a.hasDepth != b.hasDepth) return a.hasDepth;
                      if (a.hasDepth) {
                          if (a.depth > b.depth) return true;
                          if (b.depth > a.depth) return false;
                      }
                  }
                  return a.index < b.index;
              });

    RenderCommandList sorted;
    sorted.reserve(commands.size());
    for (const SortKey& key : order) {
        sorted.push_back(std::move(commands[key.index]));
    }
    commands = std::move(sorted);
}

} // namespace earth_engine
