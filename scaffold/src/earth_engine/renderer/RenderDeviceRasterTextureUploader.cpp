#include "RenderDeviceRasterTextureUploader.h"
#include "../platform/bridge/PlatformBridge.h"

namespace earth_engine {

RenderDeviceRasterTextureUploader::RenderDeviceRasterTextureUploader(
    RenderDevice* device)
    : device_(device) {}

int RenderDeviceRasterTextureUploader::maxTextureSize() const {
    return device_ ? device_->maxTextureSize() : 0;
}

std::unique_ptr<Texture> RenderDeviceRasterTextureUploader::uploadRasterTexture(
    const DecodedImage& image,
    const RasterTextureUploadOptions& options) {
    if (!device_ || image.pixels.empty()) {
        return nullptr;
    }

    // Render-chain step 5: this TextureDesc is the only contract between
    // DecodedImage memory and the GLES/Metal upload path. Core imagery tests
    // cannot prove orientation, channel order, or row alignment after this
    // point; use backend readback / fixture-color tests for those failures.
    TextureDesc desc;
    desc.width = image.width;
    desc.height = image.height;
    desc.format = (image.channels == 4) ? TextureDesc::Format::RGBA8
                                        : TextureDesc::Format::RGB8;
    desc.data = image.pixels.data();
    desc.dataSize = image.pixels.size();
    desc.mipmap = options.generateMipmaps;
    desc.minFilter = TextureDesc::Filter::Linear;
    desc.magFilter = TextureDesc::Filter::Linear;
    desc.maxAnisotropy = 4.0f;
    desc.wrapS = TextureDesc::Wrap::Clamp;
    desc.wrapT = TextureDesc::Wrap::Clamp;

    return device_->createTexture(desc);
}

} // namespace earth_engine
