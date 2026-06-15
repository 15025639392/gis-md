#include <gtest/gtest.h>

#include "earth_engine/content/GltfContentProvider.h"
#include "earth_engine/content/GltfExtensions.h"
#include "earth_engine/content/GltfModel.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>
#include <functional>

using namespace earth_engine;

namespace {

void appendU32(std::vector<uint8_t>& bytes, uint32_t value) {
    bytes.push_back(static_cast<uint8_t>(value & 0xffu));
    bytes.push_back(static_cast<uint8_t>((value >> 8) & 0xffu));
    bytes.push_back(static_cast<uint8_t>((value >> 16) & 0xffu));
    bytes.push_back(static_cast<uint8_t>((value >> 24) & 0xffu));
}

void appendU16(std::vector<uint8_t>& bytes, uint16_t value) {
    bytes.push_back(static_cast<uint8_t>(value & 0xffu));
    bytes.push_back(static_cast<uint8_t>((value >> 8) & 0xffu));
}

void appendI16(std::vector<uint8_t>& bytes, int16_t value) {
    appendU16(bytes, static_cast<uint16_t>(value));
}

void appendF32(std::vector<uint8_t>& bytes, float value) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&value);
    bytes.insert(bytes.end(), p, p + sizeof(float));
}

void pad4(std::vector<uint8_t>& bytes, uint8_t pad) {
    while ((bytes.size() % 4u) != 0u) {
        bytes.push_back(pad);
    }
}

std::string base64Encode(const std::vector<uint8_t>& bytes) {
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string encoded;
    encoded.reserve(((bytes.size() + 2u) / 3u) * 4u);
    for (size_t i = 0; i < bytes.size(); i += 3u) {
        const uint32_t b0 = bytes[i];
        const bool hasB1 = i + 1u < bytes.size();
        const bool hasB2 = i + 2u < bytes.size();
        const uint32_t b1 = hasB1 ? bytes[i + 1u] : 0u;
        const uint32_t b2 = hasB2 ? bytes[i + 2u] : 0u;
        const uint32_t packed = (b0 << 16u) | (b1 << 8u) | b2;
        encoded.push_back(kAlphabet[(packed >> 18u) & 0x3fu]);
        encoded.push_back(kAlphabet[(packed >> 12u) & 0x3fu]);
        encoded.push_back(hasB1 ? kAlphabet[(packed >> 6u) & 0x3fu] : '=');
        encoded.push_back(hasB2 ? kAlphabet[packed & 0x3fu] : '=');
    }
    return encoded;
}

std::vector<uint8_t> makeFakeWebpBytes() {
    return {
        'R', 'I', 'F', 'F',
        4, 0, 0, 0,
        'W', 'E', 'B', 'P',
        1, 2, 3, 4};
}

bool bytesLookLikeFakeWebp(const uint8_t* data, size_t size) {
    return size == 16u &&
           data[0] == 'R' &&
           data[1] == 'I' &&
           data[2] == 'F' &&
           data[3] == 'F' &&
           data[8] == 'W' &&
           data[9] == 'E' &&
           data[10] == 'B' &&
           data[11] == 'P' &&
           data[12] == 1u &&
           data[13] == 2u &&
           data[14] == 3u &&
           data[15] == 4u;
}

enum class NonFiniteTriangleAttribute {
    None,
    Position,
    Normal,
    TexCoord,
    Color
};

std::vector<uint8_t> makeTriangleGlb(
    std::array<float, 4> baseColor = {1.0f, 1.0f, 1.0f, 1.0f},
    bool doubleSided = false,
    int uvComponentType = 5126,
    bool uvNormalized = false,
    size_t uvByteStride = 0,
    bool indexNormalized = false,
    int colorComponentType = 0,
    bool colorNormalized = false,
    std::string colorType = std::string{},
    int tangentComponentType = 0,
    std::string tangentType = std::string{},
    bool zeroTangent = false,
    float tangentW = 1.0f,
    bool legacyBatchIds = false,
    bool declareMeshQuantization = false,
    NonFiniteTriangleAttribute nonFiniteAttribute =
        NonFiniteTriangleAttribute::None,
    const std::string& legacyBatchIdSemantic = "_BATCHID") {
    const bool hasColor = colorComponentType != 0;
    if (hasColor && colorType.empty()) {
        colorType = "VEC4";
    }
    const bool hasTangent = tangentComponentType != 0;
    if (hasTangent && tangentType.empty()) {
        tangentType = "VEC4";
    }

    std::vector<uint8_t> bin;
    const size_t positionsOffset = bin.size();
    appendF32(
        bin,
        nonFiniteAttribute == NonFiniteTriangleAttribute::Position
            ? std::numeric_limits<float>::quiet_NaN()
            : 0.0f);
    appendF32(bin, 0.0f); appendF32(bin, 0.0f);
    appendF32(bin, 1.0f); appendF32(bin, 0.0f); appendF32(bin, 0.0f);
    appendF32(bin, 0.0f); appendF32(bin, 1.0f); appendF32(bin, 0.0f);

    const size_t normalsOffset = bin.size();
    for (int i = 0; i < 3; ++i) {
        appendF32(
            bin,
            i == 0 &&
                    nonFiniteAttribute == NonFiniteTriangleAttribute::Normal
                ? std::numeric_limits<float>::infinity()
                : 0.0f);
        appendF32(bin, 0.0f); appendF32(bin, 1.0f);
    }

    const size_t uvOffset = bin.size();
    if (uvComponentType == 5121) {
        bin.push_back(0); bin.push_back(0);
        bin.push_back(255); bin.push_back(0);
        bin.push_back(0); bin.push_back(255);
    } else if (uvComponentType == 5123) {
        appendU16(bin, 0); appendU16(bin, 0);
        appendU16(bin, 65535); appendU16(bin, 0);
        appendU16(bin, 0); appendU16(bin, 65535);
    } else {
        appendF32(
            bin,
            nonFiniteAttribute == NonFiniteTriangleAttribute::TexCoord
                ? std::numeric_limits<float>::quiet_NaN()
                : 0.0f);
        appendF32(bin, 0.0f);
        appendF32(bin, 1.0f); appendF32(bin, 0.0f);
        appendF32(bin, 0.0f); appendF32(bin, 1.0f);
    }
    const size_t uvByteLength = bin.size() - uvOffset;

    size_t colorOffset = 0;
    size_t colorByteLength = 0;
    if (hasColor) {
        pad4(bin, 0);
        colorOffset = bin.size();
        const int colorComponents =
            colorType == "VEC2" ? 2 : (colorType == "VEC3" ? 3 : 4);
        const uint8_t u8Colors[3][4] = {
            {64, 128, 191, 255},
            {255, 0, 128, 128},
            {0, 255, 64, 64}};
        const uint16_t u16Colors[3][4] = {
            {16384, 32768, 49152, 65535},
            {65535, 0, 32768, 32768},
            {0, 65535, 16384, 16384}};
        const float f32Colors[3][4] = {
            {0.25f, 0.5f, 0.75f, 1.0f},
            {1.0f, 0.0f, 0.5f, 0.5f},
            {0.0f, 1.0f, 0.25f, 0.25f}};
        for (int vertex = 0; vertex < 3; ++vertex) {
            for (int component = 0; component < colorComponents; ++component) {
                if (colorComponentType == 5121) {
                    bin.push_back(u8Colors[vertex][component]);
                } else if (colorComponentType == 5123) {
                    appendU16(bin, u16Colors[vertex][component]);
                } else {
                    appendF32(
                        bin,
                        vertex == 0 &&
                                component == 0 &&
                                nonFiniteAttribute ==
                                    NonFiniteTriangleAttribute::Color
                            ? std::numeric_limits<float>::infinity()
                            : f32Colors[vertex][component]);
                }
            }
        }
        colorByteLength = bin.size() - colorOffset;
    }

    size_t tangentOffset = 0;
    size_t tangentByteLength = 0;
    if (hasTangent) {
        pad4(bin, 0);
        tangentOffset = bin.size();
        const int tangentComponents =
            tangentType == "VEC2" ? 2 : (tangentType == "VEC3" ? 3 : 4);
        const float tangentValues[3][4] = {
            {zeroTangent ? 0.0f : 1.0f, 0.0f, 0.0f, tangentW},
            {zeroTangent ? 0.0f : 1.0f, 0.0f, 0.0f, tangentW},
            {zeroTangent ? 0.0f : 1.0f, 0.0f, 0.0f, tangentW}};
        for (int vertex = 0; vertex < 3; ++vertex) {
            for (int component = 0; component < tangentComponents; ++component) {
                if (tangentComponentType == 5121) {
                    bin.push_back(
                        tangentValues[vertex][component] < 0.0f ? 0 : 255);
                } else if (tangentComponentType == 5123) {
                    appendU16(
                        bin,
                        tangentValues[vertex][component] < 0.0f ? 0 : 65535);
                } else {
                    appendF32(bin, tangentValues[vertex][component]);
                }
            }
        }
        tangentByteLength = bin.size() - tangentOffset;
    }

    size_t batchIdOffset = 0;
    size_t batchIdByteLength = 0;
    if (legacyBatchIds) {
        pad4(bin, 0);
        batchIdOffset = bin.size();
        appendU16(bin, 1);
        appendU16(bin, 0);
        appendU16(bin, 1);
        batchIdByteLength = bin.size() - batchIdOffset;
    }

    if ((bin.size() % 2u) != 0u) {
        bin.push_back(0);
    }
    const size_t indicesOffset = bin.size();
    appendU16(bin, 0); appendU16(bin, 1); appendU16(bin, 2);
    pad4(bin, 0);

    int nextAccessor = 3;
    std::string attributes =
        "\"POSITION\":0,\"NORMAL\":1,\"TEXCOORD_0\":2";
    std::string bufferViews =
        "{\"buffer\":0,\"byteOffset\":" + std::to_string(positionsOffset) + ",\"byteLength\":36}," +
        "{\"buffer\":0,\"byteOffset\":" + std::to_string(normalsOffset) + ",\"byteLength\":36}," +
        "{\"buffer\":0,\"byteOffset\":" + std::to_string(uvOffset) + ",\"byteLength\":" +
        std::to_string(uvByteLength) +
        (uvByteStride > 0 ? ",\"byteStride\":" + std::to_string(uvByteStride) : "") +
        "}";
    std::string accessors =
        "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"}," +
        std::string("{\"bufferView\":1,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},") +
        "{\"bufferView\":2,\"componentType\":" +
        std::to_string(uvComponentType) +
        ",\"count\":3,\"type\":\"VEC2\"" +
        (uvNormalized ? ",\"normalized\":true" : "") +
        "}";
    if (hasColor) {
        const int colorAccessor = nextAccessor++;
        attributes +=
            ",\"COLOR_0\":" + std::to_string(colorAccessor);
        bufferViews +=
            ",{\"buffer\":0,\"byteOffset\":" +
            std::to_string(colorOffset) +
            ",\"byteLength\":" +
            std::to_string(colorByteLength) +
            "}";
        accessors +=
            ",{\"bufferView\":" +
            std::to_string(colorAccessor) +
            ",\"componentType\":" +
            std::to_string(colorComponentType) +
            ",\"count\":3,\"type\":\"" +
            colorType +
            "\"" +
            (colorNormalized ? ",\"normalized\":true" : "") +
            "}";
    }
    if (hasTangent) {
        const int tangentAccessor = nextAccessor++;
        attributes +=
            ",\"TANGENT\":" + std::to_string(tangentAccessor);
        bufferViews +=
            ",{\"buffer\":0,\"byteOffset\":" +
            std::to_string(tangentOffset) +
            ",\"byteLength\":" +
            std::to_string(tangentByteLength) +
            "}";
        accessors +=
            ",{\"bufferView\":" +
            std::to_string(tangentAccessor) +
            ",\"componentType\":" +
            std::to_string(tangentComponentType) +
            ",\"count\":3,\"type\":\"" +
            tangentType +
            "\"}";
    }
    if (legacyBatchIds) {
        const int batchIdAccessor = nextAccessor++;
        attributes +=
            ",\"" + legacyBatchIdSemantic + "\":" +
            std::to_string(batchIdAccessor);
        bufferViews +=
            ",{\"buffer\":0,\"byteOffset\":" +
            std::to_string(batchIdOffset) +
            ",\"byteLength\":" +
            std::to_string(batchIdByteLength) +
            "}";
        accessors +=
            ",{\"bufferView\":" +
            std::to_string(batchIdAccessor) +
            ",\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}";
    }
    const int indexAccessor = nextAccessor;
    bufferViews +=
        ",{\"buffer\":0,\"byteOffset\":" +
        std::to_string(indicesOffset) +
        ",\"byteLength\":6}";
    accessors +=
        ",{\"bufferView\":" +
        std::to_string(indexAccessor) +
        ",\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"" +
        (indexNormalized ? ",\"normalized\":true" : "") +
        "}";

    const std::string extensionDeclarations = declareMeshQuantization
        ? "\"extensionsUsed\":[\"KHR_mesh_quantization\"],"
          "\"extensionsRequired\":[\"KHR_mesh_quantization\"],"
        : "";
    const std::string jsonText =
        std::string("{") +
        "\"asset\":{\"version\":\"2.0\"}," +
        extensionDeclarations +
        "\"scene\":0," +
        "\"scenes\":[{\"nodes\":[0]}]," +
        "\"nodes\":[{\"mesh\":0,\"translation\":[10,20,30]}]," +
        "\"meshes\":[{\"primitives\":[{\"attributes\":{" +
        attributes +
        "},\"indices\":" +
        std::to_string(indexAccessor) +
        ",\"mode\":4,\"material\":0}]}]," +
        "\"materials\":[{\"doubleSided\":" +
        std::string(doubleSided ? "true" : "false") +
        ",\"pbrMetallicRoughness\":{\"baseColorFactor\":[" +
        std::to_string(baseColor[0]) + "," +
        std::to_string(baseColor[1]) + "," +
        std::to_string(baseColor[2]) + "," +
        std::to_string(baseColor[3]) + "]}}]," +
        "\"buffers\":[{\"byteLength\":" + std::to_string(bin.size()) + "}]," +
        "\"bufferViews\":[" +
        bufferViews +
        "]," +
        "\"accessors\":[" +
        accessors +
        "]}";

    std::vector<uint8_t> jsonBytes(jsonText.begin(), jsonText.end());
    pad4(jsonBytes, 0x20);

    std::vector<uint8_t> glb;
    appendU32(glb, 0x46546C67u);
    appendU32(glb, 2u);
    appendU32(glb, static_cast<uint32_t>(12 + 8 + jsonBytes.size() + 8 + bin.size()));
    appendU32(glb, static_cast<uint32_t>(jsonBytes.size()));
    appendU32(glb, 0x4E4F534Au);
    glb.insert(glb.end(), jsonBytes.begin(), jsonBytes.end());
    appendU32(glb, static_cast<uint32_t>(bin.size()));
    appendU32(glb, 0x004E4942u);
    glb.insert(glb.end(), bin.begin(), bin.end());
    return glb;
}

uint32_t readTestU32LE(const std::vector<uint8_t>& bytes, size_t offset) {
    return uint32_t(bytes[offset]) |
           (uint32_t(bytes[offset + 1]) << 8) |
           (uint32_t(bytes[offset + 2]) << 16) |
           (uint32_t(bytes[offset + 3]) << 24);
}

std::vector<uint8_t> makeGlbFromChunks(
    const std::vector<std::pair<uint32_t, std::vector<uint8_t>>>& chunks) {
    size_t byteLength = 12u;
    for (const auto& chunk : chunks) {
        byteLength += 8u + chunk.second.size();
    }

    std::vector<uint8_t> glb;
    appendU32(glb, 0x46546C67u);
    appendU32(glb, 2u);
    appendU32(glb, static_cast<uint32_t>(byteLength));
    for (const auto& chunk : chunks) {
        appendU32(glb, static_cast<uint32_t>(chunk.second.size()));
        appendU32(glb, chunk.first);
        glb.insert(glb.end(), chunk.second.begin(), chunk.second.end());
    }
    return glb;
}

std::pair<std::vector<uint8_t>, std::vector<uint8_t>>
triangleGlbJsonAndBinChunks() {
    const std::vector<uint8_t> glb = makeTriangleGlb();
    const uint32_t jsonLength = readTestU32LE(glb, 12u);
    const size_t jsonOffset = 20u;
    const size_t binHeaderOffset = jsonOffset + jsonLength;
    const uint32_t binLength = readTestU32LE(glb, binHeaderOffset);
    const size_t binOffset = binHeaderOffset + 8u;

    return {
        std::vector<uint8_t>(
            glb.begin() + static_cast<std::ptrdiff_t>(jsonOffset),
            glb.begin() + static_cast<std::ptrdiff_t>(
                jsonOffset + jsonLength)),
        std::vector<uint8_t>(
            glb.begin() + static_cast<std::ptrdiff_t>(binOffset),
            glb.begin() + static_cast<std::ptrdiff_t>(
                binOffset + binLength))};
}

std::vector<uint8_t> makeTriangleGlbWithNonFiniteAttribute(
    NonFiniteTriangleAttribute attribute) {
    return makeTriangleGlb(
        {1.0f, 1.0f, 1.0f, 1.0f},
        false,
        5126,
        false,
        0,
        false,
        attribute == NonFiniteTriangleAttribute::Color ? 5126 : 0,
        false,
        std::string{},
        0,
        std::string{},
        false,
        1.0f,
        false,
        false,
        attribute);
}

std::vector<uint8_t> makeLegacyBatchIdTriangleGlb() {
    return makeTriangleGlb(
        {1.0f, 1.0f, 1.0f, 1.0f},
        false,
        5126,
        false,
        0,
        false,
        0,
        false,
        std::string{},
        0,
        std::string{},
        false,
        1.0f,
        true);
}

std::vector<uint8_t> makeLegacyBatchIdTriangleGlbWithSemantic(
    const std::string& semantic) {
    return makeTriangleGlb(
        {1.0f, 1.0f, 1.0f, 1.0f},
        false,
        5126,
        false,
        0,
        false,
        0,
        false,
        std::string{},
        0,
        std::string{},
        false,
        1.0f,
        true,
        false,
        NonFiniteTriangleAttribute::None,
        semantic);
}

std::vector<uint8_t> makeQuadPrimitiveModeGlb(int mode, bool indexed = true) {
    std::vector<uint8_t> bin;
    const size_t positionsOffset = bin.size();
    appendF32(bin, 0.0f); appendF32(bin, 0.0f); appendF32(bin, 0.0f);
    appendF32(bin, 1.0f); appendF32(bin, 0.0f); appendF32(bin, 0.0f);
    appendF32(bin, 0.0f); appendF32(bin, 1.0f); appendF32(bin, 0.0f);
    appendF32(bin, 1.0f); appendF32(bin, 1.0f); appendF32(bin, 0.0f);

    const size_t normalsOffset = bin.size();
    for (int i = 0; i < 4; ++i) {
        appendF32(bin, 0.0f); appendF32(bin, 0.0f); appendF32(bin, 1.0f);
    }

    size_t indicesOffset = 0;
    if (indexed) {
        indicesOffset = bin.size();
        appendU16(bin, 0); appendU16(bin, 1);
        appendU16(bin, 2); appendU16(bin, 3);
    }
    pad4(bin, 0);

    std::string bufferViews =
        "{\"buffer\":0,\"byteOffset\":" + std::to_string(positionsOffset) + ",\"byteLength\":48}," +
        "{\"buffer\":0,\"byteOffset\":" + std::to_string(normalsOffset) + ",\"byteLength\":48}";
    std::string accessors =
        "{\"bufferView\":0,\"componentType\":5126,\"count\":4,\"type\":\"VEC3\"}," +
        std::string("{\"bufferView\":1,\"componentType\":5126,\"count\":4,\"type\":\"VEC3\"}");
    std::string indicesProperty;
    if (indexed) {
        bufferViews +=
            ",{\"buffer\":0,\"byteOffset\":" +
            std::to_string(indicesOffset) +
            ",\"byteLength\":8}";
        accessors +=
            ",{\"bufferView\":2,\"componentType\":5123,\"count\":4,\"type\":\"SCALAR\"}";
        indicesProperty = "\"indices\":2,";
    }

    const std::string jsonText =
        std::string("{") +
        "\"asset\":{\"version\":\"2.0\"}," +
        "\"scene\":0," +
        "\"scenes\":[{\"nodes\":[0]}]," +
        "\"nodes\":[{\"mesh\":0}]," +
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"NORMAL\":1}," +
        indicesProperty +
        "\"mode\":" + std::to_string(mode) + "}]}]," +
        "\"buffers\":[{\"byteLength\":" + std::to_string(bin.size()) + "}]," +
        "\"bufferViews\":[" + bufferViews + "]," +
        "\"accessors\":[" + accessors + "]}";

    std::vector<uint8_t> jsonBytes(jsonText.begin(), jsonText.end());
    pad4(jsonBytes, 0x20);

    std::vector<uint8_t> glb;
    appendU32(glb, 0x46546C67u);
    appendU32(glb, 2u);
    appendU32(glb, static_cast<uint32_t>(12 + 8 + jsonBytes.size() + 8 + bin.size()));
    appendU32(glb, static_cast<uint32_t>(jsonBytes.size()));
    appendU32(glb, 0x4E4F534Au);
    glb.insert(glb.end(), jsonBytes.begin(), jsonBytes.end());
    appendU32(glb, static_cast<uint32_t>(bin.size()));
    appendU32(glb, 0x004E4942u);
    glb.insert(glb.end(), bin.begin(), bin.end());
    return glb;
}

std::vector<uint8_t> makeSparsePositionTriangleGlb(
    uint8_t lastSparseIndex = 2,
    uint16_t lastPrimitiveIndex = 2,
    size_t sparseValuesByteOffset = 0) {
    std::vector<uint8_t> bin;
    const size_t sparseIndicesOffset = bin.size();
    bin.push_back(0);
    bin.push_back(1);
    bin.push_back(lastSparseIndex);
    pad4(bin, 0);

    const size_t sparseValuesOffset = bin.size();
    bin.insert(bin.end(), sparseValuesByteOffset, 0);
    appendF32(bin, 0.0f); appendF32(bin, 0.0f); appendF32(bin, 0.0f);
    appendF32(bin, 1.0f); appendF32(bin, 0.0f); appendF32(bin, 0.0f);
    appendF32(bin, 0.0f); appendF32(bin, 1.0f); appendF32(bin, 0.0f);
    const size_t sparseValuesByteLength = sparseValuesByteOffset + 36u;

    const size_t normalsOffset = bin.size();
    for (int i = 0; i < 3; ++i) {
        appendF32(bin, 0.0f); appendF32(bin, 0.0f); appendF32(bin, 1.0f);
    }

    const size_t indicesOffset = bin.size();
    appendU16(bin, 0); appendU16(bin, 1); appendU16(bin, lastPrimitiveIndex);
    pad4(bin, 0);

    const std::string jsonText =
        std::string("{") +
        "\"asset\":{\"version\":\"2.0\"}," +
        "\"scene\":0," +
        "\"scenes\":[{\"nodes\":[0]}]," +
        "\"nodes\":[{\"mesh\":0,\"translation\":[10,20,30]}]," +
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"NORMAL\":1},\"indices\":2,\"mode\":4}]}]," +
        "\"buffers\":[{\"byteLength\":" + std::to_string(bin.size()) + "}]," +
        "\"bufferViews\":[" +
        "{\"buffer\":0,\"byteOffset\":" + std::to_string(sparseIndicesOffset) + ",\"byteLength\":3}," +
        "{\"buffer\":0,\"byteOffset\":" + std::to_string(sparseValuesOffset) + ",\"byteLength\":" +
        std::to_string(sparseValuesByteLength) + "}," +
        "{\"buffer\":0,\"byteOffset\":" + std::to_string(normalsOffset) + ",\"byteLength\":36}," +
        "{\"buffer\":0,\"byteOffset\":" + std::to_string(indicesOffset) + ",\"byteLength\":6}" +
        "]," +
        "\"accessors\":[" +
        "{\"componentType\":5126,\"count\":3,\"type\":\"VEC3\",\"sparse\":{\"count\":3,\"indices\":{\"bufferView\":0,\"componentType\":5121},\"values\":{\"bufferView\":1,\"byteOffset\":" +
        std::to_string(sparseValuesByteOffset) + "}}}," +
        "{\"bufferView\":2,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"}," +
        "{\"bufferView\":3,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}" +
        "]}";

    std::vector<uint8_t> jsonBytes(jsonText.begin(), jsonText.end());
    pad4(jsonBytes, 0x20);

    std::vector<uint8_t> glb;
    appendU32(glb, 0x46546C67u);
    appendU32(glb, 2u);
    appendU32(glb, static_cast<uint32_t>(12 + 8 + jsonBytes.size() + 8 + bin.size()));
    appendU32(glb, static_cast<uint32_t>(jsonBytes.size()));
    appendU32(glb, 0x4E4F534Au);
    glb.insert(glb.end(), jsonBytes.begin(), jsonBytes.end());
    appendU32(glb, static_cast<uint32_t>(bin.size()));
    appendU32(glb, 0x004E4942u);
    glb.insert(glb.end(), bin.begin(), bin.end());
    return glb;
}

std::vector<uint8_t> makeSkinnedTriangleGlb(
    bool omitWeightsAttribute = false,
    bool invalidJointIndex = false,
    bool nonFiniteWeight = false,
    bool nonFiniteInverseBind = false,
    bool extraInverseBindMatrix = false) {
    std::vector<uint8_t> bin;
    const size_t positionsOffset = bin.size();
    appendF32(bin, 1.0f); appendF32(bin, 0.0f); appendF32(bin, 0.0f);
    appendF32(bin, 0.0f); appendF32(bin, 1.0f); appendF32(bin, 0.0f);
    appendF32(bin, 0.0f); appendF32(bin, 0.0f); appendF32(bin, 1.0f);

    const size_t normalsOffset = bin.size();
    for (int i = 0; i < 3; ++i) {
        appendF32(bin, 0.0f); appendF32(bin, 0.0f); appendF32(bin, 1.0f);
    }

    const size_t jointsOffset = bin.size();
    for (int i = 0; i < 3; ++i) {
        appendU16(bin, 0); appendU16(bin, 0);
        appendU16(bin, 0); appendU16(bin, 0);
    }

    const size_t weightsOffset = bin.size();
    for (int i = 0; i < 3; ++i) {
        appendF32(
            bin,
            i == 0 && nonFiniteWeight
                ? std::numeric_limits<float>::quiet_NaN()
                : 1.0f);
        appendF32(bin, 0.0f);
        appendF32(bin, 0.0f); appendF32(bin, 0.0f);
    }

    const size_t inverseBindOffset = bin.size();
    const int inverseBindCount = extraInverseBindMatrix ? 2 : 1;
    for (int matrix = 0; matrix < inverseBindCount; ++matrix) {
        for (int column = 0; column < 4; ++column) {
            for (int row = 0; row < 4; ++row) {
                appendF32(
                    bin,
                    matrix == 0 &&
                            column == 0 &&
                            row == 0 &&
                            nonFiniteInverseBind
                        ? std::numeric_limits<float>::infinity()
                        : (column == row ? 1.0f : 0.0f));
            }
        }
    }

    const size_t indicesOffset = bin.size();
    appendU16(bin, 0); appendU16(bin, 1); appendU16(bin, 2);
    pad4(bin, 0);

    const std::string attributes =
        std::string("{\"POSITION\":0,\"NORMAL\":1,\"JOINTS_0\":2") +
        (omitWeightsAttribute ? "" : ",\"WEIGHTS_0\":3") +
        "}";
    const std::string jsonText =
        std::string("{") +
        "\"asset\":{\"version\":\"2.0\"}," +
        "\"scene\":0," +
        "\"scenes\":[{\"nodes\":[0]}]," +
        "\"nodes\":[" +
        "{\"mesh\":0,\"skin\":0,\"children\":[1]}," +
        "{\"translation\":[10,20,30]}" +
        "]," +
        "\"skins\":[{\"joints\":[" +
        std::string(invalidJointIndex ? "9" : "1") +
        "],\"inverseBindMatrices\":4}]," +
        "\"meshes\":[{\"primitives\":[{\"attributes\":" + attributes +
        ",\"indices\":5,\"mode\":4}]}]," +
        "\"buffers\":[{\"byteLength\":" + std::to_string(bin.size()) + "}]," +
        "\"bufferViews\":[" +
        "{\"buffer\":0,\"byteOffset\":" + std::to_string(positionsOffset) + ",\"byteLength\":36}," +
        "{\"buffer\":0,\"byteOffset\":" + std::to_string(normalsOffset) + ",\"byteLength\":36}," +
        "{\"buffer\":0,\"byteOffset\":" + std::to_string(jointsOffset) + ",\"byteLength\":24}," +
        "{\"buffer\":0,\"byteOffset\":" + std::to_string(weightsOffset) + ",\"byteLength\":48}," +
        "{\"buffer\":0,\"byteOffset\":" + std::to_string(inverseBindOffset) + ",\"byteLength\":" + std::to_string(64 * inverseBindCount) + "}," +
        "{\"buffer\":0,\"byteOffset\":" + std::to_string(indicesOffset) + ",\"byteLength\":6}" +
        "]," +
        "\"accessors\":[" +
        "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"}," +
        "{\"bufferView\":1,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"}," +
        "{\"bufferView\":2,\"componentType\":5123,\"count\":3,\"type\":\"VEC4\"}," +
        "{\"bufferView\":3,\"componentType\":5126,\"count\":3,\"type\":\"VEC4\"}," +
        "{\"bufferView\":4,\"componentType\":5126,\"count\":" + std::to_string(inverseBindCount) + ",\"type\":\"MAT4\"}," +
        "{\"bufferView\":5,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}" +
        "]}";

    std::vector<uint8_t> jsonBytes(jsonText.begin(), jsonText.end());
    pad4(jsonBytes, 0x20);

    std::vector<uint8_t> glb;
    appendU32(glb, 0x46546C67u);
    appendU32(glb, 2u);
    appendU32(glb, static_cast<uint32_t>(12 + 8 + jsonBytes.size() + 8 + bin.size()));
    appendU32(glb, static_cast<uint32_t>(jsonBytes.size()));
    appendU32(glb, 0x4E4F534Au);
    glb.insert(glb.end(), jsonBytes.begin(), jsonBytes.end());
    appendU32(glb, static_cast<uint32_t>(bin.size()));
    appendU32(glb, 0x004E4942u);
    glb.insert(glb.end(), bin.begin(), bin.end());
    return glb;
}

std::vector<uint8_t> makeAnimatedTranslationTriangleGlb(
    const std::string& interpolation = "LINEAR",
    bool malformedCubicOutput = false,
    bool duplicateAnimationTarget = false,
    bool nonFiniteOutput = false,
    bool rotationAnimation = false,
    bool zeroRotationOutput = false) {
    std::vector<uint8_t> bin;
    const size_t positionsOffset = bin.size();
    appendF32(bin, 0.0f); appendF32(bin, 0.0f); appendF32(bin, 0.0f);
    appendF32(bin, 1.0f); appendF32(bin, 0.0f); appendF32(bin, 0.0f);
    appendF32(bin, 0.0f); appendF32(bin, 1.0f); appendF32(bin, 0.0f);

    const size_t normalsOffset = bin.size();
    for (int i = 0; i < 3; ++i) {
        appendF32(bin, 0.0f); appendF32(bin, 0.0f); appendF32(bin, 1.0f);
    }

    const size_t indicesOffset = bin.size();
    appendU16(bin, 0); appendU16(bin, 1); appendU16(bin, 2);
    pad4(bin, 0);

    const size_t inputOffset = bin.size();
    appendF32(bin, 0.0f); appendF32(bin, 1.0f);

    const size_t outputOffset = bin.size();
    int outputAccessorCount = 2;
    if (rotationAnimation) {
        constexpr float kSqrtHalf = 0.7071067811865476f;
        auto appendRotationValue = [&](bool secondKeyframe) {
            if (zeroRotationOutput) {
                appendF32(bin, 0.0f);
                appendF32(bin, 0.0f);
                appendF32(bin, 0.0f);
                appendF32(bin, 0.0f);
                return;
            }
            appendF32(
                bin,
                nonFiniteOutput
                    ? std::numeric_limits<float>::infinity()
                    : 0.0f);
            appendF32(bin, 0.0f);
            appendF32(bin, secondKeyframe ? kSqrtHalf : 0.0f);
            appendF32(bin, secondKeyframe ? kSqrtHalf : 1.0f);
        };
        auto appendRotationTangent = [&]() {
            appendF32(bin, 0.0f);
            appendF32(bin, 0.0f);
            appendF32(bin, 0.0f);
            appendF32(bin, 0.0f);
        };
        if (interpolation == "CUBICSPLINE" && !malformedCubicOutput) {
            outputAccessorCount = 6;
            appendRotationTangent();
            appendRotationValue(false);
            appendRotationTangent();
            appendRotationTangent();
            appendRotationValue(true);
            appendRotationTangent();
        } else {
            appendRotationValue(false);
            appendRotationValue(true);
        }
    } else if (interpolation == "CUBICSPLINE" && !malformedCubicOutput) {
        outputAccessorCount = 6;
        appendF32(
            bin,
            nonFiniteOutput
                ? std::numeric_limits<float>::infinity()
                : 0.0f);
        appendF32(bin, 0.0f); appendF32(bin, 0.0f);
        appendF32(bin, 0.0f); appendF32(bin, 0.0f); appendF32(bin, 0.0f);
        appendF32(bin, 0.0f); appendF32(bin, 0.0f); appendF32(bin, 0.0f);
        appendF32(bin, 0.0f); appendF32(bin, 0.0f); appendF32(bin, 0.0f);
        appendF32(bin, 10.0f); appendF32(bin, 0.0f); appendF32(bin, 0.0f);
        appendF32(bin, 0.0f); appendF32(bin, 0.0f); appendF32(bin, 0.0f);
    } else {
        appendF32(
            bin,
            nonFiniteOutput
                ? std::numeric_limits<float>::infinity()
                : 0.0f);
        appendF32(bin, 0.0f); appendF32(bin, 0.0f);
        appendF32(bin, 10.0f); appendF32(bin, 0.0f); appendF32(bin, 0.0f);
    }
    const size_t outputByteLength = bin.size() - outputOffset;

    const std::string jsonText =
        std::string("{") +
        "\"asset\":{\"version\":\"2.0\"}," +
        "\"scene\":0," +
        "\"scenes\":[{\"nodes\":[0]}]," +
        "\"nodes\":[{\"mesh\":0}]," +
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"NORMAL\":1},\"indices\":2,\"mode\":4}]}]," +
        "\"animations\":[{\"samplers\":[{\"input\":3,\"output\":4,\"interpolation\":\"" +
        interpolation +
        "\"}],\"channels\":[{\"sampler\":0,\"target\":{\"node\":0,\"path\":\"" +
        (rotationAnimation ? "rotation" : "translation") + "\"}}" +
        (duplicateAnimationTarget
             ? ",{\"sampler\":0,\"target\":{\"node\":0,\"path\":\"translation\"}}"
             : "") +
        "]}]," +
        "\"buffers\":[{\"byteLength\":" + std::to_string(bin.size()) + "}]," +
        "\"bufferViews\":[" +
        "{\"buffer\":0,\"byteOffset\":" + std::to_string(positionsOffset) + ",\"byteLength\":36}," +
        "{\"buffer\":0,\"byteOffset\":" + std::to_string(normalsOffset) + ",\"byteLength\":36}," +
        "{\"buffer\":0,\"byteOffset\":" + std::to_string(indicesOffset) + ",\"byteLength\":6}," +
        "{\"buffer\":0,\"byteOffset\":" + std::to_string(inputOffset) + ",\"byteLength\":8}," +
        "{\"buffer\":0,\"byteOffset\":" + std::to_string(outputOffset) + ",\"byteLength\":" +
        std::to_string(outputByteLength) + "}" +
        "]," +
        "\"accessors\":[" +
        "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"}," +
        "{\"bufferView\":1,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"}," +
        "{\"bufferView\":2,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}," +
        "{\"bufferView\":3,\"componentType\":5126,\"count\":2,\"type\":\"SCALAR\"}," +
        "{\"bufferView\":4,\"componentType\":5126,\"count\":" +
        std::to_string(outputAccessorCount) + ",\"type\":\"" +
        (rotationAnimation ? "VEC4" : "VEC3") + "\"}" +
        "]}";

    std::vector<uint8_t> jsonBytes(jsonText.begin(), jsonText.end());
    pad4(jsonBytes, 0x20);
    std::vector<uint8_t> glb;
    appendU32(glb, 0x46546C67u);
    appendU32(glb, 2u);
    appendU32(glb, static_cast<uint32_t>(12 + 8 + jsonBytes.size() + 8 + bin.size()));
    appendU32(glb, static_cast<uint32_t>(jsonBytes.size()));
    appendU32(glb, 0x4E4F534Au);
    glb.insert(glb.end(), jsonBytes.begin(), jsonBytes.end());
    appendU32(glb, static_cast<uint32_t>(bin.size()));
    appendU32(glb, 0x004E4942u);
    glb.insert(glb.end(), bin.begin(), bin.end());
    return glb;
}

std::vector<uint8_t> makeDualAnimationTranslationTriangleGlb() {
    std::vector<uint8_t> bin;
    const size_t positionsOffset = bin.size();
    appendF32(bin, 0.0f); appendF32(bin, 0.0f); appendF32(bin, 0.0f);
    appendF32(bin, 1.0f); appendF32(bin, 0.0f); appendF32(bin, 0.0f);
    appendF32(bin, 0.0f); appendF32(bin, 1.0f); appendF32(bin, 0.0f);

    const size_t normalsOffset = bin.size();
    for (int i = 0; i < 3; ++i) {
        appendF32(bin, 0.0f); appendF32(bin, 0.0f); appendF32(bin, 1.0f);
    }

    const size_t indicesOffset = bin.size();
    appendU16(bin, 0); appendU16(bin, 1); appendU16(bin, 2);
    pad4(bin, 0);

    const size_t inputOffset = bin.size();
    appendF32(bin, 0.0f); appendF32(bin, 1.0f);

    const size_t outputXOffset = bin.size();
    appendF32(bin, 0.0f); appendF32(bin, 0.0f); appendF32(bin, 0.0f);
    appendF32(bin, 10.0f); appendF32(bin, 0.0f); appendF32(bin, 0.0f);

    const size_t outputYOffset = bin.size();
    appendF32(bin, 0.0f); appendF32(bin, 0.0f); appendF32(bin, 0.0f);
    appendF32(bin, 0.0f); appendF32(bin, 20.0f); appendF32(bin, 0.0f);

    const std::string jsonText =
        std::string("{") +
        "\"asset\":{\"version\":\"2.0\"}," +
        "\"scene\":0," +
        "\"scenes\":[{\"nodes\":[0]}]," +
        "\"nodes\":[{\"mesh\":0}]," +
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"NORMAL\":1},\"indices\":2,\"mode\":4}]}]," +
        "\"animations\":[" +
        "{\"samplers\":[{\"input\":3,\"output\":4}],\"channels\":[{\"sampler\":0,\"target\":{\"node\":0,\"path\":\"translation\"}}]}," +
        "{\"samplers\":[{\"input\":3,\"output\":5}],\"channels\":[{\"sampler\":0,\"target\":{\"node\":0,\"path\":\"translation\"}}]}" +
        "]," +
        "\"buffers\":[{\"byteLength\":" + std::to_string(bin.size()) + "}]," +
        "\"bufferViews\":[" +
        "{\"buffer\":0,\"byteOffset\":" + std::to_string(positionsOffset) + ",\"byteLength\":36}," +
        "{\"buffer\":0,\"byteOffset\":" + std::to_string(normalsOffset) + ",\"byteLength\":36}," +
        "{\"buffer\":0,\"byteOffset\":" + std::to_string(indicesOffset) + ",\"byteLength\":6}," +
        "{\"buffer\":0,\"byteOffset\":" + std::to_string(inputOffset) + ",\"byteLength\":8}," +
        "{\"buffer\":0,\"byteOffset\":" + std::to_string(outputXOffset) + ",\"byteLength\":24}," +
        "{\"buffer\":0,\"byteOffset\":" + std::to_string(outputYOffset) + ",\"byteLength\":24}" +
        "]," +
        "\"accessors\":[" +
        "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"}," +
        "{\"bufferView\":1,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"}," +
        "{\"bufferView\":2,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}," +
        "{\"bufferView\":3,\"componentType\":5126,\"count\":2,\"type\":\"SCALAR\"}," +
        "{\"bufferView\":4,\"componentType\":5126,\"count\":2,\"type\":\"VEC3\"}," +
        "{\"bufferView\":5,\"componentType\":5126,\"count\":2,\"type\":\"VEC3\"}" +
        "]}";

    std::vector<uint8_t> jsonBytes(jsonText.begin(), jsonText.end());
    pad4(jsonBytes, 0x20);
    std::vector<uint8_t> glb;
    appendU32(glb, 0x46546C67u);
    appendU32(glb, 2u);
    appendU32(glb, static_cast<uint32_t>(
        12 + 8 + jsonBytes.size() + 8 + bin.size()));
    appendU32(glb, static_cast<uint32_t>(jsonBytes.size()));
    appendU32(glb, 0x4E4F534Au);
    glb.insert(glb.end(), jsonBytes.begin(), jsonBytes.end());
    appendU32(glb, static_cast<uint32_t>(bin.size()));
    appendU32(glb, 0x004E4942u);
    glb.insert(glb.end(), bin.begin(), bin.end());
    return glb;
}

std::vector<uint8_t> makeAnimatedMorphWeightTriangleGlb(
    bool nonFiniteMorphDelta = false) {
    std::vector<uint8_t> bin;
    const size_t positionsOffset = bin.size();
    appendF32(bin, 0.0f); appendF32(bin, 0.0f); appendF32(bin, 0.0f);
    appendF32(bin, 1.0f); appendF32(bin, 0.0f); appendF32(bin, 0.0f);
    appendF32(bin, 0.0f); appendF32(bin, 1.0f); appendF32(bin, 0.0f);

    const size_t normalsOffset = bin.size();
    for (int i = 0; i < 3; ++i) {
        appendF32(bin, 0.0f); appendF32(bin, 0.0f); appendF32(bin, 1.0f);
    }

    const size_t morphOffset = bin.size();
    appendF32(
        bin,
        nonFiniteMorphDelta
            ? std::numeric_limits<float>::quiet_NaN()
            : 10.0f);
    appendF32(bin, 0.0f); appendF32(bin, 0.0f);
    appendF32(bin, 0.0f); appendF32(bin, 0.0f); appendF32(bin, 0.0f);
    appendF32(bin, 0.0f); appendF32(bin, 0.0f); appendF32(bin, 0.0f);

    const size_t indicesOffset = bin.size();
    appendU16(bin, 0); appendU16(bin, 1); appendU16(bin, 2);
    pad4(bin, 0);

    const size_t inputOffset = bin.size();
    appendF32(bin, 0.0f); appendF32(bin, 1.0f);

    const size_t outputOffset = bin.size();
    appendF32(bin, 0.0f); appendF32(bin, 1.0f);

    const std::string jsonText =
        std::string("{") +
        "\"asset\":{\"version\":\"2.0\"}," +
        "\"scene\":0," +
        "\"scenes\":[{\"nodes\":[0]}]," +
        "\"nodes\":[{\"mesh\":0}]," +
        "\"meshes\":[{\"weights\":[0],\"primitives\":[{\"attributes\":{\"POSITION\":0,\"NORMAL\":1},\"targets\":[{\"POSITION\":2}],\"indices\":3,\"mode\":4}]}]," +
        "\"animations\":[{\"samplers\":[{\"input\":4,\"output\":5,\"interpolation\":\"LINEAR\"}],\"channels\":[{\"sampler\":0,\"target\":{\"node\":0,\"path\":\"weights\"}}]}]," +
        "\"buffers\":[{\"byteLength\":" + std::to_string(bin.size()) + "}]," +
        "\"bufferViews\":[" +
        "{\"buffer\":0,\"byteOffset\":" + std::to_string(positionsOffset) + ",\"byteLength\":36}," +
        "{\"buffer\":0,\"byteOffset\":" + std::to_string(normalsOffset) + ",\"byteLength\":36}," +
        "{\"buffer\":0,\"byteOffset\":" + std::to_string(morphOffset) + ",\"byteLength\":36}," +
        "{\"buffer\":0,\"byteOffset\":" + std::to_string(indicesOffset) + ",\"byteLength\":6}," +
        "{\"buffer\":0,\"byteOffset\":" + std::to_string(inputOffset) + ",\"byteLength\":8}," +
        "{\"buffer\":0,\"byteOffset\":" + std::to_string(outputOffset) + ",\"byteLength\":8}" +
        "]," +
        "\"accessors\":[" +
        "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"}," +
        "{\"bufferView\":1,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"}," +
        "{\"bufferView\":2,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"}," +
        "{\"bufferView\":3,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}," +
        "{\"bufferView\":4,\"componentType\":5126,\"count\":2,\"type\":\"SCALAR\"}," +
        "{\"bufferView\":5,\"componentType\":5126,\"count\":2,\"type\":\"SCALAR\"}" +
        "]}";

    std::vector<uint8_t> jsonBytes(jsonText.begin(), jsonText.end());
    pad4(jsonBytes, 0x20);
    std::vector<uint8_t> glb;
    appendU32(glb, 0x46546C67u);
    appendU32(glb, 2u);
    appendU32(glb, static_cast<uint32_t>(12 + 8 + jsonBytes.size() + 8 + bin.size()));
    appendU32(glb, static_cast<uint32_t>(jsonBytes.size()));
    appendU32(glb, 0x4E4F534Au);
    glb.insert(glb.end(), jsonBytes.begin(), jsonBytes.end());
    appendU32(glb, static_cast<uint32_t>(bin.size()));
    appendU32(glb, 0x004E4942u);
    glb.insert(glb.end(), bin.begin(), bin.end());
    return glb;
}

std::vector<uint8_t> makeAnimatedMorphTangentWeightTriangleGlb(
    bool includeBaseTangent) {
    std::vector<uint8_t> bin;
    const size_t positionsOffset = bin.size();
    appendF32(bin, 0.0f); appendF32(bin, 0.0f); appendF32(bin, 0.0f);
    appendF32(bin, 1.0f); appendF32(bin, 0.0f); appendF32(bin, 0.0f);
    appendF32(bin, 0.0f); appendF32(bin, 1.0f); appendF32(bin, 0.0f);

    const size_t normalsOffset = bin.size();
    for (int i = 0; i < 3; ++i) {
        appendF32(bin, 0.0f); appendF32(bin, 0.0f); appendF32(bin, 1.0f);
    }

    size_t baseTangentsOffset = 0;
    if (includeBaseTangent) {
        baseTangentsOffset = bin.size();
        for (int i = 0; i < 3; ++i) {
            appendF32(bin, 1.0f);
            appendF32(bin, 0.0f);
            appendF32(bin, 0.0f);
            appendF32(bin, 1.0f);
        }
    }

    const size_t morphTangentsOffset = bin.size();
    for (int i = 0; i < 3; ++i) {
        appendF32(bin, -1.0f);
        appendF32(bin, 1.0f);
        appendF32(bin, 0.0f);
    }

    const size_t indicesOffset = bin.size();
    appendU16(bin, 0); appendU16(bin, 1); appendU16(bin, 2);
    pad4(bin, 0);

    const size_t inputOffset = bin.size();
    appendF32(bin, 0.0f); appendF32(bin, 1.0f);

    const size_t outputOffset = bin.size();
    appendF32(bin, 0.0f); appendF32(bin, 1.0f);

    int nextAccessor = 2;
    std::string attributes = "\"POSITION\":0,\"NORMAL\":1";
    std::string bufferViews =
        "{\"buffer\":0,\"byteOffset\":" +
        std::to_string(positionsOffset) +
        ",\"byteLength\":36}," +
        "{\"buffer\":0,\"byteOffset\":" +
        std::to_string(normalsOffset) +
        ",\"byteLength\":36}";
    std::string accessors =
        "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"}," +
        std::string(
            "{\"bufferView\":1,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"}");

    if (includeBaseTangent) {
        const int tangentAccessor = nextAccessor++;
        attributes += ",\"TANGENT\":" + std::to_string(tangentAccessor);
        bufferViews +=
            ",{\"buffer\":0,\"byteOffset\":" +
            std::to_string(baseTangentsOffset) +
            ",\"byteLength\":48}";
        accessors +=
            ",{\"bufferView\":" +
            std::to_string(tangentAccessor) +
            ",\"componentType\":5126,\"count\":3,\"type\":\"VEC4\"}";
    }

    const int morphTangentAccessor = nextAccessor++;
    bufferViews +=
        ",{\"buffer\":0,\"byteOffset\":" +
        std::to_string(morphTangentsOffset) +
        ",\"byteLength\":36}";
    accessors +=
        ",{\"bufferView\":" +
        std::to_string(morphTangentAccessor) +
        ",\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"}";

    const int indexAccessor = nextAccessor++;
    bufferViews +=
        ",{\"buffer\":0,\"byteOffset\":" +
        std::to_string(indicesOffset) +
        ",\"byteLength\":6}";
    accessors +=
        ",{\"bufferView\":" +
        std::to_string(indexAccessor) +
        ",\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}";

    const int inputAccessor = nextAccessor++;
    bufferViews +=
        ",{\"buffer\":0,\"byteOffset\":" +
        std::to_string(inputOffset) +
        ",\"byteLength\":8}";
    accessors +=
        ",{\"bufferView\":" +
        std::to_string(inputAccessor) +
        ",\"componentType\":5126,\"count\":2,\"type\":\"SCALAR\"}";

    const int outputAccessor = nextAccessor;
    bufferViews +=
        ",{\"buffer\":0,\"byteOffset\":" +
        std::to_string(outputOffset) +
        ",\"byteLength\":8}";
    accessors +=
        ",{\"bufferView\":" +
        std::to_string(outputAccessor) +
        ",\"componentType\":5126,\"count\":2,\"type\":\"SCALAR\"}";

    const std::string jsonText =
        std::string("{") +
        "\"asset\":{\"version\":\"2.0\"}," +
        "\"scene\":0," +
        "\"scenes\":[{\"nodes\":[0]}]," +
        "\"nodes\":[{\"mesh\":0}]," +
        "\"meshes\":[{\"weights\":[0],\"primitives\":[{\"attributes\":{" +
        attributes +
        "},\"targets\":[{\"TANGENT\":" +
        std::to_string(morphTangentAccessor) +
        "}],\"indices\":" +
        std::to_string(indexAccessor) +
        ",\"mode\":4}]}]," +
        "\"animations\":[{\"samplers\":[{\"input\":" +
        std::to_string(inputAccessor) +
        ",\"output\":" +
        std::to_string(outputAccessor) +
        ",\"interpolation\":\"LINEAR\"}],\"channels\":[{\"sampler\":0,\"target\":{\"node\":0,\"path\":\"weights\"}}]}]," +
        "\"buffers\":[{\"byteLength\":" + std::to_string(bin.size()) + "}]," +
        "\"bufferViews\":[" + bufferViews + "]," +
        "\"accessors\":[" + accessors + "]}";

    std::vector<uint8_t> jsonBytes(jsonText.begin(), jsonText.end());
    pad4(jsonBytes, 0x20);
    std::vector<uint8_t> glb;
    appendU32(glb, 0x46546C67u);
    appendU32(glb, 2u);
    appendU32(glb, static_cast<uint32_t>(
        12 + 8 + jsonBytes.size() + 8 + bin.size()));
    appendU32(glb, static_cast<uint32_t>(jsonBytes.size()));
    appendU32(glb, 0x4E4F534Au);
    glb.insert(glb.end(), jsonBytes.begin(), jsonBytes.end());
    appendU32(glb, static_cast<uint32_t>(bin.size()));
    appendU32(glb, 0x004E4942u);
    glb.insert(glb.end(), bin.begin(), bin.end());
    return glb;
}

std::vector<uint8_t> makeQuantizedTriangleGlb(bool declareExtension) {
    std::vector<uint8_t> bin;
    const size_t positionsOffset = bin.size();
    appendU16(bin, 0); appendU16(bin, 0); appendU16(bin, 0);
    appendU16(bin, 32767); appendU16(bin, 0); appendU16(bin, 0);
    appendU16(bin, 0); appendU16(bin, 32767); appendU16(bin, 0);

    const size_t texCoordsOffset = bin.size();
    appendU16(bin, 10); appendU16(bin, 20);
    appendU16(bin, 30); appendU16(bin, 40);
    appendU16(bin, 50); appendU16(bin, 60);

    const size_t tangentsOffset = bin.size();
    for (int i = 0; i < 3; ++i) {
        bin.push_back(127);
        bin.push_back(0);
        bin.push_back(0);
        bin.push_back(127);
    }

    const size_t normalsOffset = bin.size();
    for (int i = 0; i < 3; ++i) {
        bin.push_back(0);
        bin.push_back(0);
        bin.push_back(127);
    }

    if ((bin.size() % 2u) != 0u) {
        bin.push_back(0);
    }
    const size_t indicesOffset = bin.size();
    appendU16(bin, 0); appendU16(bin, 1); appendU16(bin, 2);
    pad4(bin, 0);

    const std::string extensionDeclarations = declareExtension
        ? "\"extensionsUsed\":[\"KHR_mesh_quantization\"],"
          "\"extensionsRequired\":[\"KHR_mesh_quantization\"],"
        : "";
    const std::string jsonText =
        std::string("{") +
        "\"asset\":{\"version\":\"2.0\"}," +
        extensionDeclarations +
        "\"scene\":0," +
        "\"scenes\":[{\"nodes\":[0]}]," +
        "\"nodes\":[{\"mesh\":0}]," +
        "\"meshes\":[{\"primitives\":[{\"attributes\":{"
        "\"POSITION\":0,\"TEXCOORD_0\":1,\"TANGENT\":2,\"NORMAL\":3"
        "},\"indices\":4,\"mode\":4}]}]," +
        "\"buffers\":[{\"byteLength\":" + std::to_string(bin.size()) + "}]," +
        "\"bufferViews\":[" +
        "{\"buffer\":0,\"byteOffset\":" + std::to_string(positionsOffset) + ",\"byteLength\":18}," +
        "{\"buffer\":0,\"byteOffset\":" + std::to_string(texCoordsOffset) + ",\"byteLength\":12}," +
        "{\"buffer\":0,\"byteOffset\":" + std::to_string(tangentsOffset) + ",\"byteLength\":12}," +
        "{\"buffer\":0,\"byteOffset\":" + std::to_string(normalsOffset) + ",\"byteLength\":9}," +
        "{\"buffer\":0,\"byteOffset\":" + std::to_string(indicesOffset) + ",\"byteLength\":6}" +
        "]," +
        "\"accessors\":[" +
        "{\"bufferView\":0,\"componentType\":5122,\"count\":3,\"type\":\"VEC3\",\"normalized\":true}," +
        "{\"bufferView\":1,\"componentType\":5123,\"count\":3,\"type\":\"VEC2\"}," +
        "{\"bufferView\":2,\"componentType\":5120,\"count\":3,\"type\":\"VEC4\",\"normalized\":true}," +
        "{\"bufferView\":3,\"componentType\":5120,\"count\":3,\"type\":\"VEC3\",\"normalized\":true}," +
        "{\"bufferView\":4,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}" +
        "]}";

    std::vector<uint8_t> jsonBytes(jsonText.begin(), jsonText.end());
    pad4(jsonBytes, 0x20);
    std::vector<uint8_t> glb;
    appendU32(glb, 0x46546C67u);
    appendU32(glb, 2u);
    appendU32(glb, static_cast<uint32_t>(
        12 + 8 + jsonBytes.size() + 8 + bin.size()));
    appendU32(glb, static_cast<uint32_t>(jsonBytes.size()));
    appendU32(glb, 0x4E4F534Au);
    glb.insert(glb.end(), jsonBytes.begin(), jsonBytes.end());
    appendU32(glb, static_cast<uint32_t>(bin.size()));
    appendU32(glb, 0x004E4942u);
    glb.insert(glb.end(), bin.begin(), bin.end());
    return glb;
}

struct ExternalGltfFixture {
    std::string jsonText;
    std::vector<uint8_t> bin;
};

ExternalGltfFixture makeExternalBufferTriangleGltf() {
    std::vector<uint8_t> bin;
    const size_t positionsOffset = bin.size();
    appendF32(bin, 0.0f); appendF32(bin, 0.0f); appendF32(bin, 0.0f);
    appendF32(bin, 1.0f); appendF32(bin, 0.0f); appendF32(bin, 0.0f);
    appendF32(bin, 0.0f); appendF32(bin, 1.0f); appendF32(bin, 0.0f);

    const size_t normalsOffset = bin.size();
    for (int i = 0; i < 3; ++i) {
        appendF32(bin, 0.0f); appendF32(bin, 0.0f); appendF32(bin, 1.0f);
    }

    const size_t uvOffset = bin.size();
    appendF32(bin, 0.0f); appendF32(bin, 0.0f);
    appendF32(bin, 1.0f); appendF32(bin, 0.0f);
    appendF32(bin, 0.0f); appendF32(bin, 1.0f);

    const size_t indicesOffset = bin.size();
    appendU16(bin, 0); appendU16(bin, 1); appendU16(bin, 2);
    pad4(bin, 0);

    ExternalGltfFixture fixture;
    fixture.bin = std::move(bin);
    fixture.jsonText =
        std::string("{") +
        "\"asset\":{\"version\":\"2.0\"}," +
        "\"scene\":0," +
        "\"scenes\":[{\"nodes\":[0]}]," +
        "\"nodes\":[{\"mesh\":0,\"translation\":[10,20,30]}]," +
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"NORMAL\":1,\"TEXCOORD_0\":2},\"indices\":3,\"mode\":4}]}]," +
        "\"buffers\":[{\"uri\":\"triangle.bin\",\"byteLength\":" +
        std::to_string(fixture.bin.size()) + "}]," +
        "\"bufferViews\":[" +
        "{\"buffer\":0,\"byteOffset\":" + std::to_string(positionsOffset) + ",\"byteLength\":36}," +
        "{\"buffer\":0,\"byteOffset\":" + std::to_string(normalsOffset) + ",\"byteLength\":36}," +
        "{\"buffer\":0,\"byteOffset\":" + std::to_string(uvOffset) + ",\"byteLength\":24}," +
        "{\"buffer\":0,\"byteOffset\":" + std::to_string(indicesOffset) + ",\"byteLength\":6}" +
        "]," +
        "\"accessors\":[" +
        "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"}," +
        "{\"bufferView\":1,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"}," +
        "{\"bufferView\":2,\"componentType\":5126,\"count\":3,\"type\":\"VEC2\"}," +
        "{\"bufferView\":3,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}" +
        "]}";
    return fixture;
}

ExternalGltfFixture makeStridedUnsignedByteTexcoordExternalGltf(
    size_t byteStride) {
    std::vector<uint8_t> bin;
    const size_t positionsOffset = bin.size();
    appendF32(bin, 0.0f); appendF32(bin, 0.0f); appendF32(bin, 0.0f);
    appendF32(bin, 1.0f); appendF32(bin, 0.0f); appendF32(bin, 0.0f);
    appendF32(bin, 0.0f); appendF32(bin, 1.0f); appendF32(bin, 0.0f);

    const size_t normalsOffset = bin.size();
    for (int i = 0; i < 3; ++i) {
        appendF32(bin, 0.0f); appendF32(bin, 0.0f); appendF32(bin, 1.0f);
    }

    const size_t uvOffset = bin.size();
    const uint8_t uvs[3][2] = {{0, 0}, {255, 0}, {0, 255}};
    for (int vertex = 0; vertex < 3; ++vertex) {
        bin.push_back(uvs[vertex][0]);
        bin.push_back(uvs[vertex][1]);
        if (vertex < 2) {
            bin.insert(bin.end(), byteStride - 2u, 0);
        }
    }
    const size_t uvByteLength = (byteStride * 2u) + 2u;

    const size_t indicesOffset = bin.size();
    appendU16(bin, 0); appendU16(bin, 1); appendU16(bin, 2);
    pad4(bin, 0);

    ExternalGltfFixture fixture;
    fixture.bin = std::move(bin);
    fixture.jsonText =
        std::string("{") +
        "\"asset\":{\"version\":\"2.0\"}," +
        "\"scene\":0," +
        "\"scenes\":[{\"nodes\":[0]}]," +
        "\"nodes\":[{\"mesh\":0,\"translation\":[10,20,30]}]," +
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"NORMAL\":1,\"TEXCOORD_0\":2},\"indices\":3,\"mode\":4}]}]," +
        "\"buffers\":[{\"uri\":\"triangle.bin\",\"byteLength\":" +
        std::to_string(fixture.bin.size()) + "}]," +
        "\"bufferViews\":[" +
        "{\"buffer\":0,\"byteOffset\":" + std::to_string(positionsOffset) + ",\"byteLength\":36}," +
        "{\"buffer\":0,\"byteOffset\":" + std::to_string(normalsOffset) + ",\"byteLength\":36}," +
        "{\"buffer\":0,\"byteOffset\":" + std::to_string(uvOffset) + ",\"byteLength\":" +
        std::to_string(uvByteLength) + ",\"byteStride\":" +
        std::to_string(byteStride) + "}," +
        "{\"buffer\":0,\"byteOffset\":" + std::to_string(indicesOffset) + ",\"byteLength\":6}" +
        "]," +
        "\"accessors\":[" +
        "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"}," +
        "{\"bufferView\":1,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"}," +
        "{\"bufferView\":2,\"componentType\":5121,\"count\":3,\"type\":\"VEC2\",\"normalized\":true}," +
        "{\"bufferView\":3,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}" +
        "]}";
    return fixture;
}

ExternalGltfFixture makeTexturedExternalBufferTriangleGltf(
    const std::string& imageUri) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    fixture.jsonText =
        std::string("{") +
        "\"asset\":{\"version\":\"2.0\"}," +
        "\"scene\":0," +
        "\"scenes\":[{\"nodes\":[0]}]," +
        "\"nodes\":[{\"mesh\":0,\"translation\":[10,20,30]}]," +
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"NORMAL\":1,\"TEXCOORD_0\":2},\"indices\":3,\"mode\":4,\"material\":0}]}]," +
        "\"materials\":[{\"doubleSided\":true,\"alphaMode\":\"BLEND\",\"alphaCutoff\":0.25,\"pbrMetallicRoughness\":{\"baseColorFactor\":[0.5,0.6,0.7,0.8],\"baseColorTexture\":{\"index\":0,\"texCoord\":0}}}]," +
        "\"textures\":[{\"source\":0,\"sampler\":0}]," +
        "\"samplers\":[{\"minFilter\":9728,\"magFilter\":9728,\"wrapS\":33648,\"wrapT\":33071}]," +
        "\"images\":[{\"uri\":\"" + imageUri + "\"}]," +
        "\"buffers\":[{\"uri\":\"triangle.bin\",\"byteLength\":" +
        std::to_string(fixture.bin.size()) + "}]," +
        "\"bufferViews\":[" +
        "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36}," +
        "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":36}," +
        "{\"buffer\":0,\"byteOffset\":72,\"byteLength\":24}," +
        "{\"buffer\":0,\"byteOffset\":96,\"byteLength\":6}" +
        "]," +
        "\"accessors\":[" +
        "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"}," +
        "{\"bufferView\":1,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"}," +
        "{\"bufferView\":2,\"componentType\":5126,\"count\":3,\"type\":\"VEC2\"}," +
        "{\"bufferView\":3,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}" +
        "]}";
    return fixture;
}

ExternalGltfFixture makeExternalWebpTextureExtensionGltf() {
    ExternalGltfFixture fixture =
        makeTexturedExternalBufferTriangleGltf("fallback.png");
    const std::string assetMarker = "\"asset\":{\"version\":\"2.0\"},";
    const size_t assetPos = fixture.jsonText.find(assetMarker);
    if (assetPos != std::string::npos) {
        fixture.jsonText.insert(
            assetPos + assetMarker.size(),
            "\"extensionsUsed\":[\"EXT_texture_webp\"],"
            "\"extensionsRequired\":[\"EXT_texture_webp\"],");
    }

    const std::string textureMarker =
        "\"textures\":[{\"source\":0,\"sampler\":0}]";
    const size_t texturePos = fixture.jsonText.find(textureMarker);
    if (texturePos != std::string::npos) {
        fixture.jsonText.replace(
            texturePos,
            textureMarker.size(),
            "\"textures\":[{\"source\":0,\"sampler\":0,"
            "\"extensions\":{\"EXT_texture_webp\":{\"source\":1}}}]");
    }

    const std::string imageMarker =
        "\"images\":[{\"uri\":\"fallback.png\"}]";
    const size_t imagePos = fixture.jsonText.find(imageMarker);
    if (imagePos != std::string::npos) {
        fixture.jsonText.replace(
            imagePos,
            imageMarker.size(),
            "\"images\":[{\"uri\":\"fallback.png\"},"
            "{\"uri\":\"texture.webp\"}]");
    }
    return fixture;
}

ExternalGltfFixture makeBufferViewImageExternalBufferTriangleGltf(
    bool includeMimeType = true,
    const std::string& mimeType = "image/png",
    const std::vector<uint8_t>& imageBytes = {}) {
    ExternalGltfFixture fixture =
        makeTexturedExternalBufferTriangleGltf("image.bin");
    const size_t imageOffset = fixture.bin.size();
    const std::vector<uint8_t> embeddedImageBytes = !imageBytes.empty()
        ? imageBytes
        : (mimeType == "image/webp"
               ? makeFakeWebpBytes()
               : std::vector<uint8_t>{9, 8, 7, 6});
    fixture.bin.insert(
        fixture.bin.end(),
        embeddedImageBytes.begin(),
        embeddedImageBytes.end());

    const std::string oldBuffer =
        "\"buffers\":[{\"uri\":\"triangle.bin\",\"byteLength\":" +
        std::to_string(imageOffset) + "}]";
    const std::string newBuffer =
        "\"buffers\":[{\"uri\":\"triangle.bin\",\"byteLength\":" +
        std::to_string(fixture.bin.size()) + "}]";
    const size_t bufferPos = fixture.jsonText.find(oldBuffer);
    if (bufferPos != std::string::npos) {
        fixture.jsonText.replace(bufferPos, oldBuffer.size(), newBuffer);
    }

    const std::string oldImage = "\"images\":[{\"uri\":\"image.bin\"}]";
    std::string newImage = "\"images\":[{\"bufferView\":4";
    if (includeMimeType) {
        newImage += ",\"mimeType\":\"" + mimeType + "\"";
    }
    newImage += "}]";
    const size_t imagePos = fixture.jsonText.find(oldImage);
    if (imagePos != std::string::npos) {
        fixture.jsonText.replace(imagePos, oldImage.size(), newImage);
    }

    const std::string lastBufferView =
        "{\"buffer\":0,\"byteOffset\":96,\"byteLength\":6}";
    const std::string imageBufferView =
        lastBufferView + ",{\"buffer\":0,\"byteOffset\":" +
        std::to_string(imageOffset) + ",\"byteLength\":" +
        std::to_string(embeddedImageBytes.size()) + "}";
    const size_t bufferViewPos = fixture.jsonText.find(lastBufferView);
    if (bufferViewPos != std::string::npos) {
        fixture.jsonText.replace(
            bufferViewPos,
            lastBufferView.size(),
            imageBufferView);
    }
    return fixture;
}

ExternalGltfFixture makeFullMaterialExternalBufferTriangleGltf() {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    fixture.jsonText =
        std::string("{") +
        "\"asset\":{\"version\":\"2.0\"}," +
        "\"extensionsUsed\":[\"KHR_texture_transform\"]," +
        "\"extensionsRequired\":[\"KHR_texture_transform\"]," +
        "\"scene\":0," +
        "\"scenes\":[{\"nodes\":[0]}]," +
        "\"nodes\":[{\"mesh\":0,\"translation\":[10,20,30]}]," +
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"NORMAL\":1,\"TEXCOORD_0\":2},\"indices\":3,\"mode\":4,\"material\":0}]}]," +
        "\"materials\":[{" +
        "\"doubleSided\":true," +
        "\"alphaMode\":\"BLEND\"," +
        "\"alphaCutoff\":0.25," +
        "\"emissiveFactor\":[0.1,0.2,0.3]," +
        "\"normalTexture\":{\"index\":2,\"texCoord\":0,\"scale\":0.35}," +
        "\"occlusionTexture\":{\"index\":3,\"texCoord\":0,\"strength\":0.65}," +
        "\"emissiveTexture\":{\"index\":4,\"texCoord\":0}," +
        "\"pbrMetallicRoughness\":{" +
        "\"baseColorFactor\":[0.5,0.6,0.7,0.8]," +
        "\"metallicFactor\":0.4," +
        "\"roughnessFactor\":0.75," +
        "\"baseColorTexture\":{\"index\":0,\"texCoord\":0,\"extensions\":{\"KHR_texture_transform\":{\"offset\":[0.25,0.5],\"scale\":[2,3],\"rotation\":1.57079632679}}}," +
        "\"metallicRoughnessTexture\":{\"index\":1,\"texCoord\":0}" +
        "}}]," +
        "\"textures\":[" +
        "{\"source\":0,\"sampler\":0}," +
        "{\"source\":1,\"sampler\":0}," +
        "{\"source\":2,\"sampler\":0}," +
        "{\"source\":3,\"sampler\":0}," +
        "{\"source\":4,\"sampler\":0}" +
        "]," +
        "\"samplers\":[{\"minFilter\":9729,\"magFilter\":9729,\"wrapS\":10497,\"wrapT\":10497}]," +
        "\"images\":[" +
        "{\"uri\":\"base.bin\"}," +
        "{\"uri\":\"mr.bin\"}," +
        "{\"uri\":\"normal.bin\"}," +
        "{\"uri\":\"occlusion.bin\"}," +
        "{\"uri\":\"emissive.bin\"}" +
        "]," +
        "\"buffers\":[{\"uri\":\"triangle.bin\",\"byteLength\":" +
        std::to_string(fixture.bin.size()) + "}]," +
        "\"bufferViews\":[" +
        "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36}," +
        "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":36}," +
        "{\"buffer\":0,\"byteOffset\":72,\"byteLength\":24}," +
        "{\"buffer\":0,\"byteOffset\":96,\"byteLength\":6}" +
        "]," +
        "\"accessors\":[" +
        "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"}," +
        "{\"bufferView\":1,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"}," +
        "{\"bufferView\":2,\"componentType\":5126,\"count\":3,\"type\":\"VEC2\"}," +
        "{\"bufferView\":3,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}" +
        "]}";
    return fixture;
}

std::unique_ptr<GltfModel> parseExternalFixture(
    const ExternalGltfFixture& fixture) {
    return GltfParser::parse(
        reinterpret_cast<const uint8_t*>(fixture.jsonText.data()),
        fixture.jsonText.size(),
        [&](const std::string& uri) {
            return uri == "triangle.bin" ? fixture.bin
                                         : std::vector<uint8_t>{};
        });
}

bool replaceTopLevelJsonArray(
    ExternalGltfFixture& fixture,
    const std::string& property,
    const std::string& replacementValue) {
    const std::string prefix = "\"" + property + "\":[";
    const size_t start = fixture.jsonText.find(prefix);
    if (start == std::string::npos) {
        return false;
    }
    const size_t end = fixture.jsonText.find("]", start + prefix.size());
    if (end == std::string::npos) {
        return false;
    }
    fixture.jsonText.replace(
        start,
        end - start + 1u,
        "\"" + property + "\":" + replacementValue);
    return true;
}

void appendAccessorToExternalFixture(
    ExternalGltfFixture& fixture,
    const std::string& accessorJson) {
    const std::string marker =
        "{\"bufferView\":3,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.replace(
        markerPos,
        marker.size(),
        marker + "," + accessorJson);
}

void appendUnreferencedMeshToExternalFixture(
    ExternalGltfFixture& fixture,
    const std::string& primitiveJson,
    const std::string& meshPrefix = std::string{}) {
    const std::string primitive =
        "{\"attributes\":{\"POSITION\":0,\"NORMAL\":1,\"TEXCOORD_0\":2},"
        "\"indices\":3,\"mode\":4}";
    const std::string marker =
        "\"meshes\":[{\"primitives\":[" + primitive + "]}]";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.replace(
        markerPos,
        marker.size(),
        "\"meshes\":[{\"primitives\":[" + primitive + "]},{" +
            meshPrefix + "\"primitives\":[" + primitiveJson + "]}]");
}

enum class GpuInstanceRotationEncoding {
    Float,
    ZeroFloat,
    NormalizedByte,
    NormalizedShort,
    NormalizedUnsignedByte,
    NormalizedUnsignedShort
};

ExternalGltfFixture makeGpuInstancedExternalGltf(
    GpuInstanceRotationEncoding rotationEncoding =
        GpuInstanceRotationEncoding::Float) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const size_t originalByteLength = fixture.bin.size();

    const size_t translationsOffset = fixture.bin.size();
    appendF32(fixture.bin, 1.0f);
    appendF32(fixture.bin, 2.0f);
    appendF32(fixture.bin, 3.0f);
    appendF32(fixture.bin, 4.0f);
    appendF32(fixture.bin, 5.0f);
    appendF32(fixture.bin, 6.0f);

    const size_t rotationsOffset = fixture.bin.size();
    switch (rotationEncoding) {
        case GpuInstanceRotationEncoding::Float: {
            appendF32(fixture.bin, 0.0f);
            appendF32(fixture.bin, 0.0f);
            appendF32(fixture.bin, 0.0f);
            appendF32(fixture.bin, 1.0f);
            constexpr float kSqrtHalf = 0.7071067811865476f;
            appendF32(fixture.bin, 0.0f);
            appendF32(fixture.bin, 0.0f);
            appendF32(fixture.bin, kSqrtHalf);
            appendF32(fixture.bin, kSqrtHalf);
            break;
        }
        case GpuInstanceRotationEncoding::ZeroFloat:
            for (int i = 0; i < 8; ++i) {
                appendF32(fixture.bin, 0.0f);
            }
            break;
        case GpuInstanceRotationEncoding::NormalizedByte:
            fixture.bin.push_back(0);
            fixture.bin.push_back(0);
            fixture.bin.push_back(0);
            fixture.bin.push_back(127);
            fixture.bin.push_back(0);
            fixture.bin.push_back(0);
            fixture.bin.push_back(90);
            fixture.bin.push_back(90);
            break;
        case GpuInstanceRotationEncoding::NormalizedShort:
            appendI16(fixture.bin, 0);
            appendI16(fixture.bin, 0);
            appendI16(fixture.bin, 0);
            appendI16(fixture.bin, 32767);
            appendI16(fixture.bin, 0);
            appendI16(fixture.bin, 0);
            appendI16(fixture.bin, 23170);
            appendI16(fixture.bin, 23170);
            break;
        case GpuInstanceRotationEncoding::NormalizedUnsignedByte:
            fixture.bin.push_back(0);
            fixture.bin.push_back(0);
            fixture.bin.push_back(0);
            fixture.bin.push_back(255);
            fixture.bin.push_back(0);
            fixture.bin.push_back(0);
            fixture.bin.push_back(180);
            fixture.bin.push_back(180);
            break;
        case GpuInstanceRotationEncoding::NormalizedUnsignedShort:
            appendU16(fixture.bin, 0);
            appendU16(fixture.bin, 0);
            appendU16(fixture.bin, 0);
            appendU16(fixture.bin, 65535);
            appendU16(fixture.bin, 0);
            appendU16(fixture.bin, 0);
            appendU16(fixture.bin, 46341);
            appendU16(fixture.bin, 46341);
            break;
    }
    const size_t rotationsByteLength =
        fixture.bin.size() - rotationsOffset;

    const size_t scalesOffset = fixture.bin.size();
    appendF32(fixture.bin, 2.0f);
    appendF32(fixture.bin, 3.0f);
    appendF32(fixture.bin, 4.0f);
    appendF32(fixture.bin, 1.0f);
    appendF32(fixture.bin, 2.0f);
    appendF32(fixture.bin, 1.0f);
    pad4(fixture.bin, 0);

    const std::string oldByteLength =
        "\"byteLength\":" + std::to_string(originalByteLength);
    const size_t byteLengthPos = fixture.jsonText.find(oldByteLength);
    if (byteLengthPos != std::string::npos) {
        fixture.jsonText.replace(
            byteLengthPos,
            oldByteLength.size(),
            "\"byteLength\":" + std::to_string(fixture.bin.size()));
    }

    const std::string assetMarker = "\"asset\":{\"version\":\"2.0\"},";
    const size_t assetPos = fixture.jsonText.find(assetMarker);
    if (assetPos != std::string::npos) {
        fixture.jsonText.insert(
            assetPos + assetMarker.size(),
            "\"extensionsUsed\":[\"EXT_mesh_gpu_instancing\"],"
            "\"extensionsRequired\":[\"EXT_mesh_gpu_instancing\"],");
    }

    const std::string nodeMarker =
        "{\"mesh\":0,\"translation\":[10,20,30]}";
    const size_t nodePos = fixture.jsonText.find(nodeMarker);
    if (nodePos != std::string::npos) {
        fixture.jsonText.replace(
            nodePos,
            nodeMarker.size(),
            "{\"mesh\":0,\"translation\":[10,20,30],"
            "\"extensions\":{\"EXT_mesh_gpu_instancing\":{"
            "\"attributes\":{\"TRANSLATION\":4,\"ROTATION\":5,\"SCALE\":6}}}}");
    }

    const std::string accessorsMarker = "],\"accessors\":[";
    const size_t accessorsPos = fixture.jsonText.find(accessorsMarker);
    if (accessorsPos != std::string::npos) {
        const std::string extraViews =
            ",{\"buffer\":0,\"byteOffset\":" +
            std::to_string(translationsOffset) +
            ",\"byteLength\":24}" +
            ",{\"buffer\":0,\"byteOffset\":" +
            std::to_string(rotationsOffset) +
            ",\"byteLength\":" +
            std::to_string(rotationsByteLength) + "}" +
            ",{\"buffer\":0,\"byteOffset\":" +
            std::to_string(scalesOffset) +
            ",\"byteLength\":24}";
        fixture.jsonText.insert(accessorsPos, extraViews);
    }

    std::string rotationAccessor;
    switch (rotationEncoding) {
        case GpuInstanceRotationEncoding::Float:
        case GpuInstanceRotationEncoding::ZeroFloat:
            rotationAccessor =
                "{\"bufferView\":5,\"componentType\":5126,\"count\":2,\"type\":\"VEC4\"}";
            break;
        case GpuInstanceRotationEncoding::NormalizedByte:
            rotationAccessor =
                "{\"bufferView\":5,\"componentType\":5120,\"normalized\":true,\"count\":2,\"type\":\"VEC4\"}";
            break;
        case GpuInstanceRotationEncoding::NormalizedShort:
            rotationAccessor =
                "{\"bufferView\":5,\"componentType\":5122,\"normalized\":true,\"count\":2,\"type\":\"VEC4\"}";
            break;
        case GpuInstanceRotationEncoding::NormalizedUnsignedByte:
            rotationAccessor =
                "{\"bufferView\":5,\"componentType\":5121,\"normalized\":true,\"count\":2,\"type\":\"VEC4\"}";
            break;
        case GpuInstanceRotationEncoding::NormalizedUnsignedShort:
            rotationAccessor =
                "{\"bufferView\":5,\"componentType\":5123,\"normalized\":true,\"count\":2,\"type\":\"VEC4\"}";
            break;
    }
    appendAccessorToExternalFixture(
        fixture,
        "{\"bufferView\":4,\"componentType\":5126,\"count\":2,\"type\":\"VEC3\"},"
        + rotationAccessor + ","
        "{\"bufferView\":6,\"componentType\":5126,\"count\":2,\"type\":\"VEC3\"}");
    return fixture;
}

std::unique_ptr<GltfModel> parseExternalFixtureWithSolidImage(
    const ExternalGltfFixture& fixture) {
    return GltfParser::parse(
        reinterpret_cast<const uint8_t*>(fixture.jsonText.data()),
        fixture.jsonText.size(),
        [&](const std::string& uri) {
            return uri == "triangle.bin" ? fixture.bin
                                         : std::vector<uint8_t>{7};
        },
        [](const uint8_t*, size_t) -> std::optional<GltfImage> {
            GltfImage image;
            image.width = 1;
            image.height = 1;
            image.channels = 4;
            image.pixels = {255, 255, 255, 255};
            return image;
        });
}

class TestHttpRequest final : public HttpRequest {
public:
    void cancel() override {}
};

class TestPlatformBridge final : public PlatformBridge {
public:
    explicit TestPlatformBridge(
        std::function<std::unique_ptr<DecodedImage>(
            const uint8_t*, size_t)> decoder)
        : decoder_(std::move(decoder)) {}

    void onMemoryPressure() override {}
    void onEnterBackground() override {}
    void onEnterForeground() override {}
    std::unique_ptr<HttpRequest> get(
        const std::string&,
        std::function<void(int, std::vector<uint8_t>)> callback) override {
        callback(-1, {});
        return std::make_unique<TestHttpRequest>();
    }
    std::string cacheDirectory() const override { return "/tmp"; }
    std::string documentsDirectory() const override { return "/tmp"; }
    std::unique_ptr<DecodedImage> decodeImage(
        const uint8_t* data,
        size_t len) override {
        return decoder_ ? decoder_(data, len) : nullptr;
    }
    void log(LogLevel, const std::string&, const std::string&) override {}
    DeviceInfo deviceInfo() const override { return {}; }
    std::string getToken(const std::string&) const override { return {}; }

private:
    std::function<std::unique_ptr<DecodedImage>(const uint8_t*, size_t)>
        decoder_;
};

std::vector<uint8_t> makeB3dm(std::vector<uint8_t> glb,
                              const std::string& featureTableJson,
                              const std::string& batchTableJson =
                                  std::string{},
                              std::vector<uint8_t> batchTableBinary = {}) {
    std::vector<uint8_t> featureBytes(
        featureTableJson.begin(),
        featureTableJson.end());
    pad4(featureBytes, 0x20);
    std::vector<uint8_t> batchBytes(
        batchTableJson.begin(),
        batchTableJson.end());
    if (!batchBytes.empty()) {
        pad4(batchBytes, 0x20);
    }
    pad4(batchTableBinary, 0);

    std::vector<uint8_t> b3dm;
    b3dm.push_back('b');
    b3dm.push_back('3');
    b3dm.push_back('d');
    b3dm.push_back('m');
    appendU32(b3dm, 1u);
    appendU32(
        b3dm,
        static_cast<uint32_t>(
            28 + featureBytes.size() + batchBytes.size() +
            batchTableBinary.size() + glb.size()));
    appendU32(b3dm, static_cast<uint32_t>(featureBytes.size()));
    appendU32(b3dm, 0u);
    appendU32(b3dm, static_cast<uint32_t>(batchBytes.size()));
    appendU32(b3dm, static_cast<uint32_t>(batchTableBinary.size()));
    b3dm.insert(b3dm.end(), featureBytes.begin(), featureBytes.end());
    b3dm.insert(b3dm.end(), batchBytes.begin(), batchBytes.end());
    b3dm.insert(b3dm.end(),
                batchTableBinary.begin(),
                batchTableBinary.end());
    b3dm.insert(b3dm.end(), glb.begin(), glb.end());
    return b3dm;
}

std::vector<uint8_t> makeLegacyB3dmHeader1(
    std::vector<uint8_t> glb,
    uint32_t batchLength,
    const std::string& batchTableJson = std::string{}) {
    std::vector<uint8_t> batchBytes(
        batchTableJson.begin(),
        batchTableJson.end());
    if (!batchBytes.empty()) {
        pad4(batchBytes, 0x20);
    }

    std::vector<uint8_t> b3dm;
    b3dm.push_back('b');
    b3dm.push_back('3');
    b3dm.push_back('d');
    b3dm.push_back('m');
    appendU32(b3dm, 1u);
    appendU32(
        b3dm,
        static_cast<uint32_t>(20 + batchBytes.size() + glb.size()));
    appendU32(b3dm, batchLength);
    appendU32(b3dm, static_cast<uint32_t>(batchBytes.size()));
    b3dm.insert(b3dm.end(), batchBytes.begin(), batchBytes.end());
    b3dm.insert(b3dm.end(), glb.begin(), glb.end());
    return b3dm;
}

std::vector<uint8_t> makeLegacyB3dmHeader2(
    std::vector<uint8_t> glb,
    uint32_t batchLength,
    const std::string& batchTableJson = std::string{},
    std::vector<uint8_t> batchTableBinary = {}) {
    std::vector<uint8_t> batchBytes(
        batchTableJson.begin(),
        batchTableJson.end());
    if (!batchBytes.empty()) {
        pad4(batchBytes, 0x20);
    }
    pad4(batchTableBinary, 0);

    std::vector<uint8_t> b3dm;
    b3dm.push_back('b');
    b3dm.push_back('3');
    b3dm.push_back('d');
    b3dm.push_back('m');
    appendU32(b3dm, 1u);
    appendU32(
        b3dm,
        static_cast<uint32_t>(
            24 + batchBytes.size() + batchTableBinary.size() + glb.size()));
    appendU32(b3dm, static_cast<uint32_t>(batchBytes.size()));
    appendU32(b3dm, static_cast<uint32_t>(batchTableBinary.size()));
    appendU32(b3dm, batchLength);
    b3dm.insert(b3dm.end(), batchBytes.begin(), batchBytes.end());
    b3dm.insert(b3dm.end(),
                batchTableBinary.begin(),
                batchTableBinary.end());
    b3dm.insert(b3dm.end(), glb.begin(), glb.end());
    return b3dm;
}

std::vector<uint8_t> makeI3dm(std::vector<uint8_t> gltfPayload,
                              uint32_t gltfFormat,
                              const std::string& gltfUri = std::string{}) {
    std::vector<uint8_t> featureBinary;
    const size_t positionsOffset = featureBinary.size();
    appendF32(featureBinary, 0.0f);
    appendF32(featureBinary, 0.0f);
    appendF32(featureBinary, 0.0f);
    appendF32(featureBinary, 10.0f);
    appendF32(featureBinary, 0.0f);
    appendF32(featureBinary, 0.0f);

    const size_t scalesOffset = featureBinary.size();
    appendF32(featureBinary, 1.0f);
    appendF32(featureBinary, 2.0f);
    pad4(featureBinary, 0);

    std::string featureJson =
        std::string("{") +
        "\"INSTANCES_LENGTH\":2," +
        "\"RTC_CENTER\":[100.0,200.0,300.0]," +
        "\"POSITION\":{\"byteOffset\":" +
        std::to_string(positionsOffset) + "}," +
        "\"SCALE\":{\"byteOffset\":" +
        std::to_string(scalesOffset) + "}" +
        "}";
    std::vector<uint8_t> featureBytes(featureJson.begin(), featureJson.end());
    pad4(featureBytes, 0x20);

    std::vector<uint8_t> payload;
    if (gltfFormat == 0) {
        payload.assign(gltfUri.begin(), gltfUri.end());
        pad4(payload, 0x20);
    } else {
        payload = std::move(gltfPayload);
    }

    std::vector<uint8_t> i3dm;
    i3dm.push_back('i');
    i3dm.push_back('3');
    i3dm.push_back('d');
    i3dm.push_back('m');
    appendU32(i3dm, 1u);
    appendU32(
        i3dm,
        static_cast<uint32_t>(
            32 + featureBytes.size() + featureBinary.size() + payload.size()));
    appendU32(i3dm, static_cast<uint32_t>(featureBytes.size()));
    appendU32(i3dm, static_cast<uint32_t>(featureBinary.size()));
    appendU32(i3dm, 0u);
    appendU32(i3dm, 0u);
    appendU32(i3dm, gltfFormat);
    i3dm.insert(i3dm.end(), featureBytes.begin(), featureBytes.end());
    i3dm.insert(i3dm.end(), featureBinary.begin(), featureBinary.end());
    i3dm.insert(i3dm.end(), payload.begin(), payload.end());
    return i3dm;
}

std::vector<uint8_t> makeI3dmWithFeatureTable(
    const std::string& featureJson,
    std::vector<uint8_t> featureBinary,
    const std::string& batchTableJson = std::string{},
    std::vector<uint8_t> batchTableBinary = {}) {
    std::vector<uint8_t> featureBytes(featureJson.begin(), featureJson.end());
    pad4(featureBytes, 0x20);
    pad4(featureBinary, 0);
    std::vector<uint8_t> batchBytes(
        batchTableJson.begin(),
        batchTableJson.end());
    if (!batchBytes.empty()) {
        pad4(batchBytes, 0x20);
    }
    pad4(batchTableBinary, 0);

    const std::vector<uint8_t> payload = makeTriangleGlb();

    std::vector<uint8_t> i3dm;
    i3dm.push_back('i');
    i3dm.push_back('3');
    i3dm.push_back('d');
    i3dm.push_back('m');
    appendU32(i3dm, 1u);
    appendU32(
        i3dm,
        static_cast<uint32_t>(
            32 + featureBytes.size() + featureBinary.size() +
            batchBytes.size() + batchTableBinary.size() + payload.size()));
    appendU32(i3dm, static_cast<uint32_t>(featureBytes.size()));
    appendU32(i3dm, static_cast<uint32_t>(featureBinary.size()));
    appendU32(i3dm, static_cast<uint32_t>(batchBytes.size()));
    appendU32(i3dm, static_cast<uint32_t>(batchTableBinary.size()));
    appendU32(i3dm, 1u);
    i3dm.insert(i3dm.end(), featureBytes.begin(), featureBytes.end());
    i3dm.insert(i3dm.end(), featureBinary.begin(), featureBinary.end());
    i3dm.insert(i3dm.end(), batchBytes.begin(), batchBytes.end());
    i3dm.insert(i3dm.end(),
                batchTableBinary.begin(),
                batchTableBinary.end());
    i3dm.insert(i3dm.end(), payload.begin(), payload.end());
    return i3dm;
}

std::vector<uint8_t> makeExternalI3dmWithFeatureTable(
    const std::string& featureJson,
    std::vector<uint8_t> featureBinary,
    const std::string& gltfUri,
    const std::string& batchTableJson = std::string{}) {
    std::vector<uint8_t> featureBytes(featureJson.begin(), featureJson.end());
    pad4(featureBytes, 0x20);
    pad4(featureBinary, 0);
    std::vector<uint8_t> batchBytes(
        batchTableJson.begin(),
        batchTableJson.end());
    if (!batchBytes.empty()) {
        pad4(batchBytes, 0x20);
    }
    std::vector<uint8_t> payload(gltfUri.begin(), gltfUri.end());
    pad4(payload, 0x20);

    std::vector<uint8_t> i3dm;
    i3dm.push_back('i');
    i3dm.push_back('3');
    i3dm.push_back('d');
    i3dm.push_back('m');
    appendU32(i3dm, 1u);
    appendU32(
        i3dm,
        static_cast<uint32_t>(
            32 + featureBytes.size() + featureBinary.size() +
            batchBytes.size() + payload.size()));
    appendU32(i3dm, static_cast<uint32_t>(featureBytes.size()));
    appendU32(i3dm, static_cast<uint32_t>(featureBinary.size()));
    appendU32(i3dm, static_cast<uint32_t>(batchBytes.size()));
    appendU32(i3dm, 0u);
    appendU32(i3dm, 0u);
    i3dm.insert(i3dm.end(), featureBytes.begin(), featureBytes.end());
    i3dm.insert(i3dm.end(), featureBinary.begin(), featureBinary.end());
    i3dm.insert(i3dm.end(), batchBytes.begin(), batchBytes.end());
    i3dm.insert(i3dm.end(), payload.begin(), payload.end());
    return i3dm;
}

std::vector<uint8_t> makePnts(const std::string& featureTableJson,
                              std::vector<uint8_t> featureBinary,
                              const std::string& batchTableJson =
                                  std::string{},
                              std::vector<uint8_t> batchTableBinary = {}) {
    std::vector<uint8_t> featureBytes(
        featureTableJson.begin(),
        featureTableJson.end());
    pad4(featureBytes, 0x20);
    pad4(featureBinary, 0);

    std::vector<uint8_t> batchBytes(
        batchTableJson.begin(),
        batchTableJson.end());
    if (!batchBytes.empty()) {
        pad4(batchBytes, 0x20);
    }
    pad4(batchTableBinary, 0);

    std::vector<uint8_t> pnts;
    pnts.push_back('p');
    pnts.push_back('n');
    pnts.push_back('t');
    pnts.push_back('s');
    appendU32(pnts, 1u);
    appendU32(
        pnts,
        static_cast<uint32_t>(
            28 + featureBytes.size() + featureBinary.size() +
            batchBytes.size() + batchTableBinary.size()));
    appendU32(pnts, static_cast<uint32_t>(featureBytes.size()));
    appendU32(pnts, static_cast<uint32_t>(featureBinary.size()));
    appendU32(pnts, static_cast<uint32_t>(batchBytes.size()));
    appendU32(pnts, static_cast<uint32_t>(batchTableBinary.size()));
    pnts.insert(pnts.end(), featureBytes.begin(), featureBytes.end());
    pnts.insert(pnts.end(), featureBinary.begin(), featureBinary.end());
    pnts.insert(pnts.end(), batchBytes.begin(), batchBytes.end());
    pnts.insert(pnts.end(),
                batchTableBinary.begin(),
                batchTableBinary.end());
    return pnts;
}

std::vector<uint8_t> makeCmpt(
    const std::vector<std::vector<uint8_t>>& innerTiles) {
    size_t byteLength = 16u;
    for (const std::vector<uint8_t>& inner : innerTiles) {
        byteLength += inner.size();
    }

    std::vector<uint8_t> cmpt;
    cmpt.push_back('c');
    cmpt.push_back('m');
    cmpt.push_back('p');
    cmpt.push_back('t');
    appendU32(cmpt, 1u);
    appendU32(cmpt, static_cast<uint32_t>(byteLength));
    appendU32(cmpt, static_cast<uint32_t>(innerTiles.size()));
    for (const std::vector<uint8_t>& inner : innerTiles) {
        cmpt.insert(cmpt.end(), inner.begin(), inner.end());
    }
    return cmpt;
}

TileContentLoadStatus decodeI3dmStatus(const std::vector<uint8_t>& i3dm) {
    SingleGltfContentProvider provider(
        TileKey{"Geographic-TMS", 0, 0, 0},
        std::vector<uint8_t>{},
        "I3DM fixture");
    return provider.decodeContent(i3dm.data(), i3dm.size()).status;
}

void writeBytes(const std::filesystem::path& path,
                const std::vector<uint8_t>& bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
}

std::vector<uint8_t> readFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return std::vector<uint8_t>(
        std::istreambuf_iterator<char>(in),
        std::istreambuf_iterator<char>());
}

std::vector<uint8_t> readFile(const char* path) {
    return readFile(std::filesystem::path(path));
}

std::vector<std::string> pathListFromEnv(const char* envName) {
    const char* value = std::getenv(envName);
    if (!value || std::string(value).empty()) {
        return {};
    }

#ifdef _WIN32
    constexpr char kPathSeparator = ';';
#else
    constexpr char kPathSeparator = ':';
#endif

    std::vector<std::string> paths;
    const std::string pathList(value);
    size_t start = 0;
    while (start <= pathList.size()) {
        const size_t end = pathList.find(kPathSeparator, start);
        std::string path = pathList.substr(
            start,
            end == std::string::npos ? std::string::npos : end - start);
        if (!path.empty()) {
            paths.push_back(std::move(path));
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1u;
    }
    return paths;
}

std::string stripUriQueryAndFragment(std::string uri) {
    const size_t end = uri.find_first_of("?#");
    if (end != std::string::npos) {
        uri.erase(end);
    }
    return uri;
}

GltfParser::ExternalResourceResolver localGltfResourceResolver(
    const std::filesystem::path& gltfPath) {
    const std::filesystem::path baseDirectory =
        std::filesystem::absolute(gltfPath).parent_path();
    return [baseDirectory](const std::string& uri) {
        constexpr const char* kFilePrefix = "file://";
        std::string resourceUri = stripUriQueryAndFragment(uri);
        if (resourceUri.rfind(kFilePrefix, 0) == 0) {
            resourceUri.erase(0, std::string(kFilePrefix).size());
        }
        std::filesystem::path resourcePath(resourceUri);
        if (resourcePath.is_relative()) {
            resourcePath = baseDirectory / resourcePath;
        }
        return readFile(resourcePath);
    };
}

GltfParser::ImageDecoder solidGltfImageDecoder(bool* decodedImage) {
    return [decodedImage](
               const uint8_t* data,
               size_t size) -> std::optional<GltfImage> {
        EXPECT_NE(nullptr, data);
        EXPECT_GT(size, 0u);
        if (decodedImage) {
            *decodedImage = true;
        }
        GltfImage image;
        image.width = 1;
        image.height = 1;
        image.channels = 4;
        image.pixels = {255, 255, 255, 255};
        return image;
    };
}

std::unique_ptr<DecodedImage> makeSolidDecodedImage(
    bool* decodedImage,
    const uint8_t* data,
    size_t size) {
    EXPECT_NE(nullptr, data);
    EXPECT_GT(size, 0u);
    if (decodedImage) {
        *decodedImage = true;
    }
    auto image = std::make_unique<DecodedImage>();
    image->width = 1;
    image->height = 1;
    image->channels = 4;
    image->pixels = {255, 255, 255, 255};
    return image;
}

void expectRenderableModelGeometry(const GltfModel& model) {
    EXPECT_GT(model.primitives.size(), 0u);
    EXPECT_GT(model.vertexCount(), 0u);
    const bool hasIndexedPrimitive = std::any_of(
        model.primitives.begin(),
        model.primitives.end(),
        [](const GltfPrimitive& primitive) {
            return !primitive.vertices.empty() && !primitive.indices.empty();
        });
    EXPECT_TRUE(hasIndexedPrimitive);
    for (const GltfPrimitive& primitive : model.primitives) {
        EXPECT_FALSE(primitive.vertices.empty());
        for (uint32_t index : primitive.indices) {
            EXPECT_LT(index, primitive.vertices.size());
        }
    }
}

std::vector<uint8_t> bytesFromString(const std::string& text) {
    return std::vector<uint8_t>(text.begin(), text.end());
}

std::string makeTilesetJsonWithRootContentObject(
    const std::string& contentObjectJson,
    const std::string& topLevelFields = std::string{},
    const std::string& rootFields = std::string{}) {
    return std::string("{") +
        "\"asset\":{\"version\":\"1.0\",\"gltfUpAxis\":\"Z\"}," +
        topLevelFields +
        "\"geometricError\":1000.0,"
        "\"root\":{"
        "\"boundingVolume\":{\"box\":[0,0,0,1,0,0,0,1,0,0,0,1]},"
        "\"geometricError\":0.0,"
        "\"refine\":\"ADD\"," +
        rootFields +
        "\"content\":" + contentObjectJson + "}}";
}

std::string makeTilesetJsonWithRootContent(
    const std::string& contentUri,
    const std::string& topLevelFields = std::string{}) {
    return makeTilesetJsonWithRootContentObject(
        "{\"uri\":\"" + contentUri + "\"}",
        topLevelFields);
}

TileContentLoadResult requestTileContentBlocking(
    TilesetContentProvider& provider,
    const TileKey& key) {
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
    TileContentLoadResult result;
    provider.requestTileContent(
        key,
        CancellationToken{},
        [&](const TileKey&, TileContentLoadResult loaded) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                result = std::move(loaded);
                done = true;
            }
            cv.notify_one();
        });
    {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait_for(lock, std::chrono::seconds(5), [&] { return done; });
    }
    EXPECT_TRUE(done);
    return result;
}

struct UnsupportedExternalGltfCase {
    const char* env;
    const char* label;
};

} // namespace

TEST(GltfParserTest, ParsesTriangleGlbWithNodeTransform) {
    const std::vector<uint8_t> glb = makeTriangleGlb();
    std::unique_ptr<GltfModel> model = GltfParser::parse(glb.data(), glb.size());

    ASSERT_NE(nullptr, model);
    ASSERT_EQ(1u, model->primitives.size());
    EXPECT_EQ(3u, model->vertexCount());
    EXPECT_EQ(3u, model->indexCount());
    EXPECT_EQ(3u, model->primitives[0].indices.size());
    EXPECT_EQ(1u, model->primitives[0].indices[1]);

    const SurfaceVertex& first = model->primitives[0].vertices[0];
    EXPECT_NEAR(10.0, first.positionEcef.x(), 1e-12);
    EXPECT_NEAR(20.0, first.positionEcef.y(), 1e-12);
    EXPECT_NEAR(30.0, first.positionEcef.z(), 1e-12);
    EXPECT_NEAR(0.0, first.normalEcef.x(), 1e-12);
    EXPECT_NEAR(0.0, first.normalEcef.y(), 1e-12);
    EXPECT_NEAR(1.0, first.normalEcef.z(), 1e-12);
    EXPECT_NEAR(0.0f, first.uv[0], 1e-6f);
    EXPECT_NEAR(0.0f, first.uv[1], 1e-6f);
    EXPECT_TRUE(model->primitives[0].vertexColors.empty());
    EXPECT_GT(model->byteSize(), 0);
}

TEST(GltfParserTest, RejectsPositionAccessorWithNonFiniteFloat) {
    const std::vector<uint8_t> glb =
        makeTriangleGlbWithNonFiniteAttribute(
            NonFiniteTriangleAttribute::Position);
    std::unique_ptr<GltfModel> model =
        GltfParser::parse(glb.data(), glb.size());

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsNormalAccessorWithNonFiniteFloat) {
    const std::vector<uint8_t> glb =
        makeTriangleGlbWithNonFiniteAttribute(
            NonFiniteTriangleAttribute::Normal);
    std::unique_ptr<GltfModel> model =
        GltfParser::parse(glb.data(), glb.size());

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsTexcoordAccessorWithNonFiniteFloat) {
    const std::vector<uint8_t> glb =
        makeTriangleGlbWithNonFiniteAttribute(
            NonFiniteTriangleAttribute::TexCoord);
    std::unique_ptr<GltfModel> model =
        GltfParser::parse(glb.data(), glb.size());

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsColorAccessorWithNonFiniteFloat) {
    const std::vector<uint8_t> glb =
        makeTriangleGlbWithNonFiniteAttribute(
            NonFiniteTriangleAttribute::Color);
    std::unique_ptr<GltfModel> model =
        GltfParser::parse(glb.data(), glb.size());

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsTopLevelArrayJson) {
    const std::string jsonText = "[]";

    std::unique_ptr<GltfModel> model = GltfParser::parse(
        reinterpret_cast<const uint8_t*>(jsonText.data()),
        jsonText.size());

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsMissingAssetObject) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string marker = "\"asset\":{\"version\":\"2.0\"},";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.erase(markerPos, marker.size());

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsAssetVersionTypeMismatch) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string marker = "\"version\":\"2.0\"";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.replace(markerPos, marker.size(), "\"version\":2.0");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsUnsupportedAssetVersion) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string marker = "\"version\":\"2.0\"";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.replace(markerPos, marker.size(), "\"version\":\"1.0\"");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsAssetMinVersionGreaterThanVersion) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string marker = "\"asset\":{\"version\":\"2.0\"}";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.replace(
        markerPos,
        marker.size(),
        "\"asset\":{\"version\":\"2.0\",\"minVersion\":\"2.1\"}");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsGlbWithBinaryChunkAndFirstBufferUri) {
    auto chunks = triangleGlbJsonAndBinChunks();
    std::string jsonText(chunks.first.begin(), chunks.first.end());
    const std::string marker = "\"buffers\":[{";
    const size_t markerPos = jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    jsonText.insert(markerPos + marker.size(), "\"uri\":\"external.bin\",");

    std::vector<uint8_t> jsonBytes(jsonText.begin(), jsonText.end());
    pad4(jsonBytes, 0x20);
    const std::vector<uint8_t> glb = makeGlbFromChunks({
        {0x4E4F534Au, jsonBytes},
        {0x004E4942u, chunks.second}});

    bool resolvedExternalBuffer = false;
    std::unique_ptr<GltfModel> model = GltfParser::parse(
        glb.data(),
        glb.size(),
        [&](const std::string& uri) {
            resolvedExternalBuffer = true;
            return uri == "external.bin" ? chunks.second
                                         : std::vector<uint8_t>{};
        });

    EXPECT_EQ(nullptr, model);
    EXPECT_FALSE(resolvedExternalBuffer);
}

TEST(GltfParserTest, RejectsGlbWhenFirstChunkIsNotJson) {
    auto chunks = triangleGlbJsonAndBinChunks();
    const std::vector<uint8_t> glb = makeGlbFromChunks({
        {0x004E4942u, chunks.second},
        {0x4E4F534Au, chunks.first}});

    std::unique_ptr<GltfModel> model =
        GltfParser::parse(glb.data(), glb.size());

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, ParsesGlbWithSpecPaddingAfterBinaryBuffer) {
    auto chunks = triangleGlbJsonAndBinChunks();
    std::string jsonText(chunks.first.begin(), chunks.first.end());
    const std::string marker = "\"buffers\":[{\"byteLength\":";
    const size_t markerPos = jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    const size_t lengthStart = markerPos + marker.size();
    const size_t lengthEnd = jsonText.find('}', lengthStart);
    ASSERT_NE(std::string::npos, lengthEnd);
    jsonText.replace(
        lengthStart,
        lengthEnd - lengthStart,
        std::to_string(chunks.second.size() + 1u));

    std::vector<uint8_t> jsonBytes(jsonText.begin(), jsonText.end());
    pad4(jsonBytes, 0x20);
    chunks.second.insert(chunks.second.end(), {0u, 0u, 0u, 0u});
    const std::vector<uint8_t> glb = makeGlbFromChunks({
        {0x4E4F534Au, jsonBytes},
        {0x004E4942u, chunks.second}});

    std::unique_ptr<GltfModel> model =
        GltfParser::parse(glb.data(), glb.size());

    ASSERT_NE(nullptr, model);
    EXPECT_EQ(3u, model->vertexCount());
    EXPECT_EQ(3u, model->indexCount());
}

TEST(GltfParserTest, RejectsGlbWithTooMuchBinaryPadding) {
    auto chunks = triangleGlbJsonAndBinChunks();
    chunks.second.insert(chunks.second.end(), {0u, 0u, 0u, 0u});
    const std::vector<uint8_t> glb = makeGlbFromChunks({
        {0x4E4F534Au, chunks.first},
        {0x004E4942u, chunks.second}});

    std::unique_ptr<GltfModel> model =
        GltfParser::parse(glb.data(), glb.size());

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, ParsesIndexedTriangleStripModeAsTriangleList) {
    const std::vector<uint8_t> glb = makeQuadPrimitiveModeGlb(5);
    std::unique_ptr<GltfModel> model = GltfParser::parse(glb.data(), glb.size());

    ASSERT_NE(nullptr, model);
    ASSERT_EQ(1u, model->primitives.size());
    const std::vector<uint32_t> expected = {0, 1, 2, 1, 3, 2};
    EXPECT_EQ(expected, model->primitives[0].indices);
}

TEST(GltfParserTest, ParsesIndexedTriangleFanModeAsTriangleList) {
    const std::vector<uint8_t> glb = makeQuadPrimitiveModeGlb(6);
    std::unique_ptr<GltfModel> model = GltfParser::parse(glb.data(), glb.size());

    ASSERT_NE(nullptr, model);
    ASSERT_EQ(1u, model->primitives.size());
    const std::vector<uint32_t> expected = {0, 1, 2, 0, 2, 3};
    EXPECT_EQ(expected, model->primitives[0].indices);
}

TEST(GltfParserTest, ParsesNonIndexedTriangleStripModeAsTriangleList) {
    const std::vector<uint8_t> glb = makeQuadPrimitiveModeGlb(5, false);
    std::unique_ptr<GltfModel> model = GltfParser::parse(glb.data(), glb.size());

    ASSERT_NE(nullptr, model);
    ASSERT_EQ(1u, model->primitives.size());
    const std::vector<uint32_t> expected = {0, 1, 2, 1, 3, 2};
    EXPECT_EQ(expected, model->primitives[0].indices);
}

TEST(GltfParserTest, ParsesLinePrimitiveModeAsLines) {
    const std::vector<uint8_t> glb = makeQuadPrimitiveModeGlb(1);
    std::unique_ptr<GltfModel> model = GltfParser::parse(glb.data(), glb.size());

    ASSERT_NE(nullptr, model);
    ASSERT_EQ(1u, model->primitives.size());
    EXPECT_EQ(GltfPrimitiveMode::Lines, model->primitives[0].primitiveMode);
    const std::vector<uint32_t> expected = {0, 1, 2, 3};
    EXPECT_EQ(expected, model->primitives[0].indices);
}

TEST(GltfParserTest, ParsesLineLoopPrimitiveModeAsLines) {
    const std::vector<uint8_t> glb = makeQuadPrimitiveModeGlb(2);
    std::unique_ptr<GltfModel> model = GltfParser::parse(glb.data(), glb.size());

    ASSERT_NE(nullptr, model);
    ASSERT_EQ(1u, model->primitives.size());
    EXPECT_EQ(GltfPrimitiveMode::Lines, model->primitives[0].primitiveMode);
    const std::vector<uint32_t> expected = {0, 1, 1, 2, 2, 3, 3, 0};
    EXPECT_EQ(expected, model->primitives[0].indices);
}

TEST(GltfParserTest, ParsesNonIndexedLineLoopPrimitiveModeAsLines) {
    const std::vector<uint8_t> glb = makeQuadPrimitiveModeGlb(2, false);
    std::unique_ptr<GltfModel> model = GltfParser::parse(glb.data(), glb.size());

    ASSERT_NE(nullptr, model);
    ASSERT_EQ(1u, model->primitives.size());
    EXPECT_EQ(GltfPrimitiveMode::Lines, model->primitives[0].primitiveMode);
    const std::vector<uint32_t> expected = {0, 1, 1, 2, 2, 3, 3, 0};
    EXPECT_EQ(expected, model->primitives[0].indices);
}

TEST(GltfParserTest, ParsesNonIndexedLineStripPrimitiveModeAsLineStrip) {
    const std::vector<uint8_t> glb = makeQuadPrimitiveModeGlb(3, false);
    std::unique_ptr<GltfModel> model = GltfParser::parse(glb.data(), glb.size());

    ASSERT_NE(nullptr, model);
    ASSERT_EQ(1u, model->primitives.size());
    EXPECT_EQ(GltfPrimitiveMode::LineStrip, model->primitives[0].primitiveMode);
    const std::vector<uint32_t> expected = {0, 1, 2, 3};
    EXPECT_EQ(expected, model->primitives[0].indices);
}

TEST(GltfParserTest, ParsesPointPrimitiveModeWithoutFakeTriangles) {
    const std::vector<uint8_t> glb = makeQuadPrimitiveModeGlb(0);
    std::unique_ptr<GltfModel> model = GltfParser::parse(glb.data(), glb.size());

    ASSERT_NE(nullptr, model);
    ASSERT_EQ(1u, model->primitives.size());
    EXPECT_EQ(GltfPrimitiveMode::Points, model->primitives[0].primitiveMode);
    const std::vector<uint32_t> expected = {0, 1, 2, 3};
    EXPECT_EQ(expected, model->primitives[0].indices);
}

TEST(GltfParserTest, ParsesKhrMeshQuantizationAttributesWhenDeclared) {
    const std::vector<uint8_t> glb = makeQuantizedTriangleGlb(true);
    std::unique_ptr<GltfModel> model = GltfParser::parse(glb.data(), glb.size());

    ASSERT_NE(nullptr, model);
    ASSERT_EQ(1u, model->primitives.size());
    const GltfPrimitive& primitive = model->primitives[0];
    ASSERT_EQ(3u, primitive.vertices.size());
    EXPECT_NEAR(1.0, primitive.vertices[1].positionEcef.x(), 1e-6);
    EXPECT_NEAR(1.0, primitive.vertices[2].positionEcef.y(), 1e-6);
    EXPECT_NEAR(1.0, primitive.vertices[0].normalEcef.z(), 1e-6);
    ASSERT_EQ(3u, primitive.vertexTexCoords[0].size());
    EXPECT_NEAR(10.0f, primitive.vertexTexCoords[0][0][0], 1e-6f);
    EXPECT_NEAR(20.0f, primitive.vertexTexCoords[0][0][1], 1e-6f);
    ASSERT_EQ(3u, primitive.vertexTangents.size());
    EXPECT_NEAR(1.0f, primitive.vertexTangents[0][0], 1e-6f);
    EXPECT_NEAR(1.0f, primitive.vertexTangents[0][3], 1e-6f);
}

TEST(GltfParserTest, RejectsQuantizedMeshAttributesWithoutExtension) {
    const std::vector<uint8_t> glb = makeQuantizedTriangleGlb(false);
    std::unique_ptr<GltfModel> model = GltfParser::parse(glb.data(), glb.size());

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsKhrMeshQuantizationDeclarationWithoutUse) {
    const std::vector<uint8_t> glb =
        makeTriangleGlb(
            {1.0f, 1.0f, 1.0f, 1.0f},
            false,
            5126,
            false,
            0,
            false,
            0,
            false,
            std::string{},
            0,
            std::string{},
            false,
            1.0f,
            false,
            true);
    std::unique_ptr<GltfModel> model = GltfParser::parse(glb.data(), glb.size());

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsKhrMeshQuantizationDeclarationForCoreTexcoords) {
    const std::vector<uint8_t> glb =
        makeTriangleGlb(
            {1.0f, 1.0f, 1.0f, 1.0f},
            false,
            5121,
            true,
            0,
            false,
            0,
            false,
            std::string{},
            0,
            std::string{},
            false,
            1.0f,
            false,
            true);
    std::unique_ptr<GltfModel> model = GltfParser::parse(glb.data(), glb.size());

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, ParsesNormalizedUnsignedByteTexcoords) {
    const std::vector<uint8_t> glb =
        makeTriangleGlb(
            {1.0f, 1.0f, 1.0f, 1.0f},
            false,
            5121,
            true);
    std::unique_ptr<GltfModel> model =
        GltfParser::parse(glb.data(), glb.size());

    ASSERT_NE(nullptr, model);
    ASSERT_EQ(1u, model->primitives.size());
    ASSERT_EQ(3u, model->primitives[0].vertices.size());
    EXPECT_NEAR(0.0f, model->primitives[0].vertices[0].uv[0], 1e-6f);
    EXPECT_NEAR(0.0f, model->primitives[0].vertices[0].uv[1], 1e-6f);
    EXPECT_NEAR(1.0f, model->primitives[0].vertices[1].uv[0], 1e-6f);
    EXPECT_NEAR(0.0f, model->primitives[0].vertices[1].uv[1], 1e-6f);
    EXPECT_NEAR(0.0f, model->primitives[0].vertices[2].uv[0], 1e-6f);
    EXPECT_NEAR(1.0f, model->primitives[0].vertices[2].uv[1], 1e-6f);
}

TEST(GltfParserTest, RejectsUnnormalizedIntegerTexcoords) {
    const std::vector<uint8_t> glb =
        makeTriangleGlb(
            {1.0f, 1.0f, 1.0f, 1.0f},
            false,
            5121,
            false);
    std::unique_ptr<GltfModel> model =
        GltfParser::parse(glb.data(), glb.size());

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsNormalizedFloatAccessor) {
    const std::vector<uint8_t> glb =
        makeTriangleGlb(
            {1.0f, 1.0f, 1.0f, 1.0f},
            false,
            5126,
            true);
    std::unique_ptr<GltfModel> model =
        GltfParser::parse(glb.data(), glb.size());

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsInvalidAccessorByteStride) {
    const std::vector<uint8_t> glb =
        makeTriangleGlb(
            {1.0f, 1.0f, 1.0f, 1.0f},
            false,
            5121,
            true,
            3);
    std::unique_ptr<GltfModel> model =
        GltfParser::parse(glb.data(), glb.size());

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsAccessorByteStrideNotFourByteAligned) {
    const ExternalGltfFixture fixture =
        makeStridedUnsignedByteTexcoordExternalGltf(5);
    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsBufferViewsTypeMismatch) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    ASSERT_TRUE(replaceTopLevelJsonArray(fixture, "bufferViews", "{}"));

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsBufferViewElementTypeMismatch) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string marker =
        "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36}";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.replace(markerPos, marker.size(), "7");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsUnreferencedBufferViewRangeOutOfBounds) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string marker =
        "{\"buffer\":0,\"byteOffset\":96,\"byteLength\":6}";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.replace(
        markerPos,
        marker.size(),
        marker +
            ",{\"buffer\":0,\"byteOffset\":999,\"byteLength\":4}");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsUnreferencedBufferViewInvalidTarget) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string marker =
        "{\"buffer\":0,\"byteOffset\":96,\"byteLength\":6}";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.replace(
        markerPos,
        marker.size(),
        marker +
            ",{\"buffer\":0,\"byteOffset\":0,\"byteLength\":4,"
            "\"target\":1}");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsAccessorsTypeMismatch) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    ASSERT_TRUE(replaceTopLevelJsonArray(fixture, "accessors", "{}"));

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsAccessorElementTypeMismatch) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string marker =
        "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"}";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.replace(markerPos, marker.size(), "7");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsAccessorComponentTypeTypeMismatch) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string marker = "\"componentType\":5126";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.replace(
        markerPos,
        marker.size(),
        "\"componentType\":\"5126\"");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsAccessorTypeTypeMismatch) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string marker = "\"type\":\"VEC3\"";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.replace(markerPos, marker.size(), "\"type\":7");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsUnreferencedAccessorBufferViewOutOfRange) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    appendAccessorToExternalFixture(
        fixture,
        "{\"bufferView\":99,\"componentType\":5126,"
        "\"count\":1,\"type\":\"VEC3\"}");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsUnreferencedAccessorByteOffsetWithoutBufferView) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    appendAccessorToExternalFixture(
        fixture,
        "{\"componentType\":5126,\"count\":1,"
        "\"type\":\"VEC3\",\"byteOffset\":4}");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsUnreferencedAccessorMinLengthMismatch) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    appendAccessorToExternalFixture(
        fixture,
        "{\"bufferView\":0,\"componentType\":5126,"
        "\"count\":1,\"type\":\"VEC3\",\"min\":[0,0]}");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsUnreferencedAccessorMaxElementTypeMismatch) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    appendAccessorToExternalFixture(
        fixture,
        "{\"bufferView\":0,\"componentType\":5126,"
        "\"count\":1,\"type\":\"VEC3\",\"max\":[0,\"1\",0]}");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsUnreferencedAccessorFloatBoundsOutOfRange) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    appendAccessorToExternalFixture(
        fixture,
        "{\"bufferView\":0,\"componentType\":5126,"
        "\"count\":1,\"type\":\"VEC3\",\"min\":[1e39,0,0]}");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsUnreferencedAccessorIntegerBoundsOutOfRange) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    appendAccessorToExternalFixture(
        fixture,
        "{\"bufferView\":0,\"componentType\":5121,"
        "\"count\":1,\"type\":\"SCALAR\",\"max\":[256]}");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsUnreferencedAccessorZeroCount) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    appendAccessorToExternalFixture(
        fixture,
        "{\"componentType\":5126,\"count\":0,\"type\":\"SCALAR\"}");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, ParsesUnreferencedPaddedMatrixAccessor) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    appendAccessorToExternalFixture(
        fixture,
        "{\"bufferView\":0,\"componentType\":5121,"
        "\"count\":3,\"type\":\"MAT3\"}");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    ASSERT_NE(nullptr, model);
    EXPECT_EQ(1u, model->primitives.size());
}

TEST(GltfParserTest, RejectsUnreferencedMatrixAccessorWithoutColumnPadding) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const size_t matrixOffset = fixture.bin.size();
    fixture.bin.insert(fixture.bin.end(), 27u, 0u);

    const std::string oldBuffer =
        "\"buffers\":[{\"uri\":\"triangle.bin\",\"byteLength\":" +
        std::to_string(matrixOffset) + "}]";
    const std::string newBuffer =
        "\"buffers\":[{\"uri\":\"triangle.bin\",\"byteLength\":" +
        std::to_string(fixture.bin.size()) + "}]";
    const size_t bufferPos = fixture.jsonText.find(oldBuffer);
    ASSERT_NE(std::string::npos, bufferPos);
    fixture.jsonText.replace(bufferPos, oldBuffer.size(), newBuffer);

    const std::string lastBufferView =
        "{\"buffer\":0,\"byteOffset\":96,\"byteLength\":6}";
    const std::string matrixBufferView =
        lastBufferView + ",{\"buffer\":0,\"byteOffset\":" +
        std::to_string(matrixOffset) + ",\"byteLength\":27}";
    const size_t bufferViewPos = fixture.jsonText.find(lastBufferView);
    ASSERT_NE(std::string::npos, bufferViewPos);
    fixture.jsonText.replace(
        bufferViewPos,
        lastBufferView.size(),
        matrixBufferView);

    appendAccessorToExternalFixture(
        fixture,
        "{\"bufferView\":4,\"componentType\":5121,"
        "\"count\":3,\"type\":\"MAT3\"}");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsUnreferencedSparseAccessorIndexOutOfRange) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    appendAccessorToExternalFixture(
        fixture,
        "{\"componentType\":5126,\"count\":1,\"type\":\"VEC3\","
        "\"sparse\":{\"count\":1,"
        "\"indices\":{\"bufferView\":3,\"byteOffset\":4,"
        "\"componentType\":5123},"
        "\"values\":{\"bufferView\":0}}}");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsNormalizedIndexAccessor) {
    const std::vector<uint8_t> glb =
        makeTriangleGlb(
            {1.0f, 1.0f, 1.0f, 1.0f},
            false,
            5126,
            false,
            0,
            true);
    std::unique_ptr<GltfModel> model =
        GltfParser::parse(glb.data(), glb.size());

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, ParsesNormalizedUnsignedByteVertexColors) {
    const std::vector<uint8_t> glb =
        makeTriangleGlb(
            {1.0f, 1.0f, 1.0f, 1.0f},
            false,
            5126,
            false,
            0,
            false,
            5121,
            true,
            "VEC4");
    std::unique_ptr<GltfModel> model =
        GltfParser::parse(glb.data(), glb.size());

    ASSERT_NE(nullptr, model);
    ASSERT_EQ(1u, model->primitives.size());
    const GltfPrimitive& primitive = model->primitives[0];
    ASSERT_EQ(3u, primitive.vertexColors.size());
    EXPECT_NEAR(64.0f / 255.0f, primitive.vertexColors[0][0], 1e-6f);
    EXPECT_NEAR(128.0f / 255.0f, primitive.vertexColors[0][1], 1e-6f);
    EXPECT_NEAR(191.0f / 255.0f, primitive.vertexColors[0][2], 1e-6f);
    EXPECT_NEAR(1.0f, primitive.vertexColors[0][3], 1e-6f);
    EXPECT_NEAR(1.0f, primitive.vertexColors[1][0], 1e-6f);
    EXPECT_NEAR(0.0f, primitive.vertexColors[1][1], 1e-6f);
    EXPECT_NEAR(128.0f / 255.0f, primitive.vertexColors[1][2], 1e-6f);
    EXPECT_NEAR(128.0f / 255.0f, primitive.vertexColors[1][3], 1e-6f);
}

TEST(GltfParserTest, ParsesVec3VertexColorsWithOpaqueAlpha) {
    const std::vector<uint8_t> glb =
        makeTriangleGlb(
            {1.0f, 1.0f, 1.0f, 1.0f},
            false,
            5126,
            false,
            0,
            false,
            5121,
            true,
            "VEC3");
    std::unique_ptr<GltfModel> model =
        GltfParser::parse(glb.data(), glb.size());

    ASSERT_NE(nullptr, model);
    ASSERT_EQ(1u, model->primitives.size());
    ASSERT_EQ(3u, model->primitives[0].vertexColors.size());
    EXPECT_NEAR(64.0f / 255.0f, model->primitives[0].vertexColors[0][0], 1e-6f);
    EXPECT_NEAR(128.0f / 255.0f, model->primitives[0].vertexColors[0][1], 1e-6f);
    EXPECT_NEAR(191.0f / 255.0f, model->primitives[0].vertexColors[0][2], 1e-6f);
    EXPECT_NEAR(1.0f, model->primitives[0].vertexColors[0][3], 1e-6f);
}

TEST(GltfParserTest, RejectsUnnormalizedIntegerVertexColors) {
    const std::vector<uint8_t> glb =
        makeTriangleGlb(
            {1.0f, 1.0f, 1.0f, 1.0f},
            false,
            5126,
            false,
            0,
            false,
            5121,
            false,
            "VEC4");
    std::unique_ptr<GltfModel> model =
        GltfParser::parse(glb.data(), glb.size());

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsMatrixVertexColors) {
    const std::vector<uint8_t> glb =
        makeTriangleGlb(
            {1.0f, 1.0f, 1.0f, 1.0f},
            false,
            5126,
            false,
            0,
            false,
            5126,
            false,
            "MAT2");
    std::unique_ptr<GltfModel> model =
        GltfParser::parse(glb.data(), glb.size());

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, ParsesFloatTangents) {
    const std::vector<uint8_t> glb =
        makeTriangleGlb(
            {1.0f, 1.0f, 1.0f, 1.0f},
            false,
            5126,
            false,
            0,
            false,
            0,
            false,
            "",
            5126,
            "VEC4");
    std::unique_ptr<GltfModel> model =
        GltfParser::parse(glb.data(), glb.size());

    ASSERT_NE(nullptr, model);
    ASSERT_EQ(1u, model->primitives.size());
    const GltfPrimitive& primitive = model->primitives[0];
    ASSERT_EQ(3u, primitive.vertexTangents.size());
    EXPECT_NEAR(1.0f, primitive.vertexTangents[0][0], 1e-6f);
    EXPECT_NEAR(0.0f, primitive.vertexTangents[0][1], 1e-6f);
    EXPECT_NEAR(0.0f, primitive.vertexTangents[0][2], 1e-6f);
    EXPECT_NEAR(1.0f, primitive.vertexTangents[0][3], 1e-6f);
}

TEST(GltfParserTest, RejectsIntegerTangents) {
    const std::vector<uint8_t> glb =
        makeTriangleGlb(
            {1.0f, 1.0f, 1.0f, 1.0f},
            false,
            5126,
            false,
            0,
            false,
            0,
            false,
            "",
            5123,
            "VEC4");
    std::unique_ptr<GltfModel> model =
        GltfParser::parse(glb.data(), glb.size());

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsInvalidTangentType) {
    const std::vector<uint8_t> glb =
        makeTriangleGlb(
            {1.0f, 1.0f, 1.0f, 1.0f},
            false,
            5126,
            false,
            0,
            false,
            0,
            false,
            "",
            5126,
            "VEC3");
    std::unique_ptr<GltfModel> model =
        GltfParser::parse(glb.data(), glb.size());

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsZeroTangents) {
    const std::vector<uint8_t> glb =
        makeTriangleGlb(
            {1.0f, 1.0f, 1.0f, 1.0f},
            false,
            5126,
            false,
            0,
            false,
            0,
            false,
            "",
            5126,
            "VEC4",
            true);
    std::unique_ptr<GltfModel> model =
        GltfParser::parse(glb.data(), glb.size());

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsInvalidTangentHandedness) {
    const std::vector<uint8_t> glb =
        makeTriangleGlb(
            {1.0f, 1.0f, 1.0f, 1.0f},
            false,
            5126,
            false,
            0,
            false,
            0,
            false,
            "",
            5126,
            "VEC4",
            false,
            0.0f);
    std::unique_ptr<GltfModel> model =
        GltfParser::parse(glb.data(), glb.size());

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, ParsesSparsePositionAccessor) {
    const std::vector<uint8_t> glb = makeSparsePositionTriangleGlb();
    std::unique_ptr<GltfModel> model = GltfParser::parse(glb.data(), glb.size());

    ASSERT_NE(nullptr, model);
    ASSERT_EQ(1u, model->primitives.size());
    ASSERT_EQ(3u, model->primitives[0].vertices.size());
    EXPECT_NEAR(10.0, model->primitives[0].vertices[0].positionEcef.x(), 1e-12);
    EXPECT_NEAR(20.0, model->primitives[0].vertices[0].positionEcef.y(), 1e-12);
    EXPECT_NEAR(30.0, model->primitives[0].vertices[0].positionEcef.z(), 1e-12);
    EXPECT_NEAR(11.0, model->primitives[0].vertices[1].positionEcef.x(), 1e-12);
    EXPECT_NEAR(21.0, model->primitives[0].vertices[2].positionEcef.y(), 1e-12);
}

TEST(GltfParserTest, RejectsSparseAccessorIndexOutOfRange) {
    const std::vector<uint8_t> glb = makeSparsePositionTriangleGlb(3);
    std::unique_ptr<GltfModel> model = GltfParser::parse(glb.data(), glb.size());

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsSparseAccessorValuesMisalignedByteOffset) {
    const std::vector<uint8_t> glb = makeSparsePositionTriangleGlb(2, 2, 1);
    std::unique_ptr<GltfModel> model = GltfParser::parse(glb.data(), glb.size());

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsPrimitiveIndexOutOfRange) {
    const std::vector<uint8_t> glb = makeSparsePositionTriangleGlb(2, 3);
    std::unique_ptr<GltfModel> model = GltfParser::parse(glb.data(), glb.size());

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, ParsesMaterialBaseColorAndDoubleSided) {
    const std::vector<uint8_t> glb =
        makeTriangleGlb({0.25f, 0.5f, 0.75f, 0.4f}, true);
    std::unique_ptr<GltfModel> model =
        GltfParser::parse(glb.data(), glb.size());

    ASSERT_NE(nullptr, model);
    ASSERT_EQ(1u, model->primitives.size());
    EXPECT_FLOAT_EQ(0.25f, model->primitives[0].baseColorFactor[0]);
    EXPECT_FLOAT_EQ(0.5f, model->primitives[0].baseColorFactor[1]);
    EXPECT_FLOAT_EQ(0.75f, model->primitives[0].baseColorFactor[2]);
    EXPECT_FLOAT_EQ(0.4f, model->primitives[0].baseColorFactor[3]);
    EXPECT_TRUE(model->primitives[0].doubleSided);
    EXPECT_EQ(GltfAlphaMode::Opaque, model->primitives[0].alphaMode);
}

TEST(GltfParserTest, ParsesKhrMaterialsUnlitMaterialExtension) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string assetMarker = "\"asset\":{\"version\":\"2.0\"},";
    const size_t assetPos = fixture.jsonText.find(assetMarker);
    ASSERT_NE(std::string::npos, assetPos);
    fixture.jsonText.insert(
        assetPos + assetMarker.size(),
        "\"extensionsUsed\":[\"KHR_materials_unlit\"],"
        "\"extensionsRequired\":[\"KHR_materials_unlit\"],");

    const std::string primitiveMarker = "\"mode\":4}";
    const size_t primitivePos = fixture.jsonText.find(primitiveMarker);
    ASSERT_NE(std::string::npos, primitivePos);
    fixture.jsonText.replace(
        primitivePos,
        primitiveMarker.size(),
        "\"mode\":4,\"material\":0}");

    const std::string buffersMarker = "\"buffers\"";
    const size_t buffersPos = fixture.jsonText.find(buffersMarker);
    ASSERT_NE(std::string::npos, buffersPos);
    fixture.jsonText.insert(
        buffersPos,
        "\"materials\":[{\"extensions\":{\"KHR_materials_unlit\":{}},"
        "\"pbrMetallicRoughness\":{\"baseColorFactor\":[0.2,0.4,0.6,0.8],"
        "\"metallicFactor\":0.9,\"roughnessFactor\":0.1},"
        "\"emissiveFactor\":[1,0.5,0.25]}],");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    ASSERT_NE(nullptr, model);
    ASSERT_EQ(1u, model->primitives.size());
    const GltfPrimitive& primitive = model->primitives[0];
    EXPECT_TRUE(primitive.unlit);
    EXPECT_NEAR(0.2f, primitive.baseColorFactor[0], 1e-6f);
    EXPECT_NEAR(0.4f, primitive.baseColorFactor[1], 1e-6f);
    EXPECT_NEAR(0.6f, primitive.baseColorFactor[2], 1e-6f);
    EXPECT_NEAR(0.8f, primitive.baseColorFactor[3], 1e-6f);
}

TEST(GltfParserTest, ParsesKhrMaterialsEmissiveStrengthMaterialExtension) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string assetMarker = "\"asset\":{\"version\":\"2.0\"},";
    const size_t assetPos = fixture.jsonText.find(assetMarker);
    ASSERT_NE(std::string::npos, assetPos);
    fixture.jsonText.insert(
        assetPos + assetMarker.size(),
        "\"extensionsUsed\":[\"KHR_materials_emissive_strength\"],"
        "\"extensionsRequired\":[\"KHR_materials_emissive_strength\"],");

    const std::string primitiveMarker = "\"mode\":4}";
    const size_t primitivePos = fixture.jsonText.find(primitiveMarker);
    ASSERT_NE(std::string::npos, primitivePos);
    fixture.jsonText.replace(
        primitivePos,
        primitiveMarker.size(),
        "\"mode\":4,\"material\":0}");

    const std::string buffersMarker = "\"buffers\"";
    const size_t buffersPos = fixture.jsonText.find(buffersMarker);
    ASSERT_NE(std::string::npos, buffersPos);
    fixture.jsonText.insert(
        buffersPos,
        "\"materials\":[{\"extensions\":{"
        "\"KHR_materials_emissive_strength\":{\"emissiveStrength\":2.5}},"
        "\"emissiveFactor\":[0.1,0.2,0.3]}],");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    ASSERT_NE(nullptr, model);
    ASSERT_EQ(1u, model->primitives.size());
    const GltfPrimitive& primitive = model->primitives[0];
    EXPECT_NEAR(0.25f, primitive.emissiveFactor[0], 1e-6f);
    EXPECT_NEAR(0.5f, primitive.emissiveFactor[1], 1e-6f);
    EXPECT_NEAR(0.75f, primitive.emissiveFactor[2], 1e-6f);
}

TEST(GltfParserTest, ParsesKhrMaterialsEmissiveStrengthDefaultValue) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string assetMarker = "\"asset\":{\"version\":\"2.0\"},";
    const size_t assetPos = fixture.jsonText.find(assetMarker);
    ASSERT_NE(std::string::npos, assetPos);
    fixture.jsonText.insert(
        assetPos + assetMarker.size(),
        "\"extensionsUsed\":[\"KHR_materials_emissive_strength\"],"
        "\"extensionsRequired\":[\"KHR_materials_emissive_strength\"],");

    const std::string primitiveMarker = "\"mode\":4}";
    const size_t primitivePos = fixture.jsonText.find(primitiveMarker);
    ASSERT_NE(std::string::npos, primitivePos);
    fixture.jsonText.replace(
        primitivePos,
        primitiveMarker.size(),
        "\"mode\":4,\"material\":0}");

    const std::string buffersMarker = "\"buffers\"";
    const size_t buffersPos = fixture.jsonText.find(buffersMarker);
    ASSERT_NE(std::string::npos, buffersPos);
    fixture.jsonText.insert(
        buffersPos,
        "\"materials\":[{\"extensions\":{"
        "\"KHR_materials_emissive_strength\":{}},"
        "\"emissiveFactor\":[0.1,0.2,0.3]}],");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    ASSERT_NE(nullptr, model);
    ASSERT_EQ(1u, model->primitives.size());
    const GltfPrimitive& primitive = model->primitives[0];
    EXPECT_NEAR(0.1f, primitive.emissiveFactor[0], 1e-6f);
    EXPECT_NEAR(0.2f, primitive.emissiveFactor[1], 1e-6f);
    EXPECT_NEAR(0.3f, primitive.emissiveFactor[2], 1e-6f);
}

TEST(GltfParserTest, ParsesKhrMaterialsIorMaterialExtension) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string assetMarker = "\"asset\":{\"version\":\"2.0\"},";
    const size_t assetPos = fixture.jsonText.find(assetMarker);
    ASSERT_NE(std::string::npos, assetPos);
    fixture.jsonText.insert(
        assetPos + assetMarker.size(),
        "\"extensionsUsed\":[\"KHR_materials_ior\"],"
        "\"extensionsRequired\":[\"KHR_materials_ior\"],");

    const std::string primitiveMarker = "\"mode\":4}";
    const size_t primitivePos = fixture.jsonText.find(primitiveMarker);
    ASSERT_NE(std::string::npos, primitivePos);
    fixture.jsonText.replace(
        primitivePos,
        primitiveMarker.size(),
        "\"mode\":4,\"material\":0}");

    const std::string buffersMarker = "\"buffers\"";
    const size_t buffersPos = fixture.jsonText.find(buffersMarker);
    ASSERT_NE(std::string::npos, buffersPos);
    fixture.jsonText.insert(
        buffersPos,
        "\"materials\":[{\"extensions\":{"
        "\"KHR_materials_ior\":{\"ior\":2.0}}}],");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    ASSERT_NE(nullptr, model);
    ASSERT_EQ(1u, model->primitives.size());
    EXPECT_NEAR(1.0f / 9.0f, model->primitives[0].dielectricSpecularF0, 1e-6f);
}

TEST(GltfParserTest, ParsesKhrMaterialsIorZeroAsFullReflectance) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string assetMarker = "\"asset\":{\"version\":\"2.0\"},";
    const size_t assetPos = fixture.jsonText.find(assetMarker);
    ASSERT_NE(std::string::npos, assetPos);
    fixture.jsonText.insert(
        assetPos + assetMarker.size(),
        "\"extensionsUsed\":[\"KHR_materials_ior\"],"
        "\"extensionsRequired\":[\"KHR_materials_ior\"],");

    const std::string primitiveMarker = "\"mode\":4}";
    const size_t primitivePos = fixture.jsonText.find(primitiveMarker);
    ASSERT_NE(std::string::npos, primitivePos);
    fixture.jsonText.replace(
        primitivePos,
        primitiveMarker.size(),
        "\"mode\":4,\"material\":0}");

    const std::string buffersMarker = "\"buffers\"";
    const size_t buffersPos = fixture.jsonText.find(buffersMarker);
    ASSERT_NE(std::string::npos, buffersPos);
    fixture.jsonText.insert(
        buffersPos,
        "\"materials\":[{\"extensions\":{"
        "\"KHR_materials_ior\":{\"ior\":0.0}}}],");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    ASSERT_NE(nullptr, model);
    ASSERT_EQ(1u, model->primitives.size());
    EXPECT_NEAR(1.0f, model->primitives[0].dielectricSpecularF0, 1e-6f);
}

TEST(GltfParserTest, ParsesKhrMaterialsIorDefaultValue) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string assetMarker = "\"asset\":{\"version\":\"2.0\"},";
    const size_t assetPos = fixture.jsonText.find(assetMarker);
    ASSERT_NE(std::string::npos, assetPos);
    fixture.jsonText.insert(
        assetPos + assetMarker.size(),
        "\"extensionsUsed\":[\"KHR_materials_ior\"],"
        "\"extensionsRequired\":[\"KHR_materials_ior\"],");

    const std::string primitiveMarker = "\"mode\":4}";
    const size_t primitivePos = fixture.jsonText.find(primitiveMarker);
    ASSERT_NE(std::string::npos, primitivePos);
    fixture.jsonText.replace(
        primitivePos,
        primitiveMarker.size(),
        "\"mode\":4,\"material\":0}");

    const std::string buffersMarker = "\"buffers\"";
    const size_t buffersPos = fixture.jsonText.find(buffersMarker);
    ASSERT_NE(std::string::npos, buffersPos);
    fixture.jsonText.insert(
        buffersPos,
        "\"materials\":[{\"extensions\":{\"KHR_materials_ior\":{}}}],");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    ASSERT_NE(nullptr, model);
    ASSERT_EQ(1u, model->primitives.size());
    EXPECT_NEAR(0.04f, model->primitives[0].dielectricSpecularF0, 1e-6f);
}

TEST(GltfParserTest, ParsesKhrMaterialsSpecularMaterialExtension) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string assetMarker = "\"asset\":{\"version\":\"2.0\"},";
    const size_t assetPos = fixture.jsonText.find(assetMarker);
    ASSERT_NE(std::string::npos, assetPos);
    fixture.jsonText.insert(
        assetPos + assetMarker.size(),
        "\"extensionsUsed\":[\"KHR_materials_specular\"],"
        "\"extensionsRequired\":[\"KHR_materials_specular\"],");

    const std::string primitiveMarker = "\"mode\":4}";
    const size_t primitivePos = fixture.jsonText.find(primitiveMarker);
    ASSERT_NE(std::string::npos, primitivePos);
    fixture.jsonText.replace(
        primitivePos,
        primitiveMarker.size(),
        "\"mode\":4,\"material\":0}");

    const std::string buffersMarker = "\"buffers\"";
    const size_t buffersPos = fixture.jsonText.find(buffersMarker);
    ASSERT_NE(std::string::npos, buffersPos);
    fixture.jsonText.insert(
        buffersPos,
        "\"materials\":[{\"extensions\":{\"KHR_materials_specular\":{"
        "\"specularFactor\":0.25,"
        "\"specularColorFactor\":[2.0,0.5,0.25]}}}],");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    ASSERT_NE(nullptr, model);
    ASSERT_EQ(1u, model->primitives.size());
    const GltfPrimitive& primitive = model->primitives[0];
    EXPECT_NEAR(0.25f, primitive.specularFactor, 1e-6f);
    EXPECT_NEAR(2.0f, primitive.specularColorFactor[0], 1e-6f);
    EXPECT_NEAR(0.5f, primitive.specularColorFactor[1], 1e-6f);
    EXPECT_NEAR(0.25f, primitive.specularColorFactor[2], 1e-6f);
}

TEST(GltfParserTest, ParsesKhrMaterialsSpecularTextureBindings) {
    ExternalGltfFixture fixture =
        makeTexturedExternalBufferTriangleGltf("image.bin");
    const std::string assetMarker = "\"asset\":{\"version\":\"2.0\"},";
    const size_t assetPos = fixture.jsonText.find(assetMarker);
    ASSERT_NE(std::string::npos, assetPos);
    fixture.jsonText.insert(
        assetPos + assetMarker.size(),
        "\"extensionsUsed\":["
        "\"KHR_materials_specular\","
        "\"KHR_texture_transform\"],"
        "\"extensionsRequired\":["
        "\"KHR_materials_specular\","
        "\"KHR_texture_transform\"],");

    const std::string materialMarker = "\"materials\":[{";
    const size_t materialPos = fixture.jsonText.find(materialMarker);
    ASSERT_NE(std::string::npos, materialPos);
    fixture.jsonText.replace(
        materialPos,
        materialMarker.size(),
        "\"materials\":[{\"extensions\":{\"KHR_materials_specular\":{"
        "\"specularTexture\":{\"index\":0,\"texCoord\":0,"
        "\"extensions\":{\"KHR_texture_transform\":{"
        "\"offset\":[0.25,0.5],\"scale\":[2,3],"
        "\"rotation\":1.57079632679}}},"
        "\"specularColorTexture\":{\"index\":0,\"texCoord\":0}}},");

    std::unique_ptr<GltfModel> model =
        parseExternalFixtureWithSolidImage(fixture);

    ASSERT_NE(nullptr, model);
    ASSERT_EQ(1u, model->primitives.size());
    const GltfPrimitive& primitive = model->primitives[0];
    ASSERT_TRUE(primitive.specularTexture);
    ASSERT_TRUE(primitive.specularColorTexture);
    EXPECT_EQ(0u, primitive.specularTexture->textureIndex);
    EXPECT_EQ(0, primitive.specularTexture->texCoord);
    EXPECT_NEAR(0.25f, primitive.specularTexture->transform.offset[0], 1e-6f);
    EXPECT_NEAR(0.5f, primitive.specularTexture->transform.offset[1], 1e-6f);
    EXPECT_NEAR(2.0f, primitive.specularTexture->transform.scale[0], 1e-6f);
    EXPECT_NEAR(3.0f, primitive.specularTexture->transform.scale[1], 1e-6f);
    EXPECT_NEAR(
        1.57079632679f,
        primitive.specularTexture->transform.rotation,
        1e-6f);
}

TEST(GltfParserTest, ParsesKhrMaterialsPbrSpecularGlossinessMaterialExtension) {
    ExternalGltfFixture fixture =
        makeTexturedExternalBufferTriangleGltf("image.bin");
    const std::string assetMarker = "\"asset\":{\"version\":\"2.0\"},";
    const size_t assetPos = fixture.jsonText.find(assetMarker);
    ASSERT_NE(std::string::npos, assetPos);
    fixture.jsonText.insert(
        assetPos + assetMarker.size(),
        "\"extensionsUsed\":["
        "\"KHR_materials_pbrSpecularGlossiness\","
        "\"KHR_texture_transform\"],"
        "\"extensionsRequired\":["
        "\"KHR_materials_pbrSpecularGlossiness\","
        "\"KHR_texture_transform\"],");

    const size_t materialPos = fixture.jsonText.find("\"materials\":[");
    ASSERT_NE(std::string::npos, materialPos);
    const size_t texturesPos = fixture.jsonText.find(",\"textures\"", materialPos);
    ASSERT_NE(std::string::npos, texturesPos);
    fixture.jsonText.replace(
        materialPos,
        texturesPos - materialPos,
        "\"materials\":[{\"doubleSided\":true,\"alphaMode\":\"BLEND\","
        "\"alphaCutoff\":0.25,"
        "\"pbrMetallicRoughness\":{\"baseColorFactor\":[1.0,0.0,0.0,1.0]},"
        "\"extensions\":{\"KHR_materials_pbrSpecularGlossiness\":{"
        "\"diffuseFactor\":[0.2,0.3,0.4,0.5],"
        "\"specularFactor\":[0.6,0.7,0.8],"
        "\"glossinessFactor\":0.9,"
        "\"diffuseTexture\":{\"index\":0,\"texCoord\":0,"
        "\"extensions\":{\"KHR_texture_transform\":{"
        "\"offset\":[0.375,0.625],\"scale\":[0.25,0.5],"
        "\"rotation\":0.78539816339}}},"
        "\"specularGlossinessTexture\":{\"index\":0,\"texCoord\":0,"
        "\"extensions\":{\"KHR_texture_transform\":{"
        "\"offset\":[0.125,0.25],\"scale\":[0.5,0.75],"
        "\"rotation\":1.57079632679}}}}}}]");

    std::unique_ptr<GltfModel> model =
        parseExternalFixtureWithSolidImage(fixture);

    ASSERT_NE(nullptr, model);
    ASSERT_EQ(1u, model->primitives.size());
    const GltfPrimitive& primitive = model->primitives[0];
    EXPECT_TRUE(primitive.specularGlossinessWorkflow);
    EXPECT_FLOAT_EQ(0.0f, primitive.metallicFactor);
    EXPECT_FLOAT_EQ(0.0f, primitive.roughnessFactor);
    EXPECT_NEAR(0.2f, primitive.baseColorFactor[0], 1e-6f);
    EXPECT_NEAR(0.3f, primitive.baseColorFactor[1], 1e-6f);
    EXPECT_NEAR(0.4f, primitive.baseColorFactor[2], 1e-6f);
    EXPECT_NEAR(0.5f, primitive.baseColorFactor[3], 1e-6f);
    ASSERT_TRUE(primitive.baseColorTexture);
    EXPECT_EQ(0u, primitive.baseColorTexture->textureIndex);
    EXPECT_NEAR(
        0.375f,
        primitive.baseColorTexture->transform.offset[0],
        1e-6f);
    EXPECT_NEAR(
        0.625f,
        primitive.baseColorTexture->transform.offset[1],
        1e-6f);
    EXPECT_NEAR(
        0.25f,
        primitive.baseColorTexture->transform.scale[0],
        1e-6f);
    EXPECT_NEAR(
        0.5f,
        primitive.baseColorTexture->transform.scale[1],
        1e-6f);
    EXPECT_NEAR(
        0.78539816339f,
        primitive.baseColorTexture->transform.rotation,
        1e-6f);
    EXPECT_NEAR(
        0.6f,
        primitive.specularGlossinessSpecularFactor[0],
        1e-6f);
    EXPECT_NEAR(
        0.7f,
        primitive.specularGlossinessSpecularFactor[1],
        1e-6f);
    EXPECT_NEAR(
        0.8f,
        primitive.specularGlossinessSpecularFactor[2],
        1e-6f);
    EXPECT_NEAR(
        0.9f,
        primitive.specularGlossinessGlossinessFactor,
        1e-6f);
    ASSERT_TRUE(primitive.specularGlossinessTexture);
    EXPECT_EQ(0u, primitive.specularGlossinessTexture->textureIndex);
    EXPECT_NEAR(
        0.125f,
        primitive.specularGlossinessTexture->transform.offset[0],
        1e-6f);
    EXPECT_NEAR(
        0.25f,
        primitive.specularGlossinessTexture->transform.offset[1],
        1e-6f);
    EXPECT_NEAR(
        0.5f,
        primitive.specularGlossinessTexture->transform.scale[0],
        1e-6f);
    EXPECT_NEAR(
        0.75f,
        primitive.specularGlossinessTexture->transform.scale[1],
        1e-6f);
    EXPECT_NEAR(
        1.57079632679f,
        primitive.specularGlossinessTexture->transform.rotation,
        1e-6f);
}

TEST(GltfParserTest, ParsesKhrMaterialsTransmissionMaterialExtension) {
    ExternalGltfFixture fixture =
        makeTexturedExternalBufferTriangleGltf("image.bin");
    const std::string assetMarker = "\"asset\":{\"version\":\"2.0\"},";
    const size_t assetPos = fixture.jsonText.find(assetMarker);
    ASSERT_NE(std::string::npos, assetPos);
    fixture.jsonText.insert(
        assetPos + assetMarker.size(),
        "\"extensionsUsed\":["
        "\"KHR_materials_transmission\","
        "\"KHR_texture_transform\"],"
        "\"extensionsRequired\":["
        "\"KHR_materials_transmission\","
        "\"KHR_texture_transform\"],");

    const std::string materialMarker = "\"materials\":[{";
    const size_t materialPos = fixture.jsonText.find(materialMarker);
    ASSERT_NE(std::string::npos, materialPos);
    fixture.jsonText.replace(
        materialPos,
        materialMarker.size(),
        "\"materials\":[{\"extensions\":{\"KHR_materials_transmission\":{"
        "\"transmissionFactor\":0.65,"
        "\"transmissionTexture\":{\"index\":0,\"texCoord\":0,"
        "\"extensions\":{\"KHR_texture_transform\":{"
        "\"offset\":[0.125,0.25],\"scale\":[0.5,0.75],"
        "\"rotation\":1.57079632679}}}}},");

    std::unique_ptr<GltfModel> model =
        parseExternalFixtureWithSolidImage(fixture);

    ASSERT_NE(nullptr, model);
    ASSERT_EQ(1u, model->primitives.size());
    const GltfPrimitive& primitive = model->primitives[0];
    EXPECT_NEAR(0.65f, primitive.transmissionFactor, 1e-6f);
    ASSERT_TRUE(primitive.transmissionTexture);
    EXPECT_EQ(0u, primitive.transmissionTexture->textureIndex);
    EXPECT_EQ(0, primitive.transmissionTexture->texCoord);
    EXPECT_NEAR(
        0.125f,
        primitive.transmissionTexture->transform.offset[0],
        1e-6f);
    EXPECT_NEAR(
        0.25f,
        primitive.transmissionTexture->transform.offset[1],
        1e-6f);
    EXPECT_NEAR(
        0.5f,
        primitive.transmissionTexture->transform.scale[0],
        1e-6f);
    EXPECT_NEAR(
        0.75f,
        primitive.transmissionTexture->transform.scale[1],
        1e-6f);
    EXPECT_NEAR(
        1.57079632679f,
        primitive.transmissionTexture->transform.rotation,
        1e-6f);
}

TEST(GltfParserTest, ParsesKhrMaterialsAnisotropyMaterialExtension) {
    ExternalGltfFixture fixture =
        makeTexturedExternalBufferTriangleGltf("image.bin");
    const std::string assetMarker = "\"asset\":{\"version\":\"2.0\"},";
    const size_t assetPos = fixture.jsonText.find(assetMarker);
    ASSERT_NE(std::string::npos, assetPos);
    fixture.jsonText.insert(
        assetPos + assetMarker.size(),
        "\"extensionsUsed\":["
        "\"KHR_materials_anisotropy\","
        "\"KHR_texture_transform\"],"
        "\"extensionsRequired\":["
        "\"KHR_materials_anisotropy\","
        "\"KHR_texture_transform\"],");

    const std::string materialMarker = "\"materials\":[{";
    const size_t materialPos = fixture.jsonText.find(materialMarker);
    ASSERT_NE(std::string::npos, materialPos);
    fixture.jsonText.replace(
        materialPos,
        materialMarker.size(),
        "\"materials\":[{\"extensions\":{\"KHR_materials_anisotropy\":{"
        "\"anisotropyStrength\":0.8,"
        "\"anisotropyRotation\":0.25,"
        "\"anisotropyTexture\":{\"index\":0,\"texCoord\":0,"
        "\"extensions\":{\"KHR_texture_transform\":{"
        "\"offset\":[0.125,0.25],\"scale\":[0.5,0.75],"
        "\"rotation\":1.57079632679}}}}},");

    std::unique_ptr<GltfModel> model =
        parseExternalFixtureWithSolidImage(fixture);

    ASSERT_NE(nullptr, model);
    ASSERT_EQ(1u, model->primitives.size());
    const GltfPrimitive& primitive = model->primitives[0];
    EXPECT_NEAR(0.8f, primitive.anisotropyStrength, 1e-6f);
    EXPECT_NEAR(0.25f, primitive.anisotropyRotation, 1e-6f);
    ASSERT_TRUE(primitive.anisotropyTexture);
    EXPECT_EQ(0u, primitive.anisotropyTexture->textureIndex);
    EXPECT_EQ(0, primitive.anisotropyTexture->texCoord);
    EXPECT_NEAR(
        0.125f,
        primitive.anisotropyTexture->transform.offset[0],
        1e-6f);
    EXPECT_NEAR(
        0.25f,
        primitive.anisotropyTexture->transform.offset[1],
        1e-6f);
    EXPECT_NEAR(
        0.5f,
        primitive.anisotropyTexture->transform.scale[0],
        1e-6f);
    EXPECT_NEAR(
        0.75f,
        primitive.anisotropyTexture->transform.scale[1],
        1e-6f);
    EXPECT_NEAR(
        1.57079632679f,
        primitive.anisotropyTexture->transform.rotation,
        1e-6f);
}

TEST(GltfParserTest, ParsesKhrMaterialsClearcoatMaterialExtension) {
    ExternalGltfFixture fixture =
        makeTexturedExternalBufferTriangleGltf("image.bin");
    const std::string assetMarker = "\"asset\":{\"version\":\"2.0\"},";
    const size_t assetPos = fixture.jsonText.find(assetMarker);
    ASSERT_NE(std::string::npos, assetPos);
    fixture.jsonText.insert(
        assetPos + assetMarker.size(),
        "\"extensionsUsed\":["
        "\"KHR_materials_clearcoat\","
        "\"KHR_texture_transform\"],"
        "\"extensionsRequired\":["
        "\"KHR_materials_clearcoat\","
        "\"KHR_texture_transform\"],");

    const std::string materialMarker = "\"materials\":[{";
    const size_t materialPos = fixture.jsonText.find(materialMarker);
    ASSERT_NE(std::string::npos, materialPos);
    fixture.jsonText.replace(
        materialPos,
        materialMarker.size(),
        "\"materials\":[{\"extensions\":{\"KHR_materials_clearcoat\":{"
        "\"clearcoatFactor\":0.75,"
        "\"clearcoatRoughnessFactor\":0.25,"
        "\"clearcoatTexture\":{\"index\":0,\"texCoord\":0,"
        "\"extensions\":{\"KHR_texture_transform\":{"
        "\"offset\":[0.125,0.25],\"scale\":[0.5,0.75],"
        "\"rotation\":1.57079632679}}},"
        "\"clearcoatRoughnessTexture\":{\"index\":0,\"texCoord\":0},"
        "\"clearcoatNormalTexture\":{\"index\":0,\"texCoord\":0,"
        "\"scale\":0.6}}},");

    std::unique_ptr<GltfModel> model =
        parseExternalFixtureWithSolidImage(fixture);

    ASSERT_NE(nullptr, model);
    ASSERT_EQ(1u, model->primitives.size());
    const GltfPrimitive& primitive = model->primitives[0];
    EXPECT_NEAR(0.75f, primitive.clearcoatFactor, 1e-6f);
    EXPECT_NEAR(0.25f, primitive.clearcoatRoughnessFactor, 1e-6f);
    EXPECT_NEAR(0.6f, primitive.clearcoatNormalTextureScale, 1e-6f);
    ASSERT_TRUE(primitive.clearcoatTexture);
    ASSERT_TRUE(primitive.clearcoatRoughnessTexture);
    ASSERT_TRUE(primitive.clearcoatNormalTexture);
    EXPECT_EQ(0u, primitive.clearcoatTexture->textureIndex);
    EXPECT_EQ(0u, primitive.clearcoatRoughnessTexture->textureIndex);
    EXPECT_EQ(0u, primitive.clearcoatNormalTexture->textureIndex);
    EXPECT_NEAR(
        0.125f,
        primitive.clearcoatTexture->transform.offset[0],
        1e-6f);
    EXPECT_NEAR(
        0.25f,
        primitive.clearcoatTexture->transform.offset[1],
        1e-6f);
    EXPECT_NEAR(
        0.5f,
        primitive.clearcoatTexture->transform.scale[0],
        1e-6f);
    EXPECT_NEAR(
        0.75f,
        primitive.clearcoatTexture->transform.scale[1],
        1e-6f);
    EXPECT_NEAR(
        1.57079632679f,
        primitive.clearcoatTexture->transform.rotation,
        1e-6f);
}

TEST(GltfParserTest, ParsesKhrMaterialsSheenMaterialExtension) {
    ExternalGltfFixture fixture =
        makeTexturedExternalBufferTriangleGltf("image.bin");
    const std::string assetMarker = "\"asset\":{\"version\":\"2.0\"},";
    const size_t assetPos = fixture.jsonText.find(assetMarker);
    ASSERT_NE(std::string::npos, assetPos);
    fixture.jsonText.insert(
        assetPos + assetMarker.size(),
        "\"extensionsUsed\":["
        "\"KHR_materials_sheen\","
        "\"KHR_texture_transform\"],"
        "\"extensionsRequired\":["
        "\"KHR_materials_sheen\","
        "\"KHR_texture_transform\"],");

    const std::string materialMarker = "\"materials\":[{";
    const size_t materialPos = fixture.jsonText.find(materialMarker);
    ASSERT_NE(std::string::npos, materialPos);
    fixture.jsonText.replace(
        materialPos,
        materialMarker.size(),
        "\"materials\":[{\"extensions\":{\"KHR_materials_sheen\":{"
        "\"sheenColorFactor\":[0.2,0.4,0.6],"
        "\"sheenRoughnessFactor\":0.35,"
        "\"sheenColorTexture\":{\"index\":0,\"texCoord\":0,"
        "\"extensions\":{\"KHR_texture_transform\":{"
        "\"offset\":[0.125,0.25],\"scale\":[0.5,0.75],"
        "\"rotation\":1.57079632679}}},"
        "\"sheenRoughnessTexture\":{\"index\":0,\"texCoord\":0}}},");

    std::unique_ptr<GltfModel> model =
        parseExternalFixtureWithSolidImage(fixture);

    ASSERT_NE(nullptr, model);
    ASSERT_EQ(1u, model->primitives.size());
    const GltfPrimitive& primitive = model->primitives[0];
    EXPECT_NEAR(0.2f, primitive.sheenColorFactor[0], 1e-6f);
    EXPECT_NEAR(0.4f, primitive.sheenColorFactor[1], 1e-6f);
    EXPECT_NEAR(0.6f, primitive.sheenColorFactor[2], 1e-6f);
    EXPECT_NEAR(0.35f, primitive.sheenRoughnessFactor, 1e-6f);
    ASSERT_TRUE(primitive.sheenColorTexture);
    ASSERT_TRUE(primitive.sheenRoughnessTexture);
    EXPECT_EQ(0u, primitive.sheenColorTexture->textureIndex);
    EXPECT_EQ(0u, primitive.sheenRoughnessTexture->textureIndex);
    EXPECT_NEAR(
        0.125f,
        primitive.sheenColorTexture->transform.offset[0],
        1e-6f);
    EXPECT_NEAR(
        0.25f,
        primitive.sheenColorTexture->transform.offset[1],
        1e-6f);
    EXPECT_NEAR(
        0.5f,
        primitive.sheenColorTexture->transform.scale[0],
        1e-6f);
    EXPECT_NEAR(
        0.75f,
        primitive.sheenColorTexture->transform.scale[1],
        1e-6f);
    EXPECT_NEAR(
        1.57079632679f,
        primitive.sheenColorTexture->transform.rotation,
        1e-6f);
}

TEST(GltfParserTest, RejectsMaterialDoubleSidedTypeMismatch) {
    ExternalGltfFixture fixture =
        makeTexturedExternalBufferTriangleGltf(
            "data:image/png;base64,AQIDBA==");
    const std::string marker = "\"doubleSided\":true";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.replace(
        markerPos,
        marker.size(),
        "\"doubleSided\":\"true\"");

    std::unique_ptr<GltfModel> model =
        parseExternalFixtureWithSolidImage(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsMaterialAlphaModeTypeMismatch) {
    ExternalGltfFixture fixture =
        makeTexturedExternalBufferTriangleGltf(
            "data:image/png;base64,AQIDBA==");
    const std::string marker = "\"alphaMode\":\"BLEND\"";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.replace(markerPos, marker.size(), "\"alphaMode\":1");

    std::unique_ptr<GltfModel> model =
        parseExternalFixtureWithSolidImage(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsMaterialAlphaCutoffTypeMismatch) {
    ExternalGltfFixture fixture =
        makeTexturedExternalBufferTriangleGltf(
            "data:image/png;base64,AQIDBA==");
    const std::string marker = "\"alphaCutoff\":0.25";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.replace(
        markerPos,
        marker.size(),
        "\"alphaCutoff\":\"0.25\"");

    std::unique_ptr<GltfModel> model =
        parseExternalFixtureWithSolidImage(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsMaterialAlphaCutoffBelowZero) {
    ExternalGltfFixture fixture =
        makeTexturedExternalBufferTriangleGltf(
            "data:image/png;base64,AQIDBA==");
    const std::string marker = "\"alphaCutoff\":0.25";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.replace(
        markerPos,
        marker.size(),
        "\"alphaCutoff\":-0.01");

    std::unique_ptr<GltfModel> model =
        parseExternalFixtureWithSolidImage(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsMaterialBaseColorFactorOutOfRange) {
    ExternalGltfFixture fixture =
        makeTexturedExternalBufferTriangleGltf(
            "data:image/png;base64,AQIDBA==");
    const std::string marker = "\"baseColorFactor\":[0.5,0.6,0.7,0.8]";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.replace(
        markerPos,
        marker.size(),
        "\"baseColorFactor\":[0.5,1.01,0.7,0.8]");

    std::unique_ptr<GltfModel> model =
        parseExternalFixtureWithSolidImage(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsMaterialMetallicRoughnessFactorsOutOfRange) {
    const std::vector<std::pair<std::string, std::string>> replacements = {
        {"\"metallicFactor\":0.4", "\"metallicFactor\":1.01"},
        {"\"roughnessFactor\":0.75", "\"roughnessFactor\":-0.01"}};

    for (const auto& replacement : replacements) {
        SCOPED_TRACE(replacement.second);
        ExternalGltfFixture fixture = makeFullMaterialExternalBufferTriangleGltf();
        const size_t markerPos = fixture.jsonText.find(replacement.first);
        ASSERT_NE(std::string::npos, markerPos);
        fixture.jsonText.replace(
            markerPos,
            replacement.first.size(),
            replacement.second);

        std::unique_ptr<GltfModel> model =
            parseExternalFixtureWithSolidImage(fixture);

        EXPECT_EQ(nullptr, model);
    }
}

TEST(GltfParserTest, RejectsMaterialOcclusionAndEmissiveOutOfRange) {
    const std::vector<std::pair<std::string, std::string>> replacements = {
        {"\"strength\":0.65", "\"strength\":1.25"},
        {"\"emissiveFactor\":[0.1,0.2,0.3]",
         "\"emissiveFactor\":[0.1,0.2,-0.01]"}};

    for (const auto& replacement : replacements) {
        SCOPED_TRACE(replacement.second);
        ExternalGltfFixture fixture = makeFullMaterialExternalBufferTriangleGltf();
        const size_t markerPos = fixture.jsonText.find(replacement.first);
        ASSERT_NE(std::string::npos, markerPos);
        fixture.jsonText.replace(
            markerPos,
            replacement.first.size(),
            replacement.second);

        std::unique_ptr<GltfModel> model =
            parseExternalFixtureWithSolidImage(fixture);

        EXPECT_EQ(nullptr, model);
    }
}

TEST(GltfParserTest, RejectsUnreferencedMaterialElementTypeMismatch) {
    ExternalGltfFixture fixture =
        makeTexturedExternalBufferTriangleGltf(
            "data:image/png;base64,AQIDBA==");
    const std::string marker =
        "\"materials\":[{\"doubleSided\":true,\"alphaMode\":\"BLEND\","
        "\"alphaCutoff\":0.25,\"pbrMetallicRoughness\":{"
        "\"baseColorFactor\":[0.5,0.6,0.7,0.8],"
        "\"baseColorTexture\":{\"index\":0,\"texCoord\":0}}}]";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.replace(
        markerPos,
        marker.size(),
        marker.substr(0, marker.size() - 1u) + ",7]");

    std::unique_ptr<GltfModel> model =
        parseExternalFixtureWithSolidImage(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsUnreferencedMaterialPbrTypeMismatch) {
    ExternalGltfFixture fixture =
        makeTexturedExternalBufferTriangleGltf(
            "data:image/png;base64,AQIDBA==");
    const std::string marker =
        "\"materials\":[{\"doubleSided\":true,\"alphaMode\":\"BLEND\","
        "\"alphaCutoff\":0.25,\"pbrMetallicRoughness\":{"
        "\"baseColorFactor\":[0.5,0.6,0.7,0.8],"
        "\"baseColorTexture\":{\"index\":0,\"texCoord\":0}}}]";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.replace(
        markerPos,
        marker.size(),
        marker.substr(0, marker.size() - 1u) +
            ",{\"pbrMetallicRoughness\":7}]");

    std::unique_ptr<GltfModel> model =
        parseExternalFixtureWithSolidImage(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsUnreferencedMaterialFactorOutOfRange) {
    ExternalGltfFixture fixture =
        makeTexturedExternalBufferTriangleGltf(
            "data:image/png;base64,AQIDBA==");
    const std::string marker =
        "\"materials\":[{\"doubleSided\":true,\"alphaMode\":\"BLEND\","
        "\"alphaCutoff\":0.25,\"pbrMetallicRoughness\":{"
        "\"baseColorFactor\":[0.5,0.6,0.7,0.8],"
        "\"baseColorTexture\":{\"index\":0,\"texCoord\":0}}}]";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.replace(
        markerPos,
        marker.size(),
        marker.substr(0, marker.size() - 1u) +
            ",{\"pbrMetallicRoughness\":{\"baseColorFactor\":[1.01,0,0,1]}}]");

    std::unique_ptr<GltfModel> model =
        parseExternalFixtureWithSolidImage(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, ParsesSkinnedTriangleDefaultPose) {
    const std::vector<uint8_t> glb = makeSkinnedTriangleGlb();
    std::unique_ptr<GltfModel> model =
        GltfParser::parse(glb.data(), glb.size());

    ASSERT_NE(nullptr, model);
    ASSERT_EQ(1u, model->primitives.size());
    const GltfPrimitive& primitive = model->primitives[0];
    EXPECT_TRUE(primitive.skinned);
    ASSERT_EQ(3u, primitive.vertices.size());

    EXPECT_NEAR(11.0, primitive.vertices[0].positionEcef.x(), 1e-12);
    EXPECT_NEAR(20.0, primitive.vertices[0].positionEcef.y(), 1e-12);
    EXPECT_NEAR(30.0, primitive.vertices[0].positionEcef.z(), 1e-12);
    EXPECT_NEAR(0.0, primitive.vertices[0].normalEcef.x(), 1e-12);
    EXPECT_NEAR(0.0, primitive.vertices[0].normalEcef.y(), 1e-12);
    EXPECT_NEAR(1.0, primitive.vertices[0].normalEcef.z(), 1e-12);
}

TEST(GltfParserTest, RejectsSkinNodeWithoutWeightsAttribute) {
    const std::vector<uint8_t> glb = makeSkinnedTriangleGlb(true, false);
    std::unique_ptr<GltfModel> model =
        GltfParser::parse(glb.data(), glb.size());

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsSkinWithInvalidJointNode) {
    const std::vector<uint8_t> glb = makeSkinnedTriangleGlb(false, true);
    std::unique_ptr<GltfModel> model =
        GltfParser::parse(glb.data(), glb.size());

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsSkinWeightsWithNonFiniteFloat) {
    const std::vector<uint8_t> glb =
        makeSkinnedTriangleGlb(false, false, true, false);
    std::unique_ptr<GltfModel> model =
        GltfParser::parse(glb.data(), glb.size());

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsSkinInverseBindWithNonFiniteFloat) {
    const std::vector<uint8_t> glb =
        makeSkinnedTriangleGlb(false, false, false, true);
    std::unique_ptr<GltfModel> model =
        GltfParser::parse(glb.data(), glb.size());

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsSkinInverseBindMatricesCountMismatch) {
    const std::vector<uint8_t> glb =
        makeSkinnedTriangleGlb(false, false, false, false, true);
    std::unique_ptr<GltfModel> model =
        GltfParser::parse(glb.data(), glb.size());

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, UpdatesLinearTranslationAnimation) {
    const std::vector<uint8_t> glb = makeAnimatedTranslationTriangleGlb();
    std::unique_ptr<GltfModel> model =
        GltfParser::parse(glb.data(), glb.size());

    ASSERT_NE(nullptr, model);
    ASSERT_TRUE(model->hasRuntimeAnimation());
    ASSERT_EQ(1u, model->primitives.size());
    ASSERT_EQ(3u, model->primitives[0].vertices.size());
    EXPECT_NEAR(0.0, model->primitives[0].vertices[0].positionEcef.x(), 1e-12);

    EXPECT_TRUE(model->updateAnimation(0.5));
    EXPECT_GT(model->currentAnimationRevision(), 0u);
    EXPECT_NEAR(5.0, model->primitives[0].vertices[0].positionEcef.x(), 1e-12);
    EXPECT_NEAR(0.0, model->primitives[0].vertices[0].positionEcef.y(), 1e-12);
    EXPECT_NEAR(0.0, model->primitives[0].vertices[0].positionEcef.z(), 1e-12);
}

TEST(GltfParserTest, UpdatesLinearRotationAnimation) {
    const std::vector<uint8_t> glb =
        makeAnimatedTranslationTriangleGlb(
            "LINEAR",
            false,
            false,
            false,
            true);
    std::unique_ptr<GltfModel> model =
        GltfParser::parse(glb.data(), glb.size());

    ASSERT_NE(nullptr, model);
    ASSERT_TRUE(model->hasRuntimeAnimation());
    ASSERT_EQ(1u, model->primitives.size());
    ASSERT_EQ(3u, model->primitives[0].vertices.size());

    EXPECT_TRUE(model->updateAnimation(0.5));
    EXPECT_NEAR(
        0.7071067811865476,
        model->primitives[0].vertices[1].positionEcef.x(),
        1e-12);
    EXPECT_NEAR(
        0.7071067811865476,
        model->primitives[0].vertices[1].positionEcef.y(),
        1e-12);
    EXPECT_NEAR(0.0, model->primitives[0].vertices[1].positionEcef.z(), 1e-12);
}

TEST(GltfParserTest, RejectsAnimationRotationWithZeroLengthOutput) {
    const std::vector<uint8_t> glb =
        makeAnimatedTranslationTriangleGlb(
            "LINEAR",
            false,
            false,
            false,
            true,
            true);
    std::unique_ptr<GltfModel> model =
        GltfParser::parse(glb.data(), glb.size());

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, PausesRuntimeAnimationWithoutChangingPose) {
    const std::vector<uint8_t> glb = makeAnimatedTranslationTriangleGlb();
    std::unique_ptr<GltfModel> model =
        GltfParser::parse(glb.data(), glb.size());

    ASSERT_NE(nullptr, model);
    ASSERT_TRUE(model->updateAnimation(0.25));
    const uint64_t revision = model->currentAnimationRevision();
    ASSERT_GT(revision, 0u);
    EXPECT_NEAR(2.5, model->primitives[0].vertices[0].positionEcef.x(), 1e-12);

    model->setAnimationPaused(true);
    EXPECT_TRUE(model->isAnimationPaused());
    EXPECT_FALSE(model->updateAnimation(0.75));
    EXPECT_EQ(revision, model->currentAnimationRevision());
    EXPECT_NEAR(2.5, model->primitives[0].vertices[0].positionEcef.x(), 1e-12);

    model->setAnimationPaused(false);
    EXPECT_FALSE(model->isAnimationPaused());
    EXPECT_TRUE(model->updateAnimation(0.75));
    EXPECT_GT(model->currentAnimationRevision(), revision);
    EXPECT_NEAR(7.5, model->primitives[0].vertices[0].positionEcef.x(), 1e-12);
}

TEST(GltfParserTest, ClampsRuntimeAnimationWhenLoopingDisabled) {
    const std::vector<uint8_t> glb = makeAnimatedTranslationTriangleGlb();
    std::unique_ptr<GltfModel> model =
        GltfParser::parse(glb.data(), glb.size());

    ASSERT_NE(nullptr, model);
    EXPECT_TRUE(model->isAnimationLooping());
    model->setAnimationLooping(false);
    EXPECT_FALSE(model->isAnimationLooping());

    EXPECT_TRUE(model->updateAnimation(1.5));
    const uint64_t revision = model->currentAnimationRevision();
    EXPECT_NEAR(10.0, model->primitives[0].vertices[0].positionEcef.x(), 1e-12);

    EXPECT_FALSE(model->updateAnimation(2.0));
    EXPECT_EQ(revision, model->currentAnimationRevision());
    EXPECT_NEAR(10.0, model->primitives[0].vertices[0].positionEcef.x(), 1e-12);
}

TEST(GltfParserTest, SelectsRuntimeAnimationByIndex) {
    const std::vector<uint8_t> glb = makeDualAnimationTranslationTriangleGlb();
    std::unique_ptr<GltfModel> model =
        GltfParser::parse(glb.data(), glb.size());

    ASSERT_NE(nullptr, model);
    ASSERT_TRUE(model->hasRuntimeAnimation());
    EXPECT_EQ(2u, model->animationCount());
    EXPECT_EQ(0, model->activeAnimation());

    EXPECT_TRUE(model->updateAnimation(0.5));
    EXPECT_NEAR(5.0, model->primitives[0].vertices[0].positionEcef.x(), 1e-12);
    EXPECT_NEAR(0.0, model->primitives[0].vertices[0].positionEcef.y(), 1e-12);

    EXPECT_FALSE(model->setActiveAnimation(2));
    EXPECT_EQ(0, model->activeAnimation());
    EXPECT_TRUE(model->setActiveAnimation(1));
    EXPECT_EQ(1, model->activeAnimation());
    EXPECT_TRUE(model->updateAnimation(0.5));
    EXPECT_NEAR(0.0, model->primitives[0].vertices[0].positionEcef.x(), 1e-12);
    EXPECT_NEAR(10.0, model->primitives[0].vertices[0].positionEcef.y(), 1e-12);
}

TEST(GltfParserTest, UpdatesStepTranslationAnimation) {
    const std::vector<uint8_t> glb =
        makeAnimatedTranslationTriangleGlb("STEP");
    std::unique_ptr<GltfModel> model =
        GltfParser::parse(glb.data(), glb.size());

    ASSERT_NE(nullptr, model);
    ASSERT_TRUE(model->hasRuntimeAnimation());
    ASSERT_EQ(1u, model->primitives.size());
    ASSERT_EQ(3u, model->primitives[0].vertices.size());

    EXPECT_TRUE(model->updateAnimation(0.5));
    EXPECT_GT(model->currentAnimationRevision(), 0u);
    EXPECT_NEAR(0.0, model->primitives[0].vertices[0].positionEcef.x(), 1e-12);
    EXPECT_NEAR(0.0, model->primitives[0].vertices[0].positionEcef.y(), 1e-12);
    EXPECT_NEAR(0.0, model->primitives[0].vertices[0].positionEcef.z(), 1e-12);
}

TEST(GltfParserTest, UpdatesCubicSplineTranslationAnimation) {
    const std::vector<uint8_t> glb =
        makeAnimatedTranslationTriangleGlb("CUBICSPLINE");
    std::unique_ptr<GltfModel> model =
        GltfParser::parse(glb.data(), glb.size());

    ASSERT_NE(nullptr, model);
    ASSERT_TRUE(model->hasRuntimeAnimation());
    ASSERT_EQ(1u, model->primitives.size());
    ASSERT_EQ(3u, model->primitives[0].vertices.size());

    EXPECT_TRUE(model->updateAnimation(0.25));
    EXPECT_GT(model->currentAnimationRevision(), 0u);
    EXPECT_NEAR(1.5625, model->primitives[0].vertices[0].positionEcef.x(), 1e-12);
    EXPECT_NEAR(0.0, model->primitives[0].vertices[0].positionEcef.y(), 1e-12);
    EXPECT_NEAR(0.0, model->primitives[0].vertices[0].positionEcef.z(), 1e-12);
}

TEST(GltfParserTest, RejectsAnimationsTypeMismatch) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string marker = "\"scene\":0";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.replace(
        markerPos,
        marker.size(),
        "\"animations\":{},\"scene\":0");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsAnimationSamplersTypeMismatch) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string marker = "\"scene\":0";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.replace(
        markerPos,
        marker.size(),
        "\"animations\":[{\"samplers\":{},\"channels\":[]}],\"scene\":0");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsAnimationChannelsTypeMismatch) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    appendAccessorToExternalFixture(
        fixture,
        "{\"bufferView\":0,\"componentType\":5126,\"count\":1,\"type\":\"SCALAR\"}");
    const std::string marker = "\"scene\":0";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.replace(
        markerPos,
        marker.size(),
        "\"animations\":[{\"samplers\":[{\"input\":4,\"output\":0}],"
        "\"channels\":{}}],\"scene\":0");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsAnimationWithoutChannels) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    appendAccessorToExternalFixture(
        fixture,
        "{\"bufferView\":0,\"componentType\":5126,\"count\":1,\"type\":\"SCALAR\"}");
    const std::string marker = "\"scene\":0";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.replace(
        markerPos,
        marker.size(),
        "\"animations\":[{\"samplers\":[{\"input\":4,\"output\":0}]}],"
        "\"scene\":0");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsMalformedCubicSplineOutputCount) {
    const std::vector<uint8_t> glb =
        makeAnimatedTranslationTriangleGlb("CUBICSPLINE", true);
    std::unique_ptr<GltfModel> model =
        GltfParser::parse(glb.data(), glb.size());

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsAnimationOutputWithNonFiniteValue) {
    const std::vector<uint8_t> glb =
        makeAnimatedTranslationTriangleGlb("LINEAR", false, false, true);
    std::unique_ptr<GltfModel> model =
        GltfParser::parse(glb.data(), glb.size());

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsUnsupportedAnimationInterpolation) {
    const std::vector<uint8_t> glb =
        makeAnimatedTranslationTriangleGlb("CATMULLROMSPLINE");
    std::unique_ptr<GltfModel> model =
        GltfParser::parse(glb.data(), glb.size());

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsDuplicateAnimationChannelTarget) {
    const std::vector<uint8_t> glb =
        makeAnimatedTranslationTriangleGlb("LINEAR", false, true);
    std::unique_ptr<GltfModel> model =
        GltfParser::parse(glb.data(), glb.size());

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, UpdatesLinearMorphWeightAnimation) {
    const std::vector<uint8_t> glb = makeAnimatedMorphWeightTriangleGlb();
    std::unique_ptr<GltfModel> model =
        GltfParser::parse(glb.data(), glb.size());

    ASSERT_NE(nullptr, model);
    ASSERT_TRUE(model->hasRuntimeAnimation());
    ASSERT_EQ(1u, model->primitives.size());
    ASSERT_EQ(3u, model->primitives[0].vertices.size());
    EXPECT_NEAR(0.0, model->primitives[0].vertices[0].positionEcef.x(), 1e-12);

    EXPECT_TRUE(model->updateAnimation(0.5));
    EXPECT_NEAR(5.0, model->primitives[0].vertices[0].positionEcef.x(), 1e-12);
    EXPECT_NEAR(1.0, model->primitives[0].vertices[1].positionEcef.x(), 1e-12);
}

TEST(GltfParserTest, RejectsMorphTargetWithNonFiniteDelta) {
    const std::vector<uint8_t> glb =
        makeAnimatedMorphWeightTriangleGlb(true);
    std::unique_ptr<GltfModel> model =
        GltfParser::parse(glb.data(), glb.size());

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, UpdatesMorphTangentWeightAnimation) {
    const std::vector<uint8_t> glb =
        makeAnimatedMorphTangentWeightTriangleGlb(true);
    std::unique_ptr<GltfModel> model =
        GltfParser::parse(glb.data(), glb.size());

    ASSERT_NE(nullptr, model);
    ASSERT_TRUE(model->hasRuntimeAnimation());
    ASSERT_EQ(1u, model->primitives.size());
    ASSERT_EQ(3u, model->primitives[0].vertexTangents.size());
    EXPECT_NEAR(1.0f, model->primitives[0].vertexTangents[0][0], 1e-6f);
    EXPECT_NEAR(0.0f, model->primitives[0].vertexTangents[0][1], 1e-6f);
    EXPECT_NEAR(1.0f, model->primitives[0].vertexTangents[0][3], 1e-6f);

    model->setAnimationLooping(false);
    EXPECT_TRUE(model->updateAnimation(1.0));
    ASSERT_EQ(3u, model->primitives[0].vertexTangents.size());
    EXPECT_NEAR(0.0f, model->primitives[0].vertexTangents[0][0], 1e-6f);
    EXPECT_NEAR(1.0f, model->primitives[0].vertexTangents[0][1], 1e-6f);
    EXPECT_NEAR(0.0f, model->primitives[0].vertexTangents[0][2], 1e-6f);
    EXPECT_NEAR(1.0f, model->primitives[0].vertexTangents[0][3], 1e-6f);
}

TEST(GltfParserTest, RejectsMorphTangentWithoutBaseTangent) {
    const std::vector<uint8_t> glb =
        makeAnimatedMorphTangentWeightTriangleGlb(false);
    std::unique_ptr<GltfModel> model =
        GltfParser::parse(glb.data(), glb.size());

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, ParsesBaseColorTextureDataUriAndSampler) {
    const ExternalGltfFixture fixture =
        makeTexturedExternalBufferTriangleGltf(
            "data:image/png;base64,AQIDBA==");
    bool decodedImage = false;

    std::unique_ptr<GltfModel> model = GltfParser::parse(
        reinterpret_cast<const uint8_t*>(fixture.jsonText.data()),
        fixture.jsonText.size(),
        [&](const std::string& uri) {
            return uri == "triangle.bin" ? fixture.bin
                                         : std::vector<uint8_t>{};
        },
        [&](const uint8_t* data, size_t size) -> std::optional<GltfImage> {
            EXPECT_EQ(4u, size);
            EXPECT_EQ(1u, data[0]);
            decodedImage = true;
            GltfImage image;
            image.width = 1;
            image.height = 1;
            image.channels = 4;
            image.pixels = {255, 128, 64, 32};
            return image;
        });

    ASSERT_NE(nullptr, model);
    ASSERT_TRUE(decodedImage);
    ASSERT_EQ(1u, model->textures.size());
    EXPECT_EQ(1, model->textures[0].image.width);
    EXPECT_EQ(GltfTextureFilter::Nearest, model->textures[0].sampler.minFilter);
    EXPECT_EQ(GltfTextureFilter::Nearest, model->textures[0].sampler.magFilter);
    EXPECT_FALSE(model->textures[0].sampler.mipmap);
    EXPECT_EQ(GltfTextureWrap::MirroredRepeat, model->textures[0].sampler.wrapS);
    EXPECT_EQ(GltfTextureWrap::ClampToEdge, model->textures[0].sampler.wrapT);

    ASSERT_EQ(1u, model->primitives.size());
    ASSERT_TRUE(model->primitives[0].baseColorTextureIndex.has_value());
    EXPECT_EQ(0u, *model->primitives[0].baseColorTextureIndex);
    EXPECT_EQ(GltfAlphaMode::Blend, model->primitives[0].alphaMode);
    EXPECT_NEAR(0.25f, model->primitives[0].alphaCutoff, 1e-6f);
    EXPECT_TRUE(model->primitives[0].doubleSided);
}

TEST(GltfParserTest, ParsesExtTextureWebpTextureExtension) {
    ExternalGltfFixture fixture =
        makeTexturedExternalBufferTriangleGltf(
            "data:image/png;base64,CQkJCQ==");
    const std::string assetMarker = "\"asset\":{\"version\":\"2.0\"},";
    const size_t assetPos = fixture.jsonText.find(assetMarker);
    ASSERT_NE(std::string::npos, assetPos);
    fixture.jsonText.insert(
        assetPos + assetMarker.size(),
        "\"extensionsUsed\":[\"EXT_texture_webp\"],"
        "\"extensionsRequired\":[\"EXT_texture_webp\"],");

    const std::string textureMarker =
        "\"textures\":[{\"source\":0,\"sampler\":0}]";
    const size_t texturePos = fixture.jsonText.find(textureMarker);
    ASSERT_NE(std::string::npos, texturePos);
    fixture.jsonText.replace(
        texturePos,
        textureMarker.size(),
        "\"textures\":[{\"source\":0,\"sampler\":0,"
        "\"extensions\":{\"EXT_texture_webp\":{\"source\":1}}}]");

    const std::string imageMarker =
        "\"images\":[{\"uri\":\"data:image/png;base64,CQkJCQ==\"}]";
    const size_t imagePos = fixture.jsonText.find(imageMarker);
    ASSERT_NE(std::string::npos, imagePos);
    fixture.jsonText.replace(
        imagePos,
        imageMarker.size(),
        "\"images\":["
        "{\"uri\":\"data:image/png;base64,CQkJCQ==\"},"
        "{\"uri\":\"data:image/webp;base64,UklGRgQAAABXRUJQAQIDBA==\"}]");

    bool decodedWebp = false;
    bool decodedFallback = false;
    std::unique_ptr<GltfModel> model = GltfParser::parse(
        reinterpret_cast<const uint8_t*>(fixture.jsonText.data()),
        fixture.jsonText.size(),
        [&](const std::string& uri) {
            return uri == "triangle.bin" ? fixture.bin
                                         : std::vector<uint8_t>{};
        },
        [&](const uint8_t* data, size_t size) -> std::optional<GltfImage> {
            if (size == 0) {
                return std::nullopt;
            }
            decodedWebp = bytesLookLikeFakeWebp(data, size);
            decodedFallback = data[0] == 9u;
            GltfImage image;
            image.width = 1;
            image.height = 1;
            image.channels = 4;
            image.pixels = {16, 32, 64, 255};
            return image;
        });

    ASSERT_NE(nullptr, model);
    EXPECT_TRUE(decodedWebp);
    EXPECT_FALSE(decodedFallback);
    ASSERT_EQ(1u, model->textures.size());
    EXPECT_EQ(1, model->textures[0].image.width);
    ASSERT_EQ(1u, model->primitives.size());
    ASSERT_TRUE(model->primitives[0].baseColorTexture);
    EXPECT_EQ(0u, model->primitives[0].baseColorTexture->textureIndex);
}

TEST(GltfParserTest, RejectsExtTextureWebpDecoderFailure) {
    ExternalGltfFixture fixture =
        makeTexturedExternalBufferTriangleGltf(
            "data:image/png;base64,CQkJCQ==");
    const std::string assetMarker = "\"asset\":{\"version\":\"2.0\"},";
    const size_t assetPos = fixture.jsonText.find(assetMarker);
    ASSERT_NE(std::string::npos, assetPos);
    fixture.jsonText.insert(
        assetPos + assetMarker.size(),
        "\"extensionsUsed\":[\"EXT_texture_webp\"],"
        "\"extensionsRequired\":[\"EXT_texture_webp\"],");

    const std::string textureMarker =
        "\"textures\":[{\"source\":0,\"sampler\":0}]";
    const size_t texturePos = fixture.jsonText.find(textureMarker);
    ASSERT_NE(std::string::npos, texturePos);
    fixture.jsonText.replace(
        texturePos,
        textureMarker.size(),
        "\"textures\":[{\"source\":0,\"sampler\":0,"
        "\"extensions\":{\"EXT_texture_webp\":{\"source\":1}}}]");

    const std::string imageMarker =
        "\"images\":[{\"uri\":\"data:image/png;base64,CQkJCQ==\"}]";
    const size_t imagePos = fixture.jsonText.find(imageMarker);
    ASSERT_NE(std::string::npos, imagePos);
    fixture.jsonText.replace(
        imagePos,
        imageMarker.size(),
        "\"images\":["
        "{\"uri\":\"data:image/png;base64,CQkJCQ==\"},"
        "{\"uri\":\"data:image/webp;base64,UklGRgQAAABXRUJQAQIDBA==\"}]");

    bool decodedWebp = false;
    bool decodedFallback = false;
    std::unique_ptr<GltfModel> model = GltfParser::parse(
        reinterpret_cast<const uint8_t*>(fixture.jsonText.data()),
        fixture.jsonText.size(),
        [&](const std::string& uri) {
            return uri == "triangle.bin" ? fixture.bin
                                         : std::vector<uint8_t>{};
        },
        [&](const uint8_t* data, size_t size) -> std::optional<GltfImage> {
            decodedWebp = bytesLookLikeFakeWebp(data, size);
            decodedFallback = size > 0 && data[0] == 9u;
            return std::nullopt;
        });

    EXPECT_EQ(nullptr, model);
    EXPECT_TRUE(decodedWebp);
    EXPECT_FALSE(decodedFallback);
}

TEST(GltfParserTest, ParsesExtTextureWebpWithoutFallbackSource) {
    ExternalGltfFixture fixture =
        makeTexturedExternalBufferTriangleGltf(
            "data:image/png;base64,CQkJCQ==");
    const std::string assetMarker = "\"asset\":{\"version\":\"2.0\"},";
    const size_t assetPos = fixture.jsonText.find(assetMarker);
    ASSERT_NE(std::string::npos, assetPos);
    fixture.jsonText.insert(
        assetPos + assetMarker.size(),
        "\"extensionsUsed\":[\"EXT_texture_webp\"],"
        "\"extensionsRequired\":[\"EXT_texture_webp\"],");

    const std::string textureMarker =
        "\"textures\":[{\"source\":0,\"sampler\":0}]";
    const size_t texturePos = fixture.jsonText.find(textureMarker);
    ASSERT_NE(std::string::npos, texturePos);
    fixture.jsonText.replace(
        texturePos,
        textureMarker.size(),
        "\"textures\":[{\"sampler\":0,"
        "\"extensions\":{\"EXT_texture_webp\":{\"source\":0}}}]");

    const std::string imageMarker =
        "\"images\":[{\"uri\":\"data:image/png;base64,CQkJCQ==\"}]";
    const size_t imagePos = fixture.jsonText.find(imageMarker);
    ASSERT_NE(std::string::npos, imagePos);
    fixture.jsonText.replace(
        imagePos,
        imageMarker.size(),
        "\"images\":[{\"uri\":\"data:image/webp;base64,UklGRgQAAABXRUJQAQIDBA==\"}]");

    bool decodedWebp = false;
    std::unique_ptr<GltfModel> model = GltfParser::parse(
        reinterpret_cast<const uint8_t*>(fixture.jsonText.data()),
        fixture.jsonText.size(),
        [&](const std::string& uri) {
            return uri == "triangle.bin" ? fixture.bin
                                         : std::vector<uint8_t>{};
        },
        [&](const uint8_t* data, size_t size) -> std::optional<GltfImage> {
            decodedWebp = bytesLookLikeFakeWebp(data, size);
            GltfImage image;
            image.width = 1;
            image.height = 1;
            image.channels = 4;
            image.pixels = {16, 32, 64, 255};
            return image;
        });

    ASSERT_NE(nullptr, model);
    EXPECT_TRUE(decodedWebp);
    ASSERT_EQ(1u, model->textures.size());
    EXPECT_EQ(1, model->textures[0].image.width);
    ASSERT_EQ(1u, model->primitives.size());
    ASSERT_TRUE(model->primitives[0].baseColorTexture);
}

TEST(GltfParserTest, ParsesExtTextureWebpBufferViewImage) {
    ExternalGltfFixture fixture =
        makeBufferViewImageExternalBufferTriangleGltf(true, "image/webp");
    const std::string assetMarker = "\"asset\":{\"version\":\"2.0\"},";
    const size_t assetPos = fixture.jsonText.find(assetMarker);
    ASSERT_NE(std::string::npos, assetPos);
    fixture.jsonText.insert(
        assetPos + assetMarker.size(),
        "\"extensionsUsed\":[\"EXT_texture_webp\"],"
        "\"extensionsRequired\":[\"EXT_texture_webp\"],");

    const std::string textureMarker =
        "\"textures\":[{\"source\":0,\"sampler\":0}]";
    const size_t texturePos = fixture.jsonText.find(textureMarker);
    ASSERT_NE(std::string::npos, texturePos);
    fixture.jsonText.replace(
        texturePos,
        textureMarker.size(),
        "\"textures\":[{\"sampler\":0,"
        "\"extensions\":{\"EXT_texture_webp\":{\"source\":0}}}]");

    bool decodedWebp = false;
    std::unique_ptr<GltfModel> model = GltfParser::parse(
        reinterpret_cast<const uint8_t*>(fixture.jsonText.data()),
        fixture.jsonText.size(),
        [&](const std::string& uri) {
            return uri == "triangle.bin" ? fixture.bin
                                         : std::vector<uint8_t>{};
        },
        [&](const uint8_t* data, size_t size) -> std::optional<GltfImage> {
            decodedWebp = bytesLookLikeFakeWebp(data, size);
            GltfImage image;
            image.width = 1;
            image.height = 1;
            image.channels = 4;
            image.pixels = {16, 32, 64, 255};
            return image;
        });

    ASSERT_NE(nullptr, model);
    EXPECT_TRUE(decodedWebp);
    ASSERT_EQ(1u, model->textures.size());
    EXPECT_EQ(1, model->textures[0].image.width);
}

TEST(GltfParserTest, RejectsExtTextureWebpBufferViewBytesWithoutWebpSignature) {
    ExternalGltfFixture fixture =
        makeBufferViewImageExternalBufferTriangleGltf(
            true,
            "image/webp",
            {9, 8, 7, 6});
    const std::string assetMarker = "\"asset\":{\"version\":\"2.0\"},";
    const size_t assetPos = fixture.jsonText.find(assetMarker);
    ASSERT_NE(std::string::npos, assetPos);
    fixture.jsonText.insert(
        assetPos + assetMarker.size(),
        "\"extensionsUsed\":[\"EXT_texture_webp\"],"
        "\"extensionsRequired\":[\"EXT_texture_webp\"],");

    const std::string textureMarker =
        "\"textures\":[{\"source\":0,\"sampler\":0}]";
    const size_t texturePos = fixture.jsonText.find(textureMarker);
    ASSERT_NE(std::string::npos, texturePos);
    fixture.jsonText.replace(
        texturePos,
        textureMarker.size(),
        "\"textures\":[{\"sampler\":0,"
        "\"extensions\":{\"EXT_texture_webp\":{\"source\":0}}}]");

    bool decodedImage = false;
    std::unique_ptr<GltfModel> model = GltfParser::parse(
        reinterpret_cast<const uint8_t*>(fixture.jsonText.data()),
        fixture.jsonText.size(),
        [&](const std::string& uri) {
            return uri == "triangle.bin" ? fixture.bin
                                         : std::vector<uint8_t>{};
        },
        [&](const uint8_t*, size_t) -> std::optional<GltfImage> {
            decodedImage = true;
            GltfImage image;
            image.width = 1;
            image.height = 1;
            image.channels = 4;
            image.pixels = {255, 255, 255, 255};
            return image;
        });

    EXPECT_EQ(nullptr, model);
    EXPECT_FALSE(decodedImage);
}

TEST(GltfParserTest, RejectsBaseColorTextureDataUriWithUnsupportedMimeType) {
    const ExternalGltfFixture fixture =
        makeTexturedExternalBufferTriangleGltf(
            "data:image/webp;base64,UklGRgQAAABXRUJQAQIDBA==");
    bool decodedImage = false;

    std::unique_ptr<GltfModel> model = GltfParser::parse(
        reinterpret_cast<const uint8_t*>(fixture.jsonText.data()),
        fixture.jsonText.size(),
        [&](const std::string& uri) {
            return uri == "triangle.bin" ? fixture.bin
                                         : std::vector<uint8_t>{};
        },
        [&](const uint8_t*, size_t) -> std::optional<GltfImage> {
            decodedImage = true;
            GltfImage image;
            image.width = 1;
            image.height = 1;
            image.channels = 4;
            image.pixels = {255, 255, 255, 255};
            return image;
        });

    EXPECT_EQ(nullptr, model);
    EXPECT_FALSE(decodedImage);
}

TEST(GltfParserTest, RejectsBaseColorTextureDataUriWithoutBase64Encoding) {
    const ExternalGltfFixture fixture =
        makeTexturedExternalBufferTriangleGltf("data:image/png,AQIDBA==");
    bool resolvedImageUri = false;
    bool decodedImage = false;

    std::unique_ptr<GltfModel> model = GltfParser::parse(
        reinterpret_cast<const uint8_t*>(fixture.jsonText.data()),
        fixture.jsonText.size(),
        [&](const std::string& uri) {
            if (uri == "triangle.bin") {
                return fixture.bin;
            }
            resolvedImageUri = true;
            return std::vector<uint8_t>{7};
        },
        [&](const uint8_t*, size_t) -> std::optional<GltfImage> {
            decodedImage = true;
            GltfImage image;
            image.width = 1;
            image.height = 1;
            image.channels = 4;
            image.pixels = {255, 255, 255, 255};
            return image;
        });

    EXPECT_EQ(nullptr, model);
    EXPECT_FALSE(resolvedImageUri);
    EXPECT_FALSE(decodedImage);
}

TEST(GltfParserTest, RejectsBaseColorTextureEmptyDataUriPayload) {
    const ExternalGltfFixture fixture =
        makeTexturedExternalBufferTriangleGltf("data:image/png;base64,");
    bool decodedImage = false;

    std::unique_ptr<GltfModel> model = GltfParser::parse(
        reinterpret_cast<const uint8_t*>(fixture.jsonText.data()),
        fixture.jsonText.size(),
        [&](const std::string& uri) {
            return uri == "triangle.bin" ? fixture.bin
                                         : std::vector<uint8_t>{7};
        },
        [&](const uint8_t*, size_t) -> std::optional<GltfImage> {
            decodedImage = true;
            GltfImage image;
            image.width = 1;
            image.height = 1;
            image.channels = 4;
            image.pixels = {255, 255, 255, 255};
            return image;
        });

    EXPECT_EQ(nullptr, model);
    EXPECT_FALSE(decodedImage);
}

TEST(GltfParserTest, RejectsBaseColorTextureMalformedBase64DataUri) {
    const ExternalGltfFixture fixture =
        makeTexturedExternalBufferTriangleGltf("data:image/png;base64,@@@@");
    bool resolvedImageUri = false;
    bool decodedImage = false;

    std::unique_ptr<GltfModel> model = GltfParser::parse(
        reinterpret_cast<const uint8_t*>(fixture.jsonText.data()),
        fixture.jsonText.size(),
        [&](const std::string& uri) {
            if (uri == "triangle.bin") {
                return fixture.bin;
            }
            resolvedImageUri = true;
            return std::vector<uint8_t>{7};
        },
        [&](const uint8_t*, size_t) -> std::optional<GltfImage> {
            decodedImage = true;
            GltfImage image;
            image.width = 1;
            image.height = 1;
            image.channels = 4;
            image.pixels = {255, 255, 255, 255};
            return image;
        });

    EXPECT_EQ(nullptr, model);
    EXPECT_FALSE(resolvedImageUri);
    EXPECT_FALSE(decodedImage);
}

TEST(GltfParserTest, RejectsBaseColorTextureDataUriWithTrailingBytesAfterPadding) {
    const ExternalGltfFixture fixture =
        makeTexturedExternalBufferTriangleGltf(
            "data:image/png;base64,AQIDBA==AAAA");
    bool resolvedImageUri = false;
    bool decodedImage = false;

    std::unique_ptr<GltfModel> model = GltfParser::parse(
        reinterpret_cast<const uint8_t*>(fixture.jsonText.data()),
        fixture.jsonText.size(),
        [&](const std::string& uri) {
            if (uri == "triangle.bin") {
                return fixture.bin;
            }
            resolvedImageUri = true;
            return std::vector<uint8_t>{7};
        },
        [&](const uint8_t*, size_t) -> std::optional<GltfImage> {
            decodedImage = true;
            GltfImage image;
            image.width = 1;
            image.height = 1;
            image.channels = 4;
            image.pixels = {255, 255, 255, 255};
            return image;
        });

    EXPECT_EQ(nullptr, model);
    EXPECT_FALSE(resolvedImageUri);
    EXPECT_FALSE(decodedImage);
}

TEST(GltfParserTest, RejectsBaseColorTextureMalformedDataUriWithoutImageDecoder) {
    const ExternalGltfFixture fixture =
        makeTexturedExternalBufferTriangleGltf(
            "data:image/png;base64,AQIDBA==AAAA");
    bool resolvedImageUri = false;

    std::unique_ptr<GltfModel> model = GltfParser::parse(
        reinterpret_cast<const uint8_t*>(fixture.jsonText.data()),
        fixture.jsonText.size(),
        [&](const std::string& uri) {
            if (uri == "triangle.bin") {
                return fixture.bin;
            }
            resolvedImageUri = true;
            return std::vector<uint8_t>{7};
        });

    EXPECT_EQ(nullptr, model);
    EXPECT_FALSE(resolvedImageUri);
}

TEST(GltfParserTest, RejectsBaseColorTextureExternalImageResolvingEmpty) {
    const ExternalGltfFixture fixture =
        makeTexturedExternalBufferTriangleGltf("image.bin");
    bool decodedImage = false;

    std::unique_ptr<GltfModel> model = GltfParser::parse(
        reinterpret_cast<const uint8_t*>(fixture.jsonText.data()),
        fixture.jsonText.size(),
        [&](const std::string& uri) {
            return uri == "triangle.bin" ? fixture.bin
                                         : std::vector<uint8_t>{};
        },
        [&](const uint8_t*, size_t) -> std::optional<GltfImage> {
            decodedImage = true;
            GltfImage image;
            image.width = 1;
            image.height = 1;
            image.channels = 4;
            image.pixels = {255, 255, 255, 255};
            return image;
        });

    EXPECT_EQ(nullptr, model);
    EXPECT_FALSE(decodedImage);
}

TEST(GltfParserTest, RejectsTextureWithInvalidSamplerFilter) {
    ExternalGltfFixture fixture =
        makeTexturedExternalBufferTriangleGltf(
            "data:image/png;base64,AQIDBA==");
    const std::string marker = "\"minFilter\":9728";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.replace(markerPos, marker.size(), "\"minFilter\":1234");

    std::unique_ptr<GltfModel> model = GltfParser::parse(
        reinterpret_cast<const uint8_t*>(fixture.jsonText.data()),
        fixture.jsonText.size(),
        [&](const std::string& uri) {
            return uri == "triangle.bin" ? fixture.bin
                                         : std::vector<uint8_t>{};
        },
        [](const uint8_t*, size_t) -> std::optional<GltfImage> {
            GltfImage image;
            image.width = 1;
            image.height = 1;
            image.channels = 4;
            image.pixels = {255, 255, 255, 255};
            return image;
        });

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsTextureWithInvalidSamplerWrap) {
    ExternalGltfFixture fixture =
        makeTexturedExternalBufferTriangleGltf(
            "data:image/png;base64,AQIDBA==");
    const std::string marker = "\"wrapS\":33648";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.replace(markerPos, marker.size(), "\"wrapS\":1234");

    std::unique_ptr<GltfModel> model = GltfParser::parse(
        reinterpret_cast<const uint8_t*>(fixture.jsonText.data()),
        fixture.jsonText.size(),
        [&](const std::string& uri) {
            return uri == "triangle.bin" ? fixture.bin
                                         : std::vector<uint8_t>{};
        },
        [](const uint8_t*, size_t) -> std::optional<GltfImage> {
            GltfImage image;
            image.width = 1;
            image.height = 1;
            image.channels = 4;
            image.pixels = {255, 255, 255, 255};
            return image;
        });

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsTextureWithInvalidSamplerIndex) {
    ExternalGltfFixture fixture =
        makeTexturedExternalBufferTriangleGltf(
            "data:image/png;base64,AQIDBA==");
    const std::string marker = "\"textures\":[{\"source\":0,\"sampler\":0}]";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.replace(
        markerPos,
        marker.size(),
        "\"textures\":[{\"source\":0,\"sampler\":7}]");

    std::unique_ptr<GltfModel> model = GltfParser::parse(
        reinterpret_cast<const uint8_t*>(fixture.jsonText.data()),
        fixture.jsonText.size(),
        [&](const std::string& uri) {
            return uri == "triangle.bin" ? fixture.bin
                                         : std::vector<uint8_t>{};
        },
        [](const uint8_t*, size_t) -> std::optional<GltfImage> {
            GltfImage image;
            image.width = 1;
            image.height = 1;
            image.channels = 4;
            image.pixels = {255, 255, 255, 255};
            return image;
        });

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsUnreferencedSamplerWithInvalidWrap) {
    ExternalGltfFixture fixture =
        makeTexturedExternalBufferTriangleGltf(
            "data:image/png;base64,AQIDBA==");
    const std::string marker =
        "\"samplers\":[{\"minFilter\":9728,\"magFilter\":9728,"
        "\"wrapS\":33648,\"wrapT\":33071}]";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.replace(
        markerPos,
        marker.size(),
        marker.substr(0, marker.size() - 1u) + ",{\"wrapS\":7}]");

    std::unique_ptr<GltfModel> model =
        parseExternalFixtureWithSolidImage(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsUnreferencedTextureWithoutSource) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string marker = "\"scene\":0";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.replace(
        markerPos,
        marker.size(),
        "\"samplers\":[{}],\"textures\":[{\"sampler\":0}],\"scene\":0");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsTextureWithNegativeSamplerIndex) {
    ExternalGltfFixture fixture =
        makeTexturedExternalBufferTriangleGltf(
            "data:image/png;base64,AQIDBA==");
    const std::string marker = "\"textures\":[{\"source\":0,\"sampler\":0}]";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.replace(
        markerPos,
        marker.size(),
        "\"textures\":[{\"source\":0,\"sampler\":-1}]");

    std::unique_ptr<GltfModel> model = GltfParser::parse(
        reinterpret_cast<const uint8_t*>(fixture.jsonText.data()),
        fixture.jsonText.size(),
        [&](const std::string& uri) {
            return uri == "triangle.bin" ? fixture.bin
                                         : std::vector<uint8_t>{};
        },
        [](const uint8_t*, size_t) -> std::optional<GltfImage> {
            GltfImage image;
            image.width = 1;
            image.height = 1;
            image.channels = 4;
            image.pixels = {255, 255, 255, 255};
            return image;
        });

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsTextureWithNegativeSourceIndex) {
    ExternalGltfFixture fixture =
        makeTexturedExternalBufferTriangleGltf(
            "data:image/png;base64,AQIDBA==");
    const std::string marker = "\"textures\":[{\"source\":0,\"sampler\":0}]";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.replace(
        markerPos,
        marker.size(),
        "\"textures\":[{\"source\":-1,\"sampler\":0}]");
    bool decodedImage = false;

    std::unique_ptr<GltfModel> model = GltfParser::parse(
        reinterpret_cast<const uint8_t*>(fixture.jsonText.data()),
        fixture.jsonText.size(),
        [&](const std::string& uri) {
            return uri == "triangle.bin" ? fixture.bin
                                         : std::vector<uint8_t>{};
        },
        [&](const uint8_t*, size_t) -> std::optional<GltfImage> {
            decodedImage = true;
            GltfImage image;
            image.width = 1;
            image.height = 1;
            image.channels = 4;
            image.pixels = {255, 255, 255, 255};
            return image;
        });

    EXPECT_EQ(nullptr, model);
    EXPECT_FALSE(decodedImage);
}

TEST(GltfParserTest, RejectsTextureWithOutOfRangeSourceIndex) {
    ExternalGltfFixture fixture =
        makeTexturedExternalBufferTriangleGltf(
            "data:image/png;base64,AQIDBA==");
    const std::string marker = "\"textures\":[{\"source\":0,\"sampler\":0}]";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.replace(
        markerPos,
        marker.size(),
        "\"textures\":[{\"source\":7,\"sampler\":0}]");
    bool decodedImage = false;

    std::unique_ptr<GltfModel> model = GltfParser::parse(
        reinterpret_cast<const uint8_t*>(fixture.jsonText.data()),
        fixture.jsonText.size(),
        [&](const std::string& uri) {
            return uri == "triangle.bin" ? fixture.bin
                                         : std::vector<uint8_t>{};
        },
        [&](const uint8_t*, size_t) -> std::optional<GltfImage> {
            decodedImage = true;
            GltfImage image;
            image.width = 1;
            image.height = 1;
            image.channels = 4;
            image.pixels = {255, 255, 255, 255};
            return image;
        });

    EXPECT_EQ(nullptr, model);
    EXPECT_FALSE(decodedImage);
}

TEST(GltfParserTest, RejectsTexturesTypeMismatch) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string marker = "\"scene\":0";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.replace(
        markerPos,
        marker.size(),
        "\"textures\":{},\"scene\":0");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsImagesTypeMismatch) {
    ExternalGltfFixture fixture =
        makeTexturedExternalBufferTriangleGltf(
            "data:image/png;base64,AQIDBA==");
    const std::string marker =
        "\"images\":[{\"uri\":\"data:image/png;base64,AQIDBA==\"}]";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.replace(
        markerPos,
        marker.size(),
        "\"images\":{}");
    bool decodedImage = false;

    std::unique_ptr<GltfModel> model = GltfParser::parse(
        reinterpret_cast<const uint8_t*>(fixture.jsonText.data()),
        fixture.jsonText.size(),
        [&](const std::string& uri) {
            return uri == "triangle.bin" ? fixture.bin
                                         : std::vector<uint8_t>{};
        },
        [&](const uint8_t*, size_t) -> std::optional<GltfImage> {
            decodedImage = true;
            GltfImage image;
            image.width = 1;
            image.height = 1;
            image.channels = 4;
            image.pixels = {255, 255, 255, 255};
            return image;
        });

    EXPECT_EQ(nullptr, model);
    EXPECT_FALSE(decodedImage);
}

TEST(GltfParserTest, RejectsImageWithoutUriOrBufferView) {
    ExternalGltfFixture fixture =
        makeTexturedExternalBufferTriangleGltf("image.bin");
    const std::string marker = "\"images\":[{\"uri\":\"image.bin\"}]";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.replace(markerPos, marker.size(), "\"images\":[{}]");
    bool decodedImage = false;

    std::unique_ptr<GltfModel> model = GltfParser::parse(
        reinterpret_cast<const uint8_t*>(fixture.jsonText.data()),
        fixture.jsonText.size(),
        [&](const std::string& uri) {
            return uri == "triangle.bin" ? fixture.bin
                                         : std::vector<uint8_t>{9, 8, 7, 6};
        },
        [&](const uint8_t*, size_t) -> std::optional<GltfImage> {
            decodedImage = true;
            GltfImage image;
            image.width = 1;
            image.height = 1;
            image.channels = 4;
            image.pixels = {255, 255, 255, 255};
            return image;
        });

    EXPECT_EQ(nullptr, model);
    EXPECT_FALSE(decodedImage);
}

TEST(GltfParserTest, RejectsImageWithEmptyUri) {
    ExternalGltfFixture fixture =
        makeTexturedExternalBufferTriangleGltf("image.bin");
    const std::string marker = "\"images\":[{\"uri\":\"image.bin\"}]";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.replace(
        markerPos,
        marker.size(),
        "\"images\":[{\"uri\":\"\"}]");
    bool decodedImage = false;

    std::unique_ptr<GltfModel> model = GltfParser::parse(
        reinterpret_cast<const uint8_t*>(fixture.jsonText.data()),
        fixture.jsonText.size(),
        [&](const std::string& uri) {
            return uri == "triangle.bin" ? fixture.bin
                                         : std::vector<uint8_t>{9, 8, 7, 6};
        },
        [&](const uint8_t*, size_t) -> std::optional<GltfImage> {
            decodedImage = true;
            GltfImage image;
            image.width = 1;
            image.height = 1;
            image.channels = 4;
            image.pixels = {255, 255, 255, 255};
            return image;
        });

    EXPECT_EQ(nullptr, model);
    EXPECT_FALSE(decodedImage);
}

TEST(GltfParserTest, RejectsUnreferencedImageDataUriWithMalformedBase64) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string marker = "\"buffers\":[";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.insert(
        markerPos,
        "\"images\":[{\"uri\":\"data:image/png;base64,AQIDBA==AAAA\"}],");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsImageWithNegativeBufferView) {
    ExternalGltfFixture fixture =
        makeTexturedExternalBufferTriangleGltf("image.bin");
    const std::string marker = "\"images\":[{\"uri\":\"image.bin\"}]";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.replace(
        markerPos,
        marker.size(),
        "\"images\":[{\"bufferView\":-1,\"mimeType\":\"image/png\"}]");
    bool decodedImage = false;

    std::unique_ptr<GltfModel> model = GltfParser::parse(
        reinterpret_cast<const uint8_t*>(fixture.jsonText.data()),
        fixture.jsonText.size(),
        [&](const std::string& uri) {
            return uri == "triangle.bin" ? fixture.bin
                                         : std::vector<uint8_t>{9, 8, 7, 6};
        },
        [&](const uint8_t*, size_t) -> std::optional<GltfImage> {
            decodedImage = true;
            GltfImage image;
            image.width = 1;
            image.height = 1;
            image.channels = 4;
            image.pixels = {255, 255, 255, 255};
            return image;
        });

    EXPECT_EQ(nullptr, model);
    EXPECT_FALSE(decodedImage);
}

TEST(GltfParserTest, RejectsImageWithUnsupportedExplicitMimeType) {
    ExternalGltfFixture fixture =
        makeTexturedExternalBufferTriangleGltf("image.bin");
    const std::string marker = "\"images\":[{\"uri\":\"image.bin\"}]";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.replace(
        markerPos,
        marker.size(),
        "\"images\":[{\"uri\":\"image.bin\",\"mimeType\":\"image/webp\"}]");
    bool decodedImage = false;

    std::unique_ptr<GltfModel> model = GltfParser::parse(
        reinterpret_cast<const uint8_t*>(fixture.jsonText.data()),
        fixture.jsonText.size(),
        [&](const std::string& uri) {
            return uri == "triangle.bin" ? fixture.bin
                                         : std::vector<uint8_t>{7};
        },
        [&](const uint8_t*, size_t) -> std::optional<GltfImage> {
            decodedImage = true;
            GltfImage image;
            image.width = 1;
            image.height = 1;
            image.channels = 4;
            image.pixels = {255, 255, 255, 255};
            return image;
        });

    EXPECT_EQ(nullptr, model);
    EXPECT_FALSE(decodedImage);
}

TEST(GltfParserTest, RejectsExternalWebpCoreImageWithoutTextureExtension) {
    ExternalGltfFixture fixture =
        makeTexturedExternalBufferTriangleGltf("texture.webp");
    bool decodedImage = false;
    const std::vector<uint8_t> webpHeader = {
        'R', 'I', 'F', 'F',
        0, 0, 0, 0,
        'W', 'E', 'B', 'P'};

    std::unique_ptr<GltfModel> model = GltfParser::parse(
        reinterpret_cast<const uint8_t*>(fixture.jsonText.data()),
        fixture.jsonText.size(),
        [&](const std::string& uri) {
            return uri == "triangle.bin" ? fixture.bin : webpHeader;
        },
        [&](const uint8_t*, size_t) -> std::optional<GltfImage> {
            decodedImage = true;
            GltfImage image;
            image.width = 1;
            image.height = 1;
            image.channels = 4;
            image.pixels = {255, 255, 255, 255};
            return image;
        });

    EXPECT_EQ(nullptr, model);
    EXPECT_FALSE(decodedImage);
}

TEST(GltfParserTest, RejectsExternalWebpCoreImageWithoutDecoder) {
    ExternalGltfFixture fixture =
        makeTexturedExternalBufferTriangleGltf("textures/texture.webp?rev=1");
    bool resolvedImage = false;

    std::unique_ptr<GltfModel> model = GltfParser::parse(
        reinterpret_cast<const uint8_t*>(fixture.jsonText.data()),
        fixture.jsonText.size(),
        [&](const std::string& uri) {
            if (uri == "triangle.bin") {
                return fixture.bin;
            }
            resolvedImage = true;
            return std::vector<uint8_t>{'R', 'I', 'F', 'F'};
        });

    EXPECT_EQ(nullptr, model);
    EXPECT_FALSE(resolvedImage);
}

TEST(GltfParserTest, RejectsExternalKtx2ImageWithoutBasisuSupport) {
    ExternalGltfFixture fixture =
        makeTexturedExternalBufferTriangleGltf("texture.ktx2");
    bool decodedImage = false;
    const std::vector<uint8_t> ktx2Header = {
        static_cast<uint8_t>(0xABu), 'K', 'T', 'X', ' ', '2', '0',
        static_cast<uint8_t>(0xBBu), 0x0Du, 0x0Au, 0x1Au, 0x0Au};

    std::unique_ptr<GltfModel> model = GltfParser::parse(
        reinterpret_cast<const uint8_t*>(fixture.jsonText.data()),
        fixture.jsonText.size(),
        [&](const std::string& uri) {
            return uri == "triangle.bin" ? fixture.bin : ktx2Header;
        },
        [&](const uint8_t*, size_t) -> std::optional<GltfImage> {
            decodedImage = true;
            GltfImage image;
            image.width = 1;
            image.height = 1;
            image.channels = 4;
            image.pixels = {255, 255, 255, 255};
            return image;
        });

    EXPECT_EQ(nullptr, model);
    EXPECT_FALSE(decodedImage);
}

TEST(GltfParserTest, RejectsExternalKtx2ImageWithoutDecoder) {
    ExternalGltfFixture fixture =
        makeTexturedExternalBufferTriangleGltf("TEXTURE.KTX2#main");
    bool resolvedImage = false;

    std::unique_ptr<GltfModel> model = GltfParser::parse(
        reinterpret_cast<const uint8_t*>(fixture.jsonText.data()),
        fixture.jsonText.size(),
        [&](const std::string& uri) {
            if (uri == "triangle.bin") {
                return fixture.bin;
            }
            resolvedImage = true;
            return std::vector<uint8_t>{
                static_cast<uint8_t>(0xABu), 'K', 'T', 'X'};
        });

    EXPECT_EQ(nullptr, model);
    EXPECT_FALSE(resolvedImage);
}

TEST(GltfParserTest, RejectsExternalKtxAndAstcImagesWithoutDecoder) {
    struct UnsupportedUriCase {
        const char* uri;
        std::vector<uint8_t> bytes;
        const char* label;
    };

    const std::array<UnsupportedUriCase, 2> cases = {{
        {
            "textures/texture.KTX?rev=1",
            {
                static_cast<uint8_t>(0xABu), 'K', 'T', 'X', ' ', '1', '1',
                static_cast<uint8_t>(0xBBu), 0x0Du, 0x0Au, 0x1Au, 0x0Au},
            "KTX1 URI"},
        {
            "textures/texture.astc#main",
            {
                0x13u,
                static_cast<uint8_t>(0xABu),
                static_cast<uint8_t>(0xA1u),
                0x5Cu},
            "ASTC URI"},
    }};

    for (const UnsupportedUriCase& testCase : cases) {
        ExternalGltfFixture fixture =
            makeTexturedExternalBufferTriangleGltf(testCase.uri);
        bool resolvedImage = false;

        std::unique_ptr<GltfModel> model = GltfParser::parse(
            reinterpret_cast<const uint8_t*>(fixture.jsonText.data()),
            fixture.jsonText.size(),
            [&](const std::string& uri) {
                if (uri == "triangle.bin") {
                    return fixture.bin;
                }
                resolvedImage = true;
                return testCase.bytes;
            });

        EXPECT_EQ(nullptr, model) << testCase.label;
        EXPECT_FALSE(resolvedImage) << testCase.label;
    }
}

TEST(GltfParserTest, RejectsExternalAvifImageWithoutDecoder) {
    ExternalGltfFixture fixture =
        makeTexturedExternalBufferTriangleGltf("textures/texture.AVIF?rev=1");
    bool resolvedImage = false;

    std::unique_ptr<GltfModel> model = GltfParser::parse(
        reinterpret_cast<const uint8_t*>(fixture.jsonText.data()),
        fixture.jsonText.size(),
        [&](const std::string& uri) {
            if (uri == "triangle.bin") {
                return fixture.bin;
            }
            resolvedImage = true;
            return std::vector<uint8_t>{'a', 'v', 'i', 'f'};
        });

    EXPECT_EQ(nullptr, model);
    EXPECT_FALSE(resolvedImage);
}

TEST(GltfParserTest, RejectsExternalDdsImageWithoutDecoder) {
    ExternalGltfFixture fixture =
        makeTexturedExternalBufferTriangleGltf("textures/texture.dds#main");
    bool resolvedImage = false;

    std::unique_ptr<GltfModel> model = GltfParser::parse(
        reinterpret_cast<const uint8_t*>(fixture.jsonText.data()),
        fixture.jsonText.size(),
        [&](const std::string& uri) {
            if (uri == "triangle.bin") {
                return fixture.bin;
            }
            resolvedImage = true;
            return std::vector<uint8_t>{'D', 'D', 'S', ' '};
        });

    EXPECT_EQ(nullptr, model);
    EXPECT_FALSE(resolvedImage);
}

TEST(GltfParserTest, RejectsUnsupportedEncodedImageSignaturesBeforeDecoder) {
    struct EncodedCase {
        std::vector<uint8_t> bytes;
        const char* label;
    };

    const std::vector<EncodedCase> cases = {
        {
            {
                static_cast<uint8_t>(0xABu), 'K', 'T', 'X', ' ', '1', '1',
                static_cast<uint8_t>(0xBBu), 0x0Du, 0x0Au, 0x1Au, 0x0Au},
            "KTX1"},
        {
            {
                static_cast<uint8_t>(0xABu), 'K', 'T', 'X', ' ', '2', '0',
                static_cast<uint8_t>(0xBBu), 0x0Du, 0x0Au, 0x1Au, 0x0Au},
            "KTX2"},
        {
            {'D', 'D', 'S', ' '},
            "DDS"},
        {
            {
                0x13u,
                static_cast<uint8_t>(0xABu),
                static_cast<uint8_t>(0xA1u),
                0x5Cu},
            "ASTC"},
        {
            {
                0, 0, 0, 20,
                'f', 't', 'y', 'p',
                'a', 'v', 'i', 'f',
                0, 0, 0, 0},
            "AVIF major brand"},
        {
            {
                0, 0, 0, 24,
                'f', 't', 'y', 'p',
                'm', 'i', 'f', '1',
                0, 0, 0, 0,
                'a', 'v', 'i', 'f'},
            "AVIF compatible brand"},
    };

    for (const EncodedCase& testCase : cases) {
        ExternalGltfFixture fixture =
            makeTexturedExternalBufferTriangleGltf("image.bin");
        bool decodedImage = false;

        std::unique_ptr<GltfModel> model = GltfParser::parse(
            reinterpret_cast<const uint8_t*>(fixture.jsonText.data()),
            fixture.jsonText.size(),
            [&](const std::string& uri) {
                return uri == "triangle.bin" ? fixture.bin : testCase.bytes;
            },
            [&](const uint8_t*, size_t) -> std::optional<GltfImage> {
                decodedImage = true;
                GltfImage image;
                image.width = 1;
                image.height = 1;
                image.channels = 4;
                image.pixels = {255, 255, 255, 255};
                return image;
            });

        EXPECT_EQ(nullptr, model) << testCase.label;
        EXPECT_FALSE(decodedImage) << testCase.label;
    }
}

TEST(GltfParserTest, RejectsUnsupportedBufferViewImageSignaturesBeforeDecoder) {
    struct EncodedCase {
        std::vector<uint8_t> bytes;
        const char* label;
    };

    const std::vector<EncodedCase> cases = {
        {
            {
                static_cast<uint8_t>(0xABu), 'K', 'T', 'X', ' ', '1', '1',
                static_cast<uint8_t>(0xBBu), 0x0Du, 0x0Au, 0x1Au, 0x0Au},
            "KTX1"},
        {
            {
                static_cast<uint8_t>(0xABu), 'K', 'T', 'X', ' ', '2', '0',
                static_cast<uint8_t>(0xBBu), 0x0Du, 0x0Au, 0x1Au, 0x0Au},
            "KTX2"},
        {
            {'D', 'D', 'S', ' '},
            "DDS"},
        {
            {
                0x13u,
                static_cast<uint8_t>(0xABu),
                static_cast<uint8_t>(0xA1u),
                0x5Cu},
            "ASTC"},
        {
            {
                0, 0, 0, 20,
                'f', 't', 'y', 'p',
                'a', 'v', 'i', 'f',
                0, 0, 0, 0},
            "AVIF major brand"},
        {
            {
                0, 0, 0, 24,
                'f', 't', 'y', 'p',
                'm', 'i', 'f', '1',
                0, 0, 0, 0,
                'a', 'v', 'i', 'f'},
            "AVIF compatible brand"},
    };

    for (const EncodedCase& testCase : cases) {
        ExternalGltfFixture fixture =
            makeBufferViewImageExternalBufferTriangleGltf(
                true,
                "image/png",
                testCase.bytes);
        bool decodedImage = false;

        std::unique_ptr<GltfModel> model = GltfParser::parse(
            reinterpret_cast<const uint8_t*>(fixture.jsonText.data()),
            fixture.jsonText.size(),
            [&](const std::string& uri) {
                return uri == "triangle.bin" ? fixture.bin
                                             : std::vector<uint8_t>{};
            },
            [&](const uint8_t*, size_t) -> std::optional<GltfImage> {
                decodedImage = true;
                GltfImage image;
                image.width = 1;
                image.height = 1;
                image.channels = 4;
                image.pixels = {255, 255, 255, 255};
                return image;
            });

        EXPECT_EQ(nullptr, model) << testCase.label;
        EXPECT_FALSE(decodedImage) << testCase.label;
    }
}

TEST(GltfParserTest, ParsesBaseColorTextureBufferViewImage) {
    const ExternalGltfFixture fixture =
        makeBufferViewImageExternalBufferTriangleGltf();
    bool decodedImage = false;

    std::unique_ptr<GltfModel> model = GltfParser::parse(
        reinterpret_cast<const uint8_t*>(fixture.jsonText.data()),
        fixture.jsonText.size(),
        [&](const std::string& uri) {
            return uri == "triangle.bin" ? fixture.bin
                                         : std::vector<uint8_t>{};
        },
        [&](const uint8_t* data, size_t size) -> std::optional<GltfImage> {
            EXPECT_EQ(4u, size);
            EXPECT_EQ(9u, data[0]);
            EXPECT_EQ(6u, data[3]);
            decodedImage = true;
            GltfImage image;
            image.width = 1;
            image.height = 1;
            image.channels = 4;
            image.pixels = {9, 8, 7, 6};
            return image;
        });

    ASSERT_NE(nullptr, model);
    EXPECT_TRUE(decodedImage);
    ASSERT_EQ(1u, model->textures.size());
    EXPECT_EQ(9u, model->textures[0].image.pixels[0]);
    ASSERT_EQ(1u, model->primitives.size());
    ASSERT_TRUE(model->primitives[0].baseColorTexture);
    EXPECT_EQ(0u, model->primitives[0].baseColorTexture->textureIndex);
}

TEST(GltfParserTest, RejectsImageUriTypeMismatch) {
    ExternalGltfFixture fixture =
        makeTexturedExternalBufferTriangleGltf(
            "data:image/png;base64,AQIDBA==");
    const std::string marker =
        "\"images\":[{\"uri\":\"data:image/png;base64,AQIDBA==\"}]";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.replace(
        markerPos,
        marker.size(),
        "\"images\":[{\"uri\":7}]");
    bool decodedImage = false;

    std::unique_ptr<GltfModel> model = GltfParser::parse(
        reinterpret_cast<const uint8_t*>(fixture.jsonText.data()),
        fixture.jsonText.size(),
        [&](const std::string& uri) {
            return uri == "triangle.bin" ? fixture.bin
                                         : std::vector<uint8_t>{};
        },
        [&](const uint8_t*, size_t) -> std::optional<GltfImage> {
            decodedImage = true;
            GltfImage image;
            image.width = 1;
            image.height = 1;
            image.channels = 4;
            image.pixels = {255, 255, 255, 255};
            return image;
        });

    EXPECT_EQ(nullptr, model);
    EXPECT_FALSE(decodedImage);
}

TEST(GltfParserTest, RejectsUnreferencedBufferViewImageWithoutMimeType) {
    ExternalGltfFixture fixture =
        makeTexturedExternalBufferTriangleGltf(
            "data:image/png;base64,AQIDBA==");
    const std::string marker =
        "\"images\":[{\"uri\":\"data:image/png;base64,AQIDBA==\"}]";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.replace(
        markerPos,
        marker.size(),
        marker.substr(0, marker.size() - 1u) + ",{\"bufferView\":3}]");

    std::unique_ptr<GltfModel> model =
        parseExternalFixtureWithSolidImage(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsUnreferencedImageBufferViewOutOfRange) {
    ExternalGltfFixture fixture =
        makeTexturedExternalBufferTriangleGltf(
            "data:image/png;base64,AQIDBA==");
    const std::string marker =
        "\"images\":[{\"uri\":\"data:image/png;base64,AQIDBA==\"}]";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.replace(
        markerPos,
        marker.size(),
        marker.substr(0, marker.size() - 1u) +
            ",{\"bufferView\":99,\"mimeType\":\"image/png\"}]");
    bool decodedImage = false;

    std::unique_ptr<GltfModel> model = GltfParser::parse(
        reinterpret_cast<const uint8_t*>(fixture.jsonText.data()),
        fixture.jsonText.size(),
        [&](const std::string& uri) {
            return uri == "triangle.bin" ? fixture.bin
                                         : std::vector<uint8_t>{7};
        },
        [&](const uint8_t*, size_t) -> std::optional<GltfImage> {
            decodedImage = true;
            GltfImage image;
            image.width = 1;
            image.height = 1;
            image.channels = 4;
            image.pixels = {255, 255, 255, 255};
            return image;
        });

    EXPECT_EQ(nullptr, model);
    EXPECT_FALSE(decodedImage);
}

TEST(GltfParserTest, RejectsImageWithUriAndBufferView) {
    ExternalGltfFixture fixture =
        makeBufferViewImageExternalBufferTriangleGltf();
    const std::string marker = "\"images\":[{\"bufferView\":4";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.replace(
        markerPos,
        marker.size(),
        "\"images\":[{\"uri\":\"image.bin\",\"bufferView\":4");
    bool decodedImage = false;

    std::unique_ptr<GltfModel> model = GltfParser::parse(
        reinterpret_cast<const uint8_t*>(fixture.jsonText.data()),
        fixture.jsonText.size(),
        [&](const std::string& uri) {
            return uri == "triangle.bin" ? fixture.bin
                                         : std::vector<uint8_t>{};
        },
        [&](const uint8_t*, size_t) -> std::optional<GltfImage> {
            decodedImage = true;
            GltfImage image;
            image.width = 1;
            image.height = 1;
            image.channels = 4;
            image.pixels = {255, 255, 255, 255};
            return image;
        });

    EXPECT_EQ(nullptr, model);
    EXPECT_FALSE(decodedImage);
}

TEST(GltfParserTest, RejectsImageBufferViewTypeMismatch) {
    ExternalGltfFixture fixture =
        makeBufferViewImageExternalBufferTriangleGltf();
    const std::string marker = "\"bufferView\":4";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.replace(
        markerPos,
        marker.size(),
        "\"bufferView\":\"4\"");
    bool decodedImage = false;

    std::unique_ptr<GltfModel> model = GltfParser::parse(
        reinterpret_cast<const uint8_t*>(fixture.jsonText.data()),
        fixture.jsonText.size(),
        [&](const std::string& uri) {
            return uri == "triangle.bin" ? fixture.bin
                                         : std::vector<uint8_t>{};
        },
        [&](const uint8_t*, size_t) -> std::optional<GltfImage> {
            decodedImage = true;
            GltfImage image;
            image.width = 1;
            image.height = 1;
            image.channels = 4;
            image.pixels = {255, 255, 255, 255};
            return image;
        });

    EXPECT_EQ(nullptr, model);
    EXPECT_FALSE(decodedImage);
}

TEST(GltfParserTest, RejectsImageMimeTypeTypeMismatch) {
    ExternalGltfFixture fixture =
        makeBufferViewImageExternalBufferTriangleGltf();
    const std::string marker = "\"mimeType\":\"image/png\"";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.replace(markerPos, marker.size(), "\"mimeType\":7");
    bool decodedImage = false;

    std::unique_ptr<GltfModel> model = GltfParser::parse(
        reinterpret_cast<const uint8_t*>(fixture.jsonText.data()),
        fixture.jsonText.size(),
        [&](const std::string& uri) {
            return uri == "triangle.bin" ? fixture.bin
                                         : std::vector<uint8_t>{};
        },
        [&](const uint8_t*, size_t) -> std::optional<GltfImage> {
            decodedImage = true;
            GltfImage image;
            image.width = 1;
            image.height = 1;
            image.channels = 4;
            image.pixels = {255, 255, 255, 255};
            return image;
        });

    EXPECT_EQ(nullptr, model);
    EXPECT_FALSE(decodedImage);
}

TEST(GltfParserTest, RejectsBufferViewImageWithoutMimeType) {
    const ExternalGltfFixture fixture =
        makeBufferViewImageExternalBufferTriangleGltf(false);
    bool decodedImage = false;

    std::unique_ptr<GltfModel> model = GltfParser::parse(
        reinterpret_cast<const uint8_t*>(fixture.jsonText.data()),
        fixture.jsonText.size(),
        [&](const std::string& uri) {
            return uri == "triangle.bin" ? fixture.bin
                                         : std::vector<uint8_t>{};
        },
        [&](const uint8_t*, size_t) -> std::optional<GltfImage> {
            decodedImage = true;
            GltfImage image;
            image.width = 1;
            image.height = 1;
            image.channels = 4;
            image.pixels = {255, 255, 255, 255};
            return image;
        });

    EXPECT_EQ(nullptr, model);
    EXPECT_FALSE(decodedImage);
}

TEST(GltfParserTest, RejectsBufferViewImageWithUnsupportedMimeType) {
    const ExternalGltfFixture fixture =
        makeBufferViewImageExternalBufferTriangleGltf(true, "image/webp");
    bool decodedImage = false;

    std::unique_ptr<GltfModel> model = GltfParser::parse(
        reinterpret_cast<const uint8_t*>(fixture.jsonText.data()),
        fixture.jsonText.size(),
        [&](const std::string& uri) {
            return uri == "triangle.bin" ? fixture.bin
                                         : std::vector<uint8_t>{};
        },
        [&](const uint8_t*, size_t) -> std::optional<GltfImage> {
            decodedImage = true;
            GltfImage image;
            image.width = 1;
            image.height = 1;
            image.channels = 4;
            image.pixels = {255, 255, 255, 255};
            return image;
        });

    EXPECT_EQ(nullptr, model);
    EXPECT_FALSE(decodedImage);
}

TEST(GltfParserTest, ParsesFullCoreMaterialTexturesAndKhrTextureTransform) {
    const ExternalGltfFixture fixture =
        makeFullMaterialExternalBufferTriangleGltf();
    int decodedImages = 0;

    std::unique_ptr<GltfModel> model = GltfParser::parse(
        reinterpret_cast<const uint8_t*>(fixture.jsonText.data()),
        fixture.jsonText.size(),
        [&](const std::string& uri) {
            if (uri == "triangle.bin") return fixture.bin;
            if (uri == "base.bin") return std::vector<uint8_t>{0};
            if (uri == "mr.bin") return std::vector<uint8_t>{1};
            if (uri == "normal.bin") return std::vector<uint8_t>{2};
            if (uri == "occlusion.bin") return std::vector<uint8_t>{3};
            if (uri == "emissive.bin") return std::vector<uint8_t>{4};
            return std::vector<uint8_t>{};
        },
        [&](const uint8_t* data, size_t size) -> std::optional<GltfImage> {
            EXPECT_EQ(1u, size);
            ++decodedImages;
            GltfImage image;
            image.width = 1;
            image.height = 1;
            image.channels = 4;
            image.pixels = {data[0], 128, 255, 255};
            return image;
        });

    ASSERT_NE(nullptr, model);
    EXPECT_EQ(5, decodedImages);
    ASSERT_EQ(5u, model->textures.size());
    ASSERT_EQ(1u, model->primitives.size());
    const GltfPrimitive& primitive = model->primitives[0];
    ASSERT_TRUE(primitive.baseColorTexture);
    EXPECT_EQ(0u, primitive.baseColorTexture->textureIndex);
    EXPECT_EQ(0, primitive.baseColorTexture->texCoord);
    EXPECT_NEAR(0.25f, primitive.baseColorTexture->transform.offset[0], 1e-6f);
    EXPECT_NEAR(0.5f, primitive.baseColorTexture->transform.offset[1], 1e-6f);
    EXPECT_NEAR(2.0f, primitive.baseColorTexture->transform.scale[0], 1e-6f);
    EXPECT_NEAR(3.0f, primitive.baseColorTexture->transform.scale[1], 1e-6f);
    EXPECT_NEAR(
        1.57079632679f,
        primitive.baseColorTexture->transform.rotation,
        1e-6f);
    EXPECT_TRUE(primitive.metallicRoughnessTexture);
    EXPECT_EQ(1u, primitive.metallicRoughnessTexture->textureIndex);
    EXPECT_NEAR(0.4f, primitive.metallicFactor, 1e-6f);
    EXPECT_NEAR(0.75f, primitive.roughnessFactor, 1e-6f);
    ASSERT_TRUE(primitive.normalTexture);
    EXPECT_EQ(2u, primitive.normalTexture->textureIndex);
    EXPECT_NEAR(0.35f, primitive.normalTextureScale, 1e-6f);
    ASSERT_TRUE(primitive.occlusionTexture);
    EXPECT_EQ(3u, primitive.occlusionTexture->textureIndex);
    EXPECT_NEAR(0.65f, primitive.occlusionTextureStrength, 1e-6f);
    ASSERT_TRUE(primitive.emissiveTexture);
    EXPECT_EQ(4u, primitive.emissiveTexture->textureIndex);
    EXPECT_NEAR(0.1f, primitive.emissiveFactor[0], 1e-6f);
    EXPECT_NEAR(0.2f, primitive.emissiveFactor[1], 1e-6f);
    EXPECT_NEAR(0.3f, primitive.emissiveFactor[2], 1e-6f);
}

TEST(GltfParserTest, ParsesKhrTextureTransformTexCoordOneWhenProvided) {
    ExternalGltfFixture fixture = makeFullMaterialExternalBufferTriangleGltf();
    const std::string attrMarker = "\"TEXCOORD_0\":2";
    const size_t attrPos = fixture.jsonText.find(attrMarker);
    ASSERT_NE(std::string::npos, attrPos);
    fixture.jsonText.insert(attrPos + attrMarker.size(), ",\"TEXCOORD_1\":2");
    const std::string rotationMarker = "\"rotation\":1.57079632679";
    const size_t rotationPos = fixture.jsonText.find(rotationMarker);
    ASSERT_NE(std::string::npos, rotationPos);
    fixture.jsonText.insert(rotationPos + rotationMarker.size(), ",\"texCoord\":1");

    std::unique_ptr<GltfModel> model = GltfParser::parse(
        reinterpret_cast<const uint8_t*>(fixture.jsonText.data()),
        fixture.jsonText.size(),
        [&](const std::string& uri) {
            if (uri == "triangle.bin") return fixture.bin;
            return std::vector<uint8_t>{7};
        },
        [](const uint8_t*, size_t) -> std::optional<GltfImage> {
            GltfImage image;
            image.width = 1;
            image.height = 1;
            image.channels = 4;
            image.pixels = {255, 255, 255, 255};
            return image;
        });

    ASSERT_NE(nullptr, model);
    ASSERT_EQ(1u, model->primitives.size());
    const GltfPrimitive& primitive = model->primitives[0];
    ASSERT_TRUE(primitive.baseColorTexture);
    EXPECT_EQ(1, primitive.baseColorTexture->texCoord);
    ASSERT_EQ(3u, primitive.vertexTexCoords[1].size());
    EXPECT_NEAR(1.0f, primitive.vertexTexCoords[1][1][0], 1e-6f);
    EXPECT_NEAR(0.0f, primitive.vertexTexCoords[1][1][1], 1e-6f);
}

TEST(GltfParserTest, RejectsMalformedKhrTextureTransformPayloads) {
    struct TransformCase {
        const char* payload;
        const char* label;
    };

    const std::array<TransformCase, 6> cases = {{
        {"true", "non-object transform"},
        {"{\"offset\":[0.25]}", "short offset"},
        {"{\"scale\":[2,\"bad\"]}", "non-numeric scale"},
        {"{\"rotation\":\"90\"}", "string rotation"},
        {"{\"texCoord\":1.5}", "non-integer texCoord"},
        {"{\"texCoord\":-1}", "negative texCoord"}}};

    const std::string transformMarker =
        "\"KHR_texture_transform\":{\"offset\":[0.25,0.5],"
        "\"scale\":[2,3],\"rotation\":1.57079632679}";
    for (const TransformCase& testCase : cases) {
        ExternalGltfFixture fixture =
            makeFullMaterialExternalBufferTriangleGltf();
        const size_t transformPos = fixture.jsonText.find(transformMarker);
        ASSERT_NE(std::string::npos, transformPos) << testCase.label;
        fixture.jsonText.replace(
            transformPos,
            transformMarker.size(),
            std::string("\"KHR_texture_transform\":") + testCase.payload);

        std::unique_ptr<GltfModel> model =
            parseExternalFixtureWithSolidImage(fixture);

        EXPECT_EQ(nullptr, model) << testCase.label;
    }
}

TEST(GltfParserTest, RejectsUnknownTextureInfoExtensionSibling) {
    ExternalGltfFixture fixture = makeFullMaterialExternalBufferTriangleGltf();
    const std::string transformMarker =
        "\"KHR_texture_transform\":{\"offset\":[0.25,0.5],"
        "\"scale\":[2,3],\"rotation\":1.57079632679}";
    const size_t transformPos = fixture.jsonText.find(transformMarker);
    ASSERT_NE(std::string::npos, transformPos);
    fixture.jsonText.replace(
        transformPos,
        transformMarker.size(),
        transformMarker + ",\"KHR_unknown_texture_info\":{}");

    std::unique_ptr<GltfModel> model =
        parseExternalFixtureWithSolidImage(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsTextureTexCoordWhenPrimitiveMissingSet) {
    ExternalGltfFixture fixture = makeFullMaterialExternalBufferTriangleGltf();
    const std::string marker = "\"rotation\":1.57079632679";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.insert(markerPos + marker.size(), ",\"texCoord\":1");

    std::unique_ptr<GltfModel> model = GltfParser::parse(
        reinterpret_cast<const uint8_t*>(fixture.jsonText.data()),
        fixture.jsonText.size(),
        [&](const std::string& uri) {
            if (uri == "triangle.bin") return fixture.bin;
            return std::vector<uint8_t>{7};
        },
        [](const uint8_t*, size_t) -> std::optional<GltfImage> {
            GltfImage image;
            image.width = 1;
            image.height = 1;
            image.channels = 4;
            image.pixels = {255, 255, 255, 255};
            return image;
        });

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsBaseColorTextureWhenImageCannotDecode) {
    const ExternalGltfFixture fixture =
        makeTexturedExternalBufferTriangleGltf(
            "data:image/png;base64,AQIDBA==");

    std::unique_ptr<GltfModel> model = GltfParser::parse(
        reinterpret_cast<const uint8_t*>(fixture.jsonText.data()),
        fixture.jsonText.size(),
        [&](const std::string& uri) {
            return uri == "triangle.bin" ? fixture.bin
                                         : std::vector<uint8_t>{};
        });

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, ParsesExternalBufferGltfWithResolver) {
    const ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    ASSERT_NE(nullptr, model);
    ASSERT_EQ(1u, model->primitives.size());
    EXPECT_EQ(3u, model->vertexCount());
    const SurfaceVertex& first = model->primitives[0].vertices[0];
    EXPECT_NEAR(10.0, first.positionEcef.x(), 1e-12);
    EXPECT_NEAR(20.0, first.positionEcef.y(), 1e-12);
    EXPECT_NEAR(30.0, first.positionEcef.z(), 1e-12);
}

TEST(GltfParserTest, NormalizesNodeRotationQuaternion) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string marker = "{\"mesh\":0,\"translation\":[10,20,30]}";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.replace(
        markerPos,
        marker.size(),
        "{\"mesh\":0,\"translation\":[10,20,30],"
        "\"rotation\":[0,0,1.4142135623730951,1.4142135623730951]}");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    ASSERT_NE(nullptr, model);
    ASSERT_EQ(1u, model->primitives.size());
    ASSERT_EQ(3u, model->primitives[0].vertices.size());
    const SurfaceVertex& second = model->primitives[0].vertices[1];
    EXPECT_NEAR(10.0, second.positionEcef.x(), 1e-12);
    EXPECT_NEAR(21.0, second.positionEcef.y(), 1e-12);
    EXPECT_NEAR(30.0, second.positionEcef.z(), 1e-12);
}

TEST(GltfParserTest, RejectsNodeZeroLengthRotationQuaternion) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string marker = "{\"mesh\":0,\"translation\":[10,20,30]}";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.replace(
        markerPos,
        marker.size(),
        "{\"mesh\":0,\"translation\":[10,20,30],"
        "\"rotation\":[0,0,0,0]}");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsNodeTranslationTypeMismatch) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string marker = "\"translation\":[10,20,30]";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.replace(markerPos, marker.size(), "\"translation\":7");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsUnsupportedCamerasArray) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string marker = "\"asset\":{\"version\":\"2.0\"},";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.insert(
        markerPos + marker.size(),
        "\"cameras\":[{\"type\":\"perspective\","
        "\"perspective\":{\"yfov\":1.0,\"znear\":0.1}}],");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsUnsupportedNodeCameraReference) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string marker =
        "{\"mesh\":0,\"translation\":[10,20,30]}";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.replace(
        markerPos,
        marker.size(),
        "{\"mesh\":0,\"camera\":0,\"translation\":[10,20,30]}");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsNodeMatrixWithTrs) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string marker =
        "{\"mesh\":0,\"translation\":[10,20,30]}";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.replace(
        markerPos,
        marker.size(),
        "{\"mesh\":0,\"translation\":[10,20,30],"
        "\"matrix\":[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1]}");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsNodeMeshIndexOutOfRange) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string marker = "\"nodes\":[{\"mesh\":0";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.replace(
        markerPos,
        marker.size(),
        "\"nodes\":[{\"mesh\":7");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsNodeChildrenIndexOutOfRange) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string marker =
        "{\"mesh\":0,\"translation\":[10,20,30]}";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.replace(
        markerPos,
        marker.size(),
        "{\"mesh\":0,\"translation\":[10,20,30],\"children\":[7]}");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsNodeChildCycle) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string marker =
        "\"nodes\":[{\"mesh\":0,\"translation\":[10,20,30]}]";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.replace(
        markerPos,
        marker.size(),
        "\"nodes\":[{\"mesh\":0,\"translation\":[10,20,30]},"
        "{\"children\":[2]},{\"children\":[1]}]");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsNodeDuplicateChildReference) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string marker =
        "\"nodes\":[{\"mesh\":0,\"translation\":[10,20,30]}]";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.replace(
        markerPos,
        marker.size(),
        "\"nodes\":[{\"mesh\":0,\"translation\":[10,20,30],"
        "\"children\":[1,1]},{}]");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsNodeWithMultipleParents) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string marker =
        "\"nodes\":[{\"mesh\":0,\"translation\":[10,20,30]}]";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.replace(
        markerPos,
        marker.size(),
        "\"nodes\":[{\"mesh\":0,\"translation\":[10,20,30],"
        "\"children\":[2]},{\"children\":[2]},{}]");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsSceneRootNodeThatIsAlsoAChild) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string sceneMarker = "\"scenes\":[{\"nodes\":[0]}]";
    const size_t scenePos = fixture.jsonText.find(sceneMarker);
    ASSERT_NE(std::string::npos, scenePos);
    fixture.jsonText.replace(
        scenePos,
        sceneMarker.size(),
        "\"scenes\":[{\"nodes\":[0,1]}]");
    const std::string nodeMarker =
        "\"nodes\":[{\"mesh\":0,\"translation\":[10,20,30]}]";
    const size_t nodePos = fixture.jsonText.find(nodeMarker);
    ASSERT_NE(std::string::npos, nodePos);
    fixture.jsonText.replace(
        nodePos,
        nodeMarker.size(),
        "\"nodes\":[{\"mesh\":0,\"translation\":[10,20,30],"
        "\"children\":[1]},{}]");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsNodeSkinWithoutMesh) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string assetMarker = "\"asset\":{\"version\":\"2.0\"},";
    const size_t assetPos = fixture.jsonText.find(assetMarker);
    ASSERT_NE(std::string::npos, assetPos);
    fixture.jsonText.insert(assetPos + assetMarker.size(), "\"skins\":[{\"joints\":[0]}],");
    const std::string nodeMarker =
        "{\"mesh\":0,\"translation\":[10,20,30]}";
    const size_t nodePos = fixture.jsonText.find(nodeMarker);
    ASSERT_NE(std::string::npos, nodePos);
    fixture.jsonText.replace(nodePos, nodeMarker.size(), "{\"skin\":0}");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsMeshWithoutPrimitives) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string marker =
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"NORMAL\":1,\"TEXCOORD_0\":2},\"indices\":3,\"mode\":4}]}]";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.replace(markerPos, marker.size(), "\"meshes\":[{}]");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsMeshPrimitiveElementTypeMismatch) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string marker =
        "{\"attributes\":{\"POSITION\":0,\"NORMAL\":1,\"TEXCOORD_0\":2},\"indices\":3,\"mode\":4}";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.replace(markerPos, marker.size(), "7");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsUnreferencedUnsupportedPrimitiveMode) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string primitive =
        "{\"attributes\":{\"POSITION\":0,\"NORMAL\":1,\"TEXCOORD_0\":2},"
        "\"indices\":3,\"mode\":4}";
    const std::string marker =
        "\"meshes\":[{\"primitives\":[" + primitive + "]}]";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    const std::string linePrimitive =
        "{\"attributes\":{\"POSITION\":0,\"NORMAL\":1,\"TEXCOORD_0\":2},"
        "\"indices\":3,\"mode\":7}";
    fixture.jsonText.replace(
        markerPos,
        marker.size(),
        "\"meshes\":[{\"primitives\":[" + primitive +
            "]},{\"primitives\":[" + linePrimitive + "]}]");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsUnreferencedPrimitiveMissingPosition) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    appendUnreferencedMeshToExternalFixture(
        fixture,
        "{\"attributes\":{\"NORMAL\":1},\"mode\":4}");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsUnreferencedAttributeCountMismatch) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    appendAccessorToExternalFixture(
        fixture,
        "{\"bufferView\":1,\"componentType\":5126,\"count\":1,\"type\":\"VEC3\"}");
    appendUnreferencedMeshToExternalFixture(
        fixture,
        "{\"attributes\":{\"POSITION\":0,\"NORMAL\":4},\"mode\":4}");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsUnreferencedIndexModeCountMismatch) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    appendUnreferencedMeshToExternalFixture(
        fixture,
        "{\"attributes\":{\"POSITION\":0},\"indices\":3,\"mode\":1}");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsUnreferencedMeshMorphWeightCountMismatch) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    appendUnreferencedMeshToExternalFixture(
        fixture,
        "{\"attributes\":{\"POSITION\":0},\"mode\":4,"
        "\"targets\":[{\"POSITION\":0}]}",
        "\"weights\":[1.0,0.0],");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsUnreferencedUnknownCorePrimitiveAttributeSemantic) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string primitive =
        "{\"attributes\":{\"POSITION\":0,\"NORMAL\":1,\"TEXCOORD_0\":2},"
        "\"indices\":3,\"mode\":4}";
    const std::string marker =
        "\"meshes\":[{\"primitives\":[" + primitive + "]}]";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    const std::string invalidPrimitive =
        "{\"attributes\":{\"POSITION\":0,\"BINORMAL\":1},\"mode\":4}";
    fixture.jsonText.replace(
        markerPos,
        marker.size(),
        "\"meshes\":[{\"primitives\":[" + primitive +
            "]},{\"primitives\":[" + invalidPrimitive + "]}]");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsUnreferencedMalformedNumberedAttributeSemantic) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string primitive =
        "{\"attributes\":{\"POSITION\":0,\"NORMAL\":1,\"TEXCOORD_0\":2},"
        "\"indices\":3,\"mode\":4}";
    const std::string marker =
        "\"meshes\":[{\"primitives\":[" + primitive + "]}]";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    const std::string invalidPrimitive =
        "{\"attributes\":{\"POSITION\":0,\"TEXCOORD_01\":2},\"mode\":4}";
    fixture.jsonText.replace(
        markerPos,
        marker.size(),
        "\"meshes\":[{\"primitives\":[" + primitive +
            "]},{\"primitives\":[" + invalidPrimitive + "]}]");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsUnreferencedUnsupportedMorphTargetSemantic) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string primitive =
        "{\"attributes\":{\"POSITION\":0,\"NORMAL\":1,\"TEXCOORD_0\":2},"
        "\"indices\":3,\"mode\":4}";
    const std::string marker =
        "\"meshes\":[{\"primitives\":[" + primitive + "]}]";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    const std::string invalidPrimitive =
        "{\"attributes\":{\"POSITION\":0},\"mode\":4,"
        "\"targets\":[{\"COLOR_0\":0}]}";
    fixture.jsonText.replace(
        markerPos,
        marker.size(),
        "\"meshes\":[{\"primitives\":[" + primitive +
            "]},{\"primitives\":[" + invalidPrimitive + "]}]");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsUnreferencedFeatureIdMorphTargetSemantic) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string primitive =
        "{\"attributes\":{\"POSITION\":0,\"NORMAL\":1,\"TEXCOORD_0\":2},"
        "\"indices\":3,\"mode\":4}";
    const std::string marker =
        "\"meshes\":[{\"primitives\":[" + primitive + "]}]";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    const std::string invalidPrimitive =
        "{\"attributes\":{\"POSITION\":0},\"mode\":4,"
        "\"targets\":[{\"_FEATURE_ID_0\":0}]}";
    fixture.jsonText.replace(
        markerPos,
        marker.size(),
        "\"meshes\":[{\"primitives\":[" + primitive +
            "]},{\"primitives\":[" + invalidPrimitive + "]}]");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsPrimitiveMaterialIndexOutOfRange) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string marker =
        "{\"attributes\":{\"POSITION\":0,\"NORMAL\":1,\"TEXCOORD_0\":2},\"indices\":3,\"mode\":4}";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.replace(
        markerPos,
        marker.size(),
        "{\"attributes\":{\"POSITION\":0,\"NORMAL\":1,\"TEXCOORD_0\":2},"
        "\"indices\":3,\"mode\":4,\"material\":7}");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsSkinJointsTypeMismatch) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string marker = "\"asset\":{\"version\":\"2.0\"},";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.insert(
        markerPos + marker.size(),
        "\"skins\":[{\"joints\":{}}],");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsSkinInverseBindMatricesTypeMismatch) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string marker = "\"asset\":{\"version\":\"2.0\"},";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.insert(
        markerPos + marker.size(),
        "\"skins\":[{\"joints\":[0],\"inverseBindMatrices\":\"4\"}],");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsScenesTypeMismatch) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string marker = "\"scenes\":[{\"nodes\":[0]}]";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.replace(markerPos, marker.size(), "\"scenes\":{}");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsSceneNodeIndexOutOfRange) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string marker = "\"scenes\":[{\"nodes\":[0]}]";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.replace(
        markerPos,
        marker.size(),
        "\"scenes\":[{\"nodes\":[7]}]");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsDefaultSceneIndexOutOfRange) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string marker = "\"scene\":0";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.replace(markerPos, marker.size(), "\"scene\":7");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsBuffersTypeMismatch) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string marker =
        "\"buffers\":[{\"uri\":\"triangle.bin\",\"byteLength\":" +
        std::to_string(fixture.bin.size()) + "}]";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.replace(markerPos, marker.size(), "\"buffers\":{}");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsBufferUriTypeMismatch) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string marker = "\"uri\":\"triangle.bin\"";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.replace(markerPos, marker.size(), "\"uri\":7");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsBufferDataUriWithoutBase64EncodingAndDoesNotResolve) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string marker = "\"uri\":\"triangle.bin\"";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.replace(
        markerPos,
        marker.size(),
        "\"uri\":\"data:application/octet-stream,AAAA\"");
    bool resolvedDataUri = false;

    std::unique_ptr<GltfModel> model = GltfParser::parse(
        reinterpret_cast<const uint8_t*>(fixture.jsonText.data()),
        fixture.jsonText.size(),
        [&](const std::string& uri) {
            if (uri.rfind("data:", 0) == 0) {
                resolvedDataUri = true;
            }
            return fixture.bin;
        });

    EXPECT_EQ(nullptr, model);
    EXPECT_FALSE(resolvedDataUri);
}

TEST(GltfParserTest, RejectsBufferDataUriWithInvalidBase64AndDoesNotResolve) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string marker = "\"uri\":\"triangle.bin\"";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.replace(
        markerPos,
        marker.size(),
        "\"uri\":\"data:application/octet-stream;base64,@@@@\"");
    bool resolvedDataUri = false;

    std::unique_ptr<GltfModel> model = GltfParser::parse(
        reinterpret_cast<const uint8_t*>(fixture.jsonText.data()),
        fixture.jsonText.size(),
        [&](const std::string& uri) {
            if (uri.rfind("data:", 0) == 0) {
                resolvedDataUri = true;
            }
            return fixture.bin;
        });

    EXPECT_EQ(nullptr, model);
    EXPECT_FALSE(resolvedDataUri);
}

TEST(GltfParserTest, RejectsBufferDataUriWithTrailingBytesAfterPadding) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    std::string encoded = base64Encode(fixture.bin);
    ASSERT_EQ('=', encoded.back());
    encoded += "AAAA";
    const std::string marker = "\"uri\":\"triangle.bin\"";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.replace(
        markerPos,
        marker.size(),
        "\"uri\":\"data:application/octet-stream;base64," + encoded + "\"");
    bool resolvedDataUri = false;

    std::unique_ptr<GltfModel> model = GltfParser::parse(
        reinterpret_cast<const uint8_t*>(fixture.jsonText.data()),
        fixture.jsonText.size(),
        [&](const std::string& uri) {
            if (uri.rfind("data:", 0) == 0) {
                resolvedDataUri = true;
            }
            return fixture.bin;
        });

    EXPECT_EQ(nullptr, model);
    EXPECT_FALSE(resolvedDataUri);
}

TEST(GltfParserTest, RejectsBufferDataUriWithMissingRequiredPadding) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    std::string encoded = base64Encode(fixture.bin);
    ASSERT_EQ('=', encoded.back());
    encoded.pop_back();
    const std::string marker = "\"uri\":\"triangle.bin\"";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.replace(
        markerPos,
        marker.size(),
        "\"uri\":\"data:application/octet-stream;base64," + encoded + "\"");
    bool resolvedDataUri = false;

    std::unique_ptr<GltfModel> model = GltfParser::parse(
        reinterpret_cast<const uint8_t*>(fixture.jsonText.data()),
        fixture.jsonText.size(),
        [&](const std::string& uri) {
            if (uri.rfind("data:", 0) == 0) {
                resolvedDataUri = true;
            }
            return fixture.bin;
        });

    EXPECT_EQ(nullptr, model);
    EXPECT_FALSE(resolvedDataUri);
}

TEST(GltfParserTest, RejectsBufferByteLengthTypeMismatch) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string marker =
        "\"byteLength\":" + std::to_string(fixture.bin.size());
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.replace(
        markerPos,
        marker.size(),
        "\"byteLength\":\"" + std::to_string(fixture.bin.size()) + "\"");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsBufferDeclaredLengthExceedsResolvedBytes) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string marker =
        "\"byteLength\":" + std::to_string(fixture.bin.size());
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.replace(
        markerPos,
        marker.size(),
        "\"byteLength\":" + std::to_string(fixture.bin.size() + 1u));

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsExternalBufferDeclaredLengthSmallerThanResolvedBytes) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    fixture.bin.insert(fixture.bin.end(), {0u, 1u, 2u, 3u});

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, ParsesBufferDataUriWithExactDeclaredLengthAndDoesNotResolve) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string marker = "\"uri\":\"triangle.bin\"";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.replace(
        markerPos,
        marker.size(),
        "\"uri\":\"data:application/octet-stream;base64," +
            base64Encode(fixture.bin) + "\"");
    bool resolvedDataUri = false;

    std::unique_ptr<GltfModel> model = GltfParser::parse(
        reinterpret_cast<const uint8_t*>(fixture.jsonText.data()),
        fixture.jsonText.size(),
        [&](const std::string& uri) {
            if (uri.rfind("data:", 0) == 0) {
                resolvedDataUri = true;
            }
            return fixture.bin;
        });

    ASSERT_NE(nullptr, model);
    EXPECT_FALSE(resolvedDataUri);
    EXPECT_EQ(3u, model->vertexCount());
    EXPECT_EQ(3u, model->indexCount());
}

TEST(GltfParserTest, RejectsBufferDataUriDeclaredLengthSmallerThanDecodedBytes) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    std::vector<uint8_t> decoded = fixture.bin;
    decoded.insert(decoded.end(), {0u, 1u, 2u, 3u});
    const std::string marker = "\"uri\":\"triangle.bin\"";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.replace(
        markerPos,
        marker.size(),
        "\"uri\":\"data:application/octet-stream;base64," +
            base64Encode(decoded) + "\"");
    bool resolvedDataUri = false;

    std::unique_ptr<GltfModel> model = GltfParser::parse(
        reinterpret_cast<const uint8_t*>(fixture.jsonText.data()),
        fixture.jsonText.size(),
        [&](const std::string& uri) {
            if (uri.rfind("data:", 0) == 0) {
                resolvedDataUri = true;
            }
            return fixture.bin;
        });

    EXPECT_EQ(nullptr, model);
    EXPECT_FALSE(resolvedDataUri);
}

TEST(GltfParserTest, ParsesCustomPrimitiveAttributeWithValidAccessor) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string marker = "\"TEXCOORD_0\":2";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.insert(markerPos + marker.size(), ",\"_TEMPERATURE\":2");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    ASSERT_NE(nullptr, model);
    ASSERT_EQ(1u, model->primitives.size());
    EXPECT_EQ(3u, model->vertexCount());
}

TEST(GltfParserTest, RejectsCustomPrimitiveAttributeWithInvalidAccessor) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string marker = "\"TEXCOORD_0\":2";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.insert(markerPos + marker.size(), ",\"_BROKEN\":99");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsUnknownCorePrimitiveAttributeSemantic) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string marker = "\"TEXCOORD_0\":2";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.insert(markerPos + marker.size(), ",\"BINORMAL\":2");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsUnsupportedStandardAttributeSets) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string marker = "\"TEXCOORD_0\":2";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.insert(
        markerPos + marker.size(),
        ",\"COLOR_1\":2,\"JOINTS_1\":2,\"WEIGHTS_1\":2");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsMalformedNumberedPrimitiveAttributeSemantic) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string marker = "\"TEXCOORD_0\":2";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.insert(markerPos + marker.size(), ",\"TEXCOORD_00\":2");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsUnsupportedRequiredExtension) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string marker = "\"asset\":{\"version\":\"2.0\"},";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.insert(
        markerPos + marker.size(),
        "\"extensionsRequired\":[\"KHR_draco_mesh_compression\"],");

    std::unique_ptr<GltfModel> model = GltfParser::parse(
        reinterpret_cast<const uint8_t*>(fixture.jsonText.data()),
        fixture.jsonText.size(),
        [&](const std::string& uri) {
            return uri == "triangle.bin" ? fixture.bin
                                         : std::vector<uint8_t>{};
        });

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsUnsupportedCompressionAndTextureExtensions) {
    const std::array<const char*, 10> unsupportedExtensions = {
        "KHR_draco_mesh_compression",
        "EXT_meshopt_compression",
        "KHR_meshopt_compression",
        "KHR_texture_basisu",
        "EXT_texture_avif",
        "MSFT_texture_dds",
        "KHR_texture_video",
        "KHR_texture_procedurals",
        "EXT_texture_procedurals_mx_1_39",
        "MPEG_texture_video"};

    for (const char* extension : unsupportedExtensions) {
        ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
        const std::string marker = "\"asset\":{\"version\":\"2.0\"},";
        const size_t markerPos = fixture.jsonText.find(marker);
        ASSERT_NE(std::string::npos, markerPos);
        fixture.jsonText.insert(
            markerPos + marker.size(),
            std::string("\"extensionsUsed\":[\"") + extension + "\"],");

        std::unique_ptr<GltfModel> model = GltfParser::parse(
            reinterpret_cast<const uint8_t*>(fixture.jsonText.data()),
            fixture.jsonText.size(),
            [&](const std::string& uri) {
                return uri == "triangle.bin" ? fixture.bin
                                             : std::vector<uint8_t>{};
            });

        EXPECT_EQ(nullptr, model) << extension;
    }
}

TEST(GltfParserTest, RejectsUnsupportedFeatureMetadataExtensions) {
    const std::array<const char*, 4> unsupportedExtensions = {
        "EXT_mesh_features",
        "EXT_instance_features",
        "EXT_structural_metadata",
        "EXT_feature_metadata"};

    for (const char* extension : unsupportedExtensions) {
        ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
        const std::string marker = "\"asset\":{\"version\":\"2.0\"},";
        const size_t markerPos = fixture.jsonText.find(marker);
        ASSERT_NE(std::string::npos, markerPos);
        fixture.jsonText.insert(
            markerPos + marker.size(),
            std::string("\"extensionsUsed\":[\"") + extension + "\"],");

        std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

        EXPECT_EQ(nullptr, model) << extension;
    }
}

TEST(GltfParserTest, RejectsUnsupportedGaussianSplattingExtensions) {
    const std::array<const char*, 4> unsupportedExtensions = {
        "KHR_gaussian_splatting",
        "KHR_gaussian_splatting_compression_spz",
        "KHR_gaussian_splatting_compression_spz_2",
        "KHR_spz_gaussian_splats_compression"};

    for (const char* extension : unsupportedExtensions) {
        ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
        const std::string marker = "\"asset\":{\"version\":\"2.0\"},";
        const size_t markerPos = fixture.jsonText.find(marker);
        ASSERT_NE(std::string::npos, markerPos);
        fixture.jsonText.insert(
            markerPos + marker.size(),
            std::string("\"extensionsUsed\":[\"") + extension + "\"],");

        std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

        EXPECT_EQ(nullptr, model) << extension;
    }
}

TEST(GltfParserTest, RejectsUnsupportedCesiumNativeGeneratedExtensions) {
    const std::array<const char*, 8> unsupportedExtensions = {
        "CESIUM_RTC",
        "CESIUM_tile_edges",
        "EXT_implicit_ellipsoid_region",
        "EXT_implicit_cylinder_region",
        "KHR_implicit_shapes",
        "EXT_mesh_polygon",
        "EXT_primitive_voxels",
        "MAXAR_mesh_variants"};

    for (const char* extension : unsupportedExtensions) {
        ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
        const std::string marker = "\"asset\":{\"version\":\"2.0\"},";
        const size_t markerPos = fixture.jsonText.find(marker);
        ASSERT_NE(std::string::npos, markerPos);
        fixture.jsonText.insert(
            markerPos + marker.size(),
            std::string("\"extensionsUsed\":[\"") + extension + "\"],");

        std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

        EXPECT_EQ(nullptr, model) << extension;
    }
}

TEST(GltfParserTest, RejectsUnsupportedSceneControlAndLightingExtensions) {
    const std::array<const char*, 19> unsupportedExtensions = {
        "KHR_accessor_float64",
        "KHR_animation_pointer",
        "KHR_audio_graph",
        "KHR_collision_shapes",
        "KHR_interactivity",
        "KHR_lights_punctual",
        "KHR_node_hoverability",
        "KHR_node_selectability",
        "KHR_node_visibility",
        "KHR_physics_rigid_bodies",
        "KHR_xmp_json_ld",
        "EXT_lights_ies",
        "EXT_lights_image_based",
        "EXT_mesh_manifold",
        "EXT_mesh_primitive_edge_visibility",
        "EXT_mesh_primitive_restart",
        "AGI_articulations",
        "MSFT_lod",
        "CESIUM_primitive_outline"};

    for (const char* extension : unsupportedExtensions) {
        ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
        const std::string marker = "\"asset\":{\"version\":\"2.0\"},";
        const size_t markerPos = fixture.jsonText.find(marker);
        ASSERT_NE(std::string::npos, markerPos);
        fixture.jsonText.insert(
            markerPos + marker.size(),
            std::string("\"extensionsUsed\":[\"") + extension + "\"],");

        std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

        EXPECT_EQ(nullptr, model) << extension;
    }
}

TEST(GltfParserTest, RejectsUnsupportedVendorRegistryExtensions) {
    const std::array<const char*, 21> unsupportedExtensions = {
        "ADOBE_materials_clearcoat_specular",
        "ADOBE_materials_clearcoat_tint",
        "ADOBE_materials_thin_transparency",
        "AGI_stk_metadata",
        "FB_geometry_metadata",
        "GODOT_single_root",
        "GRIFFEL_bim_data",
        "KHR_techniques_webgl",
        "KHR_xmp",
        "MPEG_accessor_timed",
        "MPEG_animation_timing",
        "MPEG_audio_spatial",
        "MPEG_buffer_circular",
        "MPEG_media",
        "MPEG_mesh_linking",
        "MPEG_scene_dynamic",
        "MPEG_viewport_recommended",
        "EXT_texture_astc",
        "MSFT_packing_normalRoughnessMetallic",
        "MSFT_packing_occlusionRoughnessMetallic",
        "NV_materials_mdl"};

    for (const char* extension : unsupportedExtensions) {
        ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
        const std::string marker = "\"asset\":{\"version\":\"2.0\"},";
        const size_t markerPos = fixture.jsonText.find(marker);
        ASSERT_NE(std::string::npos, markerPos);
        fixture.jsonText.insert(
            markerPos + marker.size(),
            std::string("\"extensionsUsed\":[\"") + extension + "\"],");

        std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

        EXPECT_EQ(nullptr, model) << extension;
    }
}

TEST(GltfParserTest, RejectsUnsupportedNativeMetadataObjectExtensions) {
    struct ObjectExtensionCase {
        const char* marker;
        const char* replacement;
        const char* label;
    };

    const std::array<ObjectExtensionCase, 5> cases = {{
        {
            "\"scene\":0,",
            "\"extensions\":{\"EXT_structural_metadata\":{\"schema\":{\"classes\":{}}}},\"scene\":0,",
            "top-level EXT_structural_metadata"},
        {
            "\"scene\":0,",
            "\"extensions\":{\"EXT_feature_metadata\":{\"schema\":{\"classes\":{}}}},\"scene\":0,",
            "top-level EXT_feature_metadata"},
        {
            "\"nodes\":[{\"mesh\":0,\"translation\":[10,20,30]}]",
            "\"nodes\":[{\"mesh\":0,\"translation\":[10,20,30],"
            "\"extensions\":{\"EXT_instance_features\":{\"featureIds\":[{\"featureCount\":1}]}}}]",
            "node EXT_instance_features"},
        {
            "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"NORMAL\":1,\"TEXCOORD_0\":2},\"indices\":3,\"mode\":4}]}]",
            "\"meshes\":[{\"extensions\":{\"EXT_mesh_features\":{\"featureIds\":[]}},"
            "\"primitives\":[{\"attributes\":{\"POSITION\":0,\"NORMAL\":1,\"TEXCOORD_0\":2},\"indices\":3,\"mode\":4}]}]",
            "mesh EXT_mesh_features"},
        {
            "\"mode\":4",
            "\"mode\":4,\"extensions\":{\"EXT_structural_metadata\":{\"propertyAttributes\":[0]}}",
            "primitive EXT_structural_metadata"},
    }};

    for (const ObjectExtensionCase& testCase : cases) {
        ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
        const size_t markerPos = fixture.jsonText.find(testCase.marker);
        ASSERT_NE(std::string::npos, markerPos) << testCase.label;
        fixture.jsonText.replace(
            markerPos,
            std::string(testCase.marker).size(),
            testCase.replacement);

        std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

        EXPECT_EQ(nullptr, model) << testCase.label;
    }
}

TEST(GltfParserTest, RejectsUnsupportedCesiumNativeGeneratedObjectExtensions) {
    struct ObjectExtensionCase {
        const char* marker;
        const char* replacement;
        const char* label;
    };

    const std::array<ObjectExtensionCase, 6> cases = {{
        {
            "\"scene\":0,",
            "\"extensions\":{\"CESIUM_RTC\":{\"center\":[1,2,3]}},\"scene\":0,",
            "top-level CESIUM_RTC"},
        {
            "\"scene\":0,",
            "\"extensions\":{\"KHR_implicit_shapes\":{\"shapes\":[{"
            "\"type\":\"ellipsoid\",\"extensions\":{"
            "\"EXT_implicit_ellipsoid_region\":{"
            "\"semiMajorAxisRadius\":6378137,"
            "\"semiMinorAxisRadius\":6356752.314245}}}]}},\"scene\":0,",
            "top-level KHR_implicit_shapes"},
        {
            "\"nodes\":[{\"mesh\":0,\"translation\":[10,20,30]}]",
            "\"nodes\":[{\"mesh\":0,\"translation\":[10,20,30],"
            "\"extensions\":{\"MAXAR_mesh_variants\":{\"mappings\":[]}}}]",
            "node MAXAR_mesh_variants"},
        {
            "\"mode\":4",
            "\"mode\":4,\"extensions\":{\"CESIUM_tile_edges\":{"
            "\"left\":3,\"bottom\":3,\"right\":3,\"top\":3}}",
            "primitive CESIUM_tile_edges"},
        {
            "\"mode\":4",
            "\"mode\":4,\"extensions\":{\"EXT_mesh_polygon\":{"
            "\"count\":1,\"loopIndices\":3,\"loopIndicesOffsets\":3,"
            "\"indicesOffsets\":3}}",
            "primitive EXT_mesh_polygon"},
        {
            "\"mode\":4",
            "\"mode\":4,\"extensions\":{\"EXT_primitive_voxels\":{"
            "\"shape\":0,\"dimensions\":[1,1,1]}}",
            "primitive EXT_primitive_voxels"},
    }};

    for (const ObjectExtensionCase& testCase : cases) {
        ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
        const size_t markerPos = fixture.jsonText.find(testCase.marker);
        ASSERT_NE(std::string::npos, markerPos) << testCase.label;
        fixture.jsonText.replace(
            markerPos,
            std::string(testCase.marker).size(),
            testCase.replacement);

        std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

        EXPECT_EQ(nullptr, model) << testCase.label;
    }
}

TEST(GltfParserTest, RejectsUnsupportedRuntimeObjectExtensions) {
    struct ObjectExtensionCase {
        const char* marker;
        const char* replacement;
        const char* label;
    };

    const std::array<ObjectExtensionCase, 7> cases = {{
        {
            "\"scene\":0,",
            "\"extensions\":{\"KHR_lights_punctual\":{\"lights\":[]}},\"scene\":0,",
            "top-level KHR_lights_punctual"},
        {
            "\"scene\":0,",
            "\"extensions\":{\"KHR_xmp_json_ld\":{\"packets\":[]}},\"scene\":0,",
            "top-level KHR_xmp_json_ld"},
        {
            "\"nodes\":[{\"mesh\":0,\"translation\":[10,20,30]}]",
            "\"nodes\":[{\"mesh\":0,\"translation\":[10,20,30],"
            "\"extensions\":{\"KHR_lights_punctual\":{\"light\":0}}}]",
            "node KHR_lights_punctual"},
        {
            "\"nodes\":[{\"mesh\":0,\"translation\":[10,20,30]}]",
            "\"nodes\":[{\"mesh\":0,\"translation\":[10,20,30],"
            "\"extensions\":{\"KHR_node_visibility\":{\"visible\":false}}}]",
            "node KHR_node_visibility"},
        {
            "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"NORMAL\":1,\"TEXCOORD_0\":2},\"indices\":3,\"mode\":4}]}]",
            "\"meshes\":[{\"extensions\":{\"EXT_mesh_manifold\":{\"manifold\":true}},"
            "\"primitives\":[{\"attributes\":{\"POSITION\":0,\"NORMAL\":1,\"TEXCOORD_0\":2},\"indices\":3,\"mode\":4}]}]",
            "mesh EXT_mesh_manifold"},
        {
            "\"mode\":4",
            "\"mode\":4,\"extensions\":{\"CESIUM_primitive_outline\":{\"indices\":3}}",
            "primitive CESIUM_primitive_outline"},
        {
            "\"buffers\":[",
            "\"materials\":[{\"extensions\":{\"KHR_materials_iridescence\":{\"iridescenceFactor\":1.0}}}],"
            "\"buffers\":[",
            "material KHR_materials_iridescence"},
    }};

    for (const ObjectExtensionCase& testCase : cases) {
        ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
        const size_t markerPos = fixture.jsonText.find(testCase.marker);
        ASSERT_NE(std::string::npos, markerPos) << testCase.label;
        fixture.jsonText.replace(
            markerPos,
            std::string(testCase.marker).size(),
            testCase.replacement);

        std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

        EXPECT_EQ(nullptr, model) << testCase.label;
    }
}

TEST(GltfParserTest, ParsesExtrasWithExtensionNamedApplicationData) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string marker = "\"scene\":0,";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.insert(
        markerPos,
        "\"extras\":{\"extensions\":{"
        "\"KHR_materials_unlit\":{\"note\":true},"
        "\"EXT_structural_metadata\":{\"ignored\":true}}},");
    const std::string nodeMarker =
        "{\"mesh\":0,\"translation\":[10,20,30]}";
    const size_t nodePos = fixture.jsonText.find(nodeMarker);
    ASSERT_NE(std::string::npos, nodePos);
    fixture.jsonText.replace(
        nodePos,
        nodeMarker.size(),
        "{\"mesh\":0,\"translation\":[10,20,30],"
        "\"extras\":{\"extensionsUsed\":[\"KHR_materials_unlit\"],"
        "\"extensions\":{\"EXT_structural_metadata\":{\"ignored\":true}}}}");
    const std::string primitiveMarker = "\"mode\":4";
    const size_t primitivePos = fixture.jsonText.find(primitiveMarker);
    ASSERT_NE(std::string::npos, primitivePos);
    fixture.jsonText.replace(
        primitivePos,
        primitiveMarker.size(),
        "\"mode\":4,\"extras\":{\"extensions\":{"
        "\"3DTILES_metadata\":{\"ignored\":true}}}");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    ASSERT_NE(nullptr, model);
    ASSERT_EQ(1u, model->primitives.size());
}

TEST(GltfParserTest, RejectsUnsupportedMaterialExtensionsWithoutRuntimeSupport) {
    const std::array<const char*, 6> unsupportedExtensions = {
        "KHR_materials_diffuse_transmission",
        "KHR_materials_dispersion",
        "KHR_materials_iridescence",
        "KHR_materials_subsurface",
        "KHR_materials_variants",
        "KHR_materials_volume"};

    for (const char* extension : unsupportedExtensions) {
        ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
        const std::string marker = "\"asset\":{\"version\":\"2.0\"},";
        const size_t markerPos = fixture.jsonText.find(marker);
        ASSERT_NE(std::string::npos, markerPos);
        fixture.jsonText.insert(
            markerPos + marker.size(),
            std::string("\"extensionsUsed\":[\"") + extension + "\"],");

        std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

        EXPECT_EQ(nullptr, model) << extension;
    }
}

TEST(GltfParserTest, RejectsDeclaredUnsupportedMaterialExtensionPayloads) {
    const std::array<const char*, 5> unsupportedExtensions = {
        "KHR_materials_diffuse_transmission",
        "KHR_materials_dispersion",
        "KHR_materials_iridescence",
        "KHR_materials_subsurface",
        "KHR_materials_volume"};

    for (const char* extension : unsupportedExtensions) {
        ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
        const std::string assetMarker = "\"asset\":{\"version\":\"2.0\"},";
        const size_t assetPos = fixture.jsonText.find(assetMarker);
        ASSERT_NE(std::string::npos, assetPos) << extension;
        fixture.jsonText.insert(
            assetPos + assetMarker.size(),
            std::string("\"extensionsUsed\":[\"") + extension + "\"],"
            "\"extensionsRequired\":[\"" + extension + "\"],");

        const std::string primitiveMarker = "\"mode\":4}";
        const size_t primitivePos = fixture.jsonText.find(primitiveMarker);
        ASSERT_NE(std::string::npos, primitivePos) << extension;
        fixture.jsonText.replace(
            primitivePos,
            primitiveMarker.size(),
            "\"mode\":4,\"material\":0}");

        const std::string buffersMarker = "\"buffers\"";
        const size_t buffersPos = fixture.jsonText.find(buffersMarker);
        ASSERT_NE(std::string::npos, buffersPos) << extension;
        fixture.jsonText.insert(
            buffersPos,
            std::string("\"materials\":[{\"extensions\":{\"") +
                extension + "\":{}}}],");

        std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

        EXPECT_EQ(nullptr, model) << extension;
    }
}

TEST(GltfParserTest, RejectsDeclaredKhrMaterialsVariantsPayload) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string assetMarker = "\"asset\":{\"version\":\"2.0\"},";
    const size_t assetPos = fixture.jsonText.find(assetMarker);
    ASSERT_NE(std::string::npos, assetPos);
    fixture.jsonText.insert(
        assetPos + assetMarker.size(),
        "\"extensionsUsed\":[\"KHR_materials_variants\"],"
        "\"extensionsRequired\":[\"KHR_materials_variants\"],");

    const std::string sceneMarker = "\"scene\":0,";
    const size_t scenePos = fixture.jsonText.find(sceneMarker);
    ASSERT_NE(std::string::npos, scenePos);
    fixture.jsonText.insert(
        scenePos,
        "\"extensions\":{\"KHR_materials_variants\":{"
        "\"variants\":[{\"name\":\"default\"}]}},");

    const std::string primitiveMarker = "\"mode\":4}";
    const size_t primitivePos = fixture.jsonText.find(primitiveMarker);
    ASSERT_NE(std::string::npos, primitivePos);
    fixture.jsonText.replace(
        primitivePos,
        primitiveMarker.size(),
        "\"mode\":4,\"material\":0,"
        "\"extensions\":{\"KHR_materials_variants\":{"
        "\"mappings\":[{\"material\":0,\"variants\":[0]}]}}}");

    const std::string buffersMarker = "\"buffers\"";
    const size_t buffersPos = fixture.jsonText.find(buffersMarker);
    ASSERT_NE(std::string::npos, buffersPos);
    fixture.jsonText.insert(buffersPos, "\"materials\":[{}],");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, ParsesExtMeshGpuInstancingNodeExtension) {
    ExternalGltfFixture fixture = makeGpuInstancedExternalGltf();
    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    ASSERT_NE(nullptr, model);
    ASSERT_EQ(1u, model->primitives.size());
    const GltfPrimitive& primitive = model->primitives[0];
    ASSERT_EQ(2u, primitive.instances.size());

    const Vec3 source = primitive.vertices[1].positionEcef;
    const Vec3 first = primitive.instances[0].transform * source;
    EXPECT_NEAR(13.0, first.x(), 1e-6);
    EXPECT_NEAR(22.0, first.y(), 1e-6);
    EXPECT_NEAR(33.0, first.z(), 1e-6);

    const Vec3 second = primitive.instances[1].transform * source;
    EXPECT_NEAR(14.0, second.x(), 1e-6);
    EXPECT_NEAR(26.0, second.y(), 1e-6);
    EXPECT_NEAR(36.0, second.z(), 1e-6);
}

TEST(GltfParserTest, ParsesExtMeshGpuInstancingUnderParentTransform) {
    ExternalGltfFixture fixture = makeGpuInstancedExternalGltf();
    const std::string nodeMarker =
        "\"nodes\":[{\"mesh\":0,\"translation\":[10,20,30],"
        "\"extensions\":{\"EXT_mesh_gpu_instancing\":{"
        "\"attributes\":{\"TRANSLATION\":4,\"ROTATION\":5,\"SCALE\":6}}}}]";
    const size_t nodePos = fixture.jsonText.find(nodeMarker);
    ASSERT_NE(std::string::npos, nodePos);
    fixture.jsonText.replace(
        nodePos,
        nodeMarker.size(),
        "\"nodes\":["
        "{\"translation\":[100,0,0],"
        "\"rotation\":[0,0,0.7071067811865476,0.7071067811865476],"
        "\"children\":[1]},"
        "{\"mesh\":0,\"translation\":[10,20,30],"
        "\"extensions\":{\"EXT_mesh_gpu_instancing\":{"
        "\"attributes\":{\"TRANSLATION\":4,\"ROTATION\":5,\"SCALE\":6}}}}]");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    ASSERT_NE(nullptr, model);
    ASSERT_EQ(1u, model->primitives.size());
    const GltfPrimitive& primitive = model->primitives[0];
    ASSERT_EQ(2u, primitive.instances.size());

    const Vec3 source = primitive.vertices[1].positionEcef;
    EXPECT_NEAR(80.0, source.x(), 1e-6);
    EXPECT_NEAR(11.0, source.y(), 1e-6);
    EXPECT_NEAR(30.0, source.z(), 1e-6);

    const Vec3 first = primitive.instances[0].transform * source;
    EXPECT_NEAR(78.0, first.x(), 1e-6);
    EXPECT_NEAR(13.0, first.y(), 1e-6);
    EXPECT_NEAR(33.0, first.z(), 1e-6);

    const Vec3 second = primitive.instances[1].transform * source;
    EXPECT_NEAR(74.0, second.x(), 1e-5);
    EXPECT_NEAR(14.0, second.y(), 1e-5);
    EXPECT_NEAR(36.0, second.z(), 1e-6);
}

TEST(GltfParserTest, RejectsExtMeshGpuInstancingUnderSingularNodeTransform) {
    ExternalGltfFixture fixture = makeGpuInstancedExternalGltf();
    const std::string nodeMarker =
        "{\"mesh\":0,\"translation\":[10,20,30],";
    const size_t nodePos = fixture.jsonText.find(nodeMarker);
    ASSERT_NE(std::string::npos, nodePos);
    fixture.jsonText.replace(
        nodePos,
        nodeMarker.size(),
        "{\"mesh\":0,\"translation\":[10,20,30],\"scale\":[0,1,1],");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, ParsesExtMeshGpuInstancingNormalizedByteRotation) {
    ExternalGltfFixture fixture = makeGpuInstancedExternalGltf(
        GpuInstanceRotationEncoding::NormalizedByte);
    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    ASSERT_NE(nullptr, model);
    ASSERT_EQ(1u, model->primitives.size());
    const GltfPrimitive& primitive = model->primitives[0];
    ASSERT_EQ(2u, primitive.instances.size());

    const Vec3 source = primitive.vertices[1].positionEcef;
    const Vec3 second = primitive.instances[1].transform * source;
    EXPECT_NEAR(14.0, second.x(), 1e-5);
    EXPECT_NEAR(26.0, second.y(), 1e-5);
    EXPECT_NEAR(36.0, second.z(), 1e-6);
}

TEST(GltfParserTest, ParsesExtMeshGpuInstancingNormalizedShortRotation) {
    ExternalGltfFixture fixture = makeGpuInstancedExternalGltf(
        GpuInstanceRotationEncoding::NormalizedShort);
    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    ASSERT_NE(nullptr, model);
    ASSERT_EQ(1u, model->primitives.size());
    const GltfPrimitive& primitive = model->primitives[0];
    ASSERT_EQ(2u, primitive.instances.size());

    const Vec3 source = primitive.vertices[1].positionEcef;
    const Vec3 second = primitive.instances[1].transform * source;
    EXPECT_NEAR(14.0, second.x(), 1e-5);
    EXPECT_NEAR(26.0, second.y(), 1e-5);
    EXPECT_NEAR(36.0, second.z(), 1e-3);
}

TEST(GltfParserTest, ParsesExtMeshGpuInstancingNormalizedUnsignedByteRotation) {
    ExternalGltfFixture fixture = makeGpuInstancedExternalGltf(
        GpuInstanceRotationEncoding::NormalizedUnsignedByte);
    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    ASSERT_NE(nullptr, model);
    ASSERT_EQ(1u, model->primitives.size());
    const GltfPrimitive& primitive = model->primitives[0];
    ASSERT_EQ(2u, primitive.instances.size());

    const Vec3 source = primitive.vertices[1].positionEcef;
    const Vec3 second = primitive.instances[1].transform * source;
    EXPECT_NEAR(14.0, second.x(), 1e-5);
    EXPECT_NEAR(26.0, second.y(), 1e-5);
    EXPECT_NEAR(36.0, second.z(), 1e-6);
}

TEST(GltfParserTest, ParsesExtMeshGpuInstancingNormalizedUnsignedShortRotation) {
    ExternalGltfFixture fixture = makeGpuInstancedExternalGltf(
        GpuInstanceRotationEncoding::NormalizedUnsignedShort);
    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    ASSERT_NE(nullptr, model);
    ASSERT_EQ(1u, model->primitives.size());
    const GltfPrimitive& primitive = model->primitives[0];
    ASSERT_EQ(2u, primitive.instances.size());

    const Vec3 source = primitive.vertices[1].positionEcef;
    const Vec3 second = primitive.instances[1].transform * source;
    EXPECT_NEAR(14.0, second.x(), 1e-5);
    EXPECT_NEAR(26.0, second.y(), 1e-5);
    EXPECT_NEAR(36.0, second.z(), 1e-6);
}

TEST(GltfParserTest, RejectsGpuInstancingZeroLengthRotation) {
    ExternalGltfFixture fixture = makeGpuInstancedExternalGltf(
        GpuInstanceRotationEncoding::ZeroFloat);
    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsUnreferencedGpuInstancingMismatchedAttributeCounts) {
    ExternalGltfFixture fixture = makeGpuInstancedExternalGltf();
    const std::string scaleAccessor =
        "{\"bufferView\":6,\"componentType\":5126,\"count\":2,\"type\":\"VEC3\"}";
    const size_t accessorPos = fixture.jsonText.find(scaleAccessor);
    ASSERT_NE(std::string::npos, accessorPos);
    fixture.jsonText.replace(
        accessorPos,
        scaleAccessor.size(),
        scaleAccessor +
            ",{\"bufferView\":4,\"componentType\":5126,\"count\":1,"
            "\"type\":\"VEC3\"}");

    const std::string nodesMarker =
        "\"nodes\":[{\"mesh\":0,\"translation\":[10,20,30],"
        "\"extensions\":{\"EXT_mesh_gpu_instancing\":{"
        "\"attributes\":{\"TRANSLATION\":4,\"ROTATION\":5,\"SCALE\":6}}}}]";
    const size_t nodesPos = fixture.jsonText.find(nodesMarker);
    ASSERT_NE(std::string::npos, nodesPos);
    fixture.jsonText.replace(
        nodesPos,
        nodesMarker.size(),
        "\"nodes\":[{\"mesh\":0,\"translation\":[10,20,30],"
        "\"extensions\":{\"EXT_mesh_gpu_instancing\":{"
        "\"attributes\":{\"TRANSLATION\":4,\"ROTATION\":5,\"SCALE\":6}}}},"
        "{\"mesh\":0,\"extensions\":{\"EXT_mesh_gpu_instancing\":{"
        "\"attributes\":{\"TRANSLATION\":7,\"ROTATION\":5}}}}]");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsUnreferencedGpuInstancingZeroLengthRotation) {
    ExternalGltfFixture fixture = makeGpuInstancedExternalGltf();
    const size_t originalByteLength = fixture.bin.size();
    const size_t rotationsOffset = fixture.bin.size();
    for (int instance = 0; instance < 2; ++instance) {
        for (int component = 0; component < 4; ++component) {
            appendF32(fixture.bin, 0.0f);
        }
    }
    const size_t rotationsByteLength = fixture.bin.size() - rotationsOffset;
    pad4(fixture.bin, 0);

    const std::string oldByteLength =
        "\"byteLength\":" + std::to_string(originalByteLength);
    const size_t byteLengthPos = fixture.jsonText.find(oldByteLength);
    ASSERT_NE(std::string::npos, byteLengthPos);
    fixture.jsonText.replace(
        byteLengthPos,
        oldByteLength.size(),
        "\"byteLength\":" + std::to_string(fixture.bin.size()));

    const std::string accessorsMarker = "],\"accessors\":[";
    const size_t accessorsPos = fixture.jsonText.find(accessorsMarker);
    ASSERT_NE(std::string::npos, accessorsPos);
    fixture.jsonText.insert(
        accessorsPos,
        ",{\"buffer\":0,\"byteOffset\":" +
            std::to_string(rotationsOffset) +
            ",\"byteLength\":" +
            std::to_string(rotationsByteLength) + "}");

    const std::string scaleAccessor =
        "{\"bufferView\":6,\"componentType\":5126,\"count\":2,\"type\":\"VEC3\"}";
    const size_t accessorPos = fixture.jsonText.find(scaleAccessor);
    ASSERT_NE(std::string::npos, accessorPos);
    fixture.jsonText.replace(
        accessorPos,
        scaleAccessor.size(),
        scaleAccessor +
            ",{\"bufferView\":7,\"componentType\":5126,\"count\":2,"
            "\"type\":\"VEC4\"}");

    const std::string nodesMarker =
        "\"nodes\":[{\"mesh\":0,\"translation\":[10,20,30],"
        "\"extensions\":{\"EXT_mesh_gpu_instancing\":{"
        "\"attributes\":{\"TRANSLATION\":4,\"ROTATION\":5,\"SCALE\":6}}}}]";
    const size_t nodesPos = fixture.jsonText.find(nodesMarker);
    ASSERT_NE(std::string::npos, nodesPos);
    fixture.jsonText.replace(
        nodesPos,
        nodesMarker.size(),
        "\"nodes\":[{\"mesh\":0,\"translation\":[10,20,30],"
        "\"extensions\":{\"EXT_mesh_gpu_instancing\":{"
        "\"attributes\":{\"TRANSLATION\":4,\"ROTATION\":5,\"SCALE\":6}}}},"
        "{\"mesh\":0,\"extensions\":{\"EXT_mesh_gpu_instancing\":{"
        "\"attributes\":{\"TRANSLATION\":4,\"ROTATION\":7}}}}]");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsExtMeshGpuInstancingDeclarationWithoutNodeExtension) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string marker = "\"asset\":{\"version\":\"2.0\"},";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.insert(
        markerPos + marker.size(),
        "\"extensionsUsed\":[\"EXT_mesh_gpu_instancing\"],");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsGpuInstancingObjectExtensionWithoutDeclaration) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string marker =
        "{\"mesh\":0,\"translation\":[10,20,30]}";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.replace(
        markerPos,
        marker.size(),
        "{\"mesh\":0,\"translation\":[10,20,30],"
        "\"extensions\":{\"EXT_mesh_gpu_instancing\":{"
        "\"attributes\":{\"TRANSLATION\":0}}}}");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsKhrTextureTransformObjectExtensionWithoutDeclaration) {
    ExternalGltfFixture fixture =
        makeTexturedExternalBufferTriangleGltf("image.bin");
    const std::string marker =
        "\"baseColorTexture\":{\"index\":0,\"texCoord\":0}";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.replace(
        markerPos,
        marker.size(),
        "\"baseColorTexture\":{\"index\":0,\"texCoord\":0,"
        "\"extensions\":{\"KHR_texture_transform\":{\"scale\":[2,2]}}}");

    std::unique_ptr<GltfModel> model =
        parseExternalFixtureWithSolidImage(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsKhrMaterialsUnlitObjectExtensionWithoutDeclaration) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string primitiveMarker = "\"mode\":4}";
    const size_t primitivePos = fixture.jsonText.find(primitiveMarker);
    ASSERT_NE(std::string::npos, primitivePos);
    fixture.jsonText.replace(
        primitivePos,
        primitiveMarker.size(),
        "\"mode\":4,\"material\":0}");

    const std::string buffersMarker = "\"buffers\"";
    const size_t buffersPos = fixture.jsonText.find(buffersMarker);
    ASSERT_NE(std::string::npos, buffersPos);
    fixture.jsonText.insert(
        buffersPos,
        "\"materials\":[{\"extensions\":{"
        "\"KHR_materials_unlit\":{}}}],");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsKhrMaterialsEmissiveStrengthObjectExtensionWithoutDeclaration) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string primitiveMarker = "\"mode\":4}";
    const size_t primitivePos = fixture.jsonText.find(primitiveMarker);
    ASSERT_NE(std::string::npos, primitivePos);
    fixture.jsonText.replace(
        primitivePos,
        primitiveMarker.size(),
        "\"mode\":4,\"material\":0}");

    const std::string buffersMarker = "\"buffers\"";
    const size_t buffersPos = fixture.jsonText.find(buffersMarker);
    ASSERT_NE(std::string::npos, buffersPos);
    fixture.jsonText.insert(
        buffersPos,
        "\"materials\":[{\"extensions\":{"
        "\"KHR_materials_emissive_strength\":{\"emissiveStrength\":2.0}}}],");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsKhrMaterialsIorObjectExtensionWithoutDeclaration) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string primitiveMarker = "\"mode\":4}";
    const size_t primitivePos = fixture.jsonText.find(primitiveMarker);
    ASSERT_NE(std::string::npos, primitivePos);
    fixture.jsonText.replace(
        primitivePos,
        primitiveMarker.size(),
        "\"mode\":4,\"material\":0}");

    const std::string buffersMarker = "\"buffers\"";
    const size_t buffersPos = fixture.jsonText.find(buffersMarker);
    ASSERT_NE(std::string::npos, buffersPos);
    fixture.jsonText.insert(
        buffersPos,
        "\"materials\":[{\"extensions\":{"
        "\"KHR_materials_ior\":{\"ior\":2.0}}}],");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsKhrMaterialsPbrSpecularGlossinessObjectExtensionWithoutDeclaration) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string primitiveMarker = "\"mode\":4}";
    const size_t primitivePos = fixture.jsonText.find(primitiveMarker);
    ASSERT_NE(std::string::npos, primitivePos);
    fixture.jsonText.replace(
        primitivePos,
        primitiveMarker.size(),
        "\"mode\":4,\"material\":0}");

    const std::string buffersMarker = "\"buffers\"";
    const size_t buffersPos = fixture.jsonText.find(buffersMarker);
    ASSERT_NE(std::string::npos, buffersPos);
    fixture.jsonText.insert(
        buffersPos,
        "\"materials\":[{\"extensions\":{"
        "\"KHR_materials_pbrSpecularGlossiness\":{"
        "\"glossinessFactor\":0.5}}}],");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsKhrMaterialsTransmissionObjectExtensionWithoutDeclaration) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string primitiveMarker = "\"mode\":4}";
    const size_t primitivePos = fixture.jsonText.find(primitiveMarker);
    ASSERT_NE(std::string::npos, primitivePos);
    fixture.jsonText.replace(
        primitivePos,
        primitiveMarker.size(),
        "\"mode\":4,\"material\":0}");

    const std::string buffersMarker = "\"buffers\"";
    const size_t buffersPos = fixture.jsonText.find(buffersMarker);
    ASSERT_NE(std::string::npos, buffersPos);
    fixture.jsonText.insert(
        buffersPos,
        "\"materials\":[{\"extensions\":{"
        "\"KHR_materials_transmission\":{"
        "\"transmissionFactor\":0.5}}}],");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsKhrMaterialsAnisotropyObjectExtensionWithoutDeclaration) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string primitiveMarker = "\"mode\":4}";
    const size_t primitivePos = fixture.jsonText.find(primitiveMarker);
    ASSERT_NE(std::string::npos, primitivePos);
    fixture.jsonText.replace(
        primitivePos,
        primitiveMarker.size(),
        "\"mode\":4,\"material\":0}");

    const std::string buffersMarker = "\"buffers\"";
    const size_t buffersPos = fixture.jsonText.find(buffersMarker);
    ASSERT_NE(std::string::npos, buffersPos);
    fixture.jsonText.insert(
        buffersPos,
        "\"materials\":[{\"extensions\":{"
        "\"KHR_materials_anisotropy\":{"
        "\"anisotropyStrength\":0.5}}}],");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsKhrMaterialsSpecularObjectExtensionWithoutDeclaration) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string primitiveMarker = "\"mode\":4}";
    const size_t primitivePos = fixture.jsonText.find(primitiveMarker);
    ASSERT_NE(std::string::npos, primitivePos);
    fixture.jsonText.replace(
        primitivePos,
        primitiveMarker.size(),
        "\"mode\":4,\"material\":0}");

    const std::string buffersMarker = "\"buffers\"";
    const size_t buffersPos = fixture.jsonText.find(buffersMarker);
    ASSERT_NE(std::string::npos, buffersPos);
    fixture.jsonText.insert(
        buffersPos,
        "\"materials\":[{\"extensions\":{"
        "\"KHR_materials_specular\":{\"specularFactor\":0.5}}}],");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsKhrMaterialsClearcoatObjectExtensionWithoutDeclaration) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string primitiveMarker = "\"mode\":4}";
    const size_t primitivePos = fixture.jsonText.find(primitiveMarker);
    ASSERT_NE(std::string::npos, primitivePos);
    fixture.jsonText.replace(
        primitivePos,
        primitiveMarker.size(),
        "\"mode\":4,\"material\":0}");

    const std::string buffersMarker = "\"buffers\"";
    const size_t buffersPos = fixture.jsonText.find(buffersMarker);
    ASSERT_NE(std::string::npos, buffersPos);
    fixture.jsonText.insert(
        buffersPos,
        "\"materials\":[{\"extensions\":{"
        "\"KHR_materials_clearcoat\":{\"clearcoatFactor\":0.5}}}],");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsKhrMaterialsSheenObjectExtensionWithoutDeclaration) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string primitiveMarker = "\"mode\":4}";
    const size_t primitivePos = fixture.jsonText.find(primitiveMarker);
    ASSERT_NE(std::string::npos, primitivePos);
    fixture.jsonText.replace(
        primitivePos,
        primitiveMarker.size(),
        "\"mode\":4,\"material\":0}");

    const std::string buffersMarker = "\"buffers\"";
    const size_t buffersPos = fixture.jsonText.find(buffersMarker);
    ASSERT_NE(std::string::npos, buffersPos);
    fixture.jsonText.insert(
        buffersPos,
        "\"materials\":[{\"extensions\":{"
        "\"KHR_materials_sheen\":{"
        "\"sheenColorFactor\":[0.2,0.4,0.6]}}}],");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsGpuInstancingFeatureIdAttributeWithoutMetadataSupport) {
    ExternalGltfFixture fixture = makeGpuInstancedExternalGltf();
    const std::string marker = "\"SCALE\":6}";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.replace(
        markerPos,
        marker.size(),
        "\"SCALE\":6,\"_FEATURE_ID_0\":4}");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsGpuInstancingCustomAttributeWithoutRendererSupport) {
    ExternalGltfFixture fixture = makeGpuInstancedExternalGltf();
    const std::string marker = "\"SCALE\":6}";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.replace(
        markerPos,
        marker.size(),
        "\"SCALE\":6,\"_CUSTOM\":4}");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsGpuInstancingEmptyAttributes) {
    ExternalGltfFixture fixture = makeGpuInstancedExternalGltf();
    const std::string marker =
        "\"attributes\":{\"TRANSLATION\":4,\"ROTATION\":5,\"SCALE\":6}";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.replace(markerPos, marker.size(), "\"attributes\":{}");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsGpuInstancingMismatchedAttributeCounts) {
    ExternalGltfFixture fixture = makeGpuInstancedExternalGltf();
    const std::string marker =
        "{\"bufferView\":6,\"componentType\":5126,\"count\":2,\"type\":\"VEC3\"}";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.replace(
        markerPos,
        marker.size(),
        "{\"bufferView\":6,\"componentType\":5126,\"count\":1,\"type\":\"VEC3\"}");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsGpuInstancingNonFloatTranslationAccessor) {
    ExternalGltfFixture fixture = makeGpuInstancedExternalGltf();
    const std::string marker =
        "{\"bufferView\":4,\"componentType\":5126,\"count\":2,\"type\":\"VEC3\"}";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.replace(
        markerPos,
        marker.size(),
        "{\"bufferView\":4,\"componentType\":5123,\"normalized\":true,"
        "\"count\":2,\"type\":\"VEC3\"}");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsGpuInstancingWrongScaleAccessorType) {
    ExternalGltfFixture fixture = makeGpuInstancedExternalGltf();
    const std::string marker =
        "{\"bufferView\":6,\"componentType\":5126,\"count\":2,\"type\":\"VEC3\"}";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.replace(
        markerPos,
        marker.size(),
        "{\"bufferView\":6,\"componentType\":5126,\"count\":2,\"type\":\"VEC4\"}");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsGpuInstancingUnnormalizedUnsignedRotationAccessor) {
    ExternalGltfFixture fixture = makeGpuInstancedExternalGltf();
    const std::string marker =
        "{\"bufferView\":5,\"componentType\":5126,\"count\":2,\"type\":\"VEC4\"}";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.replace(
        markerPos,
        marker.size(),
        "{\"bufferView\":5,\"componentType\":5121,\"count\":2,\"type\":\"VEC4\"}");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsGpuInstancingWithRuntimeAnimations) {
    ExternalGltfFixture fixture = makeGpuInstancedExternalGltf();
    const std::string marker = "\"buffers\"";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.insert(markerPos, "\"animations\":[{}],");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsGpuInstancingOnSkinnedNode) {
    ExternalGltfFixture fixture = makeGpuInstancedExternalGltf();
    const std::string nodeMarker =
        "{\"mesh\":0,\"translation\":[10,20,30],";
    const size_t nodePos = fixture.jsonText.find(nodeMarker);
    ASSERT_NE(std::string::npos, nodePos);
    fixture.jsonText.replace(
        nodePos,
        nodeMarker.size(),
        "{\"mesh\":0,\"skin\":0,\"translation\":[10,20,30],");

    const std::string buffersMarker = "\"buffers\"";
    const size_t buffersPos = fixture.jsonText.find(buffersMarker);
    ASSERT_NE(std::string::npos, buffersPos);
    fixture.jsonText.insert(
        buffersPos,
        "\"skins\":[{\"joints\":[0]}],");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsKhrTextureTransformDeclarationWithoutTextureInfoExtension) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string marker = "\"asset\":{\"version\":\"2.0\"},";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.insert(
        markerPos + marker.size(),
        "\"extensionsUsed\":[\"KHR_texture_transform\"],");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsKhrMaterialsUnlitDeclarationWithoutMaterialExtension) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string marker = "\"asset\":{\"version\":\"2.0\"},";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.insert(
        markerPos + marker.size(),
        "\"extensionsUsed\":[\"KHR_materials_unlit\"],");

    std::unique_ptr<GltfModel> model = GltfParser::parse(
        reinterpret_cast<const uint8_t*>(fixture.jsonText.data()),
        fixture.jsonText.size(),
        [&](const std::string& uri) {
            return uri == "triangle.bin" ? fixture.bin
                                         : std::vector<uint8_t>{};
        });

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsKhrMaterialsEmissiveStrengthDeclarationWithoutMaterialExtension) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string marker = "\"asset\":{\"version\":\"2.0\"},";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.insert(
        markerPos + marker.size(),
        "\"extensionsUsed\":[\"KHR_materials_emissive_strength\"],");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsKhrMaterialsIorDeclarationWithoutMaterialExtension) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string marker = "\"asset\":{\"version\":\"2.0\"},";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.insert(
        markerPos + marker.size(),
        "\"extensionsUsed\":[\"KHR_materials_ior\"],");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsKhrMaterialsPbrSpecularGlossinessDeclarationWithoutMaterialExtension) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string marker = "\"asset\":{\"version\":\"2.0\"},";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.insert(
        markerPos + marker.size(),
        "\"extensionsUsed\":[\"KHR_materials_pbrSpecularGlossiness\"],");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsKhrMaterialsTransmissionDeclarationWithoutMaterialExtension) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string marker = "\"asset\":{\"version\":\"2.0\"},";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.insert(
        markerPos + marker.size(),
        "\"extensionsUsed\":[\"KHR_materials_transmission\"],");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsKhrMaterialsAnisotropyDeclarationWithoutMaterialExtension) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string marker = "\"asset\":{\"version\":\"2.0\"},";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.insert(
        markerPos + marker.size(),
        "\"extensionsUsed\":[\"KHR_materials_anisotropy\"],");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsKhrMaterialsSpecularDeclarationWithoutMaterialExtension) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string marker = "\"asset\":{\"version\":\"2.0\"},";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.insert(
        markerPos + marker.size(),
        "\"extensionsUsed\":[\"KHR_materials_specular\"],");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsKhrMaterialsClearcoatDeclarationWithoutMaterialExtension) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string marker = "\"asset\":{\"version\":\"2.0\"},";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.insert(
        markerPos + marker.size(),
        "\"extensionsUsed\":[\"KHR_materials_clearcoat\"],");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsKhrMaterialsSheenDeclarationWithoutMaterialExtension) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string marker = "\"asset\":{\"version\":\"2.0\"},";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.insert(
        markerPos + marker.size(),
        "\"extensionsUsed\":[\"KHR_materials_sheen\"],");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsDeclaredObjectSupportedExtensionsWithoutPayload) {
    for (std::string_view extensionName : kSupportedGltfObjectExtensions) {
        SCOPED_TRACE(std::string(extensionName));
        ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
        const std::string marker = "\"asset\":{\"version\":\"2.0\"},";
        const size_t markerPos = fixture.jsonText.find(marker);
        ASSERT_NE(std::string::npos, markerPos);
        fixture.jsonText.insert(
            markerPos + marker.size(),
            "\"extensionsUsed\":[\"" + std::string(extensionName) + "\"],");

        std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

        EXPECT_EQ(nullptr, model);
    }
}

TEST(GltfParserTest, RejectsSupportedObjectExtensionsAtTopLevel) {
    for (std::string_view extensionName : kSupportedGltfObjectExtensions) {
        SCOPED_TRACE(std::string(extensionName));
        ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
        const std::string assetMarker = "\"asset\":{\"version\":\"2.0\"},";
        const size_t assetPos = fixture.jsonText.find(assetMarker);
        ASSERT_NE(std::string::npos, assetPos);
        fixture.jsonText.insert(
            assetPos + assetMarker.size(),
            "\"extensionsUsed\":[\"" + std::string(extensionName) + "\"],");

        const std::string sceneMarker = "\"scene\":0,";
        const size_t scenePos = fixture.jsonText.find(sceneMarker);
        ASSERT_NE(std::string::npos, scenePos);
        fixture.jsonText.insert(
            scenePos,
            "\"extensions\":{\"" + std::string(extensionName) + "\":{}},");

        std::unique_ptr<GltfModel> model =
            parseExternalFixtureWithSolidImage(fixture);

        EXPECT_EQ(nullptr, model);
    }
}

TEST(GltfParserTest, RejectsKhrMaterialsUnlitMaterialExtensionTypeMismatch) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string assetMarker = "\"asset\":{\"version\":\"2.0\"},";
    const size_t assetPos = fixture.jsonText.find(assetMarker);
    ASSERT_NE(std::string::npos, assetPos);
    fixture.jsonText.insert(
        assetPos + assetMarker.size(),
        "\"extensionsUsed\":[\"KHR_materials_unlit\"],");

    const std::string primitiveMarker = "\"mode\":4}";
    const size_t primitivePos = fixture.jsonText.find(primitiveMarker);
    ASSERT_NE(std::string::npos, primitivePos);
    fixture.jsonText.replace(
        primitivePos,
        primitiveMarker.size(),
        "\"mode\":4,\"material\":0}");

    const std::string buffersMarker = "\"buffers\"";
    const size_t buffersPos = fixture.jsonText.find(buffersMarker);
    ASSERT_NE(std::string::npos, buffersPos);
    fixture.jsonText.insert(
        buffersPos,
        "\"materials\":[{\"extensions\":{\"KHR_materials_unlit\":true}}],");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsKhrMaterialsEmissiveStrengthTypeMismatch) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string assetMarker = "\"asset\":{\"version\":\"2.0\"},";
    const size_t assetPos = fixture.jsonText.find(assetMarker);
    ASSERT_NE(std::string::npos, assetPos);
    fixture.jsonText.insert(
        assetPos + assetMarker.size(),
        "\"extensionsUsed\":[\"KHR_materials_emissive_strength\"],");

    const std::string primitiveMarker = "\"mode\":4}";
    const size_t primitivePos = fixture.jsonText.find(primitiveMarker);
    ASSERT_NE(std::string::npos, primitivePos);
    fixture.jsonText.replace(
        primitivePos,
        primitiveMarker.size(),
        "\"mode\":4,\"material\":0}");

    const std::string buffersMarker = "\"buffers\"";
    const size_t buffersPos = fixture.jsonText.find(buffersMarker);
    ASSERT_NE(std::string::npos, buffersPos);
    fixture.jsonText.insert(
        buffersPos,
        "\"materials\":[{\"extensions\":{"
        "\"KHR_materials_emissive_strength\":true}}],");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsKhrMaterialsIorTypeMismatch) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string assetMarker = "\"asset\":{\"version\":\"2.0\"},";
    const size_t assetPos = fixture.jsonText.find(assetMarker);
    ASSERT_NE(std::string::npos, assetPos);
    fixture.jsonText.insert(
        assetPos + assetMarker.size(),
        "\"extensionsUsed\":[\"KHR_materials_ior\"],");

    const std::string primitiveMarker = "\"mode\":4}";
    const size_t primitivePos = fixture.jsonText.find(primitiveMarker);
    ASSERT_NE(std::string::npos, primitivePos);
    fixture.jsonText.replace(
        primitivePos,
        primitiveMarker.size(),
        "\"mode\":4,\"material\":0}");

    const std::string buffersMarker = "\"buffers\"";
    const size_t buffersPos = fixture.jsonText.find(buffersMarker);
    ASSERT_NE(std::string::npos, buffersPos);
    fixture.jsonText.insert(
        buffersPos,
        "\"materials\":[{\"extensions\":{\"KHR_materials_ior\":true}}],");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsKhrMaterialsPbrSpecularGlossinessTypeMismatch) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string assetMarker = "\"asset\":{\"version\":\"2.0\"},";
    const size_t assetPos = fixture.jsonText.find(assetMarker);
    ASSERT_NE(std::string::npos, assetPos);
    fixture.jsonText.insert(
        assetPos + assetMarker.size(),
        "\"extensionsUsed\":[\"KHR_materials_pbrSpecularGlossiness\"],");

    const std::string primitiveMarker = "\"mode\":4}";
    const size_t primitivePos = fixture.jsonText.find(primitiveMarker);
    ASSERT_NE(std::string::npos, primitivePos);
    fixture.jsonText.replace(
        primitivePos,
        primitiveMarker.size(),
        "\"mode\":4,\"material\":0}");

    const std::string buffersMarker = "\"buffers\"";
    const size_t buffersPos = fixture.jsonText.find(buffersMarker);
    ASSERT_NE(std::string::npos, buffersPos);
    fixture.jsonText.insert(
        buffersPos,
        "\"materials\":[{\"extensions\":{"
        "\"KHR_materials_pbrSpecularGlossiness\":true}}],");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsKhrMaterialsTransmissionTypeMismatch) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string assetMarker = "\"asset\":{\"version\":\"2.0\"},";
    const size_t assetPos = fixture.jsonText.find(assetMarker);
    ASSERT_NE(std::string::npos, assetPos);
    fixture.jsonText.insert(
        assetPos + assetMarker.size(),
        "\"extensionsUsed\":[\"KHR_materials_transmission\"],");

    const std::string primitiveMarker = "\"mode\":4}";
    const size_t primitivePos = fixture.jsonText.find(primitiveMarker);
    ASSERT_NE(std::string::npos, primitivePos);
    fixture.jsonText.replace(
        primitivePos,
        primitiveMarker.size(),
        "\"mode\":4,\"material\":0}");

    const std::string buffersMarker = "\"buffers\"";
    const size_t buffersPos = fixture.jsonText.find(buffersMarker);
    ASSERT_NE(std::string::npos, buffersPos);
    fixture.jsonText.insert(
        buffersPos,
        "\"materials\":[{\"extensions\":{"
        "\"KHR_materials_transmission\":true}}],");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsKhrMaterialsTransmissionTextureInfoTypeMismatch) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string assetMarker = "\"asset\":{\"version\":\"2.0\"},";
    const size_t assetPos = fixture.jsonText.find(assetMarker);
    ASSERT_NE(std::string::npos, assetPos);
    fixture.jsonText.insert(
        assetPos + assetMarker.size(),
        "\"extensionsUsed\":[\"KHR_materials_transmission\"],");

    const std::string primitiveMarker = "\"mode\":4}";
    const size_t primitivePos = fixture.jsonText.find(primitiveMarker);
    ASSERT_NE(std::string::npos, primitivePos);
    fixture.jsonText.replace(
        primitivePos,
        primitiveMarker.size(),
        "\"mode\":4,\"material\":0}");

    const std::string buffersMarker = "\"buffers\"";
    const size_t buffersPos = fixture.jsonText.find(buffersMarker);
    ASSERT_NE(std::string::npos, buffersPos);
    fixture.jsonText.insert(
        buffersPos,
        "\"materials\":[{\"extensions\":{"
        "\"KHR_materials_transmission\":{"
        "\"transmissionTexture\":true}}}],");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsKhrMaterialsAnisotropyTypeMismatch) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string assetMarker = "\"asset\":{\"version\":\"2.0\"},";
    const size_t assetPos = fixture.jsonText.find(assetMarker);
    ASSERT_NE(std::string::npos, assetPos);
    fixture.jsonText.insert(
        assetPos + assetMarker.size(),
        "\"extensionsUsed\":[\"KHR_materials_anisotropy\"],");

    const std::string primitiveMarker = "\"mode\":4}";
    const size_t primitivePos = fixture.jsonText.find(primitiveMarker);
    ASSERT_NE(std::string::npos, primitivePos);
    fixture.jsonText.replace(
        primitivePos,
        primitiveMarker.size(),
        "\"mode\":4,\"material\":0}");

    const std::string buffersMarker = "\"buffers\"";
    const size_t buffersPos = fixture.jsonText.find(buffersMarker);
    ASSERT_NE(std::string::npos, buffersPos);
    fixture.jsonText.insert(
        buffersPos,
        "\"materials\":[{\"extensions\":{"
        "\"KHR_materials_anisotropy\":true}}],");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsKhrMaterialsSpecularTypeMismatch) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string assetMarker = "\"asset\":{\"version\":\"2.0\"},";
    const size_t assetPos = fixture.jsonText.find(assetMarker);
    ASSERT_NE(std::string::npos, assetPos);
    fixture.jsonText.insert(
        assetPos + assetMarker.size(),
        "\"extensionsUsed\":[\"KHR_materials_specular\"],");

    const std::string primitiveMarker = "\"mode\":4}";
    const size_t primitivePos = fixture.jsonText.find(primitiveMarker);
    ASSERT_NE(std::string::npos, primitivePos);
    fixture.jsonText.replace(
        primitivePos,
        primitiveMarker.size(),
        "\"mode\":4,\"material\":0}");

    const std::string buffersMarker = "\"buffers\"";
    const size_t buffersPos = fixture.jsonText.find(buffersMarker);
    ASSERT_NE(std::string::npos, buffersPos);
    fixture.jsonText.insert(
        buffersPos,
        "\"materials\":[{\"extensions\":{"
        "\"KHR_materials_specular\":true}}],");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsKhrMaterialsClearcoatTypeMismatch) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string assetMarker = "\"asset\":{\"version\":\"2.0\"},";
    const size_t assetPos = fixture.jsonText.find(assetMarker);
    ASSERT_NE(std::string::npos, assetPos);
    fixture.jsonText.insert(
        assetPos + assetMarker.size(),
        "\"extensionsUsed\":[\"KHR_materials_clearcoat\"],");

    const std::string primitiveMarker = "\"mode\":4}";
    const size_t primitivePos = fixture.jsonText.find(primitiveMarker);
    ASSERT_NE(std::string::npos, primitivePos);
    fixture.jsonText.replace(
        primitivePos,
        primitiveMarker.size(),
        "\"mode\":4,\"material\":0}");

    const std::string buffersMarker = "\"buffers\"";
    const size_t buffersPos = fixture.jsonText.find(buffersMarker);
    ASSERT_NE(std::string::npos, buffersPos);
    fixture.jsonText.insert(
        buffersPos,
        "\"materials\":[{\"extensions\":{"
        "\"KHR_materials_clearcoat\":true}}],");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsKhrMaterialsSheenTypeMismatch) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string assetMarker = "\"asset\":{\"version\":\"2.0\"},";
    const size_t assetPos = fixture.jsonText.find(assetMarker);
    ASSERT_NE(std::string::npos, assetPos);
    fixture.jsonText.insert(
        assetPos + assetMarker.size(),
        "\"extensionsUsed\":[\"KHR_materials_sheen\"],");

    const std::string primitiveMarker = "\"mode\":4}";
    const size_t primitivePos = fixture.jsonText.find(primitiveMarker);
    ASSERT_NE(std::string::npos, primitivePos);
    fixture.jsonText.replace(
        primitivePos,
        primitiveMarker.size(),
        "\"mode\":4,\"material\":0}");

    const std::string buffersMarker = "\"buffers\"";
    const size_t buffersPos = fixture.jsonText.find(buffersMarker);
    ASSERT_NE(std::string::npos, buffersPos);
    fixture.jsonText.insert(
        buffersPos,
        "\"materials\":[{\"extensions\":{"
        "\"KHR_materials_sheen\":true}}],");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsKhrMaterialsIorBelowOneNonZeroValue) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string assetMarker = "\"asset\":{\"version\":\"2.0\"},";
    const size_t assetPos = fixture.jsonText.find(assetMarker);
    ASSERT_NE(std::string::npos, assetPos);
    fixture.jsonText.insert(
        assetPos + assetMarker.size(),
        "\"extensionsUsed\":[\"KHR_materials_ior\"],");

    const std::string primitiveMarker = "\"mode\":4}";
    const size_t primitivePos = fixture.jsonText.find(primitiveMarker);
    ASSERT_NE(std::string::npos, primitivePos);
    fixture.jsonText.replace(
        primitivePos,
        primitiveMarker.size(),
        "\"mode\":4,\"material\":0}");

    const std::string buffersMarker = "\"buffers\"";
    const size_t buffersPos = fixture.jsonText.find(buffersMarker);
    ASSERT_NE(std::string::npos, buffersPos);
    fixture.jsonText.insert(
        buffersPos,
        "\"materials\":[{\"extensions\":{"
        "\"KHR_materials_ior\":{\"ior\":0.5}}}],");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsKhrMaterialsPbrSpecularGlossinessFactorOutsideUnitRange) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string assetMarker = "\"asset\":{\"version\":\"2.0\"},";
    const size_t assetPos = fixture.jsonText.find(assetMarker);
    ASSERT_NE(std::string::npos, assetPos);
    fixture.jsonText.insert(
        assetPos + assetMarker.size(),
        "\"extensionsUsed\":[\"KHR_materials_pbrSpecularGlossiness\"],");

    const std::string primitiveMarker = "\"mode\":4}";
    const size_t primitivePos = fixture.jsonText.find(primitiveMarker);
    ASSERT_NE(std::string::npos, primitivePos);
    fixture.jsonText.replace(
        primitivePos,
        primitiveMarker.size(),
        "\"mode\":4,\"material\":0}");

    const std::string buffersMarker = "\"buffers\"";
    const size_t buffersPos = fixture.jsonText.find(buffersMarker);
    ASSERT_NE(std::string::npos, buffersPos);
    fixture.jsonText.insert(
        buffersPos,
        "\"materials\":[{\"extensions\":{"
        "\"KHR_materials_pbrSpecularGlossiness\":{"
        "\"glossinessFactor\":1.5}}}],");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsKhrMaterialsTransmissionFactorOutsideUnitRange) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string assetMarker = "\"asset\":{\"version\":\"2.0\"},";
    const size_t assetPos = fixture.jsonText.find(assetMarker);
    ASSERT_NE(std::string::npos, assetPos);
    fixture.jsonText.insert(
        assetPos + assetMarker.size(),
        "\"extensionsUsed\":[\"KHR_materials_transmission\"],");

    const std::string primitiveMarker = "\"mode\":4}";
    const size_t primitivePos = fixture.jsonText.find(primitiveMarker);
    ASSERT_NE(std::string::npos, primitivePos);
    fixture.jsonText.replace(
        primitivePos,
        primitiveMarker.size(),
        "\"mode\":4,\"material\":0}");

    const std::string buffersMarker = "\"buffers\"";
    const size_t buffersPos = fixture.jsonText.find(buffersMarker);
    ASSERT_NE(std::string::npos, buffersPos);
    fixture.jsonText.insert(
        buffersPos,
        "\"materials\":[{\"extensions\":{"
        "\"KHR_materials_transmission\":{"
        "\"transmissionFactor\":1.5}}}],");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsKhrMaterialsAnisotropyStrengthOutsideUnitRange) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string assetMarker = "\"asset\":{\"version\":\"2.0\"},";
    const size_t assetPos = fixture.jsonText.find(assetMarker);
    ASSERT_NE(std::string::npos, assetPos);
    fixture.jsonText.insert(
        assetPos + assetMarker.size(),
        "\"extensionsUsed\":[\"KHR_materials_anisotropy\"],");

    const std::string primitiveMarker = "\"mode\":4}";
    const size_t primitivePos = fixture.jsonText.find(primitiveMarker);
    ASSERT_NE(std::string::npos, primitivePos);
    fixture.jsonText.replace(
        primitivePos,
        primitiveMarker.size(),
        "\"mode\":4,\"material\":0}");

    const std::string buffersMarker = "\"buffers\"";
    const size_t buffersPos = fixture.jsonText.find(buffersMarker);
    ASSERT_NE(std::string::npos, buffersPos);
    fixture.jsonText.insert(
        buffersPos,
        "\"materials\":[{\"extensions\":{"
        "\"KHR_materials_anisotropy\":{"
        "\"anisotropyStrength\":1.5}}}],");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsKhrMaterialsAnisotropyWithoutTangentSpace) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string assetMarker = "\"asset\":{\"version\":\"2.0\"},";
    const size_t assetPos = fixture.jsonText.find(assetMarker);
    ASSERT_NE(std::string::npos, assetPos);
    fixture.jsonText.insert(
        assetPos + assetMarker.size(),
        "\"extensionsUsed\":[\"KHR_materials_anisotropy\"],");

    const std::string attributesMarker =
        "\"POSITION\":0,\"NORMAL\":1,\"TEXCOORD_0\":2";
    const size_t attributesPos = fixture.jsonText.find(attributesMarker);
    ASSERT_NE(std::string::npos, attributesPos);
    fixture.jsonText.replace(
        attributesPos,
        attributesMarker.size(),
        "\"POSITION\":0,\"NORMAL\":1");

    const std::string primitiveMarker = "\"mode\":4}";
    const size_t primitivePos = fixture.jsonText.find(primitiveMarker);
    ASSERT_NE(std::string::npos, primitivePos);
    fixture.jsonText.replace(
        primitivePos,
        primitiveMarker.size(),
        "\"mode\":4,\"material\":0}");

    const std::string buffersMarker = "\"buffers\"";
    const size_t buffersPos = fixture.jsonText.find(buffersMarker);
    ASSERT_NE(std::string::npos, buffersPos);
    fixture.jsonText.insert(
        buffersPos,
        "\"materials\":[{\"extensions\":{"
        "\"KHR_materials_anisotropy\":{"
        "\"anisotropyStrength\":0.5}}}],");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsKhrMaterialsSpecularFactorOutsideUnitRange) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string assetMarker = "\"asset\":{\"version\":\"2.0\"},";
    const size_t assetPos = fixture.jsonText.find(assetMarker);
    ASSERT_NE(std::string::npos, assetPos);
    fixture.jsonText.insert(
        assetPos + assetMarker.size(),
        "\"extensionsUsed\":[\"KHR_materials_specular\"],");

    const std::string primitiveMarker = "\"mode\":4}";
    const size_t primitivePos = fixture.jsonText.find(primitiveMarker);
    ASSERT_NE(std::string::npos, primitivePos);
    fixture.jsonText.replace(
        primitivePos,
        primitiveMarker.size(),
        "\"mode\":4,\"material\":0}");

    const std::string buffersMarker = "\"buffers\"";
    const size_t buffersPos = fixture.jsonText.find(buffersMarker);
    ASSERT_NE(std::string::npos, buffersPos);
    fixture.jsonText.insert(
        buffersPos,
        "\"materials\":[{\"extensions\":{"
        "\"KHR_materials_specular\":{\"specularFactor\":1.5}}}],");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsKhrMaterialsClearcoatFactorOutsideUnitRange) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string assetMarker = "\"asset\":{\"version\":\"2.0\"},";
    const size_t assetPos = fixture.jsonText.find(assetMarker);
    ASSERT_NE(std::string::npos, assetPos);
    fixture.jsonText.insert(
        assetPos + assetMarker.size(),
        "\"extensionsUsed\":[\"KHR_materials_clearcoat\"],");

    const std::string primitiveMarker = "\"mode\":4}";
    const size_t primitivePos = fixture.jsonText.find(primitiveMarker);
    ASSERT_NE(std::string::npos, primitivePos);
    fixture.jsonText.replace(
        primitivePos,
        primitiveMarker.size(),
        "\"mode\":4,\"material\":0}");

    const std::string buffersMarker = "\"buffers\"";
    const size_t buffersPos = fixture.jsonText.find(buffersMarker);
    ASSERT_NE(std::string::npos, buffersPos);
    fixture.jsonText.insert(
        buffersPos,
        "\"materials\":[{\"extensions\":{"
        "\"KHR_materials_clearcoat\":{\"clearcoatFactor\":1.5}}}],");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsKhrMaterialsSheenFactorOutsideUnitRange) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string assetMarker = "\"asset\":{\"version\":\"2.0\"},";
    const size_t assetPos = fixture.jsonText.find(assetMarker);
    ASSERT_NE(std::string::npos, assetPos);
    fixture.jsonText.insert(
        assetPos + assetMarker.size(),
        "\"extensionsUsed\":[\"KHR_materials_sheen\"],");

    const std::string primitiveMarker = "\"mode\":4}";
    const size_t primitivePos = fixture.jsonText.find(primitiveMarker);
    ASSERT_NE(std::string::npos, primitivePos);
    fixture.jsonText.replace(
        primitivePos,
        primitiveMarker.size(),
        "\"mode\":4,\"material\":0}");

    const std::string buffersMarker = "\"buffers\"";
    const size_t buffersPos = fixture.jsonText.find(buffersMarker);
    ASSERT_NE(std::string::npos, buffersPos);
    fixture.jsonText.insert(
        buffersPos,
        "\"materials\":[{\"extensions\":{"
        "\"KHR_materials_sheen\":{"
        "\"sheenColorFactor\":[1.0,1.2,1.0]}}}],");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsKhrMaterialsSheenRoughnessOutsideUnitRange) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string assetMarker = "\"asset\":{\"version\":\"2.0\"},";
    const size_t assetPos = fixture.jsonText.find(assetMarker);
    ASSERT_NE(std::string::npos, assetPos);
    fixture.jsonText.insert(
        assetPos + assetMarker.size(),
        "\"extensionsUsed\":[\"KHR_materials_sheen\"],");

    const std::string primitiveMarker = "\"mode\":4}";
    const size_t primitivePos = fixture.jsonText.find(primitiveMarker);
    ASSERT_NE(std::string::npos, primitivePos);
    fixture.jsonText.replace(
        primitivePos,
        primitiveMarker.size(),
        "\"mode\":4,\"material\":0}");

    const std::string buffersMarker = "\"buffers\"";
    const size_t buffersPos = fixture.jsonText.find(buffersMarker);
    ASSERT_NE(std::string::npos, buffersPos);
    fixture.jsonText.insert(
        buffersPos,
        "\"materials\":[{\"extensions\":{"
        "\"KHR_materials_sheen\":{"
        "\"sheenRoughnessFactor\":1.5}}}],");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsKhrMaterialsSpecularNegativeColorFactor) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string assetMarker = "\"asset\":{\"version\":\"2.0\"},";
    const size_t assetPos = fixture.jsonText.find(assetMarker);
    ASSERT_NE(std::string::npos, assetPos);
    fixture.jsonText.insert(
        assetPos + assetMarker.size(),
        "\"extensionsUsed\":[\"KHR_materials_specular\"],");

    const std::string primitiveMarker = "\"mode\":4}";
    const size_t primitivePos = fixture.jsonText.find(primitiveMarker);
    ASSERT_NE(std::string::npos, primitivePos);
    fixture.jsonText.replace(
        primitivePos,
        primitiveMarker.size(),
        "\"mode\":4,\"material\":0}");

    const std::string buffersMarker = "\"buffers\"";
    const size_t buffersPos = fixture.jsonText.find(buffersMarker);
    ASSERT_NE(std::string::npos, buffersPos);
    fixture.jsonText.insert(
        buffersPos,
        "\"materials\":[{\"extensions\":{"
        "\"KHR_materials_specular\":{"
        "\"specularColorFactor\":[1.0,-0.1,1.0]}}}],");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsKhrMaterialsEmissiveStrengthNegativeValue) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string assetMarker = "\"asset\":{\"version\":\"2.0\"},";
    const size_t assetPos = fixture.jsonText.find(assetMarker);
    ASSERT_NE(std::string::npos, assetPos);
    fixture.jsonText.insert(
        assetPos + assetMarker.size(),
        "\"extensionsUsed\":[\"KHR_materials_emissive_strength\"],");

    const std::string primitiveMarker = "\"mode\":4}";
    const size_t primitivePos = fixture.jsonText.find(primitiveMarker);
    ASSERT_NE(std::string::npos, primitivePos);
    fixture.jsonText.replace(
        primitivePos,
        primitiveMarker.size(),
        "\"mode\":4,\"material\":0}");

    const std::string buffersMarker = "\"buffers\"";
    const size_t buffersPos = fixture.jsonText.find(buffersMarker);
    ASSERT_NE(std::string::npos, buffersPos);
    fixture.jsonText.insert(
        buffersPos,
        "\"materials\":[{\"extensions\":{"
        "\"KHR_materials_emissive_strength\":{\"emissiveStrength\":-0.1}}}],");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsKhrMaterialsEmissiveStrengthWithUnlitMaterial) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string assetMarker = "\"asset\":{\"version\":\"2.0\"},";
    const size_t assetPos = fixture.jsonText.find(assetMarker);
    ASSERT_NE(std::string::npos, assetPos);
    fixture.jsonText.insert(
        assetPos + assetMarker.size(),
        "\"extensionsUsed\":["
        "\"KHR_materials_unlit\","
        "\"KHR_materials_emissive_strength\"],");

    const std::string primitiveMarker = "\"mode\":4}";
    const size_t primitivePos = fixture.jsonText.find(primitiveMarker);
    ASSERT_NE(std::string::npos, primitivePos);
    fixture.jsonText.replace(
        primitivePos,
        primitiveMarker.size(),
        "\"mode\":4,\"material\":0}");

    const std::string buffersMarker = "\"buffers\"";
    const size_t buffersPos = fixture.jsonText.find(buffersMarker);
    ASSERT_NE(std::string::npos, buffersPos);
    fixture.jsonText.insert(
        buffersPos,
        "\"materials\":[{\"extensions\":{"
        "\"KHR_materials_unlit\":{},"
        "\"KHR_materials_emissive_strength\":{\"emissiveStrength\":2.0}}}],");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsKhrMaterialsIorWithUnlitMaterial) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string assetMarker = "\"asset\":{\"version\":\"2.0\"},";
    const size_t assetPos = fixture.jsonText.find(assetMarker);
    ASSERT_NE(std::string::npos, assetPos);
    fixture.jsonText.insert(
        assetPos + assetMarker.size(),
        "\"extensionsUsed\":["
        "\"KHR_materials_unlit\","
        "\"KHR_materials_ior\"],");

    const std::string primitiveMarker = "\"mode\":4}";
    const size_t primitivePos = fixture.jsonText.find(primitiveMarker);
    ASSERT_NE(std::string::npos, primitivePos);
    fixture.jsonText.replace(
        primitivePos,
        primitiveMarker.size(),
        "\"mode\":4,\"material\":0}");

    const std::string buffersMarker = "\"buffers\"";
    const size_t buffersPos = fixture.jsonText.find(buffersMarker);
    ASSERT_NE(std::string::npos, buffersPos);
    fixture.jsonText.insert(
        buffersPos,
        "\"materials\":[{\"extensions\":{"
        "\"KHR_materials_unlit\":{},"
        "\"KHR_materials_ior\":{\"ior\":2.0}}}],");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsKhrMaterialsPbrSpecularGlossinessWithIorMaterial) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string assetMarker = "\"asset\":{\"version\":\"2.0\"},";
    const size_t assetPos = fixture.jsonText.find(assetMarker);
    ASSERT_NE(std::string::npos, assetPos);
    fixture.jsonText.insert(
        assetPos + assetMarker.size(),
        "\"extensionsUsed\":["
        "\"KHR_materials_pbrSpecularGlossiness\","
        "\"KHR_materials_ior\"],");

    const std::string primitiveMarker = "\"mode\":4}";
    const size_t primitivePos = fixture.jsonText.find(primitiveMarker);
    ASSERT_NE(std::string::npos, primitivePos);
    fixture.jsonText.replace(
        primitivePos,
        primitiveMarker.size(),
        "\"mode\":4,\"material\":0}");

    const std::string buffersMarker = "\"buffers\"";
    const size_t buffersPos = fixture.jsonText.find(buffersMarker);
    ASSERT_NE(std::string::npos, buffersPos);
    fixture.jsonText.insert(
        buffersPos,
        "\"materials\":[{\"extensions\":{"
        "\"KHR_materials_pbrSpecularGlossiness\":{"
        "\"glossinessFactor\":0.5},"
        "\"KHR_materials_ior\":{\"ior\":2.0}}}],");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsKhrMaterialsPbrSpecularGlossinessWithTransmissionMaterial) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string assetMarker = "\"asset\":{\"version\":\"2.0\"},";
    const size_t assetPos = fixture.jsonText.find(assetMarker);
    ASSERT_NE(std::string::npos, assetPos);
    fixture.jsonText.insert(
        assetPos + assetMarker.size(),
        "\"extensionsUsed\":["
        "\"KHR_materials_pbrSpecularGlossiness\","
        "\"KHR_materials_transmission\"],");

    const std::string primitiveMarker = "\"mode\":4}";
    const size_t primitivePos = fixture.jsonText.find(primitiveMarker);
    ASSERT_NE(std::string::npos, primitivePos);
    fixture.jsonText.replace(
        primitivePos,
        primitiveMarker.size(),
        "\"mode\":4,\"material\":0}");

    const std::string buffersMarker = "\"buffers\"";
    const size_t buffersPos = fixture.jsonText.find(buffersMarker);
    ASSERT_NE(std::string::npos, buffersPos);
    fixture.jsonText.insert(
        buffersPos,
        "\"materials\":[{\"extensions\":{"
        "\"KHR_materials_pbrSpecularGlossiness\":{"
        "\"glossinessFactor\":0.5},"
        "\"KHR_materials_transmission\":{"
        "\"transmissionFactor\":0.5}}}],");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsKhrMaterialsPbrSpecularGlossinessWithUnlitMaterial) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string assetMarker = "\"asset\":{\"version\":\"2.0\"},";
    const size_t assetPos = fixture.jsonText.find(assetMarker);
    ASSERT_NE(std::string::npos, assetPos);
    fixture.jsonText.insert(
        assetPos + assetMarker.size(),
        "\"extensionsUsed\":["
        "\"KHR_materials_pbrSpecularGlossiness\","
        "\"KHR_materials_unlit\"],");

    const std::string primitiveMarker = "\"mode\":4}";
    const size_t primitivePos = fixture.jsonText.find(primitiveMarker);
    ASSERT_NE(std::string::npos, primitivePos);
    fixture.jsonText.replace(
        primitivePos,
        primitiveMarker.size(),
        "\"mode\":4,\"material\":0}");

    const std::string buffersMarker = "\"buffers\"";
    const size_t buffersPos = fixture.jsonText.find(buffersMarker);
    ASSERT_NE(std::string::npos, buffersPos);
    fixture.jsonText.insert(
        buffersPos,
        "\"materials\":[{\"extensions\":{"
        "\"KHR_materials_unlit\":{},"
        "\"KHR_materials_pbrSpecularGlossiness\":{"
        "\"glossinessFactor\":0.5}}}],");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsKhrMaterialsTransmissionWithUnlitMaterial) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string assetMarker = "\"asset\":{\"version\":\"2.0\"},";
    const size_t assetPos = fixture.jsonText.find(assetMarker);
    ASSERT_NE(std::string::npos, assetPos);
    fixture.jsonText.insert(
        assetPos + assetMarker.size(),
        "\"extensionsUsed\":["
        "\"KHR_materials_unlit\","
        "\"KHR_materials_transmission\"],");

    const std::string primitiveMarker = "\"mode\":4}";
    const size_t primitivePos = fixture.jsonText.find(primitiveMarker);
    ASSERT_NE(std::string::npos, primitivePos);
    fixture.jsonText.replace(
        primitivePos,
        primitiveMarker.size(),
        "\"mode\":4,\"material\":0}");

    const std::string buffersMarker = "\"buffers\"";
    const size_t buffersPos = fixture.jsonText.find(buffersMarker);
    ASSERT_NE(std::string::npos, buffersPos);
    fixture.jsonText.insert(
        buffersPos,
        "\"materials\":[{\"extensions\":{"
        "\"KHR_materials_unlit\":{},"
        "\"KHR_materials_transmission\":{"
        "\"transmissionFactor\":0.5}}}],");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsKhrMaterialsAnisotropyWithUnlitMaterial) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string assetMarker = "\"asset\":{\"version\":\"2.0\"},";
    const size_t assetPos = fixture.jsonText.find(assetMarker);
    ASSERT_NE(std::string::npos, assetPos);
    fixture.jsonText.insert(
        assetPos + assetMarker.size(),
        "\"extensionsUsed\":["
        "\"KHR_materials_unlit\","
        "\"KHR_materials_anisotropy\"],");

    const std::string primitiveMarker = "\"mode\":4}";
    const size_t primitivePos = fixture.jsonText.find(primitiveMarker);
    ASSERT_NE(std::string::npos, primitivePos);
    fixture.jsonText.replace(
        primitivePos,
        primitiveMarker.size(),
        "\"mode\":4,\"material\":0}");

    const std::string buffersMarker = "\"buffers\"";
    const size_t buffersPos = fixture.jsonText.find(buffersMarker);
    ASSERT_NE(std::string::npos, buffersPos);
    fixture.jsonText.insert(
        buffersPos,
        "\"materials\":[{\"extensions\":{"
        "\"KHR_materials_unlit\":{},"
        "\"KHR_materials_anisotropy\":{"
        "\"anisotropyStrength\":0.5}}}],");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsKhrMaterialsSpecularWithUnlitMaterial) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string assetMarker = "\"asset\":{\"version\":\"2.0\"},";
    const size_t assetPos = fixture.jsonText.find(assetMarker);
    ASSERT_NE(std::string::npos, assetPos);
    fixture.jsonText.insert(
        assetPos + assetMarker.size(),
        "\"extensionsUsed\":["
        "\"KHR_materials_unlit\","
        "\"KHR_materials_specular\"],");

    const std::string primitiveMarker = "\"mode\":4}";
    const size_t primitivePos = fixture.jsonText.find(primitiveMarker);
    ASSERT_NE(std::string::npos, primitivePos);
    fixture.jsonText.replace(
        primitivePos,
        primitiveMarker.size(),
        "\"mode\":4,\"material\":0}");

    const std::string buffersMarker = "\"buffers\"";
    const size_t buffersPos = fixture.jsonText.find(buffersMarker);
    ASSERT_NE(std::string::npos, buffersPos);
    fixture.jsonText.insert(
        buffersPos,
        "\"materials\":[{\"extensions\":{"
        "\"KHR_materials_unlit\":{},"
        "\"KHR_materials_specular\":{\"specularFactor\":0.5}}}],");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsKhrMaterialsClearcoatWithUnlitMaterial) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string assetMarker = "\"asset\":{\"version\":\"2.0\"},";
    const size_t assetPos = fixture.jsonText.find(assetMarker);
    ASSERT_NE(std::string::npos, assetPos);
    fixture.jsonText.insert(
        assetPos + assetMarker.size(),
        "\"extensionsUsed\":["
        "\"KHR_materials_unlit\","
        "\"KHR_materials_clearcoat\"],");

    const std::string primitiveMarker = "\"mode\":4}";
    const size_t primitivePos = fixture.jsonText.find(primitiveMarker);
    ASSERT_NE(std::string::npos, primitivePos);
    fixture.jsonText.replace(
        primitivePos,
        primitiveMarker.size(),
        "\"mode\":4,\"material\":0}");

    const std::string buffersMarker = "\"buffers\"";
    const size_t buffersPos = fixture.jsonText.find(buffersMarker);
    ASSERT_NE(std::string::npos, buffersPos);
    fixture.jsonText.insert(
        buffersPos,
        "\"materials\":[{\"extensions\":{"
        "\"KHR_materials_unlit\":{},"
        "\"KHR_materials_clearcoat\":{\"clearcoatFactor\":0.5}}}],");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsKhrMaterialsSheenWithUnlitMaterial) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string assetMarker = "\"asset\":{\"version\":\"2.0\"},";
    const size_t assetPos = fixture.jsonText.find(assetMarker);
    ASSERT_NE(std::string::npos, assetPos);
    fixture.jsonText.insert(
        assetPos + assetMarker.size(),
        "\"extensionsUsed\":["
        "\"KHR_materials_unlit\","
        "\"KHR_materials_sheen\"],");

    const std::string primitiveMarker = "\"mode\":4}";
    const size_t primitivePos = fixture.jsonText.find(primitiveMarker);
    ASSERT_NE(std::string::npos, primitivePos);
    fixture.jsonText.replace(
        primitivePos,
        primitiveMarker.size(),
        "\"mode\":4,\"material\":0}");

    const std::string buffersMarker = "\"buffers\"";
    const size_t buffersPos = fixture.jsonText.find(buffersMarker);
    ASSERT_NE(std::string::npos, buffersPos);
    fixture.jsonText.insert(
        buffersPos,
        "\"materials\":[{\"extensions\":{"
        "\"KHR_materials_unlit\":{},"
        "\"KHR_materials_sheen\":{"
        "\"sheenColorFactor\":[0.2,0.4,0.6]}}}],");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsExtensionsUsedTypeMismatch) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string marker = "\"asset\":{\"version\":\"2.0\"},";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.insert(
        markerPos + marker.size(),
        "\"extensionsUsed\":{},");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsExtensionsRequiredElementTypeMismatch) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string marker = "\"asset\":{\"version\":\"2.0\"},";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.insert(
        markerPos + marker.size(),
        "\"extensionsRequired\":[7],");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsSupportedRequiredExtensionMissingExtensionsUsed) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string assetMarker = "\"asset\":{\"version\":\"2.0\"},";
    const size_t assetPos = fixture.jsonText.find(assetMarker);
    ASSERT_NE(std::string::npos, assetPos);
    fixture.jsonText.insert(
        assetPos + assetMarker.size(),
        "\"extensionsRequired\":[\"KHR_materials_unlit\"],");

    const std::string primitiveMarker = "\"mode\":4}";
    const size_t primitivePos = fixture.jsonText.find(primitiveMarker);
    ASSERT_NE(std::string::npos, primitivePos);
    fixture.jsonText.replace(
        primitivePos,
        primitiveMarker.size(),
        "\"mode\":4,\"material\":0}");

    const std::string buffersMarker = "\"buffers\"";
    const size_t buffersPos = fixture.jsonText.find(buffersMarker);
    ASSERT_NE(std::string::npos, buffersPos);
    fixture.jsonText.insert(
        buffersPos,
        "\"materials\":[{\"extensions\":{"
        "\"KHR_materials_unlit\":{}}}],");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsDuplicateExtensionsUsedEntries) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string assetMarker = "\"asset\":{\"version\":\"2.0\"},";
    const size_t assetPos = fixture.jsonText.find(assetMarker);
    ASSERT_NE(std::string::npos, assetPos);
    fixture.jsonText.insert(
        assetPos + assetMarker.size(),
        "\"extensionsUsed\":["
        "\"KHR_materials_unlit\","
        "\"KHR_materials_unlit\"],");

    const std::string primitiveMarker = "\"mode\":4}";
    const size_t primitivePos = fixture.jsonText.find(primitiveMarker);
    ASSERT_NE(std::string::npos, primitivePos);
    fixture.jsonText.replace(
        primitivePos,
        primitiveMarker.size(),
        "\"mode\":4,\"material\":0}");

    const std::string buffersMarker = "\"buffers\"";
    const size_t buffersPos = fixture.jsonText.find(buffersMarker);
    ASSERT_NE(std::string::npos, buffersPos);
    fixture.jsonText.insert(
        buffersPos,
        "\"materials\":[{\"extensions\":{"
        "\"KHR_materials_unlit\":{}}}],");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsDuplicateExtensionsRequiredEntries) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string assetMarker = "\"asset\":{\"version\":\"2.0\"},";
    const size_t assetPos = fixture.jsonText.find(assetMarker);
    ASSERT_NE(std::string::npos, assetPos);
    fixture.jsonText.insert(
        assetPos + assetMarker.size(),
        "\"extensionsUsed\":[\"KHR_materials_unlit\"],"
        "\"extensionsRequired\":["
        "\"KHR_materials_unlit\","
        "\"KHR_materials_unlit\"],");

    const std::string primitiveMarker = "\"mode\":4}";
    const size_t primitivePos = fixture.jsonText.find(primitiveMarker);
    ASSERT_NE(std::string::npos, primitivePos);
    fixture.jsonText.replace(
        primitivePos,
        primitiveMarker.size(),
        "\"mode\":4,\"material\":0}");

    const std::string buffersMarker = "\"buffers\"";
    const size_t buffersPos = fixture.jsonText.find(buffersMarker);
    ASSERT_NE(std::string::npos, buffersPos);
    fixture.jsonText.insert(
        buffersPos,
        "\"materials\":[{\"extensions\":{"
        "\"KHR_materials_unlit\":{}}}],");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsFeatureIdAttributeWithoutMetadataSupport) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string marker = "\"TEXCOORD_0\":2";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.insert(markerPos + marker.size(), ",\"_FEATURE_ID_0\":2");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsFeatureIdAttributeWithoutLeadingUnderscore) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string marker = "\"TEXCOORD_0\":2";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.insert(markerPos + marker.size(), ",\"FEATURE_ID_0\":2");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsMalformedFeatureIdAttributePrefix) {
    const std::array<const char*, 5> semantics = {
        "_FEATURE_ID",
        "_FEATURE_IDABC",
        "_FEATURE_ID_00",
        "_FEATURE_ID_8",
        "_FEATURE_ID_0_EXTRA"};

    for (const char* semantic : semantics) {
        ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
        const std::string marker = "\"TEXCOORD_0\":2";
        const size_t markerPos = fixture.jsonText.find(marker);
        ASSERT_NE(std::string::npos, markerPos);
        fixture.jsonText.insert(
            markerPos + marker.size(),
            std::string(",\"") + semantic + "\":2");

        std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

        EXPECT_EQ(nullptr, model) << semantic;
    }
}

TEST(GltfParserTest, RejectsLegacyBatchIdAttributeWithoutMetadataSupport) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string marker = "\"TEXCOORD_0\":2";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.insert(markerPos + marker.size(), ",\"_BATCHID\":2");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsLegacyBatchIdPrefixedAttributeWithoutMetadataSupport) {
    const std::array<const char*, 3> semantics = {
        "_BATCHID_0",
        "_BATCHID_00",
        "_BATCHID_EXTRA"};

    for (const char* semantic : semantics) {
        ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
        const std::string marker = "\"TEXCOORD_0\":2";
        const size_t markerPos = fixture.jsonText.find(marker);
        ASSERT_NE(std::string::npos, markerPos);
        fixture.jsonText.insert(
            markerPos + marker.size(),
            std::string(",\"") + semantic + "\":2");

        std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

        EXPECT_EQ(nullptr, model) << semantic;
    }
}

TEST(GltfParserTest, RejectsMeshFeaturesPrimitiveExtensionWithoutSupport) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string marker = "\"mode\":4";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.replace(
        markerPos,
        marker.size(),
        "\"mode\":4,"
        "\"extensions\":{\"EXT_mesh_features\":{\"featureIds\":[{"
        "\"featureCount\":1,\"attribute\":0,\"propertyTable\":0}]}}");

    std::unique_ptr<GltfModel> model = parseExternalFixture(fixture);

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsUnsupportedDracoPrimitiveExtensionWithoutDecoder) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string marker = "\"mode\":4";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.replace(
        markerPos,
        marker.size(),
        "\"mode\":4,"
        "\"extensions\":{\"KHR_draco_mesh_compression\":{"
        "\"bufferView\":0,\"attributes\":{\"POSITION\":0}}}");

    std::unique_ptr<GltfModel> model = GltfParser::parse(
        reinterpret_cast<const uint8_t*>(fixture.jsonText.data()),
        fixture.jsonText.size(),
        [&](const std::string& uri) {
            return uri == "triangle.bin" ? fixture.bin
                                         : std::vector<uint8_t>{};
        });

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsUnsupportedObjectExtensionWithoutTopLevelDeclaration) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string marker =
        "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36}";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.replace(
        markerPos,
        marker.size(),
        "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36,"
        "\"extensions\":{\"EXT_meshopt_compression\":{"
        "\"buffer\":0,\"byteOffset\":0,\"byteLength\":1,"
        "\"byteStride\":12,\"count\":3,\"mode\":\"ATTRIBUTES\"}}}");

    std::unique_ptr<GltfModel> model = GltfParser::parse(
        reinterpret_cast<const uint8_t*>(fixture.jsonText.data()),
        fixture.jsonText.size(),
        [&](const std::string& uri) {
            return uri == "triangle.bin" ? fixture.bin
                                         : std::vector<uint8_t>{};
        });

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsUnsupportedBasisuTextureExtensionWithoutTranscoder) {
    ExternalGltfFixture fixture =
        makeTexturedExternalBufferTriangleGltf("image.bin");
    const std::string marker =
        "\"textures\":[{\"source\":0,\"sampler\":0}]";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.replace(
        markerPos,
        marker.size(),
        "\"textures\":[{\"source\":0,\"sampler\":0,"
        "\"extensions\":{\"KHR_texture_basisu\":{\"source\":0}}}]");

    std::unique_ptr<GltfModel> model = GltfParser::parse(
        reinterpret_cast<const uint8_t*>(fixture.jsonText.data()),
        fixture.jsonText.size(),
        [&](const std::string& uri) {
            return uri == "triangle.bin" ? fixture.bin
                                         : std::vector<uint8_t>{9, 8, 7, 6};
        });

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsDeclaredDracoPrimitiveExtensionBeforeLoadingBuffers) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string assetMarker = "\"asset\":{\"version\":\"2.0\"},";
    const size_t assetPos = fixture.jsonText.find(assetMarker);
    ASSERT_NE(std::string::npos, assetPos);
    fixture.jsonText.insert(
        assetPos + assetMarker.size(),
        "\"extensionsUsed\":[\"KHR_draco_mesh_compression\"],"
        "\"extensionsRequired\":[\"KHR_draco_mesh_compression\"],");

    const std::string primitiveMarker = "\"mode\":4";
    const size_t primitivePos = fixture.jsonText.find(primitiveMarker);
    ASSERT_NE(std::string::npos, primitivePos);
    fixture.jsonText.replace(
        primitivePos,
        primitiveMarker.size(),
        "\"mode\":4,"
        "\"extensions\":{\"KHR_draco_mesh_compression\":{"
        "\"bufferView\":0,\"attributes\":{\"POSITION\":0}}}");

    bool resolvedResource = false;
    std::unique_ptr<GltfModel> model = GltfParser::parse(
        reinterpret_cast<const uint8_t*>(fixture.jsonText.data()),
        fixture.jsonText.size(),
        [&](const std::string&) {
            resolvedResource = true;
            return fixture.bin;
        });

    EXPECT_EQ(nullptr, model);
    EXPECT_FALSE(resolvedResource);
}

TEST(GltfParserTest, RejectsDeclaredMeshoptBufferViewExtensionBeforeLoadingBuffers) {
    const std::array<const char*, 2> extensionNames = {
        "EXT_meshopt_compression",
        "KHR_meshopt_compression"};

    for (const char* extensionName : extensionNames) {
        ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
        const std::string assetMarker = "\"asset\":{\"version\":\"2.0\"},";
        const size_t assetPos = fixture.jsonText.find(assetMarker);
        ASSERT_NE(std::string::npos, assetPos);
        fixture.jsonText.insert(
            assetPos + assetMarker.size(),
            std::string("\"extensionsUsed\":[\"") + extensionName + "\"],"
            "\"extensionsRequired\":[\"" + extensionName + "\"],");

        const std::string bufferViewMarker =
            "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36}";
        const size_t bufferViewPos = fixture.jsonText.find(bufferViewMarker);
        ASSERT_NE(std::string::npos, bufferViewPos);
        fixture.jsonText.replace(
            bufferViewPos,
            bufferViewMarker.size(),
            std::string("{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36,"
            "\"extensions\":{\"") + extensionName + "\":{"
            "\"buffer\":0,\"byteOffset\":0,\"byteLength\":1,"
            "\"byteStride\":12,\"count\":3,\"mode\":\"ATTRIBUTES\"}}}");

        bool resolvedResource = false;
        std::unique_ptr<GltfModel> model = GltfParser::parse(
            reinterpret_cast<const uint8_t*>(fixture.jsonText.data()),
            fixture.jsonText.size(),
            [&](const std::string&) {
                resolvedResource = true;
                return fixture.bin;
            });

        EXPECT_EQ(nullptr, model) << extensionName;
        EXPECT_FALSE(resolvedResource) << extensionName;
    }
}

TEST(GltfParserTest, RejectsDeclaredBasisuTextureExtensionBeforeDecode) {
    ExternalGltfFixture fixture =
        makeTexturedExternalBufferTriangleGltf("image.bin");
    const std::string assetMarker = "\"asset\":{\"version\":\"2.0\"},";
    const size_t assetPos = fixture.jsonText.find(assetMarker);
    ASSERT_NE(std::string::npos, assetPos);
    fixture.jsonText.insert(
        assetPos + assetMarker.size(),
        "\"extensionsUsed\":[\"KHR_texture_basisu\"],"
        "\"extensionsRequired\":[\"KHR_texture_basisu\"],");

    const std::string textureMarker =
        "\"textures\":[{\"source\":0,\"sampler\":0}]";
    const size_t texturePos = fixture.jsonText.find(textureMarker);
    ASSERT_NE(std::string::npos, texturePos);
    fixture.jsonText.replace(
        texturePos,
        textureMarker.size(),
        "\"textures\":[{\"source\":0,\"sampler\":0,"
        "\"extensions\":{\"KHR_texture_basisu\":{\"source\":0}}}]");

    bool resolvedResource = false;
    bool decodedImage = false;
    std::unique_ptr<GltfModel> model = GltfParser::parse(
        reinterpret_cast<const uint8_t*>(fixture.jsonText.data()),
        fixture.jsonText.size(),
        [&](const std::string&) {
            resolvedResource = true;
            return std::vector<uint8_t>{9, 8, 7, 6};
        },
        [&](const uint8_t*, size_t) -> std::optional<GltfImage> {
            decodedImage = true;
            return std::nullopt;
        });

    EXPECT_EQ(nullptr, model);
    EXPECT_FALSE(resolvedResource);
    EXPECT_FALSE(decodedImage);
}

TEST(GltfParserTest, RejectsExtTextureWebpDeclarationWithoutTextureExtension) {
    ExternalGltfFixture fixture =
        makeTexturedExternalBufferTriangleGltf("image.bin");
    const std::string marker = "\"asset\":{\"version\":\"2.0\"},";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.insert(
        markerPos + marker.size(),
        "\"extensionsUsed\":[\"EXT_texture_webp\"],");

    std::unique_ptr<GltfModel> model = GltfParser::parse(
        reinterpret_cast<const uint8_t*>(fixture.jsonText.data()),
        fixture.jsonText.size(),
        [&](const std::string& uri) {
            return uri == "triangle.bin" ? fixture.bin
                                         : std::vector<uint8_t>{9, 8, 7, 6};
        });

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsExtTextureWebpObjectExtensionWithoutDeclaration) {
    ExternalGltfFixture fixture =
        makeTexturedExternalBufferTriangleGltf("image.bin");
    const std::string marker =
        "\"textures\":[{\"source\":0,\"sampler\":0}]";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.replace(
        markerPos,
        marker.size(),
        "\"textures\":[{\"source\":0,\"sampler\":0,"
        "\"extensions\":{\"EXT_texture_webp\":{\"source\":0}}}]");

    std::unique_ptr<GltfModel> model = GltfParser::parse(
        reinterpret_cast<const uint8_t*>(fixture.jsonText.data()),
        fixture.jsonText.size(),
        [&](const std::string& uri) {
            return uri == "triangle.bin" ? fixture.bin
                                         : std::vector<uint8_t>{9, 8, 7, 6};
        });

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsExtTextureWebpWithoutDecoder) {
    ExternalGltfFixture fixture =
        makeTexturedExternalBufferTriangleGltf("image.bin");
    const std::string assetMarker = "\"asset\":{\"version\":\"2.0\"},";
    const size_t assetPos = fixture.jsonText.find(assetMarker);
    ASSERT_NE(std::string::npos, assetPos);
    fixture.jsonText.insert(
        assetPos + assetMarker.size(),
        "\"extensionsUsed\":[\"EXT_texture_webp\"],");

    const std::string textureMarker =
        "\"textures\":[{\"source\":0,\"sampler\":0}]";
    const size_t texturePos = fixture.jsonText.find(textureMarker);
    ASSERT_NE(std::string::npos, texturePos);
    fixture.jsonText.replace(
        texturePos,
        textureMarker.size(),
        "\"textures\":[{\"sampler\":0,"
        "\"extensions\":{\"EXT_texture_webp\":{\"source\":0}}}]");

    std::unique_ptr<GltfModel> model = GltfParser::parse(
        reinterpret_cast<const uint8_t*>(fixture.jsonText.data()),
        fixture.jsonText.size(),
        [&](const std::string& uri) {
            return uri == "triangle.bin" ? fixture.bin
                                         : std::vector<uint8_t>{9, 8, 7, 6};
        });

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsUnusedExtTextureWebpDataUriWithoutDecoder) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string assetMarker = "\"asset\":{\"version\":\"2.0\"},";
    const size_t assetPos = fixture.jsonText.find(assetMarker);
    ASSERT_NE(std::string::npos, assetPos);
    fixture.jsonText.insert(
        assetPos + assetMarker.size(),
        "\"extensionsUsed\":[\"EXT_texture_webp\"],"
        "\"extensionsRequired\":[\"EXT_texture_webp\"],");

    const std::string bufferMarker = "\"buffers\":[";
    const size_t bufferPos = fixture.jsonText.find(bufferMarker);
    ASSERT_NE(std::string::npos, bufferPos);
    fixture.jsonText.insert(
        bufferPos,
        "\"textures\":[{\"extensions\":{\"EXT_texture_webp\":{\"source\":0}}}],"
        "\"images\":[{\"uri\":\"data:image/webp;base64,"
        "UklGRgQAAABXRUJQAQIDBA==\"}],");

    std::unique_ptr<GltfModel> model = GltfParser::parse(
        reinterpret_cast<const uint8_t*>(fixture.jsonText.data()),
        fixture.jsonText.size(),
        [&](const std::string& uri) {
            return uri == "triangle.bin" ? fixture.bin
                                         : std::vector<uint8_t>{};
        });

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, RejectsExtTextureWebpInvalidSource) {
    ExternalGltfFixture fixture =
        makeTexturedExternalBufferTriangleGltf("image.bin");
    const std::string assetMarker = "\"asset\":{\"version\":\"2.0\"},";
    const size_t assetPos = fixture.jsonText.find(assetMarker);
    ASSERT_NE(std::string::npos, assetPos);
    fixture.jsonText.insert(
        assetPos + assetMarker.size(),
        "\"extensionsUsed\":[\"EXT_texture_webp\"],");

    const std::string textureMarker =
        "\"textures\":[{\"source\":0,\"sampler\":0}]";
    const size_t texturePos = fixture.jsonText.find(textureMarker);
    ASSERT_NE(std::string::npos, texturePos);
    fixture.jsonText.replace(
        texturePos,
        textureMarker.size(),
        "\"textures\":[{\"source\":0,\"sampler\":0,"
        "\"extensions\":{\"EXT_texture_webp\":{\"source\":7}}}]");

    bool decodedImage = false;
    std::unique_ptr<GltfModel> model = GltfParser::parse(
        reinterpret_cast<const uint8_t*>(fixture.jsonText.data()),
        fixture.jsonText.size(),
        [&](const std::string& uri) {
            return uri == "triangle.bin" ? fixture.bin
                                         : std::vector<uint8_t>{9, 8, 7, 6};
        },
        [&](const uint8_t*, size_t) -> std::optional<GltfImage> {
            decodedImage = true;
            return std::nullopt;
        });

    EXPECT_EQ(nullptr, model);
    EXPECT_FALSE(decodedImage);
}

TEST(GltfParserTest, RejectsExtTextureWebpNonIntegerCoreFallbackSource) {
    ExternalGltfFixture fixture =
        makeTexturedExternalBufferTriangleGltf(
            "data:image/png;base64,CQkJCQ==");
    const std::string assetMarker = "\"asset\":{\"version\":\"2.0\"},";
    const size_t assetPos = fixture.jsonText.find(assetMarker);
    ASSERT_NE(std::string::npos, assetPos);
    fixture.jsonText.insert(
        assetPos + assetMarker.size(),
        "\"extensionsUsed\":[\"EXT_texture_webp\"],");

    const std::string textureMarker =
        "\"textures\":[{\"source\":0,\"sampler\":0}]";
    const size_t texturePos = fixture.jsonText.find(textureMarker);
    ASSERT_NE(std::string::npos, texturePos);
    fixture.jsonText.replace(
        texturePos,
        textureMarker.size(),
        "\"textures\":[{\"source\":\"0\",\"sampler\":0,"
        "\"extensions\":{\"EXT_texture_webp\":{\"source\":1}}}]");

    const std::string imageMarker =
        "\"images\":[{\"uri\":\"data:image/png;base64,CQkJCQ==\"}]";
    const size_t imagePos = fixture.jsonText.find(imageMarker);
    ASSERT_NE(std::string::npos, imagePos);
    fixture.jsonText.replace(
        imagePos,
        imageMarker.size(),
        "\"images\":["
        "{\"uri\":\"data:image/png;base64,CQkJCQ==\"},"
        "{\"uri\":\"data:image/webp;base64,UklGRgQAAABXRUJQAQIDBA==\"}]");

    bool decodedImage = false;
    std::unique_ptr<GltfModel> model = GltfParser::parse(
        reinterpret_cast<const uint8_t*>(fixture.jsonText.data()),
        fixture.jsonText.size(),
        [&](const std::string& uri) {
            return uri == "triangle.bin" ? fixture.bin
                                         : std::vector<uint8_t>{};
        },
        [&](const uint8_t*, size_t) -> std::optional<GltfImage> {
            decodedImage = true;
            return std::nullopt;
        });

    EXPECT_EQ(nullptr, model);
    EXPECT_FALSE(decodedImage);
}

TEST(GltfParserTest, RejectsExtTextureWebpOutOfRangeCoreFallbackSource) {
    ExternalGltfFixture fixture =
        makeTexturedExternalBufferTriangleGltf(
            "data:image/png;base64,CQkJCQ==");
    const std::string assetMarker = "\"asset\":{\"version\":\"2.0\"},";
    const size_t assetPos = fixture.jsonText.find(assetMarker);
    ASSERT_NE(std::string::npos, assetPos);
    fixture.jsonText.insert(
        assetPos + assetMarker.size(),
        "\"extensionsUsed\":[\"EXT_texture_webp\"],");

    const std::string textureMarker =
        "\"textures\":[{\"source\":0,\"sampler\":0}]";
    const size_t texturePos = fixture.jsonText.find(textureMarker);
    ASSERT_NE(std::string::npos, texturePos);
    fixture.jsonText.replace(
        texturePos,
        textureMarker.size(),
        "\"textures\":[{\"source\":7,\"sampler\":0,"
        "\"extensions\":{\"EXT_texture_webp\":{\"source\":1}}}]");

    const std::string imageMarker =
        "\"images\":[{\"uri\":\"data:image/png;base64,CQkJCQ==\"}]";
    const size_t imagePos = fixture.jsonText.find(imageMarker);
    ASSERT_NE(std::string::npos, imagePos);
    fixture.jsonText.replace(
        imagePos,
        imageMarker.size(),
        "\"images\":["
        "{\"uri\":\"data:image/png;base64,CQkJCQ==\"},"
        "{\"uri\":\"data:image/webp;base64,UklGRgQAAABXRUJQAQIDBA==\"}]");

    bool decodedImage = false;
    std::unique_ptr<GltfModel> model = GltfParser::parse(
        reinterpret_cast<const uint8_t*>(fixture.jsonText.data()),
        fixture.jsonText.size(),
        [&](const std::string& uri) {
            return uri == "triangle.bin" ? fixture.bin
                                         : std::vector<uint8_t>{};
        },
        [&](const uint8_t*, size_t) -> std::optional<GltfImage> {
            decodedImage = true;
            return std::nullopt;
        });

    EXPECT_EQ(nullptr, model);
    EXPECT_FALSE(decodedImage);
}

TEST(GltfParserTest, RejectsExtTextureWebpUnsupportedCoreFallbackImage) {
    struct FallbackCase {
        const char* imageJson;
        const char* label;
    };

    const std::array<FallbackCase, 4> cases = {{
        {"{\"uri\":\"data:image/webp;base64,UklGRgQAAABXRUJQAQIDBA==\"}",
         "webp data URI"},
        {"{\"uri\":\"fallback.webp\"}", "webp URI"},
        {"{\"uri\":\"fallback.ktx2\"}", "KTX2 URI"},
        {"{\"mimeType\":\"image/webp\"}", "webp MIME type"},
    }};

    for (const FallbackCase& testCase : cases) {
        ExternalGltfFixture fixture =
            makeTexturedExternalBufferTriangleGltf("fallback.png");
        const std::string assetMarker = "\"asset\":{\"version\":\"2.0\"},";
        const size_t assetPos = fixture.jsonText.find(assetMarker);
        ASSERT_NE(std::string::npos, assetPos) << testCase.label;
        fixture.jsonText.insert(
            assetPos + assetMarker.size(),
            "\"extensionsUsed\":[\"EXT_texture_webp\"],");

        const std::string textureMarker =
            "\"textures\":[{\"source\":0,\"sampler\":0}]";
        const size_t texturePos = fixture.jsonText.find(textureMarker);
        ASSERT_NE(std::string::npos, texturePos) << testCase.label;
        fixture.jsonText.replace(
            texturePos,
            textureMarker.size(),
            "\"textures\":[{\"source\":0,\"sampler\":0,"
            "\"extensions\":{\"EXT_texture_webp\":{\"source\":1}}}]");

        const std::string imageMarker =
            "\"images\":[{\"uri\":\"fallback.png\"}]";
        const size_t imagePos = fixture.jsonText.find(imageMarker);
        ASSERT_NE(std::string::npos, imagePos) << testCase.label;
        fixture.jsonText.replace(
            imagePos,
            imageMarker.size(),
            std::string("\"images\":[") + testCase.imageJson + "," +
                "{\"uri\":\"data:image/webp;base64,UklGRgQAAABXRUJQAQIDBA==\"}]");

        bool decodedImage = false;
        std::unique_ptr<GltfModel> model = GltfParser::parse(
            reinterpret_cast<const uint8_t*>(fixture.jsonText.data()),
            fixture.jsonText.size(),
            [&](const std::string& uri) {
                return uri == "triangle.bin" ? fixture.bin
                                             : std::vector<uint8_t>{};
            },
            [&](const uint8_t*, size_t) -> std::optional<GltfImage> {
                decodedImage = true;
                return std::nullopt;
            });

        EXPECT_EQ(nullptr, model) << testCase.label;
        EXPECT_FALSE(decodedImage) << testCase.label;
    }
}

TEST(GltfParserTest, RejectsExtTextureWebpMalformedTextureExtensionPayloads) {
    struct PayloadCase {
        const char* payload;
        const char* label;
    };

    const std::array<PayloadCase, 4> cases = {{
        {"7", "non-object extension"},
        {"{}", "missing source"},
        {"{\"source\":\"0\"}", "string source"},
        {"{\"source\":0.5}", "non-integer source"},
    }};

    for (const PayloadCase& testCase : cases) {
        ExternalGltfFixture fixture =
            makeTexturedExternalBufferTriangleGltf("image.bin");
        const std::string assetMarker = "\"asset\":{\"version\":\"2.0\"},";
        const size_t assetPos = fixture.jsonText.find(assetMarker);
        ASSERT_NE(std::string::npos, assetPos) << testCase.label;
        fixture.jsonText.insert(
            assetPos + assetMarker.size(),
            "\"extensionsUsed\":[\"EXT_texture_webp\"],");

        const std::string textureMarker =
            "\"textures\":[{\"source\":0,\"sampler\":0}]";
        const size_t texturePos = fixture.jsonText.find(textureMarker);
        ASSERT_NE(std::string::npos, texturePos) << testCase.label;
        fixture.jsonText.replace(
            texturePos,
            textureMarker.size(),
            std::string("\"textures\":[{\"source\":0,\"sampler\":0,")
                + "\"extensions\":{\"EXT_texture_webp\":"
                + testCase.payload + "}}]");

        bool decodedImage = false;
        std::unique_ptr<GltfModel> model = GltfParser::parse(
            reinterpret_cast<const uint8_t*>(fixture.jsonText.data()),
            fixture.jsonText.size(),
            [&](const std::string& uri) {
                return uri == "triangle.bin" ? fixture.bin
                                             : std::vector<uint8_t>{9, 8, 7, 6};
            },
            [&](const uint8_t*, size_t) -> std::optional<GltfImage> {
                decodedImage = true;
                GltfImage image;
                image.width = 1;
                image.height = 1;
                image.channels = 4;
                image.pixels = {255, 255, 255, 255};
                return image;
            });

        EXPECT_EQ(nullptr, model) << testCase.label;
        EXPECT_FALSE(decodedImage) << testCase.label;
    }
}

TEST(GltfParserTest, RejectsExtTextureWebpExplicitNonWebpSource) {
    ExternalGltfFixture fixture =
        makeTexturedExternalBufferTriangleGltf("image.bin");
    const std::string assetMarker = "\"asset\":{\"version\":\"2.0\"},";
    const size_t assetPos = fixture.jsonText.find(assetMarker);
    ASSERT_NE(std::string::npos, assetPos);
    fixture.jsonText.insert(
        assetPos + assetMarker.size(),
        "\"extensionsUsed\":[\"EXT_texture_webp\"],");

    const std::string textureMarker =
        "\"textures\":[{\"source\":0,\"sampler\":0}]";
    const size_t texturePos = fixture.jsonText.find(textureMarker);
    ASSERT_NE(std::string::npos, texturePos);
    fixture.jsonText.replace(
        texturePos,
        textureMarker.size(),
        "\"textures\":[{\"source\":0,\"sampler\":0,"
        "\"extensions\":{\"EXT_texture_webp\":{\"source\":0}}}]");

    const std::string imageMarker = "\"images\":[{\"uri\":\"image.bin\"}]";
    const size_t imagePos = fixture.jsonText.find(imageMarker);
    ASSERT_NE(std::string::npos, imagePos);
    fixture.jsonText.replace(
        imagePos,
        imageMarker.size(),
        "\"images\":[{\"uri\":\"image.bin\",\"mimeType\":\"image/png\"}]");

    bool decodedImage = false;
    std::unique_ptr<GltfModel> model = GltfParser::parse(
        reinterpret_cast<const uint8_t*>(fixture.jsonText.data()),
        fixture.jsonText.size(),
        [&](const std::string& uri) {
            return uri == "triangle.bin" ? fixture.bin
                                         : std::vector<uint8_t>{9, 8, 7, 6};
        },
        [&](const uint8_t*, size_t) -> std::optional<GltfImage> {
            decodedImage = true;
            return std::nullopt;
        });

    EXPECT_EQ(nullptr, model);
    EXPECT_FALSE(decodedImage);
}

TEST(GltfParserTest, RejectsExtTextureWebpExternalPngUriWithoutMimeType) {
    ExternalGltfFixture fixture =
        makeTexturedExternalBufferTriangleGltf("texture.png");
    const std::string assetMarker = "\"asset\":{\"version\":\"2.0\"},";
    const size_t assetPos = fixture.jsonText.find(assetMarker);
    ASSERT_NE(std::string::npos, assetPos);
    fixture.jsonText.insert(
        assetPos + assetMarker.size(),
        "\"extensionsUsed\":[\"EXT_texture_webp\"],");

    const std::string textureMarker =
        "\"textures\":[{\"source\":0,\"sampler\":0}]";
    const size_t texturePos = fixture.jsonText.find(textureMarker);
    ASSERT_NE(std::string::npos, texturePos);
    fixture.jsonText.replace(
        texturePos,
        textureMarker.size(),
        "\"textures\":[{\"source\":0,\"sampler\":0,"
        "\"extensions\":{\"EXT_texture_webp\":{\"source\":0}}}]");

    bool decodedImage = false;
    std::unique_ptr<GltfModel> model = GltfParser::parse(
        reinterpret_cast<const uint8_t*>(fixture.jsonText.data()),
        fixture.jsonText.size(),
        [&](const std::string& uri) {
            return uri == "triangle.bin" ? fixture.bin
                                         : makeFakeWebpBytes();
        },
        [&](const uint8_t*, size_t) -> std::optional<GltfImage> {
            decodedImage = true;
            GltfImage image;
            image.width = 1;
            image.height = 1;
            image.channels = 4;
            image.pixels = {255, 255, 255, 255};
            return image;
        });

    EXPECT_EQ(nullptr, model);
    EXPECT_FALSE(decodedImage);
}

TEST(GltfParserTest, RejectsExtTextureWebpBytesWithoutWebpSignature) {
    ExternalGltfFixture fixture =
        makeTexturedExternalBufferTriangleGltf("texture.webp");
    const std::string assetMarker = "\"asset\":{\"version\":\"2.0\"},";
    const size_t assetPos = fixture.jsonText.find(assetMarker);
    ASSERT_NE(std::string::npos, assetPos);
    fixture.jsonText.insert(
        assetPos + assetMarker.size(),
        "\"extensionsUsed\":[\"EXT_texture_webp\"],");

    const std::string textureMarker =
        "\"textures\":[{\"source\":0,\"sampler\":0}]";
    const size_t texturePos = fixture.jsonText.find(textureMarker);
    ASSERT_NE(std::string::npos, texturePos);
    fixture.jsonText.replace(
        texturePos,
        textureMarker.size(),
        "\"textures\":[{\"sampler\":0,"
        "\"extensions\":{\"EXT_texture_webp\":{\"source\":0}}}]");

    bool decodedImage = false;
    std::unique_ptr<GltfModel> model = GltfParser::parse(
        reinterpret_cast<const uint8_t*>(fixture.jsonText.data()),
        fixture.jsonText.size(),
        [&](const std::string& uri) {
            return uri == "triangle.bin" ? fixture.bin
                                         : std::vector<uint8_t>{9, 8, 7, 6};
        },
        [&](const uint8_t*, size_t) -> std::optional<GltfImage> {
            decodedImage = true;
            GltfImage image;
            image.width = 1;
            image.height = 1;
            image.channels = 4;
            image.pixels = {255, 255, 255, 255};
            return image;
        });

    EXPECT_EQ(nullptr, model);
    EXPECT_FALSE(decodedImage);
}

TEST(GltfParserTest, RejectsExtTextureWebpDataUriWithNonWebpMimeSource) {
    ExternalGltfFixture fixture =
        makeTexturedExternalBufferTriangleGltf(
            "data:image/png;base64,AQIDBA==");
    const std::string assetMarker = "\"asset\":{\"version\":\"2.0\"},";
    const size_t assetPos = fixture.jsonText.find(assetMarker);
    ASSERT_NE(std::string::npos, assetPos);
    fixture.jsonText.insert(
        assetPos + assetMarker.size(),
        "\"extensionsUsed\":[\"EXT_texture_webp\"],");

    const std::string textureMarker =
        "\"textures\":[{\"source\":0,\"sampler\":0}]";
    const size_t texturePos = fixture.jsonText.find(textureMarker);
    ASSERT_NE(std::string::npos, texturePos);
    fixture.jsonText.replace(
        texturePos,
        textureMarker.size(),
        "\"textures\":[{\"source\":0,\"sampler\":0,"
        "\"extensions\":{\"EXT_texture_webp\":{\"source\":0}}}]");

    bool decodedImage = false;
    std::unique_ptr<GltfModel> model = GltfParser::parse(
        reinterpret_cast<const uint8_t*>(fixture.jsonText.data()),
        fixture.jsonText.size(),
        [&](const std::string& uri) {
            return uri == "triangle.bin" ? fixture.bin
                                         : std::vector<uint8_t>{};
        },
        [&](const uint8_t*, size_t) -> std::optional<GltfImage> {
            decodedImage = true;
            GltfImage image;
            image.width = 1;
            image.height = 1;
            image.channels = 4;
            image.pixels = {255, 255, 255, 255};
            return image;
        });

    EXPECT_EQ(nullptr, model);
    EXPECT_FALSE(decodedImage);
}

TEST(GltfParserTest, RejectsKhrTextureTransformOutsideTextureInfo) {
    ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    const std::string marker = "\"meshes\":[{\"primitives\"";
    const size_t markerPos = fixture.jsonText.find(marker);
    ASSERT_NE(std::string::npos, markerPos);
    fixture.jsonText.replace(
        markerPos,
        marker.size(),
        "\"meshes\":[{\"extensions\":{\"KHR_texture_transform\":{}},"
        "\"primitives\"");

    std::unique_ptr<GltfModel> model = GltfParser::parse(
        reinterpret_cast<const uint8_t*>(fixture.jsonText.data()),
        fixture.jsonText.size(),
        [&](const std::string& uri) {
            return uri == "triangle.bin" ? fixture.bin
                                         : std::vector<uint8_t>{};
        });

    EXPECT_EQ(nullptr, model);
}

TEST(GltfParserTest, ContentProviderResolvesExternalBufferRelativeToGltfUrl) {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        "earth-md-gltf-external-buffer";
    std::filesystem::remove_all(root);
    const ExternalGltfFixture fixture = makeExternalBufferTriangleGltf();
    writeBytes(root / "models" / "triangle.bin", fixture.bin);

    SingleGltfContentProvider provider(
        TileKey{"Geographic-TMS", 0, 0, 0},
        "file://" + (root / "models" / "triangle.gltf").generic_string(),
        "external buffer fixture");
    TileContentLoadResult result = provider.decodeContent(
        reinterpret_cast<const uint8_t*>(fixture.jsonText.data()),
        fixture.jsonText.size());

    EXPECT_EQ(TileContentLoadStatus::Render, result.status);
    ASSERT_NE(nullptr, result.gltfModel);
    ASSERT_EQ(1u, result.gltfModel->primitives.size());
    EXPECT_EQ(3u, result.gltfModel->vertexCount());

    std::filesystem::remove_all(root);
}

TEST(GltfParserTest, ContentProviderResolvesExternalImageRelativeToGltfUrl) {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        "earth-md-gltf-external-image";
    std::filesystem::remove_all(root);
    const ExternalGltfFixture fixture =
        makeTexturedExternalBufferTriangleGltf("image.bin");
    writeBytes(root / "models" / "triangle.bin", fixture.bin);
    writeBytes(root / "models" / "image.bin", {9, 8, 7, 6});

    bool decodedExternalImage = false;
    TestPlatformBridge bridge(
        [&](const uint8_t* data, size_t size) -> std::unique_ptr<DecodedImage> {
            EXPECT_EQ(4u, size);
            EXPECT_EQ(9u, data[0]);
            decodedExternalImage = true;
            auto image = std::make_unique<DecodedImage>();
            image->width = 1;
            image->height = 1;
            image->channels = 4;
            image->pixels = {10, 20, 30, 255};
            return image;
        });

    SingleGltfContentProvider provider(
        TileKey{"Geographic-TMS", 0, 0, 0},
        "file://" + (root / "models" / "triangle.gltf").generic_string(),
        "external image fixture");
    provider.setPlatformBridge(&bridge);
    TileContentLoadResult result = provider.decodeContent(
        reinterpret_cast<const uint8_t*>(fixture.jsonText.data()),
        fixture.jsonText.size());

    EXPECT_EQ(TileContentLoadStatus::Render, result.status);
    ASSERT_NE(nullptr, result.gltfModel);
    ASSERT_TRUE(decodedExternalImage);
    ASSERT_EQ(1u, result.gltfModel->textures.size());
    ASSERT_EQ(1u, result.gltfModel->primitives.size());
    ASSERT_TRUE(result.gltfModel->primitives[0].baseColorTextureIndex);

    std::filesystem::remove_all(root);
}

TEST(GltfParserTest, ContentProviderDecodesExternalWebpTextureExtension) {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        "earth-md-gltf-external-webp";
    std::filesystem::remove_all(root);
    ExternalGltfFixture fixture =
        makeTexturedExternalBufferTriangleGltf("fallback.png");
    const std::string assetMarker = "\"asset\":{\"version\":\"2.0\"},";
    const size_t assetPos = fixture.jsonText.find(assetMarker);
    ASSERT_NE(std::string::npos, assetPos);
    fixture.jsonText.insert(
        assetPos + assetMarker.size(),
        "\"extensionsUsed\":[\"EXT_texture_webp\"],"
        "\"extensionsRequired\":[\"EXT_texture_webp\"],");

    const std::string textureMarker =
        "\"textures\":[{\"source\":0,\"sampler\":0}]";
    const size_t texturePos = fixture.jsonText.find(textureMarker);
    ASSERT_NE(std::string::npos, texturePos);
    fixture.jsonText.replace(
        texturePos,
        textureMarker.size(),
        "\"textures\":[{\"source\":0,\"sampler\":0,"
        "\"extensions\":{\"EXT_texture_webp\":{\"source\":1}}}]");

    const std::string imageMarker =
        "\"images\":[{\"uri\":\"fallback.png\"}]";
    const size_t imagePos = fixture.jsonText.find(imageMarker);
    ASSERT_NE(std::string::npos, imagePos);
    fixture.jsonText.replace(
        imagePos,
        imageMarker.size(),
        "\"images\":[{\"uri\":\"fallback.png\"},{\"uri\":\"texture.webp\"}]");
    writeBytes(root / "models" / "triangle.bin", fixture.bin);
    writeBytes(root / "models" / "fallback.png", {9, 9, 9, 9});
    writeBytes(root / "models" / "texture.webp", makeFakeWebpBytes());

    bool decodedWebp = false;
    bool decodedFallback = false;
    TestPlatformBridge bridge(
        [&](const uint8_t* data, size_t size) -> std::unique_ptr<DecodedImage> {
            if (size == 0) {
                return nullptr;
            }
            decodedWebp = bytesLookLikeFakeWebp(data, size);
            decodedFallback = data[0] == 9u;
            auto image = std::make_unique<DecodedImage>();
            image->width = 1;
            image->height = 1;
            image->channels = 4;
            image->pixels = {20, 40, 80, 255};
            return image;
        });

    SingleGltfContentProvider provider(
        TileKey{"Geographic-TMS", 0, 0, 0},
        "file://" + (root / "models" / "triangle.gltf").generic_string(),
        "external WebP texture fixture");
    provider.setPlatformBridge(&bridge);
    TileContentLoadResult result = provider.decodeContent(
        reinterpret_cast<const uint8_t*>(fixture.jsonText.data()),
        fixture.jsonText.size());

    EXPECT_EQ(TileContentLoadStatus::Render, result.status);
    ASSERT_NE(nullptr, result.gltfModel);
    EXPECT_TRUE(decodedWebp);
    EXPECT_FALSE(decodedFallback);
    ASSERT_EQ(1u, result.gltfModel->textures.size());
    EXPECT_EQ(1, result.gltfModel->textures[0].image.width);
    ASSERT_EQ(1u, result.gltfModel->primitives.size());
    ASSERT_TRUE(result.gltfModel->primitives[0].baseColorTexture);

    std::filesystem::remove_all(root);
}

TEST(GltfParserTest, ContentProviderRejectsMissingExternalWebpTextureExtension) {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        "earth-md-gltf-missing-external-webp";
    std::filesystem::remove_all(root);
    ExternalGltfFixture fixture =
        makeTexturedExternalBufferTriangleGltf("fallback.png");
    const std::string assetMarker = "\"asset\":{\"version\":\"2.0\"},";
    const size_t assetPos = fixture.jsonText.find(assetMarker);
    ASSERT_NE(std::string::npos, assetPos);
    fixture.jsonText.insert(
        assetPos + assetMarker.size(),
        "\"extensionsUsed\":[\"EXT_texture_webp\"],"
        "\"extensionsRequired\":[\"EXT_texture_webp\"],");

    const std::string textureMarker =
        "\"textures\":[{\"source\":0,\"sampler\":0}]";
    const size_t texturePos = fixture.jsonText.find(textureMarker);
    ASSERT_NE(std::string::npos, texturePos);
    fixture.jsonText.replace(
        texturePos,
        textureMarker.size(),
        "\"textures\":[{\"source\":0,\"sampler\":0,"
        "\"extensions\":{\"EXT_texture_webp\":{\"source\":1}}}]");

    const std::string imageMarker =
        "\"images\":[{\"uri\":\"fallback.png\"}]";
    const size_t imagePos = fixture.jsonText.find(imageMarker);
    ASSERT_NE(std::string::npos, imagePos);
    fixture.jsonText.replace(
        imagePos,
        imageMarker.size(),
        "\"images\":[{\"uri\":\"fallback.png\"},{\"uri\":\"texture.webp\"}]");
    writeBytes(root / "models" / "triangle.bin", fixture.bin);
    writeBytes(root / "models" / "fallback.png", {9, 9, 9, 9});

    bool decodedImage = false;
    TestPlatformBridge bridge(
        [&](const uint8_t*, size_t) -> std::unique_ptr<DecodedImage> {
            decodedImage = true;
            auto image = std::make_unique<DecodedImage>();
            image->width = 1;
            image->height = 1;
            image->channels = 4;
            image->pixels = {20, 40, 80, 255};
            return image;
        });

    SingleGltfContentProvider provider(
        TileKey{"Geographic-TMS", 0, 0, 0},
        "file://" + (root / "models" / "triangle.gltf").generic_string(),
        "missing external WebP texture fixture");
    provider.setPlatformBridge(&bridge);
    TileContentLoadResult result = provider.decodeContent(
        reinterpret_cast<const uint8_t*>(fixture.jsonText.data()),
        fixture.jsonText.size());

    EXPECT_EQ(TileContentLoadStatus::Failed, result.status);
    EXPECT_EQ(nullptr, result.gltfModel);
    EXPECT_FALSE(decodedImage);

    std::filesystem::remove_all(root);
}

TEST(GltfParserTest, ParsesExternalRobotExpressiveWhenProvided) {
    const char* path = std::getenv("EARTH_ENGINE_TEST_GLTF_PATH");
    if (!path || std::string(path).empty()) {
        GTEST_SKIP() << "EARTH_ENGINE_TEST_GLTF_PATH not set";
    }

    const std::vector<uint8_t> bytes = readFile(path);
    ASSERT_FALSE(bytes.empty());
    bool decodedEmbeddedImage = false;
    std::unique_ptr<GltfModel> model = GltfParser::parse(
        bytes.data(),
        bytes.size(),
        GltfParser::ExternalResourceResolver{},
        [&](const uint8_t* data, size_t size) -> std::optional<GltfImage> {
            EXPECT_NE(nullptr, data);
            EXPECT_GT(size, 0u);
            decodedEmbeddedImage = true;
            GltfImage image;
            image.width = 1;
            image.height = 1;
            image.channels = 4;
            image.pixels = {255, 255, 255, 255};
            return image;
        });
    ASSERT_NE(nullptr, model);
    EXPECT_GT(model->primitives.size(), 0u);
    EXPECT_GT(model->vertexCount(), 0u);
    EXPECT_GT(model->indexCount(), 0u);
    if (decodedEmbeddedImage) {
        ASSERT_FALSE(model->textures.empty());
        EXPECT_GT(model->textures[0].image.pixels.size(), 0u);
    }
    const bool hasSkinnedPrimitive = std::any_of(
        model->primitives.begin(),
        model->primitives.end(),
        [](const GltfPrimitive& primitive) { return primitive.skinned; });
    EXPECT_TRUE(hasSkinnedPrimitive);
    EXPECT_TRUE(model->hasRuntimeAnimation());
    EXPECT_TRUE(model->updateAnimation(0.5));
    EXPECT_GT(model->currentAnimationRevision(), 0u);
}

TEST(GltfParserTest, ParsesExternalRegressionAssetsWhenProvided) {
    const std::vector<std::string> paths =
        pathListFromEnv("EARTH_ENGINE_TEST_GLTF_PATHS");
    if (paths.empty()) {
        GTEST_SKIP() << "EARTH_ENGINE_TEST_GLTF_PATHS not set";
    }

    for (const std::string& pathString : paths) {
        SCOPED_TRACE(pathString);
        const std::filesystem::path path =
            std::filesystem::absolute(pathString);
        const std::vector<uint8_t> bytes = readFile(path);
        ASSERT_FALSE(bytes.empty());

        bool parserDecodedImage = false;
        std::unique_ptr<GltfModel> model = GltfParser::parse(
            bytes.data(),
            bytes.size(),
            localGltfResourceResolver(path),
            solidGltfImageDecoder(&parserDecodedImage));
        ASSERT_NE(nullptr, model);
        expectRenderableModelGeometry(*model);
        if (parserDecodedImage) {
            const bool hasDecodedTexture = std::any_of(
                model->textures.begin(),
                model->textures.end(),
                [](const GltfTexture& texture) {
                    return !texture.image.pixels.empty();
                });
            EXPECT_TRUE(hasDecodedTexture);
        }

        bool providerDecodedImage = false;
        TestPlatformBridge bridge(
            [&](const uint8_t* data,
                size_t size) -> std::unique_ptr<DecodedImage> {
                return makeSolidDecodedImage(
                    &providerDecodedImage,
                    data,
                    size);
            });
        SingleGltfContentProvider provider(
            TileKey{"Geographic-TMS", 0, 0, 0},
            "file://" + path.generic_string(),
            path.filename().generic_string());
        provider.setPlatformBridge(&bridge);
        TileContentLoadResult result =
            provider.decodeContent(bytes.data(), bytes.size());
        EXPECT_EQ(TileContentLoadStatus::Render, result.status);
        ASSERT_NE(nullptr, result.gltfModel);
        expectRenderableModelGeometry(*result.gltfModel);
        if (providerDecodedImage) {
            const bool hasDecodedTexture = std::any_of(
                result.gltfModel->textures.begin(),
                result.gltfModel->textures.end(),
                [](const GltfTexture& texture) {
                    return !texture.image.pixels.empty();
                });
            EXPECT_TRUE(hasDecodedTexture);
        }
    }
}

TEST(GltfParserTest, ContentProviderDecodesExternalRobotExpressiveWhenProvided) {
    const char* path = std::getenv("EARTH_ENGINE_TEST_GLTF_PATH");
    if (!path || std::string(path).empty()) {
        GTEST_SKIP() << "EARTH_ENGINE_TEST_GLTF_PATH not set";
    }

    const std::vector<uint8_t> bytes = readFile(path);
    ASSERT_FALSE(bytes.empty());
    bool decodedEmbeddedImage = false;
    TestPlatformBridge bridge(
        [&](const uint8_t* data, size_t size) -> std::unique_ptr<DecodedImage> {
            EXPECT_NE(nullptr, data);
            EXPECT_GT(size, 0u);
            decodedEmbeddedImage = true;
            auto image = std::make_unique<DecodedImage>();
            image->width = 1;
            image->height = 1;
            image->channels = 4;
            image->pixels = {255, 255, 255, 255};
            return image;
        });
    SingleGltfContentProvider provider(
        TileKey{"Geographic-TMS", 0, 0, 0},
        std::vector<uint8_t>{},
        "RobotExpressive fixture");
    provider.setPlatformBridge(&bridge);
    TileContentLoadResult result =
        provider.decodeContent(bytes.data(), bytes.size());
    EXPECT_EQ(TileContentLoadStatus::Render, result.status);
    ASSERT_NE(nullptr, result.gltfModel);
    EXPECT_GT(result.gltfModel->primitives.size(), 0u);
    EXPECT_GT(result.gltfModel->vertexCount(), 0u);
    EXPECT_GT(result.gltfModel->indexCount(), 0u);
    if (decodedEmbeddedImage) {
        ASSERT_FALSE(result.gltfModel->textures.empty());
        EXPECT_GT(result.gltfModel->textures[0].image.pixels.size(), 0u);
    }
    const bool hasSkinnedPrimitive = std::any_of(
        result.gltfModel->primitives.begin(),
        result.gltfModel->primitives.end(),
        [](const GltfPrimitive& primitive) { return primitive.skinned; });
    EXPECT_TRUE(hasSkinnedPrimitive);
    EXPECT_TRUE(result.gltfModel->hasRuntimeAnimation());
    EXPECT_TRUE(result.gltfModel->updateAnimation(0.5));
    EXPECT_GT(result.gltfModel->currentAnimationRevision(), 0u);
}

TEST(GltfParserTest, ContentProviderDecodesExternalI3dmWhenProvided) {
    const char* path = std::getenv("EARTH_ENGINE_TEST_I3DM_PATH");
    if (!path || std::string(path).empty()) {
        GTEST_SKIP() << "EARTH_ENGINE_TEST_I3DM_PATH not set";
    }

    const std::vector<uint8_t> bytes = readFile(path);
    ASSERT_FALSE(bytes.empty());
    SingleGltfContentProvider provider(
        TileKey{"Geographic-TMS", 0, 0, 0},
        "file://" + std::filesystem::path(path).generic_string(),
        "I3DM fixture");
    TileContentLoadResult result =
        provider.decodeContent(bytes.data(), bytes.size());
    EXPECT_EQ(TileContentLoadStatus::Render, result.status);
    ASSERT_NE(nullptr, result.gltfModel);
    EXPECT_GT(result.gltfModel->primitives.size(), 0u);
    ASSERT_FALSE(result.gltfModel->primitives.empty());
    EXPECT_GT(result.gltfModel->primitives[0].instances.size(), 0u);
}

TEST(GltfParserTest, RejectsExternalUnsupportedCompressedTextureAssetsWhenProvided) {
    const std::array<UnsupportedExternalGltfCase, 3> cases = {{
        {
            "EARTH_ENGINE_TEST_UNSUPPORTED_DRACO_GLTF_PATH",
            "KHR_draco_mesh_compression"},
        {
            "EARTH_ENGINE_TEST_UNSUPPORTED_MESHOPT_GLTF_PATH",
            "EXT_meshopt_compression"},
        {
            "EARTH_ENGINE_TEST_UNSUPPORTED_KTX2_GLTF_PATH",
            "KHR_texture_basisu"}}};

    bool ranCase = false;
    for (const UnsupportedExternalGltfCase& fixtureCase : cases) {
        const char* path = std::getenv(fixtureCase.env);
        if (!path || std::string(path).empty()) {
            continue;
        }
        ranCase = true;

        const std::vector<uint8_t> bytes = readFile(path);
        ASSERT_FALSE(bytes.empty()) << fixtureCase.label;
        std::unique_ptr<GltfModel> model =
            GltfParser::parse(bytes.data(), bytes.size());
        EXPECT_EQ(nullptr, model) << fixtureCase.label;

        SingleGltfContentProvider provider(
            TileKey{"Geographic-TMS", 0, 0, 0},
            "file://" + std::filesystem::path(path).generic_string(),
            fixtureCase.label);
        TileContentLoadResult result =
            provider.decodeContent(bytes.data(), bytes.size());
        EXPECT_EQ(TileContentLoadStatus::Failed, result.status)
            << fixtureCase.label;
        EXPECT_EQ(nullptr, result.gltfModel) << fixtureCase.label;
    }

    if (!ranCase) {
        GTEST_SKIP()
            << "No EARTH_ENGINE_TEST_UNSUPPORTED_*_GLTF_PATH values set";
    }
}

TEST(GltfParserTest, ContentProviderDecodesPntsPositionRgbAndRtcCenter) {
    std::vector<uint8_t> featureBinary;
    appendF32(featureBinary, 1.0f);
    appendF32(featureBinary, 2.0f);
    appendF32(featureBinary, 3.0f);
    appendF32(featureBinary, -4.0f);
    appendF32(featureBinary, 5.0f);
    appendF32(featureBinary, 6.0f);
    const size_t rgbOffset = featureBinary.size();
    featureBinary.insert(
        featureBinary.end(),
        {255u, 128u, 0u, 0u, 255u, 64u});

    const std::string featureJson =
        std::string("{") +
        "\"POINTS_LENGTH\":2,"
        "\"RTC_CENTER\":[100.0,200.0,300.0],"
        "\"POSITION\":{\"byteOffset\":0},"
        "\"RGB\":{\"byteOffset\":" +
        std::to_string(rgbOffset) + "}}";
    const std::vector<uint8_t> pnts = makePnts(featureJson, featureBinary);

    SingleGltfContentProvider provider(
        TileKey{"Geographic-TMS", 0, 0, 0},
        std::vector<uint8_t>{},
        "PNTS RGB fixture");
    TileContentLoadResult result =
        provider.decodeContent(pnts.data(), pnts.size());

    EXPECT_EQ(TileContentLoadStatus::Render, result.status);
    ASSERT_NE(nullptr, result.gltfModel);
    ASSERT_EQ(1u, result.gltfModel->primitives.size());
    const GltfPrimitive& primitive = result.gltfModel->primitives[0];
    EXPECT_EQ(GltfPrimitiveMode::Points, primitive.primitiveMode);
    EXPECT_TRUE(primitive.unlit);
    EXPECT_FLOAT_EQ(0.0f, primitive.metallicFactor);
    EXPECT_NEAR(0.9f, primitive.roughnessFactor, 1e-6f);
    ASSERT_EQ(2u, primitive.vertices.size());
    EXPECT_NEAR(101.0, primitive.vertices[0].positionEcef.x(), 1e-6);
    EXPECT_NEAR(202.0, primitive.vertices[0].positionEcef.y(), 1e-6);
    EXPECT_NEAR(303.0, primitive.vertices[0].positionEcef.z(), 1e-6);
    EXPECT_NEAR(96.0, primitive.vertices[1].positionEcef.x(), 1e-6);
    EXPECT_NEAR(205.0, primitive.vertices[1].positionEcef.y(), 1e-6);
    EXPECT_NEAR(306.0, primitive.vertices[1].positionEcef.z(), 1e-6);
    EXPECT_EQ((std::vector<uint32_t>{0u, 1u}), primitive.indices);
    ASSERT_EQ(2u, primitive.vertexColors.size());
    EXPECT_NEAR(1.0f, primitive.vertexColors[0][0], 1e-6f);
    EXPECT_NEAR(
        std::pow(128.0f / 255.0f, 2.2f),
        primitive.vertexColors[0][1],
        1e-6f);
    EXPECT_NEAR(0.0f, primitive.vertexColors[0][2], 1e-6f);
    EXPECT_NEAR(1.0f, primitive.vertexColors[0][3], 1e-6f);
    EXPECT_NEAR(0.0f, primitive.vertexColors[1][0], 1e-6f);
    EXPECT_NEAR(1.0f, primitive.vertexColors[1][1], 1e-6f);
    EXPECT_NEAR(
        std::pow(64.0f / 255.0f, 2.2f),
        primitive.vertexColors[1][2],
        1e-6f);
}

TEST(GltfParserTest, ContentProviderDecodesPntsQuantizedPositionAndRtcCenter) {
    std::vector<uint8_t> featureBinary;
    appendU16(featureBinary, 0u);
    appendU16(featureBinary, 0u);
    appendU16(featureBinary, 0u);
    appendU16(featureBinary, 65535u);
    appendU16(featureBinary, 65535u);
    appendU16(featureBinary, 65535u);

    const std::string featureJson =
        "{\"POINTS_LENGTH\":2,"
        "\"RTC_CENTER\":[1.0,2.0,3.0],"
        "\"POSITION_QUANTIZED\":{\"byteOffset\":0},"
        "\"QUANTIZED_VOLUME_OFFSET\":[10.0,20.0,30.0],"
        "\"QUANTIZED_VOLUME_SCALE\":[100.0,200.0,300.0]}";
    const std::vector<uint8_t> pnts = makePnts(featureJson, featureBinary);

    SingleGltfContentProvider provider(
        TileKey{"Geographic-TMS", 0, 0, 0},
        std::vector<uint8_t>{},
        "PNTS quantized position fixture");
    TileContentLoadResult result =
        provider.decodeContent(pnts.data(), pnts.size());

    EXPECT_EQ(TileContentLoadStatus::Render, result.status);
    ASSERT_NE(nullptr, result.gltfModel);
    ASSERT_EQ(1u, result.gltfModel->primitives.size());
    const GltfPrimitive& primitive = result.gltfModel->primitives[0];
    ASSERT_EQ(2u, primitive.vertices.size());
    EXPECT_NEAR(11.0, primitive.vertices[0].positionEcef.x(), 1e-6);
    EXPECT_NEAR(22.0, primitive.vertices[0].positionEcef.y(), 1e-6);
    EXPECT_NEAR(33.0, primitive.vertices[0].positionEcef.z(), 1e-6);
    EXPECT_NEAR(111.0, primitive.vertices[1].positionEcef.x(), 1e-6);
    EXPECT_NEAR(222.0, primitive.vertices[1].positionEcef.y(), 1e-6);
    EXPECT_NEAR(333.0, primitive.vertices[1].positionEcef.z(), 1e-6);
}

TEST(GltfParserTest, ContentProviderDecodesPntsNormals) {
    std::vector<uint8_t> featureBinary;
    appendF32(featureBinary, 1.0f);
    appendF32(featureBinary, 2.0f);
    appendF32(featureBinary, 3.0f);
    appendF32(featureBinary, 4.0f);
    appendF32(featureBinary, 5.0f);
    appendF32(featureBinary, 6.0f);
    const size_t normalOffset = featureBinary.size();
    appendF32(featureBinary, 0.0f);
    appendF32(featureBinary, 0.0f);
    appendF32(featureBinary, 2.0f);
    appendF32(featureBinary, 0.0f);
    appendF32(featureBinary, 3.0f);
    appendF32(featureBinary, 0.0f);

    const std::string featureJson =
        std::string("{") +
        "\"POINTS_LENGTH\":2,"
        "\"POSITION\":{\"byteOffset\":0},"
        "\"NORMAL\":{\"byteOffset\":" +
        std::to_string(normalOffset) + "}}";
    const std::vector<uint8_t> pnts = makePnts(featureJson, featureBinary);

    SingleGltfContentProvider provider(
        TileKey{"Geographic-TMS", 0, 0, 0},
        std::vector<uint8_t>{},
        "PNTS normal fixture");
    TileContentLoadResult result =
        provider.decodeContent(pnts.data(), pnts.size());

    EXPECT_EQ(TileContentLoadStatus::Render, result.status);
    ASSERT_NE(nullptr, result.gltfModel);
    ASSERT_EQ(1u, result.gltfModel->primitives.size());
    const GltfPrimitive& primitive = result.gltfModel->primitives[0];
    EXPECT_FALSE(primitive.unlit);
    ASSERT_EQ(2u, primitive.vertices.size());
    EXPECT_NEAR(0.0, primitive.vertices[0].normalEcef.x(), 1e-6);
    EXPECT_NEAR(0.0, primitive.vertices[0].normalEcef.y(), 1e-6);
    EXPECT_NEAR(1.0, primitive.vertices[0].normalEcef.z(), 1e-6);
    EXPECT_NEAR(0.0, primitive.vertices[1].normalEcef.x(), 1e-6);
    EXPECT_NEAR(1.0, primitive.vertices[1].normalEcef.y(), 1e-6);
    EXPECT_NEAR(0.0, primitive.vertices[1].normalEcef.z(), 1e-6);
}

TEST(GltfParserTest, ContentProviderDecodesPntsOctEncodedNormals) {
    std::vector<uint8_t> featureBinary;
    appendF32(featureBinary, 1.0f);
    appendF32(featureBinary, 2.0f);
    appendF32(featureBinary, 3.0f);
    const size_t normalOffset = featureBinary.size();
    featureBinary.insert(featureBinary.end(), {128u, 128u});

    const std::string featureJson =
        std::string("{") +
        "\"POINTS_LENGTH\":1,"
        "\"POSITION\":{\"byteOffset\":0},"
        "\"NORMAL_OCT16P\":{\"byteOffset\":" +
        std::to_string(normalOffset) + "}}";
    const std::vector<uint8_t> pnts = makePnts(featureJson, featureBinary);

    SingleGltfContentProvider provider(
        TileKey{"Geographic-TMS", 0, 0, 0},
        std::vector<uint8_t>{},
        "PNTS oct normal fixture");
    TileContentLoadResult result =
        provider.decodeContent(pnts.data(), pnts.size());

    EXPECT_EQ(TileContentLoadStatus::Render, result.status);
    ASSERT_NE(nullptr, result.gltfModel);
    ASSERT_EQ(1u, result.gltfModel->primitives.size());
    const GltfPrimitive& primitive = result.gltfModel->primitives[0];
    EXPECT_FALSE(primitive.unlit);
    ASSERT_EQ(1u, primitive.vertices.size());
    const double expectedScale = std::sqrt(1.0 + 1.0 + 253.0 * 253.0);
    EXPECT_NEAR(
        1.0 / expectedScale,
        primitive.vertices[0].normalEcef.x(),
        1e-6);
    EXPECT_NEAR(
        1.0 / expectedScale,
        primitive.vertices[0].normalEcef.y(),
        1e-6);
    EXPECT_NEAR(
        253.0 / expectedScale,
        primitive.vertices[0].normalEcef.z(),
        1e-6);
}

TEST(GltfParserTest, ContentProviderDecodesPntsConstantRgbaMaterialColor) {
    std::vector<uint8_t> featureBinary;
    appendF32(featureBinary, 1.0f);
    appendF32(featureBinary, 2.0f);
    appendF32(featureBinary, 3.0f);

    const std::string featureJson =
        "{\"POINTS_LENGTH\":1,"
        "\"POSITION\":{\"byteOffset\":0},"
        "\"CONSTANT_RGBA\":[64,128,255,127]}";
    const std::vector<uint8_t> pnts = makePnts(featureJson, featureBinary);

    SingleGltfContentProvider provider(
        TileKey{"Geographic-TMS", 0, 0, 0},
        std::vector<uint8_t>{},
        "PNTS constant color fixture");
    TileContentLoadResult result =
        provider.decodeContent(pnts.data(), pnts.size());

    EXPECT_EQ(TileContentLoadStatus::Render, result.status);
    ASSERT_NE(nullptr, result.gltfModel);
    ASSERT_EQ(1u, result.gltfModel->primitives.size());
    const GltfPrimitive& primitive = result.gltfModel->primitives[0];
    EXPECT_TRUE(primitive.vertexColors.empty());
    EXPECT_EQ(GltfAlphaMode::Blend, primitive.alphaMode);
    EXPECT_NEAR(
        std::pow(64.0f / 255.0f, 2.2f),
        primitive.baseColorFactor[0],
        1e-6f);
    EXPECT_NEAR(
        std::pow(128.0f / 255.0f, 2.2f),
        primitive.baseColorFactor[1],
        1e-6f);
    EXPECT_NEAR(1.0f, primitive.baseColorFactor[2], 1e-6f);
    EXPECT_NEAR(127.0f / 255.0f, primitive.baseColorFactor[3], 1e-6f);
}

TEST(GltfParserTest, ContentProviderDecodesPntsRgb565) {
    std::vector<uint8_t> featureBinary;
    appendF32(featureBinary, 1.0f);
    appendF32(featureBinary, 2.0f);
    appendF32(featureBinary, 3.0f);
    const size_t rgb565Offset = featureBinary.size();
    appendU16(featureBinary, 33800u);

    const std::string featureJson =
        std::string("{") +
        "\"POINTS_LENGTH\":1,"
        "\"POSITION\":{\"byteOffset\":0},"
        "\"RGB565\":{\"byteOffset\":" +
        std::to_string(rgb565Offset) + "},"
        "\"CONSTANT_RGBA\":[0,0,0,127]}";
    const std::vector<uint8_t> pnts = makePnts(featureJson, featureBinary);

    SingleGltfContentProvider provider(
        TileKey{"Geographic-TMS", 0, 0, 0},
        std::vector<uint8_t>{},
        "PNTS RGB565 fixture");
    TileContentLoadResult result =
        provider.decodeContent(pnts.data(), pnts.size());

    EXPECT_EQ(TileContentLoadStatus::Render, result.status);
    ASSERT_NE(nullptr, result.gltfModel);
    ASSERT_EQ(1u, result.gltfModel->primitives.size());
    const GltfPrimitive& primitive = result.gltfModel->primitives[0];
    EXPECT_EQ(GltfAlphaMode::Opaque, primitive.alphaMode);
    EXPECT_NEAR(1.0f, primitive.baseColorFactor[0], 1e-6f);
    EXPECT_NEAR(1.0f, primitive.baseColorFactor[1], 1e-6f);
    EXPECT_NEAR(1.0f, primitive.baseColorFactor[2], 1e-6f);
    EXPECT_NEAR(1.0f, primitive.baseColorFactor[3], 1e-6f);
    ASSERT_EQ(1u, primitive.vertexColors.size());
    EXPECT_NEAR(
        std::pow(16.0f / 31.0f, 2.2f),
        primitive.vertexColors[0][0],
        1e-6f);
    EXPECT_NEAR(
        std::pow(32.0f / 63.0f, 2.2f),
        primitive.vertexColors[0][1],
        1e-6f);
    EXPECT_NEAR(
        std::pow(8.0f / 31.0f, 2.2f),
        primitive.vertexColors[0][2],
        1e-6f);
    EXPECT_NEAR(1.0f, primitive.vertexColors[0][3], 1e-6f);
}

TEST(GltfParserTest, ContentProviderPrefersPntsRgbOverConstantRgba) {
    std::vector<uint8_t> featureBinary;
    appendF32(featureBinary, 1.0f);
    appendF32(featureBinary, 2.0f);
    appendF32(featureBinary, 3.0f);
    const size_t rgbOffset = featureBinary.size();
    featureBinary.insert(featureBinary.end(), {255u, 128u, 0u});

    const std::string featureJson =
        std::string("{") +
        "\"POINTS_LENGTH\":1,"
        "\"POSITION\":{\"byteOffset\":0},"
        "\"RGB\":{\"byteOffset\":" +
        std::to_string(rgbOffset) + "},"
        "\"CONSTANT_RGBA\":[0,0,0,127]}";
    const std::vector<uint8_t> pnts = makePnts(featureJson, featureBinary);

    SingleGltfContentProvider provider(
        TileKey{"Geographic-TMS", 0, 0, 0},
        std::vector<uint8_t>{},
        "PNTS RGB and constant color fixture");
    TileContentLoadResult result =
        provider.decodeContent(pnts.data(), pnts.size());

    EXPECT_EQ(TileContentLoadStatus::Render, result.status);
    ASSERT_NE(nullptr, result.gltfModel);
    ASSERT_EQ(1u, result.gltfModel->primitives.size());
    const GltfPrimitive& primitive = result.gltfModel->primitives[0];
    EXPECT_EQ(GltfAlphaMode::Opaque, primitive.alphaMode);
    EXPECT_NEAR(1.0f, primitive.baseColorFactor[0], 1e-6f);
    EXPECT_NEAR(1.0f, primitive.baseColorFactor[1], 1e-6f);
    EXPECT_NEAR(1.0f, primitive.baseColorFactor[2], 1e-6f);
    EXPECT_NEAR(1.0f, primitive.baseColorFactor[3], 1e-6f);
    ASSERT_EQ(1u, primitive.vertexColors.size());
    EXPECT_NEAR(1.0f, primitive.vertexColors[0][0], 1e-6f);
    EXPECT_NEAR(
        std::pow(128.0f / 255.0f, 2.2f),
        primitive.vertexColors[0][1],
        1e-6f);
    EXPECT_NEAR(0.0f, primitive.vertexColors[0][2], 1e-6f);
    EXPECT_NEAR(1.0f, primitive.vertexColors[0][3], 1e-6f);
}

TEST(GltfParserTest, ContentProviderPrefersPntsRgbaOverConstantRgba) {
    std::vector<uint8_t> featureBinary;
    appendF32(featureBinary, 1.0f);
    appendF32(featureBinary, 2.0f);
    appendF32(featureBinary, 3.0f);
    const size_t rgbaOffset = featureBinary.size();
    featureBinary.insert(featureBinary.end(), {64u, 128u, 255u, 63u});

    const std::string featureJson =
        std::string("{") +
        "\"POINTS_LENGTH\":1,"
        "\"POSITION\":{\"byteOffset\":0},"
        "\"RGBA\":{\"byteOffset\":" +
        std::to_string(rgbaOffset) + "},"
        "\"CONSTANT_RGBA\":[0,0,0,127]}";
    const std::vector<uint8_t> pnts = makePnts(featureJson, featureBinary);

    SingleGltfContentProvider provider(
        TileKey{"Geographic-TMS", 0, 0, 0},
        std::vector<uint8_t>{},
        "PNTS RGBA and constant color fixture");
    TileContentLoadResult result =
        provider.decodeContent(pnts.data(), pnts.size());

    EXPECT_EQ(TileContentLoadStatus::Render, result.status);
    ASSERT_NE(nullptr, result.gltfModel);
    ASSERT_EQ(1u, result.gltfModel->primitives.size());
    const GltfPrimitive& primitive = result.gltfModel->primitives[0];
    EXPECT_EQ(GltfAlphaMode::Blend, primitive.alphaMode);
    EXPECT_NEAR(1.0f, primitive.baseColorFactor[0], 1e-6f);
    EXPECT_NEAR(1.0f, primitive.baseColorFactor[1], 1e-6f);
    EXPECT_NEAR(1.0f, primitive.baseColorFactor[2], 1e-6f);
    EXPECT_NEAR(1.0f, primitive.baseColorFactor[3], 1e-6f);
    ASSERT_EQ(1u, primitive.vertexColors.size());
    EXPECT_NEAR(
        std::pow(64.0f / 255.0f, 2.2f),
        primitive.vertexColors[0][0],
        1e-6f);
    EXPECT_NEAR(
        std::pow(128.0f / 255.0f, 2.2f),
        primitive.vertexColors[0][1],
        1e-6f);
    EXPECT_NEAR(1.0f, primitive.vertexColors[0][2], 1e-6f);
    EXPECT_NEAR(63.0f / 255.0f, primitive.vertexColors[0][3], 1e-6f);
}

TEST(GltfParserTest, ContentProviderDecodesPntsJsonBatchTablePerPoint) {
    std::vector<uint8_t> featureBinary;
    appendF32(featureBinary, 1.0f);
    appendF32(featureBinary, 2.0f);
    appendF32(featureBinary, 3.0f);
    appendF32(featureBinary, 4.0f);
    appendF32(featureBinary, 5.0f);
    appendF32(featureBinary, 6.0f);

    const std::string featureJson =
        "{\"POINTS_LENGTH\":2,\"POSITION\":{\"byteOffset\":0}}";
    const std::vector<uint8_t> pnts = makePnts(
        featureJson,
        featureBinary,
        "{\"name\":[\"first\",\"second\"],"
        "\"Height\":[10,20],"
        "\"enabled\":[true,false],"
        "\"nullable\":[null,null]}");

    SingleGltfContentProvider provider(
        TileKey{"Geographic-TMS", 0, 0, 0},
        std::vector<uint8_t>{},
        "PNTS per-point batch table fixture");
    TileContentLoadResult result =
        provider.decodeContent(pnts.data(), pnts.size());

    EXPECT_EQ(TileContentLoadStatus::Render, result.status);
    ASSERT_NE(nullptr, result.gltfModel);
    ASSERT_EQ(1u, result.gltfModel->primitives.size());
    const GltfPrimitive& primitive = result.gltfModel->primitives[0];
    EXPECT_EQ((std::vector<uint32_t>{0u, 1u}), primitive.featureIds);
    ASSERT_EQ(2u, primitive.featureProperties.size());
    ASSERT_EQ(4u, primitive.featureProperties[0].size());
    ASSERT_EQ(4u, primitive.featureProperties[1].size());
    EXPECT_EQ(
        "first",
        *std::get_if<std::string>(&primitive.featureProperties[0].at("name")));
    EXPECT_EQ(
        "second",
        *std::get_if<std::string>(&primitive.featureProperties[1].at("name")));
    EXPECT_EQ(
        10u,
        *std::get_if<uint64_t>(&primitive.featureProperties[0].at("Height")));
    EXPECT_EQ(
        20u,
        *std::get_if<uint64_t>(&primitive.featureProperties[1].at("Height")));
    EXPECT_EQ(
        true,
        *std::get_if<bool>(&primitive.featureProperties[0].at("enabled")));
    EXPECT_EQ(
        false,
        *std::get_if<bool>(&primitive.featureProperties[1].at("enabled")));
    EXPECT_TRUE(std::holds_alternative<std::monostate>(
        primitive.featureProperties[0].at("nullable")));
}

TEST(GltfParserTest, ContentProviderDecodesPntsBatchIdsToJsonBatchTableRows) {
    std::vector<uint8_t> featureBinary;
    appendF32(featureBinary, 1.0f);
    appendF32(featureBinary, 2.0f);
    appendF32(featureBinary, 3.0f);
    appendF32(featureBinary, 4.0f);
    appendF32(featureBinary, 5.0f);
    appendF32(featureBinary, 6.0f);
    const size_t batchIdOffset = featureBinary.size();
    featureBinary.push_back(1u);
    featureBinary.push_back(0u);

    const std::string featureJson =
        std::string("{") +
        "\"POINTS_LENGTH\":2,"
        "\"POSITION\":{\"byteOffset\":0},"
        "\"BATCH_ID\":{\"byteOffset\":" +
        std::to_string(batchIdOffset) +
        ",\"componentType\":\"UNSIGNED_BYTE\"},"
        "\"BATCH_LENGTH\":2}";
    const std::vector<uint8_t> pnts = makePnts(
        featureJson,
        featureBinary,
        "{\"name\":[\"zero\",\"one\"],\"Height\":[100,200]}");

    SingleGltfContentProvider provider(
        TileKey{"Geographic-TMS", 0, 0, 0},
        std::vector<uint8_t>{},
        "PNTS batch ID batch table fixture");
    TileContentLoadResult result =
        provider.decodeContent(pnts.data(), pnts.size());

    EXPECT_EQ(TileContentLoadStatus::Render, result.status);
    ASSERT_NE(nullptr, result.gltfModel);
    ASSERT_EQ(1u, result.gltfModel->primitives.size());
    const GltfPrimitive& primitive = result.gltfModel->primitives[0];
    EXPECT_EQ((std::vector<uint32_t>{1u, 0u}), primitive.featureIds);
    ASSERT_EQ(2u, primitive.featureProperties.size());
    EXPECT_EQ(
        "one",
        *std::get_if<std::string>(&primitive.featureProperties[0].at("name")));
    EXPECT_EQ(
        "zero",
        *std::get_if<std::string>(&primitive.featureProperties[1].at("name")));
    EXPECT_EQ(
        200u,
        *std::get_if<uint64_t>(&primitive.featureProperties[0].at("Height")));
    EXPECT_EQ(
        100u,
        *std::get_if<uint64_t>(&primitive.featureProperties[1].at("Height")));
}

TEST(GltfParserTest, ContentProviderRejectsUnsupportedPntsSemanticsAndMetadata) {
    auto decodeStatus = [](const std::string& featureJson,
                           std::vector<uint8_t> featureBinary,
                           const std::string& batchJson = std::string{}) {
        const std::vector<uint8_t> pnts =
            makePnts(featureJson, std::move(featureBinary), batchJson);
        SingleGltfContentProvider provider(
            TileKey{"Geographic-TMS", 0, 0, 0},
            std::vector<uint8_t>{},
            "unsupported PNTS fixture");
        return provider.decodeContent(pnts.data(), pnts.size()).status;
    };

    std::vector<uint8_t> positionBinary;
    appendF32(positionBinary, 1.0f);
    appendF32(positionBinary, 2.0f);
    appendF32(positionBinary, 3.0f);

    EXPECT_EQ(
        TileContentLoadStatus::Failed,
        decodeStatus(
            "{\"POINTS_LENGTH\":1,"
            "\"POSITION\":{\"byteOffset\":0},"
            "\"extensions\":{\"3DTILES_draco_point_compression\":{"
            "\"byteOffset\":12,\"byteLength\":4,"
            "\"properties\":{\"POSITION\":0}}}}",
            positionBinary));
    EXPECT_EQ(
        TileContentLoadStatus::Failed,
        decodeStatus(
            "{\"POINTS_LENGTH\":1,\"POSITION\":{\"byteOffset\":0}}",
            positionBinary,
            "{\"HIERARCHY\":{\"instances\":[]}}"));
    EXPECT_EQ(
        TileContentLoadStatus::Failed,
        decodeStatus(
            "{\"POINTS_LENGTH\":1,\"POSITION\":{\"byteOffset\":0}}",
            positionBinary,
            "{\"extensions\":{\"3DTILES_draco_point_compression\":{"
            "\"properties\":{\"name\":0}}},"
            "\"name\":[\"feature\"]}"));

    std::vector<uint8_t> colorConflictBinary = positionBinary;
    const size_t rgbOffset = colorConflictBinary.size();
    colorConflictBinary.insert(colorConflictBinary.end(), {255u, 0u, 0u});
    const size_t rgb565Offset = colorConflictBinary.size();
    appendU16(colorConflictBinary, 33800u);
    EXPECT_EQ(
        TileContentLoadStatus::Failed,
        decodeStatus(
            std::string("{\"POINTS_LENGTH\":1,"
                        "\"POSITION\":{\"byteOffset\":0},"
                        "\"RGB\":{\"byteOffset\":") +
                std::to_string(rgbOffset) +
                "},\"RGB565\":{\"byteOffset\":" +
                std::to_string(rgb565Offset) + "}}",
            colorConflictBinary));

    EXPECT_EQ(
        TileContentLoadStatus::Failed,
        decodeStatus(
            "{\"POINTS_LENGTH\":1,"
            "\"POSITION_QUANTIZED\":{\"byteOffset\":0},"
            "\"QUANTIZED_VOLUME_OFFSET\":[0,0,0]}",
            {0, 0, 0, 0, 0, 0}));

    std::vector<uint8_t> positionConflictBinary = positionBinary;
    const size_t quantizedPositionOffset = positionConflictBinary.size();
    appendU16(positionConflictBinary, 0u);
    appendU16(positionConflictBinary, 0u);
    appendU16(positionConflictBinary, 0u);
    EXPECT_EQ(
        TileContentLoadStatus::Failed,
        decodeStatus(
            std::string("{\"POINTS_LENGTH\":1,"
                        "\"POSITION\":{\"byteOffset\":0},"
                        "\"POSITION_QUANTIZED\":{\"byteOffset\":") +
                std::to_string(quantizedPositionOffset) +
                "},\"QUANTIZED_VOLUME_OFFSET\":[0,0,0],"
                "\"QUANTIZED_VOLUME_SCALE\":[1,1,1]}",
            positionConflictBinary));
    EXPECT_EQ(
        TileContentLoadStatus::Failed,
        decodeStatus(
            "{\"POINTS_LENGTH\":1,"
            "\"POSITION\":{\"byteOffset\":0},"
            "\"NORMAL\":{\"byteOffset\":12}}",
            positionBinary));

    std::vector<uint8_t> normalConflictBinary = positionBinary;
    const size_t normalOffset = normalConflictBinary.size();
    appendF32(normalConflictBinary, 0.0f);
    appendF32(normalConflictBinary, 0.0f);
    appendF32(normalConflictBinary, 1.0f);
    const size_t octNormalOffset = normalConflictBinary.size();
    normalConflictBinary.insert(normalConflictBinary.end(), {128u, 128u});
    EXPECT_EQ(
        TileContentLoadStatus::Failed,
        decodeStatus(
            std::string("{\"POINTS_LENGTH\":1,"
                        "\"POSITION\":{\"byteOffset\":0},"
                        "\"NORMAL\":{\"byteOffset\":") +
                std::to_string(normalOffset) +
                "},\"NORMAL_OCT16P\":{\"byteOffset\":" +
                std::to_string(octNormalOffset) + "}}",
            normalConflictBinary));
    EXPECT_EQ(
        TileContentLoadStatus::Failed,
        decodeStatus(
            "{\"POINTS_LENGTH\":1,\"POSITION\":{\"byteOffset\":0}}",
            positionBinary,
            "{\"name\":[\"feature\",\"extra\"]}"));

    std::vector<uint8_t> batchIdBinary = positionBinary;
    batchIdBinary.push_back(0u);
    EXPECT_EQ(
        TileContentLoadStatus::Failed,
        decodeStatus(
            "{\"POINTS_LENGTH\":1,"
            "\"POSITION\":{\"byteOffset\":0},"
            "\"BATCH_ID\":{\"byteOffset\":12,"
            "\"componentType\":\"UNSIGNED_BYTE\"}}",
            batchIdBinary,
            "{\"name\":[\"feature\"]}"));

    EXPECT_EQ(
        TileContentLoadStatus::Failed,
        decodeStatus(
            "{\"POINTS_LENGTH\":1,"
            "\"POSITION\":{\"byteOffset\":0},"
            "\"BATCH_ID\":{\"byteOffset\":12,"
            "\"componentType\":\"BYTE\"},"
            "\"BATCH_LENGTH\":1}",
            batchIdBinary,
            "{\"name\":[\"feature\"]}"));

    EXPECT_EQ(
        TileContentLoadStatus::Failed,
        decodeStatus(
            "{\"POINTS_LENGTH\":1,\"POSITION\":{\"byteOffset\":0}}",
            positionBinary,
            "{\"name\":[{\"nested\":true}]}"));
}

TEST(GltfParserTest, ContentProviderRejectsPntsBinaryBatchTable) {
    std::vector<uint8_t> positionBinary;
    appendF32(positionBinary, 1.0f);
    appendF32(positionBinary, 2.0f);
    appendF32(positionBinary, 3.0f);
    const std::vector<uint8_t> pnts = makePnts(
        "{\"POINTS_LENGTH\":1,\"POSITION\":{\"byteOffset\":0}}",
        std::move(positionBinary),
        "{\"name\":[\"feature\"]}",
        {0u, 1u, 2u, 3u});

    SingleGltfContentProvider provider(
        TileKey{"Geographic-TMS", 0, 0, 0},
        std::vector<uint8_t>{},
        "PNTS binary batch table fixture");
    TileContentLoadResult result =
        provider.decodeContent(pnts.data(), pnts.size());

    EXPECT_EQ(TileContentLoadStatus::Failed, result.status);
    EXPECT_EQ(nullptr, result.gltfModel);
}

TEST(GltfParserTest, ContentProviderDecodesEmptyCmptContent) {
    const std::vector<uint8_t> cmpt = makeCmpt({});

    SingleGltfContentProvider provider(
        TileKey{"Geographic-TMS", 0, 0, 0},
        std::vector<uint8_t>{},
        "empty CMPT fixture");
    TileContentLoadResult result =
        provider.decodeContent(cmpt.data(), cmpt.size());

    EXPECT_EQ(TileContentLoadStatus::Empty, result.status);
    EXPECT_EQ(nullptr, result.gltfModel);
}

TEST(GltfParserTest, ContentProviderRejectsInvalidCmptInnerContent) {
    std::vector<uint8_t> cmpt;
    cmpt.push_back('c');
    cmpt.push_back('m');
    cmpt.push_back('p');
    cmpt.push_back('t');
    appendU32(cmpt, 1u);
    appendU32(cmpt, 16u);
    appendU32(cmpt, 1u);

    SingleGltfContentProvider provider(
        TileKey{"Geographic-TMS", 0, 0, 0},
        std::vector<uint8_t>{},
        "invalid CMPT fixture");
    TileContentLoadResult result =
        provider.decodeContent(cmpt.data(), cmpt.size());

    EXPECT_EQ(TileContentLoadStatus::Failed, result.status);
    EXPECT_EQ(nullptr, result.gltfModel);
}

TEST(GltfParserTest, ContentProviderRejectsCmptImpossibleTileCount) {
    std::vector<uint8_t> cmpt;
    cmpt.push_back('c');
    cmpt.push_back('m');
    cmpt.push_back('p');
    cmpt.push_back('t');
    appendU32(cmpt, 1u);
    appendU32(cmpt, 16u);
    appendU32(cmpt, std::numeric_limits<uint32_t>::max());

    SingleGltfContentProvider provider(
        TileKey{"Geographic-TMS", 0, 0, 0},
        std::vector<uint8_t>{},
        "impossible CMPT tile count fixture");
    TileContentLoadResult result =
        provider.decodeContent(cmpt.data(), cmpt.size());

    EXPECT_EQ(TileContentLoadStatus::Failed, result.status);
    EXPECT_EQ(nullptr, result.gltfModel);
}

TEST(GltfParserTest, ContentProviderDecodesSingleInnerCmptWithRuntimeAnimation) {
    const std::vector<uint8_t> cmpt =
        makeCmpt({makeAnimatedTranslationTriangleGlb()});

    SingleGltfContentProvider provider(
        TileKey{"Geographic-TMS", 0, 0, 0},
        std::vector<uint8_t>{},
        "animated single-inner CMPT fixture");
    TileContentLoadResult result =
        provider.decodeContent(cmpt.data(), cmpt.size());

    EXPECT_EQ(TileContentLoadStatus::Render, result.status);
    ASSERT_NE(nullptr, result.gltfModel);
    ASSERT_TRUE(result.gltfModel->hasRuntimeAnimation());
    ASSERT_EQ(1u, result.gltfModel->primitives.size());
    ASSERT_EQ(3u, result.gltfModel->primitives[0].vertices.size());
    EXPECT_NEAR(
        0.0,
        result.gltfModel->primitives[0].vertices[0].positionEcef.x(),
        1e-12);

    EXPECT_TRUE(result.gltfModel->updateAnimation(0.5));
    EXPECT_NEAR(
        5.0,
        result.gltfModel->primitives[0].vertices[0].positionEcef.x(),
        1e-12);
}

TEST(GltfParserTest, ContentProviderRejectsCmptWithUnsupportedInnerContent) {
    const std::vector<uint8_t> unsupportedB3dm = makeB3dm(
        makeTriangleGlb(),
        "{\"BATCH_LENGTH\":1}",
        "{\"name\":[\"building\"]}");
    const std::vector<uint8_t> cmpt =
        makeCmpt({makeTriangleGlb(), unsupportedB3dm});

    SingleGltfContentProvider provider(
        TileKey{"Geographic-TMS", 0, 0, 0},
        std::vector<uint8_t>{},
        "unsupported inner CMPT fixture");
    TileContentLoadResult result =
        provider.decodeContent(cmpt.data(), cmpt.size());

    EXPECT_EQ(TileContentLoadStatus::Failed, result.status);
    EXPECT_EQ(nullptr, result.gltfModel);
}

TEST(GltfParserTest, ContentProviderRejectsMultiInnerCmptWithRuntimeAnimation) {
    const std::vector<uint8_t> cmpt =
        makeCmpt({makeAnimatedTranslationTriangleGlb(), makeTriangleGlb()});

    SingleGltfContentProvider provider(
        TileKey{"Geographic-TMS", 0, 0, 0},
        std::vector<uint8_t>{},
        "animated multi-inner CMPT fixture");
    TileContentLoadResult result =
        provider.decodeContent(cmpt.data(), cmpt.size());

    EXPECT_EQ(TileContentLoadStatus::Failed, result.status);
    EXPECT_EQ(nullptr, result.gltfModel);
}

TEST(GltfParserTest, ContentProviderDecodesCmptWithB3dmAndPnts) {
    std::vector<uint8_t> pntsBinary;
    appendF32(pntsBinary, 4.0f);
    appendF32(pntsBinary, 5.0f);
    appendF32(pntsBinary, 6.0f);
    const size_t rgbOffset = pntsBinary.size();
    pntsBinary.insert(pntsBinary.end(), {255u, 0u, 128u});
    const std::string pntsJson =
        std::string("{") +
        "\"POINTS_LENGTH\":1,"
        "\"RTC_CENTER\":[10.0,20.0,30.0],"
        "\"POSITION\":{\"byteOffset\":0},"
        "\"RGB\":{\"byteOffset\":" +
        std::to_string(rgbOffset) + "}}";
    const std::vector<uint8_t> pnts = makePnts(pntsJson, pntsBinary);
    const std::vector<uint8_t> b3dm = makeB3dm(
        makeTriangleGlb(),
        "{\"BATCH_LENGTH\":0,\"RTC_CENTER\":[1.0,2.0,3.0]}");
    const std::vector<uint8_t> cmpt = makeCmpt({b3dm, pnts});

    SingleGltfContentProvider provider(
        TileKey{"Geographic-TMS", 0, 0, 0},
        std::vector<uint8_t>{},
        "CMPT fixture");
    provider.setContentTransform(
        Mat4::translation(Vec3(100.0, 200.0, 300.0)));
    TileContentLoadResult result =
        provider.decodeContent(cmpt.data(), cmpt.size());

    EXPECT_EQ(TileContentLoadStatus::Render, result.status);
    ASSERT_NE(nullptr, result.gltfModel);
    ASSERT_EQ(2u, result.gltfModel->primitives.size());
    EXPECT_EQ(GltfPrimitiveMode::Triangles,
              result.gltfModel->primitives[0].primitiveMode);
    EXPECT_EQ(GltfPrimitiveMode::Points,
              result.gltfModel->primitives[1].primitiveMode);

    const Vec3 triangleFirst =
        result.contentTransform *
        result.gltfModel->primitives[0].vertices[0].positionEcef;
    EXPECT_NEAR(111.0, triangleFirst.x(), 1e-12);
    EXPECT_NEAR(222.0, triangleFirst.y(), 1e-12);
    EXPECT_NEAR(333.0, triangleFirst.z(), 1e-12);

    ASSERT_EQ(1u, result.gltfModel->primitives[1].vertices.size());
    const Vec3 point =
        result.contentTransform *
        result.gltfModel->primitives[1].vertices[0].positionEcef;
    EXPECT_NEAR(114.0, point.x(), 1e-12);
    EXPECT_NEAR(225.0, point.y(), 1e-12);
    EXPECT_NEAR(336.0, point.z(), 1e-12);
    ASSERT_EQ(1u, result.gltfModel->primitives[1].vertexColors.size());
    EXPECT_NEAR(1.0f, result.gltfModel->primitives[1].vertexColors[0][0],
                1e-6f);
    EXPECT_NEAR(
        std::pow(128.0f / 255.0f, 2.2f),
        result.gltfModel->primitives[1].vertexColors[0][2],
        1e-6f);
}

TEST(GltfParserTest, ContentProviderDecodesNestedCmptContent) {
    std::vector<uint8_t> pntsBinary;
    appendF32(pntsBinary, 7.0f);
    appendF32(pntsBinary, 8.0f);
    appendF32(pntsBinary, 9.0f);
    const std::vector<uint8_t> pnts = makePnts(
        "{\"POINTS_LENGTH\":1,"
        "\"RTC_CENTER\":[10.0,20.0,30.0],"
        "\"POSITION\":{\"byteOffset\":0}}",
        pntsBinary);
    const std::vector<uint8_t> nestedB3dm = makeB3dm(
        makeTriangleGlb(),
        "{\"BATCH_LENGTH\":0,\"RTC_CENTER\":[1.0,2.0,3.0]}");
    const std::vector<uint8_t> nestedCmpt = makeCmpt({nestedB3dm, pnts});
    const std::vector<uint8_t> cmpt =
        makeCmpt({makeTriangleGlb(), nestedCmpt});

    SingleGltfContentProvider provider(
        TileKey{"Geographic-TMS", 0, 0, 0},
        std::vector<uint8_t>{},
        "nested CMPT fixture");
    TileContentLoadResult result =
        provider.decodeContent(cmpt.data(), cmpt.size());

    EXPECT_EQ(TileContentLoadStatus::Render, result.status);
    ASSERT_NE(nullptr, result.gltfModel);
    ASSERT_EQ(3u, result.gltfModel->primitives.size());
    EXPECT_EQ(
        GltfPrimitiveMode::Triangles,
        result.gltfModel->primitives[0].primitiveMode);
    EXPECT_EQ(
        GltfPrimitiveMode::Triangles,
        result.gltfModel->primitives[1].primitiveMode);
    EXPECT_EQ(
        GltfPrimitiveMode::Points,
        result.gltfModel->primitives[2].primitiveMode);

    const Vec3 firstNestedTriangle =
        result.contentTransform *
        result.gltfModel->primitives[1].vertices[0].positionEcef;
    EXPECT_NEAR(11.0, firstNestedTriangle.x(), 1e-12);
    EXPECT_NEAR(22.0, firstNestedTriangle.y(), 1e-12);
    EXPECT_NEAR(33.0, firstNestedTriangle.z(), 1e-12);

    ASSERT_EQ(1u, result.gltfModel->primitives[2].vertices.size());
    const Vec3 nestedPoint =
        result.contentTransform *
        result.gltfModel->primitives[2].vertices[0].positionEcef;
    EXPECT_NEAR(17.0, nestedPoint.x(), 1e-12);
    EXPECT_NEAR(28.0, nestedPoint.y(), 1e-12);
    EXPECT_NEAR(39.0, nestedPoint.z(), 1e-12);
}

TEST(GltfParserTest, ContentProviderDecodesCmptWithInstancedI3dm) {
    std::vector<uint8_t> pntsBinary;
    appendF32(pntsBinary, 1.0f);
    appendF32(pntsBinary, 2.0f);
    appendF32(pntsBinary, 3.0f);
    const std::vector<uint8_t> pnts = makePnts(
        "{\"POINTS_LENGTH\":1,\"POSITION\":{\"byteOffset\":0}}",
        pntsBinary);
    const std::vector<uint8_t> i3dm = makeI3dm(makeTriangleGlb(), 1u);
    const std::vector<uint8_t> cmpt = makeCmpt({i3dm, pnts});

    SingleGltfContentProvider provider(
        TileKey{"Geographic-TMS", 0, 0, 0},
        std::vector<uint8_t>{},
        "CMPT I3DM fixture");
    TileContentLoadResult result =
        provider.decodeContent(cmpt.data(), cmpt.size());

    EXPECT_EQ(TileContentLoadStatus::Render, result.status);
    ASSERT_NE(nullptr, result.gltfModel);
    ASSERT_EQ(2u, result.gltfModel->primitives.size());
    const GltfPrimitive& primitive = result.gltfModel->primitives[0];
    ASSERT_EQ(2u, primitive.instances.size());

    const Vec3 source = primitive.vertices[0].positionEcef;
    const Vec3 first =
        result.contentTransform *
        (primitive.instances[0].transform * source);
    EXPECT_NEAR(110.0, first.x(), 1e-12);
    EXPECT_NEAR(220.0, first.y(), 1e-12);
    EXPECT_NEAR(330.0, first.z(), 1e-12);

    const Vec3 second =
        result.contentTransform *
        (primitive.instances[1].transform * source);
    EXPECT_NEAR(130.0, second.x(), 1e-12);
    EXPECT_NEAR(240.0, second.y(), 1e-12);
    EXPECT_NEAR(360.0, second.z(), 1e-12);
}

TEST(GltfParserTest, ContentProviderDecodesB3dmAndAppliesRtcCenter) {
    const std::vector<uint8_t> b3dm = makeB3dm(
        makeTriangleGlb(),
        "{\"BATCH_LENGTH\":0,\"RTC_CENTER\":[1.0,2.0,3.0]}");

    SingleGltfContentProvider provider(
        TileKey{"Geographic-TMS", 0, 0, 0},
        std::vector<uint8_t>{},
        "B3DM fixture");
    provider.setContentTransform(
        Mat4::translation(Vec3(100.0, 200.0, 300.0)));

    TileContentLoadResult result =
        provider.decodeContent(b3dm.data(), b3dm.size());
    EXPECT_EQ(TileContentLoadStatus::Render, result.status);
    ASSERT_NE(nullptr, result.gltfModel);
    ASSERT_EQ(1u, result.gltfModel->primitives.size());

    const SurfaceVertex& first =
        result.gltfModel->primitives[0].vertices[0];
    const Vec3 transformed = result.contentTransform * first.positionEcef;
    EXPECT_NEAR(111.0, transformed.x(), 1e-12);
    EXPECT_NEAR(222.0, transformed.y(), 1e-12);
    EXPECT_NEAR(333.0, transformed.z(), 1e-12);
}

TEST(GltfParserTest, ContentProviderDecodesLegacyB3dmHeaders) {
    const std::array<std::vector<uint8_t>, 2> legacyTiles = {{
        makeLegacyB3dmHeader1(makeTriangleGlb(), 0u),
        makeLegacyB3dmHeader2(makeTriangleGlb(), 0u)}};

    for (const std::vector<uint8_t>& b3dm : legacyTiles) {
        SingleGltfContentProvider provider(
            TileKey{"Geographic-TMS", 0, 0, 0},
            std::vector<uint8_t>{},
            "legacy B3DM fixture");
        TileContentLoadResult result =
            provider.decodeContent(b3dm.data(), b3dm.size());

        EXPECT_EQ(TileContentLoadStatus::Render, result.status);
        ASSERT_NE(nullptr, result.gltfModel);
        ASSERT_EQ(1u, result.gltfModel->primitives.size());
        ASSERT_EQ(3u, result.gltfModel->primitives[0].vertices.size());
        const Vec3 first =
            result.contentTransform *
            result.gltfModel->primitives[0].vertices[0].positionEcef;
        EXPECT_NEAR(10.0, first.x(), 1e-12);
        EXPECT_NEAR(20.0, first.y(), 1e-12);
        EXPECT_NEAR(30.0, first.z(), 1e-12);
    }
}

TEST(GltfParserTest, ContentProviderDecodesLegacyB3dmBatchTableWithBatchIds) {
    const std::array<std::vector<uint8_t>, 2> legacyTiles = {{
        makeLegacyB3dmHeader1(
            makeLegacyBatchIdTriangleGlb(),
            2u,
            "{\"name\":[\"zero\",\"one\"],\"Height\":[100,200]}"),
        makeLegacyB3dmHeader2(
            makeLegacyBatchIdTriangleGlb(),
            2u,
            "{\"name\":[\"zero\",\"one\"],\"Height\":[100,200]}")}};

    for (const std::vector<uint8_t>& b3dm : legacyTiles) {
        SingleGltfContentProvider provider(
            TileKey{"Geographic-TMS", 0, 0, 0},
            std::vector<uint8_t>{},
            "legacy B3DM batch table fixture");
        TileContentLoadResult result =
            provider.decodeContent(b3dm.data(), b3dm.size());

        EXPECT_EQ(TileContentLoadStatus::Render, result.status);
        ASSERT_NE(nullptr, result.gltfModel);
        ASSERT_EQ(1u, result.gltfModel->primitives.size());
        const GltfPrimitive& primitive = result.gltfModel->primitives[0];
        EXPECT_EQ((std::vector<uint32_t>{1u, 0u, 1u}),
                  primitive.featureIds);
        ASSERT_EQ(3u, primitive.featureProperties.size());
        EXPECT_EQ(
            "one",
            *std::get_if<std::string>(
                &primitive.featureProperties[0].at("name")));
        EXPECT_EQ(
            "zero",
            *std::get_if<std::string>(
                &primitive.featureProperties[1].at("name")));
        EXPECT_EQ(
            200u,
            *std::get_if<uint64_t>(
                &primitive.featureProperties[0].at("Height")));
        EXPECT_EQ(
            100u,
            *std::get_if<uint64_t>(
                &primitive.featureProperties[1].at("Height")));
    }
}

TEST(GltfParserTest, ContentProviderRejectsMalformedB3dmRtcCenter) {
    const std::array<std::string, 3> featureTables = {{
        "{\"BATCH_LENGTH\":0,\"RTC_CENTER\":[1.0,2.0]}",
        "{\"BATCH_LENGTH\":0,\"RTC_CENTER\":[1.0,\"bad\",3.0]}",
        "{\"BATCH_LENGTH\":0,\"RTC_CENTER\":true}"}};

    for (const std::string& featureTable : featureTables) {
        const std::vector<uint8_t> b3dm =
            makeB3dm(makeTriangleGlb(), featureTable);

        SingleGltfContentProvider provider(
            TileKey{"Geographic-TMS", 0, 0, 0},
            std::vector<uint8_t>{},
            "malformed B3DM RTC_CENTER fixture");
        TileContentLoadResult result =
            provider.decodeContent(b3dm.data(), b3dm.size());

        EXPECT_EQ(TileContentLoadStatus::Failed, result.status);
        EXPECT_EQ(nullptr, result.gltfModel);
    }
}

TEST(GltfParserTest, ContentProviderRejectsB3dmBatchTableMetadata) {
    const std::vector<uint8_t> b3dm = makeB3dm(
        makeTriangleGlb(),
        "{\"BATCH_LENGTH\":1}",
        "{\"name\":[\"building\"]}");

    SingleGltfContentProvider provider(
        TileKey{"Geographic-TMS", 0, 0, 0},
        std::vector<uint8_t>{},
        "B3DM batch table fixture");
    TileContentLoadResult result =
        provider.decodeContent(b3dm.data(), b3dm.size());

    EXPECT_EQ(TileContentLoadStatus::Failed, result.status);
    EXPECT_EQ(nullptr, result.gltfModel);
}

TEST(GltfParserTest, ContentProviderRejectsB3dmUnsupportedMetadataExtensions) {
    auto decodeStatus = [](const std::vector<uint8_t>& b3dm) {
        SingleGltfContentProvider provider(
            TileKey{"Geographic-TMS", 0, 0, 0},
            std::vector<uint8_t>{},
            "B3DM unsupported metadata fixture");
        return provider.decodeContent(b3dm.data(), b3dm.size()).status;
    };

    EXPECT_EQ(
        TileContentLoadStatus::Failed,
        decodeStatus(makeB3dm(
            makeTriangleGlb(),
            "{\"BATCH_LENGTH\":0,\"extensions\":{\"VENDOR_feature\":{}}}")));
    EXPECT_EQ(
        TileContentLoadStatus::Failed,
        decodeStatus(makeB3dm(
            makeLegacyBatchIdTriangleGlb(),
            "{\"BATCH_LENGTH\":2}",
            "{\"HIERARCHY\":{\"instances\":[]},"
            "\"name\":[\"zero\",\"one\"]}")));
    EXPECT_EQ(
        TileContentLoadStatus::Failed,
        decodeStatus(makeB3dm(
            makeLegacyBatchIdTriangleGlb(),
            "{\"BATCH_LENGTH\":2}",
            "{\"extensions\":{\"3DTILES_batch_table_hierarchy\":{"
            "\"instances\":[]}},\"name\":[\"zero\",\"one\"]}")));
}

TEST(GltfParserTest, ContentProviderDecodesB3dmBatchTableWithBatchIds) {
    const std::vector<uint8_t> b3dm = makeB3dm(
        makeLegacyBatchIdTriangleGlb(),
        "{\"BATCH_LENGTH\":2}",
        "{\"name\":[\"zero\",\"one\"],\"Height\":[100,200]}");

    SingleGltfContentProvider provider(
        TileKey{"Geographic-TMS", 0, 0, 0},
        std::vector<uint8_t>{},
        "B3DM batch table with batch IDs fixture");
    TileContentLoadResult result =
        provider.decodeContent(b3dm.data(), b3dm.size());

    EXPECT_EQ(TileContentLoadStatus::Render, result.status);
    ASSERT_NE(nullptr, result.gltfModel);
    ASSERT_EQ(1u, result.gltfModel->primitives.size());
    const GltfPrimitive& primitive = result.gltfModel->primitives[0];
    EXPECT_EQ((std::vector<uint32_t>{1u, 0u, 1u}), primitive.featureIds);
    ASSERT_EQ(3u, primitive.featureProperties.size());
    EXPECT_EQ(
        "one",
        *std::get_if<std::string>(&primitive.featureProperties[0].at("name")));
    EXPECT_EQ(
        "zero",
        *std::get_if<std::string>(&primitive.featureProperties[1].at("name")));
    EXPECT_EQ(
        200u,
        *std::get_if<uint64_t>(&primitive.featureProperties[0].at("Height")));
    EXPECT_EQ(
        100u,
        *std::get_if<uint64_t>(&primitive.featureProperties[1].at("Height")));
}

TEST(GltfParserTest, ContentProviderRejectsB3dmBatchIdPrefixedAttribute) {
    const std::vector<uint8_t> b3dm = makeB3dm(
        makeLegacyBatchIdTriangleGlbWithSemantic("_BATCHID_0"),
        "{\"BATCH_LENGTH\":2}",
        "{\"name\":[\"zero\",\"one\"]}");

    SingleGltfContentProvider provider(
        TileKey{"Geographic-TMS", 0, 0, 0},
        std::vector<uint8_t>{},
        "B3DM prefixed batch id fixture");
    TileContentLoadResult result =
        provider.decodeContent(b3dm.data(), b3dm.size());

    EXPECT_EQ(TileContentLoadStatus::Failed, result.status);
    EXPECT_EQ(nullptr, result.gltfModel);
}

TEST(GltfParserTest, ContentProviderRejectsB3dmPositiveBatchLength) {
    const std::vector<uint8_t> b3dm = makeB3dm(
        makeTriangleGlb(),
        "{\"BATCH_LENGTH\":1}");

    SingleGltfContentProvider provider(
        TileKey{"Geographic-TMS", 0, 0, 0},
        std::vector<uint8_t>{},
        "B3DM batch length fixture");
    TileContentLoadResult result =
        provider.decodeContent(b3dm.data(), b3dm.size());

    EXPECT_EQ(TileContentLoadStatus::Failed, result.status);
    EXPECT_EQ(nullptr, result.gltfModel);
}

TEST(GltfParserTest, ContentProviderRejectsLegacyB3dmUnmappedBatchTable) {
    const std::array<std::vector<uint8_t>, 2> legacyTiles = {{
        makeLegacyB3dmHeader1(
            makeTriangleGlb(),
            1u,
            "{\"name\":[\"building\"]}"),
        makeLegacyB3dmHeader2(
            makeLegacyBatchIdTriangleGlb(),
            2u,
            "{\"name\":[\"zero\",\"one\"]}",
            {0u, 1u, 2u, 3u})}};

    for (const std::vector<uint8_t>& b3dm : legacyTiles) {
        SingleGltfContentProvider provider(
            TileKey{"Geographic-TMS", 0, 0, 0},
            std::vector<uint8_t>{},
            "legacy B3DM unsupported batch table fixture");
        TileContentLoadResult result =
            provider.decodeContent(b3dm.data(), b3dm.size());

        EXPECT_EQ(TileContentLoadStatus::Failed, result.status);
        EXPECT_EQ(nullptr, result.gltfModel);
    }
}

TEST(GltfParserTest, ContentProviderRejectsB3dmBatchIdOutsideBatchLength) {
    const std::vector<uint8_t> b3dm = makeB3dm(
        makeLegacyBatchIdTriangleGlb(),
        "{\"BATCH_LENGTH\":1}",
        "{\"name\":[\"zero\"]}");

    SingleGltfContentProvider provider(
        TileKey{"Geographic-TMS", 0, 0, 0},
        std::vector<uint8_t>{},
        "B3DM out-of-range batch ID fixture");
    TileContentLoadResult result =
        provider.decodeContent(b3dm.data(), b3dm.size());

    EXPECT_EQ(TileContentLoadStatus::Failed, result.status);
    EXPECT_EQ(nullptr, result.gltfModel);
}

TEST(GltfParserTest, ContentProviderRejectsB3dmBinaryBatchTable) {
    const std::vector<uint8_t> b3dm = makeB3dm(
        makeLegacyBatchIdTriangleGlb(),
        "{\"BATCH_LENGTH\":2}",
        "{\"name\":[\"zero\",\"one\"]}",
        {0u, 1u, 2u, 3u});

    SingleGltfContentProvider provider(
        TileKey{"Geographic-TMS", 0, 0, 0},
        std::vector<uint8_t>{},
        "B3DM binary batch table fixture");
    TileContentLoadResult result =
        provider.decodeContent(b3dm.data(), b3dm.size());

    EXPECT_EQ(TileContentLoadStatus::Failed, result.status);
    EXPECT_EQ(nullptr, result.gltfModel);
}

TEST(GltfParserTest, ContentProviderRejectsUnknownFeatureTableSemantics) {
    auto decodeStatus = [](const std::vector<uint8_t>& content) {
        SingleGltfContentProvider provider(
            TileKey{"Geographic-TMS", 0, 0, 0},
            std::vector<uint8_t>{},
            "unknown feature semantic fixture");
        return provider.decodeContent(content.data(), content.size()).status;
    };

    const std::vector<uint8_t> b3dm = makeB3dm(
        makeTriangleGlb(),
        "{\"BATCH_LENGTH\":0,\"UNKNOWN_SEMANTIC\":1}");
    EXPECT_EQ(TileContentLoadStatus::Failed, decodeStatus(b3dm));

    std::vector<uint8_t> i3dmFeatureBinary;
    appendF32(i3dmFeatureBinary, 0.0f);
    appendF32(i3dmFeatureBinary, 0.0f);
    appendF32(i3dmFeatureBinary, 0.0f);
    const std::vector<uint8_t> i3dm = makeI3dmWithFeatureTable(
        "{\"INSTANCES_LENGTH\":1,"
        "\"POSITION\":{\"byteOffset\":0},"
        "\"UNKNOWN_SEMANTIC\":1}",
        std::move(i3dmFeatureBinary));
    EXPECT_EQ(TileContentLoadStatus::Failed, decodeStatus(i3dm));

    std::vector<uint8_t> pntsFeatureBinary;
    appendF32(pntsFeatureBinary, 0.0f);
    appendF32(pntsFeatureBinary, 0.0f);
    appendF32(pntsFeatureBinary, 0.0f);
    const std::vector<uint8_t> pnts = makePnts(
        "{\"POINTS_LENGTH\":1,"
        "\"POSITION\":{\"byteOffset\":0},"
        "\"UNKNOWN_SEMANTIC\":1}",
        std::move(pntsFeatureBinary));
    EXPECT_EQ(TileContentLoadStatus::Failed, decodeStatus(pnts));
}

TEST(GltfParserTest, ContentProviderDecodesEmbeddedI3dmInstancesAndRtcCenter) {
    const std::vector<uint8_t> i3dm = makeI3dm(makeTriangleGlb(), 1u);

    SingleGltfContentProvider provider(
        TileKey{"Geographic-TMS", 0, 0, 0},
        std::vector<uint8_t>{},
        "I3DM fixture");

    TileContentLoadResult result =
        provider.decodeContent(i3dm.data(), i3dm.size());
    EXPECT_EQ(TileContentLoadStatus::Render, result.status);
    ASSERT_NE(nullptr, result.gltfModel);
    ASSERT_EQ(1u, result.gltfModel->primitives.size());
    const GltfPrimitive& primitive = result.gltfModel->primitives[0];
    ASSERT_EQ(2u, primitive.instances.size());

    const Vec3 source = primitive.vertices[0].positionEcef;
    const Vec3 first =
        result.contentTransform *
        (primitive.instances[0].transform * source);
    EXPECT_NEAR(110.0, first.x(), 1e-12);
    EXPECT_NEAR(220.0, first.y(), 1e-12);
    EXPECT_NEAR(330.0, first.z(), 1e-12);

    const Vec3 second =
        result.contentTransform *
        (primitive.instances[1].transform * source);
    EXPECT_NEAR(130.0, second.x(), 1e-12);
    EXPECT_NEAR(240.0, second.y(), 1e-12);
    EXPECT_NEAR(360.0, second.z(), 1e-12);
}

TEST(GltfParserTest, ContentProviderDecodesZeroInstanceI3dmAsEmpty) {
    const std::vector<uint8_t> i3dm = makeI3dmWithFeatureTable(
        "{\"INSTANCES_LENGTH\":0,\"POSITION\":{\"byteOffset\":0}}",
        {0u});

    SingleGltfContentProvider provider(
        TileKey{"Geographic-TMS", 0, 0, 0},
        std::vector<uint8_t>{},
        "zero-instance I3DM fixture");
    TileContentLoadResult result =
        provider.decodeContent(i3dm.data(), i3dm.size());

    EXPECT_EQ(TileContentLoadStatus::Empty, result.status);
    EXPECT_EQ(nullptr, result.gltfModel);
}

TEST(GltfParserTest, ContentProviderDecodesI3dmBatchIdFeatureProperties) {
    std::vector<uint8_t> featureBinary;
    const size_t positionsOffset = featureBinary.size();
    appendF32(featureBinary, 0.0f);
    appendF32(featureBinary, 0.0f);
    appendF32(featureBinary, 0.0f);
    appendF32(featureBinary, 10.0f);
    appendF32(featureBinary, 0.0f);
    appendF32(featureBinary, 0.0f);
    const size_t batchIdOffset = featureBinary.size();
    appendU16(featureBinary, 1u);
    appendU32(featureBinary, 0u);

    const std::vector<uint8_t> i3dm = makeI3dmWithFeatureTable(
        std::string("{\"INSTANCES_LENGTH\":2,") +
        "\"POSITION\":{\"byteOffset\":" +
        std::to_string(positionsOffset) + "}," +
        "\"BATCH_ID\":{\"byteOffset\":" +
        std::to_string(batchIdOffset) +
        ",\"componentType\":\"UNSIGNED_SHORT\"}}",
        std::move(featureBinary),
        "{\"Height\":[10,20],\"name\":[\"low\",\"high\"]}");

    SingleGltfContentProvider provider(
        TileKey{"Geographic-TMS", 0, 0, 0},
        std::vector<uint8_t>{},
        "I3DM BATCH_ID fixture");
    TileContentLoadResult result =
        provider.decodeContent(i3dm.data(), i3dm.size());

    ASSERT_EQ(TileContentLoadStatus::Render, result.status);
    ASSERT_NE(nullptr, result.gltfModel);
    ASSERT_EQ(1u, result.gltfModel->primitives.size());
    const GltfPrimitive& primitive = result.gltfModel->primitives[0];
    ASSERT_EQ(2u, primitive.instances.size());

    EXPECT_EQ(1u, primitive.instances[0].featureId);
    ASSERT_NE(
        nullptr,
        std::get_if<uint64_t>(
            &primitive.instances[0].featureProperties.at("Height")));
    EXPECT_EQ(
        20u,
        *std::get_if<uint64_t>(
            &primitive.instances[0].featureProperties.at("Height")));
    EXPECT_EQ(
        "high",
        *std::get_if<std::string>(
            &primitive.instances[0].featureProperties.at("name")));

    EXPECT_EQ(0u, primitive.instances[1].featureId);
    EXPECT_EQ(
        10u,
        *std::get_if<uint64_t>(
            &primitive.instances[1].featureProperties.at("Height")));
    EXPECT_EQ(
        "low",
        *std::get_if<std::string>(
            &primitive.instances[1].featureProperties.at("name")));
}

TEST(GltfParserTest, ContentProviderDecodesI3dmBatchIdComponentTypes) {
    auto decodeSingleInstance = [](const std::string& batchIdObject,
                                   std::vector<uint8_t> batchIdBytes) {
        std::vector<uint8_t> featureBinary;
        appendF32(featureBinary, 0.0f);
        appendF32(featureBinary, 0.0f);
        appendF32(featureBinary, 0.0f);
        const size_t batchIdOffset = featureBinary.size();
        featureBinary.insert(
            featureBinary.end(),
            batchIdBytes.begin(),
            batchIdBytes.end());

        std::string batchIdJson = batchIdObject;
        const std::string marker = "$OFFSET";
        const size_t markerPos = batchIdJson.find(marker);
        if (markerPos != std::string::npos) {
            batchIdJson.replace(
                markerPos,
                marker.size(),
                std::to_string(batchIdOffset));
        }

        const std::vector<uint8_t> i3dm = makeI3dmWithFeatureTable(
            std::string("{\"INSTANCES_LENGTH\":1,") +
            "\"POSITION\":{\"byteOffset\":0},"
            "\"BATCH_ID\":" + batchIdJson + "}",
            std::move(featureBinary),
            "{\"Height\":[10,20,30]}");

        SingleGltfContentProvider provider(
            TileKey{"Geographic-TMS", 0, 0, 0},
            std::vector<uint8_t>{},
            "I3DM BATCH_ID component fixture");
        TileContentLoadResult result =
            provider.decodeContent(i3dm.data(), i3dm.size());
        return result;
    };

    std::vector<uint8_t> defaultUshort;
    appendU16(defaultUshort, 2u);
    TileContentLoadResult defaultResult =
        decodeSingleInstance("{\"byteOffset\":$OFFSET}", defaultUshort);
    ASSERT_EQ(TileContentLoadStatus::Render, defaultResult.status);
    ASSERT_NE(nullptr, defaultResult.gltfModel);
    ASSERT_EQ(1u, defaultResult.gltfModel->primitives.size());
    ASSERT_EQ(1u, defaultResult.gltfModel->primitives[0].instances.size());
    EXPECT_EQ(
        2u,
        defaultResult.gltfModel->primitives[0].instances[0].featureId);

    TileContentLoadResult ubyteResult = decodeSingleInstance(
        "{\"byteOffset\":$OFFSET,\"componentType\":\"UNSIGNED_BYTE\"}",
        {static_cast<uint8_t>(2u)});
    ASSERT_EQ(TileContentLoadStatus::Render, ubyteResult.status);
    ASSERT_NE(nullptr, ubyteResult.gltfModel);
    EXPECT_EQ(2u, ubyteResult.gltfModel->primitives[0].instances[0].featureId);

    std::vector<uint8_t> uintBytes;
    appendU32(uintBytes, 2u);
    TileContentLoadResult uintResult = decodeSingleInstance(
        "{\"byteOffset\":$OFFSET,\"componentType\":\"UNSIGNED_INT\"}",
        uintBytes);
    ASSERT_EQ(TileContentLoadStatus::Render, uintResult.status);
    ASSERT_NE(nullptr, uintResult.gltfModel);
    EXPECT_EQ(2u, uintResult.gltfModel->primitives[0].instances[0].featureId);
}

TEST(GltfParserTest, ContentProviderRejectsMalformedI3dmBatchId) {
    auto makeWithBatchId = [](const std::string& batchIdJson,
                              std::vector<uint8_t> featureBinary,
                              const std::string& batchJson) {
        return makeI3dmWithFeatureTable(
            std::string("{\"INSTANCES_LENGTH\":1,") +
            "\"POSITION\":{\"byteOffset\":0}," +
            "\"BATCH_ID\":" + batchIdJson + "}",
            std::move(featureBinary),
            batchJson);
    };

    std::vector<uint8_t> invalidComponentBinary;
    appendF32(invalidComponentBinary, 0.0f);
    appendF32(invalidComponentBinary, 0.0f);
    appendF32(invalidComponentBinary, 0.0f);
    appendU16(invalidComponentBinary, 0u);
    EXPECT_EQ(
        TileContentLoadStatus::Failed,
        decodeI3dmStatus(makeWithBatchId(
            "{\"byteOffset\":12,\"componentType\":\"FLOAT\"}",
            std::move(invalidComponentBinary),
            "{\"Height\":[10]}")));

    std::vector<uint8_t> outOfRangeBinary;
    appendF32(outOfRangeBinary, 0.0f);
    appendF32(outOfRangeBinary, 0.0f);
    appendF32(outOfRangeBinary, 0.0f);
    EXPECT_EQ(
        TileContentLoadStatus::Failed,
        decodeI3dmStatus(makeWithBatchId(
            "{\"byteOffset\":12}",
            std::move(outOfRangeBinary),
            "{\"Height\":[10]}")));

    std::vector<uint8_t> mismatchedBatchRowsBinary;
    appendF32(mismatchedBatchRowsBinary, 0.0f);
    appendF32(mismatchedBatchRowsBinary, 0.0f);
    appendF32(mismatchedBatchRowsBinary, 0.0f);
    appendU16(mismatchedBatchRowsBinary, 2u);
    EXPECT_EQ(
        TileContentLoadStatus::Failed,
        decodeI3dmStatus(makeWithBatchId(
            "{\"byteOffset\":12}",
            std::move(mismatchedBatchRowsBinary),
            "{\"Height\":[10,20]}")));
}

TEST(GltfParserTest, ContentProviderDecodesI3dmJsonBatchTableProperties) {
    std::vector<uint8_t> featureBinary;
    appendF32(featureBinary, 0.0f);
    appendF32(featureBinary, 0.0f);
    appendF32(featureBinary, 0.0f);

    const std::vector<uint8_t> i3dm = makeI3dmWithFeatureTable(
        "{\"INSTANCES_LENGTH\":1,"
        "\"POSITION\":{\"byteOffset\":0}}",
        std::move(featureBinary),
        "{\"Height\":[20],"
        "\"name\":[\"instance\"],"
        "\"enabled\":[true],"
        "\"nullable\":[null]}");

    SingleGltfContentProvider provider(
        TileKey{"Geographic-TMS", 0, 0, 0},
        std::vector<uint8_t>{},
        "I3DM JSON batch table fixture");
    TileContentLoadResult result =
        provider.decodeContent(i3dm.data(), i3dm.size());

    ASSERT_EQ(TileContentLoadStatus::Render, result.status);
    ASSERT_NE(nullptr, result.gltfModel);
    ASSERT_EQ(1u, result.gltfModel->primitives.size());
    const GltfPrimitive& primitive = result.gltfModel->primitives[0];
    ASSERT_EQ(1u, primitive.instances.size());

    const GltfInstance& instance = primitive.instances[0];
    EXPECT_EQ(0u, instance.featureId);
    ASSERT_EQ(4u, instance.featureProperties.size());
    ASSERT_NE(nullptr,
              std::get_if<uint64_t>(&instance.featureProperties.at("Height")));
    EXPECT_EQ(
        20u,
        *std::get_if<uint64_t>(
            &instance.featureProperties.at("Height")));
    ASSERT_NE(nullptr,
              std::get_if<std::string>(
                  &instance.featureProperties.at("name")));
    EXPECT_EQ(
        "instance",
        *std::get_if<std::string>(
            &instance.featureProperties.at("name")));
    ASSERT_NE(nullptr,
              std::get_if<bool>(
                  &instance.featureProperties.at("enabled")));
    EXPECT_TRUE(
        *std::get_if<bool>(
            &instance.featureProperties.at("enabled")));
    EXPECT_TRUE(
        std::holds_alternative<std::monostate>(
            instance.featureProperties.at("nullable")));
}

TEST(GltfParserTest, ContentProviderRejectsMalformedI3dmJsonBatchTable) {
    auto decodeStatus = [](const std::string& batchJson) {
        std::vector<uint8_t> featureBinary;
        appendF32(featureBinary, 0.0f);
        appendF32(featureBinary, 0.0f);
        appendF32(featureBinary, 0.0f);
        const std::vector<uint8_t> i3dm = makeI3dmWithFeatureTable(
            "{\"INSTANCES_LENGTH\":1,"
            "\"POSITION\":{\"byteOffset\":0}}",
            std::move(featureBinary),
            batchJson);
        return decodeI3dmStatus(i3dm);
    };

    EXPECT_EQ(
        TileContentLoadStatus::Failed,
        decodeStatus("{\"Height\":[20,21]}"));
    EXPECT_EQ(
        TileContentLoadStatus::Failed,
        decodeStatus("{\"Height\":{\"byteOffset\":0}}"));
    EXPECT_EQ(
        TileContentLoadStatus::Failed,
        decodeStatus("{\"Height\":[{\"value\":20}]}"));
    EXPECT_EQ(
        TileContentLoadStatus::Failed,
        decodeStatus("{\"HIERARCHY\":{\"instances\":[]}}"));
}

TEST(GltfParserTest, ContentProviderRejectsI3dmBinaryBatchTable) {
    std::vector<uint8_t> featureBinary;
    appendF32(featureBinary, 0.0f);
    appendF32(featureBinary, 0.0f);
    appendF32(featureBinary, 0.0f);
    const std::vector<uint8_t> i3dm = makeI3dmWithFeatureTable(
        "{\"INSTANCES_LENGTH\":1,"
        "\"POSITION\":{\"byteOffset\":0}}",
        std::move(featureBinary),
        "{\"Height\":[20]}",
        {0u, 1u, 2u, 3u});

    EXPECT_EQ(TileContentLoadStatus::Failed, decodeI3dmStatus(i3dm));
}

TEST(GltfParserTest, ContentProviderRejectsI3dmEastNorthUpTypeMismatch) {
    std::vector<uint8_t> featureBinary;
    appendF32(featureBinary, 0.0f);
    appendF32(featureBinary, 0.0f);
    appendF32(featureBinary, 0.0f);

    const std::vector<uint8_t> i3dm = makeI3dmWithFeatureTable(
        "{\"INSTANCES_LENGTH\":1,"
        "\"EAST_NORTH_UP\":\"true\","
        "\"POSITION\":{\"byteOffset\":0}}",
        std::move(featureBinary));

    EXPECT_EQ(TileContentLoadStatus::Failed, decodeI3dmStatus(i3dm));
}

TEST(GltfParserTest, ContentProviderRejectsI3dmPositionWithNonFiniteFloat) {
    std::vector<uint8_t> featureBinary;
    appendF32(featureBinary, std::numeric_limits<float>::quiet_NaN());
    appendF32(featureBinary, 0.0f);
    appendF32(featureBinary, 0.0f);

    const std::vector<uint8_t> i3dm = makeI3dmWithFeatureTable(
        "{\"INSTANCES_LENGTH\":1,"
        "\"POSITION\":{\"byteOffset\":0}}",
        std::move(featureBinary));

    EXPECT_EQ(TileContentLoadStatus::Failed, decodeI3dmStatus(i3dm));
}

TEST(GltfParserTest, ContentProviderRejectsI3dmScaleWithNonFiniteFloat) {
    std::vector<uint8_t> featureBinary;
    const size_t positionsOffset = featureBinary.size();
    appendF32(featureBinary, 0.0f);
    appendF32(featureBinary, 0.0f);
    appendF32(featureBinary, 0.0f);
    const size_t scaleOffset = featureBinary.size();
    appendF32(featureBinary, std::numeric_limits<float>::infinity());

    const std::vector<uint8_t> i3dm = makeI3dmWithFeatureTable(
        std::string("{\"INSTANCES_LENGTH\":1,") +
        "\"POSITION\":{\"byteOffset\":" +
        std::to_string(positionsOffset) + "}," +
        "\"SCALE\":{\"byteOffset\":" +
        std::to_string(scaleOffset) + "}}",
        std::move(featureBinary));

    EXPECT_EQ(TileContentLoadStatus::Failed, decodeI3dmStatus(i3dm));
}

TEST(GltfParserTest, ContentProviderResolvesExternalI3dmGltfRelativeToTileUrl) {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        "earth-md-i3dm-external-gltf";
    std::filesystem::remove_all(root);
    writeBytes(root / "tiles" / "models" / "triangle.glb", makeTriangleGlb());

    const std::vector<uint8_t> i3dm =
        makeI3dm({}, 0u, "models/triangle.glb");
    SingleGltfContentProvider provider(
        TileKey{"Geographic-TMS", 0, 0, 0},
        "file://" + (root / "tiles" / "tile.i3dm").generic_string(),
        "external I3DM fixture");

    TileContentLoadResult result =
        provider.decodeContent(i3dm.data(), i3dm.size());
    EXPECT_EQ(TileContentLoadStatus::Render, result.status);
    ASSERT_NE(nullptr, result.gltfModel);
    ASSERT_EQ(1u, result.gltfModel->primitives.size());
    EXPECT_EQ(2u, result.gltfModel->primitives[0].instances.size());

    std::filesystem::remove_all(root);
}

TEST(GltfParserTest, ContentProviderCombinesI3dmAndNativeGltfInstances) {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        "earth-md-i3dm-native-gltf-instances";
    std::filesystem::remove_all(root);
    ExternalGltfFixture fixture = makeGpuInstancedExternalGltf();
    writeBytes(
        root / "tiles" / "models" / "triangle.gltf",
        std::vector<uint8_t>(
            fixture.jsonText.begin(),
            fixture.jsonText.end()));
    writeBytes(root / "tiles" / "models" / "triangle.bin", fixture.bin);

    const std::vector<uint8_t> i3dm =
        makeI3dm({}, 0u, "models/triangle.gltf");
    SingleGltfContentProvider provider(
        TileKey{"Geographic-TMS", 0, 0, 0},
        "file://" + (root / "tiles" / "tile.i3dm").generic_string(),
        "external native-instanced I3DM fixture");

    TileContentLoadResult result =
        provider.decodeContent(i3dm.data(), i3dm.size());
    EXPECT_EQ(TileContentLoadStatus::Render, result.status);
    ASSERT_NE(nullptr, result.gltfModel);
    ASSERT_EQ(1u, result.gltfModel->primitives.size());
    const GltfPrimitive& primitive = result.gltfModel->primitives[0];
    ASSERT_EQ(4u, primitive.instances.size());

    const Vec3 source = primitive.vertices[1].positionEcef;
    const Vec3 first =
        result.contentTransform *
        (primitive.instances[0].transform * source);
    EXPECT_NEAR(113.0, first.x(), 1e-6);
    EXPECT_NEAR(222.0, first.y(), 1e-6);
    EXPECT_NEAR(333.0, first.z(), 1e-6);

    const Vec3 third =
        result.contentTransform *
        (primitive.instances[2].transform * source);
    EXPECT_NEAR(136.0, third.x(), 1e-6);
    EXPECT_NEAR(244.0, third.y(), 1e-6);
    EXPECT_NEAR(366.0, third.z(), 1e-6);

    std::filesystem::remove_all(root);
}

TEST(GltfParserTest, ContentProviderCopiesI3dmFeaturesAcrossNativeGltfInstances) {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        "earth-md-i3dm-native-feature-instances";
    std::filesystem::remove_all(root);
    ExternalGltfFixture fixture = makeGpuInstancedExternalGltf();
    writeBytes(
        root / "tiles" / "models" / "triangle.gltf",
        std::vector<uint8_t>(
            fixture.jsonText.begin(),
            fixture.jsonText.end()));
    writeBytes(root / "tiles" / "models" / "triangle.bin", fixture.bin);

    std::vector<uint8_t> featureBinary;
    appendF32(featureBinary, 0.0f);
    appendF32(featureBinary, 0.0f);
    appendF32(featureBinary, 0.0f);
    appendF32(featureBinary, 10.0f);
    appendF32(featureBinary, 0.0f);
    appendF32(featureBinary, 0.0f);
    const size_t batchIdOffset = featureBinary.size();
    appendU16(featureBinary, 1u);
    appendU16(featureBinary, 0u);

    const std::vector<uint8_t> i3dm = makeExternalI3dmWithFeatureTable(
        std::string("{\"INSTANCES_LENGTH\":2,") +
        "\"RTC_CENTER\":[100.0,200.0,300.0],"
        "\"POSITION\":{\"byteOffset\":0},"
        "\"BATCH_ID\":{\"byteOffset\":" +
        std::to_string(batchIdOffset) + "}}",
        std::move(featureBinary),
        "models/triangle.gltf",
        "{\"name\":[\"zero\",\"one\"],\"Height\":[10,20]}");
    SingleGltfContentProvider provider(
        TileKey{"Geographic-TMS", 0, 0, 0},
        "file://" + (root / "tiles" / "tile.i3dm").generic_string(),
        "external native-instanced I3DM feature fixture");

    TileContentLoadResult result =
        provider.decodeContent(i3dm.data(), i3dm.size());
    EXPECT_EQ(TileContentLoadStatus::Render, result.status);
    ASSERT_NE(nullptr, result.gltfModel);
    ASSERT_EQ(1u, result.gltfModel->primitives.size());
    const GltfPrimitive& primitive = result.gltfModel->primitives[0];
    ASSERT_EQ(4u, primitive.instances.size());

    EXPECT_EQ(1u, primitive.instances[0].featureId);
    EXPECT_EQ(1u, primitive.instances[1].featureId);
    EXPECT_EQ(0u, primitive.instances[2].featureId);
    EXPECT_EQ(0u, primitive.instances[3].featureId);
    ASSERT_EQ(2u, primitive.instances[0].featureProperties.size());
    ASSERT_EQ(2u, primitive.instances[2].featureProperties.size());
    EXPECT_EQ(
        "one",
        *std::get_if<std::string>(
            &primitive.instances[0].featureProperties.at("name")));
    EXPECT_EQ(
        "one",
        *std::get_if<std::string>(
            &primitive.instances[1].featureProperties.at("name")));
    EXPECT_EQ(
        "zero",
        *std::get_if<std::string>(
            &primitive.instances[2].featureProperties.at("name")));
    EXPECT_EQ(
        10u,
        *std::get_if<uint64_t>(
            &primitive.instances[2].featureProperties.at("Height")));
    EXPECT_EQ(
        20u,
        *std::get_if<uint64_t>(
            &primitive.instances[0].featureProperties.at("Height")));

    std::filesystem::remove_all(root);
}

TEST(GltfParserTest, TilesetRejectsUnsupportedTopLevelMetadataAndStyleFields) {
    struct FieldCase {
        const char* fieldJson;
        const char* label;
    };

    const std::array<FieldCase, 7> cases = {{
        {"\"properties\":{\"height\":{\"minimum\":0}},", "properties"},
        {"\"schema\":{\"classes\":{}},", "schema"},
        {"\"schemaUri\":\"schema.json\",", "schemaUri"},
        {"\"statistics\":{},", "statistics"},
        {"\"groups\":[{}],", "groups"},
        {"\"metadata\":{},", "metadata"},
        {"\"style\":{\"color\":\"color('red')\"},", "style"},
    }};

    for (const FieldCase& testCase : cases) {
        SCOPED_TRACE(testCase.label);
        const std::string tilesetJson =
            makeTilesetJsonWithRootContent("root.glb", testCase.fieldJson);
        TilesetJsonContentProvider provider(
            "file:///earth-md/tileset.json",
            bytesFromString(tilesetJson),
            testCase.label);

        EXPECT_FALSE(provider.valid());
        EXPECT_TRUE(provider.rootTiles().empty());
    }
}

TEST(GltfParserTest, TilesetFailsTileContentWithUnsupportedMetadataFields) {
    struct MetadataCase {
        std::string tilesetJson;
        const char* label;
    };

    const std::array<MetadataCase, 3> cases = {{
        {
            makeTilesetJsonWithRootContentObject(
                "{\"uri\":\"root.glb\"}",
                std::string{},
                "\"metadata\":{},"),
            "tile metadata"},
        {
            makeTilesetJsonWithRootContentObject(
                "{\"uri\":\"root.glb\",\"metadata\":{}}"),
            "content metadata"},
        {
            makeTilesetJsonWithRootContentObject(
                "{\"uri\":\"root.glb\",\"group\":0}"),
            "content group"},
    }};

    for (const MetadataCase& testCase : cases) {
        SCOPED_TRACE(testCase.label);
        TilesetJsonContentProvider provider(
            "file:///earth-md/tileset.json",
            bytesFromString(testCase.tilesetJson),
            testCase.label);

        ASSERT_TRUE(provider.valid());
        const std::vector<TileKey> roots = provider.rootTiles();
        ASSERT_EQ(1u, roots.size());
        const std::vector<TileKey> children =
            provider.childTiles(roots.front());
        ASSERT_EQ(1u, children.size());

        TileContentLoadResult result =
            requestTileContentBlocking(provider, children.front());
        EXPECT_EQ(TileContentLoadStatus::Failed, result.status);
        EXPECT_EQ(nullptr, result.gltfModel);
    }
}

TEST(GltfParserTest, TilesetContentGltfAllowsSupportedPayloadAndRendersContent) {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        "earth-md-tileset-content-gltf-supported";
    std::filesystem::remove_all(root);
    writeBytes(root / "root.glb", makeTriangleGlb());

    const std::string tilesetJson = makeTilesetJsonWithRootContent(
        "root.glb",
        "\"extensionsUsed\":[\"3DTILES_content_gltf\"],"
        "\"extensions\":{\"3DTILES_content_gltf\":{"
        "\"extensionsUsed\":[\"KHR_mesh_quantization\"]}},");
    TilesetJsonContentProvider provider(
        "file://" + (root / "tileset.json").generic_string(),
        bytesFromString(tilesetJson),
        "3DTILES_content_gltf supported fixture");

    ASSERT_TRUE(provider.valid());
    const std::vector<TileKey> roots = provider.rootTiles();
    ASSERT_EQ(1u, roots.size());
    const std::vector<TileKey> children = provider.childTiles(roots.front());
    ASSERT_EQ(1u, children.size());

    TileContentLoadResult result =
        requestTileContentBlocking(provider, children.front());
    EXPECT_EQ(TileContentLoadStatus::Render, result.status);
    ASSERT_NE(nullptr, result.gltfModel);
    expectRenderableModelGeometry(*result.gltfModel);

    std::filesystem::remove_all(root);
}

TEST(GltfParserTest, TilesetContentGltfAllowsRequiredSupportedPayloadAndRendersContent) {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        "earth-md-tileset-content-gltf-required-supported";
    std::filesystem::remove_all(root);
    writeBytes(root / "root.glb", makeTriangleGlb());

    const std::string tilesetJson = makeTilesetJsonWithRootContent(
        "root.glb",
        "\"extensionsUsed\":[\"3DTILES_content_gltf\"],"
        "\"extensionsRequired\":[\"3DTILES_content_gltf\"],"
        "\"extensions\":{\"3DTILES_content_gltf\":{"
        "\"extensionsUsed\":[\"KHR_mesh_quantization\"],"
        "\"extensionsRequired\":[\"KHR_mesh_quantization\"]}},");
    TilesetJsonContentProvider provider(
        "file://" + (root / "tileset.json").generic_string(),
        bytesFromString(tilesetJson),
        "required 3DTILES_content_gltf supported fixture");

    ASSERT_TRUE(provider.valid());
    const std::vector<TileKey> roots = provider.rootTiles();
    ASSERT_EQ(1u, roots.size());
    const std::vector<TileKey> children = provider.childTiles(roots.front());
    ASSERT_EQ(1u, children.size());

    TileContentLoadResult result =
        requestTileContentBlocking(provider, children.front());
    EXPECT_EQ(TileContentLoadStatus::Render, result.status);
    ASSERT_NE(nullptr, result.gltfModel);
    expectRenderableModelGeometry(*result.gltfModel);

    std::filesystem::remove_all(root);
}

TEST(GltfParserTest, TilesetContentGltfAllowsEveryParserSupportedExtensionPayload) {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        "earth-md-tileset-content-gltf-all-supported";
    std::filesystem::remove_all(root);
    writeBytes(root / "root.glb", makeTriangleGlb());

    auto extensionArrayJson = [] {
        std::string json = "[";
        for (size_t i = 0; i < kSupportedGltfExtensions.size(); ++i) {
            if (i > 0) {
                json += ",";
            }
            json += "\"";
            json += std::string(kSupportedGltfExtensions[i]);
            json += "\"";
        }
        json += "]";
        return json;
    };

    const std::string extensions = extensionArrayJson();
    const std::string tilesetJson = makeTilesetJsonWithRootContent(
        "root.glb",
        "\"extensionsUsed\":[\"3DTILES_content_gltf\"],"
        "\"extensionsRequired\":[\"3DTILES_content_gltf\"],"
        "\"extensions\":{\"3DTILES_content_gltf\":{"
        "\"extensionsUsed\":" + extensions + ","
        "\"extensionsRequired\":" + extensions + "}},");
    TilesetJsonContentProvider provider(
        "file://" + (root / "tileset.json").generic_string(),
        bytesFromString(tilesetJson),
        "all supported 3DTILES_content_gltf payload fixture");

    ASSERT_TRUE(provider.valid());
    const std::vector<TileKey> roots = provider.rootTiles();
    ASSERT_EQ(1u, roots.size());
    const std::vector<TileKey> children = provider.childTiles(roots.front());
    ASSERT_EQ(1u, children.size());

    TileContentLoadResult result =
        requestTileContentBlocking(provider, children.front());
    EXPECT_EQ(TileContentLoadStatus::Render, result.status);
    ASSERT_NE(nullptr, result.gltfModel);
    expectRenderableModelGeometry(*result.gltfModel);

    std::filesystem::remove_all(root);
}

TEST(GltfParserTest, TilesetContentGltfRejectsWebpContentWithoutDecoder) {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        "earth-md-tileset-content-gltf-webp-no-decoder";
    std::filesystem::remove_all(root);
    ExternalGltfFixture fixture = makeExternalWebpTextureExtensionGltf();
    writeBytes(
        root / "root.gltf",
        std::vector<uint8_t>(
            fixture.jsonText.begin(),
            fixture.jsonText.end()));
    writeBytes(root / "triangle.bin", fixture.bin);
    writeBytes(root / "fallback.png", {9, 9, 9, 9});
    writeBytes(root / "texture.webp", makeFakeWebpBytes());

    const std::string tilesetJson = makeTilesetJsonWithRootContent(
        "root.gltf",
        "\"extensionsUsed\":[\"3DTILES_content_gltf\"],"
        "\"extensionsRequired\":[\"3DTILES_content_gltf\"],"
        "\"extensions\":{\"3DTILES_content_gltf\":{"
        "\"extensionsUsed\":[\"EXT_texture_webp\"],"
        "\"extensionsRequired\":[\"EXT_texture_webp\"]}},");
    TilesetJsonContentProvider provider(
        "file://" + (root / "tileset.json").generic_string(),
        bytesFromString(tilesetJson),
        "3DTILES_content_gltf WebP without decoder fixture");

    ASSERT_TRUE(provider.valid());
    const std::vector<TileKey> roots = provider.rootTiles();
    ASSERT_EQ(1u, roots.size());
    const std::vector<TileKey> children = provider.childTiles(roots.front());
    ASSERT_EQ(1u, children.size());

    TileContentLoadResult result =
        requestTileContentBlocking(provider, children.front());
    EXPECT_EQ(TileContentLoadStatus::Failed, result.status);
    EXPECT_EQ(nullptr, result.gltfModel);

    std::filesystem::remove_all(root);
}

TEST(GltfParserTest, TilesetContentGltfDecodesWebpContentWithDecoder) {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        "earth-md-tileset-content-gltf-webp-decoder";
    std::filesystem::remove_all(root);
    ExternalGltfFixture fixture = makeExternalWebpTextureExtensionGltf();
    writeBytes(
        root / "root.gltf",
        std::vector<uint8_t>(
            fixture.jsonText.begin(),
            fixture.jsonText.end()));
    writeBytes(root / "triangle.bin", fixture.bin);
    writeBytes(root / "fallback.png", {9, 9, 9, 9});
    writeBytes(root / "texture.webp", makeFakeWebpBytes());

    const std::string tilesetJson = makeTilesetJsonWithRootContent(
        "root.gltf",
        "\"extensionsUsed\":[\"3DTILES_content_gltf\"],"
        "\"extensionsRequired\":[\"3DTILES_content_gltf\"],"
        "\"extensions\":{\"3DTILES_content_gltf\":{"
        "\"extensionsUsed\":[\"EXT_texture_webp\"],"
        "\"extensionsRequired\":[\"EXT_texture_webp\"]}},");
    TilesetJsonContentProvider provider(
        "file://" + (root / "tileset.json").generic_string(),
        bytesFromString(tilesetJson),
        "3DTILES_content_gltf WebP decoder fixture");

    bool decodedWebp = false;
    bool decodedFallback = false;
    TestPlatformBridge bridge(
        [&](const uint8_t* data, size_t size) -> std::unique_ptr<DecodedImage> {
            if (size == 0) {
                return nullptr;
            }
            decodedWebp = bytesLookLikeFakeWebp(data, size);
            decodedFallback = data[0] == 9u;
            auto image = std::make_unique<DecodedImage>();
            image->width = 1;
            image->height = 1;
            image->channels = 4;
            image->pixels = {20, 40, 80, 255};
            return image;
        });
    provider.setPlatformBridge(&bridge);

    ASSERT_TRUE(provider.valid());
    const std::vector<TileKey> roots = provider.rootTiles();
    ASSERT_EQ(1u, roots.size());
    const std::vector<TileKey> children = provider.childTiles(roots.front());
    ASSERT_EQ(1u, children.size());

    TileContentLoadResult result =
        requestTileContentBlocking(provider, children.front());
    EXPECT_EQ(TileContentLoadStatus::Render, result.status);
    ASSERT_NE(nullptr, result.gltfModel);
    EXPECT_TRUE(decodedWebp);
    EXPECT_FALSE(decodedFallback);
    expectRenderableModelGeometry(*result.gltfModel);

    std::filesystem::remove_all(root);
}

TEST(GltfParserTest, TilesetContentGltfRejectsUndeclaredPayload) {
    const std::string tilesetJson = makeTilesetJsonWithRootContent(
        "root.glb",
        "\"extensions\":{\"3DTILES_content_gltf\":{"
        "\"extensionsUsed\":[\"KHR_mesh_quantization\"]}},");
    TilesetJsonContentProvider provider(
        "file:///earth-md/tileset.json",
        bytesFromString(tilesetJson),
        "undeclared 3DTILES_content_gltf fixture");

    EXPECT_FALSE(provider.valid());
    EXPECT_TRUE(provider.rootTiles().empty());
}

TEST(GltfParserTest, TilesetContentGltfRejectsRequiredGltfExtensionMissingUsed) {
    const std::string tilesetJson = makeTilesetJsonWithRootContent(
        "root.glb",
        "\"extensionsUsed\":[\"3DTILES_content_gltf\"],"
        "\"extensions\":{\"3DTILES_content_gltf\":{"
        "\"extensionsRequired\":[\"KHR_mesh_quantization\"]}},");
    TilesetJsonContentProvider provider(
        "file:///earth-md/tileset.json",
        bytesFromString(tilesetJson),
        "missing used glTF extension payload fixture");

    EXPECT_FALSE(provider.valid());
    EXPECT_TRUE(provider.rootTiles().empty());
}

TEST(GltfParserTest, TilesetContentGltfRejectsUnsupportedGltfExtensions) {
    const std::string tilesetJson = makeTilesetJsonWithRootContent(
        "root.glb",
        "\"extensionsUsed\":[\"3DTILES_content_gltf\"],"
        "\"extensions\":{\"3DTILES_content_gltf\":{"
        "\"extensionsUsed\":[\"KHR_draco_mesh_compression\"]}},");
    TilesetJsonContentProvider provider(
        "file:///earth-md/tileset.json",
        bytesFromString(tilesetJson),
        "unsupported 3DTILES_content_gltf fixture");

    EXPECT_FALSE(provider.valid());
    EXPECT_TRUE(provider.rootTiles().empty());
}

TEST(GltfParserTest, TilesetContentGltfRejectsMalformedGltfExtensionArrays) {
    const std::array<std::string, 4> payloads = {{
        "\"extensionsUsed\":{}",
        "\"extensionsUsed\":[7]",
        "\"extensionsUsed\":[\"KHR_mesh_quantization\","
            "\"KHR_mesh_quantization\"]",
        "\"extensionsUsed\":[\"KHR_mesh_quantization\"],"
            "\"extensionsRequired\":[\"KHR_mesh_quantization\","
            "\"KHR_mesh_quantization\"]"}};

    for (const std::string& payload : payloads) {
        SCOPED_TRACE(payload);
        const std::string tilesetJson = makeTilesetJsonWithRootContent(
            "root.glb",
            "\"extensionsUsed\":[\"3DTILES_content_gltf\"],"
            "\"extensions\":{\"3DTILES_content_gltf\":{" +
                payload + "}},");
        TilesetJsonContentProvider provider(
            "file:///earth-md/tileset.json",
            bytesFromString(tilesetJson),
            "malformed 3DTILES_content_gltf payload fixture");

        EXPECT_FALSE(provider.valid());
        EXPECT_TRUE(provider.rootTiles().empty());
    }
}
