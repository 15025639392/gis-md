#include "SceneRenderCommandUniformUpdater.h"
#include "Camera.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <cstring>

namespace earth_engine {

void SceneRenderCommandUniformUpdater::apply(
    const FrameState& frameState,
    RenderCommandList& commands) {
    if (!frameState.camera) {
        return;
    }

    const Camera& cam = *frameState.camera;
    const float vpW = static_cast<float>(frameState.viewportWidthPixels);
    const float vpH = static_cast<float>(frameState.viewportHeightPixels);

    const Mat4& viewRaw = cam.viewMatrix();
    const Mat4& projRaw = cam.projectionMatrix(
        static_cast<double>(vpW), static_cast<double>(vpH));
    glm::dmat4 viewD(viewRaw.raw());
    glm::dmat4 projD(projRaw.raw());
    glm::dmat4 viewProj = projD * viewD;

    for (auto& cmd : commands) {
        if (cmd.owner == "globe") {
            continue;
        }

        if (cmd.kind == RenderCommandKind::SurfaceTile &&
            cmd.hasSurfaceTileUniforms) {
            glm::dvec3 origin(cmd.surfaceTileOrigin[0],
                              cmd.surfaceTileOrigin[1],
                              cmd.surfaceTileOrigin[2]);
            glm::dmat4 model = glm::translate(glm::dmat4(1.0), origin);
            glm::dmat4 mvp = projD * viewD * model;
            glm::mat4 mvpFloat = glm::mat4(mvp);
            std::memcpy(cmd.surfaceModelViewProjection.data(),
                        glm::value_ptr(mvpFloat),
                        16 * sizeof(float));
            cmd.surfaceLightDir = {frameState.lightDir.x,
                                   frameState.lightDir.y,
                                   frameState.lightDir.z};
            continue;
        }

        if (cmd.kind == RenderCommandKind::GltfPrimitive ||
            cmd.kind == RenderCommandKind::GltfPrimitiveInstanced) {
            glm::dmat4 model(1.0);
            auto originIt = cmd.uniforms.find("u_modelOrigin");
            if (originIt != cmd.uniforms.end() &&
                originIt->second.size() >= 3) {
                glm::dvec3 origin(originIt->second[0],
                                  originIt->second[1],
                                  originIt->second[2]);
                model = glm::translate(glm::dmat4(1.0), origin);
            }
            glm::dmat4 mvp = viewProj * model;
            glm::mat4 mvpFloat = glm::mat4(mvp);
            auto& mvpU = cmd.uniforms["u_modelViewProjection"];
            mvpU.resize(16);
            std::memcpy(
                mvpU.data(), glm::value_ptr(mvpFloat), 16 * sizeof(float));
            cmd.uniforms["u_lightDir"] = {frameState.lightDir.x,
                                          frameState.lightDir.y,
                                          frameState.lightDir.z};
            if (cmd.hasWorldSortCenter) {
                const Vec3 center(cmd.worldSortCenter[0],
                                  cmd.worldSortCenter[1],
                                  cmd.worldSortCenter[2]);
                cmd.translucentSortDepth =
                    (center - cam.position()).dot(cam.direction());
                cmd.hasTranslucentSortDepth = true;
            }
            continue;
        }

        auto& mvpU = cmd.uniforms["u_modelViewProjection"];
        if (mvpU.empty()) {
            mvpU.resize(16);
            std::memcpy(
                mvpU.data(), glm::value_ptr(viewProj), 16 * sizeof(float));
        }
        if (cmd.owner == "surface_tile") {
            cmd.uniforms["u_lightDir"] = {frameState.lightDir.x,
                                          frameState.lightDir.y,
                                          frameState.lightDir.z};
        }
    }
}

} // namespace earth_engine
