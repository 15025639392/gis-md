#pragma once

#include <cstddef>
#include <functional>
#include <string>

namespace earth_engine {

// Interned tile-scheme identifier.
//
// Stores a STABLE pointer into a process-wide intern table, so copy / compare /
// hash are O(1) pointer ops instead of std::string alloc / strcmp / string-hash.
// Tile schemes are few (usually 1–2 per run) and interned once at tile creation;
// the hot paths (TileLoadQueue::queue, traversal, plan append) then copy TileKeys
// — i.e. copy this pointer — for free every frame.
//
// Implicitly constructs from std::string / const char* and converts back to
// const std::string&, so existing `TileKey{"scheme", z, x, y}` construction and
// `key.schemeId` reads compile unchanged.
class SchemeId {
public:
    SchemeId() : ptr_(&intern(emptyString())) {}
    SchemeId(const std::string& s) : ptr_(&intern(s)) {}
    SchemeId(const char* s) : ptr_(&intern(std::string(s))) {}

    const std::string& str() const { return *ptr_; }
    operator const std::string&() const { return *ptr_; }

    bool operator==(const SchemeId& other) const { return ptr_ == other.ptr_; }
    bool operator!=(const SchemeId& other) const { return ptr_ != other.ptr_; }
    // Ordered by scheme NAME (not pointer) so sorts stay deterministic across
    // runs — required by golden diff. Rare path (only explicit key sorts).
    bool operator<(const SchemeId& other) const { return *ptr_ < *other.ptr_; }

    const std::string* handle() const { return ptr_; }

private:
    // Returns a reference with a stable address for the canonical copy of `s`,
    // shared by all SchemeId equal to it. Thread-safe.
    static const std::string& intern(const std::string& s);
    static const std::string& emptyString();

    const std::string* ptr_;
};

} // namespace earth_engine

template <>
struct std::hash<earth_engine::SchemeId> {
    std::size_t operator()(const earth_engine::SchemeId& id) const noexcept {
        return std::hash<const void*>()(id.handle());
    }
};
