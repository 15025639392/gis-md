#!/usr/bin/env python3
"""GLSL ES 编译守卫(host 离屏 GL 前的第一道自动化锁,对应地形 T-P6 方案 B)。

## 为什么存在

地形/矢量/大气 shader 全是运行时 C++ 字符串,host ctest 从不编译它们,
GLSL 语法错只能真机肉眼验(T-P6)。历史上已兑现过一次后果:GPU 烘焙
`sampleH` 漏移植 CPU 的 no-data 角剔除,两份实现静默分叉很久无人发现
(T-V10 根因①)。本守卫把**主要 GLSL ES 源**(含注入后的最终形态)离线
交给 glslangValidator 编译,语法/接口错在 ctest 里直接炸,不用等真机。

## 它检查什么 —— 以及明确不检查什么

✅ 检查:
  - Renderer.cpp 主渲染 shader(gltf/terrain/terrainInstanced/vector/color/
    point/label/stencil),按 createShaders 的真实注入顺序装配:
    withTerrainLight / withPageStoreSampling / withSceneOutput / withGltfHdr。
  - LDR 默认注入(LDR 是生产主路径)+ 同名 shader 的 **HDR 注入变体**
    (kEnableHdrPipeline=true 的冻结态首次被自动化编译触碰,呼应 L-P3)。
  - 高度纹理烘焙 shader(TerrainHeightBakeShader.h,CPU/GPU 分叉重灾区)。
  - SkyBox / FXAA / blit / bake PoC 等自包含 shader。

❌ 不检查(显式 SKIP,不静默):
  - 大气背景 pass / tonemap / aerial fog:由 C++ 生成函数
    (skyColorGLSLPreamble / aerialFogMathGLSL / pbrNeutralToneMappingGLSL)
    拼装,Python 侧复制生成逻辑会引入第二事实源 —— 留给 host 离屏 GL
    (T-P6 方案 A)整体覆盖;数值侧已有 computeSkyColorCpu 对拍(L-P4)。
  - MSL:需 Metal 编译器,glslang 不负责。
  - VtIndirectionSamplePoc:按 DESCENT 动态生成,非静态字面量。
  - 无 `void main(` 的 helper 块(注入后再验,不单独编译)。

## 用法

  check_glsl_compile.py --src <src 根目录> [--glslang <validator 路径>]

glslangValidator 定位顺序:--glslang > $GLSLANG_VALIDATOR > PATH >
scaffold/build/tools/glslang/bin(由 tools/fetch_glslang.sh 放置)。
找不到时打印 SKIP 并以 0 退出(与 check_hdr_variant_integrity 的
"没扫到比没有更糟"同哲学:SKIP 必须显式可见)。找到但任一 shader 编译失败
则以非 0 退出。
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

EXTS = (".cpp", ".h", ".mm")

# 注入 helper 的 LDR(生产默认)/HDR 字面量,与 Renderer.cpp 的
# withSceneOutput / withGltfHdr 完全同文(改那边必须同步这里,否则守卫
# 编译的就不是生产形态 —— 脚本头注释指向此处为唯一镜像点)。
SCENE_OUTPUT_LDR = "vec3 encodeSceneOutput(vec3 c){return c;}\n"
SCENE_OUTPUT_HDR = (
    "vec3 encodeSceneOutput(vec3 c){return pow(max(c,vec3(0.0)),vec3(2.2));}\n"
)
HDR_ALBEDO_LDR = "vec3 hdrAlbedo(vec3 c){return c;}\n"
HDR_ALBEDO_HDR = (
    "vec3 hdrAlbedo(vec3 c){return pow(max(c,vec3(0.0)),vec3(2.2));}\n"
)

# 需要额外编译一遍 HDR 注入变体的片元 shader(与 createShaders 的注入点对应)。
HDR_VARIANT_FRAGMENTS = (
    "kGltfFragmentGLSL",
    "kTerrainFragmentGLSL",
    "kTerrainInstancedFragmentGLSL",
    "kColorFragmentGLSL",
    "kVectorFillFragmentGLSL",
    "kVectorPageMeshFragmentGLSL",
    "kVectorLineFragmentGLSL",
    "kVectorLineStencilFragmentGLSL",
    "kVectorPointFragmentGLSL",
    "kVectorLabelFragmentGLSL",
)

# 显式 SKIP:结构上由 C++ 生成函数拼装或动态生成,见脚本头。
SKIP_NAMES = {
    "kAtmosphereBackgroundFragHead": "大气 pass 头,与 main 拼装(留 T-P6 方案 A)",
    "kAtmosphereBackgroundFragMain": "大气 pass 主段,依赖生成函数拼装(留方案 A)",
    "kAtmosphereComposeLdr": "大气合成段,依赖头+主段拼装(留方案 A)",
    "kAtmosphereComposeHdr": "大气合成段,依赖头+主段拼装(留方案 A)",
    "kTonemapFragHead": "tonemap 头,依赖 pbrNeutralToneMappingGLSL 拼装(留方案 A)",
    "kTonemapFragMain": "tonemap 主段,依赖生成函数拼装(留方案 A)",
    "kAerialFogFragHead": "aerial fog 头,依赖生成函数拼装(留方案 A)",
    "kAerialFogFragMain": "aerial fog 主段,依赖生成函数拼装(留方案 A)",
    "kAerialFogFragMainTail": "aerial fog 尾段,依赖生成函数拼装(留方案 A)",
    "kAerialFogTonemapPreamble": "AerialFogTonemap 段,依赖生成函数拼装(留方案 A)",
    "kAerialFogTonemapMainHead": "AerialFogTonemap 段,依赖生成函数拼装(留方案 A)",
    "kAerialFogTonemapTail": "AerialFogTonemap 段,依赖生成函数拼装(留方案 A)",
}


def read(path):
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        return f.read()


def gather_sources(src_root):
    files = {}
    for dirpath, _dirs, names in os.walk(src_root):
        for n in names:
            if n.endswith(EXTS):
                p = os.path.join(dirpath, n)
                files[p] = read(p)
    return files


def _raw_string_at(text, r):
    """text[r] 处若是 R"delim( 起始,返回 (body, end_index_after_terminator)。"""
    mm = re.match(r'R"([A-Za-z0-9_]*)\(', text[r : r + 64])
    if not mm:
        return None
    delim = mm.group(1)
    bstart = r + mm.end()
    term = ")" + delim + '"'
    bend = text.find(term, bstart)
    if bend == -1:
        return None
    return text[bstart:bend], bend + len(term)


