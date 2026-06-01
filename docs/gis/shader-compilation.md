# Shader 编译管线

本文件定义如何将 MSL (`.metal`) 和 GLSL ES 3.0 (`.glsl`) shader 源码编译为引擎可加载的 GPU 程序。与 `shader-interface.md` 配合使用。

## 策略总览

| 平台 | Shader 语言 | 编译方式 | 编译时机 | 产物 |
|------|-------------|----------|----------|------|
| iOS | MSL (Metal Shading Language) | Xcode Metal compiler (`metal`) | 构建时（预编译） | `.metallib` (Metal Library) |
| Android (GL ES) | GLSL ES 3.0 | OpenGL ES runtime (`glCompileShader`) | 运行时（首次使用时编译，缓存二进制） | GPU 厂商二进制 |
| Android (Vulkan) | SPIR-V | `glslangValidator` 或 `glslc` | 构建时（预编译） | `.spv` |

## iOS: MSL → .metallib

### 编译命令

```bash
# 编译单个 .metal 文件为 .air (Apple Intermediate Representation)
xcrun -sdk iphoneos metal -c -o globe.air globe.metal

# 将多个 .air 打包为 .metallib (Metal Library)
xcrun -sdk iphoneos metallib -o shaders.metallib globe.air tile.air vector.air
```

### CMake 集成

```cmake
# 在 examples/ios/CMakeLists.txt 中
set(SHADER_SOURCES
    shaders/globe.metal
    shaders/tile.metal
    shaders/vector.metal
    shaders/picking.metal
)

set(METALLIB_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/shaders.metallib")

add_custom_command(
    OUTPUT ${METALLIB_OUTPUT}
    COMMAND xcrun -sdk iphoneos metal
        -c -o ${CMAKE_CURRENT_BINARY_DIR}/shaders.air
        ${SHADER_SOURCES}
    COMMAND xcrun -sdk iphoneos metallib
        -o ${METALLIB_OUTPUT}
        ${CMAKE_CURRENT_BINARY_DIR}/shaders.air
    DEPENDS ${SHADER_SOURCES}
    COMMENT "Compiling Metal shaders → shaders.metallib"
)

add_custom_target(compile_shaders DEPENDS ${METALLIB_OUTPUT})
add_dependencies(MinimalGlobe compile_shaders)
```

### 运行时加载

```objc
// RenderDeviceMetal.mm
NSError *error = nil;
NSString *path = [[NSBundle mainBundle] pathForResource:@"shaders" ofType:@"metallib"];
id<MTLLibrary> library = [device newLibraryWithFile:path error:&error];
id<MTLFunction> vertexFunc = [library newFunctionWithName:@"globe_vertex"];
id<MTLFunction> fragmentFunc = [library newFunctionWithName:@"globe_fragment"];
```

## Android (GL ES): 运行时编译

### Runtime 编译

GLSL ES 在运行时通过 `glCompileShader` 编译：

```cpp
// RenderDeviceGLES.cpp
GLuint compileShader(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    GLint compiled = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        GLint infoLen = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &infoLen);
        std::string log(infoLen, '\0');
        glGetShaderInfoLog(shader, infoLen, nullptr, log.data());
        LOGE("Shader compile error: %s", log.c_str());
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}
```

### Shader 源码管理

GLSL ES 源码嵌入 C++ 代码为字符串常量，或从 assets 读取：

```cpp
// 方式 A：嵌入为 const char*（简单场景、少量 shader）
static const char* kGlobeVertexGLSL = R"glsl(
    #version 300 es
    layout(location = 0) in vec3 a_position;
    layout(location = 1) in vec3 a_normal;
    layout(location = 2) in vec2 a_texcoord;
    // ...
)glsl";

// 方式 B：从 APK assets 读取（大量 shader，便于独立维护）
// Android: AAssetManager_open(assetMgr, "shaders/globe.vert.glsl", AASSET_MODE_BUFFER)
```

### 程序二进制缓存

GL ES 3.0 支持 `glGetProgramBinary` / `glProgramBinary`，可在首次编译后缓存 GPU 厂商二进制：

```cpp
// 编译后保存
GLint binaryLength = 0;
glGetProgramiv(program, GL_PROGRAM_BINARY_LENGTH, &binaryLength);
std::vector<uint8_t> binary(binaryLength);
GLenum binaryFormat;
glGetProgramBinary(program, binaryLength, nullptr, &binaryFormat, binary.data());
// 保存 binaryFormat + binary 到磁盘缓存

// 下次启动时加载
glProgramBinary(program, binaryFormat, binary.data(), binaryLength);
```

缓存 key = shader 源码 hash + GPU 型号。注意：GPU 厂商二进制在不同设备/驱动版本间不兼容。

## Android (Vulkan): GLSL → SPIR-V

### 预编译

```bash
# 使用 glslangValidator
glslangValidator -V globe.vert.glsl -o globe.vert.spv
glslangValidator -V globe.frag.glsl -o globe.frag.spv

# 或使用 glslc (shaderc)
glslc -fshader-stage=vert globe.vert.glsl -o globe.vert.spv
glslc -fshader-stage=frag globe.frag.glsl -o globe.frag.spv
```

### CMake 集成

```cmake
find_program(GLSLANG_VALIDATOR glslangValidator REQUIRED)

add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/shaders/globe.vert.spv
    COMMAND ${GLSLANG_VALIDATOR} -V ${CMAKE_CURRENT_SOURCE_DIR}/shaders/globe.vert.glsl
        -o ${CMAKE_CURRENT_BINARY_DIR}/shaders/globe.vert.spv
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/shaders/globe.vert.glsl
    COMMENT "Compiling SPIR-V: globe.vert"
)
```

### 运行时加载

SPIR-V 二进制直接传给 `vkCreateShaderModule`：

```cpp
vkCreateShaderModule(device, &createInfo, nullptr, &shaderModule);
```

## Shader 目录结构

```text
shaders/
  msl/                        # Metal Shading Language (iOS)
    globe.metal
    tile.metal
    vector.metal
    picking.metal
    label.metal
    debug.metal
  glsl/                       # GLSL ES 3.0 (Android GL ES)
    globe.vert.glsl
    globe.frag.glsl
    tile.vert.glsl
    tile.frag.glsl
    vector.vert.glsl
    polygon.frag.glsl
    line.frag.glsl
    point.frag.glsl
    picking.vert.glsl
    picking.frag.glsl
    label.vert.glsl
    label.frag.glsl
    debug.vert.glsl
    debug.frag.glsl
```

每种 shader 变体（见 `shader-interface.md` 的变体列表）都需要 MSL 和 GLSL ES 两个版本。

## 验收

- iOS: `xcrun metal` 编译无错误；运行时 `newLibraryWithFile` 不返回 nil。
- Android GL ES: `glCompileShader` 返回 `GL_COMPILE_STATUS` 成功。
- Android Vulkan: `glslangValidator -V` 编译无错误。
- 两个平台渲染相同场景的截图像素差 < 2%（光照模型和 MSAA 差异可接受）。
- Shader 二进制缓存命中率 > 0%（首次启动后缓存文件存在且可加载）。
