#include "TerrainHeightService.h"

#include "DecodedHeightmapSampler.h"
#include "TileRenderContentState.h"
#include "TileScheme.h"
#include "TilesetTile.h"
#include "TilesetTileRegistry.h"

#include "../debug/Policies.h"
#include "../providers/TerrainProvider.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace earth_engine {

namespace {

std::uint64_t packCell(int x, int y) {
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) << 32) |
           static_cast<std::uint32_t>(y);
}

// bounds 与 scheme 矩形的一致性判定。二者理应由同一套 scheme 数学产出、
// 位级相等;给 1e-12 rad(亚微米)容差只为吸收潜在的构造路径差异,任何
// 真实的 bounds 毒化(半个瓦片起步)远超此阈。
bool boundsMatchScheme(const TilesetTile& tile, const TileScheme& scheme) {
    const Rectangle expected = scheme.tileToRectangle(tile.key);
    constexpr double kEps = 1e-12;
    return std::abs(tile.bounds.west() - expected.west()) < kEps &&
           std::abs(tile.bounds.south() - expected.south()) < kEps &&
           std::abs(tile.bounds.east() - expected.east()) < kEps &&
           std::abs(tile.bounds.north() - expected.north()) < kEps;
}

// 与旧 LoadedTerrainHeightSampler 相同的查询期验真:索引/候选只是提示,
// 资格在此裁决,索引过期条目零危害。
bool usableTerrainTile(const TilesetTile& tile,
                       double longitudeRadians,
                       double latitudeRadians) {
    if (!tile.bounds.contains(longitudeRadians, latitudeRadians)) {
        return false;
    }
    const TileRenderContentState& renderContent = tile.content.renderContent;
    if (!renderContent.isTerrainRenderContent()) {
        return false;
    }
    const DecodedHeightmap* heightmap = renderContent.retainedHeightmap().get();
    return heightmap && heightmap->valid();
}

float sampleTile(const TilesetTile& tile,
                 double longitudeRadians,
                 double latitudeRadians,
                 TerrainHeightService::Interp interp) {
    const TileRenderContentState& renderContent = tile.content.renderContent;
    const DecodedHeightmap& heightmap = *renderContent.retainedHeightmap();
    if (interp == TerrainHeightService::Interp::RenderGridConsistent) {
        // 本帧实际渲染档位是常驻 draw 命令上的真值,不能按 SSE 重算
        // (迟滞使"当前 SSE 推出的档"未必等于"正在渲染的档")。
        int renderGridSize = 0;
        for (const RenderCommand& cmd : renderContent.cachedDrawCommands()) {
            if (cmd.terrainHeightGridSize > 0) {
                renderGridSize = cmd.terrainHeightGridSize;
                break;
            }
        }
        return DecodedHeightmapSampler::sampleHeightRenderGrid(
            heightmap, tile.bounds,
            longitudeRadians, latitudeRadians, renderGridSize);
    }
    return DecodedHeightmapSampler::sampleHeight(
        heightmap, tile.bounds, longitudeRadians, latitudeRadians);
}

// 旧 commitBestSample 的语义原样保留:更深赢;同深(过渡重叠缝)取更高面,
// 与 cesium-native 高度采样一致。
void commitBestSample(std::optional<TerrainHeightService::Sample>& best,
                      TerrainHeightService::Sample candidate) {
    if (!best ||
        candidate.zoom > best->zoom ||
        (candidate.zoom == best->zoom && candidate.height > best->height)) {
        best = candidate;
    }
}

} // namespace

TerrainHeightService::TerrainHeightService(const TilesetTileRegistry& registry,
                                           const TileScheme& scheme)
    : registry_(&registry),
      scheme_(&scheme) {}

std::uint64_t TerrainHeightService::heightmapGeneration() {
    return TileRenderContentState::heightmapGeneration();
}

TerrainHeightService::SampleStats TerrainHeightService::takeSampleStats()
    const {
    SampleStats out = sampleStats_;
    sampleStats_ = {};
    return out;
}

std::size_t TerrainHeightService::irregularCount() const {
    std::size_t count = 0;
    for (const auto& [zoom, tiles] : irregularByZoom_) {
        (void)zoom;
        count += tiles.size();
    }
    return count;
}

std::size_t TerrainHeightService::indexedCount() const {
    refreshIfStale();
    std::size_t count = 0;
    for (const ZoomLevel& level : levels_) {
        count += level.cells.size();
    }
    return count + irregularCount();
}

void TerrainHeightService::refreshIfStale() const {
    const std::uint64_t generation =
        TileRenderContentState::heightmapGeneration();
    if (generation == builtGeneration_) {
        return;
    }
    rebuild();
    builtGeneration_ = generation;
}

