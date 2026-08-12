#pragma once

#include <memory>

namespace earth_engine {

class AtmosphereBackgroundPass;
class RenderDevice;
class SkyBox;
class SkyGradient;
class TimeController;
class Vec3;

class SceneEnvironmentCoordinator {
public:
    SceneEnvironmentCoordinator();
    ~SceneEnvironmentCoordinator();

    SceneEnvironmentCoordinator(const SceneEnvironmentCoordinator&) = delete;
    SceneEnvironmentCoordinator& operator=(const SceneEnvironmentCoordinator&) = delete;

    void initializeRenderResources(RenderDevice* device);

    void setTime(double julianDate);
    void setSunsetTerrainTint(float warmth, float shadowScale);
    double time() const;
    void advanceTime(double seconds);
    Vec3 sunDirection() const;

    TimeController* timeController() const { return timeController_.get(); }
    SkyGradient* skyGradient() const { return skyGradient_.get(); }
    AtmosphereBackgroundPass* atmospherePass() const {
        return atmospherePass_.get();
    }
    SkyBox* skyBox() const { return skyBox_.get(); }

private:
    std::unique_ptr<TimeController> timeController_;
    std::unique_ptr<SkyGradient> skyGradient_;
    std::unique_ptr<AtmosphereBackgroundPass> atmospherePass_;
    std::unique_ptr<SkyBox> skyBox_;
};

} // namespace earth_engine
