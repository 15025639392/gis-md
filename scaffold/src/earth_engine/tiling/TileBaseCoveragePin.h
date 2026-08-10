#pragma once

namespace earth_engine {

/// 根层常驻(漏底根修第 1 步,支柱三):z ≤ 本档的瓦片一经加载,几何与影像
/// 都**豁免预算驱逐** —— 让「选中瓦片必然有可画祖先」从直觉变成结构保证。
///
/// 正统对照:cesium 的 `_levelZeroTiles` 由 QuadtreePrimitive 永久持有、
/// 淘汰队列摸不到;osgEarth REX 靠场景树结构保证父链常驻。此前我们两头都
/// 没有:低空久留时中低 z 祖先被 LRU 换出,外拉后整条祖先链要重走网络,
/// finalizer 找不到可画祖先只能丢弃选中瓦片(renderEntryDropNotBuildable
/// = 真空洞),而呈现保持在镜头运动期按设计不咬合 → 漏底直接上屏。
///
/// 语义边界:
///  - **按 tileset 选择加入**(TilesetOptions::pinBaseCoverage,默认 false):
///    钉扎只对承担底图覆盖的地形 tileset 有意义;3D 内容树(TilesetJson/
///    SingleGltf)的低 z 包装节点可能很重,无差别钉扎是语义错误。SDK 场景
///    路径对主地形 tileset 与底图影像 overlay 显式开启。
///  - 只拦**预算驱逐**(LRU 入队),不拦强制卸载 —— loadState=Unloading 的
///    重建/失效卸载在 TileCacheUnloadCoordinator 里本就绕过资格检查,源切换
///    整树重建也不走这条队。钉住的是"内存紧张时别拿兜底层开刀"。
///  - 是"加载后不淘汰",不是"启动即预载":冷启动首帧的空窗仍由呈现保持
///    覆盖(相机静止时 hold 咬合)。
///  - 成本上界:全球 z≤3 两套 scheme 约 2×85 片,几何(粗档 + fade=0 压平)
///    + 256² 影像各 ~256KB 级,实际会话只驻留看过的子集,MB 量级。
inline constexpr int kPinnedBaseCoverageMaxZoom = 3;

inline bool isBaseCoveragePinnedZoom(int zoom) {
    return zoom <= kPinnedBaseCoverageMaxZoom;
}

} // namespace earth_engine
