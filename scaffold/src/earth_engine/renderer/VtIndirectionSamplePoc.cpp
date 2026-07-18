#include "VtIndirectionSamplePoc.h"

#include "../debug/PerfTimer.h"
#include "RenderCommand.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace earth_engine {

namespace {

// 全屏 quad 顶点 shader(与 OffscreenPostProcess/bake PoC 同款)。
const char* kVertGLSL = R"(#version 300 es
precision highp float;
layout(location = 0) in vec2 a_position;
out vec2 v_uv;
void main() {
    v_uv = a_position * 0.5 + 0.5;
    gl_Position = vec4(a_position, 0.0, 1.0);
}
)";

// 片元 shader:DESCENT 层**依赖间接 fetch** + 1 次 atlas 采样。
//   • u_tileTexture = 间接纹理(unit 0,后端固定绑此名到 0);NEAREST。
//   • u_depthTexture = atlas(unit 1,后端固定绑此名到 1);linear。
//   命名复用后端已配置的 sampler 名(RenderDeviceGLES samplersConfigured 块),
//   零后端改动即拿到 unit 0/1。
//   依赖链:node 每层由上一次 fetch 取回的 .xy 决定下一次取址 → 编译器无法把
//   fetch 优化掉(结果流入 atlasUv→输出),且 latency-hiding 对依赖链失效 =
//   忠实量出间接寻址的真实成本命门。DESCENT=0 → 退化成纯 1 次 atlas 采样地板。
std::string makeFragGLSL(int descentLevels) {
    const int d = std::max(0, descentLevels);
    std::string src;
    src += "#version 300 es\n";
    src += "precision highp float;\n";
    src += "#define DESCENT " + std::to_string(d) + "\n";
    src += R"(
uniform sampler2D u_tileTexture;   // 间接纹理(unit 0)
uniform sampler2D u_depthTexture;  // atlas(unit 1)
in vec2 v_uv;
out vec4 fragColor;
void main() {
    vec2 uv = v_uv;
    vec2 node = uv;            // 当前降到的节点原点(归一化 atlas 空间)
#if DESCENT > 0
    for (int i = 0; i < DESCENT; ++i) {
        // 取址依赖上一层结果(node):自顶向下降的依赖 fetch 链。
        vec2 addr = fract(node + uv * exp2(float(i)) * 0.017);
        vec4 entry = texture(u_tileTexture, addr);   // NEAREST 子指针 fetch
        node = entry.xy;                              // 依赖:下一层取址用它
    }
#endif
    vec2 atlasUv = fract(node + uv * 0.0625);   // 解出的槽 + 局部 fraction
    fragColor = texture(u_depthTexture, atlasUv);
}
)";
    return src;
}

std::unique_ptr<ShaderProgram> buildShader(RenderDevice* device, int descent) {
    ShaderDesc sd;
    sd.vertexSource = kVertGLSL;
    sd.fragmentSource = makeFragGLSL(descent);
    return device->createShader(sd);
}

}  // namespace

