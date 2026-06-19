#include "ImplicitTileIdUtilities.h"

#include <cmath>

namespace earth_engine {

namespace {

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
