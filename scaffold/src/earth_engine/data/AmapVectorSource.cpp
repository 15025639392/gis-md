#include "AmapVectorSource.h"
#include "AmapVectorSourceInternal.h"
#include "AmapTileManifest.h"
#include "AmapGeometry.h"
#include "MvtVectorSource.h"
#include "../Engine.h"
#include "../core/async/AsyncSystem.h"
#include "../core/geodesy/Ellipsoid.h"
#include "../layers/FeatureRenderLayer.h"
#include "../style/AmapClassicStyleInternal.h"
#include "../tiling/TileScheme.h"

#include <cmath>
#include <array>
#include <functional>
#include <iterator>
#include <mutex>
#include <stdexcept>

namespace earth_engine {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kDegToRad = kPi / 180.0;

bool isOrdinaryAmapSurfaceIdentity(int classCode, int subKey) {
    return classCode != 55001 &&
           isAmapClassicSurfaceIdentity(classCode, subKey);
}

} // namespace

void AmapClassicSourceBundle::Impl::appendPart(
    const AmapDecodedLayerPart& part, std::vector<Feature>& out) {
    auto features = convertPart(
        part, false, isAmapClassicTransportIdentity,
        isAmapClassicSurfaceIdentity, nullptr);
    out.insert(out.end(), std::make_move_iterator(features.begin()),
               std::make_move_iterator(features.end()));
}

size_t AmapClassicSourceBundle::Impl::approxBytes(const DecodedTile& tile) {
    constexpr size_t kVectorHeaderBytes = sizeof(std::vector<int>);
    size_t bytes = sizeof(DecodedTile) +
                   tile.parts.capacity() * sizeof(AmapDecodedLayerPart);
    for (const AmapDecodedLayerPart& part : tile.parts) {
        bytes += part.features.capacity() * sizeof(AmapDecodedFeature);
        for (const AmapDecodedFeature& feature : part.features) {
            bytes += feature.name.capacity();
            bytes += feature.rings.capacity() * kVectorHeaderBytes;
            for (const auto& ring : feature.rings) {
                bytes += ring.capacity() * sizeof(std::pair<double, double>);
            }
        }
    }
    return bytes;
}

struct AmapClassicSourceBundle::Impl::RegionsToFeatures {
    std::vector<Feature> operator()(
        const TileKey&, std::shared_ptr<const DecodedTile> tile,
        const std::vector<std::string>&,
        const std::vector<SourceLayerRule>&) const {
        return convertRegions(tile->parts);
    }
};

std::vector<Feature> AmapClassicSourceBundle::Impl::convertRegions(
    const std::vector<AmapDecodedLayerPart>& parts) {
    std::vector<Feature> out;
    for (const auto& part : parts) {
        if (part.type != 2) continue;
        auto features = convertPart(
            part, false, isAmapClassicTransportIdentity,
            isAmapClassicSurfaceIdentity, nullptr);
        out.insert(out.end(), std::make_move_iterator(features.begin()),
                   std::make_move_iterator(features.end()));
    }
    return out;
}

struct AmapClassicSourceBundle::Impl::MainToFeatures {
    std::vector<Feature> operator()(
        const TileKey&, std::shared_ptr<const DecodedTile> tile,
        const std::vector<std::string>&,
        const std::vector<SourceLayerRule>&) const {
        return convertMain(tile->parts);
    }
};

std::vector<Feature> AmapClassicSourceBundle::Impl::convertMain(
    const std::vector<AmapDecodedLayerPart>& parts) {
    std::vector<Feature> out;
    for (const auto& part : parts) {
        if (part.type < 1 || part.type > 4) continue;
        if (part.type == 2 && part.z < 12) continue;
        appendPart(part, out);
    }
    return out;
}

struct AmapClassicSourceBundle::Impl::PoiToFeatures {
    std::vector<Feature> operator()(
        const TileKey&, std::shared_ptr<const DecodedTile> tile,
        const std::vector<std::string>&,
        const std::vector<SourceLayerRule>&) const {
        return convertPoi(tile->parts);
    }
};

std::vector<Feature> AmapClassicSourceBundle::Impl::convertPoi(
    const std::vector<AmapDecodedLayerPart>& parts) {
    std::vector<Feature> out;
    for (const auto& part : parts) {
        if (part.type != 0 && part.type != 1 && part.type != 4) continue;
        auto features = convertPart(
            part, false, isAmapClassicRoadLabelIdentity, nullptr,
            isAmapClassicPoiIdentity);
        out.insert(out.end(), std::make_move_iterator(features.begin()),
                   std::make_move_iterator(features.end()));
    }
    return out;
}

