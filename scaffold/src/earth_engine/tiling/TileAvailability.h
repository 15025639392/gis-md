#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <variant>
#include <vector>

#include "OctreeTilingScheme.h"

namespace earth_engine {

enum TileAvailabilityFlags : uint8_t {
    TileAvailable = 1U,
    ContentAvailable = 2U,
    SubtreeAvailable = 4U,
    SubtreeLoaded = 8U,
    Reachable = 16U
};

namespace TileAvailabilityUtilities {
uint8_t countOnesInByte(uint8_t byte);
uint32_t countOnesInBuffer(const std::byte* buffer, size_t size);
uint32_t countOnesInBuffer(const std::vector<std::byte>& buffer);
} // namespace TileAvailabilityUtilities

struct ConstantTileAvailability {
    bool constant = false;
};

struct TileSubtreeBufferView {
    uint32_t byteOffset = 0;
    uint32_t byteLength = 0;
    uint8_t buffer = 0;
};

using TileAvailabilityView =
    std::variant<ConstantTileAvailability, TileSubtreeBufferView>;

struct TileAvailabilitySubtree {
    TileAvailabilityView tileAvailability;
    TileAvailabilityView contentAvailability;
    TileAvailabilityView subtreeAvailability;
    std::vector<std::vector<std::byte>> buffers;
};

struct TileAvailabilityNode {
    std::optional<TileAvailabilitySubtree> subtree;
    std::vector<std::unique_ptr<TileAvailabilityNode>> childNodes;

    void setLoadedSubtree(TileAvailabilitySubtree&& subtree,
                          uint32_t maxChildrenSubtrees);
};

class TileAvailabilityAccessor {
public:
    TileAvailabilityAccessor(const TileAvailabilityView& view,
                             const TileAvailabilitySubtree& subtree);

    bool isConstant() const { return constant_ != nullptr; }
    bool isBufferView() const {
        return bufferView_ != nullptr && bufferData_ != nullptr;
    }
    bool getConstant() const { return constant_->constant; }
    const std::byte* getBufferAccessor() const { return bufferData_; }
    const std::byte& operator[](size_t i) const {
        return bufferData_[i];
    }
    size_t size() const { return bufferView_->byteLength; }

private:
    const ConstantTileAvailability* constant_ = nullptr;
    const TileSubtreeBufferView* bufferView_ = nullptr;
    const std::byte* bufferData_ = nullptr;
};

class TileQuadtreeAvailability {
public:
    TileQuadtreeAvailability(uint32_t subtreeLevels, uint32_t maximumLevel);

    uint8_t computeAvailability(uint32_t level,
                                uint32_t x,
                                uint32_t y) const;
    uint8_t computeAvailability(uint32_t level,
                                uint32_t x,
                                uint32_t y,
                                const TileAvailabilityNode* node) const;
    bool addSubtree(uint32_t level,
                    uint32_t x,
                    uint32_t y,
                    TileAvailabilitySubtree&& subtree);
    TileAvailabilityNode* addNode(uint32_t level,
                                  uint32_t x,
                                  uint32_t y,
                                  TileAvailabilityNode* parentNode);
    bool addLoadedSubtree(TileAvailabilityNode* node,
                          TileAvailabilitySubtree&& subtree);
    std::optional<uint32_t> findChildNodeIndex(
        uint32_t level,
        uint32_t x,
        uint32_t y,
        const TileAvailabilityNode* parentNode) const;
    TileAvailabilityNode* findChildNode(
        uint32_t level,
        uint32_t x,
        uint32_t y,
        TileAvailabilityNode* parentNode) const;

    TileAvailabilityNode* rootNode() { return root_.get(); }
    const TileAvailabilityNode* rootNode() const { return root_.get(); }

private:
    uint32_t subtreeLevels_ = 0;
    uint32_t maximumLevel_ = 0;
    uint32_t maximumChildrenSubtrees_ = 0;
    std::unique_ptr<TileAvailabilityNode> root_;
};

class TileOctreeAvailability {
public:
    TileOctreeAvailability(uint32_t subtreeLevels, uint32_t maximumLevel);

    uint8_t computeAvailability(const OctreeTileID& tileID) const;
    uint8_t computeAvailability(const OctreeTileID& tileID,
                                const TileAvailabilityNode* node) const;
    bool addSubtree(const OctreeTileID& tileID,
                    TileAvailabilitySubtree&& subtree);
    TileAvailabilityNode* addNode(const OctreeTileID& tileID,
                                  TileAvailabilityNode* parentNode);
    bool addLoadedSubtree(TileAvailabilityNode* node,
                          TileAvailabilitySubtree&& subtree);
    std::optional<uint32_t> findChildNodeIndex(
        const OctreeTileID& tileID,
        const TileAvailabilityNode* parentNode) const;
    TileAvailabilityNode* findChildNode(
        const OctreeTileID& tileID,
        TileAvailabilityNode* parentNode) const;

    TileAvailabilityNode* rootNode() { return root_.get(); }
    const TileAvailabilityNode* rootNode() const { return root_.get(); }

private:
    uint32_t subtreeLevels_ = 0;
    uint32_t maximumLevel_ = 0;
    uint32_t maximumChildrenSubtrees_ = 0;
    std::unique_ptr<TileAvailabilityNode> root_;
};

} // namespace earth_engine
