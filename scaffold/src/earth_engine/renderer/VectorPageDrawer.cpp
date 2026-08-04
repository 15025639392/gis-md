#include "VectorPageDrawer.h"

#include <algorithm>
#include <cstring>

#include "../core/async/AsyncSystem.h"
#include "../data/MvtDecoder.h"
#include "../debug/PlatformLog.h"
#include "RenderCommand.h"
#include "RenderDevice.h"
#include "Renderer.h"

namespace earth_engine {

namespace {

/// 与 TerrainPageStore::packKey 同布局(z/x/y),此处只做本类内部索引。
uint64_t packTile(const TileKey& key) {
    return (static_cast<uint64_t>(key.z) << 44) |
           (static_cast<uint64_t>(key.x) << 22) |
           static_cast<uint64_t>(key.y);
}

constexpr int kMaxUploadsPerTick = 4;  // 建 GPU buffer 涓流,勿在一帧里灌爆

}  // namespace

struct VectorPageDrawer::Inbox {
    struct Item {
        uint64_t key = 0;
        VectorTileMesh mesh;
        bool failed = false;
    };
    std::mutex mutex;
    std::vector<Item> items;
};

VectorPageDrawer::VectorPageDrawer(RenderDevice* device, Renderer* renderer,
                                   ThreadPool* pool, Options options,
                                   FetchFn fetch)
    : device_(device),
      renderer_(renderer),
      pool_(pool),
      options_(std::move(options)),
      fetch_(std::move(fetch)),
      inbox_(std::make_shared<Inbox>()) {}

VectorPageDrawer::~VectorPageDrawer() = default;

void VectorPageDrawer::makePageOrtho(double originX, double originY,
                                     double span, bool flipY,
                                     float outMatrix[16]) {
    const double s = span > 0.0 ? span : 1.0;
    const double sx = 2.0 / s;
    const double tx = -2.0 * originX / s - 1.0;
    // 瓦片本地 y 向下(y=0 在北)。页的行 0 也是北 —— GL 的 FBO 行 0 在 NDC 的
    // -1 侧,Metal 的行 0 在 +1 侧,故只有 y 的符号不同。
    const double sy = flipY ? -2.0 / s : 2.0 / s;
    const double ty = flipY ? (2.0 * originY / s + 1.0)
                            : (-2.0 * originY / s - 1.0);
    // 列主序。
    outMatrix[0] = static_cast<float>(sx);
    outMatrix[1] = 0.0f;
    outMatrix[2] = 0.0f;
    outMatrix[3] = 0.0f;
    outMatrix[4] = 0.0f;
    outMatrix[5] = static_cast<float>(sy);
    outMatrix[6] = 0.0f;
    outMatrix[7] = 0.0f;
    outMatrix[8] = 0.0f;
    outMatrix[9] = 0.0f;
    outMatrix[10] = 1.0f;
    outMatrix[11] = 0.0f;
    outMatrix[12] = static_cast<float>(tx);
    outMatrix[13] = static_cast<float>(ty);
    outMatrix[14] = 0.0f;
    outMatrix[15] = 1.0f;
}

void VectorPageDrawer::kickFetch(const TileKey& srcKey, uint64_t srcPacked) {
    if (!fetch_) {
        tiles_[srcPacked].state = TileState::Failed;
        return;
    }
    ++fetchesKicked_;
    std::shared_ptr<Inbox> inbox = inbox_;
    ThreadPool* pool = pool_;
    VectorRasterStyle style = options_.style;  // 按值:worker 可能活过样式变更
    const int zoom = srcKey.z;
    fetch_(srcKey, [inbox, pool, style, zoom, srcPacked](
                       int statusCode, std::vector<uint8_t> body) {
        auto work = [inbox, style, zoom, srcPacked, body = std::move(body),
                     statusCode]() {
            Inbox::Item item;
            item.key = srcPacked;
            MvtTile tile;
            if (statusCode != 200 || body.empty() ||
                !decodeMvtTile(body.data(), body.size(), tile)) {
                item.failed = true;
            } else {
                item.mesh = buildVectorTileMesh(tile, zoom, style);
            }
            std::lock_guard<std::mutex> lock(inbox->mutex);
            inbox->items.push_back(std::move(item));
        };
        if (pool) {
            pool->enqueue(std::move(work));
        } else {
            work();
        }
    });
}

void VectorPageDrawer::tickDecorator() {
    // 节流插桩(~每 60 帧)。**看的是增量**:稳态下 drawn/fetch 每段都应趋近 0;
    // fetch 增量持续 > 0 = LRU thrash(缓存装不下当前可见页对应的源瓦片数);
    // fetch≈0 而 drawn 持续高 = pass 数本身是成本,得从合批/降频下手。
    if (++tickCounter_ % 60u == 0u) {
        platformLog(LogLevel::Warning, "VecPage",
                    "cachedTiles=%d ready=%d drawn+=%d fetch+=%d evict=%d",
                    static_cast<int>(tiles_.size()), readyTiles_,
                    drawnPages_ - lastDrawn_, fetchesKicked_ - lastFetches_,
                    evictions_);
        lastDrawn_ = drawnPages_;
        lastFetches_ = fetchesKicked_;
    }
    std::vector<Inbox::Item> ready;
    {
        std::lock_guard<std::mutex> lock(inbox_->mutex);
        ready.swap(inbox_->items);
    }
    int uploaded = 0;
    for (size_t i = 0; i < ready.size(); ++i) {
        if (uploaded >= kMaxUploadsPerTick) {
            // 超预算:剩余回投递箱,下帧继续(建 buffer 是真 GPU 活)。
            std::lock_guard<std::mutex> lock(inbox_->mutex);
            for (size_t j = i; j < ready.size(); ++j) {
                inbox_->items.push_back(std::move(ready[j]));
            }
            break;
        }
        Inbox::Item& item = ready[i];
        auto it = tiles_.find(item.key);
        if (it == tiles_.end()) {
            continue;  // 已被 LRU 淘汰:结果作废(下次要用会重拉)
        }
        GpuTile& gpu = it->second;
        if (item.failed) {
            gpu.state = TileState::Failed;
            continue;
        }
        if (item.mesh.empty()) {
            // 该瓦片确实没有可画内容 —— 不是失败。必须区分:Empty 让页立刻
            // 判「已叠画」,否则页存储会每帧重试到天荒地老。
            gpu.state = TileState::Empty;
            continue;
        }
        BufferDesc vbd;
        vbd.size = item.mesh.vertices.size() * sizeof(VectorTileMeshVertex);
        vbd.data = item.mesh.vertices.data();
        vbd.type = BufferDesc::Type::Vertex;
        BufferDesc ibd;
        ibd.size = item.mesh.indices.size() * sizeof(uint32_t);
        ibd.data = item.mesh.indices.data();
        ibd.type = BufferDesc::Type::Index;
        gpu.vertexBuffer = device_->createBuffer(vbd);
        gpu.indexBuffer = device_->createBuffer(ibd);
        if (!gpu.vertexBuffer || !gpu.indexBuffer) {
            gpu.state = TileState::Failed;
            continue;
        }
        gpu.indexCount = static_cast<int>(item.mesh.indices.size());
        gpu.state = TileState::Ready;
        ++readyTiles_;
        ++uploaded;
    }
}

void VectorPageDrawer::touchAndTrim(uint64_t srcPacked) {
    auto posIt = lruPos_.find(srcPacked);
    if (posIt != lruPos_.end()) {
        lru_.erase(posIt->second);
    }
    lru_.push_front(srcPacked);
    lruPos_[srcPacked] = lru_.begin();
    while (lru_.size() > options_.maxCachedTiles) {
        const uint64_t victim = lru_.back();
        lru_.pop_back();
        lruPos_.erase(victim);
        tiles_.erase(victim);  // GPU buffer 随 unique_ptr 释放
        ++evictions_;
    }
}

bool VectorPageDrawer::ensureFramebuffer(Texture* target) {
    if (framebuffer_ && framebufferTarget_ == target) {
        return true;
    }
    FramebufferDesc desc;
    desc.width = options_.pageSizeTexels;
    desc.height = options_.pageSizeTexels;
    desc.hasColor = true;
    desc.hasDepth = false;  // 2D 叠画,画家序即最终顺序
    desc.externalColorTarget = target;
    desc.externalColorLayer = 0;
    framebuffer_ = device_->createFramebuffer(desc);
    framebufferTarget_ = framebuffer_ ? target : nullptr;
    return framebuffer_ != nullptr;
}

bool VectorPageDrawer::decoratePage(const TileKey& pageKey, Texture* target,
                                    int layer) {
    if (!device_ || !renderer_ || !target || layer < 0) {
        return true;  // 无从叠画:别让页存储无限重试
    }
    ShaderProgram* shader = renderer_->vectorPageMeshShader();
    if (!shader) {
        return true;  // 着色器不可用 → 回落 mappedRaster 上的栅格版
    }

    // 页比数据侧上限深时取祖先瓦片 + 子矩形正交投影(**几何不放大**)。
    const int srcZ = std::min(pageKey.z, options_.maxSourceZoom);
    const int depth = std::max(0, pageKey.z - srcZ);
    TileKey srcKey = pageKey;
    srcKey.z = srcZ;
    srcKey.x = pageKey.x >> depth;
    srcKey.y = pageKey.y >> depth;
    const uint64_t srcPacked = packTile(srcKey);

    auto it = tiles_.find(srcPacked);
    if (it == tiles_.end()) {
        tiles_[srcPacked] = GpuTile{};
        touchAndTrim(srcPacked);
        kickFetch(srcKey, srcPacked);
        return false;  // 源在路上,页存储后续帧再叫
    }
    touchAndTrim(srcPacked);
    GpuTile& gpu = it->second;
    switch (gpu.state) {
        case TileState::Pending:
            return false;
        case TileState::Empty:
        case TileState::Failed:
            return true;  // 没得画/画不了,都算「已处理」,别无限重试
        case TileState::Ready:
            break;
    }

    if (!ensureFramebuffer(target)) {
        return true;
    }
    // ⚠️ 改绑失败必须短路:继续画会画到上一次绑定的层上 ——
    // 现象是「别处的路网出现在这块地面上」,极难反推。
    if (!device_->setFramebufferColorLayer(framebuffer_.get(), target, layer)) {
        return true;
    }

    const double span = 1.0 / static_cast<double>(1 << depth);
    const double originX =
        static_cast<double>(pageKey.x & ((1 << depth) - 1)) * span;
    const double originY =
        static_cast<double>(pageKey.y & ((1 << depth) - 1)) * span;
    float mvp[16];
    makePageOrtho(originX, originY, span,
                  device_->backendType() == RenderDevice::Backend::Metal, mvp);

    RenderCommand cmd;
    cmd.kind = RenderCommandKind::VectorFill;  // 靠 stride 20 分派到 PageMesh20
    cmd.pass = "color";
    cmd.shader = shader;
    cmd.vertexBuffer = gpu.vertexBuffer.get();
    cmd.indexBuffer = gpu.indexBuffer.get();
    cmd.indexCount = gpu.indexCount;
    cmd.indexType = RenderCommand::IndexType::UInt32;
    cmd.vertexStride = static_cast<int>(sizeof(VectorTileMeshVertex));
    cmd.primitive = RenderCommand::PrimitiveType::Triangles;
    cmd.depthTest = false;
    cmd.depthWrite = false;
    cmd.blend = true;   // 叠在已灌的影像上 = alphaOver
    cmd.cullFace = false;  // 镶嵌不保证绕向一致(接头方块与线段四边形各自成三角)
    cmd.uniforms["u_modelViewProjection"] =
        std::vector<float>(mvp, mvp + 16);
    // 一个页像素等于多少瓦片归一化单位 —— 顶点着色器用它展开线宽。
    const float extrudeScale =
        static_cast<float>(span / std::max(1, options_.pageSizeTexels));
    cmd.uniforms["u_extrudeScale"] = {extrudeScale, extrudeScale};

    if (!device_->beginPass(framebuffer_.get(), /*clearTarget=*/false)) {
        return false;  // pass 拿不到(如 Metal 跳帧):下帧再试
    }
    RenderCommandList list;
    list.push_back(std::move(cmd));
    device_->submit(list);
    device_->endPass();
    ++drawnPages_;
    return true;
}

}  // namespace earth_engine
