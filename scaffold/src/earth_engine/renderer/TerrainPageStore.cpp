#include "TerrainPageStore.h"

#include <algorithm>
#include <array>
#include <vector>

#include "../tiling/TilesetTile.h"
#include "RenderCommand.h"
#include "RenderDevice.h"

namespace earth_engine {

namespace {

// Step 3a 合成图案:每层一个可区分的纯色。gridN=2 → 4 层 = 目标瓦片四象限
// 显示红/绿/蓝/黄,肉眼即可确认「按 mesh UV 落格 → 选对 layer → 采样 array」
// 整链正确(选错格/层会串色,ghost 会跨象限渗)。3b 换真实影像。
constexpr std::array<std::array<uint8_t, 4>, 4> kLayerColors = {{
    {255, 64, 64, 255},    // layer 0 红
    {64, 255, 64, 255},    // layer 1 绿
    {64, 96, 255, 255},    // layer 2 蓝
    {255, 220, 48, 255},   // layer 3 黄
}};

}  // namespace

TerrainPageStore::~TerrainPageStore() = default;

bool TerrainPageStore::initialize(RenderDevice* device, const Config& config) {
    if (!device) {
        return false;
    }
    device_ = device;
    config_ = config;
    config_.pageSizeTexels = std::max(1, config_.pageSizeTexels);
    config_.gridN = std::max(1, config_.gridN);
    const int layers = config_.gridN * config_.gridN;

    // texture2DArray:pageSize×pageSize×layers,RGBA8,逐层 CLAMP_TO_EDGE(§13.1
    // 无页缝),nearest 放大让合成图案边界清晰(3b 真实影像再回 linear)。
    TextureDesc desc;
    desc.width = config_.pageSizeTexels;
    desc.height = config_.pageSizeTexels;
    desc.arrayLayers = layers;
    desc.format = TextureDesc::Format::RGBA8;
    desc.mipmap = false;
    desc.minFilter = TextureDesc::Filter::Nearest;
    desc.magFilter = TextureDesc::Filter::Nearest;
    desc.wrapS = TextureDesc::Wrap::Clamp;
    desc.wrapT = TextureDesc::Wrap::Clamp;
    arrayTexture_ = device_->createTexture(desc);
    if (!arrayTexture_) {
        device_ = nullptr;
        return false;
    }

    // 逐层灌纯色(updateTextureRegion 的 layer 维,Step 2 能力)。
    const int side = config_.pageSizeTexels;
    std::vector<uint8_t> page(static_cast<size_t>(side) *
                              static_cast<size_t>(side) * 4u);
    for (int layer = 0; layer < layers; ++layer) {
        const std::array<uint8_t, 4>& color =
            kLayerColors[static_cast<size_t>(layer) % kLayerColors.size()];
        for (size_t px = 0; px < page.size(); px += 4) {
            page[px + 0] = color[0];
            page[px + 1] = color[1];
            page[px + 2] = color[2];
            page[px + 3] = color[3];
        }
        if (!device_->updateTextureRegion(arrayTexture_.get(), 0, 0, side, side,
                                          page.data(),
                                          static_cast<size_t>(side) * 4u,
                                          layer)) {
            arrayTexture_.reset();
            device_ = nullptr;
            return false;
        }
    }
    return true;
}

void TerrainPageStore::applyToTerrainCommand(RenderCommand& cmd,
                                             const TilesetTile& tile) {
    if (!arrayTexture_ || !cmd.terrainRenderContent) {
        return;
    }
    // 只挂真实地形(fill/ellipsoid proxy 不是最终高清目标)。
    if (cmd.terrainSurfaceSource != TerrainSurfaceCommandSource::RealTerrain) {
        return;
    }
    // 锁定绘制序里第一个真实地形瓦片为目标(demo 相机静止下稳定)。
    if (!targetLocked_) {
        targetKey_ = tile.key;
        targetLocked_ = true;
    }
    if (tile.key != targetKey_) {
        return;
    }

    if (cmd.textures.size() <=
        static_cast<size_t>(kGltfPageStoreArrayTextureSlot)) {
        cmd.textures.resize(
            static_cast<size_t>(kGltfPageStoreArrayTextureSlot) + 1u, nullptr);
    }
    cmd.textures[kGltfPageStoreArrayTextureSlot] = arrayTexture_.get();
    // enabled=1、gridN、layerBase=0 → 片元改采页存储,覆盖上采样 mappedRaster。
    cmd.gltfUniforms.pageStoreParams = {
        1.0f, static_cast<float>(config_.gridN), 0.0f, 0.0f};
}

}  // namespace earth_engine
