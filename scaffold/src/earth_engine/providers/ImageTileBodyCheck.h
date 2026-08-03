#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace earth_engine {

/// 瓦片响应体魔数校验(写入 HttpCache 前由写入方调用,cache 本身保持只读契约)。
///
/// 背景:阿里云 OSS + 字节 CDN 边缘节点实测会间歇返回 HTTP 200 + NoSuchKey
/// XML 错误体(过期负缓存,2026-08-03 真机取证)。XML 体一旦入 HttpCache 会
/// 持续毒化解码;非法体不入缓存,并按 RetryLater(瞬时故障)语义处理,
/// 不是永久 Failed。
///
/// 白名单:PNG / JPEG / WebP。名单外的合法图格式会被拒 → 该瓦片停留在
/// RetryLater 反复重试;接新图格式的数据源前需先扩这里。
inline bool looksLikeImageTileBody(const uint8_t* data, size_t size) {
    // 12 字节下限同时是 WebP 头(RIFF????WEBP)的最短读取长度;真实图像
    // 瓦片不会短于它。
    if (data == nullptr || size < 12) return false;
    static constexpr uint8_t kPng[8] = {
        0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (std::memcmp(data, kPng, sizeof(kPng)) == 0) return true;
    if (data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF) return true;
    if (std::memcmp(data, "RIFF", 4) == 0 &&
        std::memcmp(data + 8, "WEBP", 4) == 0) {
        return true;
    }
    return false;
}

inline bool looksLikeImageTileBody(const std::vector<uint8_t>& body) {
    return looksLikeImageTileBody(body.data(), body.size());
}

} // namespace earth_engine
