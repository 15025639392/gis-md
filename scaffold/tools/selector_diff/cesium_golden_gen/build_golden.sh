#!/usr/bin/env bash
# 构建并运行 cesium golden 轨迹生成器，产出 golden/s1.trace + s2.trace
# （见 ../DESIGN.md）。可重复执行；依赖本地 cesium-native checkout
# （bfc2c574c）与其 build-selectordiff 里已装好的 vcpkg 依赖。
#
# 安全网：
#   - 生成先落 staging，跑两遍 diff 验确定性；
#   - s1.trace 若已入库则做 byte 级回归比对，不一致立即报错且不覆盖；
#   - s2 校验至少一帧 kicked>0（restore/kick 面被覆盖的证据），
#     全零只告警（报告概览），不自行改场景参数。
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SELECTOR_DIFF_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
GOLDEN_DIR="${SELECTOR_DIFF_DIR}/golden"
SCENARIOS=(s1 s2)

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

# ---- 生成（staging，两遍验确定性）----
# 注意：比对一律用绝对路径 /usr/bin/diff——本机 PATH 里有一个 OpenHarmony
# 工具链的 diff，不认 -q 还对错误参数退 0，会让校验静默假通过。
DIFF=/usr/bin/diff
STAGE_A="${BUILD_DIR}/golden-stage-a"
STAGE_B="${BUILD_DIR}/golden-stage-b"
rm -rf "${STAGE_A}" "${STAGE_B}"
mkdir -p "${STAGE_A}" "${STAGE_B}"

"${BUILD_DIR}/cesium_golden_gen" "${STAGE_A}"
"${BUILD_DIR}/cesium_golden_gen" "${STAGE_B}" > /dev/null

for scenario in "${SCENARIOS[@]}"; do
    if ! "${DIFF}" -q "${STAGE_A}/${scenario}.trace" "${STAGE_B}/${scenario}.trace" > /dev/null; then
        echo "ERROR: ${scenario}.trace 两次运行不一致（determinism 失败），不覆盖 golden" >&2
        exit 1
    fi
done
echo "determinism OK (${SCENARIOS[*]})"

# ---- s1 回归保证：与已入库版本 byte 级一致，否则报错且不覆盖 ----
if [ -f "${GOLDEN_DIR}/s1.trace" ]; then
    if ! "${DIFF}" "${GOLDEN_DIR}/s1.trace" "${STAGE_A}/s1.trace"; then
        echo "ERROR: 新生成的 s1.trace 与入库版本不一致（回归！），已停止，未覆盖任何 golden 文件" >&2
        exit 1
    fi
    echo "s1.trace 回归比对 OK（与入库版本 byte 级一致）"
fi

# ---- s2 验收：至少一帧 kicked > 0（restore/kick 面被触发的证据）----
if ! grep -q "kicked=[1-9]" "${STAGE_A}/s2.trace"; then
    echo "WARNING: s2.trace 全部帧 kicked=0，未覆盖 restore/kick 面；每帧概览见上方 s2 输出" >&2
fi

# ---- 落库 ----
mkdir -p "${GOLDEN_DIR}"
for scenario in "${SCENARIOS[@]}"; do
    cp "${STAGE_A}/${scenario}.trace" "${GOLDEN_DIR}/${scenario}.trace"
done
echo "golden updated: ${SCENARIOS[*]/%/.trace}"
