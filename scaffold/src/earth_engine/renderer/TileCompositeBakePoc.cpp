#include "TileCompositeBakePoc.h"

#include "../debug/PerfTimer.h"
#include "RenderCommand.h"

#include <algorithm>
#include <vector>

namespace earth_engine {

namespace {

// 复用 OffscreenPostProcess 的全屏 quad + blit 采样 shader:每个合成 quad 在
// bake 目标上着色整幅(采样源影像)= 一遍 fill。u_tileTexture 后端固定绑 unit 0。
const char* kBakeVertGLSL = R"(#version 300 es
precision highp float;
layout(location = 0) in vec2 a_position;
out vec2 v_uv;
void main() {
    v_uv = a_position * 0.5 + 0.5;
    gl_Position = vec4(a_position, 0.0, 1.0);
}
)";
const char* kBakeFragGLSL = R"(#version 300 es
precision mediump float;
uniform sampler2D u_tileTexture;
in vec2 v_uv;
out vec4 fragColor;
void main() {
    fragColor = texture(u_tileTexture, v_uv);
}
)";

}  // namespace

bool TileCompositeBakePoc::initialize(RenderDevice* device,
                                      const TileCompositeBakePocConfig& config) {
    if (!device) {
        return false;
    }
    device_ = device;
    config_ = config;
    config_.bakeTargetSize = std::max(1, config_.bakeTargetSize);
    config_.maxBakeTilesPerFrame = std::max(0, config_.maxBakeTilesPerFrame);
    config_.quadsPerTile = std::max(0, config_.quadsPerTile);

    // 合成资源:GLES 才建(shader 仅 GLSL,同 OffscreenPostProcess 的 Metal 限制)。
    // 建不出(Metal/失败)→ compositeShader_ 空 → tick 退化为只 pass 切换。
    if (config_.quadsPerTile > 0 &&
        device_->backendType() != RenderDevice::Backend::Metal) {
        ShaderDesc sd;
        sd.vertexSource = kBakeVertGLSL;
        sd.fragmentSource = kBakeFragGLSL;
        compositeShader_ = device_->createShader(sd);

        const float quad[] = {-1.f, -1.f, 1.f, -1.f, -1.f, 1.f, 1.f, 1.f};
        BufferDesc bd;
        bd.size = sizeof(quad);
        bd.data = quad;
        bd.usage = BufferDesc::Usage::Static;
        bd.type = BufferDesc::Type::Vertex;
        quadBuffer_ = device_->createBuffer(bd);

        // 源影像纹理(代表被合成的覆盖瓦片):64×64 RGBA8 就够,fill 成本在目标
        // 像素数不在源尺寸。内容无所谓(全 0),量的是采样+着色 fill。
        const int srcN = 64;
        std::vector<uint8_t> pixels(
            static_cast<size_t>(srcN) * srcN * 4u, uint8_t{128});
        TextureDesc td;
        td.width = srcN;
        td.height = srcN;
        td.format = TextureDesc::Format::RGBA8;
        td.data = pixels.data();
        td.dataSize = pixels.size();
        td.mipmap = false;
        sourceTexture_ = device_->createTexture(td);
    }
    return true;
}

bool TileCompositeBakePoc::ensureResources() {
    if (!device_) {
        return false;
    }
    if (bakeFbo_) {
        return true;
    }
    FramebufferDesc desc;
    desc.width = config_.bakeTargetSize;
    desc.height = config_.bakeTargetSize;
    desc.hasColor = true;
    desc.hasDepth = false;  // 合成只写 color,不需深度
    desc.depthSampleable = false;
    auto fbo = device_->createFramebuffer(desc);
    if (!fbo) {
        return false;
    }
    bakeFbo_ = std::move(fbo);
    return true;
}

TileCompositeBakeFrameStats TileCompositeBakePoc::tick(int cappedTileCount) {
    TileCompositeBakeFrameStats stats;
    if (!isReady()) {
        lastStats_ = stats;
        return stats;
    }
    stats.ready = true;
    const int wanted = std::max(0, cappedTileCount);
    const int n = std::min(wanted, config_.maxBakeTilesPerFrame);
    stats.bakedTiles = n;
    // B 的逐瓦片纹理内存需求(未截上限,反映真实场景需要多少):N × 定尺²×4。
    stats.bakeBytes =
        static_cast<int64_t>(wanted) *
        static_cast<int64_t>(config_.bakeTargetSize) *
        static_cast<int64_t>(config_.bakeTargetSize) * 4;

    // 每 capped 瓦片一次离屏 bake pass。骨架复用同一 bake FBO 逐瓦片重绑——在
    // tiled GPU 上每次 beginPass 都是独立的 tile load/store 切换,故 N 次 = N 个
    // pass 切换的真实成本(=B 相对 C 单 atlas 的每帧代价)。quadsPerTile 个合成
    // draw 是下一步(需 textured-quad shader);骨架先量 pass 切换地板(下界)。
    // 预构建合成命令(每个 = 全屏 quad 采样源影像着色整个 bake 目标 = 一遍 fill)。
    const bool canComposite = compositeShader_ && quadBuffer_ && sourceTexture_ &&
                              config_.quadsPerTile > 0;
    RenderCommandList compositeCmds;
    if (canComposite) {
        RenderCommand cmd;
        cmd.kind = RenderCommandKind::Unknown;
        cmd.owner = "tile_composite_bake";
        cmd.pass = "bake";
        cmd.shader = compositeShader_.get();
        cmd.vertexBuffer = quadBuffer_.get();
        cmd.vertexCount = 4;
        cmd.vertexStride = 2 * sizeof(float);
        cmd.primitive = RenderCommand::PrimitiveType::TriangleStrip;
        cmd.depthTest = false;
        cmd.depthWrite = false;
        cmd.blend = false;
        cmd.cullFace = false;
        cmd.textures.push_back(sourceTexture_.get());  // unit 0 = 源影像
        compositeCmds.assign(config_.quadsPerTile, cmd);
    }

    const double startMs = perf::nowMs();
    for (int i = 0; i < n; ++i) {
        if (device_->beginPass(bakeFbo_.get())) {
            // quadsPerTile 个 fill draw 合成进 bake 目标(GLES);Metal 无 shader
            // 时 compositeCmds 空 → 只量 pass 切换 + clear 地板。
            if (!compositeCmds.empty()) {
                device_->submit(compositeCmds);
            }
            device_->endPass();
        }
    }
    stats.bakeMs = perf::nowMs() - startMs;

    lastStats_ = stats;
    return stats;
}

void TileCompositeBakePoc::dispose() {
    bakeFbo_.reset();
    compositeShader_.reset();
    quadBuffer_.reset();
    sourceTexture_.reset();
    lastStats_ = TileCompositeBakeFrameStats{};
}

}  // namespace earth_engine
