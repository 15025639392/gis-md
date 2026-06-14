#pragma once

#include "TileKey.h"
#include "../core/math/Rectangle.h"
#include "../renderer/RenderDevice.h"

#include <memory>
#include <cstdint>

namespace earth_engine {

struct ImageryAttachment;

/// cesium-native RasterMappedTo3DTile equivalent.
/// Links a geometry tile to an imagery tile with state management.
class RasterMappedToTilesetTile {
public:
    enum class State { Unattached, TemporarilyAttached, Attached };

    RasterMappedToTilesetTile();

    /// Update state: check if loading tile is ready, handle fallback
    /// @return true if the attached texture changed
    bool update(const TileKey& geometryKey,
                const Rectangle& geometryBounds,
                class BasemapLayer* imageryLayer,
                RenderDevice* device);

    /// Compute UV translation/scale for current mapping
    void computeTranslationAndScale(const Rectangle& geometryBounds);

    // Accessors
    State state() const { return state_; }
    Texture* texture() const { return readyTexture_; }
    float offsetU() const { return offsetU_; }
    float offsetV() const { return offsetV_; }
    float scaleU() const { return scaleU_; }
    float scaleV() const { return scaleV_; }

private:
    State state_ = State::Unattached;
    TileKey loadingKey_;
    TileKey readyKey_;
    Texture* readyTexture_ = nullptr;
    float offsetU_ = 0.0f, offsetV_ = 0.0f;
    float scaleU_ = 1.0f, scaleV_ = 1.0f;
    bool originalFailed_ = false;
};

} // namespace earth_engine