Rectangle amapTileRectangle(const TileKey& key) {
    const double n = std::exp2(key.z);
    const double westDeg = static_cast<double>(key.x) / n * 360.0 - 180.0;
    const double eastDeg = static_cast<double>(key.x + 1) / n * 360.0 - 180.0;
    const double northDeg = 90.0 - static_cast<double>(key.y) / n * 180.0;
    const double southDeg =
        90.0 - static_cast<double>(key.y + 1) / n * 180.0;
    return Rectangle(westDeg * kDegToRad, southDeg * kDegToRad,
                     eastDeg * kDegToRad, northDeg * kDegToRad);
}

#if defined(EARTH_ENGINE_TESTING)
std::vector<Feature> amapRegionsToFeaturesForContractTest(
    std::shared_ptr<const AmapDecodedTile> tile) {
    return AmapClassicSourceBundle::Impl::convertRegions(tile->parts);
}

std::vector<Feature> amapMainToFeaturesForContractTest(
    std::shared_ptr<const AmapDecodedTile> tile) {
    return AmapClassicSourceBundle::Impl::convertMain(tile->parts);
}

std::vector<Feature> amapPoiToFeaturesForContractTest(
    std::shared_ptr<const AmapDecodedTile> tile) {
    return AmapClassicSourceBundle::Impl::convertPoi(tile->parts);
}
#endif

AmapClassicSourceBundle::AmapClassicSourceBundle(
    Engine& engine, RenderDevice& renderDevice, Type1Fetch type1Fetch,
    PoiFetch poiFetch,
    std::shared_ptr<ThreadPool> type1DecodePool,
    std::shared_ptr<ThreadPool> poiDecodePool,
    std::shared_ptr<ThreadPool> tessellationPool, Options options)
    : engine_(engine), impl_(std::make_unique<Impl>()) {
    impl_->type1Cache = std::make_shared<Impl::Type1Cache>(
        std::move(type1Fetch), options.decodedCacheTiles,
        options.rawCacheTiles, std::move(type1DecodePool));
    impl_->poiCache = std::make_shared<Impl::PoiCache>(
        std::move(poiFetch), options.decodedCacheTiles,
        options.rawCacheTiles, std::move(poiDecodePool));

    const auto rollback = [&]() {
        impl_->poiSource.reset();
        impl_->mainSource.reset();
        impl_->regionsSource.reset();
        if (poiLayer_) engine_.removeOfficialFeatureRenderLayer("amap-poi");
        if (mainLayer_)
            engine_.removeOfficialFeatureRenderLayer("amap-vector");
        if (regionsLayer_)
            engine_.removeOfficialFeatureRenderLayer("amap-regions");
        poiLayer_ = nullptr;
        mainLayer_ = nullptr;
        regionsLayer_ = nullptr;
    };

    const auto addLayer = [&](const char* id,
                              FeatureRenderLayer::AmapClassicProfile profile) {
        auto layer = std::make_unique<FeatureRenderLayer>(
            id, &renderDevice, Ellipsoid::WGS84());
        layer->installAmapClassicProfile(profile);
        FeatureRenderLayer* ptr = layer.get();
        return engine_.addOfficialFeatureRenderLayer(std::move(layer))
                   ? ptr : nullptr;
    };
    try {
        regionsLayer_ = addLayer(
            "amap-regions", FeatureRenderLayer::AmapClassicProfile::Regions);
        mainLayer_ = addLayer(
            "amap-vector", FeatureRenderLayer::AmapClassicProfile::Main);
        poiLayer_ = addLayer(
            "amap-poi", FeatureRenderLayer::AmapClassicProfile::Poi);
        if (!regionsLayer_ || !mainLayer_ || !poiLayer_) {
            throw std::runtime_error("AMap official layer ids already exist");
        }

        const auto configureTree = [&](auto& sourceOptions, int maxZoom) {
            sourceOptions.tree.minZoom = 3;
            sourceOptions.tree.maxZoom = maxZoom;
            sourceOptions.tree.supportedZooms =
                maxZoom == 10 ? std::vector<int>{3, 6, 8, 10}
                              : std::vector<int>{3, 6, 8, 10, 12, 14};
            sourceOptions.tree.dataZoomForCanonicalZoom = [](int z) {
                return amapDataZoom(z);
            };
            sourceOptions.tree.scheme = TileScheme::createAmapGeographic();
            sourceOptions.tree.maxTilesPerView = options.maximumTilesPerView;
            sourceOptions.tree.refinement =
                VectorTileTree::RefinementPolicy::GeometryReplace;
            sourceOptions.maxTessellationsInFlight =
                options.maximumTessellationsInFlight;
        };
        const auto configureSinks = [&](auto& sinks,
                                        FeatureRenderLayer* layer) {
            sinks.tessellate =
                [layer, collectDiagnostics = options.collectDiagnostics](
                    const TileKey& key, std::vector<Feature>&& features) {
                    auto ctx = layer->workerTessellationContextForArea(
                        amapTileRectangle(key));
                    ctx.collectDiagnostics = collectDiagnostics;
                    return FeatureRenderLayer::tessellateTileMesh(
                        ctx, features);
                };
            sinks.commit = [layer](const TileKey& key,
                                   FeatureTileMesh& mesh) {
                return layer->commitTileMesh(key, mesh);
            };
            sinks.drop = [layer](const TileKey& key) {
                layer->dropTileMesh(key);
            };
        };

        Impl::RegionsSource::Options regionsOptions;
        regionsOptions.debugName = "amap-regions";
        configureTree(regionsOptions, 10);
        Impl::RegionsSource::Sinks regionsSinks;
        configureSinks(regionsSinks, regionsLayer_);
        impl_->regionsSource = std::make_unique<Impl::RegionsSource>(
            std::move(regionsOptions), std::move(regionsSinks),
            impl_->type1Cache, tessellationPool);
#if defined(EARTH_ENGINE_TESTING)
        if (options.failAfterSourceConstruction == 1) {
            throw std::runtime_error("injected AMap source construction failure");
        }
#endif

        Impl::MainSource::Options mainOptions;
        mainOptions.debugName = "amap-main";
        configureTree(mainOptions, 14);
        Impl::MainSource::Sinks mainSinks;
        configureSinks(mainSinks, mainLayer_);
        impl_->mainSource = std::make_unique<Impl::MainSource>(
            std::move(mainOptions), std::move(mainSinks), impl_->type1Cache,
            tessellationPool);

        Impl::PoiSource::Options poiOptions;
        poiOptions.debugName = "amap-poi";
        configureTree(poiOptions, 14);
        Impl::PoiSource::Sinks poiSinks;
        configureSinks(poiSinks, poiLayer_);
        impl_->poiSource = std::make_unique<Impl::PoiSource>(
            std::move(poiOptions), std::move(poiSinks), impl_->poiCache,
            std::move(tessellationPool));
    } catch (...) {
        rollback();
        throw;
    }
}

