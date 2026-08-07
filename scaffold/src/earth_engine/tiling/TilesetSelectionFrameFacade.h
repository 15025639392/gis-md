#pragma once

#include "TileSelectionPerformanceTimings.h"

namespace earth_engine {

struct FrameState;
class Tileset;
class IPrepareRendererResources;

/// 每帧瓦片选择入口(同步,渲染线程)。
/// 历史:曾有 asyncSelection 影子树路径(选择下 worker)与 ③ 增量切面路径,
/// 均于 2026-08-07 删除 —— 前者自 2026-07-19「release 下 selector 非瓶颈」
/// 裁决后从未在生产启用;后者 2026-07-07 已裁决 NO-GO(对拖动零杠杆),
/// Layer 2/3 从未建成,二者互为唯一存在理由。需要时 git 找回。
class TilesetSelectionFrameFacade {
public:
    static void selectTiles(
        Tileset& tileset,
        const FrameState& frameState,
        TileSelectionPerformanceTimings* performanceTimings = nullptr,
        IPrepareRendererResources* pPrepRenderer = nullptr);
};

} // namespace earth_engine
