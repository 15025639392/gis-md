# Android 黑屏问题：纹理采样器超限

**日期**：2026-06-28
**状态**：✅ 已修复（方案 1）
**影响**：所有 Android 设备（特别是 Adreno/Mali GPU）

---

## 问题描述

Android MinimalGlobe Demo 启动后黑屏，logcat 显示：

```
E GLES: Program link error: Error: Sampler Sampler location or component exceeds max allowed.
E GLES: Error: Linking failed.
```

## 根本原因

glTF shader 声明了 **20 个纹理采样器**，但大多数 Android GPU 只支持 **16 个纹理单元**（`GL_MAX_TEXTURE_IMAGE_UNITS = 16`）。

### 错误链

```
glTF shader 链接失败（20 > 16）
  → Renderer::initialize() 返回 false
  → surfaceCreated_ = false
  → Engine::render() 每帧 early return
  → beginFrame() 不执行，不清除帧缓冲
  → eglSwapBuffers 交换黑色缓冲 → 黑屏
```

### 纹理采样器分配（修复前）

| 类别 | 采样器 | 数量 |
|---|---|---|
| PBR 基础 | baseColor, metallicRoughness, normal, occlusion, emissive | 5 |
| PBR 扩展 | anisotropy, specular, specularColor, specularGlossiness, transmission, clearcoat, clearcoatRoughness, clearcoatNormal, sheenColor, sheenRoughness | 10 |
| 栅格叠加 | mappedRasterTexture0-3 | 4 |
| 水印 | gltfWaterMaskTexture | 1 |
| **总计** | | **20** |

---

## 解决方案清单

### 方案 1：移除 PBR 扩展纹理（已实施 ✅）

**改动**：注释掉 glTF shader 中 10 个 PBR 扩展纹理的声明和采样代码

**影响的功能**：
- ❌ Anisotropy（拉丝金属、头发）— KHR_materials_anisotropy
- ❌ Specular workflow（镜面反射控制）— KHR_materials_specular
- ❌ Specular-Glossiness workflow（旧版 PBR）— KHR_materials_pbrSpecularGlossiness
- ❌ Transmission（玻璃、半透明）— KHR_materials_transmission
- ❌ Clearcoat（车漆、清漆层）— KHR_materials_clearcoat
- ❌ Sheen（布料、天鹅绒）— KHR_materials_sheen

**保留的功能**：
- ✅ baseColor（基础颜色贴图）
- ✅ metallicRoughness（金属度/粗糙度）
- ✅ normal（法线贴图）
- ✅ occlusion（环境光遮蔽）
- ✅ emissive（自发光）
- ✅ 栅格叠加（卫星影像/路网）
- ✅ 水印

**适用场景**：地形/卫星影像渲染、简单 3D 模型

---

### 方案 2：运行时检测 + 多 shader 变体（推荐长期方案）

**思路**：查询设备 `GL_MAX_TEXTURE_IMAGE_UNITS`，根据能力选择 shader 变体

```cpp
GLint maxTextureUnits;
glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &maxTextureUnits);

if (maxTextureUnits >= 20) {
    // 完整版 shader（支持所有 PBR 扩展）
    impl_->gltfShader = dev->createShader(gltfFullSd);
} else {
    // 精简版 shader（仅核心 PBR）
    impl_->gltfShader = dev->createShader(gltfReducedSd);
}
```

**优点**：高端设备完整支持，低端设备自动降级
**缺点**：需维护两套 shader 源码

---

### 方案 3：纹理打包（Channel Packing）

**思路**：将多个单通道纹理合并到一个 RGBA 纹理的不同通道

```
原来：3 个纹理
  - metallicRoughness (R=金属度, G=粗糙度)
  - occlusion (R=AO)
  - emissive (RGB)

打包后：2 个纹理
  - metallicRoughnessOcclusion (R=金属度, G=粗糙ness, B=AO)
  - emissive (RGB)
```

**优点**：减少 1-2 个采样器，保持功能完整
**缺点**：需修改 glTF 加载器和 shader

---

### 方案 4：减少栅格叠加槽位

**思路**：当前配置 4 个槽位（`kMaxGltfRasterOverlays = 4`），实际只用 2 个

```cpp
static constexpr int kMaxGltfRasterOverlays = 2;  // 从 4 减到 2
```

**优点**：改动极小
**缺点**：仅节省 2 个采样器（20→18），仍超限

---

### 方案 5：使用 UBO 存储材质参数

**思路**：将材质参数（颜色、系数等）存入 Uniform Buffer Object，仅保留必要的纹理

**优点**：更现代的做法，减少采样器依赖
**缺点**：改动大，仍需纹理用于贴图

---

## 推荐实施路径

1. **短期（已完成）**：方案 1 — 注释掉 PBR 扩展，确保 demo 能跑
2. **中期**：方案 2 — 实现运行时检测，高端设备启用完整功能
3. **长期**：方案 3 + 方案 2 — 纹理打包 + 多 shader 变体，最大化兼容性

---

## 相关文件

- Shader 源码：`scaffold/src/earth_engine/renderer/Renderer.cpp`
- 纹理槽位常量：`scaffold/src/earth_engine/renderer/RenderCommand.h`
- GLES 渲染设备：`scaffold/src/earth_engine/platform/android/RenderDeviceGLES.cpp`
- Demo 配置：`scaffold/examples/android/MinimalGlobe/MinimalGlobeDemoConfig.cpp`

---

## 测试验证

修复后应验证：
- [x] Android 设备不再黑屏 ✅
- [x] 地形正常显示 ✅
- [x] 卫星影像正常加载 ✅
- [x] 路网叠加正常 ✅
- [x] 基础 PBR 材质正常（金属度、粗糙度、法线）✅
- [x] RobotExpressive.glb 模型正常显示 ✅

## 修复总结

**修改文件**：`scaffold/src/earth_engine/renderer/Renderer.cpp`

**修改内容**：
1. 注释掉 10 个 PBR 扩展纹理采样器声明
2. 注释掉相关采样代码块（sheen, clearcoat, specular-glossiness, transmission, anisotropy）
3. 注释掉 `perturbClearcoatNormal` 函数（使用了已禁用的纹理）
4. 将相关变量设置为默认值（0 或 vec3(0.0)）

**修改后采样器数量**：
- glTF shader: 20 → 10（减少 10 个）
- 总计：仍在 16 限制内

**测试结果**：
- Shader 编译成功
- 引擎初始化成功
- 渲染流畅（~60 FPS）
- 地形和影像正常加载
- 无 GL 错误
