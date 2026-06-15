#include "GltfModel.h"

#include <nlohmann/json.hpp>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <variant>

namespace earth_engine {
namespace {

using json = nlohmann::json;

constexpr uint32_t kGlbMagic = 0x46546C67u;
constexpr uint32_t kGlbJsonChunk = 0x4E4F534Au;
constexpr uint32_t kGlbBinChunk = 0x004E4942u;

std::optional<size_t> jsonSizeValue(const json& value);

int64_t gltfFeaturePropertyExtraByteSize(
    const GltfFeaturePropertyValue& value) {
    if (const auto* text = std::get_if<std::string>(&value)) {
        return static_cast<int64_t>(text->size());
    }
    return 0;
}

int64_t gltfFeaturePropertiesByteSize(
    const std::map<std::string, GltfFeaturePropertyValue>& properties) {
    int64_t bytes = 0;
    for (const auto& property : properties) {
        bytes += static_cast<int64_t>(
            sizeof(std::pair<const std::string, GltfFeaturePropertyValue>));
        bytes += static_cast<int64_t>(property.first.size());
        bytes += gltfFeaturePropertyExtraByteSize(property.second);
    }
    return bytes;
}

bool validTexCoordSetIndex(int texCoord) {
    return texCoord >= 0 &&
           texCoord < static_cast<int>(kGltfMaxTexCoordSets);
}

std::optional<size_t> texCoordSetIndexFromSemantic(
    const std::string& semantic) {
    constexpr const char* prefix = "TEXCOORD_";
    const size_t prefixLength = std::strlen(prefix);
    if (semantic.rfind(prefix, 0) != 0 ||
        semantic.size() == prefixLength) {
        return std::nullopt;
    }
    if (semantic.size() > prefixLength + 1u &&
        semantic[prefixLength] == '0') {
        return std::nullopt;
    }
    size_t value = 0;
    for (size_t i = prefixLength; i < semantic.size(); ++i) {
        const char c = semantic[i];
        if (!std::isdigit(static_cast<unsigned char>(c))) {
            return std::nullopt;
        }
        value = value * 10u + static_cast<size_t>(c - '0');
        if (value >= kGltfMaxTexCoordSets) {
            return std::nullopt;
        }
    }
    return value;
}

bool attributeSemanticStartsWith(
    const std::string& semantic,
    const char* prefix) {
    return semantic.rfind(prefix, 0) == 0;
}

bool primitiveAttributeSemanticSupported(const std::string& semantic,
                                         bool allowLegacyBatchIdAttribute);
bool morphTargetSemanticSupported(const std::string& semantic);

uint32_t readU32LE(const uint8_t* p) {
    return uint32_t(p[0]) |
           (uint32_t(p[1]) << 8) |
           (uint32_t(p[2]) << 16) |
           (uint32_t(p[3]) << 24);
}

std::string trimRightJsonPadding(std::string value) {
    while (!value.empty() &&
           (value.back() == '\0' ||
            std::isspace(static_cast<unsigned char>(value.back())))) {
        value.pop_back();
    }
    return value;
}

size_t componentSize(int componentType) {
    switch (componentType) {
        case 5120:
        case 5121:
            return 1;
        case 5122:
        case 5123:
            return 2;
        case 5125:
        case 5126:
            return 4;
        default:
            return 0;
    }
}

struct AccessorElementLayout {
    int components = 0;
    size_t elementBytes = 0;
    std::array<size_t, 16> componentOffsets{};
};

std::optional<AccessorElementLayout> accessorElementLayoutForType(
    const std::string& type,
    size_t componentBytes) {
    if (componentBytes == 0) {
        return std::nullopt;
    }

    AccessorElementLayout layout;
    int rows = 0;
    int columns = 1;
    if (type == "SCALAR") {
        rows = 1;
    } else if (type == "VEC2") {
        rows = 2;
    } else if (type == "VEC3") {
        rows = 3;
    } else if (type == "VEC4") {
        rows = 4;
    } else if (type == "MAT2") {
        rows = 2;
        columns = 2;
    } else if (type == "MAT3") {
        rows = 3;
        columns = 3;
    } else if (type == "MAT4") {
        rows = 4;
        columns = 4;
    } else {
        return std::nullopt;
    }

    layout.components = rows * columns;
    const size_t rowBytes = static_cast<size_t>(rows) * componentBytes;
    const bool matrix = columns > 1;
    const size_t columnStride =
        matrix ? ((rowBytes + 3u) / 4u) * 4u : rowBytes;
    layout.elementBytes =
        matrix ? columnStride * static_cast<size_t>(columns) : rowBytes;
    for (int column = 0; column < columns; ++column) {
        for (int row = 0; row < rows; ++row) {
            const int component = column * rows + row;
            layout.componentOffsets[static_cast<size_t>(component)] =
                static_cast<size_t>(column) * columnStride +
                static_cast<size_t>(row) * componentBytes;
        }
    }
    return layout;
}

double readComponent(const uint8_t* p, int componentType, bool normalized) {
    switch (componentType) {
        case 5120: {
            int8_t v = 0;
            std::memcpy(&v, p, sizeof(v));
            return normalized ? std::max(-1.0, double(v) / 127.0) : double(v);
        }
        case 5121: {
            uint8_t v = *p;
            return normalized ? double(v) / 255.0 : double(v);
        }
        case 5122: {
            int16_t v = 0;
            std::memcpy(&v, p, sizeof(v));
            return normalized ? std::max(-1.0, double(v) / 32767.0) : double(v);
        }
        case 5123: {
            uint16_t v = 0;
            std::memcpy(&v, p, sizeof(v));
            return normalized ? double(v) / 65535.0 : double(v);
        }
        case 5125: {
            uint32_t v = 0;
            std::memcpy(&v, p, sizeof(v));
            return double(v);
        }
        case 5126: {
            float v = 0.0f;
            std::memcpy(&v, p, sizeof(v));
            return double(v);
        }
        default:
            return 0.0;
    }
}

uint32_t readIndexComponent(const uint8_t* p, int componentType) {
    switch (componentType) {
        case 5121:
            return uint32_t(*p);
        case 5123: {
            uint16_t v = 0;
            std::memcpy(&v, p, sizeof(v));
            return uint32_t(v);
        }
        case 5125: {
            uint32_t v = 0;
            std::memcpy(&v, p, sizeof(v));
            return v;
        }
        default:
            return 0;
    }
}

std::optional<std::vector<uint8_t>> decodeBase64DataUri(const std::string& uri) {
    const std::string marker = ";base64,";
    const size_t markerPos = uri.find(marker);
    if (uri.rfind("data:", 0) != 0 || markerPos == std::string::npos) {
        return std::nullopt;
    }

    static constexpr int8_t kInvalid = -1;
    std::array<int8_t, 256> table{};
    table.fill(kInvalid);
    const std::string alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (size_t i = 0; i < alphabet.size(); ++i) {
        table[static_cast<uint8_t>(alphabet[i])] = static_cast<int8_t>(i);
    }

    std::vector<uint8_t> output;
    int value = 0;
    int bits = -8;
    for (char c : uri.substr(markerPos + marker.size())) {
        if (std::isspace(static_cast<unsigned char>(c))) continue;
        if (c == '=') break;
        const int8_t decoded = table[static_cast<uint8_t>(c)];
        if (decoded == kInvalid) return std::nullopt;
        value = (value << 6) | decoded;
        bits += 6;
        if (bits >= 0) {
            output.push_back(static_cast<uint8_t>((value >> bits) & 0xff));
            bits -= 8;
        }
    }
    return output;
}

bool supportedCoreImageMimeType(const std::string& mimeType) {
    return mimeType == "image/png" || mimeType == "image/jpeg";
}

bool supportedImageMimeType(const std::string& mimeType, bool allowWebp) {
    return supportedCoreImageMimeType(mimeType) ||
           (allowWebp && mimeType == "image/webp");
}

bool uriPathEndsWith(std::string uri, const char* extension) {
    const size_t fragment = uri.find('#');
    if (fragment != std::string::npos) {
        uri.resize(fragment);
    }
    const size_t query = uri.find('?');
    if (query != std::string::npos) {
        uri.resize(query);
    }
    std::transform(
        uri.begin(),
        uri.end(),
        uri.begin(),
        [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
    const std::string ext(extension);
    return uri.size() >= ext.size() &&
           uri.compare(uri.size() - ext.size(), ext.size(), ext) == 0;
}

bool imageUriUsesUnsupportedFormat(
    const std::string& uri,
    bool allowWebp) {
    if (uri.rfind("data:", 0) == 0) {
        return false;
    }
    if (!allowWebp && uriPathEndsWith(uri, ".webp")) {
        return true;
    }
    return uriPathEndsWith(uri, ".ktx") ||
           uriPathEndsWith(uri, ".ktx2") ||
           uriPathEndsWith(uri, ".basis") ||
           uriPathEndsWith(uri, ".avif") ||
           uriPathEndsWith(uri, ".dds") ||
           uriPathEndsWith(uri, ".astc");
}

std::optional<std::string> dataUriMimeType(const std::string& uri) {
    constexpr const char* prefix = "data:";
    constexpr size_t prefixLength = 5u;
    if (uri.rfind(prefix, 0) != 0) {
        return std::nullopt;
    }
    const size_t comma = uri.find(',', prefixLength);
    if (comma == std::string::npos) {
        return std::nullopt;
    }
    std::string mediaType = uri.substr(prefixLength, comma - prefixLength);
    const size_t parameterStart = mediaType.find(';');
    if (parameterStart != std::string::npos) {
        mediaType.resize(parameterStart);
    }
    return mediaType;
}

bool validImageDataUriSource(const std::string& uri, bool allowWebp) {
    if (uri.rfind("data:", 0) != 0) {
        return true;
    }
    const auto mimeType = dataUriMimeType(uri);
    return mimeType &&
           supportedImageMimeType(*mimeType, allowWebp) &&
           uri.find(";base64,") != std::string::npos;
}

bool encodedImageLooksLikeWebp(const std::vector<uint8_t>& encoded) {
    return encoded.size() >= 12u &&
           encoded[0] == 'R' &&
           encoded[1] == 'I' &&
           encoded[2] == 'F' &&
           encoded[3] == 'F' &&
           encoded[8] == 'W' &&
           encoded[9] == 'E' &&
           encoded[10] == 'B' &&
           encoded[11] == 'P';
}

template <size_t N>
bool encodedImageStartsWith(
    const std::vector<uint8_t>& encoded,
    const std::array<uint8_t, N>& prefix) {
    return encoded.size() >= prefix.size() &&
           std::equal(prefix.begin(), prefix.end(), encoded.begin());
}

bool encodedImageLooksLikeKtx(const std::vector<uint8_t>& encoded) {
    static constexpr std::array<uint8_t, 12> kKtx1Identifier = {
        static_cast<uint8_t>(0xABu), 'K', 'T', 'X', ' ', '1', '1',
        static_cast<uint8_t>(0xBBu), 0x0Du, 0x0Au, 0x1Au, 0x0Au};
    static constexpr std::array<uint8_t, 12> kKtx2Identifier = {
        static_cast<uint8_t>(0xABu), 'K', 'T', 'X', ' ', '2', '0',
        static_cast<uint8_t>(0xBBu), 0x0Du, 0x0Au, 0x1Au, 0x0Au};
    return encodedImageStartsWith(encoded, kKtx1Identifier) ||
           encodedImageStartsWith(encoded, kKtx2Identifier);
}

bool encodedImageLooksLikeDds(const std::vector<uint8_t>& encoded) {
    static constexpr std::array<uint8_t, 4> kDdsIdentifier = {
        'D', 'D', 'S', ' '};
    return encodedImageStartsWith(encoded, kDdsIdentifier);
}

bool encodedImageLooksLikeAstc(const std::vector<uint8_t>& encoded) {
    static constexpr std::array<uint8_t, 4> kAstcIdentifier = {
        0x13u,
        static_cast<uint8_t>(0xABu),
        static_cast<uint8_t>(0xA1u),
        0x5Cu};
    return encodedImageStartsWith(encoded, kAstcIdentifier);
}

bool encodedImageLooksLikeAvif(const std::vector<uint8_t>& encoded) {
    if (encoded.size() < 12u ||
        encoded[4] != 'f' ||
        encoded[5] != 't' ||
        encoded[6] != 'y' ||
        encoded[7] != 'p') {
        return false;
    }
    auto isAvifBrand = [&encoded](size_t offset) {
        return offset + 4u <= encoded.size() &&
               encoded[offset] == 'a' &&
               encoded[offset + 1u] == 'v' &&
               encoded[offset + 2u] == 'i' &&
               (encoded[offset + 3u] == 'f' ||
                encoded[offset + 3u] == 's');
    };
    if (isAvifBrand(8u)) {
        return true;
    }
    for (size_t offset = 16u; offset + 4u <= encoded.size(); offset += 4u) {
        if (isAvifBrand(offset)) {
            return true;
        }
    }
    return false;
}

bool encodedImageUsesUnsupportedFormat(
    const std::vector<uint8_t>& encoded,
    bool allowWebp) {
    if (!allowWebp && encodedImageLooksLikeWebp(encoded)) {
        return true;
    }
    return encodedImageLooksLikeKtx(encoded) ||
           encodedImageLooksLikeDds(encoded) ||
           encodedImageLooksLikeAstc(encoded) ||
           encodedImageLooksLikeAvif(encoded);
}

struct ParsedInput {
    json document;
    std::vector<uint8_t> binaryChunk;
};

std::optional<ParsedInput> parseInput(const uint8_t* data, size_t size) {
    if (!data || size == 0) return std::nullopt;

    if (size >= 20 && readU32LE(data) == kGlbMagic) {
        const uint32_t version = readU32LE(data + 4);
        const uint32_t totalLength = readU32LE(data + 8);
        if (version != 2 || totalLength > size) return std::nullopt;

        std::string jsonChunk;
        std::vector<uint8_t> binChunk;
        size_t offset = 12;
        while (offset + 8 <= totalLength) {
            const uint32_t chunkLength = readU32LE(data + offset);
            const uint32_t chunkType = readU32LE(data + offset + 4);
            offset += 8;
            if (offset + chunkLength > totalLength) return std::nullopt;
            if (chunkType == kGlbJsonChunk) {
                jsonChunk.assign(
                    reinterpret_cast<const char*>(data + offset),
                    chunkLength);
            } else if (chunkType == kGlbBinChunk) {
                binChunk.assign(data + offset, data + offset + chunkLength);
            }
            offset += chunkLength;
        }
        if (jsonChunk.empty()) return std::nullopt;
        json doc = json::parse(trimRightJsonPadding(std::move(jsonChunk)),
                               nullptr,
                               false);
        if (doc.is_discarded()) return std::nullopt;
        return ParsedInput{std::move(doc), std::move(binChunk)};
    }

    json doc = json::parse(
        std::string(reinterpret_cast<const char*>(data), size),
        nullptr,
        false);
    if (doc.is_discarded()) return std::nullopt;
    return ParsedInput{std::move(doc), {}};
}

std::optional<std::pair<int, int>> parseVersionPair(
    const std::string& version) {
    const size_t dot = version.find('.');
    if (dot == std::string::npos || dot == 0u ||
        dot + 1u >= version.size()) {
        return std::nullopt;
    }
    int major = 0;
    for (size_t i = 0; i < dot; ++i) {
        const char c = version[i];
        if (!std::isdigit(static_cast<unsigned char>(c))) {
            return std::nullopt;
        }
        major = major * 10 + int(c - '0');
    }
    int minor = 0;
    for (size_t i = dot + 1u; i < version.size(); ++i) {
        const char c = version[i];
        if (!std::isdigit(static_cast<unsigned char>(c))) {
            return std::nullopt;
        }
        minor = minor * 10 + int(c - '0');
    }
    return std::make_pair(major, minor);
}

bool versionGreater(
    const std::pair<int, int>& lhs,
    const std::pair<int, int>& rhs) {
    return lhs.first > rhs.first ||
           (lhs.first == rhs.first && lhs.second > rhs.second);
}

bool validateAsset(const json& doc) {
    if (!doc.is_object()) {
        return false;
    }
    const auto assetIt = doc.find("asset");
    if (assetIt == doc.end() || !assetIt->is_object()) {
        return false;
    }
    const json& asset = *assetIt;
    const auto versionIt = asset.find("version");
    if (versionIt == asset.end() || !versionIt->is_string()) {
        return false;
    }
    const std::string version = versionIt->get<std::string>();
    const auto parsedVersion = parseVersionPair(version);
    if (!parsedVersion ||
        parsedVersion->first != 2 ||
        parsedVersion->second != 0) {
        return false;
    }
    const auto minVersionIt = asset.find("minVersion");
    if (minVersionIt != asset.end()) {
        if (!minVersionIt->is_string()) {
            return false;
        }
        const auto minVersion =
            parseVersionPair(minVersionIt->get<std::string>());
        if (!minVersion || versionGreater(*minVersion, *parsedVersion)) {
            return false;
        }
    }
    if (asset.contains("copyright") &&
        !asset["copyright"].is_string()) {
        return false;
    }
    if (asset.contains("generator") &&
        !asset["generator"].is_string()) {
        return false;
    }
    return true;
}

bool isSupportedExtensionName(const std::string& name) {
    static constexpr std::array<const char*, 13> kSupportedExtensions = {
        "KHR_texture_transform",
        "KHR_mesh_quantization",
        "KHR_materials_unlit",
        "KHR_materials_emissive_strength",
        "KHR_materials_ior",
        "KHR_materials_pbrSpecularGlossiness",
        "KHR_materials_transmission",
        "KHR_materials_anisotropy",
        "KHR_materials_specular",
        "KHR_materials_clearcoat",
        "KHR_materials_sheen",
        "EXT_texture_webp",
        "EXT_mesh_gpu_instancing"};
    return std::find(
        kSupportedExtensions.begin(),
        kSupportedExtensions.end(),
        name) != kSupportedExtensions.end();
}

bool hasUnsupportedDeclaredExtensions(const json& doc) {
    auto checkArray = [](const json& doc, const char* name) {
        const auto extensionsIt = doc.find(name);
        if (extensionsIt == doc.end()) {
            return false;
        }
        const json& extensions = *extensionsIt;
        if (!extensions.is_array()) {
            return true;
        }
        std::unordered_set<std::string> seenExtensions;
        seenExtensions.reserve(extensions.size());
        for (const json& extension : extensions) {
            if (!extension.is_string()) {
                return true;
            }
            const std::string extensionName = extension.get<std::string>();
            if (!seenExtensions.insert(extensionName).second) {
                return true;
            }
            if (!isSupportedExtensionName(extensionName)) {
                return true;
            }
        }
        return false;
    };
    return checkArray(doc, "extensionsRequired") ||
           checkArray(doc, "extensionsUsed");
}

bool requiredExtensionsAreUsed(const json& doc) {
    const auto requiredIt = doc.find("extensionsRequired");
    if (requiredIt == doc.end()) {
        return true;
    }
    if (!requiredIt->is_array()) {
        return false;
    }
    if (requiredIt->empty()) {
        return true;
    }
    const auto usedIt = doc.find("extensionsUsed");
    if (usedIt == doc.end() || !usedIt->is_array()) {
        return false;
    }
    std::unordered_set<std::string> usedExtensions;
    usedExtensions.reserve(usedIt->size());
    for (const json& extension : *usedIt) {
        if (!extension.is_string()) {
            return false;
        }
        usedExtensions.insert(extension.get<std::string>());
    }
    for (const json& extension : *requiredIt) {
        if (!extension.is_string()) {
            return false;
        }
        if (usedExtensions.find(extension.get<std::string>()) ==
            usedExtensions.end()) {
            return false;
        }
    }
    return true;
}

bool declaredExtension(const json& doc, const char* extensionName) {
    auto checkArray = [extensionName](const json& extensions) {
        if (!extensions.is_array()) {
            return false;
        }
        return std::any_of(
            extensions.begin(),
            extensions.end(),
            [extensionName](const json& extension) {
                return extension.is_string() &&
                       extension.get<std::string>() == extensionName;
            });
    };
    return checkArray(doc.value("extensionsRequired", json::array())) ||
           checkArray(doc.value("extensionsUsed", json::array()));
}

bool meshQuantizationDeclarationComponentType(int componentType) {
    return componentType == 5120 ||
           componentType == 5121 ||
           componentType == 5122 ||
           componentType == 5123;
}

bool accessorUsesMeshQuantization(
    const json& accessors,
    const std::string& semantic,
    const json& accessorIndexJson) {
    if (!accessorIndexJson.is_number_integer()) {
        return false;
    }
    const int accessorIndex = accessorIndexJson.get<int>();
    if (accessorIndex < 0 ||
        static_cast<size_t>(accessorIndex) >= accessors.size()) {
        return false;
    }

    const json& accessor = accessors[static_cast<size_t>(accessorIndex)];
    if (!accessor.is_object()) {
        return false;
    }
    const auto componentTypeIt = accessor.find("componentType");
    const auto typeIt = accessor.find("type");
    if (componentTypeIt == accessor.end() ||
        typeIt == accessor.end() ||
        !componentTypeIt->is_number_integer() ||
        !typeIt->is_string()) {
        return false;
    }

    const int componentType = componentTypeIt->get<int>();
    if (!meshQuantizationDeclarationComponentType(componentType)) {
        return false;
    }

    const std::string type = typeIt->get<std::string>();
    if (semantic == "POSITION" || semantic == "NORMAL") {
        return type == "VEC3";
    }
    if (semantic == "TANGENT") {
        return type == "VEC4";
    }
    if (!texCoordSetIndexFromSemantic(semantic) || type != "VEC2") {
        return false;
    }

    bool normalized = false;
    const auto normalizedIt = accessor.find("normalized");
    if (normalizedIt != accessor.end()) {
        if (!normalizedIt->is_boolean()) {
            return false;
        }
        normalized = normalizedIt->get<bool>();
    }
    return !normalized || (componentType != 5121 && componentType != 5123);
}

bool documentUsesMeshQuantization(const json& doc) {
    const auto meshesIt = doc.find("meshes");
    const auto accessorsIt = doc.find("accessors");
    if (meshesIt == doc.end() ||
        accessorsIt == doc.end() ||
        !meshesIt->is_array() ||
        !accessorsIt->is_array()) {
        return false;
    }

    for (const json& mesh : *meshesIt) {
        if (!mesh.is_object()) {
            continue;
        }
        const auto primitivesIt = mesh.find("primitives");
        if (primitivesIt == mesh.end() || !primitivesIt->is_array()) {
            continue;
        }
        for (const json& primitive : *primitivesIt) {
            if (!primitive.is_object()) {
                continue;
            }
            const auto attributesIt = primitive.find("attributes");
            if (attributesIt == primitive.end() || !attributesIt->is_object()) {
                continue;
            }
            for (auto it = attributesIt->begin();
                 it != attributesIt->end();
                 ++it) {
                if (accessorUsesMeshQuantization(
                        *accessorsIt,
                        it.key(),
                        it.value())) {
                    return true;
                }
            }
        }
    }
    return false;
}

bool isTextureInfoExtensionParentPath(const std::vector<std::string>& path) {
    if (path.size() == 4 &&
        path[0] == "materials" &&
        path[2] == "pbrMetallicRoughness" &&
        (path[3] == "baseColorTexture" ||
         path[3] == "metallicRoughnessTexture")) {
        return true;
    }
    return (path.size() == 3 &&
            path[0] == "materials" &&
            (path[2] == "normalTexture" ||
             path[2] == "occlusionTexture" ||
             path[2] == "emissiveTexture")) ||
           (path.size() == 5 &&
            path[0] == "materials" &&
            path[2] == "extensions" &&
            ((path[3] == "KHR_materials_pbrSpecularGlossiness" &&
              (path[4] == "diffuseTexture" ||
               path[4] == "specularGlossinessTexture")) ||
             (path[3] == "KHR_materials_transmission" &&
              path[4] == "transmissionTexture") ||
             (path[3] == "KHR_materials_anisotropy" &&
              path[4] == "anisotropyTexture") ||
             (path[3] == "KHR_materials_specular" &&
              (path[4] == "specularTexture" ||
               path[4] == "specularColorTexture")) ||
             (path[3] == "KHR_materials_clearcoat" &&
              (path[4] == "clearcoatTexture" ||
               path[4] == "clearcoatRoughnessTexture" ||
               path[4] == "clearcoatNormalTexture")) ||
             (path[3] == "KHR_materials_sheen" &&
              (path[4] == "sheenColorTexture" ||
               path[4] == "sheenRoughnessTexture"))));
}

bool isMaterialExtensionParentPath(const std::vector<std::string>& path) {
    return path.size() == 2 &&
           path[0] == "materials";
}

bool isNodeExtensionParentPath(const std::vector<std::string>& path) {
    return path.size() == 2 &&
           path[0] == "nodes";
}

bool isTextureExtensionParentPath(const std::vector<std::string>& path) {
    return path.size() == 2 &&
           path[0] == "textures";
}

bool extensionsObjectSupportedAtPath(
    const json& extensions,
    const std::vector<std::string>& ownerPath) {
    if (!extensions.is_object()) {
        return false;
    }
    for (auto it = extensions.begin(); it != extensions.end(); ++it) {
        if (it.key() == "KHR_texture_transform") {
            if (!isTextureInfoExtensionParentPath(ownerPath)) {
                return false;
            }
            continue;
        }
        if (it.key() == "KHR_materials_unlit" ||
            it.key() == "KHR_materials_emissive_strength" ||
            it.key() == "KHR_materials_ior" ||
            it.key() == "KHR_materials_pbrSpecularGlossiness" ||
            it.key() == "KHR_materials_transmission" ||
            it.key() == "KHR_materials_anisotropy" ||
            it.key() == "KHR_materials_specular" ||
            it.key() == "KHR_materials_clearcoat" ||
            it.key() == "KHR_materials_sheen") {
            if (!isMaterialExtensionParentPath(ownerPath)) {
                return false;
            }
            continue;
        }
        if (it.key() == "EXT_mesh_gpu_instancing") {
            if (!isNodeExtensionParentPath(ownerPath)) {
                return false;
            }
            continue;
        }
        if (it.key() == "EXT_texture_webp") {
            if (!isTextureExtensionParentPath(ownerPath)) {
                return false;
            }
            continue;
        }
        return false;
    }
    return true;
}

bool hasUnsupportedObjectExtensions(
    const json& value,
    std::vector<std::string>& path) {
    if (value.is_object()) {
        auto extensionsIt = value.find("extensions");
        if (extensionsIt != value.end() &&
            !extensionsObjectSupportedAtPath(*extensionsIt, path)) {
            return true;
        }
        for (auto it = value.begin(); it != value.end(); ++it) {
            if (it.key() == "extras") {
                continue;
            }
            path.push_back(it.key());
            if (hasUnsupportedObjectExtensions(*it, path)) {
                return true;
            }
            path.pop_back();
        }
        return false;
    }
    if (value.is_array()) {
        for (const json& element : value) {
            path.push_back("*");
            if (hasUnsupportedObjectExtensions(element, path)) {
                return true;
            }
            path.pop_back();
        }
    }
    return false;
}

bool hasUnsupportedObjectExtensions(const json& doc) {
    std::vector<std::string> path;
    return hasUnsupportedObjectExtensions(doc, path);
}

bool documentHasObjectExtension(
    const json& value,
    const std::string& extensionName) {
    if (value.is_object()) {
        auto extensionsIt = value.find("extensions");
        if (extensionsIt != value.end() && extensionsIt->is_object() &&
            extensionsIt->contains(extensionName)) {
            return true;
        }
        for (auto it = value.begin(); it != value.end(); ++it) {
            if (it.key() == "extras") {
                continue;
            }
            if (documentHasObjectExtension(*it, extensionName)) {
                return true;
            }
        }
        return false;
    }
    if (value.is_array()) {
        for (const json& element : value) {
            if (documentHasObjectExtension(element, extensionName)) {
                return true;
            }
        }
    }
    return false;
}

bool supportedObjectExtensionsAreDeclared(const json& doc) {
    constexpr std::array<const char*, 12> kObjectExtensions = {
        "KHR_texture_transform",
        "KHR_materials_unlit",
        "KHR_materials_emissive_strength",
        "KHR_materials_ior",
        "KHR_materials_pbrSpecularGlossiness",
        "KHR_materials_transmission",
        "KHR_materials_anisotropy",
        "KHR_materials_specular",
        "KHR_materials_clearcoat",
        "KHR_materials_sheen",
        "EXT_texture_webp",
        "EXT_mesh_gpu_instancing"};
    for (const char* extensionName : kObjectExtensions) {
        if (documentHasObjectExtension(doc, extensionName) &&
            !declaredExtension(doc, extensionName)) {
            return false;
        }
    }
    return true;
}

std::optional<std::vector<std::vector<uint8_t>>> loadBuffers(
    const json& doc,
    const std::vector<uint8_t>& binaryChunk,
    const GltfParser::ExternalResourceResolver& externalResourceResolver) {
    std::vector<std::vector<uint8_t>> buffers;
    const auto bufferArrayIt = doc.find("buffers");
    if (bufferArrayIt == doc.end()) {
        return buffers;
    }
    if (!bufferArrayIt->is_array()) {
        return std::nullopt;
    }
    const json& bufferArray = *bufferArrayIt;
    buffers.reserve(bufferArray.size());
    for (size_t i = 0; i < bufferArray.size(); ++i) {
        const json& bufferJson = bufferArray[i];
        if (!bufferJson.is_object()) {
            return std::nullopt;
        }

        std::vector<uint8_t> buffer;
        std::string uri;
        const auto uriIt = bufferJson.find("uri");
        if (uriIt != bufferJson.end()) {
            if (!uriIt->is_string()) {
                return std::nullopt;
            }
            uri = uriIt->get<std::string>();
        }

        if (uri.empty() && i == 0) {
            buffer = binaryChunk;
        } else if (!uri.empty()) {
            if (auto decoded = decodeBase64DataUri(uri)) {
                buffer = std::move(*decoded);
            } else if (uri.rfind("data:", 0) == 0) {
                return std::nullopt;
            } else if (externalResourceResolver) {
                buffer = externalResourceResolver(uri);
            }
        }

        const auto byteLengthIt = bufferJson.find("byteLength");
        if (byteLengthIt == bufferJson.end()) {
            return std::nullopt;
        }
        auto parsedLength = jsonSizeValue(*byteLengthIt);
        if (!parsedLength || *parsedLength == 0) {
            return std::nullopt;
        }
        const size_t declaredLength = *parsedLength;
        if (buffer.size() < declaredLength) {
            return std::nullopt;
        }
        if (buffer.size() > declaredLength) {
            buffer.resize(declaredLength);
        }
        buffers.push_back(std::move(buffer));
    }
    return buffers;
}

std::optional<std::vector<uint8_t>> bufferViewBytes(
    const json& doc,
    const std::vector<std::vector<uint8_t>>& buffers,
    int bufferViewIndex) {
    const auto bufferViewsIt = doc.find("bufferViews");
    if (bufferViewsIt == doc.end() || !bufferViewsIt->is_array()) {
        return std::nullopt;
    }
    const json& bufferViews = *bufferViewsIt;
    if (bufferViewIndex < 0 ||
        static_cast<size_t>(bufferViewIndex) >= bufferViews.size()) {
        return std::nullopt;
    }

    const json& view = bufferViews[static_cast<size_t>(bufferViewIndex)];
    if (!view.is_object()) {
        return std::nullopt;
    }
    const auto bufferIt = view.find("buffer");
    if (bufferIt == view.end() || !bufferIt->is_number_integer()) {
        return std::nullopt;
    }
    const int bufferIndex = bufferIt->get<int>();
    if (bufferIndex < 0 || static_cast<size_t>(bufferIndex) >= buffers.size()) {
        return std::nullopt;
    }

    size_t offset = 0;
    const auto offsetIt = view.find("byteOffset");
    if (offsetIt != view.end()) {
        auto parsedOffset = jsonSizeValue(*offsetIt);
        if (!parsedOffset) {
            return std::nullopt;
        }
        offset = *parsedOffset;
    }

    const auto lengthIt = view.find("byteLength");
    if (lengthIt == view.end()) {
        return std::nullopt;
    }
    auto parsedLength = jsonSizeValue(*lengthIt);
    if (!parsedLength) {
        return std::nullopt;
    }
    const size_t length = *parsedLength;
    const std::vector<uint8_t>& buffer = buffers[static_cast<size_t>(bufferIndex)];
    if (length == 0 ||
        offset > buffer.size() ||
        length > buffer.size() - offset) {
        return std::nullopt;
    }
    return std::vector<uint8_t>(
        buffer.begin() + static_cast<std::ptrdiff_t>(offset),
        buffer.begin() + static_cast<std::ptrdiff_t>(offset + length));
}

std::optional<std::vector<uint8_t>> embeddedImageBytes(
    const json& doc,
    const std::vector<std::vector<uint8_t>>& buffers,
    const json& imageJson,
    bool allowWebp = false) {
    auto bufferViewIt = imageJson.find("bufferView");
    if (bufferViewIt == imageJson.end()) {
        return std::nullopt;
    }
    auto mimeTypeIt = imageJson.find("mimeType");
    if (!bufferViewIt->is_number_integer() ||
        mimeTypeIt == imageJson.end() ||
        !mimeTypeIt->is_string()) {
        return std::nullopt;
    }
    const std::string mimeType = mimeTypeIt->get<std::string>();
    if (!supportedImageMimeType(mimeType, allowWebp)) {
        return std::nullopt;
    }
    return bufferViewBytes(doc, buffers, bufferViewIt->get<int>());
}

std::optional<GltfTextureFilter> parseMagFilter(int filter) {
    if (filter == 9728) return GltfTextureFilter::Nearest;
    if (filter == 9729) return GltfTextureFilter::Linear;
    return std::nullopt;
}

std::optional<std::pair<GltfTextureFilter, bool>> parseMinFilter(int filter) {
    switch (filter) {
        case 9728:
            return std::make_pair(GltfTextureFilter::Nearest, false);
        case 9729:
            return std::make_pair(GltfTextureFilter::Linear, false);
        case 9984:
            return std::make_pair(GltfTextureFilter::Nearest, true);
        case 9985:
            return std::make_pair(GltfTextureFilter::Nearest, true);
        case 9986:
            return std::make_pair(GltfTextureFilter::Linear, true);
        case 9987:
            return std::make_pair(GltfTextureFilter::Linear, true);
        default:
            return std::nullopt;
    }
}

std::optional<GltfTextureWrap> parseWrap(int wrap) {
    switch (wrap) {
        case 33071:
            return GltfTextureWrap::ClampToEdge;
        case 33648:
            return GltfTextureWrap::MirroredRepeat;
        case 10497:
            return GltfTextureWrap::Repeat;
        default:
            return std::nullopt;
    }
}

bool validRenderablePrimitiveMode(int primitiveMode) {
    return primitiveMode >= 0 &&
           primitiveMode <= 6;
}

std::optional<GltfPrimitiveMode> renderModeForPrimitiveMode(
    int primitiveMode) {
    switch (primitiveMode) {
        case 0:
            return GltfPrimitiveMode::Points;
        case 1:
        case 2:
            return GltfPrimitiveMode::Lines;
        case 3:
            return GltfPrimitiveMode::LineStrip;
        case 4:
        case 5:
        case 6:
            return GltfPrimitiveMode::Triangles;
        default:
            return std::nullopt;
    }
}

bool trianglePrimitiveMode(int primitiveMode) {
    return primitiveMode == 4 ||
           primitiveMode == 5 ||
           primitiveMode == 6;
}

std::optional<int> samplerIntProperty(
    const json& object,
    const char* name,
    int defaultValue) {
    auto it = object.find(name);
    if (it == object.end()) {
        return defaultValue;
    }
    if (!it->is_number_integer()) {
        return std::nullopt;
    }
    return it->get<int>();
}

bool validateSamplerJson(const json& samplerJson) {
    if (!samplerJson.is_object()) {
        return false;
    }
    auto minFilterValue = samplerIntProperty(samplerJson, "minFilter", 9987);
    auto magFilterValue = samplerIntProperty(samplerJson, "magFilter", 9729);
    auto wrapSValue = samplerIntProperty(samplerJson, "wrapS", 10497);
    auto wrapTValue = samplerIntProperty(samplerJson, "wrapT", 10497);
    if (!minFilterValue || !magFilterValue || !wrapSValue || !wrapTValue) {
        return false;
    }
    return parseMinFilter(*minFilterValue).has_value() &&
           parseMagFilter(*magFilterValue).has_value() &&
           parseWrap(*wrapSValue).has_value() &&
           parseWrap(*wrapTValue).has_value();
}

std::optional<GltfSampler> parseSampler(const json& doc, int samplerIndex) {
    GltfSampler sampler;
    if (samplerIndex < 0) {
        return sampler;
    }

    const auto& samplers = doc.value("samplers", json::array());
    if (!samplers.is_array() ||
        static_cast<size_t>(samplerIndex) >= samplers.size()) {
        return std::nullopt;
    }

    const json& samplerJson = samplers[static_cast<size_t>(samplerIndex)];
    if (!validateSamplerJson(samplerJson)) {
        return std::nullopt;
    }
    auto minFilterValue = samplerIntProperty(samplerJson, "minFilter", 9987);
    auto magFilterValue = samplerIntProperty(samplerJson, "magFilter", 9729);
    auto wrapSValue = samplerIntProperty(samplerJson, "wrapS", 10497);
    auto wrapTValue = samplerIntProperty(samplerJson, "wrapT", 10497);
    if (!minFilterValue || !magFilterValue || !wrapSValue || !wrapTValue) {
        return std::nullopt;
    }

    const auto min = parseMinFilter(*minFilterValue);
    const auto mag = parseMagFilter(*magFilterValue);
    const auto wrapS = parseWrap(*wrapSValue);
    const auto wrapT = parseWrap(*wrapTValue);
    if (!min || !mag || !wrapS || !wrapT) {
        return std::nullopt;
    }

    sampler.minFilter = min->first;
    sampler.mipmap = min->second;
    sampler.magFilter = *mag;
    sampler.wrapS = *wrapS;
    sampler.wrapT = *wrapT;
    return sampler;
}

bool validImageSourceFields(const json& imageJson, bool allowWebp = false) {
    if (!imageJson.is_object()) {
        return false;
    }

    const auto uriIt = imageJson.find("uri");
    const auto bufferViewIt = imageJson.find("bufferView");
    const auto mimeTypeIt = imageJson.find("mimeType");

    if (uriIt != imageJson.end() && !uriIt->is_string()) {
        return false;
    }
    if (bufferViewIt != imageJson.end() && !bufferViewIt->is_number_integer()) {
        return false;
    }
    if (mimeTypeIt != imageJson.end() && !mimeTypeIt->is_string()) {
        return false;
    }
    if (uriIt == imageJson.end() && bufferViewIt == imageJson.end()) {
        return false;
    }
    if (uriIt != imageJson.end() && uriIt->get<std::string>().empty()) {
        return false;
    }
    if (bufferViewIt != imageJson.end() && bufferViewIt->get<int>() < 0) {
        return false;
    }
    if (mimeTypeIt != imageJson.end() &&
        !supportedImageMimeType(mimeTypeIt->get<std::string>(), allowWebp)) {
        return false;
    }
    if (uriIt != imageJson.end() &&
        !validImageDataUriSource(uriIt->get<std::string>(), allowWebp)) {
        return false;
    }
    if (uriIt != imageJson.end() &&
        imageUriUsesUnsupportedFormat(uriIt->get<std::string>(), allowWebp)) {
        return false;
    }
    if (uriIt != imageJson.end() && bufferViewIt != imageJson.end()) {
        return false;
    }
    if (bufferViewIt != imageJson.end()) {
        if (mimeTypeIt == imageJson.end()) {
            return false;
        }
        const std::string mimeType = mimeTypeIt->get<std::string>();
        return supportedImageMimeType(mimeType, allowWebp);
    }
    return true;
}

bool imageBufferViewReferenceInRange(const json& doc, const json& imageJson) {
    const auto bufferViewIt = imageJson.find("bufferView");
    if (bufferViewIt == imageJson.end()) {
        return true;
    }
    if (!bufferViewIt->is_number_integer()) {
        return false;
    }
    const int bufferViewIndex = bufferViewIt->get<int>();
    if (bufferViewIndex < 0) {
        return false;
    }
    const auto bufferViewsIt = doc.find("bufferViews");
    if (bufferViewsIt == doc.end() || !bufferViewsIt->is_array()) {
        return false;
    }
    if (static_cast<size_t>(bufferViewIndex) >= bufferViewsIt->size()) {
        return false;
    }
    return (*bufferViewsIt)[static_cast<size_t>(bufferViewIndex)].is_object();
}

bool imageSourceDeclaresNonWebp(const json& imageJson) {
    const auto mimeTypeIt = imageJson.find("mimeType");
    if (mimeTypeIt != imageJson.end() &&
        mimeTypeIt->is_string() &&
        mimeTypeIt->get<std::string>() != "image/webp") {
        return true;
    }
    const auto uriIt = imageJson.find("uri");
    if (uriIt != imageJson.end() && uriIt->is_string()) {
        const std::string uri = uriIt->get<std::string>();
        if (uriPathEndsWith(uri, ".png") ||
            uriPathEndsWith(uri, ".jpg") ||
            uriPathEndsWith(uri, ".jpeg")) {
            return true;
        }
        const auto mimeType = dataUriMimeType(uriIt->get<std::string>());
        return mimeType && *mimeType != "image/webp";
    }
    return false;
}

std::optional<int> webpTextureSourceIndex(const json& textureJson) {
    const auto extensionsIt = textureJson.find("extensions");
    if (extensionsIt == textureJson.end()) {
        return std::nullopt;
    }
    if (!extensionsIt->is_object()) {
        return std::nullopt;
    }
    const auto webpIt = extensionsIt->find("EXT_texture_webp");
    if (webpIt == extensionsIt->end()) {
        return std::nullopt;
    }
    if (!webpIt->is_object()) {
        return -1;
    }
    const auto sourceIt = webpIt->find("source");
    if (sourceIt == webpIt->end() || !sourceIt->is_number_integer()) {
        return -1;
    }
    return sourceIt->get<int>();
}

bool validCoreTextureSource(const json& imageArray, const json& sourceJson) {
    if (!sourceJson.is_number_integer()) {
        return false;
    }
    const int sourceIndex = sourceJson.get<int>();
    if (sourceIndex < 0) {
        return false;
    }
    if (!imageArray.is_array()) {
        return false;
    }
    if (static_cast<size_t>(sourceIndex) >= imageArray.size()) {
        return false;
    }
    return validImageSourceFields(
        imageArray[static_cast<size_t>(sourceIndex)],
        false);
}

std::optional<std::vector<GltfTexture>> loadTextures(
    const json& doc,
    const std::vector<std::vector<uint8_t>>& buffers,
    const GltfParser::ExternalResourceResolver& externalResourceResolver,
    const GltfParser::ImageDecoder& imageDecoder) {
    std::vector<GltfTexture> result;
    const auto textureArrayIt = doc.find("textures");
    if (textureArrayIt == doc.end()) return result;
    if (!textureArrayIt->is_array()) return std::nullopt;

    const json& textureArray = *textureArrayIt;
    if (textureArray.empty()) return result;

    result.resize(textureArray.size());

    const auto& imageArray = doc.value("images", json::array());
    for (size_t textureIndex = 0; textureIndex < textureArray.size(); ++textureIndex) {
        const json& textureJson = textureArray[textureIndex];
        if (!textureJson.is_object()) {
            return std::nullopt;
        }

        int samplerIndex = -1;
        const auto samplerIt = textureJson.find("sampler");
        if (samplerIt != textureJson.end()) {
            if (!samplerIt->is_number_integer()) {
                return std::nullopt;
            }
            samplerIndex = samplerIt->get<int>();
            if (samplerIndex < 0) {
                return std::nullopt;
            }
        }
        auto sampler = parseSampler(doc, samplerIndex);
        if (!sampler) {
            return std::nullopt;
        }
        result[textureIndex].sampler = *sampler;

        const std::optional<int> webpSourceIndex =
            webpTextureSourceIndex(textureJson);
        const bool useWebpSource = webpSourceIndex.has_value();
        const auto sourceIt = textureJson.find("source");
        if (sourceIt != textureJson.end() &&
            !validCoreTextureSource(imageArray, *sourceIt)) {
            return std::nullopt;
        }
        if (!useWebpSource && sourceIt == textureJson.end()) {
            continue;
        }
        const int sourceIndex = useWebpSource
            ? *webpSourceIndex
            : sourceIt->get<int>();
        if (sourceIndex < 0) {
            return std::nullopt;
        }
        if (!imageArray.is_array()) {
            return std::nullopt;
        }
        if (static_cast<size_t>(sourceIndex) >= imageArray.size()) {
            return std::nullopt;
        }

        const json& imageJson = imageArray[static_cast<size_t>(sourceIndex)];
        if (!validImageSourceFields(imageJson, useWebpSource)) {
            return std::nullopt;
        }
        if (useWebpSource && imageSourceDeclaresNonWebp(imageJson)) {
            return std::nullopt;
        }
        if (!imageDecoder) {
            continue;
        }

        std::vector<uint8_t> encoded;
        const auto uriIt = imageJson.find("uri");
        const auto bufferViewIt = imageJson.find("bufferView");
        if (uriIt != imageJson.end() && !uriIt->get<std::string>().empty()) {
            const std::string uri = uriIt->get<std::string>();
            if (auto decoded = decodeBase64DataUri(uri)) {
                encoded = std::move(*decoded);
            } else if (externalResourceResolver) {
                encoded = externalResourceResolver(uri);
            }
        } else if (bufferViewIt != imageJson.end()) {
            auto bytes = embeddedImageBytes(
                doc,
                buffers,
                imageJson,
                useWebpSource);
            if (!bytes) {
                return std::nullopt;
            }
            encoded = std::move(*bytes);
        }

        if (encoded.empty()) {
            return std::nullopt;
        }
        if (useWebpSource && !encodedImageLooksLikeWebp(encoded)) {
            return std::nullopt;
        }
        if (encodedImageUsesUnsupportedFormat(encoded, useWebpSource)) {
            return std::nullopt;
        }
        std::optional<GltfImage> image =
            imageDecoder(encoded.data(), encoded.size());
        if (!image || image->width <= 0 || image->height <= 0 ||
            (image->channels != 3 && image->channels != 4) ||
            image->pixels.empty()) {
            return std::nullopt;
        }
        result[textureIndex].image = std::move(*image);
    }
    return result;
}

template <size_t N>
bool readFloatArray(const json& value, std::array<float, N>& out) {
    if (!value.is_array() || value.size() != N) {
        return false;
    }
    std::array<float, N> parsed = out;
    for (size_t i = 0; i < N; ++i) {
        if (!value[i].is_number()) {
            return false;
        }
        const double raw = value[i].get<double>();
        const float converted = static_cast<float>(raw);
        if (!std::isfinite(raw) || !std::isfinite(converted)) {
            return false;
        }
        parsed[i] = converted;
    }
    out = parsed;
    return true;
}

std::optional<int> integerProperty(
    const json& object,
    const char* name,
    int defaultValue) {
    auto it = object.find(name);
    if (it == object.end()) {
        return defaultValue;
    }
    if (!it->is_number_integer()) {
        return std::nullopt;
    }
    return it->get<int>();
}

std::optional<float> numberProperty(
    const json& object,
    const char* name,
    float defaultValue) {
    auto it = object.find(name);
    if (it == object.end()) {
        return defaultValue;
    }
    if (!it->is_number()) {
        return std::nullopt;
    }
    const double raw = it->get<double>();
    const float converted = static_cast<float>(raw);
    if (!std::isfinite(raw) || !std::isfinite(converted)) {
        return std::nullopt;
    }
    return converted;
}

std::optional<bool> boolProperty(
    const json& object,
    const char* name,
    bool defaultValue) {
    auto it = object.find(name);
    if (it == object.end()) {
        return defaultValue;
    }
    if (!it->is_boolean()) {
        return std::nullopt;
    }
    return it->get<bool>();
}

std::optional<std::string> stringProperty(
    const json& object,
    const char* name,
    std::string defaultValue) {
    auto it = object.find(name);
    if (it == object.end()) {
        return defaultValue;
    }
    if (!it->is_string()) {
        return std::nullopt;
    }
    return it->get<std::string>();
}

bool parseTextureTransform(
    const json& textureInfo,
    int textureInfoTexCoord,
    GltfTextureTransform& transform,
    int& resolvedTexCoord) {
    resolvedTexCoord = textureInfoTexCoord;

    auto extensionsIt = textureInfo.find("extensions");
    if (extensionsIt == textureInfo.end()) {
        return true;
    }
    if (!extensionsIt->is_object()) {
        return false;
    }

    for (auto it = extensionsIt->begin(); it != extensionsIt->end(); ++it) {
        if (it.key() != "KHR_texture_transform") {
            return false;
        }
    }

    auto transformIt = extensionsIt->find("KHR_texture_transform");
    if (transformIt == extensionsIt->end()) {
        return true;
    }
    if (!transformIt->is_object()) {
        return false;
    }

    const json& transformJson = *transformIt;
    if (transformJson.contains("offset") &&
        !readFloatArray(transformJson["offset"], transform.offset)) {
        return false;
    }
    if (transformJson.contains("scale") &&
        !readFloatArray(transformJson["scale"], transform.scale)) {
        return false;
    }
    if (auto rotation = numberProperty(transformJson, "rotation", 0.0f)) {
        transform.rotation = *rotation;
    } else {
        return false;
    }
    if (auto texCoord = integerProperty(
            transformJson,
            "texCoord",
            textureInfoTexCoord)) {
        resolvedTexCoord = *texCoord;
    } else {
        return false;
    }
    return true;
}

std::optional<GltfTextureBinding> parseTextureBinding(
    const json& textureInfo,
    const std::vector<GltfTexture>& textures,
    bool& strictFailure) {
    if (!textureInfo.is_object()) {
        strictFailure = true;
        return std::nullopt;
    }
    auto textureIndex = integerProperty(textureInfo, "index", -1);
    auto texCoord = integerProperty(textureInfo, "texCoord", 0);
    if (!textureIndex || !texCoord) {
        strictFailure = true;
        return std::nullopt;
    }

    GltfTextureTransform transform;
    int resolvedTexCoord = *texCoord;
    if (!parseTextureTransform(
            textureInfo,
            *texCoord,
            transform,
            resolvedTexCoord)) {
        strictFailure = true;
        return std::nullopt;
    }
    if (!validTexCoordSetIndex(resolvedTexCoord) ||
        *textureIndex < 0 ||
        static_cast<size_t>(*textureIndex) >= textures.size()) {
        strictFailure = true;
        return std::nullopt;
    }

    const GltfTexture& texture = textures[static_cast<size_t>(*textureIndex)];
    if (texture.image.width <= 0 ||
        texture.image.height <= 0 ||
        texture.image.pixels.empty()) {
        strictFailure = true;
        return std::nullopt;
    }

    GltfTextureBinding binding;
    binding.textureIndex = static_cast<size_t>(*textureIndex);
    binding.texCoord = resolvedTexCoord;
    binding.transform = transform;
    return binding;
}

struct AccessorSpan {
    const uint8_t* base = nullptr;
    size_t count = 0;
    int componentType = 0;
    int components = 0;
    std::string type;
    size_t elementBytes = 0;
    size_t stride = 0;
    std::array<size_t, 16> componentOffsets{};
    bool normalized = false;
    std::vector<uint8_t> ownedData;
};

std::optional<size_t> jsonSizeValue(const json& value) {
    if (!value.is_number_integer()) {
        return std::nullopt;
    }
    const int64_t signedValue = value.get<int64_t>();
    if (signedValue < 0) {
        return std::nullopt;
    }
    return static_cast<size_t>(signedValue);
}

std::optional<size_t> jsonSizeProperty(const json& object,
                                       const char* name,
                                       size_t defaultValue) {
    auto it = object.find(name);
    if (it == object.end()) {
        return defaultValue;
    }
    return jsonSizeValue(*it);
}

std::optional<int> jsonIntProperty(const json& object, const char* name) {
    auto it = object.find(name);
    if (it == object.end() || !it->is_number_integer()) {
        return std::nullopt;
    }
    return it->get<int>();
}

const uint8_t* accessorElementPtr(const AccessorSpan& span, size_t index) {
    const uint8_t* base = span.ownedData.empty()
        ? span.base
        : span.ownedData.data();
    return base + index * span.stride;
}

const uint8_t* accessorComponentPtr(
    const AccessorSpan& span,
    size_t index,
    int component) {
    return accessorElementPtr(span, index) +
           span.componentOffsets[static_cast<size_t>(component)];
}

bool rangeWithin(size_t offset, size_t length, size_t total) {
    return offset <= total && length <= total - offset;
}

std::optional<size_t> accessorRequiredBytes(
    size_t count,
    size_t stride,
    size_t elementBytes) {
    if (count == 0u || stride == 0u || elementBytes == 0u) {
        return std::nullopt;
    }
    const size_t trailingLimit =
        std::numeric_limits<size_t>::max() - elementBytes;
    if ((count - 1u) > trailingLimit / stride) {
        return std::nullopt;
    }
    return (count - 1u) * stride + elementBytes;
}

bool alignedByteOffset(
    size_t bufferViewOffset,
    size_t extraByteOffset,
    size_t alignment) {
    return alignment > 0u &&
           extraByteOffset <=
               std::numeric_limits<size_t>::max() - bufferViewOffset &&
           ((bufferViewOffset + extraByteOffset) % alignment) == 0u;
}

std::optional<std::pair<const uint8_t*, size_t>> bufferViewRange(
    const json& doc,
    const std::vector<std::vector<uint8_t>>& buffers,
    int bufferViewIndex,
    size_t extraByteOffset,
    bool rejectSparseForbiddenFields) {
    const auto bufferViewsIt = doc.find("bufferViews");
    if (bufferViewsIt == doc.end() || !bufferViewsIt->is_array()) {
        return std::nullopt;
    }
    const json& bufferViews = *bufferViewsIt;
    if (bufferViewIndex < 0 ||
        static_cast<size_t>(bufferViewIndex) >= bufferViews.size()) {
        return std::nullopt;
    }
    const json& view = bufferViews[static_cast<size_t>(bufferViewIndex)];
    if (!view.is_object()) {
        return std::nullopt;
    }
    if (rejectSparseForbiddenFields &&
        (view.contains("byteStride") || view.contains("target"))) {
        return std::nullopt;
    }
    auto bufferIndex = jsonIntProperty(view, "buffer");
    auto viewOffset = jsonSizeProperty(view, "byteOffset", 0u);
    auto byteLength = jsonSizeProperty(view, "byteLength", 0u);
    if (!bufferIndex || !viewOffset || !byteLength ||
        *bufferIndex < 0 ||
        static_cast<size_t>(*bufferIndex) >= buffers.size()) {
        return std::nullopt;
    }
    const std::vector<uint8_t>& buffer = buffers[static_cast<size_t>(*bufferIndex)];
    if (!rangeWithin(*viewOffset, *byteLength, buffer.size()) ||
        extraByteOffset > *byteLength) {
        return std::nullopt;
    }
    return std::make_pair(
        buffer.data() + *viewOffset + extraByteOffset,
        *byteLength - extraByteOffset);
}

std::optional<size_t> bufferViewByteOffset(
    const json& doc,
    int bufferViewIndex) {
    const auto bufferViewsIt = doc.find("bufferViews");
    if (bufferViewsIt == doc.end() || !bufferViewsIt->is_array()) {
        return std::nullopt;
    }
    const json& bufferViews = *bufferViewsIt;
    if (bufferViewIndex < 0 ||
        static_cast<size_t>(bufferViewIndex) >= bufferViews.size()) {
        return std::nullopt;
    }
    if (!bufferViews[static_cast<size_t>(bufferViewIndex)].is_object()) {
        return std::nullopt;
    }
    return jsonSizeProperty(
        bufferViews[static_cast<size_t>(bufferViewIndex)],
        "byteOffset",
        0u);
}

void materializeAccessorData(AccessorSpan& span) {
    if (!span.ownedData.empty()) {
        return;
    }
    std::vector<uint8_t> data(span.count * span.elementBytes, 0u);
    if (span.base) {
        for (size_t i = 0; i < span.count; ++i) {
            std::memcpy(
                data.data() + i * span.elementBytes,
                span.base + i * span.stride,
                span.elementBytes);
        }
    }
    span.ownedData = std::move(data);
    span.base = nullptr;
    span.stride = span.elementBytes;
}

bool applySparseAccessor(
    const json& doc,
    const std::vector<std::vector<uint8_t>>& buffers,
    const json& accessor,
    AccessorSpan& span,
    bool materializeValues = true) {
    auto sparseIt = accessor.find("sparse");
    if (sparseIt == accessor.end()) {
        return true;
    }
    if (!sparseIt->is_object()) {
        return false;
    }
    const json& sparse = *sparseIt;
    auto sparseCount = jsonSizeProperty(sparse, "count", 0u);
    if (!sparseCount || *sparseCount > span.count) {
        return false;
    }
    if (*sparseCount == 0) {
        return true;
    }
    auto indicesIt = sparse.find("indices");
    auto valuesIt = sparse.find("values");
    if (indicesIt == sparse.end() || valuesIt == sparse.end() ||
        !indicesIt->is_object() || !valuesIt->is_object()) {
        return false;
    }

    const auto indexComponentType =
        jsonIntProperty(*indicesIt, "componentType");
    if (!indexComponentType ||
        (*indexComponentType != 5121 &&
         *indexComponentType != 5123 &&
         *indexComponentType != 5125)) {
        return false;
    }
    const size_t indexComponentBytes = componentSize(*indexComponentType);
    auto indicesBufferView = jsonIntProperty(*indicesIt, "bufferView");
    auto indicesByteOffset =
        jsonSizeProperty(*indicesIt, "byteOffset", 0u);
    auto valuesBufferView = jsonIntProperty(*valuesIt, "bufferView");
    auto valuesByteOffset = jsonSizeProperty(*valuesIt, "byteOffset", 0u);
    if (!indicesBufferView || !indicesByteOffset ||
        !valuesBufferView || !valuesByteOffset ||
        (*indicesByteOffset % indexComponentBytes) != 0u) {
        return false;
    }

    const auto indicesRange = bufferViewRange(
        doc,
        buffers,
        *indicesBufferView,
        *indicesByteOffset,
        true);
    const auto valuesRange = bufferViewRange(
        doc,
        buffers,
        *valuesBufferView,
        *valuesByteOffset,
        true);
    const auto indicesViewOffset =
        bufferViewByteOffset(doc, *indicesBufferView);
    const auto valuesViewOffset =
        bufferViewByteOffset(doc, *valuesBufferView);
    const size_t valueComponentBytes = componentSize(span.componentType);
    if (!indicesRange || !valuesRange ||
        !indicesViewOffset || !valuesViewOffset ||
        !alignedByteOffset(
            *indicesViewOffset,
            *indicesByteOffset,
            indexComponentBytes) ||
        !alignedByteOffset(
            *valuesViewOffset,
            *valuesByteOffset,
            valueComponentBytes) ||
        *sparseCount > indicesRange->second / indexComponentBytes ||
        *sparseCount > valuesRange->second / span.elementBytes) {
        return false;
    }

    if (materializeValues) {
        materializeAccessorData(span);
    }
    uint32_t previousIndex = 0;
    bool hasPrevious = false;
    for (size_t sparseElement = 0;
         sparseElement < *sparseCount;
         ++sparseElement) {
        const uint32_t targetIndex = readIndexComponent(
            indicesRange->first + sparseElement * indexComponentBytes,
            *indexComponentType);
        if (targetIndex >= span.count ||
            (hasPrevious && targetIndex <= previousIndex)) {
            return false;
        }
        hasPrevious = true;
        previousIndex = targetIndex;
        if (materializeValues) {
            std::memcpy(
                span.ownedData.data() +
                    static_cast<size_t>(targetIndex) * span.elementBytes,
                valuesRange->first + sparseElement * span.elementBytes,
                span.elementBytes);
        }
    }
    return true;
}

std::optional<AccessorSpan> accessorSpan(
    const json& doc,
    const std::vector<std::vector<uint8_t>>& buffers,
    int accessorIndex) {
    const auto accessorsIt = doc.find("accessors");
    if (accessorsIt == doc.end() || !accessorsIt->is_array()) {
        return std::nullopt;
    }
    const json& accessors = *accessorsIt;
    if (accessorIndex < 0 ||
        static_cast<size_t>(accessorIndex) >= accessors.size()) {
        return std::nullopt;
    }

    const json& accessor = accessors[static_cast<size_t>(accessorIndex)];
    if (!accessor.is_object()) {
        return std::nullopt;
    }
    const auto componentTypeValue = jsonIntProperty(accessor, "componentType");
    if (!componentTypeValue) {
        return std::nullopt;
    }
    const int componentType = *componentTypeValue;
    const size_t componentBytes = componentSize(componentType);
    const auto typeIt = accessor.find("type");
    if (typeIt == accessor.end() || !typeIt->is_string()) {
        return std::nullopt;
    }
    const std::string accessorType = typeIt->get<std::string>();
    const auto layout = accessorElementLayoutForType(
        accessorType,
        componentBytes);
    const auto countValue = jsonSizeProperty(accessor, "count", 0u);
    const auto accessorOffset = jsonSizeProperty(accessor, "byteOffset", 0u);
    if (!countValue || !accessorOffset) {
        return std::nullopt;
    }
    const size_t count = *countValue;
    if (!layout || count == 0) {
        return std::nullopt;
    }
    if ((*accessorOffset % componentBytes) != 0u) {
        return std::nullopt;
    }
    if (accessor.contains("normalized") &&
        !accessor["normalized"].is_boolean()) {
        return std::nullopt;
    }
    const bool normalized = accessor.contains("normalized")
        ? accessor["normalized"].get<bool>()
        : false;
    if (normalized && (componentType == 5125 || componentType == 5126)) {
        return std::nullopt;
    }

    AccessorSpan span;
    span.count = count;
    span.componentType = componentType;
    span.components = layout->components;
    span.type = accessorType;
    span.elementBytes = layout->elementBytes;
    span.componentOffsets = layout->componentOffsets;
    span.normalized = normalized;

    if (accessor.contains("bufferView")) {
        if (!accessor["bufferView"].is_number_integer()) {
            return std::nullopt;
        }
        const int bufferViewIndex = accessor["bufferView"].get<int>();
        const auto bufferViewsIt = doc.find("bufferViews");
        if (bufferViewsIt == doc.end() || !bufferViewsIt->is_array()) {
            return std::nullopt;
        }
        const json& bufferViews = *bufferViewsIt;
        if (bufferViewIndex < 0 ||
            static_cast<size_t>(bufferViewIndex) >= bufferViews.size()) {
            return std::nullopt;
        }
        const json& view = bufferViews[static_cast<size_t>(bufferViewIndex)];
        if (!view.is_object()) {
            return std::nullopt;
        }
        const auto viewOffset = jsonSizeProperty(view, "byteOffset", 0u);
        const auto byteStride =
            jsonSizeProperty(view, "byteStride", span.elementBytes);
        if (!viewOffset || !byteStride ||
            !alignedByteOffset(*viewOffset, *accessorOffset, componentBytes)) {
            return std::nullopt;
        }
        const bool explicitStride =
            view.contains("byteStride") && *byteStride != 0u;
        const size_t stride =
            *byteStride == 0u ? span.elementBytes : *byteStride;
        if (stride < span.elementBytes ||
            (explicitStride &&
             (stride < 4u || stride > 252u ||
              (stride % 4u) != 0u))) {
            return std::nullopt;
        }
        const auto range = bufferViewRange(
            doc,
            buffers,
            bufferViewIndex,
            *accessorOffset,
            false);
        if (!range) {
            return std::nullopt;
        }
        const auto required =
            accessorRequiredBytes(count, stride, span.elementBytes);
        if (!required || *required > range->second) {
            return std::nullopt;
        }
        span.base = range->first;
        span.stride = stride;
    } else {
        if (accessor.contains("byteOffset")) {
            return std::nullopt;
        }
        span.ownedData.assign(count * span.elementBytes, 0u);
        span.stride = span.elementBytes;
    }

    if (!applySparseAccessor(doc, buffers, accessor, span)) {
        return std::nullopt;
    }
    return span;
}

bool validateAllBufferViews(
    const json& doc,
    const std::vector<std::vector<uint8_t>>& buffers) {
    const auto bufferViewsIt = doc.find("bufferViews");
    if (bufferViewsIt == doc.end()) {
        return true;
    }
    if (!bufferViewsIt->is_array()) {
        return false;
    }

    for (const json& view : *bufferViewsIt) {
        if (!view.is_object()) {
            return false;
        }
        const auto bufferIndex = jsonIntProperty(view, "buffer");
        const auto byteOffset = jsonSizeProperty(view, "byteOffset", 0u);
        const auto byteLengthIt = view.find("byteLength");
        if (byteLengthIt == view.end()) {
            return false;
        }
        const auto byteLength = jsonSizeValue(*byteLengthIt);
        if (!bufferIndex ||
            !byteOffset ||
            !byteLength ||
            *byteLength == 0u ||
            *bufferIndex < 0 ||
            static_cast<size_t>(*bufferIndex) >= buffers.size()) {
            return false;
        }
        if (!rangeWithin(
                *byteOffset,
                *byteLength,
                buffers[static_cast<size_t>(*bufferIndex)].size())) {
            return false;
        }
        if (view.contains("byteStride")) {
            const auto byteStride = jsonSizeValue(view["byteStride"]);
            if (!byteStride ||
                *byteStride < 4u ||
                *byteStride > 252u ||
                (*byteStride % 4u) != 0u) {
                return false;
            }
        }
        if (view.contains("target")) {
            const auto target = jsonIntProperty(view, "target");
            if (!target || (*target != 34962 && *target != 34963)) {
                return false;
            }
        }
    }
    return true;
}

bool validateAccessorJson(
    const json& doc,
    const std::vector<std::vector<uint8_t>>& buffers,
    int accessorIndex) {
    const auto accessorsIt = doc.find("accessors");
    if (accessorsIt == doc.end() || !accessorsIt->is_array()) {
        return false;
    }
    const json& accessors = *accessorsIt;
    if (accessorIndex < 0 ||
        static_cast<size_t>(accessorIndex) >= accessors.size()) {
        return false;
    }

    const json& accessor = accessors[static_cast<size_t>(accessorIndex)];
    if (!accessor.is_object()) {
        return false;
    }
    const auto componentTypeValue = jsonIntProperty(accessor, "componentType");
    if (!componentTypeValue) {
        return false;
    }
    const int componentType = *componentTypeValue;
    const size_t componentBytes = componentSize(componentType);
    const auto typeIt = accessor.find("type");
    if (typeIt == accessor.end() || !typeIt->is_string()) {
        return false;
    }
    const std::string accessorType = typeIt->get<std::string>();
    const auto layout = accessorElementLayoutForType(
        accessorType,
        componentBytes);
    const auto countValue = jsonSizeProperty(accessor, "count", 0u);
    const auto accessorOffset = jsonSizeProperty(accessor, "byteOffset", 0u);
    if (!countValue || !accessorOffset) {
        return false;
    }
    const size_t count = *countValue;
    if (!layout || count == 0 || (*accessorOffset % componentBytes) != 0u) {
        return false;
    }
    if (accessor.contains("normalized") &&
        !accessor["normalized"].is_boolean()) {
        return false;
    }
    const bool normalized = accessor.contains("normalized")
        ? accessor["normalized"].get<bool>()
        : false;
    if (normalized && (componentType == 5125 || componentType == 5126)) {
        return false;
    }
    auto validBoundsValue = [componentType](const json& value) {
        if (!value.is_number()) {
            return false;
        }
        const double raw = value.get<double>();
        if (!std::isfinite(raw)) {
            return false;
        }
        switch (componentType) {
            case 5120:
                return raw >= -128.0 &&
                       raw <= 127.0 &&
                       std::trunc(raw) == raw;
            case 5121:
                return raw >= 0.0 &&
                       raw <= 255.0 &&
                       std::trunc(raw) == raw;
            case 5122:
                return raw >= -32768.0 &&
                       raw <= 32767.0 &&
                       std::trunc(raw) == raw;
            case 5123:
                return raw >= 0.0 &&
                       raw <= 65535.0 &&
                       std::trunc(raw) == raw;
            case 5125:
                return raw >= 0.0 &&
                       raw <= 4294967295.0 &&
                       std::trunc(raw) == raw;
            case 5126: {
                const float converted = static_cast<float>(raw);
                return std::isfinite(converted);
            }
            default:
                return false;
        }
    };
    auto validBoundsArray = [&](const char* name) {
        const auto boundsIt = accessor.find(name);
        if (boundsIt == accessor.end()) {
            return true;
        }
        if (!boundsIt->is_array() ||
            boundsIt->size() != static_cast<size_t>(layout->components)) {
            return false;
        }
        return std::all_of(
            boundsIt->begin(),
            boundsIt->end(),
            validBoundsValue);
    };
    if (!validBoundsArray("min") || !validBoundsArray("max")) {
        return false;
    }

    AccessorSpan span;
    span.count = count;
    span.componentType = componentType;
    span.components = layout->components;
    span.type = accessorType;
    span.elementBytes = layout->elementBytes;
    span.componentOffsets = layout->componentOffsets;
    span.normalized = normalized;

    if (accessor.contains("bufferView")) {
        if (!accessor["bufferView"].is_number_integer()) {
            return false;
        }
        const int bufferViewIndex = accessor["bufferView"].get<int>();
        const auto bufferViewsIt = doc.find("bufferViews");
        if (bufferViewsIt == doc.end() || !bufferViewsIt->is_array()) {
            return false;
        }
        const json& bufferViews = *bufferViewsIt;
        if (bufferViewIndex < 0 ||
            static_cast<size_t>(bufferViewIndex) >= bufferViews.size()) {
            return false;
        }
        const json& view = bufferViews[static_cast<size_t>(bufferViewIndex)];
        if (!view.is_object()) {
            return false;
        }
        const auto viewOffset = jsonSizeProperty(view, "byteOffset", 0u);
        const auto byteStride =
            jsonSizeProperty(view, "byteStride", span.elementBytes);
        if (!viewOffset || !byteStride ||
            !alignedByteOffset(*viewOffset, *accessorOffset, componentBytes)) {
            return false;
        }
        const bool explicitStride =
            view.contains("byteStride") && *byteStride != 0u;
        const size_t stride =
            *byteStride == 0u ? span.elementBytes : *byteStride;
        if (stride < span.elementBytes ||
            (explicitStride &&
             (stride < 4u || stride > 252u ||
              (stride % 4u) != 0u))) {
            return false;
        }
        const auto range = bufferViewRange(
            doc,
            buffers,
            bufferViewIndex,
            *accessorOffset,
            false);
        if (!range) {
            return false;
        }
        const auto required =
            accessorRequiredBytes(count, stride, span.elementBytes);
        if (!required || *required > range->second) {
            return false;
        }
        span.base = range->first;
        span.stride = stride;
    } else {
        if (accessor.contains("byteOffset")) {
            return false;
        }
        span.stride = span.elementBytes;
    }

    return applySparseAccessor(doc, buffers, accessor, span, false);
}

bool validateAllAccessors(
    const json& doc,
    const std::vector<std::vector<uint8_t>>& buffers) {
    const auto accessorsIt = doc.find("accessors");
    if (accessorsIt == doc.end()) {
        return true;
    }
    if (!accessorsIt->is_array()) {
        return false;
    }
    for (size_t i = 0; i < accessorsIt->size(); ++i) {
        if (!validateAccessorJson(
                doc,
                buffers,
                static_cast<int>(i))) {
            return false;
        }
    }
    return true;
}

glm::dvec3 readVec3(const AccessorSpan& span, size_t index) {
    return glm::dvec3(
        readComponent(
            accessorComponentPtr(span, index, 0),
            span.componentType,
            span.normalized),
        readComponent(
            accessorComponentPtr(span, index, 1),
            span.componentType,
            span.normalized),
        readComponent(
            accessorComponentPtr(span, index, 2),
            span.componentType,
            span.normalized));
}

glm::dvec2 readVec2(const AccessorSpan& span, size_t index) {
    return glm::dvec2(
        readComponent(
            accessorComponentPtr(span, index, 0),
            span.componentType,
            span.normalized),
        readComponent(
            accessorComponentPtr(span, index, 1),
            span.componentType,
            span.normalized));
}

glm::dvec4 readVec4(const AccessorSpan& span, size_t index) {
    return glm::dvec4(
        readComponent(
            accessorComponentPtr(span, index, 0),
            span.componentType,
            span.normalized),
        readComponent(
            accessorComponentPtr(span, index, 1),
            span.componentType,
            span.normalized),
        readComponent(
            accessorComponentPtr(span, index, 2),
            span.componentType,
            span.normalized),
        readComponent(
            accessorComponentPtr(span, index, 3),
            span.componentType,
            span.normalized));
}

glm::dmat4 readMat4(const AccessorSpan& span, size_t index) {
    glm::dmat4 matrix(1.0);
    for (int component = 0; component < 16; ++component) {
        matrix[component / 4][component % 4] =
            readComponent(
                accessorComponentPtr(span, index, component),
                span.componentType,
                span.normalized);
    }
    return matrix;
}

bool finiteVec2Value(const glm::dvec2& value) {
    return std::isfinite(value.x) &&
           std::isfinite(value.y);
}

bool finiteVec3Value(const glm::dvec3& value) {
    return std::isfinite(value.x) &&
           std::isfinite(value.y) &&
           std::isfinite(value.z);
}

bool finiteVec4Value(const glm::dvec4& value) {
    return std::isfinite(value.x) &&
           std::isfinite(value.y) &&
           std::isfinite(value.z) &&
           std::isfinite(value.w);
}

bool finiteMat4Value(const glm::dmat4& value) {
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            if (!std::isfinite(value[column][row])) {
                return false;
            }
        }
    }
    return true;
}

std::vector<uint32_t> readIndices(const AccessorSpan& span) {
    if (span.components != 1) return {};
    std::vector<uint32_t> indices;
    indices.reserve(span.count);
    for (size_t i = 0; i < span.count; ++i) {
        indices.push_back(readIndexComponent(
            accessorElementPtr(span, i),
            span.componentType));
    }
    return indices;
}

bool normalizeIndicesForPrimitiveMode(
    std::vector<uint32_t>& indices,
    int primitiveMode,
    GltfPrimitiveMode& renderMode) {
    const auto mode = renderModeForPrimitiveMode(primitiveMode);
    if (!mode || indices.empty()) {
        return false;
    }
    renderMode = *mode;
    if (primitiveMode == 0) {
        return true;
    }
    if (primitiveMode == 1) {
        return indices.size() >= 2u && (indices.size() % 2u) == 0u;
    }
    if (primitiveMode == 2) {
        if (indices.size() < 2u ||
            indices.size() >
                std::numeric_limits<size_t>::max() / 2u) {
            return false;
        }
        std::vector<uint32_t> lines;
        lines.reserve(indices.size() * 2u);
        for (size_t i = 0; i < indices.size(); ++i) {
            lines.push_back(indices[i]);
            lines.push_back(indices[(i + 1u) % indices.size()]);
        }
        indices = std::move(lines);
        renderMode = GltfPrimitiveMode::Lines;
        return true;
    }
    if (primitiveMode == 3) {
        return indices.size() >= 2u;
    }
    if (!trianglePrimitiveMode(primitiveMode)) {
        return false;
    }
    if (primitiveMode == 4) {
        return !indices.empty() && (indices.size() % 3u) == 0u;
    }
    if (indices.size() < 3u) {
        return false;
    }
    if (indices.size() >
        (std::numeric_limits<size_t>::max() / 3u) + 2u) {
        return false;
    }

    std::vector<uint32_t> triangles;
    triangles.reserve((indices.size() - 2u) * 3u);
    if (primitiveMode == 5) {
        for (size_t i = 0; i + 2u < indices.size(); ++i) {
            triangles.push_back(indices[i]);
            if ((i % 2u) == 0u) {
                triangles.push_back(indices[i + 1u]);
                triangles.push_back(indices[i + 2u]);
            } else {
                triangles.push_back(indices[i + 2u]);
                triangles.push_back(indices[i + 1u]);
            }
        }
    } else {
        for (size_t i = 1; i + 1u < indices.size(); ++i) {
            triangles.push_back(indices[0]);
            triangles.push_back(indices[i]);
            triangles.push_back(indices[i + 1u]);
        }
    }
    indices = std::move(triangles);
    return true;
}

bool jsonNumberArray(const json& value, size_t expectedSize) {
    if (!value.is_array() || value.size() != expectedSize) {
        return false;
    }
    return std::all_of(
        value.begin(),
        value.end(),
        [](const json& element) {
            return element.is_number() &&
                   std::isfinite(element.get<double>());
        });
}

bool jsonNumberArray(const json& value) {
    if (!value.is_array()) {
        return false;
    }
    return std::all_of(
        value.begin(),
        value.end(),
        [](const json& element) {
            return element.is_number() &&
                   std::isfinite(element.get<double>());
        });
}

bool jsonNumberArrayInRange(
    const json& value,
    size_t expectedSize,
    double minimum,
    double maximum) {
    if (!value.is_array() || value.size() != expectedSize) {
        return false;
    }
    return std::all_of(
        value.begin(),
        value.end(),
        [minimum, maximum](const json& element) {
            if (!element.is_number()) {
                return false;
            }
            const double value = element.get<double>();
            return std::isfinite(value) &&
                   value >= minimum &&
                   value <= maximum;
        });
}

bool jsonNumberArrayAtLeast(
    const json& value,
    size_t expectedSize,
    double minimum) {
    if (!value.is_array() || value.size() != expectedSize) {
        return false;
    }
    return std::all_of(
        value.begin(),
        value.end(),
        [minimum](const json& element) {
            if (!element.is_number()) {
                return false;
            }
            const double value = element.get<double>();
            return std::isfinite(value) && value >= minimum;
        });
}

bool jsonQuaternionArrayNonZero(const json& value) {
    if (!jsonNumberArray(value, 4u)) {
        return false;
    }
    const double x = value[0].get<double>();
    const double y = value[1].get<double>();
    const double z = value[2].get<double>();
    const double w = value[3].get<double>();
    const double lengthSquared = x * x + y * y + z * z + w * w;
    return std::isfinite(lengthSquared) && lengthSquared > 0.0;
}

bool jsonNodeIndexArray(const json& value, size_t nodeCount) {
    if (!value.is_array()) {
        return false;
    }
    for (const json& nodeIndex : value) {
        if (!nodeIndex.is_number_integer()) {
            return false;
        }
        const int index = nodeIndex.get<int>();
        if (index < 0 || static_cast<size_t>(index) >= nodeCount) {
            return false;
        }
    }
    return true;
}

bool validateTextureInfoShape(
    const json& textureInfo,
    size_t textureCount) {
    if (!textureInfo.is_object()) {
        return false;
    }
    const auto textureIndex = integerProperty(textureInfo, "index", -1);
    const auto texCoord = integerProperty(textureInfo, "texCoord", 0);
    if (!textureIndex ||
        !texCoord ||
        *textureIndex < 0 ||
        static_cast<size_t>(*textureIndex) >= textureCount ||
        !validTexCoordSetIndex(*texCoord)) {
        return false;
    }

    GltfTextureTransform transform;
    int resolvedTexCoord = *texCoord;
    if (!parseTextureTransform(
            textureInfo,
            *texCoord,
            transform,
            resolvedTexCoord)) {
        return false;
    }
    return validTexCoordSetIndex(resolvedTexCoord);
}

bool validateMaterialTextureInfo(
    const json& material,
    const char* propertyName,
    size_t textureCount) {
    const auto textureIt = material.find(propertyName);
    if (textureIt == material.end()) {
        return true;
    }
    return validateTextureInfoShape(*textureIt, textureCount);
}

bool materialHasKhrMaterialsUnlit(const json& material) {
    const auto extensionsIt = material.find("extensions");
    if (extensionsIt == material.end()) {
        return false;
    }
    if (!extensionsIt->is_object()) {
        return false;
    }
    const auto unlitIt = extensionsIt->find("KHR_materials_unlit");
    return unlitIt != extensionsIt->end() && unlitIt->is_object();
}

bool validMaterialIor(float ior) {
    return ior == 0.0f || ior >= 1.0f;
}

float dielectricSpecularF0FromIor(float ior) {
    if (ior == 0.0f) {
        return 1.0f;
    }
    const float f0 = (ior - 1.0f) / (ior + 1.0f);
    return f0 * f0;
}

bool validateMaterialExtensions(const json& material) {
    const auto extensionsIt = material.find("extensions");
    if (extensionsIt == material.end()) {
        return true;
    }
    if (!extensionsIt->is_object()) {
        return false;
    }
    const auto unlitIt = extensionsIt->find("KHR_materials_unlit");
    if (unlitIt != extensionsIt->end() && !unlitIt->is_object()) {
        return false;
    }
    const auto emissiveStrengthIt =
        extensionsIt->find("KHR_materials_emissive_strength");
    if (emissiveStrengthIt != extensionsIt->end()) {
        if (unlitIt != extensionsIt->end() ||
            !emissiveStrengthIt->is_object()) {
            return false;
        }
        const auto strengthIt = emissiveStrengthIt->find("emissiveStrength");
        if (strengthIt != emissiveStrengthIt->end()) {
            const auto strength = numberProperty(
                *emissiveStrengthIt,
                "emissiveStrength",
                1.0f);
            if (!strength || *strength < 0.0f) {
                return false;
            }
        }
    }

    const auto pbrSpecGlossIt =
        extensionsIt->find("KHR_materials_pbrSpecularGlossiness");
    if (pbrSpecGlossIt != extensionsIt->end()) {
        if (unlitIt != extensionsIt->end() ||
            extensionsIt->contains("KHR_materials_ior") ||
            extensionsIt->contains("KHR_materials_transmission") ||
            extensionsIt->contains("KHR_materials_anisotropy") ||
            extensionsIt->contains("KHR_materials_specular") ||
            extensionsIt->contains("KHR_materials_clearcoat") ||
            extensionsIt->contains("KHR_materials_sheen") ||
            !pbrSpecGlossIt->is_object()) {
            return false;
        }
        const auto diffuseFactorIt = pbrSpecGlossIt->find("diffuseFactor");
        if (diffuseFactorIt != pbrSpecGlossIt->end() &&
            !jsonNumberArrayInRange(*diffuseFactorIt, 4u, 0.0, 1.0)) {
            return false;
        }
        const auto specularFactorIt =
            pbrSpecGlossIt->find("specularFactor");
        if (specularFactorIt != pbrSpecGlossIt->end() &&
            !jsonNumberArrayInRange(*specularFactorIt, 3u, 0.0, 1.0)) {
            return false;
        }
        const auto glossinessIt =
            pbrSpecGlossIt->find("glossinessFactor");
        if (glossinessIt != pbrSpecGlossIt->end()) {
            const auto glossiness = numberProperty(
                *pbrSpecGlossIt,
                "glossinessFactor",
                1.0f);
            if (!glossiness || *glossiness < 0.0f || *glossiness > 1.0f) {
                return false;
            }
        }
    }

    const auto transmissionIt =
        extensionsIt->find("KHR_materials_transmission");
    if (transmissionIt != extensionsIt->end()) {
        if (unlitIt != extensionsIt->end() ||
            extensionsIt->contains("KHR_materials_pbrSpecularGlossiness") ||
            !transmissionIt->is_object()) {
            return false;
        }
        const auto factorIt = transmissionIt->find("transmissionFactor");
        if (factorIt != transmissionIt->end()) {
            const auto factor = numberProperty(
                *transmissionIt,
                "transmissionFactor",
                0.0f);
            if (!factor || *factor < 0.0f || *factor > 1.0f) {
                return false;
            }
        }
    }

    const auto anisotropyIt =
        extensionsIt->find("KHR_materials_anisotropy");
    if (anisotropyIt != extensionsIt->end()) {
        if (unlitIt != extensionsIt->end() ||
            extensionsIt->contains("KHR_materials_pbrSpecularGlossiness") ||
            !anisotropyIt->is_object()) {
            return false;
        }
        const auto strengthIt = anisotropyIt->find("anisotropyStrength");
        if (strengthIt != anisotropyIt->end()) {
            const auto strength =
                numberProperty(*anisotropyIt, "anisotropyStrength", 0.0f);
            if (!strength || *strength < 0.0f || *strength > 1.0f) {
                return false;
            }
        }
        const auto rotationIt = anisotropyIt->find("anisotropyRotation");
        if (rotationIt != anisotropyIt->end() &&
            !numberProperty(*anisotropyIt, "anisotropyRotation", 0.0f)) {
            return false;
        }
    }

    const auto specularIt = extensionsIt->find("KHR_materials_specular");
    if (specularIt != extensionsIt->end()) {
        if (unlitIt != extensionsIt->end() ||
            extensionsIt->contains("KHR_materials_pbrSpecularGlossiness") ||
            !specularIt->is_object()) {
            return false;
        }
        const auto factorIt = specularIt->find("specularFactor");
        if (factorIt != specularIt->end()) {
            const auto factor = numberProperty(
                *specularIt,
                "specularFactor",
                1.0f);
            if (!factor || *factor < 0.0f || *factor > 1.0f) {
                return false;
            }
        }
        const auto colorFactorIt = specularIt->find("specularColorFactor");
        if (colorFactorIt != specularIt->end() &&
            !jsonNumberArrayAtLeast(*colorFactorIt, 3u, 0.0)) {
            return false;
        }
    }

    const auto clearcoatIt = extensionsIt->find("KHR_materials_clearcoat");
    if (clearcoatIt != extensionsIt->end()) {
        if (unlitIt != extensionsIt->end() ||
            extensionsIt->contains("KHR_materials_pbrSpecularGlossiness") ||
            !clearcoatIt->is_object()) {
            return false;
        }
        const auto factorIt = clearcoatIt->find("clearcoatFactor");
        if (factorIt != clearcoatIt->end()) {
            const auto factor = numberProperty(
                *clearcoatIt,
                "clearcoatFactor",
                0.0f);
            if (!factor || *factor < 0.0f || *factor > 1.0f) {
                return false;
            }
        }
        const auto roughnessIt =
            clearcoatIt->find("clearcoatRoughnessFactor");
        if (roughnessIt != clearcoatIt->end()) {
            const auto roughness = numberProperty(
                *clearcoatIt,
                "clearcoatRoughnessFactor",
                0.0f);
            if (!roughness || *roughness < 0.0f || *roughness > 1.0f) {
                return false;
            }
        }
    }

    const auto sheenIt = extensionsIt->find("KHR_materials_sheen");
    if (sheenIt != extensionsIt->end()) {
        if (unlitIt != extensionsIt->end() ||
            extensionsIt->contains("KHR_materials_pbrSpecularGlossiness") ||
            !sheenIt->is_object()) {
            return false;
        }
        const auto colorFactorIt = sheenIt->find("sheenColorFactor");
        if (colorFactorIt != sheenIt->end() &&
            !jsonNumberArrayInRange(*colorFactorIt, 3u, 0.0, 1.0)) {
            return false;
        }
        const auto roughnessIt = sheenIt->find("sheenRoughnessFactor");
        if (roughnessIt != sheenIt->end()) {
            const auto roughness =
                numberProperty(*sheenIt, "sheenRoughnessFactor", 0.0f);
            if (!roughness || *roughness < 0.0f || *roughness > 1.0f) {
                return false;
            }
        }
    }

    const auto iorIt = extensionsIt->find("KHR_materials_ior");
    if (iorIt == extensionsIt->end()) {
        return true;
    }
    if (unlitIt != extensionsIt->end() || !iorIt->is_object()) {
        return false;
    }
    const auto iorPropertyIt = iorIt->find("ior");
    if (iorPropertyIt == iorIt->end()) {
        return true;
    }
    const auto ior = numberProperty(*iorIt, "ior", 1.5f);
    return ior.has_value() && validMaterialIor(*ior);
}

float materialEmissiveStrength(const json& material, bool& strictFailure) {
    const auto extensionsIt = material.find("extensions");
    if (extensionsIt == material.end() || !extensionsIt->is_object()) {
        return 1.0f;
    }
    const auto emissiveStrengthIt =
        extensionsIt->find("KHR_materials_emissive_strength");
    if (emissiveStrengthIt == extensionsIt->end()) {
        return 1.0f;
    }
    if (!emissiveStrengthIt->is_object()) {
        strictFailure = true;
        return 1.0f;
    }
    const auto strength = numberProperty(
        *emissiveStrengthIt,
        "emissiveStrength",
        1.0f);
    if (!strength || *strength < 0.0f) {
        strictFailure = true;
        return 1.0f;
    }
    return *strength;
}

float materialDielectricSpecularF0(const json& material, bool& strictFailure) {
    const auto extensionsIt = material.find("extensions");
    if (extensionsIt == material.end() || !extensionsIt->is_object()) {
        return dielectricSpecularF0FromIor(1.5f);
    }
    const auto iorIt = extensionsIt->find("KHR_materials_ior");
    if (iorIt == extensionsIt->end()) {
        return dielectricSpecularF0FromIor(1.5f);
    }
    if (!iorIt->is_object()) {
        strictFailure = true;
        return dielectricSpecularF0FromIor(1.5f);
    }
    const auto ior = numberProperty(*iorIt, "ior", 1.5f);
    if (!ior || !validMaterialIor(*ior)) {
        strictFailure = true;
        return dielectricSpecularF0FromIor(1.5f);
    }
    return dielectricSpecularF0FromIor(*ior);
}

const json* materialObjectExtension(
    const json& material,
    const char* extensionName) {
    const auto extensionsIt = material.find("extensions");
    if (extensionsIt == material.end() || !extensionsIt->is_object()) {
        return nullptr;
    }
    const auto extensionIt = extensionsIt->find(extensionName);
    if (extensionIt == extensionsIt->end()) {
        return nullptr;
    }
    if (!extensionIt->is_object()) {
        return nullptr;
    }
    return &*extensionIt;
}

float materialSpecularFactor(const json& material, bool& strictFailure) {
    const json* specular =
        materialObjectExtension(material, "KHR_materials_specular");
    if (!specular) {
        return 1.0f;
    }
    const auto factor = numberProperty(*specular, "specularFactor", 1.0f);
    if (!factor || *factor < 0.0f || *factor > 1.0f) {
        strictFailure = true;
        return 1.0f;
    }
    return *factor;
}

std::array<float, 3> materialSpecularColorFactor(
    const json& material,
    bool& strictFailure) {
    std::array<float, 3> factor = {1.0f, 1.0f, 1.0f};
    const json* specular =
        materialObjectExtension(material, "KHR_materials_specular");
    if (!specular) {
        return factor;
    }
    const auto factorIt = specular->find("specularColorFactor");
    if (factorIt == specular->end()) {
        return factor;
    }
    if (!readFloatArray(*factorIt, factor) ||
        std::any_of(factor.begin(), factor.end(), [](float component) {
            return component < 0.0f;
        })) {
        strictFailure = true;
        return {1.0f, 1.0f, 1.0f};
    }
    return factor;
}

std::array<float, 4> materialSpecularGlossinessDiffuseFactor(
    const json& material,
    bool& strictFailure) {
    std::array<float, 4> factor = {1.0f, 1.0f, 1.0f, 1.0f};
    const json* specGloss =
        materialObjectExtension(
            material,
            "KHR_materials_pbrSpecularGlossiness");
    if (!specGloss) {
        return factor;
    }
    const auto factorIt = specGloss->find("diffuseFactor");
    if (factorIt == specGloss->end()) {
        return factor;
    }
    if (!readFloatArray(*factorIt, factor) ||
        std::any_of(factor.begin(), factor.end(), [](float component) {
            return component < 0.0f || component > 1.0f;
        })) {
        strictFailure = true;
        return {1.0f, 1.0f, 1.0f, 1.0f};
    }
    return factor;
}

std::array<float, 3> materialSpecularGlossinessSpecularFactor(
    const json& material,
    bool& strictFailure) {
    std::array<float, 3> factor = {1.0f, 1.0f, 1.0f};
    const json* specGloss =
        materialObjectExtension(
            material,
            "KHR_materials_pbrSpecularGlossiness");
    if (!specGloss) {
        return factor;
    }
    const auto factorIt = specGloss->find("specularFactor");
    if (factorIt == specGloss->end()) {
        return factor;
    }
    if (!readFloatArray(*factorIt, factor) ||
        std::any_of(factor.begin(), factor.end(), [](float component) {
            return component < 0.0f || component > 1.0f;
        })) {
        strictFailure = true;
        return {1.0f, 1.0f, 1.0f};
    }
    return factor;
}

float materialSpecularGlossinessGlossinessFactor(
    const json& material,
    bool& strictFailure) {
    const json* specGloss =
        materialObjectExtension(
            material,
            "KHR_materials_pbrSpecularGlossiness");
    if (!specGloss) {
        return 1.0f;
    }
    const auto glossiness =
        numberProperty(*specGloss, "glossinessFactor", 1.0f);
    if (!glossiness || *glossiness < 0.0f || *glossiness > 1.0f) {
        strictFailure = true;
        return 1.0f;
    }
    return *glossiness;
}

float materialTransmissionFactor(
    const json& material,
    bool& strictFailure) {
    const json* transmission =
        materialObjectExtension(material, "KHR_materials_transmission");
    if (!transmission) {
        return 0.0f;
    }
    const auto factor =
        numberProperty(*transmission, "transmissionFactor", 0.0f);
    if (!factor || *factor < 0.0f || *factor > 1.0f) {
        strictFailure = true;
        return 0.0f;
    }
    return *factor;
}

float materialAnisotropyStrength(
    const json& material,
    bool& strictFailure) {
    const json* anisotropy =
        materialObjectExtension(material, "KHR_materials_anisotropy");
    if (!anisotropy) {
        return 0.0f;
    }
    const auto strength =
        numberProperty(*anisotropy, "anisotropyStrength", 0.0f);
    if (!strength || *strength < 0.0f || *strength > 1.0f) {
        strictFailure = true;
        return 0.0f;
    }
    return *strength;
}

float materialAnisotropyRotation(
    const json& material,
    bool& strictFailure) {
    const json* anisotropy =
        materialObjectExtension(material, "KHR_materials_anisotropy");
    if (!anisotropy) {
        return 0.0f;
    }
    const auto rotation =
        numberProperty(*anisotropy, "anisotropyRotation", 0.0f);
    if (!rotation) {
        strictFailure = true;
        return 0.0f;
    }
    return *rotation;
}

float materialClearcoatFactor(const json& material, bool& strictFailure) {
    const json* clearcoat =
        materialObjectExtension(material, "KHR_materials_clearcoat");
    if (!clearcoat) {
        return 0.0f;
    }
    const auto factor = numberProperty(*clearcoat, "clearcoatFactor", 0.0f);
    if (!factor || *factor < 0.0f || *factor > 1.0f) {
        strictFailure = true;
        return 0.0f;
    }
    return *factor;
}

float materialClearcoatRoughnessFactor(
    const json& material,
    bool& strictFailure) {
    const json* clearcoat =
        materialObjectExtension(material, "KHR_materials_clearcoat");
    if (!clearcoat) {
        return 0.0f;
    }
    const auto roughness =
        numberProperty(*clearcoat, "clearcoatRoughnessFactor", 0.0f);
    if (!roughness || *roughness < 0.0f || *roughness > 1.0f) {
        strictFailure = true;
        return 0.0f;
    }
    return *roughness;
}

std::array<float, 3> materialSheenColorFactor(
    const json& material,
    bool& strictFailure) {
    std::array<float, 3> factor = {0.0f, 0.0f, 0.0f};
    const json* sheen =
        materialObjectExtension(material, "KHR_materials_sheen");
    if (!sheen) {
        return factor;
    }
    const auto factorIt = sheen->find("sheenColorFactor");
    if (factorIt == sheen->end()) {
        return factor;
    }
    if (!readFloatArray(*factorIt, factor) ||
        std::any_of(factor.begin(), factor.end(), [](float component) {
            return component < 0.0f || component > 1.0f;
        })) {
        strictFailure = true;
        return {0.0f, 0.0f, 0.0f};
    }
    return factor;
}

float materialSheenRoughnessFactor(
    const json& material,
    bool& strictFailure) {
    const json* sheen =
        materialObjectExtension(material, "KHR_materials_sheen");
    if (!sheen) {
        return 0.0f;
    }
    const auto roughness =
        numberProperty(*sheen, "sheenRoughnessFactor", 0.0f);
    if (!roughness || *roughness < 0.0f || *roughness > 1.0f) {
        strictFailure = true;
        return 0.0f;
    }
    return *roughness;
}

bool validateMaterialJson(const json& material, size_t textureCount) {
    if (!material.is_object()) {
        return false;
    }
    if (!validateMaterialExtensions(material)) {
        return false;
    }
    const auto doubleSided =
        boolProperty(material, "doubleSided", false);
    if (!doubleSided) {
        return false;
    }

    const auto alphaMode =
        stringProperty(material, "alphaMode", std::string{"OPAQUE"});
    if (!alphaMode ||
        (*alphaMode != "OPAQUE" &&
         *alphaMode != "MASK" &&
         *alphaMode != "BLEND")) {
        return false;
    }
    const auto alphaCutoff = numberProperty(material, "alphaCutoff", 0.5f);
    if (!alphaCutoff || *alphaCutoff < 0.0f) {
        return false;
    }

    const auto pbrIt = material.find("pbrMetallicRoughness");
    if (pbrIt != material.end()) {
        if (!pbrIt->is_object()) {
            return false;
        }
        const json& pbr = *pbrIt;
        if (pbr.contains("baseColorFactor") &&
            !jsonNumberArrayInRange(
                pbr["baseColorFactor"],
                4u,
                0.0,
                1.0)) {
            return false;
        }
        const auto metallicFactor =
            numberProperty(pbr, "metallicFactor", 1.0f);
        const auto roughnessFactor =
            numberProperty(pbr, "roughnessFactor", 1.0f);
        if (!metallicFactor ||
            *metallicFactor < 0.0f ||
            *metallicFactor > 1.0f ||
            !roughnessFactor ||
            *roughnessFactor < 0.0f ||
            *roughnessFactor > 1.0f) {
            return false;
        }
        if (!validateMaterialTextureInfo(
                pbr,
                "baseColorTexture",
                textureCount) ||
            !validateMaterialTextureInfo(
                pbr,
                "metallicRoughnessTexture",
                textureCount)) {
            return false;
        }
    }

    const auto extensionsIt = material.find("extensions");
    if (extensionsIt != material.end() && extensionsIt->is_object()) {
        const auto pbrSpecGlossIt =
            extensionsIt->find("KHR_materials_pbrSpecularGlossiness");
        if (pbrSpecGlossIt != extensionsIt->end()) {
            if (!pbrSpecGlossIt->is_object() ||
                !validateMaterialTextureInfo(
                    *pbrSpecGlossIt,
                    "diffuseTexture",
                    textureCount) ||
                !validateMaterialTextureInfo(
                    *pbrSpecGlossIt,
                    "specularGlossinessTexture",
                    textureCount)) {
                return false;
            }
        }
        const auto transmissionIt =
            extensionsIt->find("KHR_materials_transmission");
        if (transmissionIt != extensionsIt->end()) {
            if (!transmissionIt->is_object() ||
                !validateMaterialTextureInfo(
                    *transmissionIt,
                    "transmissionTexture",
                    textureCount)) {
                return false;
            }
        }
        const auto anisotropyIt =
            extensionsIt->find("KHR_materials_anisotropy");
        if (anisotropyIt != extensionsIt->end()) {
            if (!anisotropyIt->is_object() ||
                !validateMaterialTextureInfo(
                    *anisotropyIt,
                    "anisotropyTexture",
                    textureCount)) {
                return false;
            }
        }
        const auto specularIt =
            extensionsIt->find("KHR_materials_specular");
        if (specularIt != extensionsIt->end()) {
            if (!specularIt->is_object() ||
                !validateMaterialTextureInfo(
                    *specularIt,
                    "specularTexture",
                    textureCount) ||
                !validateMaterialTextureInfo(
                    *specularIt,
                    "specularColorTexture",
                    textureCount)) {
                return false;
            }
        }
        const auto clearcoatIt =
            extensionsIt->find("KHR_materials_clearcoat");
        if (clearcoatIt != extensionsIt->end()) {
            if (!clearcoatIt->is_object() ||
                !validateMaterialTextureInfo(
                    *clearcoatIt,
                    "clearcoatTexture",
                    textureCount) ||
                !validateMaterialTextureInfo(
                    *clearcoatIt,
                    "clearcoatRoughnessTexture",
                    textureCount)) {
                return false;
            }
            const auto clearcoatNormalIt =
                clearcoatIt->find("clearcoatNormalTexture");
            if (clearcoatNormalIt != clearcoatIt->end()) {
                if (!validateTextureInfoShape(
                        *clearcoatNormalIt,
                        textureCount) ||
                    !numberProperty(
                        *clearcoatNormalIt,
                        "scale",
                        1.0f)) {
                    return false;
                }
            }
        }
        const auto sheenIt = extensionsIt->find("KHR_materials_sheen");
        if (sheenIt != extensionsIt->end()) {
            if (!sheenIt->is_object() ||
                !validateMaterialTextureInfo(
                    *sheenIt,
                    "sheenColorTexture",
                    textureCount) ||
                !validateMaterialTextureInfo(
                    *sheenIt,
                    "sheenRoughnessTexture",
                    textureCount)) {
                return false;
            }
        }
    }

    const auto normalIt = material.find("normalTexture");
    if (normalIt != material.end()) {
        if (!validateTextureInfoShape(*normalIt, textureCount) ||
            !numberProperty(*normalIt, "scale", 1.0f)) {
            return false;
        }
    }
    const auto occlusionIt = material.find("occlusionTexture");
    if (occlusionIt != material.end()) {
        const auto strength = numberProperty(*occlusionIt, "strength", 1.0f);
        if (!validateTextureInfoShape(*occlusionIt, textureCount) ||
            !strength ||
            *strength < 0.0f ||
            *strength > 1.0f) {
            return false;
        }
    }
    if (!validateMaterialTextureInfo(
            material,
            "emissiveTexture",
            textureCount)) {
        return false;
    }
    if (material.contains("emissiveFactor") &&
        !jsonNumberArrayInRange(
            material["emissiveFactor"],
            3u,
            0.0,
            1.0)) {
        return false;
    }
    return true;
}

bool gpuInstancingAttributeSemanticSupported(const std::string& semantic) {
    return semantic == "TRANSLATION" ||
           semantic == "ROTATION" ||
           semantic == "SCALE";
}

bool validateGpuInstancingNodeExtensionShape(
    const json& node,
    const json* accessors) {
    const auto extensionsIt = node.find("extensions");
    if (extensionsIt == node.end()) {
        return true;
    }
    if (!extensionsIt->is_object()) {
        return false;
    }
    const auto instancingIt =
        extensionsIt->find("EXT_mesh_gpu_instancing");
    if (instancingIt == extensionsIt->end()) {
        return true;
    }
    if (!node.contains("mesh") ||
        node.contains("skin") ||
        !instancingIt->is_object()) {
        return false;
    }
    const auto attributesIt = instancingIt->find("attributes");
    if (attributesIt == instancingIt->end() ||
        !attributesIt->is_object() ||
        attributesIt->empty()) {
        return false;
    }
    if (!accessors || !accessors->is_array()) {
        return false;
    }
    for (auto it = attributesIt->begin(); it != attributesIt->end(); ++it) {
        if (!gpuInstancingAttributeSemanticSupported(it.key()) ||
            !it.value().is_number_integer()) {
            return false;
        }
        const int accessorIndex = it.value().get<int>();
        if (accessorIndex < 0 ||
            static_cast<size_t>(accessorIndex) >= accessors->size()) {
            return false;
        }
    }
    return true;
}

bool validateSceneGraph(const json& doc,
                        bool allowLegacyBatchIdAttribute) {
    const auto nodesIt = doc.find("nodes");
    if (nodesIt != doc.end() && !nodesIt->is_array()) {
        return false;
    }
    const size_t nodeCount =
        nodesIt == doc.end() ? 0u : nodesIt->size();

    const auto meshesIt = doc.find("meshes");
    if (meshesIt != doc.end() && !meshesIt->is_array()) {
        return false;
    }
    const auto skinsIt = doc.find("skins");
    if (skinsIt != doc.end() && !skinsIt->is_array()) {
        return false;
    }
    const auto accessorsIt = doc.find("accessors");
    if (accessorsIt != doc.end() && !accessorsIt->is_array()) {
        return false;
    }
    const auto materialsIt = doc.find("materials");
    if (materialsIt != doc.end() && !materialsIt->is_array()) {
        return false;
    }
    if (doc.contains("cameras")) {
        return false;
    }
    const auto texturesIt = doc.find("textures");
    if (texturesIt != doc.end() && !texturesIt->is_array()) {
        return false;
    }
    const size_t textureCount =
        texturesIt == doc.end() ? 0u : texturesIt->size();
    const auto samplersIt = doc.find("samplers");
    if (samplersIt != doc.end()) {
        if (!samplersIt->is_array()) {
            return false;
        }
        for (const json& sampler : *samplersIt) {
            if (!validateSamplerJson(sampler)) {
                return false;
            }
        }
    }
    const auto imagesIt = doc.find("images");
    if (imagesIt != doc.end()) {
        if (!imagesIt->is_array()) {
            return false;
        }
        const bool webpImagesAllowed =
            declaredExtension(doc, "EXT_texture_webp");
        for (const json& image : *imagesIt) {
            if (!validImageSourceFields(image, webpImagesAllowed) ||
                !imageBufferViewReferenceInRange(doc, image)) {
                return false;
            }
        }
    }
    if (materialsIt != doc.end()) {
        for (const json& material : *materialsIt) {
            if (!validateMaterialJson(material, textureCount)) {
                return false;
            }
        }
    }

    if (nodesIt != doc.end()) {
        for (const json& node : *nodesIt) {
            if (!node.is_object()) {
                return false;
            }

            const bool hasMatrix = node.contains("matrix");
            const bool hasTrs =
                node.contains("translation") ||
                node.contains("rotation") ||
                node.contains("scale");
            if (hasMatrix && hasTrs) {
                return false;
            }
            if (hasMatrix && !jsonNumberArray(node["matrix"], 16u)) {
                return false;
            }
            if (node.contains("translation") &&
                !jsonNumberArray(node["translation"], 3u)) {
                return false;
            }
            if (node.contains("rotation") &&
                !jsonQuaternionArrayNonZero(node["rotation"])) {
                return false;
            }
            if (node.contains("scale") &&
                !jsonNumberArray(node["scale"], 3u)) {
                return false;
            }
            if (node.contains("weights") &&
                !jsonNumberArray(node["weights"])) {
                return false;
            }
            if (node.contains("camera")) {
                return false;
            }
            if (!validateGpuInstancingNodeExtensionShape(
                    node,
                    accessorsIt == doc.end() ? nullptr : &*accessorsIt)) {
                return false;
            }
            if (node.contains("children") &&
                !jsonNodeIndexArray(node["children"], nodeCount)) {
                return false;
            }
            if (node.contains("mesh")) {
                if (!node["mesh"].is_number_integer()) {
                    return false;
                }
                const int meshIndex = node["mesh"].get<int>();
                if (meshIndex < 0 ||
                    meshesIt == doc.end() ||
                    static_cast<size_t>(meshIndex) >= meshesIt->size()) {
                    return false;
                }
            }
            if (node.contains("skin")) {
                if (!node["skin"].is_number_integer()) {
                    return false;
                }
                if (!node.contains("mesh")) {
                    return false;
                }
                const int skinIndex = node["skin"].get<int>();
                if (skinIndex < 0 ||
                    skinsIt == doc.end() ||
                    static_cast<size_t>(skinIndex) >= skinsIt->size()) {
                    return false;
                }
            }
        }
    }

    if (meshesIt != doc.end()) {
        for (const json& mesh : *meshesIt) {
            if (!mesh.is_object()) {
                return false;
            }
            if (mesh.contains("weights") && !jsonNumberArray(mesh["weights"])) {
                return false;
            }
            const auto primitivesIt = mesh.find("primitives");
            if (primitivesIt == mesh.end() ||
                !primitivesIt->is_array() ||
                primitivesIt->empty()) {
                return false;
            }
            for (const json& primitive : *primitivesIt) {
                if (!primitive.is_object()) {
                    return false;
                }
                const auto attributesIt = primitive.find("attributes");
                if (attributesIt == primitive.end() ||
                    !attributesIt->is_object()) {
                    return false;
                }
                for (auto it = attributesIt->begin();
                     it != attributesIt->end();
                     ++it) {
                    if (!primitiveAttributeSemanticSupported(
                            it.key(),
                            allowLegacyBatchIdAttribute)) {
                        return false;
                    }
                    if (!it.value().is_number_integer()) {
                        return false;
                    }
                    const int accessorIndex = it.value().get<int>();
                    if (accessorIndex < 0 ||
                        accessorsIt == doc.end() ||
                        static_cast<size_t>(accessorIndex) >=
                            accessorsIt->size()) {
                        return false;
                    }
                }
                if (primitive.contains("indices")) {
                    if (!primitive["indices"].is_number_integer()) {
                        return false;
                    }
                    const int accessorIndex = primitive["indices"].get<int>();
                    if (accessorIndex < 0 ||
                        accessorsIt == doc.end() ||
                        static_cast<size_t>(accessorIndex) >=
                            accessorsIt->size()) {
                        return false;
                    }
                }
                if (primitive.contains("material")) {
                    if (!primitive["material"].is_number_integer()) {
                        return false;
                    }
                    const int materialIndex = primitive["material"].get<int>();
                    if (materialIndex < 0 ||
                        materialsIt == doc.end() ||
                        static_cast<size_t>(materialIndex) >=
                            materialsIt->size()) {
                        return false;
                    }
                }
                if (primitive.contains("mode") &&
                    !primitive["mode"].is_number_integer()) {
                    return false;
                }
                if (primitive.contains("mode")) {
                    const int primitiveMode = primitive["mode"].get<int>();
                    if (!validRenderablePrimitiveMode(primitiveMode)) {
                        return false;
                    }
                }
                if (primitive.contains("targets")) {
                    if (!primitive["targets"].is_array()) {
                        return false;
                    }
                    for (const json& target : primitive["targets"]) {
                        if (!target.is_object()) {
                            return false;
                        }
                        for (auto it = target.begin();
                             it != target.end();
                             ++it) {
                            if (!morphTargetSemanticSupported(it.key())) {
                                return false;
                            }
                            if (!it.value().is_number_integer()) {
                                return false;
                            }
                            const int accessorIndex = it.value().get<int>();
                            if (accessorIndex < 0 ||
                                accessorsIt == doc.end() ||
                                static_cast<size_t>(accessorIndex) >=
                                    accessorsIt->size()) {
                                return false;
                            }
                        }
                    }
                }
            }
        }
    }

    if (skinsIt != doc.end()) {
        for (const json& skin : *skinsIt) {
            if (!skin.is_object()) {
                return false;
            }
            const auto jointsIt = skin.find("joints");
            if (jointsIt == skin.end() ||
                !jsonNodeIndexArray(*jointsIt, nodeCount) ||
                jointsIt->empty()) {
                return false;
            }
            if (skin.contains("inverseBindMatrices")) {
                if (!skin["inverseBindMatrices"].is_number_integer()) {
                    return false;
                }
                const int accessorIndex = skin["inverseBindMatrices"].get<int>();
                if (accessorIndex < 0 ||
                    accessorsIt == doc.end() ||
                    static_cast<size_t>(accessorIndex) >= accessorsIt->size()) {
                    return false;
                }
            }
            if (skin.contains("skeleton")) {
                if (!skin["skeleton"].is_number_integer()) {
                    return false;
                }
                const int skeleton = skin["skeleton"].get<int>();
                if (skeleton < 0 ||
                    static_cast<size_t>(skeleton) >= nodeCount) {
                    return false;
                }
            }
        }
    }

    const auto scenesIt = doc.find("scenes");
    if (scenesIt != doc.end()) {
        if (!scenesIt->is_array()) {
            return false;
        }
        for (const json& scene : *scenesIt) {
            if (!scene.is_object()) {
                return false;
            }
            if (scene.contains("nodes") &&
                !jsonNodeIndexArray(scene["nodes"], nodeCount)) {
                return false;
            }
        }
    }
    if (doc.contains("scene")) {
        if (!doc["scene"].is_number_integer()) {
            return false;
        }
        const int sceneIndex = doc["scene"].get<int>();
        if (sceneIndex < 0 ||
            scenesIt == doc.end() ||
            static_cast<size_t>(sceneIndex) >= scenesIt->size()) {
            return false;
        }
    }
    return true;
}

glm::dquat normalizedRotationFromNodeJson(const json& node) {
    if (!node.contains("rotation") ||
        !node["rotation"].is_array() ||
        node["rotation"].size() != 4) {
        return glm::dquat(1.0, 0.0, 0.0, 0.0);
    }

    const glm::dquat rotation(
        node["rotation"][3].get<double>(),
        node["rotation"][0].get<double>(),
        node["rotation"][1].get<double>(),
        node["rotation"][2].get<double>());
    return glm::normalize(rotation);
}

glm::dmat4 nodeLocalTransform(const json& node) {
    if (node.contains("matrix") && node["matrix"].is_array() &&
        node["matrix"].size() == 16) {
        glm::dmat4 matrix(1.0);
        for (size_t i = 0; i < 16; ++i) {
            matrix[int(i / 4)][int(i % 4)] = node["matrix"][i].get<double>();
        }
        return matrix;
    }

    glm::dvec3 translation(0.0);
    if (node.contains("translation") && node["translation"].is_array() &&
        node["translation"].size() == 3) {
        translation = glm::dvec3(
            node["translation"][0].get<double>(),
            node["translation"][1].get<double>(),
            node["translation"][2].get<double>());
    }

    const glm::dquat rotation = normalizedRotationFromNodeJson(node);

    glm::dvec3 scale(1.0);
    if (node.contains("scale") && node["scale"].is_array() &&
        node["scale"].size() == 3) {
        scale = glm::dvec3(
            node["scale"][0].get<double>(),
            node["scale"][1].get<double>(),
            node["scale"][2].get<double>());
    }

    return glm::translate(glm::dmat4(1.0), translation) *
           glm::mat4_cast(rotation) *
           glm::scale(glm::dmat4(1.0), scale);
}

struct NodeRecord {
    glm::dmat4 localTransform = glm::dmat4(1.0);
    glm::dmat4 globalTransform = glm::dmat4(1.0);
    glm::dvec3 translation = glm::dvec3(0.0);
    glm::dquat rotation = glm::dquat(1.0, 0.0, 0.0, 0.0);
    glm::dvec3 scale = glm::dvec3(1.0);
    std::vector<double> weights;
    std::vector<int> children;
    int parent = -1;
    int mesh = -1;
    int skin = -1;
    bool hasMatrix = false;
    bool globalResolved = false;
};

struct SkinRecord {
    std::vector<int> joints;
    std::vector<glm::dmat4> inverseBindMatrices;
    bool valid = true;
};

void resolveNodeGlobalTransforms(
    const json& doc,
    int nodeIndex,
    const glm::dmat4& parentTransform,
    std::vector<NodeRecord>& nodes) {
    const auto& nodeArray = doc.value("nodes", json::array());
    if (nodeIndex < 0 || static_cast<size_t>(nodeIndex) >= nodes.size() ||
        static_cast<size_t>(nodeIndex) >= nodeArray.size()) {
        return;
    }

    NodeRecord& record = nodes[static_cast<size_t>(nodeIndex)];
    record.globalTransform = parentTransform * record.localTransform;
    record.globalResolved = true;
    const json& node = nodeArray[static_cast<size_t>(nodeIndex)];
    for (const json& child : node.value("children", json::array())) {
        const int childIndex = child.get<int>();
        if (childIndex >= 0 && static_cast<size_t>(childIndex) < nodes.size()) {
            nodes[static_cast<size_t>(childIndex)].parent = nodeIndex;
        }
        resolveNodeGlobalTransforms(
            doc,
            childIndex,
            record.globalTransform,
            nodes);
    }
}

std::vector<NodeRecord> parseNodes(const json& doc) {
    const auto& nodeArray = doc.value("nodes", json::array());
    std::vector<NodeRecord> nodes(nodeArray.size());
    for (size_t i = 0; i < nodeArray.size(); ++i) {
        if (!nodeArray[i].is_object()) continue;
        const json& node = nodeArray[i];
        nodes[i].localTransform = nodeLocalTransform(node);
        nodes[i].hasMatrix =
            node.contains("matrix") && node["matrix"].is_array();
        if (!nodes[i].hasMatrix) {
            if (node.contains("translation") &&
                node["translation"].is_array() &&
                node["translation"].size() == 3) {
                nodes[i].translation = glm::dvec3(
                    node["translation"][0].get<double>(),
                    node["translation"][1].get<double>(),
                    node["translation"][2].get<double>());
            }
            if (node.contains("rotation") &&
                node["rotation"].is_array() &&
                node["rotation"].size() == 4) {
                nodes[i].rotation = normalizedRotationFromNodeJson(node);
            }
            if (node.contains("scale") &&
                node["scale"].is_array() &&
                node["scale"].size() == 3) {
                nodes[i].scale = glm::dvec3(
                    node["scale"][0].get<double>(),
                    node["scale"][1].get<double>(),
                    node["scale"][2].get<double>());
            }
        }
        nodes[i].mesh = node.value("mesh", -1);
        nodes[i].skin = node.value("skin", -1);
        if (node.contains("weights") && node["weights"].is_array()) {
            nodes[i].weights.reserve(node["weights"].size());
            for (const json& weight : node["weights"]) {
                nodes[i].weights.push_back(weight.get<double>());
            }
        }
        for (const json& child : node.value("children", json::array())) {
            if (child.is_number_integer()) {
                nodes[i].children.push_back(child.get<int>());
            }
        }
    }

    const auto& scenes = doc.value("scenes", json::array());
    const int defaultScene = doc.value("scene", 0);
    if (!scenes.empty() &&
        defaultScene >= 0 &&
        static_cast<size_t>(defaultScene) < scenes.size()) {
        for (const json& node : scenes[static_cast<size_t>(defaultScene)]
                 .value("nodes", json::array())) {
            resolveNodeGlobalTransforms(
                doc,
                node.get<int>(),
                glm::dmat4(1.0),
                nodes);
        }
    }

    for (size_t i = 0; i < nodes.size(); ++i) {
        if (!nodes[i].globalResolved) {
            resolveNodeGlobalTransforms(
                doc,
                static_cast<int>(i),
                glm::dmat4(1.0),
                nodes);
        }
    }
    return nodes;
}

size_t meshMorphTargetCount(const json& doc, int meshIndex) {
    const auto& meshes = doc.value("meshes", json::array());
    if (meshIndex < 0 || static_cast<size_t>(meshIndex) >= meshes.size()) {
        return 0;
    }
    size_t targetCount = 0;
    const auto& primitives =
        meshes[static_cast<size_t>(meshIndex)].value(
            "primitives",
            json::array());
    for (const json& primitive : primitives) {
        const auto& targets = primitive.value("targets", json::array());
        if (targets.is_array()) {
            targetCount = std::max(targetCount, targets.size());
        }
    }
    return targetCount;
}

void applyMeshDefaultWeights(const json& doc, std::vector<NodeRecord>& nodes) {
    const auto& meshes = doc.value("meshes", json::array());
    for (NodeRecord& node : nodes) {
        if (node.mesh < 0 ||
            static_cast<size_t>(node.mesh) >= meshes.size() ||
            !node.weights.empty()) {
            continue;
        }
        const json& mesh = meshes[static_cast<size_t>(node.mesh)];
        if (mesh.contains("weights") && mesh["weights"].is_array()) {
            node.weights.reserve(mesh["weights"].size());
            for (const json& weight : mesh["weights"]) {
                node.weights.push_back(weight.get<double>());
            }
        } else {
            node.weights.assign(meshMorphTargetCount(doc, node.mesh), 0.0);
        }
    }
}

std::vector<SkinRecord> parseSkins(
    const json& doc,
    const std::vector<std::vector<uint8_t>>& buffers,
    size_t nodeCount) {
    const auto& skinArray = doc.value("skins", json::array());
    std::vector<SkinRecord> skins;
    skins.reserve(skinArray.size());
    for (const json& skinJson : skinArray) {
        SkinRecord skin;
        if (!skinJson.is_object()) {
            skin.valid = false;
            skins.push_back(std::move(skin));
            continue;
        }

        const auto& jointsJson = skinJson.value("joints", json::array());
        if (!jointsJson.is_array() || jointsJson.empty()) {
            skin.valid = false;
        }
        for (const json& joint : jointsJson) {
            if (!joint.is_number_integer()) {
                skin.valid = false;
                continue;
            }
            const int jointIndex = joint.get<int>();
            if (jointIndex < 0 ||
                static_cast<size_t>(jointIndex) >= nodeCount) {
                skin.valid = false;
            }
            skin.joints.push_back(jointIndex);
        }
        skin.inverseBindMatrices.assign(
            skin.joints.size(),
            glm::dmat4(1.0));
        if (skinJson.contains("inverseBindMatrices")) {
            if (!skinJson["inverseBindMatrices"].is_number_integer()) {
                skin.valid = false;
                skins.push_back(std::move(skin));
                continue;
            }
            const auto inverseBindSpan = accessorSpan(
                doc,
                buffers,
                skinJson["inverseBindMatrices"].get<int>());
            if (!inverseBindSpan || inverseBindSpan->components != 16 ||
                inverseBindSpan->componentType != 5126 ||
                inverseBindSpan->count < skin.joints.size()) {
                skin.valid = false;
            } else {
                for (size_t i = 0; i < skin.joints.size(); ++i) {
                    const glm::dmat4 inverseBind =
                        readMat4(*inverseBindSpan, i);
                    if (!finiteMat4Value(inverseBind)) {
                        skin.valid = false;
                        break;
                    }
                    skin.inverseBindMatrices[i] = inverseBind;
                }
            }
        }
        if (skinJson.contains("skeleton")) {
            if (!skinJson["skeleton"].is_number_integer()) {
                skin.valid = false;
            } else {
                const int skeleton = skinJson["skeleton"].get<int>();
                if (skeleton < 0 ||
                    static_cast<size_t>(skeleton) >= nodeCount) {
                    skin.valid = false;
                }
            }
        }
        skins.push_back(std::move(skin));
    }
    return skins;
}

void generateNormalsIfMissing(GltfPrimitive& primitive) {
    for (SurfaceVertex& vertex : primitive.vertices) {
        vertex.normalEcef = Vec3::zero();
    }

    const std::vector<uint32_t>& indices = primitive.indices;
    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        const uint32_t ia = indices[i + 0];
        const uint32_t ib = indices[i + 1];
        const uint32_t ic = indices[i + 2];
        if (ia >= primitive.vertices.size() ||
            ib >= primitive.vertices.size() ||
            ic >= primitive.vertices.size()) {
            continue;
        }
        const Vec3 a = primitive.vertices[ia].positionEcef;
        const Vec3 b = primitive.vertices[ib].positionEcef;
        const Vec3 c = primitive.vertices[ic].positionEcef;
        const Vec3 n = (b - a).cross(c - a);
        primitive.vertices[ia].normalEcef += n;
        primitive.vertices[ib].normalEcef += n;
        primitive.vertices[ic].normalEcef += n;
    }

