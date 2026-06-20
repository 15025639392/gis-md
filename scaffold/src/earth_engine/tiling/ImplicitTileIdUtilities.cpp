#include "ImplicitTileIdUtilities.h"

#include "earth_engine/core/geodesy/Transforms.h"
#include "earth_engine/core/math/MathUtils.h"

#include <cmath>
#include <functional>
#include <glm/ext/matrix_transform.hpp>

namespace earth_engine {

namespace {

using TemplateSubstitution =
    std::function<std::string(const std::string& placeholder)>;

bool hasUrlScheme(const std::string& url) {
    auto isAsciiAlpha = [](unsigned char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
    };
    auto isAsciiAlphanumeric = [isAsciiAlpha](unsigned char c) {
        return isAsciiAlpha(c) || (c >= '0' && c <= '9');
    };

    for (size_t i = 0; i < url.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(url[i]);
        if (c == ':') {
            return true;
        }
        if ((i == 0 && !isAsciiAlpha(c)) ||
            (!isAsciiAlphanumeric(c) && c != '+' && c != '-' && c != '.')) {
            return false;
        }
    }

    return false;
}

std::string substituteTemplateParameters(
    const std::string& templateUri,
    const TemplateSubstitution& substitute) {
    std::string result;

    size_t startPos = 0;
    size_t nextPos = std::string::npos;
    while ((nextPos = templateUri.find('{', startPos)) != std::string::npos) {
        result.append(templateUri, startPos, nextPos - startPos);

        ++nextPos;
        const size_t endPos = templateUri.find('}', nextPos);
        if (endPos == std::string::npos) {
            startPos = nextPos - 1;
            break;
        }

        result.append(substitute(templateUri.substr(nextPos, endPos - nextPos)));
        startPos = endPos + 1;
    }

    result.append(templateUri, startPos, templateUri.length() - startPos);
    return result;
}

std::string normalizeUrlPath(const std::string& path) {
    const bool absolute = !path.empty() && path.front() == '/';
    std::vector<std::string> segments;

    size_t start = 0;
    while (start <= path.size()) {
        const size_t end = path.find('/', start);
        const std::string segment = path.substr(
            start,
            end == std::string::npos ? std::string::npos : end - start);
        if (segment == "..") {
            if (!segments.empty() && segments.back() != "..") {
                segments.pop_back();
            } else if (!absolute) {
                segments.push_back(segment);
            }
        } else if (!segment.empty() && segment != ".") {
            segments.push_back(segment);
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }

    std::string normalized = absolute ? "/" : "";
    for (size_t i = 0; i < segments.size(); ++i) {
        if (i > 0) {
            normalized.push_back('/');
        }
        normalized += segments[i];
    }
    return normalized.empty() && absolute ? "/" : normalized;
}

std::string percentEncodeUnsafeUrlBytes(std::string value) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string encoded;
    encoded.reserve(value.size());

    for (const unsigned char c : value) {
        if (c > 0x20U && c < 0x80U) {
            encoded.push_back(static_cast<char>(c));
            continue;
        }
        encoded.push_back('%');
        encoded.push_back(hex[c >> 4U]);
        encoded.push_back(hex[c & 0x0FU]);
    }

    return encoded;
}

std::string resolveUrl(const std::string& baseUrl, const std::string& relative) {
    if (hasUrlScheme(relative)) {
        return percentEncodeUnsafeUrlBytes(relative);
    }
    if (relative.rfind("//", 0) == 0) {
        return percentEncodeUnsafeUrlBytes("https:" + relative);
    }

    const std::string baseWithoutFragment = baseUrl.substr(0, baseUrl.find('#'));
    if (!relative.empty() && relative.front() == '#') {
        return percentEncodeUnsafeUrlBytes(baseWithoutFragment + relative);
    }

    std::string baseWithoutQuery = baseWithoutFragment;
    const size_t baseQueryPos = baseWithoutQuery.find('?');
    if (baseQueryPos != std::string::npos) {
        baseWithoutQuery.erase(baseQueryPos);
    }
    if (relative.empty()) {
        return percentEncodeUnsafeUrlBytes(baseWithoutQuery);
    }
    if (!relative.empty() && relative.front() == '?') {
        return percentEncodeUnsafeUrlBytes(baseWithoutQuery + relative);
    }

    std::string relativePath = relative;
    std::string relativeSuffix;
    const size_t suffixPos = relativePath.find_first_of("?#");
    if (suffixPos != std::string::npos) {
        relativeSuffix = relativePath.substr(suffixPos);
        relativePath.erase(suffixPos);
    }

    std::string prefix;
    std::string basePath;
    const size_t schemePos = baseWithoutQuery.find("://");
    if (schemePos != std::string::npos) {
        const size_t pathStart = baseWithoutQuery.find('/', schemePos + 3);
        if (pathStart == std::string::npos) {
            prefix = baseWithoutQuery;
            basePath = "/";
        } else {
            prefix = baseWithoutQuery.substr(0, pathStart);
            basePath = baseWithoutQuery.substr(pathStart);
        }
    } else if (baseWithoutQuery.rfind("//", 0) == 0) {
        const size_t pathStart = baseWithoutQuery.find('/', 2);
        if (pathStart == std::string::npos) {
            prefix = "https:" + baseWithoutQuery;
            basePath = "/";
        } else {
            prefix = "https:" + baseWithoutQuery.substr(0, pathStart);
            basePath = baseWithoutQuery.substr(pathStart);
        }
    } else {
        basePath = baseWithoutQuery;
    }

    std::string mergedPath;
    if (!relativePath.empty() && relativePath.front() == '/') {
        mergedPath = relativePath;
    } else {
        const size_t slash = basePath.rfind('/');
        const std::string directory =
            slash == std::string::npos ? std::string{} : basePath.substr(0, slash + 1);
        mergedPath = directory + relativePath;
    }

    return percentEncodeUnsafeUrlBytes(
        prefix + normalizeUrlPath(mergedPath) + relativeSuffix);
}

uint64_t spread2(uint64_t value) {
    value &= 0x00000000FFFFFFFFULL;
    value = (value | (value << 16U)) & 0x0000FFFF0000FFFFULL;
    value = (value | (value << 8U)) & 0x00FF00FF00FF00FFULL;
    value = (value | (value << 4U)) & 0x0F0F0F0F0F0F0F0FULL;
    value = (value | (value << 2U)) & 0x3333333333333333ULL;
    value = (value | (value << 1U)) & 0x5555555555555555ULL;
    return value;
}

uint64_t spread3(uint64_t value) {
    value &= 0x1FFFFFULL;
    value = (value | (value << 32U)) & 0x1F00000000FFFFULL;
    value = (value | (value << 16U)) & 0x1F0000FF0000FFULL;
    value = (value | (value << 8U)) & 0x100F00F00F00F00FULL;
    value = (value | (value << 4U)) & 0x10C30C30C30C30C3ULL;
    value = (value | (value << 2U)) & 0x1249249249249249ULL;
    return value;
}

Rectangle subdivideRectangle(const Rectangle& rootRectangle,
                             const TileKey& tileID,
                             double denominator) {
    const double latitudeSize =
        (rootRectangle.north() - rootRectangle.south()) / denominator;
    const double longitudeSize =
        (rootRectangle.east() - rootRectangle.west()) / denominator;

    const double childWest =
        rootRectangle.west() + longitudeSize * static_cast<double>(tileID.x);
    const double childEast =
        rootRectangle.west() + longitudeSize * static_cast<double>(tileID.x + 1);
    const double childSouth =
        rootRectangle.south() + latitudeSize * static_cast<double>(tileID.y);
    const double childNorth =
        rootRectangle.south() + latitudeSize * static_cast<double>(tileID.y + 1);

    return Rectangle(childWest, childSouth, childEast, childNorth);
}

} // namespace

std::vector<TileKey> ImplicitTileIdUtilities::children(
    const TileKey& parent) {
    const int childLevel = parent.z + 1;
    const int childX = parent.x << 1;
    const int childY = parent.y << 1;
    return {
        TileKey{parent.schemeId, childLevel, childX, childY},
        TileKey{parent.schemeId, childLevel, childX + 1, childY},
        TileKey{parent.schemeId, childLevel, childX, childY + 1},
        TileKey{parent.schemeId, childLevel, childX + 1, childY + 1}};
}

std::vector<OctreeTileID> ImplicitTileIdUtilities::children(
    const OctreeTileID& parent) {
    const int childLevel = parent.level + 1;
    const int childX = parent.x << 1;
    const int childY = parent.y << 1;
    const int childZ = parent.z << 1;
    return {
        OctreeTileID{childLevel, childX, childY, childZ},
        OctreeTileID{childLevel, childX + 1, childY, childZ},
        OctreeTileID{childLevel, childX, childY + 1, childZ},
        OctreeTileID{childLevel, childX + 1, childY + 1, childZ},
        OctreeTileID{childLevel, childX, childY, childZ + 1},
        OctreeTileID{childLevel, childX + 1, childY, childZ + 1},
        OctreeTileID{childLevel, childX, childY + 1, childZ + 1},
        OctreeTileID{childLevel, childX + 1, childY + 1, childZ + 1}};
}

std::string ImplicitTileIdUtilities::resolveUrl(
    const std::string& baseUrl,
    const std::string& urlTemplate,
    const TileKey& tileID) {
    const std::string url = substituteTemplateParameters(
        urlTemplate,
        [&tileID](const std::string& placeholder) {
            if (placeholder == "level") {
                return std::to_string(tileID.z);
            }
            if (placeholder == "x") {
                return std::to_string(tileID.x);
            }
            if (placeholder == "y") {
                return std::to_string(tileID.y);
            }
            return placeholder;
        });
    return earth_engine::resolveUrl(baseUrl, url);
}

std::string ImplicitTileIdUtilities::resolveUrl(
    const std::string& baseUrl,
    const std::string& urlTemplate,
    const OctreeTileID& tileID) {
    const std::string url = substituteTemplateParameters(
        urlTemplate,
        [&tileID](const std::string& placeholder) {
            if (placeholder == "level") {
                return std::to_string(tileID.level);
            }
            if (placeholder == "x") {
                return std::to_string(tileID.x);
            }
            if (placeholder == "y") {
                return std::to_string(tileID.y);
            }
            if (placeholder == "z") {
                return std::to_string(tileID.z);
            }
            return placeholder;
        });
    return earth_engine::resolveUrl(baseUrl, url);
}

OrientedBoundingBox ImplicitTileIdUtilities::computeBoundingVolume(
    const OrientedBoundingBox& rootBoundingVolume,
    const TileKey& tileID) {
    const Vec3& center = rootBoundingVolume.getCenter();
    const Vec3& xAxis = rootBoundingVolume.getHalfAxis(0);
    const Vec3& yAxis = rootBoundingVolume.getHalfAxis(1);
    const Vec3& zAxis = rootBoundingVolume.getHalfAxis(2);

    const double denominator = levelDenominator(static_cast<uint32_t>(tileID.z));
    const Vec3 minimum = center - xAxis - yAxis - zAxis;
    const Vec3 xDim = xAxis * (2.0 / denominator);
    const Vec3 yDim = yAxis * (2.0 / denominator);

    const Vec3 childMin =
        minimum + xDim * static_cast<double>(tileID.x) +
        yDim * static_cast<double>(tileID.y);
    const Vec3 childMax =
        minimum + xDim * static_cast<double>(tileID.x + 1) +
        yDim * static_cast<double>(tileID.y + 1) + zAxis * 2.0;

    return OrientedBoundingBox(
        (childMin + childMax) * 0.5,
        xDim * 0.5,
        yDim * 0.5,
        zAxis);
}

OrientedBoundingBox ImplicitTileIdUtilities::computeBoundingVolume(
    const OrientedBoundingBox& rootBoundingVolume,
    const OctreeTileID& tileID) {
    const Vec3& center = rootBoundingVolume.getCenter();
    const Vec3& xAxis = rootBoundingVolume.getHalfAxis(0);
    const Vec3& yAxis = rootBoundingVolume.getHalfAxis(1);
    const Vec3& zAxis = rootBoundingVolume.getHalfAxis(2);

    const double denominator =
        levelDenominator(static_cast<uint32_t>(tileID.level));
    const Vec3 minimum = center - xAxis - yAxis - zAxis;
    const Vec3 xDim = xAxis * (2.0 / denominator);
    const Vec3 yDim = yAxis * (2.0 / denominator);
    const Vec3 zDim = zAxis * (2.0 / denominator);

    const Vec3 childMin =
        minimum + xDim * static_cast<double>(tileID.x) +
        yDim * static_cast<double>(tileID.y) +
        zDim * static_cast<double>(tileID.z);
    const Vec3 childMax =
        minimum + xDim * static_cast<double>(tileID.x + 1) +
        yDim * static_cast<double>(tileID.y + 1) +
        zDim * static_cast<double>(tileID.z + 1);

    return OrientedBoundingBox(
        (childMin + childMax) * 0.5,
        xDim * 0.5,
        yDim * 0.5,
        zDim * 0.5);
}

BoundingCylinderRegion ImplicitTileIdUtilities::computeBoundingVolume(
    const BoundingCylinderRegion& rootBoundingVolume,
    const TileKey& tileID) {
    const double denominator =
        levelDenominator(static_cast<uint32_t>(tileID.z));

    const glm::dvec2& rootRadialBounds =
        rootBoundingVolume.getRadialBounds();
    const glm::dvec2& rootAngularBounds =
        rootBoundingVolume.getAngularBounds();

    const bool angleReversed = rootAngularBounds.x > rootAngularBounds.y;

    const double rootRadiusRange = rootRadialBounds.y - rootRadialBounds.x;
    double rootAngularRange = std::abs(rootAngularBounds.y - rootAngularBounds.x);
    if (angleReversed) {
        rootAngularRange = MathUtils::TwoPi - rootAngularRange;
    }

    const double radiusDim = rootRadiusRange / denominator;
    const double angleDim = rootAngularRange / denominator;

    const double minRadius =
        rootRadialBounds.x + static_cast<double>(tileID.x) * radiusDim;
    double minAngle =
        rootAngularBounds.x + static_cast<double>(tileID.y) * angleDim;
    double maxAngle = minAngle + angleDim;

    if (minAngle >= MathUtils::OnePi) {
        minAngle = MathUtils::convertLongitudeRange(minAngle);
    }
    if (maxAngle > MathUtils::OnePi) {
        maxAngle = MathUtils::convertLongitudeRange(maxAngle);
    }

    return BoundingCylinderRegion(
        rootBoundingVolume.getTranslation(),
        rootBoundingVolume.getRotation(),
        rootBoundingVolume.getHeight(),
        glm::dvec2(minRadius, minRadius + radiusDim),
        glm::dvec2(minAngle, maxAngle));
}

BoundingCylinderRegion ImplicitTileIdUtilities::computeBoundingVolume(
    const BoundingCylinderRegion& rootBoundingVolume,
    const OctreeTileID& tileID) {
    const double denominator =
        levelDenominator(static_cast<uint32_t>(tileID.level));

    const BoundingCylinderRegion quadtreeRegion = computeBoundingVolume(
        rootBoundingVolume,
        TileKey{"", tileID.level, tileID.x, tileID.y});

    const double rootHeight = rootBoundingVolume.getHeight();
    const double heightDim = rootHeight / denominator;

    double heightOffset = -0.5 * rootHeight + 0.5 * heightDim;
    heightOffset += heightDim * static_cast<double>(tileID.z);

    const Mat4 rootTransform =
        Transforms::createTranslationRotationScaleMatrix(
            rootBoundingVolume.getTranslation(),
            rootBoundingVolume.getRotation(),
            Vec3(1.0, 1.0, 1.0));
    const glm::dmat4 heightTranslation =
        glm::translate(glm::dmat4(1.0), glm::dvec3(0.0, 0.0, heightOffset));
    const Mat4 transform(rootTransform.raw() * heightTranslation);

    Vec3 translation;
    glm::dquat rotation(1.0, 0.0, 0.0, 0.0);
    Transforms::computeTranslationRotationScaleFromMatrix(
        transform,
        &translation,
        &rotation,
        nullptr);

    return BoundingCylinderRegion(
        translation,
        rotation,
        heightDim,
        quadtreeRegion.getRadialBounds(),
        quadtreeRegion.getAngularBounds());
}

TileBoundingVolume ImplicitTileIdUtilities::computeRegionBoundingVolume(
    const TileBoundingVolume& rootBoundingVolume,
    const TileKey& tileID) {
    const double denominator =
        levelDenominator(static_cast<uint32_t>(tileID.z));

    return TileBoundingVolume::fromRegion(
        subdivideRectangle(rootBoundingVolume.region, tileID, denominator),
        rootBoundingVolume.minimumHeight,
        rootBoundingVolume.maximumHeight);
}

TileBoundingVolume ImplicitTileIdUtilities::computeRegionBoundingVolume(
    const TileBoundingVolume& rootBoundingVolume,
    const OctreeTileID& tileID) {
    const double denominator =
        levelDenominator(static_cast<uint32_t>(tileID.level));
    const double heightSize =
        (rootBoundingVolume.maximumHeight - rootBoundingVolume.minimumHeight) /
        denominator;

    const double childMinimumHeight =
        rootBoundingVolume.minimumHeight +
        heightSize * static_cast<double>(tileID.z);
    const double childMaximumHeight =
        rootBoundingVolume.minimumHeight +
        heightSize * static_cast<double>(tileID.z + 1);

    return TileBoundingVolume::fromRegion(
        subdivideRectangle(
            rootBoundingVolume.region,
            TileKey{"", tileID.level, tileID.x, tileID.y},
            denominator),
        childMinimumHeight,
        childMaximumHeight);
}

S2CellBoundingVolume ImplicitTileIdUtilities::computeBoundingVolume(
    const S2CellBoundingVolume& rootBoundingVolume,
    const TileKey& tileID) {
    const uint8_t face = rootBoundingVolume.getCellID().getFace();
    return S2CellBoundingVolume(
        S2CellID::fromQuadtreeTileID(
            face,
            static_cast<uint32_t>(tileID.z),
            static_cast<uint32_t>(tileID.x),
            static_cast<uint32_t>(tileID.y)),
        rootBoundingVolume.getMinimumHeight(),
        rootBoundingVolume.getMaximumHeight());
}

S2CellBoundingVolume ImplicitTileIdUtilities::computeBoundingVolume(
    const S2CellBoundingVolume& rootBoundingVolume,
    const OctreeTileID& tileID) {
    const double denominator =
        levelDenominator(static_cast<uint32_t>(tileID.level));
    const double heightSize =
        (rootBoundingVolume.getMaximumHeight() -
         rootBoundingVolume.getMinimumHeight()) /
        denominator;

    const double childMinimumHeight =
        rootBoundingVolume.getMinimumHeight() +
        heightSize * static_cast<double>(tileID.z);
    const double childMaximumHeight =
        rootBoundingVolume.getMinimumHeight() +
        heightSize * static_cast<double>(tileID.z + 1);

    const uint8_t face = rootBoundingVolume.getCellID().getFace();
    return S2CellBoundingVolume(
        S2CellID::fromQuadtreeTileID(
            face,
            static_cast<uint32_t>(tileID.level),
            static_cast<uint32_t>(tileID.x),
            static_cast<uint32_t>(tileID.y)),
        childMinimumHeight,
        childMaximumHeight);
}

TileBoundingVolume ImplicitTileIdUtilities::computeBoundingVolume(
    const TileBoundingVolume& rootBoundingVolume,
    const TileKey& tileID) {
    switch (rootBoundingVolume.kind) {
        case TileBoundingVolumeKind::Box: {
            const OrientedBoundingBox box =
                computeBoundingVolume(rootBoundingVolume.box, tileID);
            return TileBoundingVolume::fromBox(
                box.getCenter(),
                box.getHalfAxis(0),
                box.getHalfAxis(1),
                box.getHalfAxis(2));
        }
        case TileBoundingVolumeKind::Region:
            return computeRegionBoundingVolume(rootBoundingVolume, tileID);
        case TileBoundingVolumeKind::CylinderRegion:
            return TileBoundingVolume::fromCylinderRegion(
                computeBoundingVolume(
                    rootBoundingVolume.cylinderRegion,
                    tileID));
        case TileBoundingVolumeKind::S2Cell:
            return TileBoundingVolume::fromS2Cell(
                computeBoundingVolume(rootBoundingVolume.s2Cell, tileID));
        case TileBoundingVolumeKind::Sphere:
            return rootBoundingVolume;
    }
    return rootBoundingVolume;
}

TileBoundingVolume ImplicitTileIdUtilities::computeBoundingVolume(
    const TileBoundingVolume& rootBoundingVolume,
    const OctreeTileID& tileID) {
    switch (rootBoundingVolume.kind) {
        case TileBoundingVolumeKind::Box: {
            const OrientedBoundingBox box =
                computeBoundingVolume(rootBoundingVolume.box, tileID);
            return TileBoundingVolume::fromBox(
                box.getCenter(),
                box.getHalfAxis(0),
                box.getHalfAxis(1),
                box.getHalfAxis(2));
        }
        case TileBoundingVolumeKind::Region:
            return computeRegionBoundingVolume(rootBoundingVolume, tileID);
        case TileBoundingVolumeKind::CylinderRegion:
            return TileBoundingVolume::fromCylinderRegion(
                computeBoundingVolume(
                    rootBoundingVolume.cylinderRegion,
                    tileID));
        case TileBoundingVolumeKind::S2Cell:
            return TileBoundingVolume::fromS2Cell(
                computeBoundingVolume(rootBoundingVolume.s2Cell, tileID));
        case TileBoundingVolumeKind::Sphere:
            return rootBoundingVolume;
    }
    return rootBoundingVolume;
}

std::optional<TileKey> ImplicitTileIdUtilities::parentId(
    const TileKey& tileID) {
    if (tileID.z == 0) {
        return std::nullopt;
    }
    return TileKey{tileID.schemeId, tileID.z - 1, tileID.x >> 1, tileID.y >> 1};
}

std::optional<OctreeTileID> ImplicitTileIdUtilities::parentId(
    const OctreeTileID& tileID) {
    if (tileID.level == 0) {
        return std::nullopt;
    }
    return OctreeTileID{
        tileID.level - 1,
        tileID.x >> 1,
        tileID.y >> 1,
        tileID.z >> 1};
}

TileKey ImplicitTileIdUtilities::subtreeRootId(
    uint32_t subtreeLevels,
    const TileKey& tileID) {
    const uint32_t level = static_cast<uint32_t>(tileID.z);
    const uint32_t subtreeLevel = level / subtreeLevels;
    const uint32_t levelsLeft = level % subtreeLevels;
    return TileKey{
        tileID.schemeId,
        static_cast<int>(subtreeLevel * subtreeLevels),
        tileID.x >> levelsLeft,
        tileID.y >> levelsLeft};
}

OctreeTileID ImplicitTileIdUtilities::subtreeRootId(
    uint32_t subtreeLevels,
    const OctreeTileID& tileID) {
    const uint32_t level = static_cast<uint32_t>(tileID.level);
    const uint32_t subtreeLevel = level / subtreeLevels;
    const uint32_t levelsLeft = level % subtreeLevels;
    return OctreeTileID{
        static_cast<int>(subtreeLevel * subtreeLevels),
        tileID.x >> levelsLeft,
        tileID.y >> levelsLeft,
        tileID.z >> levelsLeft};
}

TileKey ImplicitTileIdUtilities::absoluteTileIdToRelative(
    const TileKey& rootID,
    const TileKey& tileID) {
    const int relativeLevel = tileID.z - rootID.z;
    return TileKey{
        tileID.schemeId,
        relativeLevel,
        tileID.x - (rootID.x << relativeLevel),
        tileID.y - (rootID.y << relativeLevel)};
}

OctreeTileID ImplicitTileIdUtilities::absoluteTileIdToRelative(
    const OctreeTileID& rootID,
    const OctreeTileID& tileID) {
    const int relativeLevel = tileID.level - rootID.level;
    return OctreeTileID{
        relativeLevel,
        tileID.x - (rootID.x << relativeLevel),
        tileID.y - (rootID.y << relativeLevel),
        tileID.z - (rootID.z << relativeLevel)};
}

uint64_t ImplicitTileIdUtilities::mortonIndex(const TileKey& tileID) {
    return spread2(static_cast<uint32_t>(tileID.x)) |
           (spread2(static_cast<uint32_t>(tileID.y)) << 1U);
}

uint64_t ImplicitTileIdUtilities::mortonIndex(const OctreeTileID& tileID) {
    return spread3(static_cast<uint32_t>(tileID.x)) |
           (spread3(static_cast<uint32_t>(tileID.y)) << 1U) |
           (spread3(static_cast<uint32_t>(tileID.z)) << 2U);
}

uint64_t ImplicitTileIdUtilities::relativeMortonIndex(
    const TileKey& subtreeID,
    const TileKey& tileID) {
    return mortonIndex(absoluteTileIdToRelative(subtreeID, tileID));
}

uint64_t ImplicitTileIdUtilities::relativeMortonIndex(
    const OctreeTileID& subtreeID,
    const OctreeTileID& tileID) {
    return mortonIndex(absoluteTileIdToRelative(subtreeID, tileID));
}

double ImplicitTileIdUtilities::levelDenominator(uint32_t level) {
    return std::ldexp(1.0, static_cast<int>(level));
}

} // namespace earth_engine
