#pragma once

namespace earth_engine {

/// Unified tile content load state, matching cesium-native TileLoadResult's
/// success-vs-terminal split while preserving this engine's external content
/// and cancellation states.
enum class TileLoadStatus {
    Renderable,
    Empty,
    External,
    RetryLater,
    Failed,
    Cancelled
};

inline constexpr bool isSuccessfulTileLoadStatus(TileLoadStatus status) {
    switch (status) {
        case TileLoadStatus::Renderable:
        case TileLoadStatus::Empty:
        case TileLoadStatus::External:
            return true;
        case TileLoadStatus::RetryLater:
        case TileLoadStatus::Failed:
        case TileLoadStatus::Cancelled:
            return false;
    }
    return false;
}

} // namespace earth_engine
