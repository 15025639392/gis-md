#pragma once

#include "TileSelectionShadowRunner.h"

#include <condition_variable>
#include <mutex>
#include <thread>

namespace earth_engine {

/// Dedicated selection worker thread (NOT the AsyncSystem decode pool — keeping
/// it separate avoids starving content decoding). It runs the shadow-tree
/// selection off the render thread.
///
/// Step 4 (this file) uses a BARRIER dispatch: runSync() hands a job to the
/// worker and blocks until it finishes, then the caller reads the result. This
/// is semantically identical to the synchronous path (golden stays 8/8) but the
/// snapshot-in / result-out data genuinely crosses a thread boundary, so TSAN
/// validates the handoff before the barrier is removed in step 5 (true async +
/// tilePlan double-buffering).
///
/// The mutex/condition-variable handoff establishes happens-before both ways:
/// render's job publish → worker's job read, and worker's result publish →
/// render's result read. While the worker runs, the render thread is blocked in
/// runSync (barrier), so the job's referenced live state is not concurrently
/// mutated. That non-overlap invariant is what step 5 replaces with an explicit
/// value snapshot + double buffer.
class TileSelectionWorker {
public:
    TileSelectionWorker();
    ~TileSelectionWorker();

    TileSelectionWorker(const TileSelectionWorker&) = delete;
    TileSelectionWorker& operator=(const TileSelectionWorker&) = delete;

    /// Run one selection job on the worker thread and block until it completes.
    /// Returns the runner holding the result (tilePlan/loadQueue/counters +
    /// shadow tree for state write-back). The reference stays valid until the
    /// next runSync().
    const TileSelectionShadowRunner& runSync(
        const TileSelectionShadowRunInput& input);

private:
    void threadMain();

    TileSelectionShadowRunner runner_;

    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    const TileSelectionShadowRunInput* pendingInput_ = nullptr;
    bool jobReady_ = false;
    bool resultReady_ = false;
    bool stop_ = false;
};

} // namespace earth_engine
