# 测试覆盖缺口分析

**日期**：2026-06-28
**状态**：待改进
**优先级**：高

---

## 问题描述

全量测试（134/134 通过）未暴露以下严重问题：
1. Shader 纹理采样器超限（20 > 16）导致黑屏
2. 地形 glTF 管线未完成导致瓦片不渲染
3. FPS 低（~35 FPS）、卡顿、发烫、渲染严重不符合预期

---

## 根本原因

### 测试环境与真实设备差异

| 方面 | 测试环境 | 真实设备 |
|---|---|---|
| Shader 编译 | ❌ `createShader()` 返回 nullptr | ✅ 实际编译，检查 GL 限制 |
| GPU 资源创建 | ❌ 返回 DummyTexture/DummyBuffer | ✅ 真实 GL 对象 |
| 纹理单元限制 | ❌ 不检查 | ✅ GPU 驱动强制执行（16） |
| 渲染管线 | ❌ 部分验证 | ✅ 完整端到端 |
| 性能特征 | ❌ 不测量 | ✅ 60 FPS 目标 |
| GPU 热管理 | ❌ 不存在 | ✅ 会热降频 |
| 内存带宽 | ❌ 无感知 | ✅ 移动端受限 |

### Mock 代码示例

```cpp
// test_tile_surface.cpp
class RecordingRenderDevice final : public RenderDevice {
    std::unique_ptr<ShaderProgram> createShader(const ShaderDesc&) override {
        return nullptr;  // ← 跳过 shader 编译
    }
    std::unique_ptr<Texture> createTexture(const TextureDesc&) override {
        return std::make_unique<DummyTexture>(...);  // ← 假纹理
    }
    std::unique_ptr<Buffer> createBuffer(const BufferDesc&) override {
        return std::make_unique<DummyBuffer>(...);   // ← 假缓冲区
    }
};
```

---

## 测试覆盖范围

```
当前覆盖：
├── 核心数学（vec3, mat4, ray, box）✅
├── 地形选择逻辑（SSE pipeline）✅
├── 瓦片生命周期（加载/卸载）✅
├── 栅格叠加映射 ✅
├── 地形 Provider 配置 ✅
│
未覆盖：
├── Shader 编译验证 ❌
├── GL 纹理单元限制检查 ❌
├── GPU 资源创建验证 ❌
├── 完整渲染管线 ❌
├── 硬件兼容性 ❌
└── 端到端渲染验证 ❌
```

---

## 改进建议

### 1. 添加 Shader 编译验证测试

**目标**：在 CI 中验证 shader 能在目标 GPU 上编译

**方案**：
- 使用 ANGLE（OpenGL ES 模拟器）创建软件渲染上下文
- 编译所有 shader 并检查错误
- 验证纹理采样器数量不超过设备限制

```cpp
// 伪代码
TEST(ShaderCompilation, AllShadersCompile) {
    ANGLERenderDevice device;
    auto shaders = Renderer::getAllShaderDescs();
    for (const auto& desc : shaders) {
        auto program = device.createShader(desc);
        ASSERT_NE(nullptr, program) << "Shader failed: " << desc.name;
    }
}

TEST(ShaderCompilation, TextureSamplerLimit) {
    ANGLERenderDevice device;
    GLint maxUnits;
    glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &maxUnits);
    auto shaders = Renderer::getAllShaderDescs();
    for (const auto& desc : shaders) {
        ASSERT_LE(desc.samplerCount, maxUnits)
            << "Shader exceeds texture limit: " << desc.name;
    }
}
```

### 2. 添加纹理单元限制检查

**目标**：运行时检查 shader 纹理采样器数量

**方案**：
- 在 `Renderer::initialize()` 中查询 `GL_MAX_TEXTURE_IMAGE_UNITS`
- 如果 shader 采样器超过限制，记录错误并降级

