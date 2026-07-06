#include "SchemeId.h"

#include <mutex>
#include <unordered_set>

namespace earth_engine {

const std::string& SchemeId::intern(const std::string& s) {
    // unordered_set node storage: element addresses are stable across inserts
    // (only invalidated by erasing that element, which never happens here), so
    // returning &*it yields a permanently stable canonical pointer.
    static std::mutex mutex;
    static std::unordered_set<std::string> table;
    std::lock_guard<std::mutex> lock(mutex);
    return *table.insert(s).first;
}

const std::string& SchemeId::emptyString() {
    return intern(std::string());
}

} // namespace earth_engine
