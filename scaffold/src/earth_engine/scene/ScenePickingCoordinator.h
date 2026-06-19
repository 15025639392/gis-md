#pragma once

#include "../interaction/PickingService.h"

#include <memory>
#include <vector>

namespace earth_engine {

class Camera;
class Tileset;
class VectorLayer;

struct ScenePickingContext {
    const PickingService* pickingService = nullptr;
    const Camera* camera = nullptr;
    double viewportWidthPixels = 0.0;
    double viewportHeightPixels = 0.0;
    const Tileset* terrainTileset = nullptr;
    const std::vector<std::unique_ptr<VectorLayer>>* vectorLayers = nullptr;
};

class ScenePickingCoordinator {
public:
    static PickResult pick(const ScenePickingContext& context,
                           float screenX,
                           float screenY);
    static bool pickInteractionFocus(const ScenePickingContext& context,
                                     float screenX,
                                     float screenY,
                                     Vec3& outPoint);
};

} // namespace earth_engine