def extract_raw_string_constants(text):
    """{const_name: 拼接后的 raw_string_body}(同 check_hdr_variant_integrity)。"""
    out = {}
    n = len(text)
    for m in re.finditer(r"\b(k[A-Za-z0-9_]+)\s*=", text):
        name = m.group(1)
        if name in out:
            continue
        i = m.end()
        parts = []
        while i < n:
            r = text.find('R"', i)
            s = text.find(";", i)
            if s == -1:
                s = n
            if r == -1 or s < r:
                break
            got = _raw_string_at(text, r)
            if got is None:
                i = r + 2
                continue
            body, i = got
            parts.append(body)
        if parts:
            out[name] = "".join(parts)
    return out


def extract_bake_shaders(path):
    """TerrainHeightBakeShader.h 的 kTerrainBake{Vert,Frag}GLSL(return R"(...)";)。"""
    text = read(path)
    out = {}
    for m in re.finditer(r"k(TerrainBake\w+GLSL)\(\)\s*\{\s*return\s+R\"([A-Za-z0-9_]*)\(",
                         text):
        name = "k" + m.group(1)
        delim = m.group(2)
        bstart = m.end()
        term = ")" + delim + '"'
        bend = text.find(term, bstart)
        if bend != -1:
            out[name] = text[bstart:bend]
    return out


def is_msl(name, body):
    if name.endswith("MSL"):
        return True
    return ("#include <metal_stdlib>" in body or "fragment float4 " in body
            or "vertex float4 " in body or "texture2d_array" in body
            or "[[buffer(" in body or "[[texture(" in body)


