#pragma once

#include "Feature.h"
#include "FeatureSpatialIndex.h"
#include "../core/math/Rectangle.h"

#include <optional>
#include <unordered_map>
#include <vector>

namespace earth_engine {

/// 权威可编辑要素存储(系统数据中心)。
///
/// 持有全精度 Feature(唯一真相源)+ R-tree 空间索引。GPU 缓冲是它的派生,
/// 编辑改存储、派生随脏区增量重建(重建逻辑属渲染层,P1+)。
///
/// **规模策略**(已锁):接口留百万级视口分页(load+evict)口,先按万级全驻内存实现。
///   - queryVisible 现为同步(全驻内存);接分页时改异步/触发 load,签名演进不破坏调用方语义。
///   - 桶(bucket,镶嵌单元)属渲染派生,P1 引入;P0 只做存储 + 索引 + 导入。
class FeatureStore {
public:
    FeatureStore() = default;

    FeatureStore(const FeatureStore&) = delete;
    FeatureStore& operator=(const FeatureStore&) = delete;

    // ---- 写入(编辑期由 EditSession 调,P2) ----

    /// 加一条要素。若 feature.id 为无效(0),分配新的稳定 ID。
    /// 返回最终 ID。bounds 若为空(未算)则由 rings 计算。
    FeatureId addFeature(Feature feature);

    /// 删除要素。返回是否删到。
    bool removeFeature(FeatureId id);

    /// 替换要素几何/属性(编辑),保持 ID,version 递增,重建索引条目。
    /// 返回是否存在并更新。
    bool updateFeature(const Feature& feature);

    // ---- 读取 ----

    /// 按 ID 取(不存在返 nullptr)。
    const Feature* getFeature(FeatureId id) const;

    /// 视口范围查询:返回包围盒与 bbox 相交的要素 ID(顺序不保证)。
    /// (全驻内存同步版;分页落地后此接口演进为异步/触发 load。)
    std::vector<FeatureId> queryVisible(const Rectangle& bbox) const;

    size_t size() const { return features_.size(); }
    bool empty() const { return features_.empty(); }

    /// 遍历全部要素(测试/导出用)。
    const std::unordered_map<FeatureId, Feature>& features() const {
        return features_;
    }

    void clear();

private:
    /// 若 bounds 为空,从 rings 计算经纬度包围盒。
    static Rectangle computeBoundsFromRings(const Feature& f);

    std::unordered_map<FeatureId, Feature> features_;
    FeatureSpatialIndex index_;
    FeatureId nextId_ = 1;  // 稳定 ID 分配器,0 保留为无效
};

} // namespace earth_engine
