#include "TileGeomorphHeightDelta.h"

#include "TilesetTile.h"

#include "../content/GltfModel.h"
#include "../core/geodesy/Cartographic.h"
#include "../core/geodesy/Ellipsoid.h"
#include "../tiling/GltfRenderGeometryBuilder.h"  // TerrainGpuVertex 布局

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include <vector>

namespace earth_engine {

namespace {

// 父瓦片一个顶点的地理坐标(经纬度 + 椭球高)。ECEF→cartographic 是采样热点,
// 每个父顶点只转一次。
struct GeoVertex {
    double lon = 0.0;
    double lat = 0.0;
    double height = 0.0;
    bool valid = false;
};

// 三角形重心插值高度:(lon,lat) 落在 abc 内则返回插值高,否则 nullopt。
// 与 LoadedTerrainHeightSampler 的 sampleTriangleHeight 同式(经纬度平面重心)。
std::optional<float> triangleHeight(const GeoVertex& a,
                                    const GeoVertex& b,
                                    const GeoVertex& c,
                                    double lon,
                                    double lat) {
    const double v0x = b.lon - a.lon;
    const double v0y = b.lat - a.lat;
    const double v1x = c.lon - a.lon;
    const double v1y = c.lat - a.lat;
    const double v2x = lon - a.lon;
    const double v2y = lat - a.lat;
    const double denom = v0x * v1y - v1x * v0y;
    if (std::abs(denom) <= std::numeric_limits<double>::epsilon()) {
        return std::nullopt;
    }
    const double invDenom = 1.0 / denom;
    const double u = (v2x * v1y - v1x * v2y) * invDenom;
    const double v = (v0x * v2y - v2x * v0y) * invDenom;
    constexpr double kEps = 1e-10;
    if (u < -kEps || v < -kEps || u + v > 1.0 + kEps) {
        return std::nullopt;
    }
    const double w = 1.0 - u - v;
    return static_cast<float>(a.height * w + b.height * u + c.height * v);
}

// 父瓦片 TIN 的经纬度均匀网格加速器:把每个三角形按其经纬度包围盒登记进重叠
// 的格子,查询时只测命中格子里的三角形→每次采样 ~O(1),避免逐子顶点扫全部父
// 三角形(O(子顶点 × 父三角形) 会炸主线程)。
class ParentHeightGrid {
public:
    ParentHeightGrid(const GltfModel& model, const Mat4& transform) {
        const Ellipsoid& ellipsoid = Ellipsoid::WGS84();
        // 收集所有 primitive 的顶点/三角形(展平成单一三角形表)。
        for (const GltfPrimitive& primitive : model.primitives) {
            const size_t base = geo_.size();
            geo_.reserve(base + primitive.vertices.size());
            for (const SurfaceVertex& v : primitive.vertices) {
                std::optional<Cartographic> c =
                    ellipsoid.tryCartesianToCartographic(
                        transform * v.positionEcef);
                GeoVertex gv;
                if (c) {
                    gv = GeoVertex{c->longitude(), c->latitude(),
                                   c->height(), true};
                }
                geo_.push_back(gv);
            }
            const auto emit = [&](uint32_t a, uint32_t b, uint32_t c) {
                tris_.push_back(Triangle{static_cast<uint32_t>(base) + a,
                                         static_cast<uint32_t>(base) + b,
                                         static_cast<uint32_t>(base) + c});
            };
            if (primitive.indices.empty()) {
                const size_t n = primitive.vertices.size();
                for (size_t i = 0; i + 2 < n; i += 3) {
                    emit(static_cast<uint32_t>(i),
                         static_cast<uint32_t>(i + 1),
                         static_cast<uint32_t>(i + 2));
                }
            } else {
                const std::vector<uint32_t>& idx = primitive.indices;
                for (size_t i = 0; i + 2 < idx.size(); i += 3) {
                    if (idx[i] < primitive.vertices.size() &&
                        idx[i + 1] < primitive.vertices.size() &&
                        idx[i + 2] < primitive.vertices.size()) {
                        emit(idx[i], idx[i + 1], idx[i + 2]);
                    }
                }
            }
        }
        buildGrid();
    }

    bool empty() const { return tris_.empty() || cols_ == 0 || rows_ == 0; }

    std::optional<float> sample(double lon, double lat) const {
        if (empty() || lon < minLon_ || lon > maxLon_ || lat < minLat_ ||
            lat > maxLat_) {
            return std::nullopt;
        }
        const int cx = clampCol(colOf(lon));
        const int cy = clampRow(rowOf(lat));
        const std::vector<uint32_t>& bucket = cells_[cy * cols_ + cx];
        for (uint32_t t : bucket) {
            const Triangle& tri = tris_[t];
            const GeoVertex& a = geo_[tri.a];
            const GeoVertex& b = geo_[tri.b];
            const GeoVertex& c = geo_[tri.c];
            if (!a.valid || !b.valid || !c.valid) {
                continue;
            }
            if (std::optional<float> h = triangleHeight(a, b, c, lon, lat)) {
                return h;
            }
        }
        return std::nullopt;
    }

private:
    struct Triangle {
        uint32_t a, b, c;
    };

