#include "SceneLayerCoordinator.h"
#include "../interaction/SelectionManager.h"
#include "../layers/FeatureRenderLayer.h"
#include "../layers/VectorLayer.h"

#include <algorithm>

namespace earth_engine {

SceneLayerCoordinator::SceneLayerCoordinator() = default;
SceneLayerCoordinator::~SceneLayerCoordinator() = default;

void SceneLayerCoordinator::setRenderDevice(RenderDevice* device) {
    renderDevice_ = device;
    if (!renderDevice_) return;
    for (auto& layer : vectorLayers_) {
        if (layer) {
            layer->initialize(renderDevice_);
        }
    }
}

void SceneLayerCoordinator::addVectorLayer(std::unique_ptr<VectorLayer> layer) {
    if (!layer) return;
    layer->initialize(renderDevice_);
    vectorLayers_.push_back(std::move(layer));
}

std::unique_ptr<VectorLayer> SceneLayerCoordinator::removeVectorLayer(
    const std::string& layerId) {
    auto it = std::find_if(
        vectorLayers_.begin(),
        vectorLayers_.end(),
        [&](const auto& layer) {
            return layer && layer->id() == layerId;
        });
    if (it == vectorLayers_.end()) return nullptr;

    auto removed = std::move(*it);
    vectorLayers_.erase(it);
    return removed;
}

void SceneLayerCoordinator::addFeatureRenderLayer(
    std::unique_ptr<FeatureRenderLayer> layer) {
    if (!layer) return;
    featureRenderLayers_.push_back(std::move(layer));
}

std::unique_ptr<FeatureRenderLayer>
SceneLayerCoordinator::removeFeatureRenderLayer(const std::string& layerId) {
    auto it = std::find_if(
        featureRenderLayers_.begin(),
        featureRenderLayers_.end(),
        [&](const auto& layer) {
            return layer && layer->id() == layerId;
        });
    if (it == featureRenderLayers_.end()) return nullptr;

    auto removed = std::move(*it);
    featureRenderLayers_.erase(it);
    return removed;
}

bool SceneLayerCoordinator::applyFeatureState(
    const std::string& layerId,
    const std::string& featureId,
    FeatureState state) {
    const char* stateStr = "";
    switch (state) {
        case FeatureState::Hovered:
            stateStr = "hover";
            break;
        case FeatureState::Selected:
            stateStr = "selected";
            break;
        default:
            break;
    }

    for (auto& layer : vectorLayers_) {
        if (layer && layer->id() == layerId) {
            layer->setFeatureState(featureId, stateStr);
            return true;
        }
    }
    return false;
}

} // namespace earth_engine
