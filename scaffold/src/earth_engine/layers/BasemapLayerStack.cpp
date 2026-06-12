#include "BasemapLayerStack.h"
#include "../scene/FrameState.h"
#include "../scene/Camera.h"
#include "../renderer/Renderer.h"
#include "../tiling/TilePlan.h"
#include "../tiling/TileScheme.h"
#include "../debug/PerfTimer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <unordered_map>

namespace earth_engine {

// ============================================================
// 图层管理
// ============================================================

void BasemapLayerStack::addLayer(std::unique_ptr<BasemapLayer> layer) {
    if (!layer) return;
    layers_.push_back(std::move(layer));
}

std::unique_ptr<BasemapLayer> BasemapLayerStack::removeLayer(const std::string& layerId) {
    auto it = std::find_if(layers_.begin(), layers_.end(),
        [&](const auto& l) { return l->id() == layerId; });
    if (it == layers_.end()) return nullptr;

    auto removed = std::move(*it);
    layers_.erase(it);
    return removed;
}

void BasemapLayerStack::moveLayer(const std::string& layerId, size_t index) {
    if (index >= layers_.size()) return;

    auto it = std::find_if(layers_.begin(), layers_.end(),
        [&](const auto& l) { return l->id() == layerId; });
    if (it == layers_.end()) return;

    auto layer = std::move(*it);
    layers_.erase(it);

    auto insertPos = layers_.begin() + static_cast<ptrdiff_t>(index);
    layers_.insert(insertPos, std::move(layer));
}

BasemapLayer* BasemapLayerStack::layerAt(size_t index) {
    if (index >= layers_.size()) return nullptr;
    return layers_[index].get();
}

BasemapLayer* BasemapLayerStack::findLayer(const std::string& layerId) {
    auto it = std::find_if(layers_.begin(), layers_.end(),
        [&](const auto& l) { return l->id() == layerId; });
    return (it != layers_.end()) ? it->get() : nullptr;
}

int BasemapLayerStack::visibleTileCount() const {
    int count = 0;
    for (const auto& layer : layers_) {
        if (!layer->visible()) continue;
        count += layer->visibleTileCount();
    }
    return count;
}

int BasemapLayerStack::cachedTileCount() const {
    int count = 0;
    for (const auto& layer : layers_) {
        count += layer->cachedTileCount();
    }
    return count;
}

// ============================================================
// 帧更新
// ============================================================

void BasemapLayerStack::update(const FrameState& frameState) {
    if (!frameState.camera) return;

    const double updateStartMs = perf::nowMs();
    double groupingMs = 0.0;
    double tilePlanMs = 0.0;
    double applyPlanMs = 0.0;
    double loadMissingMs = 0.0;
    int groupCount = 0;
    int layerCount = 0;

    groupPlans_.clear();

    double vpW = static_cast<double>(frameState.viewportWidthPixels);
    double vpH = static_cast<double>(frameState.viewportHeightPixels);

    // 1. 按 scheme ID 分组（避免同 scheme 图层重复计算 TilePlan）
    std::unordered_map<std::string, std::vector<BasemapLayer*>> schemeGroups;
    const double groupingStartMs = perf::nowMs();
    for (auto& layer : layers_) {
        if (!layer->visible()) continue;
        schemeGroups[layer->tileScheme().id()].push_back(layer.get());
        ++layerCount;
    }
    groupingMs = perf::nowMs() - groupingStartMs;
    groupCount = static_cast<int>(schemeGroups.size());

    // 2. 每组计算一次 TilePlan，然后分发给组内所有图层
    for (auto& [schemeId, group] : schemeGroups) {
        if (group.empty()) continue;
        const TileScheme& scheme = group[0]->tileScheme();
        const Camera& camera = *frameState.camera;
        int previousZoom = -1;
        auto prevIt = previousZoomByScheme_.find(schemeId);
        if (prevIt != previousZoomByScheme_.end()) {
            previousZoom = prevIt->second;
        }

        TileQuadTree& quadTree = quadTreesByScheme_[schemeId];
        const double planStartMs = perf::nowMs();
        TilePlan plan = quadTree.compute(camera, scheme, vpW, vpH, previousZoom);
        tilePlanMs += perf::nowMs() - planStartMs;
        plan.frameId = frameState.frameId;
        previousZoomByScheme_[schemeId] = plan.zoom;

        TileGroupKey key{schemeId, plan.zoom,
                         static_cast<int>(vpW), static_cast<int>(vpH)};
        auto iter = groupPlans_.emplace(std::move(key), std::move(plan)).first;

        const double applyStartMs = perf::nowMs();
        for (auto* layer : group) {
            layer->applyPlan(iter->second,
                             camera.position(),
                             camera.direction(),
                             frameState.hasInteractionFocus,
                             frameState.interactionFocusDirection);
        }
        applyPlanMs += perf::nowMs() - applyStartMs;
    }

    // 3. 所有可见图层加载缺失瓦片
    const double loadStartMs = perf::nowMs();
    for (auto& layer : layers_) {
        if (!layer->visible()) continue;
        layer->loadMissingTiles();
    }
    loadMissingMs = perf::nowMs() - loadStartMs;

    char detail[192];
    std::snprintf(detail, sizeof(detail),
        "grouping=%.2f tilePlan=%.2f apply=%.2f loadMissing=%.2f groups=%d layers=%d",
        groupingMs,
        tilePlanMs,
        applyPlanMs,
        loadMissingMs,
        groupCount,
        layerCount);
    perf::logTiming(frameState.frameId,
                    "BasemapLayerStack.update",
                    perf::nowMs() - updateStartMs,
                    detail);
}

// ============================================================
// 渲染命令
// ============================================================

void BasemapLayerStack::buildRenderCommands(Renderer& renderer,
                                             const TerrainLayer* terrainLayer,
                                             RenderCommandList& commands) {
    for (auto& layer : layers_) {
        if (!layer->visible()) continue;
        layer->buildRenderCommands(renderer, terrainLayer, commands);
    }
}

// ============================================================
// 调试
// ============================================================

std::vector<std::pair<std::string, std::vector<TileKey>>>
BasemapLayerStack::allVisibleTiles() const {
    std::vector<std::pair<std::string, std::vector<TileKey>>> result;
    for (const auto& layer : layers_) {
        if (!layer->visible()) continue;
        result.emplace_back(layer->id(), layer->visibleTiles());
    }
    return result;
}

// ============================================================
// 控制点验证
// ============================================================

std::vector<BasemapLayerStack::ControlPointResult>
BasemapLayerStack::verifyControlPoint(double lngRad, double latRad, int zoom) const {
    std::vector<ControlPointResult> results;

    for (const auto& layer : layers_) {
        const TileScheme& scheme = layer->tileScheme();

        TileKey key = scheme.positionToTile(lngRad, latRad, zoom);
        Rectangle bounds = scheme.tileToRectangle(key);

        ControlPointResult r;
        r.layerId = layer->id();
        r.tileKey = key;
        r.tileBounds = bounds;
        r.inExpectedRange = bounds.contains(lngRad, latRad);

        results.push_back(r);
    }

    // 跨 scheme 对比：所有 layer 的 tileBounds 应包含控制点
    // 如果同 CRS 不同 scheme（如 XYZ vs TMS），tileKey 不同但 tileBounds
    // （在经纬度空间）应一致或非常接近
    return results;
}

} // namespace earth_engine