def insert_before_main(src, helper):
    """照 withTerrainLight 的锚点语义:插到 void main( 之前(声明之后)。"""
    anchor = "void main("
    pos = src.find(anchor)
    if pos == -1:
        return None
    return src[:pos] + helper + "\n" + src[pos:]


def assemble_ldr(name, body, helpers):
    """按生产 LDR 注入顺序装配;返回 (source, missing_anchors)。"""
    out = body
    missing = []
    for helper in helpers:
        if helper is None:
            continue
        merged = insert_before_main(out, helper)
        if merged is None:
            missing.append("void main( anchor")
            continue
        out = merged
    return out, missing


def auto_helpers(body, hdr=False, symbol_occlusion=None, symbol_sdf=None):
    """按 body 引用的符号推断需要的注入 helper(与 createShaders 一致)。"""
    helpers = []
    if "encodeSceneOutput(" in body:
        helpers.append(SCENE_OUTPUT_HDR if hdr else SCENE_OUTPUT_LDR)
    if "hdrAlbedo(" in body:
        helpers.append(HDR_ALBEDO_HDR if hdr else HDR_ALBEDO_LDR)
    if "eeSymbolTerrainVisibility(" in body and symbol_occlusion:
        helpers.append(symbol_occlusion)
    if "symbolSdf(" in body and symbol_sdf:
        helpers.append(symbol_sdf)
    return helpers


def find_glslang(explicit):
    if explicit and os.path.isfile(explicit):
        return explicit
    env = os.environ.get("GLSLANG_VALIDATOR")
    if env and os.path.isfile(env):
        return env
    which = shutil.which("glslangValidator")
    if which:
        return which
    script_dir = os.path.dirname(os.path.abspath(__file__))
    for cand in (
        os.path.join(script_dir, "..", "build", "tools", "glslang", "bin",
                     "glslangValidator"),
        os.path.join(script_dir, "glslang", "bin", "glslangValidator"),
    ):
        cand = os.path.normpath(cand)
        if os.path.isfile(cand):
            return cand
    return None


