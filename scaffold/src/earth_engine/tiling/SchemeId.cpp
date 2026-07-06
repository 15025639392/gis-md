#include "SchemeId.h"

#include <memory>
#include <mutex>
#include <vector>

namespace earth_engine {

const std::string& SchemeId::intern(const std::string& s) {
    // Schemes are few (1–2 per run) but intern() is called on the hot path
    // (every TileKey built from / compared against a std::string scheme). A
    // small vector with a linear scan costs ONE string compare in the steady
    // state — cheaper than an unordered_set's per-call string hash + table
    // probe, and without thread_local (which on Android routes through the
    // costly __emutls_get_address). unique_ptr pointees have stable addresses
    // across vector reallocation, so the returned reference is permanent.
    static std::mutex mutex;
    static std::vector<std::unique_ptr<std::string>> table;
    std::lock_guard<std::mutex> lock(mutex);
    for (const std::unique_ptr<std::string>& entry : table) {
        if (*entry == s) {
            return *entry;
        }
    }
    table.push_back(std::make_unique<std::string>(s));
    return *table.back();
}

const std::string& SchemeId::emptyString() {
    return intern(std::string());
}

} // namespace earth_engine
