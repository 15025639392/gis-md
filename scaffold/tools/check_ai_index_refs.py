#!/usr/bin/env python3
"""AI_INDEX.md 的行号引用校验。

## 为什么存在

AI_INDEX.md 里成百上千个 `File.cpp:1234` 引用没有任何东西盯着它变。2026-08-06
一次复核发现 Renderer 两张表的行号整体停在一次约 +2200 行的位移之前 —— 每一个
都还"在文件范围内",只是指向了无关代码。

## 它检查什么(以及不检查什么)

⚠️ **只检查落点是否命中声称的符号,不检查文字描述是否属实。**

这个区分是本脚本设计的核心。同一次复核里查出的四处失真中,只有"行号漂移"这一类
可机器判定;而 `makeGltfPrimitiveCommand` 那条("seeds the full PBR uniform set"
—— 实际该函数被禁止写 uniform map,方向正相反)行号哪怕完全正确也照样是错的,
必须靠人读源码。别把本脚本通过当成"AI_INDEX 是对的"。

判定规则,按置信度分两档:

  FAIL(高置信,确定性错误)
    1. 引用行号 > 文件总行数           —— 文件缩短或引用凭空捏造
    2. 该行所属的表格行点名了某个符号,而该符号在文件里**存在**,但引用行号
       落在它的定义范围之外 —— 这正是 +2200 位移的形态

  SKIP(证据不足,只计数不判错)
    - 表格行没点名可解析的符号
    - 符号在文件里找不到(可能是宏、模板、被删、或本就写在别处)
    - 引用的文件名无法唯一解析

SKIP 数会打出来并按原因归类。**这是刻意的**:只报 FAIL 的话,"全绿"既可能是
"都对",也可能是"什么都没查到" —— 这两种状态读数相同的检查比没有更糟。

用法:
    python3 tools/check_ai_index_refs.py [--doc AI_INDEX.md] [--src src] [-v]
退出码 0 = 无 FAIL。
"""

import argparse
import collections
import os
import re
import sys

# 定义起点容差(行)。见 check() 中的说明。
_DEF_LEAD_IN = 3

# ---------------------------------------------------------------- 源码索引

# .cpp 里的顶层定义:行首非空白,含 `Name(` 或 `Class::Name(`。
# 故意宽松 —— 宁可多收几个候选(range 会因此偏小、判定更保守),
# 也不要漏掉真正的定义导致整行退化成 SKIP。
_DEF_RE = re.compile(
    r'^[A-Za-z_~][\w:<>,\s*&\[\]]*?(?:(?P<cls>\w+)::)?(?P<name>~?\w+)\s*\('
)


def index_definitions(lines):
    """返回 {符号名: [(起始行, 结束行), ...]},行号 1-based、闭区间。"""
    starts = []  # (line_no, name)
    for i, line in enumerate(lines, 1):
        if not line or line[0].isspace():
            continue
        if line.startswith(('#', '//', '/*', '*', '}')):
            continue
        m = _DEF_RE.match(line)
        if not m:
            continue
        name = m.group('name')
        # 排除控制流关键字与常见非定义
        if name in ('if', 'for', 'while', 'switch', 'return', 'sizeof',
                    'static_assert', 'catch', 'else'):
            continue
        starts.append((i, name))

    table = collections.defaultdict(list)
    for idx, (line_no, name) in enumerate(starts):
        end = starts[idx + 1][0] - 1 if idx + 1 < len(starts) else len(lines)
        table[name].append((line_no, end))
    return table


# ---------------------------------------------------------------- 文件解析

def build_file_map(src_root):
    """basename → [相对路径, ...]。同名多份时留全部,解析不唯一即 SKIP。"""
    file_map = collections.defaultdict(list)
    for dirpath, _dirnames, filenames in os.walk(src_root):
        for fn in filenames:
            if fn.endswith(('.cpp', '.h', '.mm', '.hpp')):
                file_map[fn].append(os.path.join(dirpath, fn))
    return file_map


# ---------------------------------------------------------------- 文档解析

# `### Renderer.h / .cpp`、`### ios/RenderDeviceMetal.h / .mm`、`### GpuUploadQueue.h`
_SECTION_RE = re.compile(r'^###\s+(?:[\w./]*/)?(\w+)\.(\w+)(?:\s*/\s*\.(\w+))?')
# 引用:可带文件名前缀(`Renderer.cpp:4418`),也可裸写(`.cpp:4418`)
_REF_RE = re.compile(r'(?:(?<![\w.])(?P<file>\w+)\.(?P<ext1>cpp|h|mm|hpp)|'
                     r'\.(?P<ext2>cpp|h|mm|hpp)):(?P<line>\d+)')
# 表格行里被反引号包住的第一个像函数名的东西
_SYMBOL_RE = re.compile(r'`~?(\w+)(?:\(|`|<)')