```cpp
// 伪代码
bool Renderer::initialize(const GlobeMesh& mesh) {
    GLint maxTextureUnits;
    glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &maxTextureUnits);

    // 检查 glTF shader 采样器数量
    if (kGltfShaderSamplerCount > maxTextureUnits) {
        LOGE("glTF shader requires %d texture units, device supports %d",
             kGltfShaderSamplerCount, maxTextureUnits);
        // 使用精简版 shader 或返回错误
        return false;
    }
    ...
}
```

### 3. 添加真机集成测试

**目标**：在真实 Android 设备上验证渲染

**方案**：
- 使用 Android Espresso/UI Automator 进行 UI 测试
- 截图验证渲染结果
- 检查 logcat 中的错误日志

```java
// 伪代码
@Test
public void testTerrainRendering() {
    launchActivity(MainActivity.class);
    waitForIdle(5000);  // 等待地形加载

    // 检查 logcat 中没有 shader 错误
    assertNoLogcatErrors("GLES");

    // 检查诊断信息
    String diag = getDiagnosticsString();
    assertThat(diag).contains("entries>0");
    assertThat(diag).contains("surface>1");
}
```

### 4. 改进 Mock RenderDevice

**目标**：让 mock 更接近真实设备行为

**方案**：
- 在 mock 中验证 shader 源码语法
- 检查纹理采样器声明数量
- 记录 API 调用以便验证

```cpp
// 改进的 mock
class ValidatingRenderDevice final : public RenderDevice {
    std::unique_ptr<ShaderProgram> createShader(const ShaderDesc& desc) override {
        // 验证 shader 源码语法
        validateShaderSource(desc.vertexSource);
        validateShaderSource(desc.fragmentSource);

        // 检查纹理采样器数量
        int samplerCount = countSamplers(desc.fragmentSource);
        ASSERT_LE(samplerCount, 16) << "Too many texture samplers";

        return std::make_unique<DummyShader>();
    }
};
```

### 5. 添加渲染管线端到端测试

**目标**：验证从地形加载到渲染命令生成的完整流程

**方案**：
- 使用软件渲染器（如 Mesa/llvmpipe）
- 加载测试地形数据
- 验证渲染命令生成

```cpp
// 伪代码
TEST(RenderPipeline, TerrainGeneratesRenderCommands) {
    SoftwareRenderDevice device;
    Tileset tileset = createTestTileset(device);
    RenderCommandList commands;

    // 模拟几帧渲染
    for (int i = 0; i < 10; ++i) {
        tileset.update(testFrameState);
        tileset.buildRenderCommands(testRenderer, commands);
    }

    // 验证生成了地形渲染命令
    int terrainCmds = countCommands(commands, RenderCommandKind::GltfPrimitive);
    ASSERT_GT(terrainCmds, 0) << "No terrain render commands generated";
}
```

---

## 实施优先级

| 优先级 | 改进项 | 工作量 | 影响 |
|---|---|---|---|
| P0 | 纹理单元限制检查（运行时） | 小 | 防止黑屏 |
| P0 | Shader 编译验证（CI） | 中 | 防止编译失败 |
| P1 | 改进 Mock RenderDevice | 中 | 提高测试质量 |
| P1 | 真机集成测试 | 大 | 验证完整渲染 |
| P2 | 渲染管线端到端测试 | 大 | 验证数据流 |

---

## 相关文件

- `scaffold/tests/unit/tiling/test_tile_surface.cpp` — Mock RenderDevice
- `scaffold/src/earth_engine/renderer/Renderer.cpp` — Shader 源码
- `scaffold/src/earth_engine/platform/android/RenderDeviceGLES.cpp` — GLES 实现
- `scaffold/examples/android/MinimalGlobe/GLESView.cpp` — Android 渲染入口

---

## 总结

测试未暴露问题的根本原因是 **Mock 过度简化**，跳过了 shader 编译和 GPU 资源创建等关键环节。改进方向是：

1. **短期**：添加运行时纹理单元限制检查（P0）
2. **中期**：在 CI 中使用 ANGLE 验证 shader 编译（P0）
3. **长期**：添加真机集成测试和端到端渲染测试（P1-P2）
