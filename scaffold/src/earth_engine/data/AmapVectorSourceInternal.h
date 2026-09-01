#pragma once

#include "AmapVectorSource.h"
#include "AmapGeometry.h"
#include "MvtVectorSource.h"

namespace earth_engine {

struct AmapClassicSourceBundle::Impl {
    struct DecodedTile {
        std::vector<AmapDecodedLayerPart> parts;
    };

    static size_t approxBytes(const DecodedTile& tile);
    static bool decodeType1(const uint8_t* data, size_t size,
                            DecodedTile& out, std::string* error);
    static bool decodePoi(const uint8_t* data, size_t size,
                          DecodedTile& out, std::string* error);
    static std::vector<Feature> convertPart(
        const AmapDecodedLayerPart& part, bool toWgs84,
        bool (*lineIdentityFilter)(int, int) = nullptr,
        bool (*polygonIdentityFilter)(int, int) = nullptr,
        bool (*pointIdentityFilter)(int, int) = nullptr);
    static std::vector<Feature> convertRegions(
        const std::vector<AmapDecodedLayerPart>& parts);
    static std::vector<Feature> convertMain(
        const std::vector<AmapDecodedLayerPart>& parts);
    static std::vector<Feature> convertPoi(
        const std::vector<AmapDecodedLayerPart>& parts);
    static void appendPart(const AmapDecodedLayerPart& part,
                           std::vector<Feature>& out);

    struct Type1Traits {
        static bool decode(const uint8_t* data, size_t size,
                           DecodedTile& out, std::string* error) {
            if (size == 1 && data && data[0] == 0) {
                out.parts.clear();
                return true;
            }
            return decodeType1(data, size, out, error);
        }
        static size_t approxBytes(const DecodedTile& tile) {
            return Impl::approxBytes(tile);
        }
    };

    struct PoiTraits {
        static bool decode(const uint8_t* data, size_t size,
                           DecodedTile& out, std::string* error) {
            return decodePoi(data, size, out, error);
        }
        static size_t approxBytes(const DecodedTile& tile) {
            return Impl::approxBytes(tile);
        }
    };

    struct RegionsToFeatures;
    struct MainToFeatures;
    struct PoiToFeatures;

    using Type1Cache = MvtTileFetchCacheT<DecodedTile, Type1Traits>;
    using PoiCache = MvtTileFetchCacheT<DecodedTile, PoiTraits>;
    using RegionsSource = VectorTileSourceT<
        DecodedTile, Type1Traits, RegionsToFeatures>;
    using MainSource = VectorTileSourceT<
        DecodedTile, Type1Traits, MainToFeatures>;
    using PoiSource = VectorTileSourceT<
        DecodedTile, PoiTraits, PoiToFeatures>;

    std::shared_ptr<Type1Cache> type1Cache;
    std::shared_ptr<PoiCache> poiCache;
    std::unique_ptr<RegionsSource> regionsSource;
    std::unique_ptr<MainSource> mainSource;
    std::unique_ptr<PoiSource> poiSource;
};

} // namespace earth_engine
