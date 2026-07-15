#include "TileLoadQueue.h"

#include "TileLoadPriorityPolicy.h"

#include <algorithm>

namespace earth_engine {

void TileLoadQueue::queue(const TileKey& key,
                          TileLoadPriorityGroup group,
                          double priority) {
    const auto indexed = requestIndices_.find(key);
    if (indexed != requestIndices_.end()) {
        TileLoadRequest& request = requests_[indexed->second];
        if (TileLoadPriorityPolicy::hasHigherPriority(
                group,
                priority,
                request.group,
                request.priority)) {
            request.group = group;
            request.priority = priority;
        }
        return;
    }
    requestIndices_.emplace(key, requests_.size());
    requests_.push_back(TileLoadRequest{key, group, priority});
}

void TileLoadQueue::erase(const TileKey& key) {
    const auto indexed = requestIndices_.find(key);
    if (indexed == requestIndices_.end()) {
        return;
    }
    requests_.erase(
        requests_.begin() +
        static_cast<std::ptrdiff_t>(indexed->second));
    rebuildIndex();
}

void TileLoadQueue::clear() {
    requests_.clear();
    requestIndices_.clear();
}

void TileLoadQueue::resize(size_t size) {
    if (size >= requests_.size()) {
        return;
    }
    requests_.resize(size);
    rebuildIndex();
}

std::vector<TileLoadRequest> TileLoadQueue::takeRequests() {
    std::vector<TileLoadRequest> result;
    result.swap(requests_);
    requestIndices_.clear();
    return result;
}

void TileLoadQueue::mergeRequests(std::vector<TileLoadRequest> requests) {
    for (const TileLoadRequest& request : requests) {
        queue(request.key, request.group, request.priority);
    }
}

bool TileLoadQueue::empty() const {
    return requests_.empty();
}

size_t TileLoadQueue::size() const {
    return requests_.size();
}

const TileLoadRequest& TileLoadQueue::front() const {
    return requests_.front();
}

const std::vector<TileLoadRequest>& TileLoadQueue::requests() const {
    return requests_;
}

std::vector<TileLoadRequest>::const_iterator TileLoadQueue::begin() const {
    return requests_.begin();
}

std::vector<TileLoadRequest>::const_iterator TileLoadQueue::end() const {
    return requests_.end();
}

void TileLoadQueue::rebuildIndex() {
    requestIndices_.clear();
    requestIndices_.reserve(requests_.size());
    for (size_t i = 0; i < requests_.size(); ++i) {
        requestIndices_[requests_[i].key] = i;
    }
}

} // namespace earth_engine