AmapClassicSourceBundle::~AmapClassicSourceBundle() {
    impl_->poiSource.reset();
    impl_->mainSource.reset();
    impl_->regionsSource.reset();
    if (poiLayer_) engine_.removeOfficialFeatureRenderLayer("amap-poi");
    if (mainLayer_) engine_.removeOfficialFeatureRenderLayer("amap-vector");
    if (regionsLayer_)
        engine_.removeOfficialFeatureRenderLayer("amap-regions");
}

void AmapClassicSourceBundle::setOfficialSurfaceFillBaked(bool enabled) {
    if (regionsLayer_) regionsLayer_->setOfficialSurfaceFillBaked(enabled);
    if (mainLayer_) mainLayer_->setOfficialSurfaceFillBaked(enabled);
}

void AmapClassicSourceBundle::update(const Rectangle& viewRectangle,
                                     double cameraHeightMeters,
                                     SceneFrameResourceArbiter& resourceArbiter) {
    const double viewZoom = std::min(
        24.0, std::max(0.0,
                       std::log2(4.0e7 / std::max(1.0, cameraHeightMeters))));
    const bool regionsActive = viewZoom < 12.0;
    if (!regionsActive) {
        impl_->regionsSource->suspend();
    }

    struct SourceUpdate {
        bool active;
        struct Limits {
            uint32_t networkRequests = 0;
            uint32_t workerDispatches = 0;
            uint32_t gpuUploads = 0;
        };
        std::function<void(Limits)> update;
    };
    std::array<SourceUpdate, 3> updates{{
        {regionsActive, [&](SourceUpdate::Limits shared) {
             Impl::RegionsSource::FrameAdmissionLimits limits;
             limits.networkRequests = shared.networkRequests;
             limits.workerDispatches = shared.workerDispatches;
             limits.gpuUploads = shared.gpuUploads;
             impl_->regionsSource->update(viewRectangle, cameraHeightMeters,
                                          &resourceArbiter, limits);
         }},
        {true, [&](SourceUpdate::Limits shared) {
             Impl::MainSource::FrameAdmissionLimits limits;
             limits.networkRequests = shared.networkRequests;
             limits.workerDispatches = shared.workerDispatches;
             limits.gpuUploads = shared.gpuUploads;
             impl_->mainSource->update(viewRectangle, cameraHeightMeters,
                                       &resourceArbiter, limits);
         }},
        {true, [&](SourceUpdate::Limits shared) {
             Impl::PoiSource::FrameAdmissionLimits limits;
             limits.networkRequests = shared.networkRequests;
             limits.workerDispatches = shared.workerDispatches;
             limits.gpuUploads = shared.gpuUploads;
             impl_->poiSource->update(viewRectangle, cameraHeightMeters,
                                      &resourceArbiter, limits);
         }},
    }};
    size_t activeCount = 0;
    for (const auto& update : updates) activeCount += update.active ? 1 : 0;
    updateCursor_ %= updates.size();
    const auto fairShare = [&](SceneFrameResourceStage stage,
                               size_t remainingSources) {
        const uint32_t remaining = resourceArbiter.remaining(
            SceneFrameResourceProducer::Mvt, stage,
            FrameResourcePriority::Normal);
        return remainingSources == 0
                   ? uint32_t{0}
                   : static_cast<uint32_t>(
                         (static_cast<uint64_t>(remaining) +
                          remainingSources - 1) /
                         remainingSources);
    };
    size_t visited = 0;
    for (size_t offset = 0; offset < updates.size(); ++offset) {
        SourceUpdate& update = updates[(updateCursor_ + offset) % updates.size()];
        if (!update.active) continue;
        const size_t remainingSources = activeCount - visited++;
        SourceUpdate::Limits limits;
        limits.networkRequests = fairShare(
            SceneFrameResourceStage::NetworkRequest, remainingSources);
        limits.workerDispatches = fairShare(
            SceneFrameResourceStage::WorkerDispatch, remainingSources);
        limits.gpuUploads = fairShare(
            SceneFrameResourceStage::GpuUpload, remainingSources);
        update.update(limits);
    }
    updateCursor_ = (updateCursor_ + 1) % updates.size();
}

