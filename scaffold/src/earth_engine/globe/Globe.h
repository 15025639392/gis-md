#pragma once

#include <cstdint>
#include <vector>

namespace earth_engine {

/// GPU 上传用的地球顶点格式。
/// 所有字段为 float32，适合直接装入 vertex buffer。
struct GlobeVertex {
    float position[3];   // 椭球面坐标（单位：semi-major axis = 1，扁平率 = WGS84）
    float normal[3];     // 椭球面法线（单位向量）
    float texcoord[2];   // u: 经度 0→1（西→东），v: 纬度 0→1（南→北）
};

/// CPU 侧椭球网格数据。
/// 创建后由 Renderer 上传到 GPU。
struct GlobeMesh {
    std::vector<GlobeVertex> vertices;
    std::vector<uint32_t> indices;
};

/// WGS84 椭球网格生成器。
/// 使用经纬线 tessellation，经线段数决定精度。
class Globe {
public:
    /// 创建 WGS84 椭球网格。
    /// @param longitudeSegments 经线段数（默认 96）
    /// @param latitudeSegments  纬线段数（默认 48）
    /// @return 顶点 + 索引数据，可上传 GPU
    static GlobeMesh createMesh(int longitudeSegments = 96,
                                int latitudeSegments = 48);

    /// WGS84 极半径比（b/a）
    static constexpr float polarRadiusRatio = 0.9966471893352525f;
};

} // namespace earth_engine