class Ref:
    __slots__ = ('doc_line', 'path', 'line', 'symbol', 'raw')

    def __init__(self, doc_line, path, line, symbol, raw):
        self.doc_line, self.path, self.line = doc_line, path, line
        self.symbol, self.raw = symbol, raw


def parse_doc(doc_path, file_map, skips):
    refs = []
    section_files = {}  # ext → path
    with open(doc_path, encoding='utf-8') as fh:
        doc_lines = fh.readlines()

    for n, raw in enumerate(doc_lines, 1):
        # ⚠️ 任何标题都先清空绑定,再尝试重新绑定。曾经只在"标题能解析出文件名"
        # 时才重置,于是 `### FrameState / render-pass wiring` 这种非文件标题会让
        # **上一节**的绑定继续生效,把裸 `.cpp:204` 算到毫不相干的文件头上,报出
        # 一堆假的越界。少查一节,好过把引用挂到错的文件上。
        if raw.startswith('#'):
            section_files = {}
            m = _SECTION_RE.match(raw)
            if m:
                stem, ext_a, ext_b = m.group(1), m.group(2), m.group(3)
                for ext in (ext_a, ext_b):
                    if not ext:
                        continue
                    cands = file_map.get('%s.%s' % (stem, ext), [])
                    if len(cands) == 1:
                        section_files[ext] = cands[0]
            continue

        found = list(_REF_RE.finditer(raw))
        if not found:
            continue
        sym_m = _SYMBOL_RE.search(raw)
        symbol = sym_m.group(1) if sym_m else None

        for fm in found:
            ext = fm.group('ext1') or fm.group('ext2')
            stem = fm.group('file')
            if stem:
                cands = file_map.get('%s.%s' % (stem, ext), [])
                if len(cands) != 1:
                    skips['文件名无法唯一解析'] += 1
                    continue
                path = cands[0]
            else:
                path = section_files.get(ext)
                if not path:
                    skips['裸引用但本节未绑定该扩展名的源文件'] += 1
                    continue
            refs.append(Ref(n, path, int(fm.group('line')), symbol,
                            raw.rstrip()))
    return refs


# ---------------------------------------------------------------- 判定

def baseline_key(path, line, symbol):
    """基线键。**刻意不含文档行号** —— 文档一改行号就全变,那样的基线每次编辑都
    要重开,很快就会被无脑 --update 掉,等于没有。键取「引用的源文件 + 引用的行号
    + 点名的符号」:只要这条引用本身没被改,键就不动;一旦有人把它改对,这条检查
    直接通过,压根不会去查基线。"""
    return '%s:%d:%s' % (os.path.basename(path), line, symbol or '-')


def load_baseline(path):
    if not path or not os.path.isfile(path):
        return set()
    keys = set()
    with open(path, encoding='utf-8') as fh:
        for line in fh:
            line = line.strip()
            if line and not line.startswith('#'):
                keys.add(line)
    return keys


