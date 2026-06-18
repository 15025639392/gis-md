#include "DebugImageryProvider.h"

#include <cstring>
#include <sstream>
#include <cstdlib>

namespace earth_engine {

DebugImageryProvider::DebugImageryProvider() = default;

std::string DebugImageryProvider::buildUrl(const TileKey& key) const {
    // 调试 provider 不使用 URL
    std::ostringstream oss;
    oss << "debug://" << key.z << "/" << key.x << "/" << key.y;
    return oss.str();
}

void DebugImageryProvider::requestTile(const TileKey& key,
                                        CancellationToken token,
                                        TileCallback callback,
                                        HttpRequestPriority /*priority*/) {
    (void)token;
    // 同步生成瓦片（无网络延迟）
    auto image = generateTile(key, tileWidth(), tileHeight());
    callback(key, std::move(image));
}

std::unique_ptr<DecodedImage> DebugImageryProvider::decodeTile(
    const uint8_t* /*data*/, size_t /*len*/) {
    // 调试 provider 不涉及真实解码
    return nullptr;
}

std::unique_ptr<DecodedImage> DebugImageryProvider::generateTile(
    const TileKey& key, int width, int height) {

    auto image = std::make_unique<DecodedImage>();
    image->width = width;
    image->height = height;
    image->channels = 4;  // RGBA
    image->pixels.resize(static_cast<size_t>(width * height * 4));

    uint8_t* data = image->pixels.data();

    // 基于 z/x/y 生成确定性颜色
    auto hash = [](int a, int b, int c) -> uint32_t {
        uint32_t h = static_cast<uint32_t>(a) * 2654435761u;
        h ^= static_cast<uint32_t>(b) * 0x9e3779b9u;
        h ^= static_cast<uint32_t>(c) * 0x517cc1b7u;
        h = h ^ (h >> 16);
        h *= 0x85ebca6bu;
        h = h ^ (h >> 13);
        return h;
    };

    uint32_t h = hash(key.z, key.x, key.y);
    uint8_t baseR = static_cast<uint8_t>((h & 0xFF0000) >> 16);
    uint8_t baseG = static_cast<uint8_t>((h & 0x00FF00) >> 8);
    uint8_t baseB = static_cast<uint8_t>(h & 0x0000FF);

    // 增强颜色饱和度
    auto saturate = [](uint8_t c) -> uint8_t {
        if (c < 64) c = 64;
        if (c > 192) c = 192;
        return c;
    };
    baseR = saturate(baseR);
    baseG = saturate(baseG);
    baseB = saturate(baseB);

    const int kCheckerSize = 32;  // 棋盘格大小（像素）

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            size_t idx = (static_cast<size_t>(y) * width + x) * 4;

            // 棋盘格
            int cx = x / kCheckerSize;
            int cy = y / kCheckerSize;
            bool white = ((cx + cy) & 1) == 0;

            // z/x/y 标签区域（留白中心）
            bool inLabel = (x > 40 && x < width - 20 &&
                            y > height / 2 - 16 && y < height / 2 + 16);

            if (inLabel) {
                // 深色背景
                data[idx + 0] = 20;
                data[idx + 1] = 20;
                data[idx + 2] = 20;
                data[idx + 3] = 255;
            } else if (white) {
                data[idx + 0] = baseR;
                data[idx + 1] = baseG;
                data[idx + 2] = baseB;
                data[idx + 3] = 255;
            } else {
                // 深色格子
                data[idx + 0] = static_cast<uint8_t>(baseR * 0.3f);
                data[idx + 1] = static_cast<uint8_t>(baseG * 0.3f);
                data[idx + 2] = static_cast<uint8_t>(baseB * 0.3f);
                data[idx + 3] = 255;
            }

            // tile 边框（1px 白线）
            if (x == 0 || x == width - 1 || y == 0 || y == height - 1) {
                data[idx + 0] = 255;
                data[idx + 1] = 255;
                data[idx + 2] = 255;
                data[idx + 3] = 255;
            }
        }
    }

    // 用简单方式绘制 z/x/y 文本（ASCII 逐像素描边）
    auto drawDigit = [&](int digit, int px, int py, uint8_t r, uint8_t g, uint8_t b) {
        // 7-segment 风格的简单数字（3×5 点阵）
        static const bool digits[10][5][3] = {
            // 0
            {{1,1,1},{1,0,1},{1,0,1},{1,0,1},{1,1,1}},
            // 1
            {{0,0,1},{0,0,1},{0,0,1},{0,0,1},{0,0,1}},
            // 2
            {{1,1,1},{0,0,1},{1,1,1},{1,0,0},{1,1,1}},
            // 3
            {{1,1,1},{0,0,1},{1,1,1},{0,0,1},{1,1,1}},
            // 4
            {{1,0,1},{1,0,1},{1,1,1},{0,0,1},{0,0,1}},
            // 5
            {{1,1,1},{1,0,0},{1,1,1},{0,0,1},{1,1,1}},
            // 6
            {{1,1,1},{1,0,0},{1,1,1},{1,0,1},{1,1,1}},
            // 7
            {{1,1,1},{0,0,1},{0,0,1},{0,0,1},{0,0,1}},
            // 8
            {{1,1,1},{1,0,1},{1,1,1},{1,0,1},{1,1,1}},
            // 9
            {{1,1,1},{1,0,1},{1,1,1},{0,0,1},{1,1,1}},
        };

        if (digit < 0 || digit > 9) return;
        for (int dy = 0; dy < 5; ++dy) {
            for (int dx = 0; dx < 3; ++dx) {
                if (digits[digit][dy][dx]) {
                    int sx = px + dx;
                    int sy = py + dy;
                    if (sx >= 0 && sx < width && sy >= 0 && sy < height) {
                        size_t i = (static_cast<size_t>(sy) * width + sx) * 4;
                        data[i + 0] = r;
                        data[i + 1] = g;
                        data[i + 2] = b;
                        data[i + 3] = 255;
                    }
                }
            }
        }
    };

    auto drawNumber = [&](int value, int px, int py, uint8_t r, uint8_t g, uint8_t b) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", value);
        int xOff = px;
        for (const char* c = buf; *c; ++c) {
            if (*c >= '0' && *c <= '9') {
                drawDigit(*c - '0', xOff, py, r, g, b);
            } else if (*c == '/') {
                // draw slash
                for (int i = 0; i < 5; ++i) {
                    int sx = xOff + 1;
                    int sy = py + i;
                    if (sx < width && sy < height) {
                        size_t idx2 = (static_cast<size_t>(sy) * width + sx) * 4;
                        data[idx2 + 0] = r;
                        data[idx2 + 1] = g;
                        data[idx2 + 2] = b;
                        data[idx2 + 3] = 255;
                    }
                }
            }
            xOff += 5;  // 每个字符占 5px
        }
    };

    // 居中绘制 z/x/y
    int labelWidth = 60;  // 估算标签宽度
    int startX = (width - labelWidth) / 2;
    int startY = height / 2 - 10;

    drawNumber(key.z, startX, startY, 255, 255, 100);
    drawNumber(key.x, startX + 20, startY, 255, 180, 100);
    drawNumber(key.y, startX + 40, startY, 100, 200, 255);

    return image;
}

} // namespace earth_engine
