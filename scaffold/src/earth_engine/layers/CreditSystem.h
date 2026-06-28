#pragma once

#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace earth_engine {

/// How multiple credits with same HTML are handled in snapshots.
enum class CreditFilteringMode : uint8_t {
    None = 0,
    UniqueHtmlAndShowOnScreen = 1,
    UniqueHtml = 2
};

/// Lightweight credit handle (value type, copyable).
struct Credit {
    uint32_t id = 0;
    uint32_t generation = 0;

    bool operator==(const Credit& rhs) const noexcept {
        return id == rhs.id && generation == rhs.generation;
    }
    bool operator!=(const Credit& rhs) const noexcept {
        return !(*this == rhs);
    }
};

/// Snapshot of active + newly-removed credits.
struct CreditsSnapshot {
    std::vector<Credit> currentCredits;
    std::vector<Credit> removedCredits;
};

/// Copyright attribution system.
/// Manages credit HTML strings with reference counting and frame-diff snapshots.
/// Aligned with cesium-native CesiumUtility::CreditSystem.
class CreditSystem final {
public:
    CreditSystem() = default;
    ~CreditSystem() = default;
    CreditSystem(const CreditSystem&) = delete;
    CreditSystem& operator=(const CreditSystem&) = delete;

    /// Create or reuse a credit for the given HTML.
    /// @return A handle to the credit.
    Credit createCredit(const std::string& html, bool showOnScreen = false);

    /// Get whether this credit should be shown on screen.
    bool shouldBeShownOnScreen(Credit credit) const noexcept;

    /// Set whether this credit should be shown on screen.
    void setShowOnScreen(Credit credit, bool showOnScreen) noexcept;

    /// Get the HTML string for this credit.
    const std::string& getHtml(Credit credit) const noexcept;

    /// Add a reference — credit stays active while refcount > 0.
    /// @return true if the credit was valid.
    bool addCreditReference(Credit credit);

    /// Remove a reference — credit becomes inactive when refcount reaches 0.
    /// @return true if the credit was valid.
    bool removeCreditReference(Credit credit);

    /// Snapshot of current active credits and newly-removed ones.
    /// Valid until the next call to this method.
    const CreditsSnapshot& getSnapshot(
        CreditFilteringMode mode = CreditFilteringMode::UniqueHtml) noexcept;

private:
    static constexpr uint32_t kInvalidIndex =
        std::numeric_limits<uint32_t>::max();

    struct Record {
        std::string html;
        bool showOnScreen = false;
        int32_t referenceCount = 0;
        bool shownLastSnapshot = false;
        uint32_t generation = 0;
    };

    uint32_t allocateIndex();
    bool isValid(uint32_t idx) const noexcept;

    std::vector<Record> records_;
    std::vector<size_t> freeIndices_;
    CreditsSnapshot snapshot_;
    uint32_t nextId_ = 0;
};

} // namespace earth_engine
