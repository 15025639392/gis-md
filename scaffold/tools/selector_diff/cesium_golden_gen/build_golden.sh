#!/usr/bin/env bash
# 构建并运行 cesium golden 轨迹生成器，产出 golden/s1.trace（见 ../DESIGN.md）。
# 可重复执行；依赖本地 cesium-native checkout（bfc2c574c）与其
# build-selectordiff 里已装好的 vcpkg 依赖。
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SELECTOR_DIFF_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

CESIUM_NATIVE_DIR="${CESIUM_NATIVE_DIR:-/Users/ldy/Desktop/work/cesium-native}"
BUILD_DIR="${BUILD_DIR:-${CESIUM_NATIVE_DIR}/build-golden-gen}"
VCPKG_TOOLCHAIN="${VCPKG_TOOLCHAIN:-/Users/ldy/Desktop/work/globe/third_party/vcpkg/scripts/buildsystems/vcpkg.cmake}"
VCPKG_INSTALLED="${VCPKG_INSTALLED:-${CESIUM_NATIVE_DIR}/build-selectordiff/vcpkg_installed}"

# cmake/ninja 来自 scaffold/env.sh（Android SDK 内置版本）
# shellcheck disable=SC1091
source "${SELECTOR_DIFF_DIR}/../../env.sh"

cmake -S "${SCRIPT_DIR}" -B "${BUILD_DIR}" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="${VCPKG_TOOLCHAIN}" \
    -DVCPKG_MANIFEST_MODE=ON \
    -DVCPKG_MANIFEST_DIR="${CESIUM_NATIVE_DIR}" \
    -DVCPKG_INSTALLED_DIR="${VCPKG_INSTALLED}" \
    -DCESIUM_NATIVE_DIR="${CESIUM_NATIVE_DIR}" \
    -DCESIUM_USE_EZVCPKG=OFF \
    -DCESIUM_TESTS_ENABLED=OFF \
    -DCESIUM_ENABLE_CLANG_TIDY=OFF

cmake --build "${BUILD_DIR}" --target cesium_golden_gen

mkdir -p "${SELECTOR_DIFF_DIR}/golden"
"${BUILD_DIR}/cesium_golden_gen" "${SELECTOR_DIFF_DIR}/golden/s1.trace"