    for (SurfaceVertex& vertex : primitive.vertices) {
        if (vertex.normalEcef.lengthSquared() > 0.0) {
            vertex.normalEcef = vertex.normalEcef.normalized();
        } else {
            vertex.normalEcef = Vec3::unitZ();
        }
    }
}

void fillDefaultNormals(GltfPrimitive& primitive) {
    for (SurfaceVertex& vertex : primitive.vertices) {
        vertex.normalEcef = Vec3::unitZ();
    }
}

void resolveMissingNormals(GltfPrimitive& primitive) {
    if (primitive.primitiveMode == GltfPrimitiveMode::Triangles) {
        generateNormalsIfMissing(primitive);
    } else {
        fillDefaultNormals(primitive);
    }
}

std::optional<std::vector<GltfVertexSkinning>> readVertexSkinning(
    const json& doc,
    const std::vector<std::vector<uint8_t>>& buffers,
    const json& attrs,
    size_t expectedCount) {
    if (!attrs.contains("JOINTS_0") || !attrs.contains("WEIGHTS_0")) {
        return std::nullopt;
    }
    if (!attrs["JOINTS_0"].is_number_integer() ||
        !attrs["WEIGHTS_0"].is_number_integer()) {
        return std::nullopt;
    }

    const auto joints = accessorSpan(doc, buffers, attrs["JOINTS_0"].get<int>());
    const auto weights =
        accessorSpan(doc, buffers, attrs["WEIGHTS_0"].get<int>());
    if (!joints || !weights || joints->components != 4 ||
        weights->components != 4 || joints->count != expectedCount ||
        weights->count != expectedCount) {
        return std::nullopt;
    }
    if (joints->componentType != 5121 && joints->componentType != 5123) {
        return std::nullopt;
    }
    const bool validFloatWeights =
        weights->componentType == 5126 && !weights->normalized;
    const bool validNormalizedIntegerWeights =
        (weights->componentType == 5121 || weights->componentType == 5123) &&
        weights->normalized;
    if (!validFloatWeights && !validNormalizedIntegerWeights) {
        return std::nullopt;
    }

    std::vector<GltfVertexSkinning> result(expectedCount);
    for (size_t i = 0; i < expectedCount; ++i) {
        const glm::dvec4 jointValues = readVec4(*joints, i);
        const glm::dvec4 weightValues = readVec4(*weights, i);
        if (!finiteVec4Value(jointValues) ||
            !finiteVec4Value(weightValues)) {
            return std::nullopt;
        }
        double weightSum = 0.0;
        for (int k = 0; k < 4; ++k) {
            result[i].joints[static_cast<size_t>(k)] =
                static_cast<uint32_t>(
                    std::max(0.0, std::floor(jointValues[k] + 0.5)));
            result[i].weights[static_cast<size_t>(k)] =
                std::max(0.0, weightValues[k]);
            weightSum += result[i].weights[static_cast<size_t>(k)];
        }
        if (weightSum <= 0.0) {
            return std::nullopt;
        }
        for (double& weight : result[i].weights) {
            weight /= weightSum;
        }
    }
    return result;
}

bool validTexCoordAccessor(const AccessorSpan& span) {
    if (span.type != "VEC2" || span.components != 2) {
        return false;
    }
    if (span.componentType == 5126) {
        return !span.normalized;
    }
    return (span.componentType == 5121 || span.componentType == 5123) &&
           span.normalized;
}

bool quantizedAttributeComponentType(int componentType) {
    return componentType == 5120 ||
           componentType == 5121 ||
           componentType == 5122 ||
           componentType == 5123;
}

bool validQuantizedVectorAccessor(
    const AccessorSpan& span,
    const char* type,
    int components,
    bool meshQuantizationEnabled) {
    if (span.type != type || span.components != components) {
        return false;
    }
    if (span.componentType == 5126) {
        return !span.normalized;
    }
    return meshQuantizationEnabled &&
           quantizedAttributeComponentType(span.componentType);
}

bool validTexCoordAccessor(
    const AccessorSpan& span,
    bool meshQuantizationEnabled) {
    if (validTexCoordAccessor(span)) {
        return true;
    }
    return meshQuantizationEnabled &&
           span.type == "VEC2" &&
           span.components == 2 &&
           quantizedAttributeComponentType(span.componentType);
}

bool validColorAccessor(const AccessorSpan& span) {
    if ((span.type != "VEC3" && span.type != "VEC4") ||
        (span.components != 3 && span.components != 4)) {
        return false;
    }
    if (span.componentType == 5126) {
        return !span.normalized;
    }
    return (span.componentType == 5121 || span.componentType == 5123) &&
           span.normalized;
}

bool validFeatureIdAccessor(const AccessorSpan& span) {
    return span.type == "SCALAR" &&
           span.components == 1 &&
           !span.normalized &&
           (span.componentType == 5121 ||
            span.componentType == 5123 ||
            span.componentType == 5125);
}

bool validTangentAccessor(const AccessorSpan& span) {
    return span.type == "VEC4" &&
           span.components == 4 &&
           span.componentType == 5126 &&
           !span.normalized;
}

bool validTangentAccessor(
    const AccessorSpan& span,
    bool meshQuantizationEnabled) {
    if (validTangentAccessor(span)) {
        return true;
    }
    return validQuantizedVectorAccessor(
        span,
        "VEC4",
        4,
        meshQuantizationEnabled);
}

bool primitiveAttributeSemanticSupported(const std::string& semantic,
                                         bool allowLegacyBatchIdAttribute) {
    if (semantic == "POSITION" ||
        semantic == "NORMAL" ||
        semantic == "TANGENT" ||
        semantic == "COLOR_0" ||
        semantic == "JOINTS_0" ||
        semantic == "WEIGHTS_0") {
        return true;
    }

    if (attributeSemanticStartsWith(semantic, "TEXCOORD_")) {
        return texCoordSetIndexFromSemantic(semantic).has_value();
    }

    if (attributeSemanticStartsWith(semantic, "_FEATURE_ID")) {
        return false;
    }

    if (attributeSemanticStartsWith(semantic, "_BATCHID")) {
        return semantic == "_BATCHID" && allowLegacyBatchIdAttribute;
    }

    if (!semantic.empty() && semantic[0] == '_') {
        return true;
    }

    return false;
}

bool morphTargetSemanticSupported(const std::string& semantic) {
    return semantic == "POSITION" ||
           semantic == "NORMAL" ||
           semantic == "TANGENT";
}

std::optional<std::array<float, 4>> readVertexColor(
    const AccessorSpan& span,
    size_t index) {
    std::array<float, 4> color = {1.0f, 1.0f, 1.0f, 1.0f};
    for (int component = 0; component < span.components; ++component) {
        const double value = readComponent(
            accessorComponentPtr(span, index, component),
            span.componentType,
            span.normalized);
        const float converted = static_cast<float>(value);
        if (!std::isfinite(value) || !std::isfinite(converted)) {
            return std::nullopt;
        }
        color[static_cast<size_t>(component)] = converted;
    }
    return color;
}

bool primitiveAttributesAreSupported(
    const json& doc,
    const std::vector<std::vector<uint8_t>>& buffers,
    const json& attrs,
    bool allowLegacyBatchIdAttribute) {
    for (auto it = attrs.begin(); it != attrs.end(); ++it) {
        if (!it.value().is_number_integer()) {
            return false;
        }

        const std::string& semantic = it.key();
        if (!primitiveAttributeSemanticSupported(
                semantic,
                allowLegacyBatchIdAttribute)) {
            return false;
        }
        if (!semantic.empty() && semantic[0] == '_') {
            if (!accessorSpan(doc, buffers, it.value().get<int>())) {
                return false;
            }
        }
    }
    return true;
}

std::optional<std::array<float, 4>> makeTangent(
    const glm::dvec3& direction,
    double handedness) {
    const double lenSq = glm::dot(direction, direction);
    if (!std::isfinite(lenSq) || lenSq <= 0.0 ||
        !std::isfinite(handedness) ||
        std::abs(std::abs(handedness) - 1.0) > 1e-5) {
        return std::nullopt;
    }
    const glm::dvec3 normalized = glm::normalize(direction);
    return std::array<float, 4>{
        static_cast<float>(normalized.x),
        static_cast<float>(normalized.y),
        static_cast<float>(normalized.z),
        handedness < 0.0 ? -1.0f : 1.0f};
}

std::optional<std::array<float, 4>> readTangent(
    const AccessorSpan& span,
    size_t index) {
    const glm::dvec4 value = readVec4(span, index);
    return makeTangent(glm::dvec3(value), value.w);
}

std::optional<std::array<float, 4>> transformTangent(
    const glm::dmat3& matrix,
    const std::array<float, 4>& tangent) {
    return makeTangent(
        matrix * glm::dvec3(tangent[0], tangent[1], tangent[2]),
        tangent[3]);
}

std::optional<std::vector<glm::dmat4>> computeJointMatrices(
    const std::vector<NodeRecord>& nodes,
    const SkinRecord& skin) {
    if (!skin.valid || skin.joints.empty() ||
        skin.inverseBindMatrices.size() != skin.joints.size()) {
        return std::nullopt;
    }

    std::vector<glm::dmat4> jointMatrices(skin.joints.size(), glm::dmat4(1.0));
    for (size_t i = 0; i < skin.joints.size(); ++i) {
        const int jointNodeIndex = skin.joints[i];
        if (jointNodeIndex < 0 ||
            static_cast<size_t>(jointNodeIndex) >= nodes.size()) {
            return std::nullopt;
        }
        jointMatrices[i] =
            nodes[static_cast<size_t>(jointNodeIndex)].globalTransform *
            skin.inverseBindMatrices[i];
    }
    return jointMatrices;
}

std::optional<glm::dvec3> skinDirection(
    const glm::dvec3& localDirection,
    const GltfVertexSkinning& skinning,
    const std::vector<glm::dmat4>& jointMatrices) {
    glm::dvec3 direction(0.0);
    for (size_t k = 0; k < 4; ++k) {
        const uint32_t jointIndex = skinning.joints[k];
        const double weight = skinning.weights[k];
        if (weight <= 0.0) continue;
        if (jointIndex >= jointMatrices.size()) {
            return std::nullopt;
        }
        direction += weight *
            (glm::dmat3(jointMatrices[jointIndex]) * localDirection);
    }
    const double lenSq = glm::dot(direction, direction);
    if (!std::isfinite(lenSq) || lenSq <= 0.0) {
        return std::nullopt;
    }
    return glm::normalize(direction);
}

std::optional<std::pair<Vec3, Vec3>> skinVertex(
    const glm::dvec3& localPosition,
    const glm::dvec3& localNormal,
    const GltfVertexSkinning& skinning,
    const std::vector<glm::dmat4>& jointMatrices) {
    glm::dvec4 position(0.0);
    glm::dvec3 normal(0.0);
    for (size_t k = 0; k < 4; ++k) {
        const uint32_t jointIndex = skinning.joints[k];
        const double weight = skinning.weights[k];
        if (weight <= 0.0) continue;
        if (jointIndex >= jointMatrices.size()) {
            return std::nullopt;
        }
        const glm::dmat4& joint = jointMatrices[jointIndex];
        position += weight * (joint * glm::dvec4(localPosition, 1.0));
        normal += weight * (glm::dmat3(joint) * localNormal);
    }
    glm::dvec3 finalPosition(position);
    if (std::abs(position.w) > 1e-14) {
        finalPosition /= position.w;
    }
    if (glm::dot(normal, normal) > 0.0) {
        normal = glm::normalize(normal);
    } else {
        normal = glm::dvec3(0.0, 0.0, 1.0);
    }
    return std::make_pair(Vec3(finalPosition), Vec3(normal));
}

bool finiteValues(const std::vector<double>& values) {
    return std::all_of(
        values.begin(),
        values.end(),
        [](double value) { return std::isfinite(value); });
}

std::optional<std::vector<GltfMorphTarget>> readMorphTargets(
    const json& doc,
    const std::vector<std::vector<uint8_t>>& buffers,
    const json& primitiveJson,
    size_t vertexCount,
    bool hasBaseTangents) {
    if (!primitiveJson.contains("targets")) {
        return std::vector<GltfMorphTarget>{};
    }
    const json& targetsJson = primitiveJson["targets"];
    if (!targetsJson.is_array()) {
        return std::nullopt;
    }

    std::vector<GltfMorphTarget> targets;
    targets.reserve(targetsJson.size());
    for (const json& targetJson : targetsJson) {
        if (!targetJson.is_object()) {
            return std::nullopt;
        }
        GltfMorphTarget target;
        for (auto it = targetJson.begin(); it != targetJson.end(); ++it) {
            if (!it.value().is_number_integer()) {
                return std::nullopt;
            }
            const auto span = accessorSpan(doc, buffers, it.value().get<int>());
            if (!span || span->components != 3 ||
                span->componentType != 5126 ||
                span->count != vertexCount) {
                return std::nullopt;
            }
            if (it.key() == "POSITION") {
                target.positionDeltas.resize(vertexCount);
                for (size_t i = 0; i < vertexCount; ++i) {
                    const glm::dvec3 delta = readVec3(*span, i);
                    if (!finiteVec3Value(delta)) {
                        return std::nullopt;
                    }
                    target.positionDeltas[i] = Vec3(delta);
                }
            } else if (it.key() == "NORMAL") {
                target.normalDeltas.resize(vertexCount);
                for (size_t i = 0; i < vertexCount; ++i) {
                    const glm::dvec3 delta = readVec3(*span, i);
                    if (!finiteVec3Value(delta)) {
                        return std::nullopt;
                    }
                    target.normalDeltas[i] = Vec3(delta);
                }
            } else if (it.key() == "TANGENT") {
                if (!hasBaseTangents) {
                    return std::nullopt;
                }
                target.tangentDeltas.resize(vertexCount);
                for (size_t i = 0; i < vertexCount; ++i) {
                    const glm::dvec3 delta = readVec3(*span, i);
                    if (!finiteVec3Value(delta)) {
                        return std::nullopt;
                    }
                    target.tangentDeltas[i] = Vec3(delta);
                }
            } else {
                return std::nullopt;
            }
        }
        targets.push_back(std::move(target));
    }
    return targets;
}

std::vector<double> readAccessorFlat(const AccessorSpan& span) {
    std::vector<double> values;
    values.reserve(span.count * static_cast<size_t>(span.components));
    for (size_t i = 0; i < span.count; ++i) {
        for (int c = 0; c < span.components; ++c) {
            values.push_back(readComponent(
                accessorComponentPtr(span, i, c),
                span.componentType,
                span.normalized));
        }
    }
    return values;
}

std::optional<GltfAnimationPath> parseAnimationPath(const std::string& path) {
    if (path == "translation") return GltfAnimationPath::Translation;
    if (path == "rotation") return GltfAnimationPath::Rotation;
    if (path == "scale") return GltfAnimationPath::Scale;
    if (path == "weights") return GltfAnimationPath::Weights;
    return std::nullopt;
}

std::optional<GltfAnimationInterpolation> parseAnimationInterpolation(
    const std::string& interpolation) {
    if (interpolation == "LINEAR") {
        return GltfAnimationInterpolation::Linear;
    }
    if (interpolation == "STEP") {
        return GltfAnimationInterpolation::Step;
    }
    if (interpolation == "CUBICSPLINE") {
        return GltfAnimationInterpolation::CubicSpline;
    }
    return std::nullopt;
}

size_t animationOutputElementsPerKey(
    GltfAnimationInterpolation interpolation) {
    return interpolation == GltfAnimationInterpolation::CubicSpline ? 3u : 1u;
}

bool hasStrictlyIncreasingAnimationTimes(
    const std::vector<double>& inputTimes) {
    if (inputTimes.empty() ||
        !std::isfinite(inputTimes.front()) ||
        inputTimes.front() < 0.0) {
        return false;
    }
    for (size_t i = 1; i < inputTimes.size(); ++i) {
        if (!std::isfinite(inputTimes[i]) ||
            !(inputTimes[i] > inputTimes[i - 1])) {
            return false;
        }
    }
    return true;
}

struct RawAnimationSampler {
    GltfAnimationSamplerRuntime runtime;
    int outputAccessorCount = 0;
    int outputAccessorComponents = 0;
};

bool animationRotationKeyframesHaveNonZeroValues(
    const RawAnimationSampler& raw,
    size_t keyframes) {
    const size_t outputElementsPerKey =
        animationOutputElementsPerKey(raw.runtime.interpolation);
    for (size_t keyframe = 0; keyframe < keyframes; ++keyframe) {
        const size_t valueElement = keyframe * outputElementsPerKey +
            (raw.runtime.interpolation == GltfAnimationInterpolation::CubicSpline
                 ? 1u
                 : 0u);
        const size_t offset = valueElement * 4u;
        if (offset + 3u >= raw.runtime.outputValues.size()) {
            return false;
        }
        const double x = raw.runtime.outputValues[offset + 0u];
        const double y = raw.runtime.outputValues[offset + 1u];
        const double z = raw.runtime.outputValues[offset + 2u];
        const double w = raw.runtime.outputValues[offset + 3u];
        const double lengthSquared = x * x + y * y + z * z + w * w;
        if (!std::isfinite(lengthSquared) || lengthSquared <= 0.0) {
            return false;
        }
    }
    return true;
}

std::optional<std::vector<GltfAnimationRuntime>> parseAnimations(
    const json& doc,
    const std::vector<std::vector<uint8_t>>& buffers,
    const std::vector<NodeRecord>& nodes) {
    const auto animationsIt = doc.find("animations");
    if (animationsIt == doc.end()) {
        return std::vector<GltfAnimationRuntime>{};
    }
    if (!animationsIt->is_array()) {
        return std::nullopt;
    }
    const json& animationsJson = *animationsIt;
    std::vector<GltfAnimationRuntime> animations;
    animations.reserve(animationsJson.size());

    for (const json& animationJson : animationsJson) {
        if (!animationJson.is_object()) {
            return std::nullopt;
        }

        const auto samplersIt = animationJson.find("samplers");
        if (samplersIt == animationJson.end() || !samplersIt->is_array()) {
            return std::nullopt;
        }
        const json& samplersJson = *samplersIt;
        if (samplersJson.empty()) {
            return std::nullopt;
        }
        std::vector<RawAnimationSampler> rawSamplers;
        rawSamplers.reserve(samplersJson.size());
        for (const json& samplerJson : samplersJson) {
            if (!samplerJson.is_object() ||
                !samplerJson.contains("input") ||
                !samplerJson["input"].is_number_integer() ||
                !samplerJson.contains("output") ||
                !samplerJson["output"].is_number_integer()) {
                return std::nullopt;
            }
            std::string interpolationText = "LINEAR";
            if (samplerJson.contains("interpolation")) {
                if (!samplerJson["interpolation"].is_string()) {
                    return std::nullopt;
                }
                interpolationText = samplerJson["interpolation"].get<std::string>();
            }
            const auto interpolation =
                parseAnimationInterpolation(interpolationText);
            if (!interpolation) {
                return std::nullopt;
            }

            const auto input = accessorSpan(
                doc,
                buffers,
                samplerJson["input"].get<int>());
            const auto output = accessorSpan(
                doc,
                buffers,
                samplerJson["output"].get<int>());
            if (!input || !output || input->components != 1 ||
                input->componentType != 5126 || input->count == 0 ||
                output->componentType != 5126) {
                return std::nullopt;
            }

            RawAnimationSampler raw;
            raw.runtime.inputTimes = readAccessorFlat(*input);
            if (!hasStrictlyIncreasingAnimationTimes(raw.runtime.inputTimes) ||
                (*interpolation == GltfAnimationInterpolation::CubicSpline &&
                 raw.runtime.inputTimes.size() < 2)) {
                return std::nullopt;
            }
            raw.runtime.outputValues = readAccessorFlat(*output);
            if (!finiteValues(raw.runtime.outputValues)) {
                return std::nullopt;
            }
            raw.runtime.interpolation = *interpolation;
            raw.outputAccessorCount = static_cast<int>(output->count);
            raw.outputAccessorComponents = output->components;
            rawSamplers.push_back(std::move(raw));
        }

        GltfAnimationRuntime animation;
        animation.samplers.reserve(rawSamplers.size());
        for (const RawAnimationSampler& raw : rawSamplers) {
            animation.samplers.push_back(raw.runtime);
            if (!raw.runtime.inputTimes.empty()) {
                animation.durationSeconds = std::max(
                    animation.durationSeconds,
                    raw.runtime.inputTimes.back());
            }
        }

        const auto channelsIt = animationJson.find("channels");
        if (channelsIt == animationJson.end() || !channelsIt->is_array()) {
            return std::nullopt;
        }
        const json& channelsJson = *channelsIt;
        if (channelsJson.empty()) {
            return std::nullopt;
        }
        animation.channels.reserve(channelsJson.size());
        std::vector<std::pair<int, GltfAnimationPath>> animatedTargets;
        animatedTargets.reserve(channelsJson.size());
        for (const json& channelJson : channelsJson) {
            if (!channelJson.is_object() ||
                !channelJson.contains("sampler") ||
                !channelJson["sampler"].is_number_integer() ||
                !channelJson.contains("target") ||
                !channelJson["target"].is_object()) {
                return std::nullopt;
            }

            const int samplerIndex = channelJson["sampler"].get<int>();
            if (samplerIndex < 0 ||
                static_cast<size_t>(samplerIndex) >= rawSamplers.size()) {
                return std::nullopt;
            }

            const json& target = channelJson["target"];
            if (!target.contains("node") ||
                !target["node"].is_number_integer() ||
                !target.contains("path") ||
                !target["path"].is_string()) {
                return std::nullopt;
            }
            const int nodeIndex = target["node"].get<int>();
            if (nodeIndex < 0 ||
                static_cast<size_t>(nodeIndex) >= nodes.size() ||
                nodes[static_cast<size_t>(nodeIndex)].hasMatrix) {
                return std::nullopt;
            }
            const auto path =
                parseAnimationPath(target["path"].get<std::string>());
            if (!path) return std::nullopt;
            const auto duplicateTarget = std::find_if(
                animatedTargets.begin(),
                animatedTargets.end(),
                [nodeIndex, path](const auto& existing) {
                    return existing.first == nodeIndex &&
                           existing.second == *path;
                });
            if (duplicateTarget != animatedTargets.end()) {
                return std::nullopt;
            }
            animatedTargets.emplace_back(nodeIndex, *path);

            RawAnimationSampler& raw = rawSamplers[static_cast<size_t>(
                samplerIndex)];
            const size_t keyframes = raw.runtime.inputTimes.size();
            const size_t outputElementsPerKey =
                animationOutputElementsPerKey(raw.runtime.interpolation);
            int expectedComponents = 0;
            switch (*path) {
                case GltfAnimationPath::Translation:
                case GltfAnimationPath::Scale:
                    expectedComponents = 3;
                    break;
                case GltfAnimationPath::Rotation:
                    expectedComponents = 4;
                    break;
                case GltfAnimationPath::Weights: {
                    const size_t targetCount = meshMorphTargetCount(
                        doc,
                        nodes[static_cast<size_t>(nodeIndex)].mesh);
                    if (targetCount == 0 ||
                        raw.runtime.outputValues.size() %
                            (keyframes * outputElementsPerKey) !=
                            0) {
                        return std::nullopt;
                    }
                    expectedComponents = static_cast<int>(
                        raw.runtime.outputValues.size() /
                        (keyframes * outputElementsPerKey));
                    if (expectedComponents !=
                        static_cast<int>(targetCount)) {
                        return std::nullopt;
                    }
                    break;
                }
            }
            if (expectedComponents <= 0 ||
                raw.runtime.outputValues.size() !=
                    keyframes * outputElementsPerKey *
                        static_cast<size_t>(expectedComponents)) {
                return std::nullopt;
            }
            if (*path != GltfAnimationPath::Weights &&
                (raw.outputAccessorComponents != expectedComponents ||
                 raw.outputAccessorCount !=
                    static_cast<int>(keyframes * outputElementsPerKey))) {
                return std::nullopt;
            }
            if (*path == GltfAnimationPath::Rotation &&
                !animationRotationKeyframesHaveNonZeroValues(
                    raw,
                    keyframes)) {
                return std::nullopt;
            }
            if (raw.runtime.outputComponents != 0 &&
                raw.runtime.outputComponents != expectedComponents) {
                return std::nullopt;
            }
            raw.runtime.outputComponents = expectedComponents;
            animation.samplers[static_cast<size_t>(samplerIndex)]
                .outputComponents = expectedComponents;

            GltfAnimationChannelRuntime channel;
            channel.samplerIndex = samplerIndex;
            channel.targetNode = nodeIndex;
            channel.path = *path;
            animation.channels.push_back(channel);
        }

        if (!animation.channels.empty()) {
            animations.push_back(std::move(animation));
        }
    }

    return animations;
}

std::array<double, 3> toArray3(const glm::dvec3& value) {
    return {value.x, value.y, value.z};
}

std::array<double, 4> toArrayQuat(const glm::dquat& value) {
    return {value.x, value.y, value.z, value.w};
}

glm::dvec3 fromArray3(const std::array<double, 3>& value) {
    return glm::dvec3(value[0], value[1], value[2]);
}

glm::dquat fromArrayQuat(const std::array<double, 4>& value) {
    return glm::dquat(value[3], value[0], value[1], value[2]);
}

glm::dmat4 composeTrs(const std::array<double, 3>& translation,
                      const std::array<double, 4>& rotation,
                      const std::array<double, 3>& scale) {
    return glm::translate(glm::dmat4(1.0), fromArray3(translation)) *
           glm::mat4_cast(fromArrayQuat(rotation)) *
           glm::scale(glm::dmat4(1.0), fromArray3(scale));
}

bool finiteVec3(const glm::dvec3& value) {
    return std::isfinite(value.x) &&
           std::isfinite(value.y) &&
           std::isfinite(value.z);
}

bool finiteVec4(const glm::dvec4& value) {
    return std::isfinite(value.x) &&
           std::isfinite(value.y) &&
           std::isfinite(value.z) &&
           std::isfinite(value.w);
}

bool validInstanceVec3Accessor(const AccessorSpan& span) {
    return span.type == "VEC3" &&
           span.components == 3 &&
           span.componentType == 5126 &&
           !span.normalized;
}

bool validInstanceRotationAccessor(const AccessorSpan& span) {
    if (span.type != "VEC4" || span.components != 4) {
        return false;
    }
    if (span.componentType == 5126) {
        return !span.normalized;
    }
    return (span.componentType == 5120 ||
            span.componentType == 5121 ||
            span.componentType == 5122 ||
            span.componentType == 5123) &&
           span.normalized;
}

std::optional<std::vector<GltfInstance>> parseGpuInstancingInstances(
    const json& doc,
    const std::vector<std::vector<uint8_t>>& buffers,
    const json& node,
    const glm::dmat4& nodeGlobalTransform,
    bool& strictFailure) {
    const auto extensionsIt = node.find("extensions");
    if (extensionsIt == node.end()) {
        return std::vector<GltfInstance>{};
    }
    if (!extensionsIt->is_object()) {
        strictFailure = true;
        return std::nullopt;
    }
    const auto instancingIt =
        extensionsIt->find("EXT_mesh_gpu_instancing");
    if (instancingIt == extensionsIt->end()) {
        return std::vector<GltfInstance>{};
    }
    if (!node.contains("mesh") ||
        node.contains("skin") ||
        !instancingIt->is_object()) {
        strictFailure = true;
        return std::nullopt;
    }
    const auto attributesIt = instancingIt->find("attributes");
    if (attributesIt == instancingIt->end() ||
        !attributesIt->is_object() ||
        attributesIt->empty()) {
        strictFailure = true;
        return std::nullopt;
    }

    const json& attributes = *attributesIt;
    auto readAttribute = [&](const char* name)
        -> std::optional<AccessorSpan> {
        const auto it = attributes.find(name);
        if (it == attributes.end()) {
            return std::nullopt;
        }
        if (!it->is_number_integer()) {
            strictFailure = true;
            return std::nullopt;
        }
        auto span = accessorSpan(doc, buffers, it->get<int>());
        if (!span) {
            strictFailure = true;
            return std::nullopt;
        }
        return span;
    };

    std::optional<AccessorSpan> translations =
        readAttribute("TRANSLATION");
    if (strictFailure) return std::nullopt;
    std::optional<AccessorSpan> rotations =
        readAttribute("ROTATION");
    if (strictFailure) return std::nullopt;
    std::optional<AccessorSpan> scales =
        readAttribute("SCALE");
    if (strictFailure) return std::nullopt;

    if ((translations && !validInstanceVec3Accessor(*translations)) ||
        (rotations && !validInstanceRotationAccessor(*rotations)) ||
        (scales && !validInstanceVec3Accessor(*scales))) {
        strictFailure = true;
        return std::nullopt;
    }

    size_t count = 0;
    auto syncCount = [&](const std::optional<AccessorSpan>& span) {
        if (!span) return true;
        if (count == 0u) {
            count = span->count;
            return true;
        }
        return count == span->count;
    };
    if (!syncCount(translations) ||
        !syncCount(rotations) ||
        !syncCount(scales) ||
        count == 0u) {
        strictFailure = true;
        return std::nullopt;
    }

    std::vector<GltfInstance> instances(count);
    for (size_t i = 0; i < count; ++i) {
        glm::dmat4 transform(1.0);
        if (translations) {
            const glm::dvec3 translation = readVec3(*translations, i);
            if (!finiteVec3(translation)) {
                strictFailure = true;
                return std::nullopt;
            }
            transform = glm::translate(transform, translation);
        }
        if (rotations) {
            const glm::dvec4 rotation = readVec4(*rotations, i);
            const double lengthSquared = glm::dot(rotation, rotation);
            if (!finiteVec4(rotation) ||
                !std::isfinite(lengthSquared) ||
                lengthSquared <= 0.0) {
                strictFailure = true;
                return std::nullopt;
            }
            const glm::dquat normalizedRotation =
                glm::normalize(glm::dquat(
                    rotation.w,
                    rotation.x,
                    rotation.y,
                    rotation.z));
            if (!std::isfinite(normalizedRotation.w) ||
                !std::isfinite(normalizedRotation.x) ||
                !std::isfinite(normalizedRotation.y) ||
                !std::isfinite(normalizedRotation.z)) {
                strictFailure = true;
                return std::nullopt;
            }
            transform = transform * glm::mat4_cast(normalizedRotation);
        }
        if (scales) {
            const glm::dvec3 scale = readVec3(*scales, i);
            if (!finiteVec3(scale)) {
                strictFailure = true;
                return std::nullopt;
            }
            transform = glm::scale(transform, scale);
        }
        instances[i].transform = Mat4(transform);
    }

    const glm::dmat3 nodeLinear(nodeGlobalTransform);
    const double determinant = glm::determinant(nodeLinear);
    if (!std::isfinite(determinant) || std::abs(determinant) <= 1e-14) {
        strictFailure = true;
        return std::nullopt;
    }
    const glm::dmat4 inverseNodeTransform =
        glm::inverse(nodeGlobalTransform);
    for (GltfInstance& instance : instances) {
        instance.transform = Mat4(
            nodeGlobalTransform *
            instance.transform.raw() *
            inverseNodeTransform);
    }
    return instances;
}

std::vector<GltfNodeRuntime> toRuntimeNodes(
    const std::vector<NodeRecord>& records) {
    std::vector<GltfNodeRuntime> nodes(records.size());
    for (size_t i = 0; i < records.size(); ++i) {
        const NodeRecord& src = records[i];
        GltfNodeRuntime& dst = nodes[i];
        dst.baseLocalTransform = Mat4(src.localTransform);
        dst.localTransform = Mat4(src.localTransform);
        dst.globalTransform = Mat4(src.globalTransform);
        dst.baseTranslation = toArray3(src.translation);
        dst.baseRotation = toArrayQuat(src.rotation);
        dst.baseScale = toArray3(src.scale);
        dst.translation = dst.baseTranslation;
        dst.rotation = dst.baseRotation;
        dst.scale = dst.baseScale;
        dst.baseWeights = src.weights;
        dst.weights = src.weights;
        dst.children = src.children;
        dst.parent = src.parent;
        dst.mesh = src.mesh;
        dst.skin = src.skin;
        dst.hasMatrix = src.hasMatrix;
    }
    return nodes;
}

std::vector<GltfSkinRuntime> toRuntimeSkins(
    const std::vector<SkinRecord>& records) {
    std::vector<GltfSkinRuntime> skins(records.size());
    for (size_t i = 0; i < records.size(); ++i) {
        skins[i].joints = records[i].joints;
        skins[i].inverseBindMatrices.reserve(
            records[i].inverseBindMatrices.size());
        for (const glm::dmat4& inverseBind : records[i].inverseBindMatrices) {
            skins[i].inverseBindMatrices.push_back(Mat4(inverseBind));
        }
    }
    return skins;
}

std::vector<int> sceneRootNodes(const json& doc) {
    const auto& scenes = doc.value("scenes", json::array());
    const auto& nodes = doc.value("nodes", json::array());
    std::vector<int> roots;
    if (!scenes.empty()) {
        const int requestedScene = doc.value("scene", 0);
        const size_t sceneIndex =
            requestedScene >= 0 &&
                    static_cast<size_t>(requestedScene) < scenes.size()
                ? static_cast<size_t>(requestedScene)
                : 0u;
        for (const json& node : scenes[sceneIndex].value(
                 "nodes",
                 json::array())) {
            if (node.is_number_integer()) {
                roots.push_back(node.get<int>());
            }
        }
    } else if (!nodes.empty()) {
        roots.push_back(0);
    }
    return roots;
}

std::optional<GltfPrimitive> parsePrimitive(
    const json& doc,
    const std::vector<std::vector<uint8_t>>& buffers,
    const std::vector<GltfTexture>& textures,
    const std::vector<NodeRecord>& nodes,
    const std::vector<SkinRecord>& skins,
    int nodeIndex,
    const json& primitiveJson,
    bool meshQuantizationEnabled,
    bool allowLegacyBatchIdAttribute,
    bool& strictFailure) {
    const auto primitiveMode = integerProperty(primitiveJson, "mode", 4);
    if (!primitiveMode || !validRenderablePrimitiveMode(*primitiveMode)) {
        strictFailure = true;
        return std::nullopt;
    }
    if (!primitiveJson.contains("attributes") ||
        !primitiveJson["attributes"].is_object()) {
        strictFailure = true;
        return std::nullopt;
    }

    const json& attrs = primitiveJson["attributes"];
    if (!primitiveAttributesAreSupported(
            doc,
            buffers,
            attrs,
            allowLegacyBatchIdAttribute)) {
        strictFailure = true;
        return std::nullopt;
    }
    if (!attrs.contains("POSITION") || !attrs["POSITION"].is_number_integer()) {
        strictFailure = true;
        return std::nullopt;
    }

    const auto positions = accessorSpan(
        doc,
        buffers,
        attrs["POSITION"].get<int>());
    if (!positions ||
        !validQuantizedVectorAccessor(
            *positions,
            "VEC3",
            3,
            meshQuantizationEnabled)) {
        strictFailure = true;
        return std::nullopt;
    }

    std::optional<AccessorSpan> normals;
    if (attrs.contains("NORMAL")) {
        if (!attrs["NORMAL"].is_number_integer()) {
            strictFailure = true;
            return std::nullopt;
        }
        normals = accessorSpan(doc, buffers, attrs["NORMAL"].get<int>());
        if (!normals ||
            !validQuantizedVectorAccessor(
                *normals,
                "VEC3",
                3,
                meshQuantizationEnabled) ||
            normals->count != positions->count) {
            strictFailure = true;
            return std::nullopt;
        }
    }

    std::optional<AccessorSpan> tangents;
    if (attrs.contains("TANGENT")) {
        if (!attrs["TANGENT"].is_number_integer()) {
            strictFailure = true;
            return std::nullopt;
        }
        tangents = accessorSpan(doc, buffers, attrs["TANGENT"].get<int>());
        if (!tangents ||
            !validTangentAccessor(*tangents, meshQuantizationEnabled) ||
            tangents->count != positions->count) {
            strictFailure = true;
            return std::nullopt;
        }
    }

    std::array<std::optional<AccessorSpan>, kGltfMaxTexCoordSets>
        texCoordSpans;
    for (auto it = attrs.begin(); it != attrs.end(); ++it) {
        if (it.key().rfind("TEXCOORD_", 0) != 0) {
            continue;
        }
        const std::optional<size_t> setIndex =
            texCoordSetIndexFromSemantic(it.key());
        if (!setIndex || !it->is_number_integer()) {
            strictFailure = true;
            return std::nullopt;
        }
        auto span = accessorSpan(doc, buffers, it->get<int>());
        if (!span ||
            !validTexCoordAccessor(*span, meshQuantizationEnabled) ||
            span->count != positions->count) {
            strictFailure = true;
            return std::nullopt;
        }
        texCoordSpans[*setIndex] = std::move(span);
    }

    std::optional<AccessorSpan> colors;
    if (attrs.contains("COLOR_0")) {
        if (!attrs["COLOR_0"].is_number_integer()) {
            strictFailure = true;
            return std::nullopt;
        }
        colors = accessorSpan(doc, buffers, attrs["COLOR_0"].get<int>());
        if (!colors || !validColorAccessor(*colors) ||
            colors->count != positions->count) {
            strictFailure = true;
            return std::nullopt;
        }
    }

    const bool hasSkinAttributes =
        attrs.contains("JOINTS_0") || attrs.contains("WEIGHTS_0");
    const int skinIndex =
        nodeIndex >= 0 && static_cast<size_t>(nodeIndex) < nodes.size()
            ? nodes[static_cast<size_t>(nodeIndex)].skin
            : -1;
    std::optional<std::vector<GltfVertexSkinning>> vertexSkinning;
    std::optional<std::vector<glm::dmat4>> jointMatrices;
    if (hasSkinAttributes || skinIndex >= 0) {
        if (skinIndex < 0 || static_cast<size_t>(skinIndex) >= skins.size()) {
            strictFailure = true;
            return std::nullopt;
        }
        vertexSkinning =
            readVertexSkinning(doc, buffers, attrs, positions->count);
        jointMatrices = computeJointMatrices(
            nodes,
            skins[static_cast<size_t>(skinIndex)]);
        if (!vertexSkinning || !jointMatrices) {
            strictFailure = true;
            return std::nullopt;
        }
    }
    std::optional<std::vector<GltfMorphTarget>> morphTargets =
        readMorphTargets(
            doc,
            buffers,
            primitiveJson,
            positions->count,
            tangents.has_value());
    if (!morphTargets) {
        strictFailure = true;
        return std::nullopt;
    }

    GltfPrimitive primitive;
    primitive.primitiveMode = *renderModeForPrimitiveMode(*primitiveMode);
    primitive.skinned = vertexSkinning.has_value();
    if (primitiveJson.contains("material")) {
        if (!primitiveJson["material"].is_number_integer()) {
            strictFailure = true;
            return std::nullopt;
        }
        const auto& materials = doc.value("materials", json::array());
        const int materialIndex = primitiveJson["material"].get<int>();
        if (materialIndex >= 0 &&
            static_cast<size_t>(materialIndex) < materials.size()) {
            const json& material = materials[static_cast<size_t>(materialIndex)];
            if (!material.is_object()) {
                strictFailure = true;
                return std::nullopt;
            }
            primitive.unlit = materialHasKhrMaterialsUnlit(material);
            if (auto doubleSided =
                    boolProperty(material, "doubleSided", false)) {
                primitive.doubleSided = *doubleSided;
            } else {
                strictFailure = true;
                return std::nullopt;
            }
            const auto alphaMode =
                stringProperty(material, "alphaMode", std::string{"OPAQUE"});
            if (!alphaMode) {
                strictFailure = true;
                return std::nullopt;
            }
            if (*alphaMode == "BLEND") {
                primitive.alphaMode = GltfAlphaMode::Blend;
            } else if (*alphaMode == "MASK") {
                primitive.alphaMode = GltfAlphaMode::Mask;
            } else if (*alphaMode == "OPAQUE") {
                primitive.alphaMode = GltfAlphaMode::Opaque;
            } else {
                strictFailure = true;
                return std::nullopt;
            }
            if (auto alphaCutoff =
                    numberProperty(material, "alphaCutoff", 0.5f)) {
                primitive.alphaCutoff = *alphaCutoff;
            } else {
                strictFailure = true;
                return std::nullopt;
            }
            const json* specGlossExtension = materialObjectExtension(
                material,
                "KHR_materials_pbrSpecularGlossiness");
            if (specGlossExtension) {
                primitive.specularGlossinessWorkflow = true;
                primitive.metallicFactor = 0.0f;
                primitive.roughnessFactor = 0.0f;
                primitive.baseColorFactor =
                    materialSpecularGlossinessDiffuseFactor(
                        material,
                        strictFailure);
                primitive.specularGlossinessSpecularFactor =
                    materialSpecularGlossinessSpecularFactor(
                        material,
                        strictFailure);
                primitive.specularGlossinessGlossinessFactor =
                    materialSpecularGlossinessGlossinessFactor(
                        material,
                        strictFailure);
                if (strictFailure) {
                    return std::nullopt;
                }

                auto diffuseTextureIt =
                    specGlossExtension->find("diffuseTexture");
                if (diffuseTextureIt != specGlossExtension->end() &&
                    diffuseTextureIt->is_object()) {
                    primitive.baseColorTexture = parseTextureBinding(
                        *diffuseTextureIt,
                        textures,
                        strictFailure);
                    if (strictFailure || !primitive.baseColorTexture) {
                        return std::nullopt;
                    }
                    primitive.baseColorTextureIndex =
                        primitive.baseColorTexture->textureIndex;
                } else if (diffuseTextureIt != specGlossExtension->end()) {
                    strictFailure = true;
                    return std::nullopt;
                }

                auto specularGlossinessTextureIt =
                    specGlossExtension->find("specularGlossinessTexture");
                if (specularGlossinessTextureIt !=
                        specGlossExtension->end() &&
                    specularGlossinessTextureIt->is_object()) {
                    primitive.specularGlossinessTexture = parseTextureBinding(
                        *specularGlossinessTextureIt,
                        textures,
                        strictFailure);
                    if (strictFailure ||
                        !primitive.specularGlossinessTexture) {
                        return std::nullopt;
                    }
                } else if (
                    specularGlossinessTextureIt !=
                    specGlossExtension->end()) {
                    strictFailure = true;
                    return std::nullopt;
                }
            } else {
                auto pbrIt = material.find("pbrMetallicRoughness");
                if (pbrIt != material.end() && pbrIt->is_object()) {
                    auto factorIt = pbrIt->find("baseColorFactor");
                    if (factorIt != pbrIt->end() &&
                        !readFloatArray(
                            *factorIt,
                            primitive.baseColorFactor)) {
                        strictFailure = true;
                        return std::nullopt;
                    }
                    if (auto metallic =
                            numberProperty(*pbrIt, "metallicFactor", 1.0f)) {
                        primitive.metallicFactor = *metallic;
                    } else {
                        strictFailure = true;
                        return std::nullopt;
                    }
                    if (auto roughness =
                            numberProperty(*pbrIt, "roughnessFactor", 1.0f)) {
                        primitive.roughnessFactor = *roughness;
                    } else {
                        strictFailure = true;
                        return std::nullopt;
                    }
                    auto textureIt = pbrIt->find("baseColorTexture");
                    if (textureIt != pbrIt->end() &&
                        textureIt->is_object()) {
                        primitive.baseColorTexture = parseTextureBinding(
                            *textureIt,
                            textures,
                            strictFailure);
                        if (strictFailure || !primitive.baseColorTexture) {
                            return std::nullopt;
                        }
                        primitive.baseColorTextureIndex =
                            primitive.baseColorTexture->textureIndex;
                    } else if (textureIt != pbrIt->end()) {
                        strictFailure = true;
                        return std::nullopt;
                    }
                    auto metallicRoughnessIt =
                        pbrIt->find("metallicRoughnessTexture");
                    if (metallicRoughnessIt != pbrIt->end() &&
                        metallicRoughnessIt->is_object()) {
                        primitive.metallicRoughnessTexture =
                            parseTextureBinding(
                                *metallicRoughnessIt,
                                textures,
                                strictFailure);
                        if (strictFailure ||
                            !primitive.metallicRoughnessTexture) {
                            return std::nullopt;
                        }
                    } else if (metallicRoughnessIt != pbrIt->end()) {
                        strictFailure = true;
                        return std::nullopt;
                    }
                } else if (pbrIt != material.end()) {
                    strictFailure = true;
                    return std::nullopt;
                }
            }

            primitive.dielectricSpecularF0 =
                materialDielectricSpecularF0(material, strictFailure);
            if (strictFailure) {
                return std::nullopt;
            }
            primitive.specularFactor =
                materialSpecularFactor(material, strictFailure);
            primitive.specularColorFactor =
                materialSpecularColorFactor(material, strictFailure);
            primitive.transmissionFactor =
                materialTransmissionFactor(material, strictFailure);
            primitive.anisotropyStrength =
                materialAnisotropyStrength(material, strictFailure);
            primitive.anisotropyRotation =
                materialAnisotropyRotation(material, strictFailure);
            primitive.clearcoatFactor =
                materialClearcoatFactor(material, strictFailure);
            primitive.clearcoatRoughnessFactor =
                materialClearcoatRoughnessFactor(material, strictFailure);
            primitive.sheenColorFactor =
                materialSheenColorFactor(material, strictFailure);
            primitive.sheenRoughnessFactor =
                materialSheenRoughnessFactor(material, strictFailure);
            if (strictFailure) {
                return std::nullopt;
            }
            const json* transmissionExtension =
                materialObjectExtension(
                    material,
                    "KHR_materials_transmission");
            if (transmissionExtension) {
                auto transmissionTextureIt =
                    transmissionExtension->find("transmissionTexture");
                if (transmissionTextureIt != transmissionExtension->end() &&
                    transmissionTextureIt->is_object()) {
                    primitive.transmissionTexture = parseTextureBinding(
                        *transmissionTextureIt,
                        textures,
                        strictFailure);
                    if (strictFailure || !primitive.transmissionTexture) {
                        return std::nullopt;
                    }
                } else if (
                    transmissionTextureIt != transmissionExtension->end()) {
                    strictFailure = true;
                    return std::nullopt;
                }
            }
            const json* anisotropyExtension =
                materialObjectExtension(material, "KHR_materials_anisotropy");
            if (anisotropyExtension) {
                auto anisotropyTextureIt =
                    anisotropyExtension->find("anisotropyTexture");
                if (anisotropyTextureIt != anisotropyExtension->end() &&
                    anisotropyTextureIt->is_object()) {
                    primitive.anisotropyTexture = parseTextureBinding(
                        *anisotropyTextureIt,
                        textures,
                        strictFailure);
                    if (strictFailure || !primitive.anisotropyTexture) {
                        return std::nullopt;
                    }
                } else if (
                    anisotropyTextureIt != anisotropyExtension->end()) {
                    strictFailure = true;
                    return std::nullopt;
                }
            }

            const json* specularExtension =
                materialObjectExtension(material, "KHR_materials_specular");
            if (specularExtension) {
                auto specularTextureIt =
                    specularExtension->find("specularTexture");
                if (specularTextureIt != specularExtension->end() &&
                    specularTextureIt->is_object()) {
                    primitive.specularTexture = parseTextureBinding(
                        *specularTextureIt,
                        textures,
                        strictFailure);
                    if (strictFailure || !primitive.specularTexture) {
                        return std::nullopt;
                    }
                } else if (specularTextureIt != specularExtension->end()) {
                    strictFailure = true;
                    return std::nullopt;
                }

                auto specularColorTextureIt =
                    specularExtension->find("specularColorTexture");
                if (specularColorTextureIt != specularExtension->end() &&
                    specularColorTextureIt->is_object()) {
                    primitive.specularColorTexture = parseTextureBinding(
                        *specularColorTextureIt,
                        textures,
                        strictFailure);
                    if (strictFailure ||
                        !primitive.specularColorTexture) {
                        return std::nullopt;
                    }
                } else if (
                    specularColorTextureIt != specularExtension->end()) {
                    strictFailure = true;
                    return std::nullopt;
                }
            }

            const json* clearcoatExtension =
                materialObjectExtension(material, "KHR_materials_clearcoat");
            if (clearcoatExtension) {
                auto clearcoatTextureIt =
                    clearcoatExtension->find("clearcoatTexture");
                if (clearcoatTextureIt != clearcoatExtension->end() &&
                    clearcoatTextureIt->is_object()) {
                    primitive.clearcoatTexture = parseTextureBinding(
                        *clearcoatTextureIt,
                        textures,
                        strictFailure);
                    if (strictFailure || !primitive.clearcoatTexture) {
                        return std::nullopt;
                    }
                } else if (clearcoatTextureIt != clearcoatExtension->end()) {
                    strictFailure = true;
                    return std::nullopt;
                }

                auto clearcoatRoughnessTextureIt =
                    clearcoatExtension->find("clearcoatRoughnessTexture");
                if (clearcoatRoughnessTextureIt != clearcoatExtension->end() &&
                    clearcoatRoughnessTextureIt->is_object()) {
                    primitive.clearcoatRoughnessTexture = parseTextureBinding(
                        *clearcoatRoughnessTextureIt,
                        textures,
                        strictFailure);
                    if (strictFailure ||
                        !primitive.clearcoatRoughnessTexture) {
                        return std::nullopt;
                    }
                } else if (
                    clearcoatRoughnessTextureIt != clearcoatExtension->end()) {
                    strictFailure = true;
                    return std::nullopt;
                }

                auto clearcoatNormalTextureIt =
                    clearcoatExtension->find("clearcoatNormalTexture");
                if (clearcoatNormalTextureIt != clearcoatExtension->end() &&
                    clearcoatNormalTextureIt->is_object()) {
                    primitive.clearcoatNormalTexture = parseTextureBinding(
                        *clearcoatNormalTextureIt,
                        textures,
                        strictFailure);
                    if (strictFailure || !primitive.clearcoatNormalTexture) {
                        return std::nullopt;
                    }
                    if (auto scale = numberProperty(
                            *clearcoatNormalTextureIt,
                            "scale",
                            1.0f)) {
                        primitive.clearcoatNormalTextureScale = *scale;
                    } else {
                        strictFailure = true;
                        return std::nullopt;
                    }
                } else if (
                    clearcoatNormalTextureIt != clearcoatExtension->end()) {
                    strictFailure = true;
                    return std::nullopt;
                }
            }

            const json* sheenExtension =
                materialObjectExtension(material, "KHR_materials_sheen");
            if (sheenExtension) {
                auto sheenColorTextureIt =
                    sheenExtension->find("sheenColorTexture");
                if (sheenColorTextureIt != sheenExtension->end() &&
                    sheenColorTextureIt->is_object()) {
                    primitive.sheenColorTexture = parseTextureBinding(
                        *sheenColorTextureIt,
                        textures,
                        strictFailure);
                    if (strictFailure || !primitive.sheenColorTexture) {
                        return std::nullopt;
                    }
                } else if (sheenColorTextureIt != sheenExtension->end()) {
                    strictFailure = true;
                    return std::nullopt;
                }

                auto sheenRoughnessTextureIt =
                    sheenExtension->find("sheenRoughnessTexture");
                if (sheenRoughnessTextureIt != sheenExtension->end() &&
                    sheenRoughnessTextureIt->is_object()) {
                    primitive.sheenRoughnessTexture = parseTextureBinding(
                        *sheenRoughnessTextureIt,
                        textures,
                        strictFailure);
                    if (strictFailure || !primitive.sheenRoughnessTexture) {
                        return std::nullopt;
                    }
                } else if (
                    sheenRoughnessTextureIt != sheenExtension->end()) {
                    strictFailure = true;
                    return std::nullopt;
                }
            }

            auto normalIt = material.find("normalTexture");
            if (normalIt != material.end() && normalIt->is_object()) {
                primitive.normalTexture =
                    parseTextureBinding(*normalIt, textures, strictFailure);
                if (strictFailure || !primitive.normalTexture) {
                    return std::nullopt;
                }
                if (auto scale =
                        numberProperty(*normalIt, "scale", 1.0f)) {
                    primitive.normalTextureScale = *scale;
                } else {
                    strictFailure = true;
                    return std::nullopt;
                }
            } else if (normalIt != material.end()) {
                strictFailure = true;
                return std::nullopt;
            }

            auto occlusionIt = material.find("occlusionTexture");
            if (occlusionIt != material.end() && occlusionIt->is_object()) {
                primitive.occlusionTexture =
                    parseTextureBinding(*occlusionIt, textures, strictFailure);
                if (strictFailure || !primitive.occlusionTexture) {
                    return std::nullopt;
                }
                if (auto strength =
                        numberProperty(*occlusionIt, "strength", 1.0f)) {
                    primitive.occlusionTextureStrength = *strength;
                } else {
                    strictFailure = true;
                    return std::nullopt;
                }
            } else if (occlusionIt != material.end()) {
                strictFailure = true;
                return std::nullopt;
            }

            auto emissiveFactorIt = material.find("emissiveFactor");
            if (emissiveFactorIt != material.end() &&
                !readFloatArray(*emissiveFactorIt, primitive.emissiveFactor)) {
                strictFailure = true;
                return std::nullopt;
            }
            const float emissiveStrength =
                materialEmissiveStrength(material, strictFailure);
            if (strictFailure) {
                return std::nullopt;
            }
            for (float& component : primitive.emissiveFactor) {
                component *= emissiveStrength;
            }
            auto emissiveTextureIt = material.find("emissiveTexture");
            if (emissiveTextureIt != material.end() &&
                emissiveTextureIt->is_object()) {
                primitive.emissiveTexture = parseTextureBinding(
                    *emissiveTextureIt,
                    textures,
                    strictFailure);
                if (strictFailure || !primitive.emissiveTexture) {
                    return std::nullopt;
                }
            } else if (emissiveTextureIt != material.end()) {
                strictFailure = true;
                return std::nullopt;
            }
        } else {
            strictFailure = true;
            return std::nullopt;
        }
    }
    auto hasTexCoordSet = [&](const std::optional<GltfTextureBinding>& binding) {
        if (!binding) {
            return true;
        }
        return validTexCoordSetIndex(binding->texCoord) &&
               texCoordSpans[static_cast<size_t>(binding->texCoord)]
                   .has_value();
    };
    if (!hasTexCoordSet(primitive.baseColorTexture) ||
        !hasTexCoordSet(primitive.metallicRoughnessTexture) ||
        !hasTexCoordSet(primitive.anisotropyTexture) ||
        !hasTexCoordSet(primitive.specularTexture) ||
        !hasTexCoordSet(primitive.specularColorTexture) ||
        !hasTexCoordSet(primitive.specularGlossinessTexture) ||
        !hasTexCoordSet(primitive.transmissionTexture) ||
        !hasTexCoordSet(primitive.clearcoatTexture) ||
        !hasTexCoordSet(primitive.clearcoatRoughnessTexture) ||
        !hasTexCoordSet(primitive.clearcoatNormalTexture) ||
        !hasTexCoordSet(primitive.sheenColorTexture) ||
        !hasTexCoordSet(primitive.sheenRoughnessTexture) ||
        !hasTexCoordSet(primitive.normalTexture) ||
        !hasTexCoordSet(primitive.occlusionTexture) ||
        !hasTexCoordSet(primitive.emissiveTexture)) {
        strictFailure = true;
        return std::nullopt;
    }
    const bool anisotropyActive =
        primitive.anisotropyStrength > 0.0f ||
        primitive.anisotropyTexture.has_value();
    if (anisotropyActive && !tangents.has_value()) {
        const int anisotropyTexCoord =
            primitive.anisotropyTexture
                ? primitive.anisotropyTexture->texCoord
                : 0;
        if (!validTexCoordSetIndex(anisotropyTexCoord) ||
            !texCoordSpans[static_cast<size_t>(anisotropyTexCoord)]
                .has_value()) {
            strictFailure = true;
            return std::nullopt;
        }
    }
    primitive.vertices.resize(positions->count);
    for (size_t texCoordSet = 0;
         texCoordSet < kGltfMaxTexCoordSets;
         ++texCoordSet) {
        if (texCoordSpans[texCoordSet]) {
            primitive.vertexTexCoords[texCoordSet].resize(positions->count);
        }
    }
    if (colors) {
        primitive.vertexColors.assign(
            positions->count,
            {1.0f, 1.0f, 1.0f, 1.0f});
    }
    std::optional<AccessorSpan> batchIds;
    if (allowLegacyBatchIdAttribute && attrs.contains("_BATCHID")) {
        if (!attrs["_BATCHID"].is_number_integer()) {
            strictFailure = true;
            return std::nullopt;
        }
        batchIds = accessorSpan(doc, buffers, attrs["_BATCHID"].get<int>());
        if (!batchIds ||
            !validFeatureIdAccessor(*batchIds) ||
            batchIds->count != positions->count) {
            strictFailure = true;
            return std::nullopt;
        }
        primitive.featureIds.resize(positions->count);
    }
    if (tangents) {
        primitive.vertexTangents.resize(positions->count);
        primitive.runtime.baseTangents.resize(positions->count);
    }
    primitive.runtime.nodeIndex = nodeIndex;
    primitive.runtime.skinIndex = skinIndex;
    primitive.runtime.baseVertices.resize(positions->count);
    primitive.runtime.hasNormals = normals.has_value();
    primitive.runtime.hasTangents = tangents.has_value();
    primitive.runtime.morphTargets = std::move(*morphTargets);
    if (vertexSkinning) {
        primitive.runtime.skinning = *vertexSkinning;
    }
    const glm::dmat4 transform =
        nodeIndex >= 0 && static_cast<size_t>(nodeIndex) < nodes.size()
            ? nodes[static_cast<size_t>(nodeIndex)].globalTransform
            : glm::dmat4(1.0);
    const glm::dmat3 normalMatrix =
        glm::transpose(glm::inverse(glm::dmat3(transform)));
    for (size_t i = 0; i < positions->count; ++i) {
        const glm::dvec3 localPosition = readVec3(*positions, i);
        const glm::dvec3 localNormal =
            normals ? readVec3(*normals, i) : glm::dvec3(0.0, 0.0, 1.0);
        if (!finiteVec3Value(localPosition) ||
            (normals && !finiteVec3Value(localNormal))) {
            strictFailure = true;
            return std::nullopt;
        }
        std::optional<std::array<float, 4>> localTangent;
        if (tangents) {
            localTangent = readTangent(*tangents, i);
            if (!localTangent) {
                strictFailure = true;
                return std::nullopt;
            }
            primitive.runtime.baseTangents[i] = *localTangent;
        }
        primitive.runtime.baseVertices[i].positionEcef = Vec3(localPosition);
        primitive.runtime.baseVertices[i].normalEcef =
            normals ? Vec3(localNormal) : Vec3::zero();

        if (vertexSkinning && jointMatrices) {
            auto skinned = skinVertex(
                localPosition,
                localNormal,
                (*vertexSkinning)[i],
                *jointMatrices);
            if (!skinned) {
                strictFailure = true;
                return std::nullopt;
            }
            primitive.vertices[i].positionEcef = skinned->first;
            primitive.vertices[i].normalEcef =
                normals ? skinned->second : Vec3::zero();
        } else {
            const glm::dvec3 position =
                glm::dvec3(transform * glm::dvec4(localPosition, 1.0));
            primitive.vertices[i].positionEcef = Vec3(position);
            if (normals) {
                glm::dvec3 normal = normalMatrix * localNormal;
                if (glm::dot(normal, normal) > 0.0) {
                    normal = glm::normalize(normal);
                } else {
                    normal = glm::dvec3(0.0, 0.0, 1.0);
                }
                primitive.vertices[i].normalEcef = Vec3(normal);
            }
        }
        if (localTangent) {
            std::optional<std::array<float, 4>> finalTangent;
            if (vertexSkinning && jointMatrices) {
                const auto skinnedTangent = skinDirection(
                    glm::dvec3(
                        (*localTangent)[0],
                        (*localTangent)[1],
                        (*localTangent)[2]),
                    (*vertexSkinning)[i],
                    *jointMatrices);
                if (skinnedTangent) {
                    finalTangent =
                        makeTangent(*skinnedTangent, (*localTangent)[3]);
                }
            } else {
                finalTangent = transformTangent(normalMatrix, *localTangent);
            }
            if (!finalTangent) {
                strictFailure = true;
                return std::nullopt;
            }
            primitive.vertexTangents[i] = *finalTangent;
        }

        for (size_t texCoordSet = 0;
             texCoordSet < kGltfMaxTexCoordSets;
             ++texCoordSet) {
            if (!texCoordSpans[texCoordSet]) {
                continue;
            }
            const glm::dvec2 uv = readVec2(*texCoordSpans[texCoordSet], i);
            if (!finiteVec2Value(uv)) {
                strictFailure = true;
                return std::nullopt;
            }
            const std::array<float, 2> texCoord = {
                static_cast<float>(uv.x),
                static_cast<float>(uv.y)};
            primitive.vertexTexCoords[texCoordSet][i] = texCoord;
            if (texCoordSet == 0) {
                primitive.vertices[i].uv = texCoord;
                primitive.runtime.baseVertices[i].uv =
                    primitive.vertices[i].uv;
            }
        }
        if (colors) {
            const auto color = readVertexColor(*colors, i);
            if (!color) {
                strictFailure = true;
                return std::nullopt;
            }
            primitive.vertexColors[i] = *color;
        }
        if (batchIds) {
            primitive.featureIds[i] = readIndexComponent(
                accessorElementPtr(*batchIds, i),
                batchIds->componentType);
        }
    }
    if (primitiveJson.contains("indices")) {
        if (!primitiveJson["indices"].is_number_integer()) {
            strictFailure = true;
            return std::nullopt;
        }
        const auto indexSpan = accessorSpan(
            doc,
            buffers,
            primitiveJson["indices"].get<int>());
        if (!indexSpan ||
            indexSpan->components != 1 ||
            indexSpan->normalized ||
            (indexSpan->componentType != 5121 &&
             indexSpan->componentType != 5123 &&
             indexSpan->componentType != 5125)) {
            strictFailure = true;
            return std::nullopt;
        }
        primitive.indices = readIndices(*indexSpan);
    } else {
        primitive.indices.resize(positions->count);
        for (size_t i = 0; i < positions->count; ++i) {
            primitive.indices[i] = static_cast<uint32_t>(i);
        }
    }
    if (!normalizeIndicesForPrimitiveMode(
            primitive.indices,
            *primitiveMode,
            primitive.primitiveMode) ||
        std::any_of(
            primitive.indices.begin(),
            primitive.indices.end(),
            [vertexCount = positions->count](uint32_t index) {
                return index >= vertexCount;
            })) {
        strictFailure = true;
        return std::nullopt;
    }

    if (!normals) {
        resolveMissingNormals(primitive);
    }
    return primitive;
}

void traverseNode(
    const json& doc,
    const std::vector<std::vector<uint8_t>>& buffers,
    const std::vector<GltfTexture>& textures,
    const std::vector<NodeRecord>& nodeRecords,
    const std::vector<SkinRecord>& skins,
    int nodeIndex,
    GltfModel& model,
    bool meshQuantizationEnabled,
    bool allowLegacyBatchIdAttribute,
    bool& strictFailure) {
    const auto& nodes = doc.value("nodes", json::array());
    const auto& meshes = doc.value("meshes", json::array());
    if (nodeIndex < 0 || static_cast<size_t>(nodeIndex) >= nodes.size()) {
        strictFailure = true;
        return;
    }

    const json& node = nodes[static_cast<size_t>(nodeIndex)];
    const glm::dmat4 nodeGlobalTransform =
        static_cast<size_t>(nodeIndex) < nodeRecords.size()
            ? nodeRecords[static_cast<size_t>(nodeIndex)].globalTransform
            : glm::dmat4(1.0);
    std::optional<std::vector<GltfInstance>> nativeInstances =
        parseGpuInstancingInstances(
            doc,
            buffers,
            node,
            nodeGlobalTransform,
            strictFailure);
    if (!nativeInstances || strictFailure) {
        return;
    }
    if (node.contains("mesh")) {
        if (!node["mesh"].is_number_integer()) {
            strictFailure = true;
            return;
        }
        const int meshIndex = node["mesh"].get<int>();
        if (meshIndex >= 0 && static_cast<size_t>(meshIndex) < meshes.size()) {
            const auto& primitives =
                meshes[static_cast<size_t>(meshIndex)].value(
                    "primitives",
                    json::array());
            for (const json& primitiveJson : primitives) {
                if (auto primitive = parsePrimitive(
                        doc,
                        buffers,
                        textures,
                        nodeRecords,
                        skins,
                        nodeIndex,
                        primitiveJson,
                        meshQuantizationEnabled,
                        allowLegacyBatchIdAttribute,
                        strictFailure)) {
                    if (!nativeInstances->empty()) {
                        primitive->instances = *nativeInstances;
                    }
                    model.primitives.push_back(std::move(*primitive));
                }
                if (strictFailure) return;
            }
        } else {
            strictFailure = true;
            return;
        }
    }

    for (const json& child : node.value("children", json::array())) {
        if (!child.is_number_integer()) {
            strictFailure = true;
            return;
        }
        traverseNode(
            doc,
            buffers,
            textures,
            nodeRecords,
            skins,
            child.get<int>(),
            model,
            meshQuantizationEnabled,
            allowLegacyBatchIdAttribute,
            strictFailure);
        if (strictFailure) return;
    }
}

std::vector<double> sampleAnimationSampler(
    const GltfAnimationSamplerRuntime& sampler,
    GltfAnimationPath path,
    double timeSeconds) {
    const size_t keyframes = sampler.inputTimes.size();
    const int components = sampler.outputComponents;
    const size_t outputElementsPerKey =
        animationOutputElementsPerKey(sampler.interpolation);
    std::vector<double> result(static_cast<size_t>(components), 0.0);
    if (keyframes == 0 || components <= 0 ||
        sampler.outputValues.size() <
            keyframes * outputElementsPerKey *
                static_cast<size_t>(components)) {
        return result;
    }

    auto keyValueOffset = [&](size_t keyIndex) {
        const size_t elementIndex =
            sampler.interpolation == GltfAnimationInterpolation::CubicSpline
                ? keyIndex * 3u + 1u
                : keyIndex;
        return elementIndex * static_cast<size_t>(components);
    };

    auto copyKey = [&](size_t keyIndex) {
        const size_t offset = keyValueOffset(keyIndex);
        for (int c = 0; c < components; ++c) {
            result[static_cast<size_t>(c)] =
                sampler.outputValues[offset + static_cast<size_t>(c)];
        }
    };

    if (keyframes == 1 || timeSeconds <= sampler.inputTimes.front()) {
        copyKey(0);
        return result;
    }
    if (timeSeconds >= sampler.inputTimes.back()) {
        copyKey(keyframes - 1);
        return result;
    }

    auto upper = std::upper_bound(
        sampler.inputTimes.begin(),
        sampler.inputTimes.end(),
        timeSeconds);
    size_t nextIndex = static_cast<size_t>(
        std::distance(sampler.inputTimes.begin(), upper));
    if (nextIndex == 0) nextIndex = 1;
    const size_t prevIndex = nextIndex - 1;
    const double t0 = sampler.inputTimes[prevIndex];
    const double t1 = sampler.inputTimes[nextIndex];
    const double denom = t1 - t0;
    const double t = denom > 0.0
        ? std::clamp((timeSeconds - t0) / denom, 0.0, 1.0)
        : 0.0;

    if (sampler.interpolation == GltfAnimationInterpolation::Step) {
        copyKey(prevIndex);
        return result;
    }

    if (sampler.interpolation == GltfAnimationInterpolation::CubicSpline) {
        const size_t prevBase =
            prevIndex * 3u * static_cast<size_t>(components);
        const size_t nextBase =
            nextIndex * 3u * static_cast<size_t>(components);
        const size_t prevValue = prevBase + static_cast<size_t>(components);
        const size_t prevOutTangent =
            prevBase + 2u * static_cast<size_t>(components);
        const size_t nextInTangent = nextBase;
        const size_t nextValue = nextBase + static_cast<size_t>(components);
        const double t2 = t * t;
        const double t3 = t2 * t;
        const double h00 = 2.0 * t3 - 3.0 * t2 + 1.0;
        const double h10 = t3 - 2.0 * t2 + t;
        const double h01 = -2.0 * t3 + 3.0 * t2;
        const double h11 = t3 - t2;
        const double deltaTime = std::max(0.0, t1 - t0);
        for (int c = 0; c < components; ++c) {
            const size_t component = static_cast<size_t>(c);
            const double p0 = sampler.outputValues[prevValue + component];
            const double m0 =
                sampler.outputValues[prevOutTangent + component] * deltaTime;
            const double p1 = sampler.outputValues[nextValue + component];
            const double m1 =
                sampler.outputValues[nextInTangent + component] * deltaTime;
            result[component] = h00 * p0 + h10 * m0 + h01 * p1 + h11 * m1;
        }
        if (path == GltfAnimationPath::Rotation && components == 4) {
            glm::dquat q(result[3], result[0], result[1], result[2]);
            if (glm::dot(q, q) > 0.0) {
                q = glm::normalize(q);
                result[0] = q.x;
                result[1] = q.y;
                result[2] = q.z;
                result[3] = q.w;
            }
        }
        return result;
    }

    const size_t prevOffset = keyValueOffset(prevIndex);
    const size_t nextOffset = keyValueOffset(nextIndex);
    if (sampler.interpolation == GltfAnimationInterpolation::Linear &&
        path == GltfAnimationPath::Rotation && components == 4) {
        glm::dquat q0(
            sampler.outputValues[prevOffset + 3],
            sampler.outputValues[prevOffset + 0],
            sampler.outputValues[prevOffset + 1],
            sampler.outputValues[prevOffset + 2]);
        glm::dquat q1(
            sampler.outputValues[nextOffset + 3],
            sampler.outputValues[nextOffset + 0],
            sampler.outputValues[nextOffset + 1],
            sampler.outputValues[nextOffset + 2]);
        q0 = glm::normalize(q0);
        q1 = glm::normalize(q1);
        const glm::dquat q = glm::normalize(glm::slerp(q0, q1, t));
        result[0] = q.x;
        result[1] = q.y;
        result[2] = q.z;
        result[3] = q.w;
        return result;
    }

    for (int c = 0; c < components; ++c) {
        const double a = sampler.outputValues[
            prevOffset + static_cast<size_t>(c)];
        const double b = sampler.outputValues[
            nextOffset + static_cast<size_t>(c)];
        result[static_cast<size_t>(c)] = a + (b - a) * t;
    }
    return result;
}

void resetRuntimeNodes(GltfModel& model) {
    for (GltfNodeRuntime& node : model.nodes) {
        node.translation = node.baseTranslation;
        node.rotation = node.baseRotation;
        node.scale = node.baseScale;
        node.weights = node.baseWeights;
        node.localTransform = node.hasMatrix
            ? node.baseLocalTransform
            : Mat4(composeTrs(
                  node.translation,
                  node.rotation,
                  node.scale));
    }
}

void resolveRuntimeNodeGlobals(GltfModel& model) {
    std::vector<bool> resolved(model.nodes.size(), false);
    std::function<void(int, const glm::dmat4&)> visit =
        [&](int nodeIndex, const glm::dmat4& parentTransform) {
            if (nodeIndex < 0 ||
                static_cast<size_t>(nodeIndex) >= model.nodes.size()) {
                return;
            }
            resolved[static_cast<size_t>(nodeIndex)] = true;
            GltfNodeRuntime& node = model.nodes[static_cast<size_t>(nodeIndex)];
            node.globalTransform =
                Mat4(parentTransform * node.localTransform.raw());
            for (int child : node.children) {
                visit(child, node.globalTransform.raw());
            }
        };

    for (int root : model.sceneRootNodes) {
        visit(root, glm::dmat4(1.0));
    }
    for (size_t i = 0; i < model.nodes.size(); ++i) {
        if (!resolved[i]) {
            visit(static_cast<int>(i), glm::dmat4(1.0));
        }
    }
}

std::optional<std::vector<glm::dmat4>> computeRuntimeJointMatrices(
    const GltfModel& model,
    int skinIndex) {
    if (skinIndex < 0 ||
        static_cast<size_t>(skinIndex) >= model.skins.size()) {
        return std::nullopt;
    }
    const GltfSkinRuntime& skin = model.skins[static_cast<size_t>(skinIndex)];
    if (skin.joints.empty() ||
        skin.inverseBindMatrices.size() != skin.joints.size()) {
        return std::nullopt;
    }
    std::vector<glm::dmat4> jointMatrices(skin.joints.size(), glm::dmat4(1.0));
    for (size_t i = 0; i < skin.joints.size(); ++i) {
        const int jointNodeIndex = skin.joints[i];
        if (jointNodeIndex < 0 ||
            static_cast<size_t>(jointNodeIndex) >= model.nodes.size()) {
            return std::nullopt;
        }
        jointMatrices[i] =
            model.nodes[static_cast<size_t>(jointNodeIndex)]
                .globalTransform.raw() *
            skin.inverseBindMatrices[i].raw();
    }
    return jointMatrices;
}

bool rebuildRuntimePrimitive(GltfModel& model, GltfPrimitive& primitive) {
    const GltfPrimitiveRuntime& runtime = primitive.runtime;
    if (runtime.baseVertices.empty() ||
        runtime.baseVertices.size() != primitive.vertices.size()) {
        return true;
    }
    if (runtime.nodeIndex < 0 ||
        static_cast<size_t>(runtime.nodeIndex) >= model.nodes.size()) {
        return false;
    }

    const GltfNodeRuntime& node =
        model.nodes[static_cast<size_t>(runtime.nodeIndex)];
    const bool skinned = !runtime.skinning.empty();
    std::optional<std::vector<glm::dmat4>> jointMatrices;
    if (skinned) {
        if (runtime.skinning.size() != runtime.baseVertices.size()) {
            return false;
        }
        jointMatrices =
            computeRuntimeJointMatrices(model, runtime.skinIndex);
        if (!jointMatrices) {
            return false;
        }
    }
    if (runtime.hasTangents) {
        if (runtime.baseTangents.size() != runtime.baseVertices.size()) {
            return false;
        }
        if (primitive.vertexTangents.size() != runtime.baseVertices.size()) {
            primitive.vertexTangents.resize(runtime.baseVertices.size());
        }
    }

    const glm::dmat4 nodeTransform = node.globalTransform.raw();
    const glm::dmat3 normalMatrix =
        glm::transpose(glm::inverse(glm::dmat3(nodeTransform)));

    for (size_t i = 0; i < runtime.baseVertices.size(); ++i) {
        const SurfaceVertex& base = runtime.baseVertices[i];
        glm::dvec3 localPosition = base.positionEcef.raw();
        glm::dvec3 localNormal = runtime.hasNormals
            ? base.normalEcef.raw()
            : glm::dvec3(0.0, 0.0, 1.0);
        glm::dvec3 localTangentDirection(0.0);
        double localTangentHandedness = 1.0;
        if (runtime.hasTangents) {
            const std::array<float, 4>& baseTangent =
                runtime.baseTangents[i];
            localTangentDirection = glm::dvec3(
                baseTangent[0],
                baseTangent[1],
                baseTangent[2]);
            localTangentHandedness = baseTangent[3];
        }

        for (size_t targetIndex = 0;
             targetIndex < runtime.morphTargets.size();
             ++targetIndex) {
            const double weight =
                targetIndex < node.weights.size()
                    ? node.weights[targetIndex]
                    : 0.0;
            if (weight == 0.0) continue;
            const GltfMorphTarget& target =
                runtime.morphTargets[targetIndex];
            if (i < target.positionDeltas.size()) {
                localPosition += target.positionDeltas[i].raw() * weight;
            }
            if (runtime.hasNormals && i < target.normalDeltas.size()) {
                localNormal += target.normalDeltas[i].raw() * weight;
            }
            if (runtime.hasTangents && i < target.tangentDeltas.size()) {
                localTangentDirection +=
                    target.tangentDeltas[i].raw() * weight;
            }
        }

        if (skinned && jointMatrices) {
            auto result = skinVertex(
                localPosition,
                localNormal,
                runtime.skinning[i],
                *jointMatrices);
            if (!result) return false;
            primitive.vertices[i].positionEcef = result->first;
            primitive.vertices[i].normalEcef =
                runtime.hasNormals ? result->second : Vec3::zero();
            if (runtime.hasTangents) {
                const auto tangentDirection = skinDirection(
                    localTangentDirection,
                    runtime.skinning[i],
                    *jointMatrices);
                const auto tangent =
                    tangentDirection
                        ? makeTangent(
                              *tangentDirection,
                              localTangentHandedness)
                        : std::nullopt;
                if (!tangent) return false;
                primitive.vertexTangents[i] = *tangent;
            }
        } else {
            primitive.vertices[i].positionEcef = Vec3(glm::dvec3(
                nodeTransform * glm::dvec4(localPosition, 1.0)));
            if (runtime.hasNormals) {
                glm::dvec3 normal = normalMatrix * localNormal;
                if (glm::dot(normal, normal) > 0.0) {
                    normal = glm::normalize(normal);
                } else {
                    normal = glm::dvec3(0.0, 0.0, 1.0);
                }
                primitive.vertices[i].normalEcef = Vec3(normal);
            }
            if (runtime.hasTangents) {
                const auto localTangent =
                    makeTangent(
                        localTangentDirection,
                        localTangentHandedness);
                const auto tangent = localTangent
                    ? transformTangent(normalMatrix, *localTangent)
                    : std::nullopt;
                if (!tangent) return false;
                primitive.vertexTangents[i] = *tangent;
            }
        }
        primitive.vertices[i].uv = base.uv;
    }

    if (!runtime.hasNormals) {
        resolveMissingNormals(primitive);
    }
    primitive.skinned = skinned;
    return true;
}

bool rebuildRuntimeModel(GltfModel& model) {
    resolveRuntimeNodeGlobals(model);
    for (GltfPrimitive& primitive : model.primitives) {
        if (!rebuildRuntimePrimitive(model, primitive)) {
            return false;
        }
    }
    return true;
}

} // namespace

