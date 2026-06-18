#include "TileUnloadQueue.h"

namespace earth_engine {

bool TileUnloadQueue::contains(const std::string& key) const {
    return map_.count(key) != 0;
}

void TileUnloadQueue::pushBackIfAbsent(const std::string& key) {
    if (contains(key)) {
        return;
    }
    queue_.push_back(key);
    map_[key] = --queue_.end();
}

void TileUnloadQueue::erase(const std::string& key) {
    const auto it = map_.find(key);
    if (it == map_.end()) {
        return;
    }
    queue_.erase(it->second);
    map_.erase(it);
}

void TileUnloadQueue::popFront() {
    if (queue_.empty()) {
        return;
    }
    map_.erase(queue_.front());
    queue_.pop_front();
}

void TileUnloadQueue::moveFrontToBack() {
    if (queue_.empty()) {
        return;
    }
    const std::string key = queue_.front();
    queue_.pop_front();
    queue_.push_back(key);
    map_[key] = --queue_.end();
}

const std::string& TileUnloadQueue::front() const {
    return queue_.front();
}

bool TileUnloadQueue::empty() const {
    return queue_.empty();
}

size_t TileUnloadQueue::size() const {
    return queue_.size();
}

std::vector<std::string> TileUnloadQueue::keys() const {
    return {queue_.begin(), queue_.end()};
}

} // namespace earth_engine
