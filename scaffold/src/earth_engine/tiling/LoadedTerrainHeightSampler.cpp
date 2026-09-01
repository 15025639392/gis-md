#include "LoadedTerrainHeightSampler.h"

#include "DecodedHeightmapSampler.h"
#include "TilesetTile.h"

#include "../providers/TerrainProvider.h"

#include <algorithm>
#include <optional>
#include <vector>

namespace earth_engine {

namespace {

struct LoadedTerrainSample {
    float height = 0.0f;
    int zoom = -1;
};

void commitBestSample(std::optional<LoadedTerrainSample>& bestSample,
                      LoadedTerrainSample candidate) {
    if (!bestSample ||
        candidate.zoom > bestSample->zoom ||
        (candidate.zoom == bestSample->zoom &&
         candidate.height > bestSample->height)) {
        bestSample = candidate;
    }
}

// Scan zoom-descending candidates for the deepest one whose retained heightmap
// covers (lon, lat). Candidates must already be sorted zoom-desc; the
// point-in-bounds test is applied here so an area-prefiltered candidate list
// can be reused across many points.
//
// Height comes from each tile's retained DecodedHeightmap (the CPU-side source
// the GPU displacement path samples), NOT the rendered glTF mesh: under GPU
// vertex displacement the per-tile mesh is a flat shared template, so the
// heightmap is the single source of truth for both the displaced geometry on
// screen and height queries here. A covering candidate without a retained
// heightmap (e.g. an upsampled child, or a fill-proxy) is skipped so the scan
// falls back to a coarser ancestor that has one — preserving the ancestor
// fallback the glTF-mesh path used to provide.
std::optional<float> sampleFromSortedCandidates(
    const std::vector<const TilesetTile*>& candidates,
    double longitudeRadians,
    double latitudeRadians,
    bool renderGridConsistent = false) {
    std::optional<LoadedTerrainSample> bestSample;
    for (const TilesetTile* tile : candidates) {
        if (bestSample && tile->key.z < bestSample->zoom) {
            break;
        }
        if (!tile->bounds.contains(longitudeRadians, latitudeRadians)) {
            continue;
        }
        const TileRenderContentState& renderContent =
            tile->content.renderContent;
        if (!renderContent.isTerrainRenderContent()) {
            continue;
        }
        const DecodedHeightmap* heightmap = renderContent.retainedHeightmap().get();
        if (!heightmap || !heightmap->valid()) {
            continue;
        }
        // 自适应几何密度:该瓦片本帧实际渲染档位是常驻 draw 命令上的真值,
        // 不能按 SSE 重算(迟滞使"当前 SSE 推出的档"未必等于"正在渲染的档")。
        int renderGridSize = 0;
        for (const RenderCommand& cmd : renderContent.cachedDrawCommands()) {
            if (cmd.terrainHeightGridSize > 0) {
                renderGridSize = cmd.terrainHeightGridSize;
                break;
            }
        }
        const float height =
            renderGridConsistent
                ? DecodedHeightmapSampler::sampleHeightRenderGrid(
                      *heightmap, tile->bounds,
                      longitudeRadians, latitudeRadians, renderGridSize)
                : DecodedHeightmapSampler::sampleHeight(
                      *heightmap, tile->bounds,
                      longitudeRadians, latitudeRadians);
        commitBestSample(
            bestSample,
            LoadedTerrainSample{height, tile->key.z});
    }
    if (bestSample) {
        return bestSample->height;
    }
    return std::nullopt;
}

} // namespace

namespace {

bool hasUsableHeightmap(const TilesetTile& tile) {
    const TileRenderContentState& renderContent = tile.content.renderContent;
    if (!renderContent.isTerrainRenderContent()) {
        return false;
    }
    const DecodedHeightmap* heightmap = renderContent.retainedHeightmap().get();
    return heightmap && heightmap->valid();
}

} // namespace

const TilesetTile* TerrainAncestorHeightSource::find(const TilesetTile& tile) {
    for (const TilesetTile* ancestor = tile.parent;
         ancestor;
         ancestor = ancestor->parent) {
        if (hasUsableHeightmap(*ancestor)) {
            return ancestor;
        }
    }
    return nullptr;
}

std::optional<float> TerrainAncestorHeightSource::sample(
    const TilesetTile& source,
    double longitudeRadians,
    double latitudeRadians) {
    if (!hasUsableHeightmap(source) ||
        !source.bounds.contains(longitudeRadians, latitudeRadians)) {
        return std::nullopt;
    }
    return DecodedHeightmapSampler::sampleHeight(
        *source.content.renderContent.retainedHeightmap(),
        source.bounds,
        longitudeRadians,
        latitudeRadians);
}

LoadedTerrainAreaSampler::LoadedTerrainAreaSampler(
    const std::unordered_map<std::string, std::unique_ptr<TilesetTile>>& tiles,
    const Rectangle& area,
    bool renderGridConsistent)
    : renderGridConsistent_(renderGridConsistent) {
    for (const auto& [cacheKey, tile] : tiles) {
        (void)cacheKey;
        if (tile && tile->content.renderContent.isTerrainRenderContent() &&
            tile->content.renderContent.hasRetainedHeightmap() &&
            tile->bounds.intersects(area)) {
            candidates_.push_back(tile.get());
        }
    }
    std::sort(
        candidates_.begin(),
        candidates_.end(),
        [](const TilesetTile* a, const TilesetTile* b) {
            return a->key.z > b->key.z;
        });
}

std::optional<float> LoadedTerrainAreaSampler::sample(
    double longitudeRadians,
    double latitudeRadians) const {
    return sampleFromSortedCandidates(
        candidates_,
        longitudeRadians,
        latitudeRadians,
        renderGridConsistent_);
}

std::optional<float> LoadedTerrainHeightSampler::sampleHeightOptional(
    const std::unordered_map<
        std::string,
        std::unique_ptr<TilesetTile>>& tiles,
        double longitudeRadians,
        double latitudeRadians) {
    // Gather the candidate tiles first (a point lies inside one tile per
    // zoom level plus its ancestors) and scan them deepest-first so a hit at
    // the finest level lets every coarser candidate be skipped.
    std::vector<const TilesetTile*> candidates;
    for (const auto& [cacheKey, tile] : tiles) {
        (void)cacheKey;
        if (tile && tile->bounds.contains(longitudeRadians, latitudeRadians)) {
            candidates.push_back(tile.get());
        }
    }
    std::sort(
        candidates.begin(),
        candidates.end(),
        [](const TilesetTile* a, const TilesetTile* b) {
            return a->key.z > b->key.z;
        });
    return sampleFromSortedCandidates(
        candidates,
        longitudeRadians,
        latitudeRadians);
}

} // namespace earth_engine
