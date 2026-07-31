#pragma once

#include "Feature.h"
#include "../core/math/Rectangle.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace earth_engine {

class FeatureStore;

/// 点聚合参数(矢量 P6c,设计 §11)。
struct FeatureClusterOptions {
    int minZoom = 0;         ///< 最粗聚合层
    int maxZoom = 16;        ///< 最细聚合层(超过此 zoom 查询即全展开)
    double radiusPx = 60.0;  ///< 聚合半径(屏幕像素)
    double tileSizePx = 512.0;  ///< 半径换算用的瓦片边长(zoom 尺度基准)
    /// 参与聚合的最小成员数:低于此的簇拆回单点(避免"2 个点也聚成簇")。
    uint32_t minPoints = 2;
};

/// 一个聚合结果(簇 或 单要素)。
struct FeatureCluster {
    /// 层内稳定 id(层号<<32 | 层内序号)。同一份 build 内稳定,可用于
    /// expansionZoom/leaves 查询;重新 build 后失效。
    uint64_t clusterId = 0;
    double longitude = 0.0;  ///< 代表点(成员经纬度均值,弧度)
    double latitude = 0.0;
    uint32_t count = 1;  ///< 成员要素数(1 = 未聚合的单要素)
    /// count==1 时为该要素 id,否则 kInvalidFeatureId。
    FeatureId featureId = kInvalidFeatureId;
    Rectangle bounds;  ///< 成员经纬度包围盒(弧度)

    bool isCluster() const { return count > 1; }
};

/// 层级预聚点索引(矢量 P6c 聚合,设计 §11)。
///
/// **分层边界(用户拍板)**:引擎只出「索引 + 按 zoom/视口查询」这项基础
/// 能力;聚合点怎么画、点击怎么展开、动效如何,全归应用层。本类不碰
/// 渲染、不改 FeatureStore、不参与编辑。
///
/// 算法 = supercluster 式层级预聚:maxZoom 层每点独立,逐层向粗合并
/// (半径 = radiusPx 在该 zoom 的世界尺度)。层级预聚而非逐帧聚类,保证
/// 缩放时**同一簇稳定不闪**;父子链固定,展开动画可用 expansionZoom。
///
/// **不受桶/瓦片边界限**:直接吃 FeatureStore 全量点(有全局索引),优于
/// MVT 流式聚合的跨瓦片割裂。
///
/// **编辑与聚合互斥**(设计 §11):进编辑模式应展开聚合以便点到单个要素
/// —— 这属应用层策略,本类只负责 build 后可随时重建(编辑改动后调用方
/// 自行 rebuild;索引不订阅 store 变更)。
///
/// 限度:球面墨卡托投影下聚类(与屏幕像素距离一致,椭球差异远小于聚合
/// 半径);不做反经线绕接(±180° 两侧的点不互相聚合);全量重建而非增量
/// (万级规模一次 build 是毫秒级,编辑后重建即可)。
class FeatureClusterIndex {
public:
    FeatureClusterIndex() = default;

    /// 全量重建:取 store 里所有 Point 要素做层级预聚(非 Point 忽略)。
    void build(const FeatureStore& store, const FeatureClusterOptions& opts);

    /// 按视口 + zoom 查询。zoom 钳制进 [minZoom, maxZoom];bbox 用经纬度
    /// (弧度)。返回顺序不保证。
    std::vector<FeatureCluster> query(const Rectangle& bbox,
                                      double zoom) const;

    /// 该簇"散开"所需的 zoom:再放大到这一级它就会拆成多个条目。
    /// 单要素/未知 id 返回 maxZoom + 1。应用层点击簇时可用它决定飞行目标。
    int expansionZoom(uint64_t clusterId) const;

    /// 簇下全部叶子要素 id(递归展开;单要素返回自身)。
    std::vector<FeatureId> leaves(uint64_t clusterId) const;

    bool empty() const { return levels_.empty(); }
    /// 层数(= maxZoom - minZoom + 1;未 build 为 0)。
    size_t levelCount() const { return levels_.size(); }
    const FeatureClusterOptions& options() const { return options_; }

private:
    /// 层内节点。x/y = 球面墨卡托归一化坐标 [0,1](成员加权均值)。
    struct Node {
        double x = 0.0;
        double y = 0.0;
        double lngSum = 0.0;  ///< 成员经纬度和(算代表点用,弧度)
        double latSum = 0.0;
        uint32_t count = 1;
        FeatureId featureId = kInvalidFeatureId;
        Rectangle bounds;
        /// 下一层(更细)中构成本节点的节点序号。
        std::vector<uint32_t> children;
    };

    /// levels_[0] = maxZoom(最细),末尾 = minZoom(最粗)。
    std::vector<std::vector<Node>> levels_;
    FeatureClusterOptions options_;

    /// clusterId ↔ (levelIndex, nodeIndex)。
    static uint64_t makeId(size_t levelIndex, size_t nodeIndex) {
        return (static_cast<uint64_t>(levelIndex) << 32) |
               static_cast<uint32_t>(nodeIndex);
    }
    bool decodeId(uint64_t id, size_t* levelIndex, size_t* nodeIndex) const;

    /// zoom → levels_ 下标(0 = maxZoom)。
    size_t levelIndexForZoom(double zoom) const;

    FeatureCluster toCluster(size_t levelIndex, size_t nodeIndex) const;
};

} // namespace earth_engine
