#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""管线特性契约守卫:同一特性在逐瓦/合批/instanced × GLES/Metal 多条管线
各有一份数据契约(per-draw uniform vs 实例流 vs MSL 镜像结构)。历史上已两次
在单条管线上静默漏配(合批漏拷场 uniform 9043e20fe;位移路径几何 UV 喂
details 标定 80892aca1)——症状随管线选择状态时隐时现,肉眼极难归因。

本守卫做四类字符串级检查(不编译 shader,只对源码做存在性/一致性断言):
  A 单一治理点:页存储采样只允许存在于 PageStoreSamplingGLSL.h,六个消费
    shader 一律经 eePageStoreCompose 调用;Renderer.cpp 里再次内联采样链
    (fingerprint: 祖先 scale-bias 公式)即 FAIL。
  B 特性搬运完备:页存储相关 uniform,在「uniform 表 / applyToTerrainCommand
    写入 / 合批实例流拷贝」三处都必须有搬运语句——漏一处 = 9043e20fe 重演。
  C 相位打包口径:params.w 的 8/512 打包与解包常数在打包端(CPU)与解包端
    (shader×2 + batcher)同时存在——单侧改常数即 FAIL。
  D MSL 镜像结构同序:GltfUniformBlock(C++)按 memcpy 语义整块喂 Metal,
    Renderer.cpp 两份 MSL `struct GltfUniforms` 的字段序列必须与 C++ 逐一
    同名同序——插错位 = 全部后续 uniform 静默错位。