def compile_one(glslang, name, stage, source):
    """返回 (ok, detail)。stage ∈ {vert, frag};同时尝试另一个 stage 兜底。"""
    # 生产源 `R"glsl(\n#version 300 es...` 允许 #version 前有换行/注释,
    # glslang 的 ES profile 则要求 #version 是第一个 token —— 仅编译前
    # 归一化,不动生产文本。
    vi = source.find("#version")
    if vi > 0:
        source = source[vi:]
    source = source.lstrip()
    tried = []
    for st in (stage, "frag" if stage == "vert" else "vert"):
        if st in tried:
            continue
        tried.append(st)
        with tempfile.NamedTemporaryFile(
                "w", suffix="." + st, delete=False, encoding="utf-8") as f:
            f.write(source)
            tmp = f.name
        try:
            proc = subprocess.run(
                [glslang, "-S", st, tmp],
                capture_output=True, text=True, timeout=60)
        finally:
            try:
                os.unlink(tmp)
            except OSError:
                pass
        if proc.returncode == 0:
            return True, st
    detail = (proc.stderr or proc.stdout or "").strip().splitlines()
    return False, "\n".join(detail[-12:])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", required=True, help="src 根目录")
    ap.add_argument("--glslang", default=None, help="glslangValidator 路径")
    args = ap.parse_args()

    glslang = find_glslang(args.glslang)
    if glslang is None:
        print("[glsl-guard] SKIP: 未找到 glslangValidator"
              "(装法: scaffold/tools/fetch_glslang.sh,"
              "或设 $GLSLANG_VALIDATOR / PATH)", file=sys.stderr)
        return 0
    print(f"[glsl-guard] validator: {glslang}")

    files = gather_sources(args.src)
    const_bodies = {}
    for text in files.values():
        const_bodies.update(extract_raw_string_constants(text))
    bake_path = os.path.join(args.src, "earth_engine", "renderer",
                             "TerrainHeightBakeShader.h")
    if os.path.isfile(bake_path):
        const_bodies.update(extract_bake_shaders(bake_path))

    # helper 表:注入用的 LDR/HDR 地形光照 + 页存储采样。
    terrain_light = const_bodies.get("kTerrainLightGLSL")
    terrain_light_hdr = const_bodies.get("kTerrainLightHdrGLSL")
    page_store = const_bodies.get("kPageStoreSamplingGLSL")
    symbol_occlusion = const_bodies.get("kSymbolTerrainOcclusionBody")
    symbol_sdf = const_bodies.get("kSymbolSdfBody")
    for helper_name, val in (("kTerrainLightGLSL", terrain_light),
                             ("kTerrainLightHdrGLSL", terrain_light_hdr),
                             ("kPageStoreSamplingGLSL", page_store)):
        if val is None:
            print(f"[glsl-guard] FAIL: 注入 helper {helper_name} 未扫到",
                  file=sys.stderr)
            return 1

    results = []
    for name in sorted(const_bodies):
        body = const_bodies[name]
        if name in SKIP_NAMES:
            results.append(("SKIP", name, SKIP_NAMES[name]))
            continue
        if is_msl(name, body):
            results.append(("SKIP", name, "MSL(留 Metal 编译器)"))
            continue
        if "void main(" not in body:
            results.append(("SKIP", name, "无 main 的 helper 块(注入后验)"))
            continue

        stage = "vert" if "Vertex" in name else "frag"

        # LDR 主路径:自动按引用符号注入(与 createShaders 的 withXxx 对应)。
        helpers = []
        if "terrainSurfaceLight(" in body:
            helpers.append(terrain_light)
        if "eePageStoreCompose(" in body or "eePageStoreSample(" in body:
            helpers.append(page_store)
        helpers.extend(auto_helpers(body, hdr=False,
                                    symbol_occlusion=symbol_occlusion,
                                    symbol_sdf=symbol_sdf))
        src, missing = assemble_ldr(name, body, helpers)
        if missing:
            results.append(("FAIL", name, "缺少注入锚点 " + ", ".join(missing)))
            continue
        ok, detail = compile_one(glslang, name, stage, src)
        if not ok:
            results.append(("FAIL", name, detail))
            continue
        results.append(("PASS", name, f"LDR {detail}"))

        # HDR 冻结态:只对已知消费方补验 HDR 注入变体(首次自动化触碰)。
        if name in HDR_VARIANT_FRAGMENTS:
            hdr_helpers = []
            if "terrainSurfaceLight(" in body:
                hdr_helpers.append(terrain_light_hdr)
            if "eePageStoreCompose(" in body or "eePageStoreSample(" in body:
                hdr_helpers.append(page_store)
            hdr_helpers.extend(auto_helpers(body, hdr=True,
                                            symbol_occlusion=symbol_occlusion,
                                            symbol_sdf=symbol_sdf))
            src_h, missing_h = assemble_ldr(name, body, hdr_helpers)
            if missing_h:
                results.append(("FAIL", name, "HDR 注入锚点缺失"))
                continue
            ok_h, detail_h = compile_one(glslang, name + " [HDR]", stage, src_h)
            if ok_h:
                results.append(("PASS", name + " [HDR]", f"HDR {detail_h}"))
            else:
                results.append(("FAIL", name + " [HDR]", detail_h))

    fails = [r for r in results if r[0] == "FAIL"]
    passes = [r for r in results if r[0] == "PASS"]
    skips = [r for r in results if r[0] == "SKIP"]
    for kind, name, detail in results:
        print(f"[glsl-guard] {kind:4s} {name}")
        if detail and kind != "PASS":
            print("    " + detail.replace("\n", "\n    "))

    print(f"[glsl-guard] summary: PASS={len(passes)} FAIL={len(fails)} "
          f"SKIP={len(skips)}")
    if fails:
        print(f"[glsl-guard] FAILED: {len(fails)} 个 shader 编译失败", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
