#pragma once

#include "../core/math/Rectangle.h"
#include "TerrainEdgeNeighborHeight.h"
#include "TerrainEdgeLutTable.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <memory>
#include <vector>

namespace earth_engine {

struct TilePlan;
struct TileRenderEntry;
struct RenderCommand;
class TileScheme;

/// Read-only snapshot of the terrain surface emitted for a candidate frame.
///
/// Unlike TerrainHeightService, this sampler does not choose the deepest
/// retained DEM in the tileset registry. It consumes the resolved
/// TileRenderEntry set, so direct tiles, clipped ancestor fallback, the
/// selected displacement grid, geomorph, relief fade, and boundary edge
/// snap/LUT match the terrain commands that will be submitted when the frame
/// is presentable. A held frame submits neither this terrain nor its vectors;
/// therefore this is deliberately not a second, cross-frame
/// "last-presented" query path.
class RenderedTerrainSurfaceSampler {
public:
    using AreaSampleFn =
        std::function<std::optional<float>(double, double)>;

    struct Stats {
        std::size_t commandIndexEntries = 0;
        std::size_t commandLookups = 0;
        std::size_t lutCopyBytes = 0;
        std::size_t areaSamplerBuilds = 0;
        std::size_t areaCandidateCopies = 0;
    };

    RenderedTerrainSurfaceSampler(const TilePlan& plan,
                                  const TileScheme& scheme);
    RenderedTerrainSurfaceSampler(
        const std::vector<TileRenderEntry>& submittedEntries,
        const TileScheme& scheme,
        const std::vector<RenderCommand>* emittedCommands = nullptr);

    /// Builds a small candidate list for one vector bucket. The returned
    /// closure owns that list and is safe after this sampler is destroyed;
    /// heightmap storage remains owned by the terrain tileset for the frame.
    AreaSampleFn makeAreaSampler(const Rectangle& area) const;

    std::optional<float> sample(double longitudeRadians,
                                double latitudeRadians) const;

    /// Stable while the resolved visible surface is unchanged. Includes
    /// heightmap generation, entry identity, grid, morph, fade and fallback
    /// state so vector reclamping observes changes that registry generation
    /// alone cannot express.
    std::uint64_t revision() const { return revision_; }

    std::size_t candidateCount() const { return candidates_.size(); }
    const Stats& stats() const { return stats_; }

private:
    struct Candidate {
        Rectangle coverage;
        terrain_edge::HeightSource source;
        int selectedZoom = -1;
        bool selectedThisFrame = false;
        float opacity = 0.0f;
        float edgeSnapPacked = 0.0f;
        std::shared_ptr<const TerrainEdgeLutTable> edgeLut;
        float clipMode = 0.0f;
        std::array<float, 4> clipUv{0.0f, 0.0f, 1.0f, 1.0f};
    };

    static std::optional<float> sampleCandidates(
        const std::vector<Candidate>& candidates,
        double longitudeRadians,
        double latitudeRadians);

    std::vector<Candidate> candidates_;
    std::uint64_t revision_ = 0;
    mutable Stats stats_;
};

} // namespace earth_engine
