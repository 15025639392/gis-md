#include "SceneEnvironmentCoordinator.h"
#include "../environment/AtmosphereBackgroundPass.h"
#include "../environment/SkyBox.h"
#include "../environment/SkyGradient.h"
#include "../environment/SunDirection.h"
#include "../environment/TimeController.h"

namespace earth_engine {

SceneEnvironmentCoordinator::SceneEnvironmentCoordinator()
    : timeController_(std::make_unique<TimeController>()),
      skyGradient_(std::make_unique<SkyGradient>()),
      atmospherePass_(std::make_unique<AtmosphereBackgroundPass>()),
      skyBox_(std::make_unique<SkyBox>()) {}

SceneEnvironmentCoordinator::~SceneEnvironmentCoordinator() = default;

void SceneEnvironmentCoordinator::initializeRenderResources(
    RenderDevice* device) {
    atmospherePass_->initialize(device);
    skyBox_->initialize(device);
}

void SceneEnvironmentCoordinator::setTime(double julianDate) {
    timeController_->setJulianDate(julianDate);
}

void SceneEnvironmentCoordinator::setSunsetTerrainTint(float warmth,
                                                       float shadowScale) {
    skyGradient_->setSunsetTerrainTint(warmth, shadowScale);
}

double SceneEnvironmentCoordinator::time() const {
    return timeController_->julianDate();
}

void SceneEnvironmentCoordinator::advanceTime(double seconds) {
    timeController_->advanceSeconds(seconds);
}

Vec3 SceneEnvironmentCoordinator::sunDirection() const {
    return SunDirection::compute(timeController_->julianDate());
}

} // namespace earth_engine
