#pragma once

#include "TileLoadTypes.h"

#include <algorithm>
#include <cstddef>
#include <unordered_map>
#include <vector>

namespace earth_engine {

class TileLoadQueue {
public:
    void queue(const TileKey& key,
               TileLoadPriorityGroup group,
               double priority);
    void erase(const TileKey& key);
    template <typename Predicate>
    void eraseIf(Predicate predicate) {
        const size_t previousSize = requests_.size();
        requests_.erase(
            std::remove_if(requests_.begin(), requests_.end(), predicate),
            requests_.end());
        if (requests_.size() != previousSize) {
            rebuildIndex();
        }
    }
    void clear();
    void resize(size_t size);
    std::vector<TileLoadRequest> takeRequests();
    void mergeRequests(std::vector<TileLoadRequest> requests);

    bool empty() const;
    size_t size() const;
    const TileLoadRequest& front() const;

    const std::vector<TileLoadRequest>& requests() const;
    std::vector<TileLoadRequest>::const_iterator begin() const;
    std::vector<TileLoadRequest>::const_iterator end() const;

private:
    void rebuildIndex();

    std::vector<TileLoadRequest> requests_;
    std::unordered_map<TileKey, size_t> requestIndices_;
};

} // namespace earth_engine
