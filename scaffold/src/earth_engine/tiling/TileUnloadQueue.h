#pragma once

#include <cstddef>
#include <list>
#include <string>
#include <unordered_map>
#include <vector>

namespace earth_engine {

class TileUnloadQueue {
public:
    bool contains(const std::string& key) const;
    void pushBackIfAbsent(const std::string& key);
    void erase(const std::string& key);
    void popFront();
    void moveFrontToBack();
    const std::string& front() const;
    bool empty() const;
    size_t size() const;
    std::vector<std::string> keys() const;

private:
    std::list<std::string> queue_;
    std::unordered_map<std::string, std::list<std::string>::iterator> map_;
};

} // namespace earth_engine
