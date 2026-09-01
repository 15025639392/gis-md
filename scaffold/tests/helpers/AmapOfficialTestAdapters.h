#pragma once

#include "earth_engine/data/AmapGeometry.h"
#include "earth_engine/data/AmapVectorSource.h"
#include "earth_engine/data/AmapVectorTile.h"
#include "earth_engine/data/Feature.h"
#include "earth_engine/data/MvtVectorSource.h"

#include <iterator>

namespace earth_engine::testing {

struct AmapRegionsToFeaturesForTest {
    std::vector<Feature> operator()(
        const TileKey&, std::shared_ptr<const AmapDecodedTile> tile,
        const std::vector<std::string>&,
        const std::vector<SourceLayerRule>&) const {
        return amapRegionsToFeaturesForContractTest(std::move(tile));
    }
};

struct AmapMainToFeaturesForTest {
    std::vector<Feature> operator()(
        const TileKey&, std::shared_ptr<const AmapDecodedTile> tile,
        const std::vector<std::string>&,
        const std::vector<SourceLayerRule>&) const {
        return amapMainToFeaturesForContractTest(std::move(tile));
    }
};

struct AmapPoiToFeaturesForTest {
    std::vector<Feature> operator()(
        const TileKey&, std::shared_ptr<const AmapDecodedTile> tile,
        const std::vector<std::string>&,
        const std::vector<SourceLayerRule>&) const {
        return amapPoiToFeaturesForContractTest(std::move(tile));
    }
};

using AmapType1TileCacheForTest =
    MvtTileFetchCacheT<AmapDecodedTile, AmapDecodedTileDecodeTraits>;
using AmapRegionsVectorSourceForTest = VectorTileSourceT<
    AmapDecodedTile, AmapDecodedTileDecodeTraits,
    AmapRegionsToFeaturesForTest>;
using AmapMainVectorSourceForTest = VectorTileSourceT<
    AmapDecodedTile, AmapDecodedTileDecodeTraits,
    AmapMainToFeaturesForTest>;

} // namespace earth_engine::testing
