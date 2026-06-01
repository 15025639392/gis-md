# Shader 接口规范

本文件定义引擎 C++ 侧与 GPU shader（MSL / GLSL ES）之间的接口契约。AI 在编写或修改 shader 前必须先理解本文件中的 vertex attribute 布局、uniform block 结构和坐标空间约定。

与 `graphics-pipeline.md` 配合使用：后者定义 GPU 管线的宏观约束，本文件定义 shader 级别的具体接口。

## 核心原则

- C++ 侧通过 `RenderCommand` 传递资源引用和数据，shader 侧按约定名称接收。
- 坐标使用 camera-relative：世界坐标减去 `cameraRelativeOrigin` 后传入 shader，避免 float32 精度问题。
- MSL (Metal Shading Language) 和 GLSL ES 3.0 分别提供等价声明。两者之间通过命名约定保持一致。
- 所有矩阵为列主序（与 GLM 一致）。

## Vertex Attribute 布局

引擎所有几何数据使用统一的 vertex attribute 索引：

| Location | 名称 | 类型 | 说明 |
|----------|------|------|------|
| 0 | `a_position` | `vec3` | 顶点位置（camera-relative ECEF，米） |
| 1 | `a_normal` | `vec3` | 顶点法线（归一化） |
| 2 | `a_texcoord` | `vec2` | 纹理坐标 (u, v)，范围 [0, 1] |
| 3 | `a_featureId` | `float` | Feature ID（用于 picking pass；color pass 不使用） |

### MSL 声明

```metal
struct VertexIn {
    float3 a_position   [[attribute(0)]];
    float3 a_normal     [[attribute(1)]];
    float2 a_texcoord   [[attribute(2)]];
    float  a_featureId  [[attribute(3)]];
};

struct VertexOut {
    float4 position     [[position]];
    float3 worldNormal;
    float2 texcoord;
    float  featureId;
};
```

### GLSL ES 声明

```glsl
// vertex shader
layout(location = 0) in vec3 a_position;
layout(location = 1) in vec3 a_normal;
layout(location = 2) in vec2 a_texcoord;
layout(location = 3) in float a_featureId;

out vec3 v_worldNormal;
out vec2 v_texcoord;
out float v_featureId;
```

## Uniform Blocks

### FrameUniforms（每帧全局）

用于全局帧状态（绑定频率：每帧一次）。

| 名称 | 类型 | 说明 |
|------|------|------|
| `u_modelViewProjection` | `mat4` | Model × View × Projection 矩阵（列主序） |
| `u_modelView` | `mat4` | Model × View 矩阵 |
| `u_normalMatrix` | `mat3` | 法线矩阵 = transpose(inverse(modelView)) |
| `u_cameraRelativeOrigin` | `vec3` | 相机相对原点（ECEF，米）。shader 中的 position 是 camera-relative。 |
| `u_viewportSize` | `vec2` | 视口尺寸（像素） |
| `u_time` | `float` | 场景时间（Julian date 或 秒） |
| `u_passType` | `int` | 0=color、1=depth、2=picking、3=shadow |

### MSL 声明

```metal
struct FrameUniforms {
    float4x4 u_modelViewProjection;
    float4x4 u_modelView;
    float3x3 u_normalMatrix;
    float3   u_cameraRelativeOrigin;
    float2   u_viewportSize;
    float    u_time;
    int      u_passType;
};
```

### GLSL ES 声明

```glsl
layout(std140) uniform FrameUniforms {
    mat4   u_modelViewProjection;
    mat4   u_modelView;
    mat3   u_normalMatrix;
    vec3   u_cameraRelativeOrigin;
    vec2   u_viewportSize;
    float  u_time;
    int    u_passType;
};
```

### TileUniforms（每个 tile）

用于单个瓦片的纹理和 LOD 信息（绑定频率：每个 tile draw call）。

| 名称 | 类型 | 说明 |
|------|------|------|
| `u_tileTexture` | `sampler2D` | 瓦片纹理（RGBA8 或 RGB8） |
| `u_tileUVOffset` | `vec2` | 纹理 UV 偏移（用于 parent fallback 裁剪） |
| `u_tileUVScale` | `vec2` | 纹理 UV 缩放 |
| `u_tileOpacity` | `float` | 瓦片透明度 [0, 1] |
| `u_tileZ` | `int` | 瓦片 zoom level（调试用） |

### StyleUniforms（图层样式）

用于矢量叠加层的样式参数（绑定频率：每个图层）。

