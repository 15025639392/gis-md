#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace earth_engine {

/**
 * @brief URI 解析、拼接、模板替换工具。
 *
 * 对齐 cesium-native CesiumUtility::Uri。
 * 纯字符串实现（不依赖外部 URL 库）。
 *
 * 覆盖 3 处现有 URL 操作：
 * - TileMapServiceUrl::resolveRelativeUrl
 * - ImplicitTileIdUtilities::resolveUrl + substituteTemplateParameters
 * - GltfContentProvider::resolveContentUrl
 */
class Uri {
public:
    struct Parsed {
        std::string scheme;
        std::string authority;
        std::string path;
        std::string query;
        std::string fragment;
        bool hasScheme = false;
        bool hasAuthority = false;
    };

    /// 解析 URL 为结构化组件
    static Parsed parse(const std::string& url);

    /// 将结构化组件组合为 URL 字符串
    static std::string compose(const Parsed& parsed);

    /// 相对 URL 解析
    /// @param base 基准 URL
    /// @param relative 相对 URL
    /// @param useBaseQuery 是否保留 base 中 relative 未覆盖的 query 参数
    static std::string resolve(const std::string& base,
                               const std::string& relative,
                               bool useBaseQuery = false);

    /// URL 模板替换：将 {placeholder} 替换为回调返回的值
    /// 未闭合的 { 作为字面量输出
    static std::string substituteTemplateParameters(
        const std::string& templateUri,
        const std::function<std::string(const std::string&)>& callback);

    /// 追加 query 参数（追加 ? 或 & 自动处理）
    static std::string addQuery(const std::string& url,
                                const std::string& key,
                                const std::string& value);

    /// 百分号编码 — 对 ASCII 控制字符、非 ASCII 字节、URL 保留字符编码
    static std::string percentEncode(const std::string& value);

    /// 百分号解码
    static std::string percentDecode(const std::string& value);

    /// 路径规范化：处理 ../、./、重复斜杠
    static std::string normalizePath(const std::string& path);

    /// 检查字符串是否有 URL scheme（如 http://, https://, file://）
    static bool hasScheme(const std::string& url);

private:
    static std::string mergeQuery(const std::string& baseQuery,
                                  const std::string& relativeQuery,
                                  bool relativePreferred);
};

} // namespace earth_engine