    void buildGrid() {
        if (tris_.empty()) {
            return;
        }
        minLon_ = minLat_ = std::numeric_limits<double>::max();
        maxLon_ = maxLat_ = std::numeric_limits<double>::lowest();
        for (const GeoVertex& v : geo_) {
            if (!v.valid) {
                continue;
            }
            minLon_ = std::min(minLon_, v.lon);
            maxLon_ = std::max(maxLon_, v.lon);
            minLat_ = std::min(minLat_, v.lat);
            maxLat_ = std::max(maxLat_, v.lat);
        }
        if (!(maxLon_ > minLon_) || !(maxLat_ > minLat_)) {
            tris_.clear();
            return;
        }
        // 格子数 ~ sqrt(三角形数),使每格平均 ~1 个三角形。
        const int dim = std::max(
            1, static_cast<int>(std::sqrt(static_cast<double>(tris_.size()))));
        cols_ = dim;
        rows_ = dim;
        invLonSpan_ = static_cast<double>(cols_) / (maxLon_ - minLon_);
        invLatSpan_ = static_cast<double>(rows_) / (maxLat_ - minLat_);
        cells_.assign(static_cast<size_t>(cols_) * rows_, {});
        for (uint32_t t = 0; t < tris_.size(); ++t) {
            const Triangle& tri = tris_[t];
            const GeoVertex& a = geo_[tri.a];
            const GeoVertex& b = geo_[tri.b];
            const GeoVertex& c = geo_[tri.c];
            if (!a.valid || !b.valid || !c.valid) {
                continue;
            }
            const int c0 = clampCol(colOf(std::min({a.lon, b.lon, c.lon})));
            const int c1 = clampCol(colOf(std::max({a.lon, b.lon, c.lon})));
            const int r0 = clampRow(rowOf(std::min({a.lat, b.lat, c.lat})));
            const int r1 = clampRow(rowOf(std::max({a.lat, b.lat, c.lat})));
            for (int y = r0; y <= r1; ++y) {
                for (int x = c0; x <= c1; ++x) {
                    cells_[y * cols_ + x].push_back(t);
                }
            }
        }
    }

    int colOf(double lon) const {
        return static_cast<int>((lon - minLon_) * invLonSpan_);
    }
    int rowOf(double lat) const {
        return static_cast<int>((lat - minLat_) * invLatSpan_);
    }
    int clampCol(int x) const { return std::clamp(x, 0, cols_ - 1); }
    int clampRow(int y) const { return std::clamp(y, 0, rows_ - 1); }

    std::vector<GeoVertex> geo_;
    std::vector<Triangle> tris_;
    std::vector<std::vector<uint32_t>> cells_;
    int cols_ = 0;
    int rows_ = 0;
    double minLon_ = 0.0, maxLon_ = 0.0, minLat_ = 0.0, maxLat_ = 0.0;
    double invLonSpan_ = 0.0, invLatSpan_ = 0.0;
};

// 从 child 向上找第一个持有可采样地形网格的祖先(父级可能是 fill/upsample
// 中间态,但它当前显示的表面正是我们要 morph 起点接的那层)。返回其模型 +
// content transform;无则 nullptr。
const GltfModel* findParentSurface(const TilesetTile& child, Mat4& outTransform) {
    const TilesetTile* ancestor = child.getParent();
    while (ancestor) {
        const GltfModel* model =
            ancestor->content.renderContent.gltfModelForRead();
        if (model && !model->primitives.empty()) {
            bool hasVerts = false;
            for (const GltfPrimitive& p : model->primitives) {
                if (!p.vertices.empty()) {
                    hasVerts = true;
                    break;
                }
            }
            if (hasVerts) {
                outTransform = ancestor->content.renderContent.gltfTransform();
                return model;
            }
        }
        ancestor = ancestor->getParent();
    }
    return nullptr;
}

} // namespace

void TileGeomorphHeightDelta::applyParentGeomorph(TilesetTile& child) {
    TileRenderContentState& rc = child.content.renderContent;
    if (rc.geomorphHeightDeltaApplied) {
        return;
    }
    if (!rc.isTerrainRenderContent()) {
        return;
    }
    GltfModel* childModel = nullptr;
    {
        auto edit = rc.editGltfContent();
        childModel = edit ? &(*edit) : nullptr;
    }
    if (!childModel || childModel->primitives.empty()) {
        return;
    }

    Mat4 parentTransform = Mat4::identity();
    const GltfModel* parentModel = findParentSurface(child, parentTransform);
    if (!parentModel) {
        // 根瓦片或父级尚无网格:不 morph,标记已处理避免逐帧重试。
        rc.geomorphHeightDeltaApplied = true;
        return;
    }
    ParentHeightGrid grid(*parentModel, parentTransform);
    if (grid.empty()) {
        rc.geomorphHeightDeltaApplied = true;
        return;
    }

    const Mat4& childTransform = rc.gltfTransform();
    const Ellipsoid& ellipsoid = Ellipsoid::WGS84();
    constexpr size_t kStride = sizeof(TerrainGpuVertex);
    constexpr size_t kDeltaOffset = offsetof(TerrainGpuVertex, heightDelta);

    for (GltfPrimitive& primitive : childModel->primitives) {
        const size_t n = primitive.vertices.size();
        if (n == 0 ||
            primitive.terrainGpuVertexBytes.size() != n * kStride) {
            continue;  // 未预建 32B 字节:非本路径,跳过(保持 0)。
        }
        uint8_t* bytes = primitive.terrainGpuVertexBytes.data();
        for (size_t i = 0; i < n; ++i) {
            const Cartographic cart = ellipsoid.cartesianToCartographic(
                childTransform * primitive.vertices[i].positionEcef);
            const double trueH = cart.height();
            const std::optional<float> parentH =
                grid.sample(cart.longitude(), cart.latitude());
            // 父级无覆盖(瓦片边缘/无数据)→delta 0=该顶点不 morph,恒贴真值,
            // 与父级边界共享处天然对齐,不产生跨瓦片裂缝。
            const float delta =
                parentH ? static_cast<float>(*parentH - trueH) : 0.0f;
            std::memcpy(bytes + i * kStride + kDeltaOffset, &delta,
                        sizeof(float));
        }
    }
    rc.geomorphHeightDeltaApplied = true;
}

} // namespace earth_engine