void AmapClassicSourceBundle::requestSurfaceFeatures(
    const TileKey& webMercatorKey, CancellationToken token,
    SurfaceFeaturesCallback callback) const {
    if (!callback) return;
    if (!impl_ || !impl_->type1Cache || token.isCancelled() ||
        (webMercatorKey.schemeId != "XYZ-WebMercator" &&
         webMercatorKey.schemeId != "Geographic-TMS") ||
        webMercatorKey.z < 0 || webMercatorKey.z > 25) {
        callback(nullptr);
        return;
    }

    auto targetScheme = webMercatorKey.schemeId == "Geographic-TMS"
        ? TileScheme::createGeographicTMS()
        : TileScheme::createXYZWebMercator();
    auto amap = TileScheme::createAmapGeographic();
    const Rectangle targetBounds = targetScheme->tileToRectangle(webMercatorKey);
    const int dataZoom = amapDataZoom(webMercatorKey.z);
    int minX = 0;
    int minY = 0;
    int maxX = -1;
    int maxY = -1;
    amap->tileRange(targetBounds, dataZoom, minX, minY, maxX, maxY);
    const int64_t width = static_cast<int64_t>(maxX) - minX + 1;
    const int64_t height = static_cast<int64_t>(maxY) - minY + 1;
    if (width <= 0 || height <= 0 || width * height > 64) {
        callback(nullptr);
        return;
    }

    struct Aggregate {
        std::mutex mutex;
        size_t remaining = 0;
        bool failed = false;
        CancellationToken token;
        SurfaceFeatures features;
        SurfaceFeaturesCallback callback;
    };
    auto aggregate = std::make_shared<Aggregate>();
    aggregate->remaining = static_cast<size_t>(width * height);
    aggregate->token = token;
    aggregate->callback = std::move(callback);

    const auto finishOne = [aggregate](
        std::shared_ptr<const Impl::DecodedTile> tile) {
        SurfaceFeatures converted;
        if (tile && !aggregate->token.isCancelled()) {
            for (const AmapDecodedLayerPart& part : tile->parts) {
                if (part.type != 2 && part.type != 4) continue;
                auto partFeatures = Impl::convertPart(
                    part, true, nullptr, isOrdinaryAmapSurfaceIdentity,
                    nullptr);
                for (Feature& feature : partFeatures) {
                    if (feature.type == GeometryType::Polygon &&
                        feature.properties.count("amap_height") == 0) {
                        converted.push_back(std::move(feature));
                    }
                }
            }
        }

        SurfaceFeaturesCallback done;
        std::shared_ptr<const SurfaceFeatures> result;
        {
            std::lock_guard<std::mutex> lock(aggregate->mutex);
            if (!tile || aggregate->token.isCancelled()) {
                aggregate->failed = true;
            } else if (!converted.empty()) {
                aggregate->features.insert(
                    aggregate->features.end(),
                    std::make_move_iterator(converted.begin()),
                    std::make_move_iterator(converted.end()));
            }
            if (--aggregate->remaining != 0) return;
            done = std::move(aggregate->callback);
            if (!aggregate->failed) {
                result = std::make_shared<const SurfaceFeatures>(
                    std::move(aggregate->features));
            }
        }
        if (done) done(std::move(result));
    };

    for (int y = minY; y <= maxY; ++y) {
        for (int x = minX; x <= maxX; ++x) {
            impl_->type1Cache->request(
                TileKey{"Amap-Geographic", dataZoom, x, y}, finishOne);
        }
    }
}

