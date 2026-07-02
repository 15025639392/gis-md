#pragma once

#include "GpuReadyData.h"

#include <memory>
#include <optional>

namespace earth_engine {

class RenderDevice;
struct TilesetTile;
struct GltfModel;
class Mat4;
class Vec3;

struct GltfRenderResourcePreparer {
    /// Legacy synchronous path (kept for animation updates).
    static void prepare(TilesetTile& tile,
                        RenderDevice* device,
                        double currentFrameTimeSeconds);

    /// Phase 1 (Worker Thread): CPU-intensive work.
    /// Converts SurfaceVertex → GPU-ready bytes, decodes textures.
    /// Returns nullopt if the model has no primitives.
    static std::optional<GpuReadyData> prepareCpuWork(
        const TilesetTile& tile,
        double currentFrameTimeSeconds);

    /// Phase 1 variant taking an already-copied model and explicit
    /// transform/origin.  Safe to call on a worker thread because the
    /// model is owned by the caller (deep-copied from the tile).
    static std::optional<GpuReadyData> prepareCpuWorkFromModel(
        const GltfModel& model,
        const Mat4& transform,
        const Vec3& localOrigin,
        double currentFrameTimeSeconds);

    /// Phase 2 (Main Thread): GPU upload only.
    /// Creates GL buffers and textures from CPU-prepared data.
    /// Returns true if all resources were created successfully.
    static bool uploadToGpu(
        TilesetTile& tile,
        RenderDevice* device,
        GpuReadyData&& ready);
};

} // namespace earth_engine
