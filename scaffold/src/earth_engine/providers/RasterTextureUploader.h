#pragma once

#include <memory>

namespace earth_engine {

struct DecodedImage;
class Texture;

struct RasterTextureUploadOptions {
    bool generateMipmaps = true;
};

/// Resource-preparation boundary for raster imagery.
///
/// RasterOverlayTileProvider owns tile lifecycle and decoded CPU imagery. The
/// uploader owns the platform/backend-specific step that turns decoded pixels
/// into a GPU texture.
class RasterTextureUploader {
public:
    virtual ~RasterTextureUploader() = default;

    virtual int maxTextureSize() const = 0;

    virtual std::unique_ptr<Texture> uploadRasterTexture(
        const DecodedImage& image,
        const RasterTextureUploadOptions& options) = 0;
};

} // namespace earth_engine
