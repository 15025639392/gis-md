#include "TileSelectionResetPolicy.h"

namespace earth_engine {

TileSelectionResetPlan TileSelectionResetPolicy::plan(
    const TileSelectionResetInput& input) {
    TileSelectionResetPlan plan;
    plan.previousSelectionState = input.currentSelectionState;
    plan.surfaceDrawable = input.surfaceDrawable;
    plan.completeRenderable = input.completeRenderable;
    plan.renderable = input.completeRenderable;
    return plan;
}

} // namespace earth_engine
