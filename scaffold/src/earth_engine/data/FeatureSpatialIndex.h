#pragma once

#include "Feature.h"
#include "../core/math/Rectangle.h"

#include <memory>
#include <vector>

namespace earth_engine {

/// 要素空间索引(R-tree)。
///
/// 覆盖 FeatureStore 的权威要素,按经纬度包围盒(radian)组织,支持:
///   - insert / remove : 编辑期动态维护(加删要素)
///   - query(bbox)     : 视口/拾取/snap 的范围查询
///
/// 采用经典 Guttman R-tree + 二次分裂(quadratic split)。选 R-tree 而非均匀网格,
/// 因为可编辑要素常空间聚集(万级集中一城),网格会热点化;R-tree 的 MBR 分裂对
/// 倾斜分布自适应,且天然支持插入/删除(编辑必需)。
///
/// **约定**:包围盒为非跨反经线矩形(west <= east)。GeoJsonParser 的 computeBounds
/// 即产出此约定(naive min/max)。跨反经线要素的正确索引(拆分为两矩形)留待后续,
/// 与 parser 的反经线处理一并推进。
class FeatureSpatialIndex {
public:
    FeatureSpatialIndex();
    ~FeatureSpatialIndex();

    FeatureSpatialIndex(const FeatureSpatialIndex&) = delete;
    FeatureSpatialIndex& operator=(const FeatureSpatialIndex&) = delete;

    /// 插入一条 (id, 包围盒)。同一 id 允许重复插入(调用方保证唯一)。
    void insert(FeatureId id, const Rectangle& bounds);

    /// 删除 id(需提供其包围盒以定位叶子)。返回是否删到。
    /// 若同一 bounds 下有多个 id,只删匹配 id 的那条。
    bool remove(FeatureId id, const Rectangle& bounds);

    /// 范围查询:返回包围盒与 queryBounds 相交的所有 FeatureId(顺序不保证)。
    std::vector<FeatureId> query(const Rectangle& queryBounds) const;

    /// 清空。
    void clear();

    /// 已索引条目数。
    size_t size() const { return count_; }

    bool empty() const { return count_ == 0; }

private:
    struct Entry {
        Rectangle mbr;
        FeatureId id;
    };

    struct Node {
        bool leaf = true;
        Rectangle mbr;                                  ///< 覆盖所有子项的包围盒
        std::vector<Entry> entries;                     ///< leaf: 数据条目
        std::vector<std::unique_ptr<Node>> children;    ///< internal: 子节点
    };

    // R-tree 参数。M=最大子项数,m=最小填充(condense 阈值)。
    static constexpr size_t kMaxEntries = 16;
    static constexpr size_t kMinEntries = kMaxEntries / 2;  // = 8

    // ---- 内部递归辅助 ----
    /// 向 node 插入 entry;若分裂,返回新兄弟节点(调用方接住并抬升)。
    std::unique_ptr<Node> insertRec(Node* node, const Entry& e);
    /// 选择最小扩张的子树。
    static Node* chooseSubtree(Node* node, const Rectangle& r);
    /// 分裂溢出节点,返回新兄弟。node 保留一半,兄弟拿另一半。
    std::unique_ptr<Node> splitNode(Node* node);
    /// 重算 node.mbr。
    static void recomputeMbr(Node* node);
    /// 递归收集与 q 相交的 id。
    static void queryRec(const Node* node, const Rectangle& q,
                         std::vector<FeatureId>& out);
    /// 删除:在子树中找到并移除 (id,bounds);把下溢的孤儿节点收进 reinsert。
    /// 返回该子树是否被改动(需重算 mbr)。found 输出是否删到。
    bool removeRec(Node* node, FeatureId id, const Rectangle& bounds,
                   std::vector<Entry>& reinsertEntries,
                   std::vector<std::unique_ptr<Node>>& reinsertNodes,
                   bool& found);
    /// 把一个内部节点子树的所有叶 entry 摊平收集(用于 condense 后重插)。
    static void collectEntries(Node* node, std::vector<Entry>& out);

    // ---- 几何辅助 ----
    static double area(const Rectangle& r);
    static double enlargement(const Rectangle& current, const Rectangle& add);

    std::unique_ptr<Node> root_;
    size_t count_ = 0;
    int rootHeight_ = 0;  ///< 叶层=0
};

} // namespace earth_engine
