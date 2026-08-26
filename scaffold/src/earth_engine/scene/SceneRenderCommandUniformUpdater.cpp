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
        if (cmd.kind == RenderCommandKind::GltfPrimitive ||
            cmd.kind == RenderCommandKind::GltfPrimitiveInstanced) {
            // 北极星 Phase 2c 地形 GPU 位移：per-tile 全刚体 ENU→ECEF 帧承载
            // 落位（顶点在 ENU 局部帧），替 translation-only 路径。gated——
            // 未置该标志的命令逐字节走原 modelOrigin 平移。
            glm::dmat4 model(1.0);
            if (cmd.hasTerrainDisplacementFrame) {
                model = glm::make_mat4(
                    cmd.terrainDisplacementModelMatrix.data());
            } else if (cmd.hasGltfUniforms) {
                glm::dvec3 origin(cmd.gltfUniforms.modelOrigin[0],
                                  cmd.gltfUniforms.modelOrigin[1],
                                  cmd.gltfUniforms.modelOrigin[2]);
                model = glm::translate(glm::dmat4(1.0), origin);
            }
            glm::dmat4 mvp = viewProj * model;
            glm::mat4 mvpFloat = glm::mat4(mvp);
            if (cmd.hasGltfUniforms) {
                std::memcpy(cmd.gltfUniforms.modelViewProjection.data(),
                            glm::value_ptr(mvpFloat),
                            16 * sizeof(float));
                // 位移帧路径下 v_normal 在 ENU 局部帧（模板法线随帧共享），故
                // lightDir 也须变到该帧，否则片元光照帧不匹配而错。方向量只旋转
                // (R 正交，R^T = R^-1)。平移路径下 lightDir 仍是世界向量。
                if (cmd.hasTerrainDisplacementFrame) {
                    const glm::dvec3 lightWorld(frameState.lightDir.x,
                                                frameState.lightDir.y,
                                                frameState.lightDir.z);
                    const glm::dvec3 lightLocal =
                        glm::transpose(glm::dmat3(model)) * lightWorld;
                    cmd.gltfUniforms.lightDir = {
                        static_cast<float>(lightLocal.x),
                        static_cast<float>(lightLocal.y),
                        static_cast<float>(lightLocal.z)};
                } else {
                    cmd.gltfUniforms.lightDir = {frameState.lightDir.x,
                                                 frameState.lightDir.y,
                                                 frameState.lightDir.z};
                }
                cmd.gltfUniforms.ambient = {frameState.ambient.r,
                                            frameState.ambient.g,
                                            frameState.ambient.b,
                                            1.0f};
                // 日落地表暖化(B1):地形受光面乘 sunTint、阴影面叠加暖补光
                // (terrainSunAmbient)。仅地形——glTF 模型 shader 不读 u_sunTint、
                // 也不应被地形专属的日落暖补光改动。地形命令走 GltfPrimitive/
                // Instanced,故在本(上)支按 owner 判定;下方 map 支对地形不可达。
                if (cmd.owner == "terrain_primitive" ||
                    cmd.owner == "terrain_instanced") {
                    cmd.gltfUniforms.sunTint = {frameState.sunTint.r,
                                                frameState.sunTint.g,
                                                frameState.sunTint.b,
                                                0.0f};
                    cmd.gltfUniforms.ambient = {
                        frameState.ambient.r + frameState.terrainSunAmbient.r,
                        frameState.ambient.g + frameState.terrainSunAmbient.g,
                        frameState.ambient.b + frameState.terrainSunAmbient.b,
                        1.0f};
                }
                // 相机世界坐标相对本瓦片局部帧原点的偏移。RTC 相减在双精度下
                // 完成，float 只承载小量级差值 → 水面 sun-glint 求视向量 V。
                // 位移帧路径下 v_position 在 ENU 局部帧，故 eye 也变到该帧
                // (inverse(frame)·eyeWorld)；平移路径下即 eyeWorld − tileOrigin。
                const glm::dvec3 eyeWorld(cam.position().x(),
                                          cam.position().y(),
                                          cam.position().z());
                glm::dvec3 eyeRtc;
                if (cmd.hasTerrainDisplacementFrame) {
                    eyeRtc = glm::dvec3(glm::inverse(model) *
                                        glm::dvec4(eyeWorld, 1.0));
                } else {
                    const glm::dvec3 tileOrigin(cmd.gltfUniforms.modelOrigin[0],
                                                cmd.gltfUniforms.modelOrigin[1],
                                                cmd.gltfUniforms.modelOrigin[2]);
                    eyeRtc = eyeWorld - tileOrigin;
                }
                cmd.gltfUniforms.eyePositionRTC = {
                    static_cast<float>(eyeRtc.x),
                    static_cast<float>(eyeRtc.y),
                    static_cast<float>(eyeRtc.z)};
            } else {
                // 兜底：外部手工构造、未启用定长块的 glTF 命令仍走 map。
                auto& mvpU = cmd.uniforms["u_modelViewProjection"];
                mvpU.resize(16);
                std::memcpy(
                    mvpU.data(), glm::value_ptr(mvpFloat), 16 * sizeof(float));
                cmd.uniforms["u_lightDir"] = {frameState.lightDir.x,
                                              frameState.lightDir.y,
                                              frameState.lightDir.z};
            }
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

        // FeatureRenderLayer 已按桶 origin 在双精度下合成 RTE MVP，并放入
        // 定长块。这里不能再向通用 map 插入绝对 viewProj：既是无效分配，
        // 也会让后端出现两套同名 uniform 的覆盖顺序歧义。
        if (cmd.hasVectorUniforms) {
            continue;
        }

        auto& mvpU = cmd.uniforms["u_modelViewProjection"];
        if (mvpU.empty()) {
            mvpU.resize(16);
            std::memcpy(
                mvpU.data(), glm::value_ptr(viewProj), 16 * sizeof(float));
        }
        // 注:地形(surface tile)命令 kind=GltfPrimitive/Instanced,走上支的
        // gltfUniforms 块并 continue,永不到达这里。日落 sunTint/暖 ambient 已在
        // 上支按 owner 写入 gltfUniforms(见 terrain_primitive/terrain_instanced
        // 分支)。此前这里有个 `owner=="surface_tile"` 块——无命令用该 owner,是
        // 死代码,B1 的地表暖化因此从未生效,已移除。
    }
}

} // namespace earth_engine