bool VtIndirectionSamplePoc::initialize(
    RenderDevice* device, const VtIndirectionSamplePocConfig& config) {
    if (!device) {
        return false;
    }
    device_ = device;
    config_ = config;
    config_.indirectionSize = std::max(1, config_.indirectionSize);
    config_.atlasSize = std::max(1, config_.atlasSize);
    config_.passesPerTick = std::max(1, config_.passesPerTick);

    // GLSL only(同 bake PoC/OffscreenPostProcess 的 Metal 限制):Metal 直接不
    // 建 shader → isReady=false,tick 空转。测量台在 Adreno。
    if (device_->backendType() == RenderDevice::Backend::Metal) {
        return true;
    }

    baselineShader_ = buildShader(device_, 0);
    if (!baselineShader_) {
        return false;
    }
    for (int i = 0; i < kVtSweepCount; ++i) {
        auto sh = buildShader(device_, vtSweepDepths()[i]);
        if (!sh) {
            baselineShader_.reset();
            descentShaders_.clear();
            return false;
        }
        descentShaders_.push_back(std::move(sh));
    }

    const float quad[] = {-1.f, -1.f, 1.f, -1.f, -1.f, 1.f, 1.f, 1.f};
    BufferDesc bd;
    bd.size = sizeof(quad);
    bd.data = quad;
    bd.usage = BufferDesc::Usage::Static;
    bd.type = BufferDesc::Type::Vertex;
    quadBuffer_ = device_->createBuffer(bd);

    // 间接纹理:填**散布的**子指针(每 texel 存自身归一化坐标的扰动),让依赖链
    // 落点铺开整张纹理 = 真实 cache 行为(而非全部命中一个 texel 的假快路径)。
    // NEAREST(间接纹理惯例:整数槽索引不能插值)。
    {
        const int n = config_.indirectionSize;
        std::vector<uint8_t> px(static_cast<size_t>(n) * n * 4u);
        for (int y = 0; y < n; ++y) {
            for (int x = 0; x < n; ++x) {
                const size_t o = (static_cast<size_t>(y) * n + x) * 4u;
                // 伪散布:高低位交错,子指针指向 atlas 各处。
                px[o + 0] = static_cast<uint8_t>((x * 37 + y * 17) & 0xFF);
                px[o + 1] = static_cast<uint8_t>((x * 11 + y * 53) & 0xFF);
                px[o + 2] = static_cast<uint8_t>((x ^ y) & 0xFF);
                px[o + 3] = 255;
            }
        }
        TextureDesc td;
        td.width = n;
        td.height = n;
        td.format = TextureDesc::Format::RGBA8;
        td.data = px.data();
        td.dataSize = px.size();
        td.mipmap = false;
        td.minFilter = TextureDesc::Filter::Nearest;
        td.magFilter = TextureDesc::Filter::Nearest;
        td.wrapS = TextureDesc::Wrap::Repeat;
        td.wrapT = TextureDesc::Wrap::Repeat;
        indirectionTexture_ = device_->createTexture(td);
    }
    // atlas:linear(真实影像采样),内容无所谓(量的是采样,不是像素值)。
    {
        const int n = config_.atlasSize;
        std::vector<uint8_t> px(static_cast<size_t>(n) * n * 4u, uint8_t{128});
        TextureDesc td;
        td.width = n;
        td.height = n;
        td.format = TextureDesc::Format::RGBA8;
        td.data = px.data();
        td.dataSize = px.size();
        td.mipmap = false;
        td.minFilter = TextureDesc::Filter::Linear;
        td.magFilter = TextureDesc::Filter::Linear;
        td.wrapS = TextureDesc::Wrap::Repeat;
        td.wrapT = TextureDesc::Wrap::Repeat;
        atlasTexture_ = device_->createTexture(td);
    }
    return indirectionTexture_ != nullptr && atlasTexture_ != nullptr &&
           quadBuffer_ != nullptr;
}

bool VtIndirectionSamplePoc::ensureResources(int surfaceWidthPixels,
                                             int surfaceHeightPixels) {
    if (!device_ || !baselineShader_) {
        return false;  // Metal / 未建 shader:测量台不适用此后端
    }
    const int w = std::max(1, surfaceWidthPixels);
    const int h = std::max(1, surfaceHeightPixels);
    if (fillFbo_ && fillWidth_ == w && fillHeight_ == h) {
        return true;
    }
    FramebufferDesc desc;
    desc.width = w;
    desc.height = h;
    desc.hasColor = true;
    desc.hasDepth = false;  // 纯 fill,不需深度
    desc.depthSampleable = false;
    auto fbo = device_->createFramebuffer(desc);
    if (!fbo) {
        return false;
    }
    fillFbo_ = std::move(fbo);
    fillWidth_ = w;
    fillHeight_ = h;
    return true;
}

