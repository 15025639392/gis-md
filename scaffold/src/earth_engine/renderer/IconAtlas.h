#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace earth_engine {

class RenderDevice;
class Texture;

/// 位图图标图集(矢量 P6c 图标/Marker,设计 §11)。
///
/// 与 GlyphAtlas 平行:位图字节由应用层注入(addImage,引擎不读文件系统
/// ——分层原则),shelf 打包进单页 RGBA8 图集纹理,名字 → {uv, 源像素尺寸}
/// 缓存。渲染线程调用(纹理上传需 GL 上下文)。
///
/// 与 GlyphAtlas 的区别:字形是 SDF 单通道复制四通道、按需栅格化;图标是
/// 应用层给的成品 RGBA 直传,不做 SDF 化(内置矢量形状走解析 SDF,见
/// SymbolShape.h —— 位图通道存在的意义就是画 SDF 表达不了的美术图标)。
///
/// 限度(P6c):单页 1024² 无淘汰,同名重注入只占新格位(旧格位不回收);
/// 位图放大到远超源尺寸会有双线性糊边(这是位图通道的固有代价,要任意
/// 缩放清晰用内置 SDF 形状)。多页/LRU 后置。
class IconAtlas {
public:
    static constexpr int kAtlasSize = 1024;
    /// 相邻 frame 间隔(px):双线性采样在 frame 边界会吃到邻居像素,
    /// 留白隔离(与 GlyphAtlas 不同——字形 SDF 边界外是背景值,天然容错)。
    static constexpr int kPadding = 2;

    explicit IconAtlas(RenderDevice* device);
    ~IconAtlas();

    IconAtlas(const IconAtlas&) = delete;
    IconAtlas& operator=(const IconAtlas&) = delete;

    /// 单图标在图集中的位置与源尺寸。
    struct Frame {
        float u0 = 0, v0 = 0, u1 = 0, v1 = 0;  ///< 图集 uv([0,1])
        float widthPx = 0;   ///< 源位图宽(px,定宽高比)
        float heightPx = 0;  ///< 源位图高(px)
    };

    /// 注入命名位图(RGBA8 行主序紧排,rgba.size() 必须 = width*height*4)。
    /// 尺寸非法/字节数不符/图集满/设备缺失 → false。同名重注入 = 覆盖
    /// 登记(占新格位)。成功则 revision 递增。
    bool addImage(const std::string& name,
                  int width,
                  int height,
                  const std::vector<uint8_t>& rgba);

    /// 查 frame(未注入返回 nullptr)。
    const Frame* frame(const std::string& name) const;

    /// 图集页满导致的注入拒绝计数(addImage 因页满返回 false 的次数)。
    /// >0 = 有图标注入失败、对应要素将回落 circle。静默丢图标不可接受:
    /// 首次页满打 Warning 日志,消费方据此报警。多页/LRU 落地前的可观测兜底。
    int pageFullRejectCount() const;

    bool empty() const;

    /// 图集纹理(尚无图标时为 nullptr)。
    Texture* texture() const;

    /// 注入代次:变化 → 已镶嵌的桶需重镶补 uv(图标可在建桶后才注入)。
    uint64_t revision() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace earth_engine
