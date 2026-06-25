#pragma once

#include "TileLoadDomainPolicy.h"
#include "TileLoadTypes.h"

namespace earth_engine {

struct TileAvailabilityUpdateCommitter {
    static void applyTerrainAvailabilityUpdates(
        TileLoadDomain domain,
        TileLoadResult& result,
        TilesetContentProvider* contentProvider) {
        if (!contentProvider || !contentProvider->providesTerrainQuadtree()) {
            return;
        }

        TileAvailabilityUpdateSelection selection =
            TileLoadDomainPolicy::availabilityUpdatesForDomain(domain, result);
        if (selection.updates && !selection.updates->empty()) {
            contentProvider->applyTerrainAvailabilityUpdates(*selection.updates);
            if (selection.clearAfterApply) {
                selection.updates->clear();
            }
        }
    }
};

} // namespace earth_engine
