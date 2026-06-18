#include "TileLoadQueue.h"

#include "TileLoadPriorityPolicy.h"

#include <algorithm>

namespace earth_engine {

void TileLoadQueue::queue(const TileKey& key,
                          TileLoadPriorityGroup group,
                          double priority) {
    auto it = std::find_if(requests_.begin(), requests_.end(),
        [&key](const TileLoadRequest& request) {
            return request.key == key;
        });
    if (it != requests_.end()) {
        if (TileLoadPriorityPolicy::hasHigherPriority(
                group,
                priority,
                it->group,
                it->priority)) {
            it->group = group;
            it->priority = priority;
        }
        return;
    }
    requests_.push_back(TileLoadRequest{key, group, priority});
}

void TileLoadQueue::erase(const TileKey& key) {
    requests_.erase(
        std::remove_if(
            requests_.begin(),
            requests_.end(),
            [&key](const TileLoadRequest& request) {
                return request.key == key;
            }),
        requests_.end());
}

void TileLoadQueue::clear() {
    requests_.clear();
}

void TileLoadQueue::resize(size_t size) {
    requests_.resize(size);
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

} // namespace earth_engine
