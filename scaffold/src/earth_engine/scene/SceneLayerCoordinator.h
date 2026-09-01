#pragma once

#include <memory>
#include <string>
#include <vector>

namespace earth_engine {

enum class FeatureState;
class FeatureRenderLayer;
class RenderDevice;
class VectorLayer;

class SceneLayerCoordinator {
public:
    using LayerList = std::vector<std::unique_ptr<VectorLayer>>;
    using FeatureLayerList = std::vector<std::unique_ptr<FeatureRenderLayer>>;

    SceneLayerCoordinator();
    ~SceneLayerCoordinator();

    void setRenderDevice(RenderDevice* device);

    void addVectorLayer(std::unique_ptr<VectorLayer> layer);
    std::unique_ptr<VectorLayer> removeVectorLayer(const std::string& layerId);
    size_t vectorLayerCount() const { return vectorLayers_.size(); }

    LayerList& vectorLayers() { return vectorLayers_; }
    const LayerList& vectorLayers() const { return vectorLayers_; }

    // ---- FeatureStore 渲染桥接层(矢量数据系统 P1) ----
    // 与旧 VectorLayer 平行的新路径;层自持 RenderDevice(构造时注入),
    // 无 initialize 二段式。
    bool addFeatureRenderLayer(std::unique_ptr<FeatureRenderLayer> layer);
    std::unique_ptr<FeatureRenderLayer> removeFeatureRenderLayer(
        const std::string& layerId);
    const FeatureLayerList& featureRenderLayers() const {
        return featureRenderLayers_;
    }

    bool applyFeatureState(const std::string& layerId,
                           const std::string& featureId,
                           FeatureState state);

private:
    friend class Scene;
    FeatureLayerList& mutableFeatureRenderLayers() {
        return featureRenderLayers_;
    }
    RenderDevice* renderDevice_ = nullptr;
    LayerList vectorLayers_;
    FeatureLayerList featureRenderLayers_;
};

} // namespace earth_engine
