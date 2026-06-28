#include "Uri.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <unordered_set>

namespace earth_engine {

static const char kHex[] = "0123456789ABCDEF";

static bool isUnreserved(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) ||
           c == '-' || c == '.' || c == '_' || c == '~';
}

static bool isSchemeChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) ||
           c == '+' || c == '-' || c == '.';
}

Uri::Parsed Uri::parse(const std::string& url) {
    Parsed result;
    size_t pos = 0;

    // fragment: # 之后的部分
    size_t hashPos = url.find('#');
    std::string beforeFragment;
    if (hashPos != std::string::npos) {
        result.fragment = url.substr(hashPos + 1);
        beforeFragment = url.substr(0, hashPos);
    } else {
        beforeFragment = url;
    }

    // query: ? 之后（fragment 之前）的部分
    size_t qPos = beforeFragment.find('?');
    std::string beforeQuery;
    if (qPos != std::string::npos) {
        result.query = beforeFragment.substr(qPos + 1);
        beforeQuery = beforeFragment.substr(0, qPos);
    } else {
        beforeQuery = beforeFragment;
    }

    // scheme: 第一个 :// 之前的部分
    size_t schemeEnd = beforeQuery.find("://");
    if (schemeEnd != std::string::npos) {
        std::string possibleScheme = beforeQuery.substr(0, schemeEnd);
        if (!possibleScheme.empty() &&
            std::isalpha(static_cast<unsigned char>(possibleScheme[0]))) {
            bool valid = true;
            for (char c : possibleScheme) {
                if (!isSchemeChar(c)) { valid = false; break; }
            }
            if (valid) {
                result.hasScheme = true;
                result.scheme = possibleScheme;
                pos = schemeEnd + 3; // skip ://

                // authority: 到下一个 /
                size_t authEnd = beforeQuery.find('/', pos);
                if (authEnd == std::string::npos) {
                    result.authority = beforeQuery.substr(pos);
                    result.hasAuthority = true;
                    result.path = "/";
                    return result;
                } else {
                    result.authority = beforeQuery.substr(pos, authEnd - pos);
                    result.hasAuthority = true;
                    pos = authEnd;
                }
            }
        }
    }

    // path: 剩余部分
    result.path = beforeQuery.substr(pos);
    if (result.path.empty()) {
        result.path = "/";
    }

    return result;
}

std::string Uri::compose(const Parsed& parsed) {
    std::string result;
    if (parsed.hasScheme) {
        result = parsed.scheme + "://" + parsed.authority;
    }
    result += parsed.path;
    if (!parsed.query.empty()) {
        result += "?" + parsed.query;
    }
    if (!parsed.fragment.empty()) {
        result += "#" + parsed.fragment;
    }
    return result;
}

bool Uri::hasScheme(const std::string& url) {
    size_t colon = url.find(':');
    if (colon == std::string::npos || colon == 0) return false;
    for (size_t i = 0; i < colon; ++i) {
        if (!isSchemeChar(url[i])) return false;
    }
    return true;
}

std::string Uri::resolve(const std::string& base,
                         const std::string& relative,
                         bool useBaseQuery) {
    if (relative.empty()) return base;

    // 绝对 URL：有 scheme 或是协议相对
    if (hasScheme(relative) || relative.rfind("//", 0) == 0) {
        if (relative.rfind("//", 0) == 0) {
            // 协议相对 — 从 base 继承 scheme
            Parsed baseParsed = parse(base);
            return baseParsed.scheme + ":" + relative;
        }
        return relative;
    }

    // fragment 继承
    if (relative[0] == '#') {
        Parsed baseParsed = parse(base);
        baseParsed.fragment = relative.substr(1);
        return compose(baseParsed);
    }

    // query 替换
    if (relative[0] == '?') {
        Parsed baseParsed = parse(base);
        std::string newQuery = relative.substr(1);
        if (useBaseQuery) {
            newQuery = mergeQuery(baseParsed.query, newQuery, true);
        }
        baseParsed.query = newQuery;
        return compose(baseParsed);
    }

    Parsed baseParsed = parse(base);
    std::string resolvedPath;

    if (!relative.empty() && relative[0] == '/') {
        // 绝对路径
        resolvedPath = normalizePath(relative);
    } else {
        // 相对路径
        std::string baseDir;
        size_t slash = baseParsed.path.rfind('/');
        if (slash != std::string::npos) {
            baseDir = baseParsed.path.substr(0, slash + 1);
        }
        resolvedPath = normalizePath(baseDir + relative);
    }

    Parsed result;
    result.hasScheme = baseParsed.hasScheme;
    result.scheme = baseParsed.scheme;
    result.hasAuthority = baseParsed.hasAuthority;
    result.authority = baseParsed.authority;
    result.path = resolvedPath;

    if (useBaseQuery) {
        result.query = mergeQuery(baseParsed.query, "", true);
    }

    return compose(result);
}

