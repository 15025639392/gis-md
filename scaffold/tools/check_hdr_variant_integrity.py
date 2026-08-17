#!/usr/bin/env python3
"""HDR 管线开启态(kEnableHdrPipeline=true)的结构完整性守卫。

## 为什么存在

HDR 线性管线(B0/B2/T2)的决策是「长期做、短期挂起」(见 docs/northstar/lighting.md
L-V5)。挂起期它 **默认关、仅 GLES、无设备回归**,于是有一整片代码:
  - host ctest 不编译 shader(它们是运行时字符串);
  - `glslc` 本机缺失,无法离线编译校验;
  - flag 默认 false,真机也走不到 HDR 分支。
→ 这片代码编不到、跑不到、测不到。并行 churn 改共享 shader 时会 **静默改坏 HDR
开启态而无人察觉**,等几个月后唤醒 HDR 才发现半成品烂了。本守卫是那段挂起期的防腐锁。

## 它检查什么 —— 以及**明确不检查什么**

⚠️⚠️ **只查字符串结构完整,不查 GLSL 语法正确。** host 无 GLSL 编译器(glslc 缺失),
所以「HDR 分支里写了语法非法的 GLSL」这类错本守卫抓不到 —— 那仍须真机或装了 glslc
的 CI。别把本守卫通过当成「HDR 能编过」。它抓的是 churn 期**最常发生**的那几类:
分支被删平、锚点被改没(致注入静默 no-op → 引用未定义函数)、HDR 变体常量/线性化
被误删。语法错是低频风险,留给唤醒时的真机验证。

判定按置信度分两档(照 check_ai_index_refs.py 的 FAIL/SKIP 取向):

  FAIL(高置信,确定性结构破坏)
    C1 kEnableHdrPipeline 开关声明消失
    C2 某注入器(withTerrainLight/withSceneOutput/withGltfHdr)函数体内不再引用
       kEnableHdrPipeline —— 分支被删平 = HDR 静默退化成 LDR
    C3 withSceneOutput / withGltfHdr 的 HDR 线性化字面量(srgbToLinear 的
       pow(max(c,vec3(0.0)),vec3(2.2)))消失
    C4 HDR 变体常量(kTerrainLightHdrGLSL/MSL、kAtmosphereComposeHdr)未定义或
       丢了线性化标记
    C5 **锚点完整性(最高价值)**:凡被 withXxx(kConst) 注入的 shader 常量,其
       raw-string 体内必须含注入锚点(`void main(` 或 `fragment float4 `);缺锚点
       → 注入 if(pos==npos)return src 静默 no-op → shader 引用未定义函数
    C6 AerialFogTonemap effect 枚举消失(B0 的 fog×tonemap 合并终端)

  SKIP(证据不足,只计数不判错)
    - 被注入的常量名在扫描到的源文件里找不到定义(可能改名/移动/宏拼接)
    - 注入器函数体边界无法可靠括号配对

SKIP 数按原因打印。**刻意的**:只报 FAIL 时「全绿」既可能是「都完好」也可能是
「什么都没扫到」,读数相同的检查比没有更糟。

用法:
  check_hdr_variant_integrity.py --src <src 根目录>
"""

import argparse
import os
import re
import sys

# ---- 扫描的文件类型 ----
EXTS = (".cpp", ".h", ".mm")

# ---- 注入器与其锚点 ----
INJECTORS = ("withTerrainLight", "withSceneOutput", "withGltfHdr")
ANCHORS = ("void main(", "fragment float4 ")

# ---- HDR 线性化字面量(srgbToLinear 的 encode 核) ----
HDR_ENCODE_LITERAL = "pow(max(c,vec3(0.0)),vec3(2.2))"