size_t GltfModel::vertexCount() const {
    size_t count = 0;
    for (const GltfPrimitive& primitive : primitives) {
        count += primitive.vertices.size();
    }
    return count;
}

size_t GltfModel::indexCount() const {
    size_t count = 0;
    for (const GltfPrimitive& primitive : primitives) {
        count += primitive.indices.size();
    }
    return count;
}

int64_t GltfModel::byteSize() const {
    int64_t bytes = 0;
    for (const GltfPrimitive& primitive : primitives) {
        bytes += static_cast<int64_t>(
            primitive.vertices.size() * sizeof(SurfaceVertex));
        for (const auto& texCoords : primitive.vertexTexCoords) {
            bytes += static_cast<int64_t>(
                texCoords.size() * sizeof(std::array<float, 2>));
        }
        bytes += static_cast<int64_t>(
            primitive.vertexColors.size() * sizeof(std::array<float, 4>));
        bytes += static_cast<int64_t>(
            primitive.vertexTangents.size() * sizeof(std::array<float, 4>));
        bytes += static_cast<int64_t>(
            primitive.indices.size() * sizeof(uint32_t));
        bytes += static_cast<int64_t>(
            primitive.featureIds.size() * sizeof(uint32_t));
        for (const auto& properties : primitive.featureProperties) {
            bytes += gltfFeaturePropertiesByteSize(properties);
        }
        bytes += static_cast<int64_t>(
            primitive.instances.size() * sizeof(GltfInstance));
        for (const GltfInstance& instance : primitive.instances) {
            bytes += gltfFeaturePropertiesByteSize(
                instance.featureProperties);
        }
    }
    for (const GltfTexture& texture : textures) {
        bytes += static_cast<int64_t>(texture.image.pixels.size());
    }
    return bytes;
}