std::string Uri::substituteTemplateParameters(
    const std::string& templateUri,
    const std::function<std::string(const std::string&)>& callback) {
    std::string result;
    size_t pos = 0;
    while (pos < templateUri.size()) {
        size_t open = templateUri.find('{', pos);
        if (open == std::string::npos) {
            result.append(templateUri, pos, std::string::npos);
            break;
        }
        result.append(templateUri, pos, open - pos);
        size_t close = templateUri.find('}', open + 1);
        if (close == std::string::npos) {
            // 未闭合 — 作为字面量
            result.append(templateUri, open, std::string::npos);
            break;
        }
        std::string placeholder = templateUri.substr(open + 1, close - open - 1);
        result.append(callback(placeholder));
        pos = close + 1;
    }
    return result;
}

std::string Uri::addQuery(const std::string& url,
                          const std::string& key,
                          const std::string& value) {
    if (key.empty() && value.empty()) return url;
    std::string result = url;
    if (url.find('?') != std::string::npos) {
        result += '&';
    } else {
        result += '?';
    }
    result += percentEncode(key);
    result += '=';
    result += percentEncode(value);
    return result;
}

std::string Uri::percentEncode(const std::string& value) {
    std::string encoded;
    for (unsigned char c : value) {
        if (isUnreserved(static_cast<char>(c))) {
            encoded.push_back(static_cast<char>(c));
        } else {
            encoded.push_back('%');
            encoded.push_back(kHex[c >> 4]);
            encoded.push_back(kHex[c & 0x0F]);
        }
    }
    return encoded;
}

std::string Uri::percentDecode(const std::string& value) {
    std::string decoded;
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%' && i + 2 < value.size()) {
            char hi = value[i + 1];
            char lo = value[i + 2];
            auto hexVal = [](char c) -> uint8_t {
                if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
                if (c >= 'A' && c <= 'F') return static_cast<uint8_t>(10 + c - 'A');
                if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(10 + c - 'a');
                return 0;
            };
            decoded.push_back(static_cast<char>((hexVal(hi) << 4) | hexVal(lo)));
            i += 2;
        } else {
            decoded.push_back(value[i]);
        }
    }
    return decoded;
}

std::string Uri::normalizePath(const std::string& path) {
    if (path.empty()) return path;

    bool absolute = path[0] == '/';
    bool trailingSlash = path.size() > 1 && path.back() == '/';

    std::vector<std::string> segments;
    size_t start = absolute ? 1 : 0;
    size_t end;
    while ((end = path.find('/', start)) != std::string::npos) {
        std::string seg = path.substr(start, end - start);
        if (seg == "..") {
            if (!segments.empty() && segments.back() != "..") {
                segments.pop_back();
            } else if (!absolute) {
                segments.push_back("..");
            }
        } else if (!seg.empty() && seg != ".") {
            segments.push_back(seg);
        }
        start = end + 1;
    }
    // 最后一段
    std::string lastSeg = path.substr(start);
    if (lastSeg == "..") {
        if (!segments.empty() && segments.back() != "..") {
            segments.pop_back();
        } else if (!absolute) {
            segments.push_back("..");
        }
    } else if (!lastSeg.empty() && lastSeg != ".") {
        segments.push_back(lastSeg);
    }

    std::string result;
    if (absolute) result.push_back('/');
    for (size_t i = 0; i < segments.size(); ++i) {
        if (i > 0) result.push_back('/');
        result += segments[i];
    }
    if (trailingSlash && !result.empty() && result.back() != '/') {
        result.push_back('/');
    }
    if (result.empty() && absolute) result = "/";
    return result;
}

std::string Uri::mergeQuery(const std::string& baseQuery,
                            const std::string& relativeQuery,
                            bool relativePreferred) {
    // parse query string into key=value pairs
    auto parseParams = [](const std::string& q) {
        std::vector<std::pair<std::string, std::string>> params;
        size_t pos = 0;
        while (pos < q.size()) {
            size_t amp = q.find('&', pos);
            std::string kv = q.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
            size_t eq = kv.find('=');
            if (eq != std::string::npos) {
                params.emplace_back(kv.substr(0, eq), kv.substr(eq + 1));
            } else if (!kv.empty()) {
                params.emplace_back(kv, "");
            }
            if (amp == std::string::npos) break;
            pos = amp + 1;
        }
        return params;
    };

    auto baseParams = parseParams(baseQuery);
    auto relParams = parseParams(relativeQuery);

    std::unordered_set<std::string> relKeys;
    for (const auto& p : relParams) relKeys.insert(p.first);

    // start with relative params
    std::string result;
    for (const auto& p : relParams) {
        if (!result.empty()) result += '&';
        result += p.first + '=' + p.second;
    }

    // add base params not in relative
    for (const auto& p : baseParams) {
        if (relKeys.find(p.first) == relKeys.end()) {
            if (!result.empty()) result += '&';
            result += p.first + '=' + p.second;
        }
    }

    return result;
}

} // namespace earth_engine