# ---- HDR 变体常量 → 其体内必含的标记 ----
HDR_VARIANT_CONSTANTS = {
    "kTerrainLightHdrGLSL": "srgbToLinear",
    "kTerrainLightHdrMSL": "srgbToLinear",
    "kAtmosphereComposeHdr": "kSunHdrBoost",
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
    """text[r] 处若是 R"delim( 起始,返回 (body, end_index_after_terminator),否则 None。"""
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
    """返回 {const_name: 拼接后的 raw_string_body}。

    支持任意 C++ raw-string 分隔符 R"delim(...)delim",且支持**跨 + 拼接的多段**定义
    (点符号 shader:`std::string(R"glsl(头)glsl") + kBody + R"glsl(void main...)glsl";`)
    —— 把 `=` 到语句结尾 `;` 之间的所有内联 raw-string 段**全部拼接**返回,故 void main(
    在第二段也能被扫到。被引用的常量(kBody)不展开(锚点 void main( 本仓均在内联段)。

    终止符按各段自己的分隔符精确配对,GLSL 体内的 `;`/`)"` 不会误判语句结束。"""
    out = {}
    n = len(text)
    for m in re.finditer(r"\b(k[A-Za-z0-9_]+)\s*=", text):
        name = m.group(1)
        if name in out:  # 保留首个定义
            continue
        i = m.end()
        parts = []
        while i < n:
            r = text.find('R"', i)
            s = text.find(";", i)
            if s == -1:
                s = n
            # 语句在遇到下一段 raw-string 之前就 `;` 结束了 → 定义完
            if r == -1 or s < r:
                break
            got = _raw_string_at(text, r)
            if got is None:
                i = r + 2  # 不是合法 raw-string 起始,跳过这个 R"
                continue
            body, i = got
            parts.append(body)
        if parts:
            out[name] = "".join(parts)
    return out


def find_function_body(text, signature_regex):
    """从匹配 signature 的位置起做括号配对,返回函数体文本;失败返回 None。"""
    m = re.search(signature_regex, text)
    if not m:
        return None
    brace = text.find("{", m.end())
    if brace == -1:
        return None
    depth = 0
    for i in range(brace, len(text)):
        c = text[i]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return text[brace : i + 1]
    return None


def injected_constant_names(renderer_text):
    """扫 withXxx(kConst) 调用点,返回 {injector: set(const_names)}。
    容忍参数跨行(withTerrainLight(\\n  kFoo)。"""
    out = {inj: set() for inj in INJECTORS}
    for inj in INJECTORS:
        for m in re.finditer(re.escape(inj) + r"\(\s*(k[A-Za-z0-9_]+)", renderer_text):
            out[inj].add(m.group(1))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", required=True, help="src 根目录")
    args = ap.parse_args()

    if not os.path.isdir(args.src):
        print(f"[hdr-guard] --src 不是目录: {args.src}", file=sys.stderr)
        return 2

    files = gather_sources(args.src)
    blob = "\n".join(files.values())  # 全局文本(存在性检查用)

    # 全局 raw-string 常量表(跨文件)
    const_bodies = {}
    for text in files.values():
        const_bodies.update(extract_raw_string_constants(text))

    fails = []
    skips = []

    # C1 开关声明存在
    if "kEnableHdrPipeline" not in blob:
        fails.append("C1 kEnableHdrPipeline 开关在整个 src 里消失了")
    elif not re.search(r"constexpr\s+bool\s+kEnableHdrPipeline", blob):
        fails.append("C1 kEnableHdrPipeline 不再是 constexpr bool 声明(签名被改)")

    # C2 注入器分支仍活 + C3 HDR 线性化字面量仍在
    renderer_text = None
    for p, t in files.items():
        if p.endswith("Renderer.cpp"):
            renderer_text = t
            break
    if renderer_text is None:
        skips.append("C2/C3/C5 未找到 Renderer.cpp,注入器相关检查跳过")
    else:
        for inj in INJECTORS:
            body = find_function_body(
                renderer_text, r"std::string\s+" + re.escape(inj) + r"\s*\("
            )
            if body is None:
                skips.append(f"C2 注入器 {inj} 函数体边界无法配对,跳过")
                continue
            if "kEnableHdrPipeline" not in body:
                fails.append(
                    f"C2 注入器 {inj} 体内不再引用 kEnableHdrPipeline —— "
                    f"HDR 分支疑被删平,开启态静默退化成 LDR"
                )
        # C3:两个内联 encode 注入器的 HDR 字面量
        for inj in ("withSceneOutput", "withGltfHdr"):
            body = find_function_body(
                renderer_text, r"std::string\s+" + re.escape(inj) + r"\s*\("
            )
            if body is None:
                continue  # C2 已记 SKIP
            # 空白无关比对:防格式化(加空格)误报
            if HDR_ENCODE_LITERAL.replace(" ", "") not in body.replace(" ", ""):
                fails.append(
                    f"C3 {inj} 的 HDR 线性化字面量 "
                    f"{HDR_ENCODE_LITERAL} 消失 —— 开启态输出不再解到线性"
                )

    # C4 HDR 变体常量定义 + 标记
    for name, marker in HDR_VARIANT_CONSTANTS.items():
        if name not in const_bodies:
            fails.append(f"C4 HDR 变体常量 {name} 未定义(缺失或改名)")
        elif marker not in const_bodies[name]:
            fails.append(f"C4 HDR 变体常量 {name} 丢了标记 '{marker}'")

    # C5 锚点完整性(最高价值)
    if renderer_text is not None:
        injected = injected_constant_names(renderer_text)
        for inj, names in injected.items():
            for cname in sorted(names):
                if cname not in const_bodies:
                    skips.append(f"C5 {inj}({cname}) 常量定义未扫到,锚点检查跳过")
                    continue
                body = const_bodies[cname]
                if not any(a in body for a in ANCHORS):
                    fails.append(
                        f"C5 {cname}(被 {inj} 注入)体内无注入锚点"
                        f"(void main( / fragment float4 )—— 注入将静默 no-op,"
                        f"开启态 shader 引用未定义函数"
                    )

    # C6 AerialFogTonemap effect 枚举
    if "AerialFogTonemap" not in blob:
        fails.append("C6 AerialFogTonemap effect 枚举消失(B0 fog×tonemap 合并终端)")

    # ---- 报告 ----
    print(f"[hdr-guard] 扫描 {len(files)} 个源文件,{len(const_bodies)} 个 raw-string 常量")
    if skips:
        print(f"[hdr-guard] SKIP {len(skips)}(证据不足,不判错):")
        for s in skips:
            print(f"    - {s}")
    if fails:
        print(f"[hdr-guard] FAIL {len(fails)}:")
        for f in fails:
            print(f"    ✗ {f}")
        print(
            "[hdr-guard] ⚠️ 提醒:本守卫只查结构,不查 GLSL 语法。"
            "唤醒 HDR 时仍须真机/glslc 做真编译校验。"
        )
        return 1
    print("[hdr-guard] OK —— HDR 开启态结构完整(注意:未做语法/编译校验)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
