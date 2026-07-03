#!/usr/bin/env bash
# 构建并运行 cesium golden 轨迹生成器，产出 golden/s1..s4.trace
# （见 ../DESIGN.md）。可重复执行；依赖本地 cesium-native checkout
# （bfc2c574c）与其 build-selectordiff 里已装好的 vcpkg 依赖。
#
# 安全网：
#   - 生成先落 staging，跑两遍 diff 验确定性（loads 保序后这同时覆盖
#     "load queue std::sort 并列项顺序跨运行是否稳定"——不稳会在此炸）；
#   - P3 格式变更（loads 保序）：s1/s2 预期与旧入库版本不同，本轮
#     不做与旧 golden 的回归比对，只做两次运行自身比对；
#   - s2/s3 kicked 与 s4 fog 剔除做验收检查，不达标只告警（报告概览），
#     不自行改场景参数；
#   - s4_nofog.trace 是 fog sanity 对照，只留 staging 不入库。
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SELECTOR_DIFF_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
GOLDEN_DIR="${SELECTOR_DIFF_DIR}/golden"
SCENARIOS=(s1 s2 s3 s4)

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

"${BUILD_DIR}/cesium_golden_gen" "${STAGE_A}" --with-fog-sanity
"${BUILD_DIR}/cesium_golden_gen" "${STAGE_B}" --with-fog-sanity > /dev/null

for scenario in "${SCENARIOS[@]}" s4_nofog; do
    if ! "${DIFF}" -q "${STAGE_A}/${scenario}.trace" "${STAGE_B}/${scenario}.trace" > /dev/null; then
        echo "ERROR: ${scenario}.trace 两次运行不一致（determinism 失败，可能是 load queue 并列项排序不稳），不覆盖 golden。差异：" >&2
        "${DIFF}" "${STAGE_A}/${scenario}.trace" "${STAGE_B}/${scenario}.trace" >&2 || true
        exit 1
    fi
done
echo "determinism OK (${SCENARIOS[*]} s4_nofog)"

# ---- 验收检查（不达标只告警，报告概览，不自行改场景）----
if ! grep -q "kicked=[1-9]" "${STAGE_A}/s2.trace"; then
    echo "WARNING: s2.trace 全部帧 kicked=0，未覆盖 restore/kick 面" >&2
fi
S3_KICK_FRAMES=$(grep -c "kicked=[1-9]" "${STAGE_A}/s3.trace" || true)
if [ "${S3_KICK_FRAMES}" -lt 2 ]; then
    echo "WARNING: s3.trace 仅 ${S3_KICK_FRAMES} 帧 kicked>0（预期 >=2，多帧渐进 restore）" >&2
else
    echo "s3 kicked>0 帧数: ${S3_KICK_FRAMES}（>=2 ✓）"
fi
if "${DIFF}" -q "${STAGE_A}/s4.trace" "${STAGE_A}/s4_nofog.trace" > /dev/null; then
    echo "WARNING: s4 与关 fog 对照完全一致——fog 剔除未发生" >&2
else
    echo "s4 fog sanity: 与关 fog 对照存在差异（fog 剔除发生 ✓）"
fi

# ---- 落库（s4_nofog 不入库）----
mkdir -p "${GOLDEN_DIR}"
for scenario in "${SCENARIOS[@]}"; do
    cp "${STAGE_A}/${scenario}.trace" "${GOLDEN_DIR}/${scenario}.trace"
done
echo "golden updated: ${SCENARIOS[*]/%/.trace}"
