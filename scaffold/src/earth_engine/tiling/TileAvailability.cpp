#include "TileAvailability.h"

#include <utility>

namespace earth_engine {

namespace {

uint16_t mortonIndexForBytes(uint8_t a, uint8_t b) {
    return static_cast<uint16_t>(
        (((a * 0x0101010101010101ULL & 0x8040201008040201ULL) *
              0x0102040810204081ULL >>
          49) &
         0x5555) |
        (((b * 0x0101010101010101ULL & 0x8040201008040201ULL) *
              0x0102040810204081ULL >>
          48) &
         0xAAAA));
}

uint32_t mortonIndex(uint32_t x, uint32_t y) {
    uint16_t a = static_cast<uint16_t>(x);
    uint16_t b = static_cast<uint16_t>(y);
    uint8_t* firstA = reinterpret_cast<uint8_t*>(&a);
    uint8_t* firstB = reinterpret_cast<uint8_t*>(&b);

    uint32_t result = 0;
    uint16_t* firstResult = reinterpret_cast<uint16_t*>(&result);
    *firstResult = mortonIndexForBytes(*firstA, *firstB);
    *(firstResult + 1) = mortonIndexForBytes(*(firstA + 1), *(firstB + 1));
    return result;
}

uint32_t lowBitsMask(uint32_t bits) {
    return bits == 0 ? 0U : ~(0xFFFFFFFFU << bits);
}

bool availabilityBitSet(const TileAvailabilityAccessor& accessor,
                        uint32_t availabilityIndex) {
    if (accessor.isConstant()) {
        return accessor.getConstant();
    }
    if (!accessor.isBufferView()) {
        return false;
    }
    const uint32_t byteIndex = availabilityIndex >> 3U;
    if (byteIndex >= accessor.size()) {
        return false;
    }
    const uint8_t bitIndex = static_cast<uint8_t>(availabilityIndex & 7U);
    const uint8_t bitMask = static_cast<uint8_t>(1U << bitIndex);
    return (static_cast<uint8_t>(accessor[byteIndex]) & bitMask) != 0;
}

std::optional<uint32_t> childSubtreeIndex(
    const TileAvailabilityAccessor& subtreeAccessor,
    uint32_t childSubtreeMortonIndex) {
    if (subtreeAccessor.isConstant()) {
        if (!subtreeAccessor.getConstant()) {
            return std::nullopt;
        }
        return childSubtreeMortonIndex;
    }

    if (!subtreeAccessor.isBufferView()) {
        return std::nullopt;
    }

    const uint32_t byteIndex = childSubtreeMortonIndex >> 3U;
    if (byteIndex >= subtreeAccessor.size()) {
        return std::nullopt;
    }
    const uint8_t bitIndex =
        static_cast<uint8_t>(childSubtreeMortonIndex & 7U);
    const uint8_t bitMask = static_cast<uint8_t>(1U << bitIndex);
    const uint8_t availabilityByte =
        static_cast<uint8_t>(subtreeAccessor[byteIndex]);
    if ((availabilityByte & bitMask) == 0) {
        return std::nullopt;
    }

    const uint32_t previousOnes =
        TileAvailabilityUtilities::countOnesInBuffer(
            subtreeAccessor.getBufferAccessor(),
            byteIndex);
    const uint32_t currentByteOnes =
        TileAvailabilityUtilities::countOnesInByte(
            static_cast<uint8_t>(availabilityByte << (8U - bitIndex)));
    return previousOnes + currentByteOnes;
}

} // namespace

namespace TileAvailabilityUtilities {

uint8_t countOnesInByte(uint8_t byte) {
    uint8_t count = 0;
    while (byte != 0) {
        count += byte & 1U;
        byte >>= 1U;
    }
    return count;
}

uint32_t countOnesInBuffer(const std::byte* buffer, size_t size) {
    uint32_t count = 0;
    for (size_t i = 0; i < size; ++i) {
        count += countOnesInByte(static_cast<uint8_t>(buffer[i]));
    }
    return count;
}

uint32_t countOnesInBuffer(const std::vector<std::byte>& buffer) {
    return countOnesInBuffer(buffer.data(), buffer.size());
}

} // namespace TileAvailabilityUtilities

TileAvailabilityAccessor::TileAvailabilityAccessor(
    const TileAvailabilityView& view,
    const TileAvailabilitySubtree& subtree)
    : constant_(std::get_if<ConstantTileAvailability>(&view)),
      bufferView_(std::get_if<TileSubtreeBufferView>(&view)) {
    if (!bufferView_ || bufferView_->buffer >= subtree.buffers.size()) {
        return;
    }

    const std::vector<std::byte>& buffer = subtree.buffers[bufferView_->buffer];
    if (bufferView_->byteOffset + bufferView_->byteLength > buffer.size()) {
        return;
    }

    bufferData_ = buffer.data() + bufferView_->byteOffset;
}

void TileAvailabilityNode::setLoadedSubtree(
    TileAvailabilitySubtree&& loadedSubtree,
    uint32_t maxChildrenSubtrees) {
    subtree = std::move(loadedSubtree);
    TileAvailabilityAccessor subtreeAccessor(
        subtree->subtreeAvailability,
        *subtree);

    size_t childCount = 0;
    if (subtreeAccessor.isConstant()) {
        childCount = subtreeAccessor.getConstant()
            ? static_cast<size_t>(maxChildrenSubtrees)
            : 0U;
    } else if (subtreeAccessor.isBufferView()) {
        childCount = TileAvailabilityUtilities::countOnesInBuffer(
            subtreeAccessor.getBufferAccessor(),
            subtreeAccessor.size());
    }
    childNodes.resize(childCount);
}

TileQuadtreeAvailability::TileQuadtreeAvailability(
    uint32_t subtreeLevels,
    uint32_t maximumLevel)
    : subtreeLevels_(subtreeLevels),
      maximumLevel_(maximumLevel),
      maximumChildrenSubtrees_(1U << (subtreeLevels << 1U)) {}

uint8_t TileQuadtreeAvailability::computeAvailability(
    uint32_t level,
    uint32_t x,
    uint32_t y) const {
    if (!root_ && level == 0) {
        return TileAvailable | SubtreeAvailable;
    }
    if (!root_ || level > maximumLevel_) {
        return 0;
    }

    uint32_t nodeLevel = 0;
    const TileAvailabilityNode* node = root_.get();
    while (node && node->subtree && level >= nodeLevel) {
        const TileAvailabilitySubtree& subtree = *node->subtree;
        TileAvailabilityAccessor tileAccessor(subtree.tileAvailability, subtree);
        TileAvailabilityAccessor contentAccessor(
            subtree.contentAvailability,
            subtree);
        TileAvailabilityAccessor subtreeAccessor(
            subtree.subtreeAvailability,
            subtree);

        const uint32_t levelsLeft = level - nodeLevel;
        const uint32_t subtreeRelativeMask = lowBitsMask(levelsLeft);
        if (levelsLeft < subtreeLevels_) {
            uint8_t availability = Reachable;
            const uint32_t relativeMortonIndex =
                mortonIndex(x & subtreeRelativeMask, y & subtreeRelativeMask);
            const uint32_t offset =
                ((1U << (levelsLeft << 1U)) - 1U) / 3U;
            const uint32_t availabilityIndex = relativeMortonIndex + offset;
            if (availabilityBitSet(tileAccessor, availabilityIndex)) {
                availability |= TileAvailable;
            }
            if (availabilityBitSet(contentAccessor, availabilityIndex)) {
                availability |= ContentAvailable;
            }
            if (levelsLeft == 0) {
                availability |= SubtreeAvailable | SubtreeLoaded;
            }
            return availability;
        }

        const uint32_t levelsLeftAfterNextLevel = levelsLeft - subtreeLevels_;
        const uint32_t childMortonIndex = mortonIndex(
            (x & subtreeRelativeMask) >> levelsLeftAfterNextLevel,
            (y & subtreeRelativeMask) >> levelsLeftAfterNextLevel);
        const std::optional<uint32_t> childIndex =
            childSubtreeIndex(subtreeAccessor, childMortonIndex);
        if (!childIndex) {
            return Reachable;
        }
        node = *childIndex < node->childNodes.size()
            ? node->childNodes[*childIndex].get()
            : nullptr;
        nodeLevel += subtreeLevels_;
    }

    if (level == nodeLevel) {
        return TileAvailable | SubtreeAvailable;
    }
    return 0;
}

uint8_t TileQuadtreeAvailability::computeAvailability(
    uint32_t level,
    uint32_t x,
    uint32_t y,
    const TileAvailabilityNode* node) const {
    const bool subtreeLoaded = node && node->subtree.has_value();
    const uint32_t relativeLevel = level % subtreeLevels_;
    if (!subtreeLoaded) {
        return relativeLevel == 0 ? static_cast<uint8_t>(
                   TileAvailable | SubtreeAvailable)
                                  : 0U;
    }

    const TileAvailabilitySubtree& subtree = *node->subtree;
    TileAvailabilityAccessor tileAccessor(subtree.tileAvailability, subtree);
    TileAvailabilityAccessor contentAccessor(subtree.contentAvailability, subtree);

    const uint32_t subtreeRelativeMask = lowBitsMask(relativeLevel);
    uint8_t availability = Reachable;
    const uint32_t relativeMortonIndex =
        mortonIndex(x & subtreeRelativeMask, y & subtreeRelativeMask);
    const uint32_t offset = ((1U << (relativeLevel << 1U)) - 1U) / 3U;
    const uint32_t availabilityIndex = relativeMortonIndex + offset;
    if (availabilityBitSet(tileAccessor, availabilityIndex)) {
        availability |= TileAvailable;
    }
    if (availabilityBitSet(contentAccessor, availabilityIndex)) {
        availability |= ContentAvailable;
    }
    if (relativeLevel == 0) {
        availability |= TileAvailable | SubtreeAvailable | SubtreeLoaded;
    }
    return availability;
}

bool TileQuadtreeAvailability::addSubtree(
    uint32_t level,
    uint32_t x,
    uint32_t y,
    TileAvailabilitySubtree&& subtree) {
    if (level == 0) {
        if (root_) return false;
        root_ = std::make_unique<TileAvailabilityNode>();
        root_->setLoadedSubtree(std::move(subtree), maximumChildrenSubtrees_);
        return true;
    }
    if (!root_) return false;

    TileAvailabilityNode* node = root_.get();
    uint32_t nodeLevel = 0;
    while (node && node->subtree && level > nodeLevel) {
        const uint32_t levelsLeft = level - nodeLevel;
        if (levelsLeft < subtreeLevels_) {
            return false;
        }

        const TileAvailabilitySubtree& loadedSubtree = *node->subtree;
        TileAvailabilityAccessor subtreeAccessor(
            loadedSubtree.subtreeAvailability,
            loadedSubtree);
        const uint32_t subtreeRelativeMask = lowBitsMask(levelsLeft);
        const uint32_t levelsLeftAfterChild = levelsLeft - subtreeLevels_;
        const uint32_t childMortonIndex = mortonIndex(
            (x & subtreeRelativeMask) >> levelsLeftAfterChild,
            (y & subtreeRelativeMask) >> levelsLeftAfterChild);
        const std::optional<uint32_t> childIndex =
            childSubtreeIndex(subtreeAccessor, childMortonIndex);
        if (!childIndex || *childIndex >= node->childNodes.size()) {
            return false;
        }

        if (levelsLeftAfterChild == 0) {
            if (node->childNodes[*childIndex]) {
                return false;
            }
            node->childNodes[*childIndex] =
                std::make_unique<TileAvailabilityNode>();
            node->childNodes[*childIndex]->setLoadedSubtree(
                std::move(subtree),
                maximumChildrenSubtrees_);
            return true;
        }

        node = node->childNodes[*childIndex].get();
        nodeLevel += subtreeLevels_;
    }

    return false;
}

TileAvailabilityNode* TileQuadtreeAvailability::addNode(
    uint32_t level,
    uint32_t x,
    uint32_t y,
    TileAvailabilityNode* parentNode) {
    if (!parentNode || level == 0) {
        if (root_) return nullptr;
        root_ = std::make_unique<TileAvailabilityNode>();
        return root_.get();
    }
    if (!parentNode->subtree) return nullptr;

    const std::optional<uint32_t> index =
        findChildNodeIndex(level, x, y, parentNode);
    if (!index || *index >= parentNode->childNodes.size()) {
        return nullptr;
    }
    parentNode->childNodes[*index] =
        std::make_unique<TileAvailabilityNode>();
    return parentNode->childNodes[*index].get();
}

bool TileQuadtreeAvailability::addLoadedSubtree(
    TileAvailabilityNode* node,
    TileAvailabilitySubtree&& subtree) {
    if (!node || node->subtree) return false;
    node->setLoadedSubtree(std::move(subtree), maximumChildrenSubtrees_);
    return true;
}

std::optional<uint32_t> TileQuadtreeAvailability::findChildNodeIndex(
    uint32_t level,
    uint32_t x,
    uint32_t y,
    const TileAvailabilityNode* parentNode) const {
    if (!parentNode || !parentNode->subtree) return std::nullopt;
    if (level % subtreeLevels_ != 0) return std::nullopt;

    const TileAvailabilitySubtree& subtree = *parentNode->subtree;
    TileAvailabilityAccessor subtreeAccessor(subtree.subtreeAvailability, subtree);
    const uint32_t relativeLevel = subtreeLevels_;
    const uint32_t subtreeRelativeMask = lowBitsMask(relativeLevel);
    const uint32_t childMortonIndex =
        mortonIndex(x & subtreeRelativeMask, y & subtreeRelativeMask);
    return childSubtreeIndex(subtreeAccessor, childMortonIndex);
}

TileAvailabilityNode* TileQuadtreeAvailability::findChildNode(
    uint32_t level,
    uint32_t x,
    uint32_t y,
    TileAvailabilityNode* parentNode) const {
    const std::optional<uint32_t> index =
        findChildNodeIndex(level, x, y, parentNode);
    if (!index || !parentNode || *index >= parentNode->childNodes.size()) {
        return nullptr;
    }
    return parentNode->childNodes[*index].get();
}

} // namespace earth_engine
