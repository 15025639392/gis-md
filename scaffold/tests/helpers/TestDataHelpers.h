#pragma once

#include "earth_engine/content/GltfModel.h"
#include "earth_engine/terrain/QuantizedMeshParser.h"

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

/// Quantized-Mesh zigzag 编码
inline uint16_t zigZagEncode16(int32_t value) {
    return static_cast<uint16_t>(value >= 0 ? value * 2 : (-value * 2) - 1);
}

/// 构建最小的合法 QuantizedMesh 字节流（3 vertices, 1 triangle, no skirt）
inline std::vector<uint8_t> makeQuantizedMeshBytes() {
    auto append = [](std::vector<uint8_t>& bytes, auto value) {
        const auto* p = reinterpret_cast<const uint8_t*>(&value);
        bytes.insert(bytes.end(), p, p + sizeof(value));
    };

    std::vector<uint8_t> bytes;
    // Header: center (3 doubles), minH/maxH (2 floats),
    //         boundingSphere + horizon (7 doubles), vertexCount (1 uint32)
    for (int i = 0; i < 3; ++i) append(bytes, 0.0);
    append(bytes, 0.0f);
    append(bytes, 100.0f);
    for (int i = 0; i < 7; ++i) append(bytes, 0.0);
    append(bytes, static_cast<uint32_t>(3));

    // UVH: 3 vertices
    const uint16_t u[] = {zigZagEncode16(0), zigZagEncode16(32767), zigZagEncode16(-32767)};
    const uint16_t v[] = {zigZagEncode16(0), zigZagEncode16(0), zigZagEncode16(32767)};
    const uint16_t h[] = {zigZagEncode16(0), zigZagEncode16(0), zigZagEncode16(0)};
    for (uint16_t val : u) append(bytes, val);
    for (uint16_t val : v) append(bytes, val);
    for (uint16_t val : h) append(bytes, val);

    // triCount = 1
    append(bytes, static_cast<uint32_t>(1));
    // indices: high-water zigzag for 1 triangle → 0, 1, 2
    append(bytes, static_cast<uint16_t>(0));
    append(bytes, static_cast<uint16_t>(0));
    append(bytes, static_cast<uint16_t>(0));

    // 4 edges: count=0 each
    for (int i = 0; i < 4; ++i) append(bytes, static_cast<uint32_t>(0));

    return bytes;
}

} // namespace testing
} // namespace earth_engine