bool GltfModel::hasRuntimeAnimation() const {
    return !animations.empty();
}

size_t GltfModel::animationCount() const {
    return animations.size();
}

int GltfModel::activeAnimation() const {
    return activeAnimationIndex;
}

bool GltfModel::setActiveAnimation(int index) {
    if (index < 0 || static_cast<size_t>(index) >= animations.size()) {
        return false;
    }
    if (activeAnimationIndex == index) {
        return true;
    }
    activeAnimationIndex = index;
    lastAnimationTimeSeconds = -1.0;
    return true;
}

bool GltfModel::isAnimationLooping() const {
    return animationLooping;
}

void GltfModel::setAnimationLooping(bool looping) {
    if (animationLooping == looping) {
        return;
    }
    animationLooping = looping;
    lastAnimationTimeSeconds = -1.0;
}

bool GltfModel::isAnimationPaused() const {
    return animationPaused;
}

void GltfModel::setAnimationPaused(bool paused) {
    animationPaused = paused;
}

uint64_t GltfModel::currentAnimationRevision() const {
    return animationRevision;
}

bool GltfModel::updateAnimation(double timeSeconds) {
    if (animations.empty() ||
        animationPaused ||
        activeAnimationIndex < 0 ||
        static_cast<size_t>(activeAnimationIndex) >= animations.size()) {
        return false;
    }

    const GltfAnimationRuntime& animation =
        animations[static_cast<size_t>(activeAnimationIndex)];
    if (animation.durationSeconds <= 0.0 || animation.channels.empty()) {
        return false;
    }

    double localTime = 0.0;
    if (animationLooping) {
        localTime = std::fmod(timeSeconds, animation.durationSeconds);
        if (localTime < 0.0) {
            localTime += animation.durationSeconds;
        }
    } else {
        localTime =
            std::clamp(timeSeconds, 0.0, animation.durationSeconds);
    }
    if (lastAnimationTimeSeconds >= 0.0 &&
        std::abs(localTime - lastAnimationTimeSeconds) < 1e-6) {
        return false;
    }

    resetRuntimeNodes(*this);
    for (const GltfAnimationChannelRuntime& channel : animation.channels) {
        if (channel.targetNode < 0 ||
            static_cast<size_t>(channel.targetNode) >= nodes.size() ||
            channel.samplerIndex < 0 ||
            static_cast<size_t>(channel.samplerIndex) >=
                animation.samplers.size()) {
            continue;
        }

        const GltfAnimationSamplerRuntime& sampler =
            animation.samplers[static_cast<size_t>(channel.samplerIndex)];
        std::vector<double> values =
            sampleAnimationSampler(sampler, channel.path, localTime);
        GltfNodeRuntime& node =
            nodes[static_cast<size_t>(channel.targetNode)];
        switch (channel.path) {
            case GltfAnimationPath::Translation:
                if (values.size() == 3) {
                    node.translation = {values[0], values[1], values[2]};
                    node.localTransform =
                        Mat4(composeTrs(
                            node.translation,
                            node.rotation,
                            node.scale));
                }
                break;
            case GltfAnimationPath::Rotation:
                if (values.size() == 4) {
                    glm::dquat q(values[3], values[0], values[1], values[2]);
                    const double lengthSquared = glm::dot(q, q);
                    if (!std::isfinite(lengthSquared) ||
                        lengthSquared <= 0.0) {
                        return false;
                    }
                    q = glm::normalize(q);
                    if (!std::isfinite(q.w) ||
                        !std::isfinite(q.x) ||
                        !std::isfinite(q.y) ||
                        !std::isfinite(q.z)) {
                        return false;
                    }
                    node.rotation = toArrayQuat(q);
                    node.localTransform =
                        Mat4(composeTrs(
                            node.translation,
                            node.rotation,
                            node.scale));
                }
                break;
            case GltfAnimationPath::Scale:
                if (values.size() == 3) {
                    node.scale = {values[0], values[1], values[2]};
                    node.localTransform =
                        Mat4(composeTrs(
                            node.translation,
                            node.rotation,
                            node.scale));
                }
                break;
            case GltfAnimationPath::Weights:
                node.weights = std::move(values);
                break;
        }
    }

    if (!rebuildRuntimeModel(*this)) {
        return false;
    }
    lastAnimationTimeSeconds = localTime;
    ++animationRevision;
    return true;
}

