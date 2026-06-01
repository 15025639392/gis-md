# 部署与分发

本文件定义如何将 `earth_engine_core` 打包为可分发的库产物（iOS: xcframework、Android: AAR），以及头文件导出和符号可见性策略。

## 产物矩阵

| 产物 | 平台 | 格式 | 用途 |
|------|------|------|------|
| `EarthEngine.xcframework` | iOS | xcframework (含 arm64 device + arm64 simulator) | 集成到 iOS app（Swift Package Manager / CocoaPods / 手动拖入） |
| `earth-engine.aar` | Android | AAR (含 arm64-v8a + armeabi-v7a .so) | 集成到 Android app（Gradle dependency） |
| `earth-engine` (header-only subset) | 桌面 | CMake `find_package` | macOS/Windows/Linux 开发调试 |

## iOS: xcframework

### 构建步骤

```bash
# 1. 构建 device slice (arm64)
cmake -B build/ios-device -S scaffold -G Xcode \
    -DCMAKE_TOOLCHAIN_FILE=~/vcpkg/scripts/buildsystems/vcpkg.cmake \
    -DVCPKG_TARGET_TRIPLET=arm64-ios \
    -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphoneos

cmake --build build/ios-device --config Release

# 2. 构建 simulator slice (arm64)
cmake -B build/ios-sim -S scaffold -G Xcode \
    -DCMAKE_TOOLCHAIN_FILE=~/vcpkg/scripts/buildsystems/vcpkg.cmake \
    -DVCPKG_TARGET_TRIPLET=arm64-ios \
    -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphonesimulator

cmake --build build/ios-sim --config Release

# 3. 创建 xcframework
xcodebuild -create-xcframework \
    -library build/ios-device/Release-iphoneos/libearth_engine_core.a \
    -library build/ios-sim/Release-iphonesimulator/libearth_engine_core.a \
    -output build/EarthEngine.xcframework
```

### 头文件

xcframework 需要包含公开头文件。在构建前将头文件复制到 staging 目录：

```bash
mkdir -p build/headers/earth_engine
cp -r scaffold/src/earth_engine/core build/headers/earth_engine/
cp -r scaffold/src/earth_engine/renderer build/headers/earth_engine/
cp -r scaffold/src/earth_engine/tiling build/headers/earth_engine/
cp -r scaffold/src/earth_engine/platform/bridge build/headers/earth_engine/
cp -r scaffold/src/earth_engine/threading build/headers/earth_engine/
```

### Swift Package Manager 集成

```swift
// Package.swift
let package = Package(
    name: "EarthEngine",
    platforms: [.iOS(.v15)],
    products: [
        .library(name: "EarthEngine", targets: ["EarthEngine"]),
    ],
    targets: [
        .binaryTarget(
            name: "EarthEngine",
            path: "build/EarthEngine.xcframework"
        ),
    ]
)
```

## Android: AAR

### 构建步骤

```bash
# 1. 构建 arm64-v8a
cmake -B build/android-arm64 -S scaffold \
    -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26
cmake --build build/android-arm64

# 2. 构建 armeabi-v7a（可选，覆盖低端设备）
cmake -B build/android-armv7 -S scaffold \
    -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake \
    -DANDROID_ABI=armeabi-v7a -DANDROID_PLATFORM=android-26
cmake --build build/android-armv7
```

### AAR 打包

AAR 是一个 ZIP 文件，包含 `.so` 库、AndroidManifest、可选的 Java/Kotlin 桥接代码和 ProGuard 规则。

```text
earth-engine.aar
├── jni/
│   ├── arm64-v8a/libearth_engine_core.so
│   └── armeabi-v7a/libearth_engine_core.so
├── AndroidManifest.xml
├── classes.jar              # Java/Kotlin 桥接层（JNI wrapper）
├── proguard.txt             # ProGuard/R8 规则
└── prefab/                  # Android Prefab (CMake find_package 支持)
    └── modules/
        └── earth_engine/
            ├── include/     # C++ 头文件
            └── module.json
```

### Gradle 集成

```groovy
// build.gradle
dependencies {
    implementation 'com.earthengine:earth-engine:0.1.0'
}
```

### Prefab 支持

Android Prefab 允许原生库通过 CMake `find_package` 被消费者使用：

```json
// prefab/modules/earth_engine/module.json
{
  "libraryName": "libearth_engine_core.so",
  "exportedHeaders": "include"
}
```

消费者的 `CMakeLists.txt`：

```cmake
find_package(earth_engine REQUIRED CONFIG)
target_link_libraries(my_app earth_engine::earth_engine)
```

## 符号可见性

### 公开 API

仅以下头文件中的符号为公开 API，保证前向兼容：

- `earth_engine/core/geodesy/Ellipsoid.h`
- `earth_engine/core/geodesy/Cartographic.h`
- `earth_engine/core/math/Vec3.h`
- `earth_engine/core/math/Mat4.h`
- `earth_engine/tiling/TileScheme.h`
- `earth_engine/tiling/TileKey.h`
- `earth_engine/renderer/RenderDevice.h`
- `earth_engine/renderer/RenderCommand.h`
- `earth_engine/platform/bridge/PlatformBridge.h`

内部实现细节（`*::Impl`、匿名命名空间内容、平台特定子目录）不保证 API 兼容。

### 符号导出

- iOS: 静态库默认导出所有符号。使用 `-fvisibility=hidden` + `__attribute__((visibility("default")))` 控制。
- Android: 使用 `-fvisibility=hidden`，JNI 函数用 `JNIEXPORT` 标记。

## 版本管理

- 语义化版本：`MAJOR.MINOR.PATCH`
- MAJOR：公开 API 不兼容变更（如删除类、修改虚函数签名）
- MINOR：新增公开 API（向后兼容）
- PATCH：Bug 修复、性能优化（不改变 API）
- 版本号在 `CMakeLists.txt` 的 `project(earth_engine VERSION x.y.z)` 中维护。

## 验收

- xcframework 可被 Xcode 项目导入并链接成功。
- AAR 可被 Android Gradle 项目导入，`System.loadLibrary("earth_engine_core")` 成功。
- 公开头文件可被消费者 `#include` 且不产生编译错误。
- 符号可见性：内部符号（Pimpl、匿名命名空间）不被导出到动态符号表。
