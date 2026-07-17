#pragma once

namespace earth_engine {

struct TilesetTile;

// geomorph 变体 A(父级采样):把子瓦片每顶点的 morph 起点从「自平滑近似」
// 换成「父瓦片当前渲染面在该顶点处的真实高度」。这样子瓦片一出生就贴在父级
// 结束的位置,屏幕上没有「塌回平版再长起来」的跳变——细化是从上一层地形接着
// 往下长细节(GE 级连续 LOD 观感),而不是每层从 0 重新生长(变体 B 的结构天花板)。
class TileGeomorphHeightDelta {
public:
    // 主线程调用一次(CPU-ready→GPU upload 缝):就地重写 child 渲染内容
    // terrainGpuVertexBytes 的每顶点 heightDelta(offset 28,float32)=
    // 父级在该顶点(lon,lat)处地形高度 − 子级真实高度。
    //   morph=0(刚 refine)→顶点落在父级表面(与上一层显示连续)
    //   morph=1(过渡结束)→回到真实高度
    // 父级缺失(根瓦片/父级未加载/父级无网格)→heightDelta 保持不变(通常 0,
    // 即不 morph、直接出现,根瓦片本就是最粗层可接受)。
    // 由 child.content.renderContent.geomorphHeightDeltaApplied 门控,幂等。
    static void applyParentGeomorph(TilesetTile& child);
};

} // namespace earth_engine