void TerrainHeightService::rebuild() const {
    levels_.clear();
    populatedZoomsDesc_.clear();
    irregularByZoom_.clear();
    ++rebuildCount_;

    for (const auto& [cacheKey, tile] : registry_->tiles()) {
        (void)cacheKey;
        if (!tile) {
            continue;
        }
        const TileRenderContentState& renderContent =
            tile->content.renderContent;
        if (!renderContent.isTerrainRenderContent() ||
            !renderContent.hasRetainedHeightmap()) {
            continue;
        }
        const int z = tile->key.z;
        if (z < 0) {
            continue;
        }
        if (!boundsMatchScheme(*tile, *scheme_)) {
            irregularByZoom_[z].push_back(tile.get());
            continue;
        }
        if (static_cast<std::size_t>(z) >= levels_.size()) {
            levels_.resize(static_cast<std::size_t>(z) + 1);
        }
        levels_[static_cast<std::size_t>(z)]
            .cells[packCell(tile->key.x, tile->key.y)] = tile.get();
    }

    // 策略层记账:正规率应精确 1.0(区间依据见 Policies.cpp),溢出瓦把点查询
    // 悄悄退化回线性扫——正确性无损所以契约层抓不到,只有比率层能看见。
    std::size_t regular = 0;
    for (const ZoomLevel& level : levels_) {
        regular += level.cells.size();
    }
    const std::size_t indexed = regular + irregularCount();
    policy::observe(policy::Id::HeightIndexRegularity,
                    static_cast<int>(regular),
                    static_cast<int>(indexed));

    for (int z = static_cast<int>(levels_.size()) - 1; z >= 0; --z) {
        if (!levels_[static_cast<std::size_t>(z)].cells.empty() ||
            irregularByZoom_.count(z) > 0) {
            populatedZoomsDesc_.push_back(z);
        }
    }
    for (const auto& [z, tiles] : irregularByZoom_) {
        (void)tiles;
        if (static_cast<std::size_t>(z) >= levels_.size() &&
            std::find(populatedZoomsDesc_.begin(), populatedZoomsDesc_.end(),
                      z) == populatedZoomsDesc_.end()) {
            populatedZoomsDesc_.push_back(z);
        }
    }
    std::sort(populatedZoomsDesc_.begin(), populatedZoomsDesc_.end(),
              [](int a, int b) { return a > b; });
}

std::optional<std::pair<double, double>>
TerrainHeightService::heightRangeForArea(const Rectangle& area) const {
    refreshIfStale();

    // 枚举上限:太深的档一块块数过去不划算,而且那种档必然不是"整块覆盖"
    // 的那一档(area 比 cell 大几个数量级)。跳过即可,下一档更粗更可能覆盖。
    constexpr int kMaxCellsPerZoom = 4096;

    for (const int z : populatedZoomsDesc_) {
        if (z < 0 || static_cast<std::size_t>(z) >= levels_.size()) {
            continue;
        }
        const ZoomLevel& level = levels_[static_cast<std::size_t>(z)];
        if (level.cells.empty()) {
            continue;
        }

        // 两个对角点定出 cell 下标区间,但要取**内部**点:cell 是半开区间,
        // 东/北边界属于下一块。area 恰好等于某块瓦片矩形是常态(调用方传的
        // 就是包围体矩形),用边界点会平白多要一圈邻居,于是"整块覆盖"永远
        // 不成立 —— 判据看起来在工作,实则恒为 nullopt。
        const double insetX =
            std::min(1e-9, (area.east() - area.west()) * 0.25);
        const double insetY =
            std::min(1e-9, (area.north() - area.south()) * 0.25);
        // y 方向随 scheme 约定可能与纬度反向,故取 min/max 而不是假设某一端更小。
        const TileKey a = scheme_->positionToTile(area.west() + insetX,
                                                 area.south() + insetY, z);
        const TileKey b = scheme_->positionToTile(area.east() - insetX,
                                                 area.north() - insetY, z);
        const int x0 = std::min(a.x, b.x);
        const int x1 = std::max(a.x, b.x);
        const int y0 = std::min(a.y, b.y);
        const int y1 = std::max(a.y, b.y);
        const long long cells =
            static_cast<long long>(x1 - x0 + 1) * (y1 - y0 + 1);
        if (cells <= 0 || cells > kMaxCellsPerZoom) {
            continue;
        }

        double minHeight = std::numeric_limits<double>::max();
        double maxHeight = std::numeric_limits<double>::lowest();
        bool fullyCovered = true;
        for (int x = x0; x <= x1 && fullyCovered; ++x) {
            for (int y = y0; y <= y1; ++y) {
                const auto it = level.cells.find(packCell(x, y));
                if (it == level.cells.end() || !it->second) {
                    fullyCovered = false;
                    break;
                }
                // 索引只保证"有 heightmap";实测区间是另一条写入路径
                // (TileLoadResultMetadataApplicator)。缺了就当这档没覆盖,
                // 不要拿一对默认 0 冒充实测值 —— 0/0 的体等于贴地整片消失。
                const TileRenderContentState& renderContent =
                    it->second->content.renderContent;
                if (!renderContent.hasTerrainHeightRange()) {
                    fullyCovered = false;
                    break;
                }
                minHeight =
                    std::min(minHeight, renderContent.terrainMinimumHeight());
                maxHeight =
                    std::max(maxHeight, renderContent.terrainMaximumHeight());
            }
        }
        if (fullyCovered && maxHeight >= minHeight) {
            return std::make_pair(minHeight, maxHeight);
        }
    }
    return std::nullopt;
}

