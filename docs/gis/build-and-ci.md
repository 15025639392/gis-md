# 构建与 CI

本文件定义地球引擎的 CMake 构建配置、跨平台 toolchain 及 CI/CD 流水线。

## 前置条件

- CMake 3.20+
- vcpkg（或 conan，等价替代）
- Xcode 15+（iOS 构建）
- Android NDK 26+（Android 构建）

## 安装依赖

```bash
# 克隆 vcpkg（如尚未安装）
git clone https://github.com/Microsoft/vcpkg.git ~/vcpkg
~/vcpkg/bootstrap-vcpkg.sh

# 安装依赖（iOS arm64）
~/vcpkg/vcpkg install glm nlohmann-json curl gtest stb \
    --triplet arm64-ios

# 安装依赖（Android arm64-v8a）
~/vcpkg/vcpkg install glm nlohmann-json curl gtest stb \
    --triplet arm64-android
```

## 构建命令

### macOS 桌面（开发调试）

```bash
cmake -B build/desktop -S scaffold \
    -DCMAKE_TOOLCHAIN_FILE=~/vcpkg/scripts/buildsystems/vcpkg.cmake \
    -DVCPKG_TARGET_TRIPLET=arm64-osx \
    -DBUILD_EXAMPLES=OFF

cmake --build build/desktop -- -j$(sysctl -n hw.logicalcpu)
cd build/desktop && ctest --output-on-failure
```

### iOS 模拟器

```bash
cmake -B build/ios-sim -S scaffold -G Xcode \
    -DCMAKE_TOOLCHAIN_FILE=~/vcpkg/scripts/buildsystems/vcpkg.cmake \
    -DVCPKG_TARGET_TRIPLET=arm64-ios \
    -DCMAKE_SYSTEM_NAME=iOS \
    -DCMAKE_OSX_SYSROOT=iphonesimulator \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 \
    -DBUILD_EXAMPLES=ON

cmake --build build/ios-sim --config Debug -- -sdk iphonesimulator
```

### iOS 真机

```bash
cmake -B build/ios -S scaffold -G Xcode \
    -DCMAKE_TOOLCHAIN_FILE=~/vcpkg/scripts/buildsystems/vcpkg.cmake \
    -DVCPKG_TARGET_TRIPLET=arm64-ios \
    -DCMAKE_SYSTEM_NAME=iOS \
    -DCMAKE_OSX_SYSROOT=iphoneos \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0

cmake --build build/ios --config Release -- -sdk iphoneos
```

### Android

```bash
# 设置 ANDROID_NDK 环境变量
export ANDROID_NDK=$HOME/Android/Sdk/ndk/26.1.10909125

cmake -B build/android-arm64 -S scaffold \
    -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake \
    -DANDROID_ABI=arm64-v8a \
    -DANDROID_PLATFORM=android-26 \
    -DANDROID_STL=c++_shared \
    -DCMAKE_TOOLCHAIN_FILE_HOST=~/vcpkg/scripts/buildsystems/vcpkg.cmake \
    -DVCPKG_TARGET_TRIPLET=arm64-android

cmake --build build/android-arm64 -- -j$(nproc)
```

## CI 流水线 (GitHub Actions)

### 触发条件

- PR 到 `main` 分支
- 推送到 `main` 分支
- 每日定时构建（凌晨 3:00 UTC）

### 矩阵策略

```yaml
strategy:
  matrix:
    platform:
      - { os: macos-14, target: ios-sim,    triplet: arm64-ios }
      - { os: macos-14, target: macos-test,  triplet: arm64-osx }
      - { os: ubuntu-22.04, target: android-arm64, triplet: arm64-android }
```

### 步骤

1. **Checkout** — `actions/checkout@v4`
2. **Install vcpkg** — 缓存 `~/vcpkg` 目录
3. **Install dependencies** — `vcpkg install <packages> --triplet ${{ matrix.triplet }}`
4. **Configure** — 按上述构建命令执行 cmake configure
5. **Build** — `cmake --build`
6. **Test (desktop)** — `ctest --output-on-failure`（桌面平台直接运行）
7. **Test (iOS sim)** — `xcodebuild test -scheme earth_engine -destination 'platform=iOS Simulator,name=iPhone 15 Pro'`（需额外配置 XCTest target）
8. **Archive logs** — 构建日志和测试结果

### 缓存策略

- `vcpkg` 安装的包缓存 7 天，按 triplet hash 区分
- CMake build 目录不缓存（每次完整构建确保可复现）

## 常见构建问题

### 问题：iOS 链接时找不到 Metal framework

确保 CMake 中 `target_link_libraries` 包含 `"-framework Metal"`。

### 问题：Android NDK 版本不匹配

检查 `ANDROID_NDK` 路径，确保使用 NDK 26+。`android.toolchain.cmake` 路径因 NDK 版本而异。

### 问题：vcpkg triplet 不存在

使用 `vcpkg help triplet` 查看可用 triplet。iOS 使用 `arm64-ios`，Android 使用 `arm64-android`。

### 问题：桌面构建 GLM 找不到

GLM 是 header-only 库，通常通过 vcpkg 安装后 `find_package(glm CONFIG)` 即可。如果使用系统包管理器安装，可能需要设置 `CMAKE_PREFIX_PATH`。
