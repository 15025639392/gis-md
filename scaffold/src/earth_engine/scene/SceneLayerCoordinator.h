#pragma once

#include <memory>
#include <string>
#include <vector>

namespace earth_engine {

enum class FeatureState;
class RenderDevice;
class VectorLayer;

class SceneLayerCoordinator {
public:
    using LayerList = std::vector<std::unique_ptr<VectorLayer>>;

    SceneLayerCoordinator();
    ~SceneLayerCoordinator();

    void setRenderDevice(RenderDevice* device);

    void addVectorLayer(std::unique_ptr<VectorLayer> layer);
    std::unique_ptr<VectorLayer> removeVectorLayer(const std::string& layerId);
    size_t vectorLayerCount() const { return vectorLayers_.size(); }

    LayerList& vectorLayers() { return vectorLayers_; }
    const LayerList& vectorLayers() const { return vectorLayers_; }

    bool applyFeatureState(const std::string& layerId,
                           const std::string& featureId,
                           FeatureState state);

private:
    RenderDevice* renderDevice_ = nullptr;
    LayerList vectorLayers_;
};

} // namespace earth_engine