AmapClassicSourceBundle::CacheStats
AmapClassicSourceBundle::type1CacheStats() const {
    CacheStats out;
    if (!impl_ || !impl_->type1Cache) return out;
    const auto stats = impl_->type1Cache->stats();
    out.hits = stats.hits;
    out.fetches = stats.fetches;
    out.refetches = stats.refetches;
    out.rawHits = stats.rawHits;
    out.residentTiles = stats.residentTiles;
    out.rawTiles = stats.rawTiles;
    out.rawBytes = stats.rawBytes;
    out.residentBytes = stats.residentBytes;
    out.failureSkips = stats.failureSkips;
    return out;
}

namespace {
template <typename Source>
AmapClassicSourceBundle::SourceStats sourceStats(const Source* source) {
    AmapClassicSourceBundle::SourceStats out;
    if (!source) return out;
    const auto& stats = source->lastUpdateStats();
    out.ingestMs = stats.ingestMs;
    out.treeMs = stats.treeMs;
    out.dispatchMs = stats.dispatchMs;
    out.commitMs = stats.commitMs;
    out.commits = stats.commits;
    out.drops = stats.drops;
    out.tessellateDispatched = stats.tessellateDispatched;
    out.selectedZoom = stats.selectedZoom;
    out.desiredTileCount = stats.desiredTileCount;
    out.scannedTileCount = stats.scannedTileCount;
    out.renderTileCount = stats.renderTileCount;
    out.requestTileCount = stats.requestTileCount;
    out.pendingTileCount = stats.pendingTileCount;
    out.tessellatingTileCount = stats.tessellatingTileCount;
    out.readyTileCount = stats.readyTileCount;
    out.activeTileCount = stats.activeTileCount;
    out.activeAncestorPairs = stats.activeAncestorPairs;
    return out;
}

template <typename Source>
bool sourcePending(const Source* source) {
    return source &&
           (source->tree().pendingCount() > 0 ||
            source->hasTessellationInFlight() ||
            source->pendingCommitCount() > 0 ||
            source->tree().hasRetryPending());
}
} // namespace

AmapClassicSourceBundle::SourceStats
AmapClassicSourceBundle::regionsSourceStats() const {
    return sourceStats(impl_ ? impl_->regionsSource.get() : nullptr);
}

AmapClassicSourceBundle::SourceStats
AmapClassicSourceBundle::mainSourceStats() const {
    return sourceStats(impl_ ? impl_->mainSource.get() : nullptr);
}

AmapClassicSourceBundle::SourceStats
AmapClassicSourceBundle::poiSourceStats() const {
    return sourceStats(impl_ ? impl_->poiSource.get() : nullptr);
}

bool AmapClassicSourceBundle::hasPendingWork() const {
    return impl_ &&
           (sourcePending(impl_->regionsSource.get()) ||
            sourcePending(impl_->mainSource.get()) ||
            sourcePending(impl_->poiSource.get()));
}

} // namespace earth_engine
