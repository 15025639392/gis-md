#include "RenderDeviceRasterTextureUploader.h"
#include "../platform/bridge/PlatformBridge.h"

namespace earth_engine {

RenderDeviceRasterTextureUploader::RenderDeviceRasterTextureUploader(
    RenderDevice* device)
    : device_(device) {}

int RenderDeviceRasterTextureUploader::maxTextureSize() const {
    return device_ ? device_->maxTextureSize() : 0;
}

/// Copy edge pixels to a 1px border to eliminate seam artifacts when
/// GL_LINEAR filtering samples near tile boundaries (cesium-native bleed pattern).
static std::vector<uint8_t> addOnePixelBorder(
    const DecodedImage& image) {
    if (image.width <= 0 || image.height <= 0 || image.pixels.empty()) {
        return {};
    }
    const int w = image.width;
    const int h = image.height;
    const int channels = (image.channels > 0) ? image.channels : 4;
    const int padW = w + 2;
    const int padH = h + 2;
    std::vector<uint8_t> padded(static_cast<size_t>(padW) * static_cast<size_t>(padH) * static_cast<size_t>(channels), 0);

    auto srcRow = [&](int y) -> const uint8_t* {
        return image.pixels.data() + static_cast<size_t>(y) * static_cast<size_t>(w) * static_cast<size_t>(channels);
    };
    auto dstRow = [&](int y) -> uint8_t* {
        return padded.data() + static_cast<size_t>(y) * static_cast<size_t>(padW) * static_cast<size_t>(channels);
    };

    // Center: copy original image
    for (int y = 0; y < h; ++y) {
        std::memcpy(dstRow(y + 1) + channels, srcRow(y), static_cast<size_t>(w) * static_cast<size_t>(channels));
    }

    // Left/right border: copy first/last column of each row
    for (int y = 0; y < h; ++y) {
        std::memcpy(dstRow(y + 1), srcRow(y), static_cast<size_t>(channels));                                // left
        std::memcpy(dstRow(y + 1) + (padW - 1) * channels, srcRow(y) + (w - 1) * channels, static_cast<size_t>(channels)); // right
    }

    // Top/bottom border: copy edge rows
    std::memcpy(dstRow(0) + channels, srcRow(0), static_cast<size_t>(w) * static_cast<size_t>(channels));           // top
    std::memcpy(dstRow(padH - 1) + channels, srcRow(h - 1), static_cast<size_t>(w) * static_cast<size_t>(channels)); // bottom

    // Corners: copy corner pixels
    std::memcpy(dstRow(0), srcRow(0), static_cast<size_t>(channels));                                              // top-left
    std::memcpy(dstRow(0) + (padW - 1) * channels, srcRow(0) + (w - 1) * channels, static_cast<size_t>(channels)); // top-right
    std::memcpy(dstRow(padH - 1), srcRow(h - 1), static_cast<size_t>(channels));                                  // bottom-left
    std::memcpy(dstRow(padH - 1) + (padW - 1) * channels, srcRow(h - 1) + (w - 1) * channels, static_cast<size_t>(channels)); // bottom-right

    return padded;
}

std::unique_ptr<Texture> RenderDeviceRasterTextureUploader::uploadRasterTexture(
    const DecodedImage& image,
    const RasterTextureUploadOptions& options) {
    if (!device_ || image.pixels.empty() || image.bytesPerChannel != 1) {
        return nullptr;
    }

    // Add 1px edge bleed to eliminate seam artifacts at tile boundaries
    // (cesium-native pattern: each tile texture is uploaded with a 1px
    // border that repeats the edge texels, so GL_LINEAR filtering at
    // adjacent tile boundaries produces continuous color).
    const int border = options.enableEdgeBleed ? 1 : 0;
    std::vector<uint8_t> padded;
    const uint8_t* texData = image.pixels.data();
    size_t texSize = image.pixels.size();
    int texW = image.width;
    int texH = image.height;

    if (border > 0) {
        padded = addOnePixelBorder(image);
        if (!padded.empty()) {
            texData = padded.data();
            texSize = padded.size();
            texW = image.width + 2;
            texH = image.height + 2;
        }
    }

    TextureDesc desc;
    desc.width = texW;
    desc.height = texH;
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