std::optional<TerrainHeightService::Sample> TerrainHeightService::sample(
    double longitudeRadians,
    double latitudeRadians,
    Interp interp) const {
    refreshIfStale();

    std::optional<Sample> best;

    for (const int z : populatedZoomsDesc_) {
        // 与旧排序扫描的 break 语义一致:更深档已有答案后,只把该档扫完
        // (同深 tie-break),不再下探更浅档。
        if (best && z < best->zoom) {
            break;
        }

        const ZoomLevel* level =
            static_cast<std::size_t>(z) < levels_.size()
                ? &levels_[static_cast<std::size_t>(z)]
                : nullptr;

        if (level && !level->cells.empty()) {
            const TileKey centerKey =
                scheme_->positionToTile(longitudeRadians, latitudeRadians, z);
            const int countX = scheme_->tileCountX(z);

            // bounds.contains 对四边都是闭区间:严格内点只属一个 cell,
            // 落在 cell 边界上的点可能同时被相邻 cell 覆盖(同深重叠缝的
            // tie-break 依赖这一点)。先探中心 cell,仅当点不在其严格内部
            // (miss 或贴边)才探 3x3 邻域——x 轴按 tileCount 回绕,y 越界
            // 由 contains 验真自然挡掉。
            bool strictlyInterior = false;
            auto centerIt =
                level->cells.find(packCell(centerKey.x, centerKey.y));
            if (centerIt != level->cells.end()) {
                const TilesetTile& tile = *centerIt->second;
                if (usableTerrainTile(tile, longitudeRadians,
                                      latitudeRadians)) {
                    commitBestSample(
                        best,
                        Sample{sampleTile(tile, longitudeRadians,
                                          latitudeRadians, interp),
                               z});
                }
                strictlyInterior =
                    longitudeRadians > tile.bounds.west() &&
                    longitudeRadians < tile.bounds.east() &&
                    latitudeRadians > tile.bounds.south() &&
                    latitudeRadians < tile.bounds.north();
            }

            if (!strictlyInterior) {
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        if (dx == 0 && dy == 0) {
                            continue;
                        }
                        int nx = centerKey.x + dx;
                        if (countX > 0) {
                            nx = ((nx % countX) + countX) % countX;
                        }
                        const int ny = centerKey.y + dy;
                        auto it = level->cells.find(packCell(nx, ny));
                        if (it == level->cells.end()) {
                            continue;
                        }
                        const TilesetTile& tile = *it->second;
                        if (!usableTerrainTile(tile, longitudeRadians,
                                               latitudeRadians)) {
                            continue;
                        }
                        commitBestSample(
                            best,
                            Sample{sampleTile(tile, longitudeRadians,
                                              latitudeRadians, interp),
                                   z});
                    }
                }
            }
        }

        auto irregularIt = irregularByZoom_.find(z);
        if (irregularIt != irregularByZoom_.end()) {
            for (const TilesetTile* tile : irregularIt->second) {
                if (!usableTerrainTile(*tile, longitudeRadians,
                                       latitudeRadians)) {
                    continue;
                }
                commitBestSample(
                    best,
                    Sample{sampleTile(*tile, longitudeRadians, latitudeRadians,
                                      interp),
                           z});
            }
        }
    }

    if (best) {
        ++sampleStats_.hits;
        const std::size_t bucket = std::min<std::size_t>(
            static_cast<std::size_t>(best->zoom < 0 ? 0 : best->zoom),
            sampleStats_.zoomHits.size() - 1);
        ++sampleStats_.zoomHits[bucket];
    } else {
        ++sampleStats_.misses;
    }

    return best;
}

} // namespace earth_engine