⚠️ 与 AI_INDEX 守卫同性质的局限:只查"搬运语句在不在",不查语义是否正确。
"""

import argparse
import re
import sys


def read(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


FAILURES = []


def fail(msg):
    FAILURES.append(msg)


def require(text, needle, where, why):
    if needle not in text:
        fail(f"[{where}] 缺少 `{needle}` —— {why}")


def require_count(text, needle, expect, where, why):
    n = text.count(needle)
    if n != expect:
        fail(f"[{where}] `{needle}` 出现 {n} 次,期望 {expect} —— {why}")


def check_single_source(renderer, sampling_h):
    # A1 治理点自身:GLSL/MSL 双份函数体都在
    require(sampling_h, "kPageStoreSamplingGLSL", "PageStoreSamplingGLSL.h",
            "GLSL 采样函数源丢失")
    require(sampling_h, "kPageStoreSamplingMSL", "PageStoreSamplingGLSL.h",
            "MSL 采样函数源丢失")
    # A2 六个消费 shader 都走函数调用(GLSL gltf/terrain/instanced +
    # MSL gltf/terrain/instanced)
    require_count(renderer, "base = eePageStoreCompose(", 6, "Renderer.cpp",
                  "页存储采样消费点应恰为 6 个 shader;增删管线须同步本清单")
    # A3 注入器对四个「当前会编译」的装配点生效(gltf/gltfInstanced/terrain/
    # terrainInstanced;Metal instanced 待 Step 4)
    require_count(renderer, "= withPageStoreSampling(", 4, "Renderer.cpp",
                  "装配点应恰 4 处(gltf/gltfInstanced/terrain/terrainInstanced"
                  " 的 GLES 编译路径);新增消费 shader 装配时必须包一层")
    # A4 禁止再内联:祖先 scale-bias 公式只允许活在治理点里
    fingerprint = "floor(gGlobal / span) * span"
    require_count(renderer, fingerprint, 0, "Renderer.cpp",
                  "采样链被重新内联进 shader —— 必须收回 PageStoreSamplingGLSL.h")
    require_count(sampling_h, fingerprint, 2, "PageStoreSamplingGLSL.h",
                  "治理点里应有 GLSL+MSL 各一份祖先 scale-bias")


def check_feature_plumbing(page_store, batcher, uniform_h):
    # B 每个页存储特性:uniform 表 + applyToTerrainCommand + 合批搬运,三处齐
    features = [
        # (uniform 表条目, applyToTerrainCommand 写入指纹, 合批搬运指纹)
        ('EE_GLTF_ENTRY("u_pageGeomA"',
         "pageGeomA = {\n        binding.sample.geomAffine[0]",
         "m.terrainPageGeomAffine[0]"),
        ('EE_GLTF_ENTRY("u_pageGeomB"',
         "pageGeomB = {\n        binding.sample.geomAffine[4]",
         "m.terrainPageGeomAffine[4]"),
    ]
    for entry, apply_fp, batch_fp in features:
        require(uniform_h, entry, "GltfUniformBlock.h", "uniform 表条目缺失")
        require(page_store, apply_fp, "TerrainPageStore.cpp",
                "applyToTerrainCommand 未写入该特性")
        if batch_fp:
            require(batcher, batch_fp, "TerrainInstanceBatcher.cpp",
                    "合批实例流未搬运该特性(9043e20fe 同型漏配)")


def check_phase_packing(page_store, batcher, batcher_h, renderer):
    # C 相位打包口径:CPU 打包(8/512)↔ shader/batcher 解包(8/512)
    require(page_store, "8.0f * static_cast<float>(phaseX)",
            "TerrainPageStore.cpp", "params.w 相位打包(×8)丢失")
    require(page_store, "512.0f * static_cast<float>(phaseY)",
            "TerrainPageStore.cpp", "params.w 相位打包(×512)丢失")
    require(batcher, "std::floor(packed / 8.0f)", "TerrainInstanceBatcher.cpp",
            "实例流相位解包(÷8)与打包端口径脱节")
    require(batcher, "std::floor(packed / 512.0f)",
            "TerrainInstanceBatcher.cpp", "实例流相位解包(÷512)与打包端口径脱节")
    # shader 侧解包(gltf/terrain GLSL 各一 + MSL 各一 = 4 处)
    n = renderer.count("psPack / 8.0") + renderer.count("psPack / 8.0f")
    if n < 2:
        fail(f"[Renderer.cpp] shader 相位解包(÷8)只剩 {n} 处,应 ≥2(GLSL+MSL)")
    # C2 pageCellDesc 打包口径(cellsX +128·cellsY +16384·texSet):
    # 打包端(batcher)↔ 实例化 shader 解包端(GLES+MSL)。texSet 解包必须
    # fmod 8，避免高位污染 UV 集选择。
    n = renderer.count("floor(packed / 16384.0), 8.0)")
    if n != 2:
        fail(f"[Renderer.cpp] pageCellDesc texSet 解包(fmod 8)应恰 2 处"
             f"(GLES+MSL instanced),实为 {n}")


def msl_struct_fields(renderer, start_needle):
    i = renderer.find(start_needle)
    if i < 0:
        return None
    j = renderer.find("};", i)
    body = renderer[i:j]
    return re.findall(
        r"^\s+(?:float4x4|packed_float[234]|float)\s+([A-Za-z_]\w*)\s*;",
        body, re.M)


def cpp_struct_fields(uniform_h):
    i = uniform_h.find("struct alignas(16) GltfUniformBlock {")
    j = uniform_h.find("\n};", i)
    body = uniform_h[i:j]
    # 只取顶层缩进(4 空格)的标量/定长数组成员声明；嵌套的
    # TextureTransform 字段位于本守卫关注的 pageStore 尾段之前。
    fields = re.findall(
        r"^    (?:std::array<float, \d+>|float)\s+"
        r"([A-Za-z_]\w*)", body, re.M)
    return fields


def check_msl_mirrors(renderer, uniform_h):
    # D MSL 两份 GltfUniforms 镜像必须与 C++ 字段同名同序(memcpy 语义)。
    cpp = cpp_struct_fields(uniform_h)
    if not cpp or "pageStoreParams" not in cpp:
        fail("[GltfUniformBlock.h] C++ 字段解析失败(结构名/格式变了?守卫需跟修)")
        return
    starts = [m.start() for m in re.finditer(r"struct GltfUniforms \{", renderer)]
    if len(starts) != 2:
        fail(f"[Renderer.cpp] MSL `struct GltfUniforms` 应恰 2 份,实为 {len(starts)}")
        return
    for k, pos in enumerate(starts):
        msl = msl_struct_fields(renderer[pos:], "struct GltfUniforms {")
        # 只强校验从 pageStoreParams 起的**尾段**(页存储/仿射特性区)
        # 同名同序；前段材质纹理变换由独立表与 shader 编译守卫覆盖：
        # 这是历史上唯一发生过增删的活跃区,也是错位代价最高的区段。
        try:
            ci = cpp.index("pageStoreParams")
            mi = msl.index("pageStoreParams")
        except ValueError:
            fail(f"[Renderer.cpp] MSL GltfUniforms #{k+1} 缺 pageStoreParams")
            continue
        cpp_tail = cpp[ci:]
        msl_tail = msl[mi:mi + len(cpp_tail)]
        if cpp_tail != msl_tail:
            fail(f"[Renderer.cpp] MSL GltfUniforms #{k+1} 尾段字段序 ≠ C++:\n"
                 f"  C++: {cpp_tail}\n  MSL: {msl_tail}\n"
                 f"  (memcpy 语义下错一位 = 后续全部 uniform 静默串位)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", required=True)
    args = ap.parse_args()
    src = args.src.rstrip("/")

    renderer = read(f"{src}/earth_engine/renderer/Renderer.cpp")
    sampling_h = read(f"{src}/earth_engine/renderer/PageStoreSamplingGLSL.h")
    page_store = read(f"{src}/earth_engine/renderer/TerrainPageStore.cpp")
    batcher = read(f"{src}/earth_engine/renderer/TerrainInstanceBatcher.cpp")
    batcher_h = read(f"{src}/earth_engine/renderer/TerrainInstanceBatcher.h")
    uniform_h = read(f"{src}/earth_engine/renderer/GltfUniformBlock.h")

    check_single_source(renderer, sampling_h)
    check_feature_plumbing(page_store, batcher, uniform_h)
    check_phase_packing(page_store, batcher, batcher_h, renderer)
    check_msl_mirrors(renderer, uniform_h)

    if FAILURES:
        print(f"{len(FAILURES)} 处管线契约脱节:")
        for f in FAILURES:
            print("FAIL", f)
        print("\n修法:特性必须在全部管线契约点同步落位(uniform 表 / apply 写入 /"
              "\n实例流拷贝 / MSL 镜像),或有意变更时同步更新本守卫清单。")
        return 1
    print("管线特性契约:全部一致")
    return 0


if __name__ == "__main__":
    sys.exit(main())
