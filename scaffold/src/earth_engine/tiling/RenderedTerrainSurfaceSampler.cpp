#include "RenderedTerrainSurfaceSampler.h"

#include "../renderer/RenderCommand.h"
#include "TerrainHeightService.h"
#include "TerrainEdgeHeightLut.h"
#include "TilePlan.h"
#include "TileScheme.h"
#include "TilesetTile.h"

#include <algorithm>
#include <cstring>
#include <functional>
#include <tuple>
#include <cmath>
#include <unordered_map>

namespace earth_engine {

namespace {

constexpr float kVisibleOpacity = 0.01f;

struct CommandIdentity {
    const void* selectedTile = nullptr;
    const void* renderTile = nullptr;
    int selectedZ = -1;
    int selectedX = 0;
    int selectedY = 0;
    int renderZ = -1;
    int renderX = 0;
    int renderY = 0;
    bool selectedPass = false;

    bool operator==(const CommandIdentity& other) const {
        return selectedTile == other.selectedTile &&
               renderTile == other.renderTile &&
               selectedZ == other.selectedZ &&
               selectedX == other.selectedX &&
               selectedY == other.selectedY &&
               renderZ == other.renderZ &&
               renderX == other.renderX &&
               renderY == other.renderY &&
               selectedPass == other.selectedPass;
    }
};

struct CommandIdentityHash {
    std::size_t operator()(const CommandIdentity& identity) const {
        const auto a = std::hash<const void*>()(identity.selectedTile);
        const auto b = std::hash<const void*>()(identity.renderTile);
        std::size_t hash = a ^
            (b + 0x9e3779b9u + (a << 6) + (a >> 2));
        const auto append = [&](std::size_t value) {
            hash ^= value + 0x9e3779b9u + (hash << 6) + (hash >> 2);
        };
        append(std::hash<int>()(identity.selectedZ));
        append(std::hash<int>()(identity.selectedX));
        append(std::hash<int>()(identity.selectedY));
        append(std::hash<int>()(identity.renderZ));
        append(std::hash<int>()(identity.renderX));
        append(std::hash<int>()(identity.renderY));
        append(std::hash<bool>()(identity.selectedPass));
        return hash;
    }
};

void hashWord(std::uint64_t& hash, std::uint64_t word) {
    constexpr std::uint64_t kFnvPrime = 1099511628211ull;
    for (int byte = 0; byte < 8; ++byte) {
        hash ^= (word >> (byte * 8)) & 0xffu;
        hash *= kFnvPrime;
    }
}

void hashKey(std::uint64_t& hash, const TileKey& key) {
    hashWord(hash, std::hash<SchemeId>()(key.schemeId));
    hashWord(hash, static_cast<std::uint32_t>(key.x));
    hashWord(hash, static_cast<std::uint32_t>(key.y));
    hashWord(hash, static_cast<std::uint32_t>(key.z));
}

void hashEdgeLut(std::uint64_t& hash, const TerrainEdgeLutTable& lut) {
    hashWord(hash, static_cast<std::uint32_t>(lut.gridSize));
    for (int edge = 0; edge < TerrainEdgeLutTable::kEdges; ++edge) {
        const int count = std::clamp(
            lut.nodeCount[edge], 0, TerrainEdgeLutTable::kMaxNodes);
        hashWord(hash, static_cast<std::uint32_t>(count));
        for (int node = 0; node < count; ++node) {
            hashWord(hash,
                     TerrainDisplacementTemplatePool::encodeEdgeLutDelta(
                         lut.delta[edge][node]));
        }
    }
}

std::uint32_t floatBits(float value) {
    std::uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

int edgeLog2(float packed, int edge) {
    const int bits = static_cast<int>(packed) & 0x0fff;
    return (bits >> (edge * 3)) & 0x7;
}

float edgeSnappedHeight(
    const terrain_edge::HeightSource& source,
    float packed,
    const TerrainEdgeLutTable* lut,
    double longitudeRadians,
    double latitudeRadians) {
    if (packed <= 0.5f || source.gridSize <= 0) {
        return terrain_edge::renderedHeight(
            source, longitudeRadians, latitudeRadians);
    }
    const Rectangle& geometryBounds = *source.bounds;
    const double u =
        (longitudeRadians - geometryBounds.west()) / geometryBounds.width();
    const double v =
        (geometryBounds.north() - latitudeRadians) / geometryBounds.height();
    const double epsilon = 0.5 / static_cast<double>(source.gridSize);
    int edge = -1;
    if (u < epsilon) edge = 0;
    else if (u > 1.0 - epsilon) edge = 1;
    else if (v < epsilon) edge = 2;
    else if (v > 1.0 - epsilon) edge = 3;
    if (edge < 0) {
        return terrain_edge::renderedHeight(
            source, longitudeRadians, latitudeRadians);
    }
    const int log2Step = edgeLog2(packed, edge);
    if (log2Step <= 0) {
        return terrain_edge::renderedHeight(
            source, longitudeRadians, latitudeRadians);
    }

    const float along = static_cast<float>(
        (edge < 2 ? v : u) * source.gridSize);
    int j0 = 0;
    int j1 = 0;
    float fraction = 0.0f;
    TerrainEdgeHeightLut::shaderNodePair(
        along, log2Step, source.gridSize, j0, j1, fraction);
    const double step = static_cast<double>(1 << log2Step);
    const double t0 = std::min(
        static_cast<double>(j0) * step / source.gridSize, 1.0);
    const double t1 = std::min(
        static_cast<double>(j1) * step / source.gridSize, 1.0);
    double lon0 = 0.0;
    double lat0 = 0.0;
    double lon1 = 0.0;
    double lat1 = 0.0;
    terrain_edge::edgePoint(geometryBounds, edge, t0, lon0, lat0);
    terrain_edge::edgePoint(geometryBounds, edge, t1, lon1, lat1);
    const auto fineSample = [&](double lon, double lat) {
        if (source.quantizedTexture) {
            return DecodedHeightmapSampler::
                sampleHeightRenderGridQuantizedDecoded(
                       *source.heightmap, *source.bounds, lon, lat,
                       source.gridSize,
                       source.quantizationMinHeight,
                       source.quantizationHeightRange,
                       source.textureMinHeight,
                       source.textureHeightRange);
        }
        return DecodedHeightmapSampler::sampleHeightRenderGrid(
                   *source.heightmap, *source.bounds, lon, lat,
                   source.gridSize) *
               source.fade;
    };
    float h0 = fineSample(lon0, lat0);
    float h1 = fineSample(lon1, lat1);
    if (lut && edge < TerrainEdgeLutTable::kEdges &&
        j0 < lut->nodeCount[edge] && j1 < lut->nodeCount[edge]) {
        const auto gpuDelta = [](float delta) {
            return TerrainDisplacementTemplatePool::decodeEdgeLutDelta(
                TerrainDisplacementTemplatePool::encodeEdgeLutDelta(delta));
        };
        h0 += gpuDelta(lut->delta[edge][j0]);
        h1 += gpuDelta(lut->delta[edge][j1]);
    }
    return h0 + (h1 - h0) * fraction;
}

std::pair<double, double> samplePosition(
    const Rectangle& coverage,
    const terrain_edge::HeightSource& source,
    float clipMode,
    const std::array<float, 4>& clipUv,
    double longitudeRadians,
    double latitudeRadians) {
    if (clipMode <= 1.5f) {
        return {longitudeRadians, latitudeRadians};
    }
    const double u =
        (longitudeRadians - coverage.west()) / coverage.width();
    const double v =
        (coverage.north() - latitudeRadians) / coverage.height();
    const double sourceU = clipUv[0] + u * clipUv[2];
    const double sourceV = clipUv[1] + v * clipUv[3];
    return {
        source.bounds->west() + sourceU * source.bounds->width(),
        source.bounds->north() - sourceV * source.bounds->height()};
}

} // namespace

RenderedTerrainSurfaceSampler::RenderedTerrainSurfaceSampler(
    const TilePlan& plan, const TileScheme& scheme)
    : RenderedTerrainSurfaceSampler(plan.renderEntries, scheme, nullptr) {}

RenderedTerrainSurfaceSampler::RenderedTerrainSurfaceSampler(
    const std::vector<TileRenderEntry>& submittedEntries,
    const TileScheme& scheme,
    const std::vector<RenderCommand>* emittedCommands) {
    candidates_.reserve(submittedEntries.size());
    revision_ = 1469598103934665603ull;
    hashWord(revision_, TerrainHeightService::heightmapGeneration());

    std::unordered_map<CommandIdentity, const RenderCommand*,
                       CommandIdentityHash>
        commandIndex;
    if (emittedCommands) {
        commandIndex.reserve(emittedCommands->size());
        for (const RenderCommand& command : *emittedCommands) {
            if (!command.terrainVisibleSurfaceValid) continue;
            commandIndex.emplace(
                CommandIdentity{command.terrainVisibleSelectedTileIdentity,
                                command.terrainVisibleRenderTileIdentity,
                                command.terrainVisibleSelectedZ,
                                command.terrainVisibleSelectedX,
                                command.terrainVisibleSelectedY,
                                command.terrainVisibleRenderZ,
                                command.terrainVisibleRenderX,
                                command.terrainVisibleRenderY,
                                command.terrainVisibleSelectedPass},
                &command);
        }
        stats_.commandIndexEntries = commandIndex.size();
    }

    for (const TileRenderEntry& entry : submittedEntries) {
        if (entry.opacity <= kVisibleOpacity || !entry.selectedTile) {
            continue;
        }
        terrain_edge::HeightSource source =
            terrain_edge::sourceOf(entry);
        if (!source.valid()) {
            continue;
        }
        float edgeSnapPacked = 0.0f;
        std::shared_ptr<const TerrainEdgeLutTable> edgeLut;
        float clipMode = 0.0f;
        std::array<float, 4> clipUv{0.0f, 0.0f, 1.0f, 1.0f};
        if (emittedCommands) {
            ++stats_.commandLookups;
            const auto found = commandIndex.find(CommandIdentity{
                entry.selectedTile, entry.renderTile,
                entry.selectedKey.z, entry.selectedKey.x,
                entry.selectedKey.y, entry.renderKey.z,
                entry.renderKey.x, entry.renderKey.y,
                entry.selectedThisFrame});
            if (found == commandIndex.end()) {
                // Planned entry emitted no terrain command, therefore it is
                // not part of this frame's submitted surface.
                continue;
            }
            const RenderCommand& command = *found->second;
            source.gridSize = command.terrainVisibleGridSize;
            source.morph = command.terrainVisibleMorph;
            source.fade = command.terrainVisibleFade;
            source.quantizedTexture = command.hasTerrainDisplacementFrame;
            source.webMercatorHeightV =
                !command.hasTerrainDisplacementFrame;
            if (source.quantizedTexture) {
                // Preserve the exact float32 values consumed by the shader.
                // These are already faded; divide-then-remultiply changes
                // rounding and can move the sampled surface by several ULPs.
                source.textureMinHeight =
                    command.gltfUniforms.heightDisplace[0];
                source.textureHeightRange =
                    command.gltfUniforms.heightDisplace[1];
                source.quantizationMinHeight =
                    source.heightmap->minHeight;
                source.quantizationHeightRange =
                    source.heightmap->maxHeight - source.heightmap->minHeight;
            }
            clipMode = command.terrainVisibleClipMode;
            clipUv = command.terrainVisibleClipUv;
            // Only the displacement terrain shader implements edge snap.
            // Keep the sampler fail-closed if a malformed/non-final command
            // carries stale packed bits from tile selection state.
            edgeSnapPacked = command.hasTerrainDisplacementFrame
                                 ? command.terrainVisibleEdgeSnapPacked
                                 : 0.0f;
            if (edgeSnapPacked > 0.5f &&
                command.terrainVisibleEdgeLutTable) {
                edgeLut = std::make_shared<const TerrainEdgeLutTable>(
                    *command.terrainVisibleEdgeLutTable);
                stats_.lutCopyBytes += sizeof(TerrainEdgeLutTable);
                hashEdgeLut(revision_, *edgeLut);
            }
        }
        candidates_.push_back(Candidate{
            // Composition may split one current entry into descendant-key
            // fragments while retaining its source tile pointer. selectedKey
            // is the submitted footprint; selectedTile->bounds would claim
            // the replaced region as well.
            scheme.tileToRectangle(entry.selectedKey),
            source,
            entry.selectedKey.z,
            entry.selectedThisFrame,
            entry.opacity,
            edgeSnapPacked,
            std::move(edgeLut),
            clipMode,
            clipUv});

        hashKey(revision_, entry.selectedKey);
        hashKey(revision_, entry.renderKey);
        hashWord(revision_, entry.selectedThisFrame ? 1u : 0u);
        hashWord(revision_, entry.usesAncestorFallback ? 1u : 0u);
        hashWord(revision_, static_cast<std::uint32_t>(source.gridSize));
        hashWord(revision_, floatBits(source.morph));
        hashWord(revision_, floatBits(source.fade));
        hashWord(revision_, source.webMercatorHeightV ? 1u : 0u);
        hashWord(revision_, floatBits(entry.opacity));
        hashWord(revision_, floatBits(edgeSnapPacked));
        hashWord(revision_, floatBits(clipMode));
        for (float component : clipUv) {
            hashWord(revision_, floatBits(component));
        }
    }
}

std::optional<float> RenderedTerrainSurfaceSampler::sampleCandidates(
    const std::vector<Candidate>& candidates,
    double longitudeRadians,
    double latitudeRadians) {
    const Candidate* best = nullptr;
    float bestHeight = 0.0f;
    for (const Candidate& candidate : candidates) {
        if (!candidate.coverage.contains(longitudeRadians,
                                         latitudeRadians)) {
            continue;
        }
        const auto [sampleLongitude, sampleLatitude] = samplePosition(
            candidate.coverage, candidate.source,
            candidate.clipMode, candidate.clipUv,
            longitudeRadians, latitudeRadians);
        const float height = edgeSnappedHeight(
            candidate.source, candidate.edgeSnapPacked,
            candidate.edgeLut.get(),
            sampleLongitude, sampleLatitude);
        if (!best) {
            best = &candidate;
            bestHeight = height;
            continue;
        }

        // The selected pass owns the current surface contract. Within the
        // same pass, the more refined selected footprint wins. Exact ties
        // retain the higher radial surface, matching depth-visible terrain at
        // shared closed bounds.
        const auto rank = [](const Candidate& c) {
            return std::tuple(c.selectedThisFrame, c.selectedZoom, c.opacity);
        };
        if (rank(candidate) > rank(*best) ||
            (rank(candidate) == rank(*best) && height > bestHeight)) {
            best = &candidate;
            bestHeight = height;
        }
    }
    return best ? std::optional<float>(bestHeight) : std::nullopt;
}

RenderedTerrainSurfaceSampler::AreaSampleFn
RenderedTerrainSurfaceSampler::makeAreaSampler(const Rectangle& area) const {
    ++stats_.areaSamplerBuilds;
    std::vector<Candidate> local;
    local.reserve(candidates_.size());
    for (const Candidate& candidate : candidates_) {
        if (candidate.coverage.intersects(area)) {
            local.push_back(candidate);
            ++stats_.areaCandidateCopies;
        }
    }
    if (local.empty()) {
        return {};
    }
    return [candidates = std::move(local)](double longitudeRadians,
                                           double latitudeRadians) {
        return sampleCandidates(candidates, longitudeRadians,
                                latitudeRadians);
    };
}

std::uint64_t RenderedTerrainSurfaceSampler::areaRevision(
    const Rectangle& area) const {
    std::uint64_t hash = 1469598103934665603ull;
    for (const Candidate& candidate : candidates_) {
        if (!candidate.coverage.intersects(area)) continue;
        // Mirrors the per-candidate fields folded into the global `revision_`,
        // so a bucket's area revision changes exactly when the terrain it
        // samples changes (data generation, grid, morph, fade, edge snap,
        // clip window).
        hashWord(hash, candidate.source.heightmap != nullptr ? 1u : 0u);
        hashWord(hash, static_cast<std::uint32_t>(candidate.source.gridSize));
        hashWord(hash, floatBits(candidate.source.morph));
        hashWord(hash, floatBits(candidate.source.fade));
        hashWord(hash, floatBits(candidate.edgeSnapPacked));
        hashWord(hash, floatBits(candidate.clipMode));
        for (float component : candidate.clipUv) {
            hashWord(hash, floatBits(component));
        }
        hashWord(hash, static_cast<std::uint32_t>(candidate.selectedZoom));
        hashWord(hash, candidate.selectedThisFrame ? 1u : 0u);
        hashWord(hash, floatBits(candidate.opacity));
    }
    return hash;
}

std::optional<float> RenderedTerrainSurfaceSampler::sample(
    double longitudeRadians,
    double latitudeRadians) const {
    return sampleCandidates(candidates_, longitudeRadians, latitudeRadians);
}

} // namespace earth_engine
