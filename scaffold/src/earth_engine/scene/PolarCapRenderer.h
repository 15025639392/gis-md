#pragma once

#include "../renderer/RenderCommand.h"

#include <cstdint>
#include <memory>

namespace earth_engine {

class Renderer;
class RenderDevice;
class Buffer;

/// Fills the geometry gap at the poles. The terrain and imagery tiling is
/// web-mercator, which stops at latitude ±85.05°, so the caps above that have
/// no tiles at all and would otherwise show clear-color/atmosphere straight
/// through the planet at globe scale. This renders a small dedicated cap mesh
/// per pole (mercator edge latitude → pole, following the ellipsoid surface) as
/// a solid-colored glTF primitive, so a gap degrades to a flat polar surface
/// rather than a see-through hole. It reuses the existing glTF primitive shader
/// (no new shader/pass); the mesh is built once and reused every frame.
class PolarCapRenderer {
public:
    PolarCapRenderer();
    ~PolarCapRenderer();

    PolarCapRenderer(const PolarCapRenderer&) = delete;
    PolarCapRenderer& operator=(const PolarCapRenderer&) = delete;

    /// Appends the two polar-cap draw commands to `commands`. Lazily builds the
    /// GPU buffers on first call (needs a valid device); a no-op if the device
    /// is null or buffer creation fails.
    void appendCommands(RenderCommandList& commands,
                        Renderer& renderer,
                        RenderDevice* device,
                        uint64_t frameId);

private:
    struct Cap {
        std::unique_ptr<Buffer> vertexBuffer;
        std::unique_ptr<Buffer> indexBuffer;
        int indexCount = 0;
        int vertexCount = 0;
        float originEcef[3] = {0.0f, 0.0f, 0.0f};
    };

    bool ensureBuilt(RenderDevice* device);

    Cap northCap_;
    Cap southCap_;
    bool built_ = false;
    bool buildFailed_ = false;
};

} // namespace earth_engine
