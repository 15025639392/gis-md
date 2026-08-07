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
    if (!device_ || image.pixels.empty() || image.bytesPerChannel != 1) {
        return nullptr;
    }

    const uint8_t* texData = image.pixels.data();
    const size_t texSize = image.pixels.size();

    TextureDesc desc;
    desc.width = image.width;
    desc.height = image.height;
    desc.format = (image.channels == 4) ? TextureDesc::Format::RGBA8
                                        : TextureDesc::Format::RGB8;
    desc.data = texData;
    desc.dataSize = texSize;
    desc.mipmap = options.generateMipmaps;
    desc.minFilter = TextureDesc::Filter::Linear;
    desc.magFilter = TextureDesc::Filter::Linear;
    desc.maxAnisotropy = 4.0f;
    desc.wrapS = TextureDesc::Wrap::Clamp;
    desc.wrapT = TextureDesc::Wrap::Clamp;

    return device_->createTexture(desc);
}

} // namespace earth_engine
