#pragma once

#include "Feature.h"
#include "../core/math/Rectangle.h"

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace earth_engine {

/// 桶键。高 32 位 = cellX,低 32 位 = cellY(int32 打包)。
using BucketKey = uint64_t;

/// 要素分桶网格(镶嵌 / 上传 / 脏区的单元)。
///
/// 与 R-tree(查询用)互补:分桶把要素按空间**分组**成稳定的渲染单元 ——
/// 编辑一个要素只标脏其所属桶,渲染层只重镶该桶。GPU 缓冲按桶组织。
///
/// **关键约定(设计 §9.2)**:要素按 **bounds 中心**整体归属**一个**桶,
/// **不在桶边界裁剪几何** —— 裁了就无法编辑一个完整的跨桶要素。跨多个 cell
/// 的**超大要素**(bounds 宽或高 > cell)走专用 `kOversizedBucket`,视图查询恒纳入,
/// 避免"视口在要素上但不在其中心 cell"时漏掉(设计 §9.5)。
///
/// 单级网格。多级(按 zoom)分桶留作后续 LOD 简化的载体。
class FeatureBucketGrid {
public:
    /// cell 尺寸(radian)。默认 ~0.02rad(赤道约 128km),按数据密度可调。
    static constexpr double kDefaultCellSize = 0.02;
    /// 超大要素专用桶(视图查询恒纳入)。int32 极值打包,与真实 cell 不冲突
    /// (lng/lat∈[-π,π] 下 cell 索引量级仅数百)。
    static constexpr BucketKey kOversizedBucket = 0xFFFFFFFFFFFFFFFFull;

    explicit FeatureBucketGrid(double cellSizeRadians = kDefaultCellSize);

    /// 计算某包围盒归属的桶(按中心;超大 → kOversizedBucket)。纯函数。
    BucketKey bucketFor(const Rectangle& bounds) const;

    /// 归入要素,返回其桶键并标脏。
    BucketKey assign(FeatureId id, const Rectangle& bounds);

    /// 移出要素(用其**当时的** bounds 定位桶;bounds 确定 → 可重算)。
    /// 桶变空则从成员表移除,但仍标脏(consumer 据此丢弃该桶几何)。
    void unassign(FeatureId id, const Rectangle& bounds);

    /// 桶内要素(不存在返 nullptr)。
    const std::unordered_set<FeatureId>* featuresIn(BucketKey key) const;

    /// 与视口相交的桶(cell 矩形广相交;超大桶若非空恒纳入)。
    std::vector<BucketKey> bucketsInView(const Rectangle& viewBbox) const;

    /// 取走并清空脏桶集合(渲染层重镶后调用)。
    std::unordered_set<BucketKey> consumeDirty();

    const std::unordered_set<BucketKey>& dirtyBuckets() const { return dirty_; }

    /// 非空桶数(含超大桶)。
    size_t bucketCount() const { return members_.size(); }

    double cellSize() const { return cellSize_; }

    void clear();

    // ---- cell / key 辅助(public static 便于测试) ----
    static BucketKey packCell(int32_t cx, int32_t cy);
    static int32_t cellX(BucketKey key);
    static int32_t cellY(BucketKey key);

    /// 桶的 cell 矩形(radian)。对 kOversizedBucket 无定义(返回空矩形)。
    Rectangle cellRect(BucketKey key) const;

private:
    double cellSize_;
    std::unordered_map<BucketKey, std::unordered_set<FeatureId>> members_;
    std::unordered_set<BucketKey> dirty_;
};

} // namespace earth_engine