double VtIndirectionSamplePoc::runPasses(ShaderProgram* shader, int n) {
    RenderCommand cmd;
    cmd.kind = RenderCommandKind::Unknown;
    cmd.owner = "vt_indirection_sample";
    cmd.pass = "vt_fill";
    cmd.shader = shader;
    cmd.vertexBuffer = quadBuffer_.get();
    cmd.vertexCount = 4;
    cmd.vertexStride = 2 * sizeof(float);
    cmd.primitive = RenderCommand::PrimitiveType::TriangleStrip;
    cmd.depthTest = false;
    cmd.depthWrite = false;
    cmd.blend = false;
    cmd.cullFace = false;
    cmd.textures.push_back(indirectionTexture_.get());  // unit 0 = u_tileTexture
    cmd.textures.push_back(atlasTexture_.get());         // unit 1 = u_depthTexture
    RenderCommandList cmds(1, cmd);

    const double startMs = perf::nowMs();
    for (int i = 0; i < n; ++i) {
        if (device_->beginPass(fillFbo_.get())) {
            device_->submit(cmds);
            device_->endPass();
        }
    }
    // **强制 GPU 完成**:tiled GPU 上 beginPass/submit 只是入队,片元着色被推迟到
    // resolve,纯 CPU 计时会量到「命令编码」而非真实 fill(实测 3.44MP fill 报
    // 0.02ms = 不可信)。1×1 回读强制 flush→resolve→等 GPU:所有 N 个 pass(同
    // 一 FBO,后画覆盖前画)必须先执行完回读才返回正确像素 → 计时区间纳入真实
    // 累计 GPU 时间。回读固定开销对 baseline/descent 两组**相同**,故其偏置在
    // 两组相消,delta(descent−baseline)= 纯逐片元间接采样开销(见 tick)。
    uint8_t oneTexel[4] = {0, 0, 0, 0};
    device_->readFramebufferPixels(fillFbo_.get(), 0, 0, 1, 1, oneTexel,
                                   sizeof(oneTexel));
    return perf::nowMs() - startMs;
}

VtIndirectionSampleFrameStats VtIndirectionSamplePoc::tick() {
    VtIndirectionSampleFrameStats stats;
    if (!isReady()) {
        lastStats_ = stats;
        return stats;
    }
    stats.ready = true;
    const int n = config_.passesPerTick;
    stats.passes = n;
    stats.fillPixels =
        static_cast<int64_t>(fillWidth_) * static_cast<int64_t>(fillHeight_);

    // 同帧同 GPU 态,每组末尾强制 GPU 同步(runPasses 内 1×1 回读)后计时:
    //   syncFloor = runPasses(_,0) = 仅同步屏障地板(无 draw);
    //   每组 total = syncFloor + Σ_N(真实 per-pass GPU fill),故减 syncFloor 再 /N
    //   = 干净的每-pass GPU fill。各组减的是同一 syncFloor,故组间可直接比。
    const double syncFloor = runPasses(baselineShader_.get(), 0);
    const double baseTotal = runPasses(baselineShader_.get(), n);
    stats.syncFloorMs = syncFloor;
    stats.baselineMs = std::max(0.0, baseTotal - syncFloor) / n;
    for (int i = 0; i < kVtSweepCount; ++i) {
        const double total = runPasses(descentShaders_[i].get(), n);
        stats.descentMs[i] = std::max(0.0, total - syncFloor) / n;
    }

    lastStats_ = stats;
    return stats;
}

void VtIndirectionSamplePoc::dispose() {
    fillFbo_.reset();
    baselineShader_.reset();
    descentShaders_.clear();
    quadBuffer_.reset();
    indirectionTexture_.reset();
    atlasTexture_.reset();
    fillWidth_ = 0;
    fillHeight_ = 0;
    lastStats_ = VtIndirectionSampleFrameStats{};
}

}  // namespace earth_engine
