#include "CreditSystem.h"

#include <algorithm>
#include <unordered_set>

namespace earth_engine {

static const std::string kInvalidCreditMsg =
    "Error: Invalid Credit, cannot get HTML string.";

uint32_t CreditSystem::allocateIndex() {
    if (!freeIndices_.empty()) {
        uint32_t idx = static_cast<uint32_t>(freeIndices_.back());
        freeIndices_.pop_back();
        return idx;
    }
    uint32_t idx = static_cast<uint32_t>(records_.size());
    records_.emplace_back();
    return idx;
}

bool CreditSystem::isValid(uint32_t idx) const noexcept {
    return idx < records_.size() &&
           records_[idx].generation != std::numeric_limits<uint32_t>::max();
}

Credit CreditSystem::createCredit(const std::string& html, bool showOnScreen) {
    // Dedup: check if same HTML already exists
    for (uint32_t i = 0; i < static_cast<uint32_t>(records_.size()); ++i) {
        if (!isValid(i)) continue;
        if (records_[i].html == html && records_[i].showOnScreen == showOnScreen) {
            return Credit{i, records_[i].generation};
        }
    }

    uint32_t idx = allocateIndex();
    Record& rec = records_[idx];
    rec.html = html;
    rec.showOnScreen = showOnScreen;
    rec.referenceCount = 0;
    rec.shownLastSnapshot = false;
    rec.generation = (rec.generation == std::numeric_limits<uint32_t>::max())
        ? 1 : rec.generation + 1;
    ++nextId_;
    return Credit{idx, rec.generation};
}

bool CreditSystem::shouldBeShownOnScreen(Credit credit) const noexcept {
    if (!isValid(credit.id)) return false;
    return records_[credit.id].showOnScreen;
}

void CreditSystem::setShowOnScreen(Credit credit, bool showOnScreen) noexcept {
    if (!isValid(credit.id)) return;
    records_[credit.id].showOnScreen = showOnScreen;
}

const std::string& CreditSystem::getHtml(Credit credit) const noexcept {
    if (!isValid(credit.id)) return kInvalidCreditMsg;
    return records_[credit.id].html;
}

bool CreditSystem::addCreditReference(Credit credit) {
    if (!isValid(credit.id) || records_[credit.id].generation != credit.generation) {
        return false;
    }
    Record& rec = records_[credit.id];
    if (rec.referenceCount < std::numeric_limits<int32_t>::max()) {
        ++rec.referenceCount;
    }
    return true;
}

bool CreditSystem::removeCreditReference(Credit credit) {
    if (!isValid(credit.id) || records_[credit.id].generation != credit.generation) {
        return false;
    }
    Record& rec = records_[credit.id];
    if (rec.referenceCount > 0) {
        --rec.referenceCount;
    }
    return true;
}

const CreditsSnapshot& CreditSystem::getSnapshot(
    CreditFilteringMode mode) noexcept {
    std::vector<Credit> newCurrent;

    for (uint32_t i = 0; i < static_cast<uint32_t>(records_.size()); ++i) {
        if (!isValid(i)) continue;
        const Record& rec = records_[i];
        if (rec.referenceCount <= 0) continue;

        Credit credit{i, rec.generation};

        // Filtering
        if (mode == CreditFilteringMode::UniqueHtmlAndShowOnScreen) {
            bool dup = false;
            for (const auto& existing : newCurrent) {
                if (records_[existing.id].html == rec.html &&
                    records_[existing.id].showOnScreen == rec.showOnScreen) {
                    dup = true;
                    break;
                }
            }
            if (dup) continue;
        } else if (mode == CreditFilteringMode::UniqueHtml) {
            bool dup = false;
            for (const auto& existing : newCurrent) {
                if (records_[existing.id].html == rec.html) {
                    dup = true;
                    break;
                }
            }
            if (dup) continue;
        }

        newCurrent.push_back(credit);
    }

    // Compute removed credits
    std::vector<Credit> removed;
    for (const auto& prev : snapshot_.currentCredits) {
        if (!isValid(prev.id)) {
            removed.push_back(prev);
            continue;
        }
        bool stillActive = false;
        for (const auto& cur : newCurrent) {
            if (cur.id == prev.id) {
                stillActive = true;
                break;
            }
        }
        if (!stillActive) {
            removed.push_back(prev);
        }
    }

    snapshot_.currentCredits = std::move(newCurrent);
    snapshot_.removedCredits = std::move(removed);
    return snapshot_;
}

} // namespace earth_engine
