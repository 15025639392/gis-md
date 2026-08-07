#include "TileSelectionRenderEntryPolicy.h"

namespace earth_engine {

TileSelectionRenderEntryPlan TileSelectionRenderEntryPolicy::plan(
    const TileSelectionRenderEntryInput& input) {
    TileSelectionRenderEntryPlan plan;
    plan.queueNormalLoad = input.queueForLoad;
    return plan;
}

} // namespace earth_engine
