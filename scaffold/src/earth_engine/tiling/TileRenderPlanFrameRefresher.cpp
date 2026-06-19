#include "TileRenderPlanFrameRefresher.h"

#include "TileCacheKey.h"
#include "TileContentAccess.h"
#include "TileRenderPlanFinalizer.h"

namespace earth_engine {

namespace {

constexpr int kActiveInteractionRenderPrepBudget = 0;
constexpr int kRecoveryRenderPrepBudget = 1;

} // namespace

void TileRenderPlanFrameRefresher::refresh(
    TilePlan& tilePlan,
    TileContentAccess& contentAccess,
    const TileRenderPlanFrameRefreshOptions& options) {
    TileRenderPlanFinalizer::refreshRenderEntries(
        tilePlan,
        TileRenderPlanFinalizeOptions{
            options.enableLodTransitionPeriod,
            options.interactionActive,
            kActiveInteractionRenderPrepBudget,
            kRecoveryRenderPrepBudget},
        [&contentAccess](const TileKey& key) {
            return contentAccess.ensureTile(key);
        },
        [](const TileKey& key) {
            return TileCacheKey::forTile(key);
        });
}

} // namespace earth_engine