std::unique_ptr<GltfModel> GltfParser::parse(const uint8_t* data, size_t size) {
    return parse(data, size, ExternalResourceResolver{});
}

std::unique_ptr<GltfModel> GltfParser::parse(
    const uint8_t* data,
    size_t size,
    const ExternalResourceResolver& externalResourceResolver) {
    return parse(data, size, externalResourceResolver, ImageDecoder{});
}

std::unique_ptr<GltfModel> GltfParser::parse(
    const uint8_t* data,
    size_t size,
    const ExternalResourceResolver& externalResourceResolver,
    const ImageDecoder& imageDecoder) {
    return parse(
        data,
        size,
        externalResourceResolver,
        imageDecoder,
        GltfParserOptions{});
}

std::unique_ptr<GltfModel> GltfParser::parse(
    const uint8_t* data,
    size_t size,
    const ExternalResourceResolver& externalResourceResolver,
    const ImageDecoder& imageDecoder,
    const GltfParserOptions& options) {
    auto input = parseInput(data, size);
    if (!input) return nullptr;
    if (!validateAsset(input->document) ||
        hasUnsupportedDeclaredExtensions(input->document) ||
        !requiredExtensionsAreUsed(input->document) ||
        hasUnsupportedObjectExtensions(input->document) ||
        !supportedObjectExtensionsAreDeclared(input->document) ||
        !validateSceneGraph(
            input->document,
            options.allowLegacyBatchIdAttribute)) {
        return nullptr;
    }
    if (declaredExtension(input->document, "KHR_materials_unlit") &&
        !documentHasObjectExtension(
            input->document,
            "KHR_materials_unlit")) {
        return nullptr;
    }
    if (declaredExtension(input->document, "KHR_texture_transform") &&
        !documentHasObjectExtension(input->document, "KHR_texture_transform")) {
        return nullptr;
    }
    if (declaredExtension(
            input->document,
            "KHR_materials_emissive_strength") &&
        !documentHasObjectExtension(
            input->document,
            "KHR_materials_emissive_strength")) {
        return nullptr;
    }
    if (declaredExtension(input->document, "KHR_materials_ior") &&
        !documentHasObjectExtension(input->document, "KHR_materials_ior")) {
        return nullptr;
    }
    if (declaredExtension(
            input->document,
            "KHR_materials_pbrSpecularGlossiness") &&
        !documentHasObjectExtension(
            input->document,
            "KHR_materials_pbrSpecularGlossiness")) {
        return nullptr;
    }
    if (declaredExtension(input->document, "KHR_materials_transmission") &&
        !documentHasObjectExtension(
            input->document,
            "KHR_materials_transmission")) {
        return nullptr;
    }
    if (declaredExtension(input->document, "KHR_materials_anisotropy") &&
        !documentHasObjectExtension(
            input->document,
            "KHR_materials_anisotropy")) {
        return nullptr;
    }
    if (declaredExtension(input->document, "KHR_materials_specular") &&
        !documentHasObjectExtension(
            input->document,
            "KHR_materials_specular")) {
        return nullptr;
    }
    if (declaredExtension(input->document, "KHR_materials_clearcoat") &&
        !documentHasObjectExtension(
            input->document,
            "KHR_materials_clearcoat")) {
        return nullptr;
    }
    if (declaredExtension(input->document, "KHR_materials_sheen") &&
        !documentHasObjectExtension(
            input->document,
            "KHR_materials_sheen")) {
        return nullptr;
    }
    if (declaredExtension(input->document, "EXT_texture_webp") &&
        !documentHasObjectExtension(input->document, "EXT_texture_webp")) {
        return nullptr;
    }
    if (declaredExtension(input->document, "EXT_mesh_gpu_instancing") &&
        !documentHasObjectExtension(
            input->document,
            "EXT_mesh_gpu_instancing")) {
        return nullptr;
    }
    if (declaredExtension(input->document, "EXT_mesh_gpu_instancing")) {
        const auto animationsIt = input->document.find("animations");
        if (animationsIt != input->document.end()) {
            if (!animationsIt->is_array() || !animationsIt->empty()) {
                return nullptr;
            }
        }
    }
    const bool meshQuantizationEnabled =
        declaredExtension(input->document, "KHR_mesh_quantization");
    if (meshQuantizationEnabled &&
        !documentUsesMeshQuantization(input->document)) {
        return nullptr;
    }

    auto loadedBuffers =
        loadBuffers(
            input->document,
            input->binaryChunk,
            externalResourceResolver);
    if (!loadedBuffers) {
        return nullptr;
    }
    std::vector<std::vector<uint8_t>> buffers = std::move(*loadedBuffers);
    if (!validateAllBufferViews(input->document, buffers) ||
        !validateAllAccessors(input->document, buffers)) {
        return nullptr;
    }
    auto model = std::make_unique<GltfModel>();
    auto textures = loadTextures(
        input->document,
        buffers,
        externalResourceResolver,
        imageDecoder);
    if (!textures) {
        return nullptr;
    }
    model->textures = std::move(*textures);

    std::vector<NodeRecord> nodeRecords = parseNodes(input->document);
    applyMeshDefaultWeights(input->document, nodeRecords);
    const std::vector<SkinRecord> skins =
        parseSkins(input->document, buffers, nodeRecords.size());
    std::optional<std::vector<GltfAnimationRuntime>> animations =
        parseAnimations(input->document, buffers, nodeRecords);
    if (!animations) {
        return nullptr;
    }
    const auto& scenes = input->document.value("scenes", json::array());
    const auto& nodes = input->document.value("nodes", json::array());
    bool strictFailure = false;
    auto traverseRoot = [&](int rootNodeIndex) {
        traverseNode(
            input->document,
            buffers,
            model->textures,
            nodeRecords,
            skins,
            rootNodeIndex,
            *model,
            meshQuantizationEnabled,
            options.allowLegacyBatchIdAttribute,
            strictFailure);
    };

    if (!scenes.empty()) {
        const int requestedScene = input->document.value("scene", 0);
        const size_t sceneIndex =
            requestedScene >= 0 &&
                    static_cast<size_t>(requestedScene) < scenes.size()
                ? static_cast<size_t>(requestedScene)
                : 0u;
        const auto& sceneNodes =
            scenes[sceneIndex].value("nodes", json::array());
        for (const json& node : sceneNodes) {
            if (!node.is_number_integer()) {
                strictFailure = true;
                break;
            }
            traverseRoot(node.get<int>());
            if (strictFailure) break;
        }
    } else if (!nodes.empty()) {
        traverseRoot(0);
    }

    if (model->primitives.empty() || strictFailure) {
        return nullptr;
    }

    model->nodes = toRuntimeNodes(nodeRecords);
    model->sceneRootNodes = sceneRootNodes(input->document);
    model->skins = toRuntimeSkins(skins);
    model->animations = std::move(*animations);
    resetRuntimeNodes(*model);
    if (!rebuildRuntimeModel(*model)) {
        return nullptr;
    }
    return model;
}

} // namespace earth_engine