| 名称 | 类型 | 说明 |
|------|------|------|
| `u_fillColor` | `vec4` | 填充颜色 (RGBA) |
| `u_outlineColor` | `vec4` | 描边颜色 (RGBA) |
| `u_outlineWidth` | `float` | 描边宽度（像素） |
| `u_pointSize` | `float` | 点大小（像素） |
| `u_lineWidth` | `float` | 线宽（像素） |

## Picking Pass

Picking pass 使用专用 shader：

- `u_passType == 2` 时启用。
- Fragment shader 输出 `featureId` 编码为 RGBA 颜色。
- 不执行光照计算。
- 不参与深度写入（或使用单独的 depth framebuffer）。

### Feature ID 编码

```glsl
// 将 float featureId 编码为 RGBA8 (0-255)
vec4 encodeFeatureId(float id) {
    uint i = uint(id);
    return vec4(
        float((i >> 24) & 0xFFu) / 255.0,
        float((i >> 16) & 0xFFu) / 255.0,
        float((i >> 8)  & 0xFFu) / 255.0,
        float(i         & 0xFFu) / 255.0
    );
}
```

C++ 侧通过 `glReadPixels` 或 MTLTexture `getBytes` 读取 RGBA，解码还原 featureId。

## 光照模型

基础光照使用简化的 Blinn-Phong 模型（适用于 lit material）：

- **太阳方向**：作为 uniform 传入（在 FrameUniforms 或单独的 LightUniforms 中）。
- **环境光**：`ambientColor = vec3(0.15, 0.15, 0.2)`（夜间降低）。
- **漫反射**：`diffuse = max(dot(normal, sunDir), 0.0) * sunColor`。
- **镜面反射**：`specular = pow(max(dot(halfVec, normal), 0.0), shininess) * sunColor`。
- **无自发光**（除非模型有 emissive texture）。

Unlit material（图标、业务颜色）不使用光照。

## 坐标空间约定

Shader 中使用的坐标空间：

```
ECEF (世界坐标) → camera-relative (shader 输入) → clip space (vertex output)
```

1. C++ 侧：`worldPos - cameraRelativeOrigin → cameraRelativePos`
2. 传入 shader 作为 `a_position`（vec3, float32）
3. Vertex shader：`u_modelViewProjection * vec4(a_position, 1.0) → clipPos`
4. `cameraRelativeOrigin` 在 `FrameUniforms` 中传递（调试用，通常 shader 不直接使用）

### 精度注意事项

- GPU 中所有位置均为 float32。
- 地球半径 ~6,371,000 米，float32 精度 ~0.5 米（在该量级）。
- camera-relative 将有效精度提升至 ~1 mm（地球表面附近）。
- 高空视角（地球完全可见）：cameraRelativeOrigin 设在相机附近 ~1,000 km，精度 ~0.1 m，可接受。

## 纹理规范

- 瓦片纹理：RGBA8 (sRGB) 或 RGB8 (sRGB)，256×256 或 512×512。
- 图标 atlas：RGBA8 (sRGB)，预乘 alpha，最大 4096×4096。
- 深度纹理：Depth32F (Metal) / GL_DEPTH_COMPONENT32F (GL ES)，等于视口尺寸。
- Picking framebuffer：RGBA8，等于视口尺寸。

### Mipmap

- 瓦片纹理：不生成 mipmap（tile 分辨率固定）。
- 图标 atlas：生成 mipmap，使用 `LinearMipmapLinear`。

## Shader 变体

引擎需要以下 shader 变体组合：

| 用途 | Vertex | Fragment | Pass |
|------|--------|----------|------|
| Globe mesh (no texture) | globe.vert | globe.frag | color |
| Globe mesh (picking) | globe.vert | picking.frag | picking |
| Imagery tile (textured) | tile.vert | tile.frag | color |
| Vector polygon (filled) | vector.vert | polygon.frag | color |
| Vector line | vector.vert | line.frag | color |
| Vector point | vector.vert | point.frag | color |
| Label (billboard) | label.vert | label.frag | color |
| Debug overlay (wireframe) | debug.vert | debug.frag | color |

每个变体需要 MSL (`.metal`) 和 GLSL ES 3.0 (`.glsl`) 两个版本。Shader 编译管线见 `shader-compilation.md`。

## 验证

- MSL 和 GLSL ES shader 在相同的输入下产生相同的像素输出（截图对比验证）。
- Feature ID 编码/解码往返无误（单元测试）。
- 各 pass 的 framebuffer 状态（depth write、blend mode）与 shader 行为一致。
- TBDR 友好：避免在片段着色器中使用 discard；使用 `[[early_fragment_tests]]`（MSL）加速 depth prepass。
