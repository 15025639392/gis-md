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

} // namespace earth_engine
