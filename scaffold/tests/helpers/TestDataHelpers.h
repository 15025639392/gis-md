#pragma once

#include "earth_engine/content/GltfModel.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace earth_engine {
namespace testing {

// ============================================================
// 共享测试数据构建器
// ============================================================

/// 创建 RGBA 图像（1-byte-per-channel）
inline std::unique_ptr<DecodedImage> makeImage(int width, int height,
                                                uint8_t r, uint8_t g = 0,
                                                uint8_t b = 0, uint8_t a = 255) {
    auto img = std::make_unique<DecodedImage>();
    img->width = width;
    img->height = height;
    img->channels = 4;
    img->pixels.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u);
    for (size_t i = 0; i < img->pixels.size(); i += 4) {
        img->pixels[i + 0] = r;
        img->pixels[i + 1] = g;
        img->pixels[i + 2] = b;
        img->pixels[i + 3] = a;
    }
    return img;
}

/// 创建 RGB 图像（3 channels）
inline std::unique_ptr<DecodedImage> makeRgbImage(int width, int height,
                                                   uint8_t r, uint8_t g, uint8_t b) {
    auto img = std::make_unique<DecodedImage>();
    img->width = width;
    img->height = height;
    img->channels = 3;
    img->pixels.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 3u);
    for (size_t i = 0; i < img->pixels.size(); i += 3) {
        img->pixels[i + 0] = r;
        img->pixels[i + 1] = g;
        img->pixels[i + 2] = b;
    }
    return img;
}

} // namespace testing
} // namespace earth_engine
