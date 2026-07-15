#pragma once

#include "GpuReadyData.h"

#include <cstdint>
#include <memory>
#include <optional>

namespace earth_engine {

class RenderDevice;
struct TilesetTile;
struct GltfModel;
class Mat4;
class Vec3;

struct GpuUploadMetrics {
    int64_t vertexBytes = 0;
    int64_t indexBytes = 0;
    int64_t instanceBytes = 0;
    int64_t textureBytes = 0;
    uint32_t primitiveCount = 0;
    uint32_t textureCount = 0;
    uint32_t vertexBufferCount = 0;
    uint32_t indexBufferCount = 0;
    uint32_t instanceBufferCount = 0;
    double textureUploadMs = 0.0;
    double vertexBufferUploadMs = 0.0;
    double indexBufferUploadMs = 0.0;
    double instanceBufferUploadMs = 0.0;
    double resourceCommitMs = 0.0;
    int64_t deferredCpuBytes = 0;
    int64_t deferredReleasePendingBytes = 0;
    uint32_t deferredReleasePendingTasks = 0;
    bool deferredReleaseInlineFallback = false;

    int64_t totalBytes() const {
        return vertexBytes + indexBytes + instanceBytes + textureBytes;
    }

    uint32_t totalBufferCount() const {
        return vertexBufferCount + indexBufferCount + instanceBufferCount;
    }
};

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
        GpuReadyData&& ready,
        GpuUploadMetrics* metrics = nullptr);

    static int64_t deferredCpuReleasePendingBytes();
    static uint32_t deferredCpuReleasePendingTasks();
    static int64_t deferredCpuReleaseLimitBytes();
};

} // namespace earth_engine
