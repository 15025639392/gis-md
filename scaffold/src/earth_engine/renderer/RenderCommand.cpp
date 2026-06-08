#include "RenderCommand.h"

#include <algorithm>

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
    auto it = cmd.uniforms.find(name);
    if (it == cmd.uniforms.end() || it->second.empty()) {
        return fallback;
    }
    return it->second.front();
}

bool surfaceTileBlendAllowed(const RenderCommand& cmd) {
    const float tileOpacity = uniformScalar(cmd, "u_tileOpacity", 1.0f);
    const float transitionOpacity = uniformScalar(cmd, "u_transitionOpacity", 1.0f);
    return tileOpacity < 0.999f || transitionOpacity < 0.999f;
}

} // namespace

int mvpRenderOrder(RenderCommandKind kind) {
    switch (kind) {
        case RenderCommandKind::GlobeSurface:
            return 10;
        case RenderCommandKind::SurfaceTile:
            return 10;
        case RenderCommandKind::VectorOverlay:
            return 30;
        case RenderCommandKind::DebugOverlay:
            return 40;
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

    for (size_t i = 0; i < commands.size(); ++i) {
        const RenderCommand& cmd = commands[i];
        const int order = mvpRenderOrder(cmd.kind);
        if (order < lastOrder) {
            fail(i, cmd, "RenderCommand order violates MVP pass order", error);
            return error;
        }
        lastOrder = order;

        switch (cmd.kind) {
            case RenderCommandKind::GlobeSurface:
                if (!requireColorPass(i, cmd, error)) return error;
                if (!requireState(i, cmd, true, true, true, false, "GlobeSurface", error)) return error;
                break;

            case RenderCommandKind::SurfaceTile:
                if (!requireColorPass(i, cmd, error)) return error;
                if (!requireState(i,
                                  cmd,
                                  true,
                                  true,
                                  true,
                                  cmd.blend && surfaceTileBlendAllowed(cmd),
                                  "SurfaceTile",
                                  error)) return error;
                if (expectedFrameId != 0 && cmd.frameId != expectedFrameId) {
                    fail(i, cmd, "SurfaceTile frameId is stale for current FrameState", error);
                    return error;
                }
                if (cmd.generation == 0) {
                    fail(i, cmd, "SurfaceTile generation must be non-zero", error);
                    return error;
                }
                break;

            case RenderCommandKind::VectorOverlay:
                if (!requireColorPass(i, cmd, error)) return error;
                break;

            case RenderCommandKind::DebugOverlay:
                if (!requireColorPass(i, cmd, error)) return error;
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
            return mvpRenderOrder(a.kind) < mvpRenderOrder(b.kind);
        });
}

} // namespace earth_engine