def check(refs, skips, verbose):
    cache = {}
    failures = []
    anchored = 0

    # 同一个 (文档行, 文件, 符号) 的多个引用只要**有一个**落在符号范围内就算过 ——
    # 一行表格常同时给出函数范围与内部若干细节行,细节行本就该在范围内,但也可能
    # 引用邻近的常量定义。取"至少一个命中"是保守判据。
    groups = collections.OrderedDict()
    for r in refs:
        groups.setdefault((r.doc_line, r.path, r.symbol), []).append(r)

    for (doc_line, path, symbol), group in groups.items():
        if path not in cache:
            with open(path, encoding='utf-8', errors='replace') as fh:
                lines = fh.read().splitlines()
            cache[path] = (lines, index_definitions(lines))
        lines, table = cache[path]

        for r in group:
            if r.line > len(lines):
                failures.append(
                    (baseline_key(r.path, r.line, symbol),
                     'AI_INDEX.md:%d  %s:%d 超出文件末尾(共 %d 行)'
                     % (r.doc_line, os.path.basename(path), r.line,
                        len(lines))))

        # 头文件不做锚定,只保留越界检查。理由:声明可以缩进、可以重载、可以在
        # 注释与内联实现里重复出现,"符号出现在第 N 行"根本不构成范围;实测把它
        # 当锚会造出上百条噪声。宁可少查,不要造一条会被学会忽略的常亮警告。
        if path.endswith(('.h', '.hpp')):
            skips['头文件:只做越界检查,不锚定符号'] += 1
            continue

        if not symbol:
            skips['表格行未点名符号'] += 1
            continue

        ranges = table.get(symbol)
        if not ranges:
            skips['符号在该文件中找不到'] += 1
            if verbose:
                print('  SKIP AI_INDEX.md:%d  `%s` 不在 %s 中'
                      % (doc_line, symbol, os.path.basename(path)))
            continue

        # 定义起点容差:C++ 返回类型常独占一行(`std::optional<X>\nCls::fn(...)`),
        # 索引锚在带函数名的那行,而文档写的是**签名首行**。放宽 3 行吸收这个偏差,
        # 以及紧贴定义的 doc 注释。位移类错误动辄几百上千行,不会被这点容差掩盖。
        def hits(line_no):
            return any(lo - _DEF_LEAD_IN <= line_no <= hi for lo, hi in ranges)

        # 判据 = **本行第一个引用**必须命中。第一个引用按本文档惯例给的就是定义
        # 范围,后面那些是内部细节行。
        #
        # ⚠️ 这条是被反例控制组逼出来的。原判据是"任一引用命中即放行" —— 注入
        # 一条漂移引用去验它时,它**没有响**:同一行里还有两个正确的细节行引用把
        # 它盖过去了。收紧后在现有文档上只多出 6 条(206→212),代价可以忽略。
        # 残余盲区:第一个引用碰巧还对、后面的细节行漂了,仍然测不出。
        ok = hits(group[0].line)
        span = '定义于 %s' % ', '.join('%d-%d' % rg for rg in ranges)

        anchored += 1
        if not ok:
            failures.append(
                (baseline_key(path, group[0].line, symbol),
                 'AI_INDEX.md:%d  `%s` %s,但本行引用的是 %s —— 行号已漂移'
                 % (doc_line, symbol, span,
                    ', '.join('%s:%d' % (os.path.basename(r.path), r.line)
                              for r in group))))
    return failures, anchored


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    repo = os.path.dirname(here)
    ap = argparse.ArgumentParser()
    ap.add_argument('--doc', default=os.path.join(repo, '..', 'AI_INDEX.md'))
    ap.add_argument('--src', default=os.path.join(repo, 'src'))
    ap.add_argument('--baseline',
                    default=os.path.join(here, 'ai_index_refs_baseline.txt'),
                    help='已知陈旧引用清单;命中的不判失败,只计入欠账并打印。')
    ap.add_argument('--update-baseline', action='store_true',
                    help='把当前全部失败写回基线。⚠️ 这是**认账**不是修复:'
                         '只在确认这些引用无法当场订正时用,并在 commit 里说明。')
    ap.add_argument('-v', '--verbose', action='store_true')
    args = ap.parse_args()

    doc = os.path.abspath(args.doc)
    src = os.path.abspath(args.src)
    if not os.path.isfile(doc):
        print('找不到文档:%s' % doc, file=sys.stderr)
        return 2
    if not os.path.isdir(src):
        print('找不到源码根:%s' % src, file=sys.stderr)
        return 2

    skips = collections.Counter()
    file_map = build_file_map(src)
    refs = parse_doc(doc, file_map, skips)
    failures, anchored = check(refs, skips, args.verbose)

    if args.update_baseline:
        with open(args.baseline, 'w', encoding='utf-8') as fh:
            fh.write('# AI_INDEX.md 已知陈旧行号引用(认账清单,非豁免许可)。\n'
                     '# 每行 = 源文件:引用行号:点名符号。命中者不判失败。\n'
                     '# 把某条改对后,请从本文件删掉它 —— 这个数字只该向下走。\n')
            for key in sorted(k for k, _ in failures):
                fh.write(key + '\n')
        print('已写入基线 %s:%d 条' % (args.baseline, len(failures)))
        return 0

    baseline = load_baseline(args.baseline)
    fresh = [(k, msg) for k, msg in failures if k not in baseline]
    known = len(failures) - len(fresh)

    total_skip = sum(skips.values())
    print('AI_INDEX 引用校验:引用 %d 处,锚定判定 %d 处,跳过 %d 处,'
          '新增失败 %d 处,已知欠账 %d 处'
          % (len(refs), anchored, total_skip, len(fresh), known))
    # 跳过原因与欠账数必须每次都打出来:只报失败的话,"全绿"既可能是"都对",
    # 也可能是"什么都没查到"或"全被基线吃掉了" —— 三者读数不能相同。
    for reason, count in sorted(skips.items(), key=lambda kv: -kv[1]):
        print('  跳过 %5d  %s' % (count, reason))

    stale_baseline = len(baseline) - known
    if stale_baseline > 0:
        print('  基线里有 %d 条已不再复现(引用被改动或删除),可从 %s 清掉'
              % (stale_baseline, os.path.basename(args.baseline)))

    for _key, msg in fresh:
        print('FAIL ' + msg)

    if fresh:
        print('\n%d 处行号引用与源码脱节。修法:grep 出符号的真实位置后改文档,'
              '并逐个回读校验(sed -n "Np")。\n'
              '确实无法当场订正才用 --update-baseline 认账。' % len(fresh))
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
