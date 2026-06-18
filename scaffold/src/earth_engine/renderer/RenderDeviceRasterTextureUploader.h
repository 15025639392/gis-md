#pragma once

#include "../providers/RasterTextureUploader.h"
#include "RenderDevice.h"

namespace earth_engine {

class RenderDeviceRasterTextureUploader final : public RasterTextureUploader {
public:
    explicit RenderDeviceRasterTextureUploader(RenderDevice* device);

    int maxTextureSize() const override;

    std::unique_ptr<Texture> uploadRasterTexture(
        const DecodedImage& image,
        const RasterTextureUploadOptions& options) override;

private:
    RenderDevice* device_ = nullptr;
};

} // namespace earth_engine
