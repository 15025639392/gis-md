#!/usr/bin/env python3
"""AI_INDEX.md 的一致性校验(两类,性质不同)。

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

## 两类检查

  A 行号引用校验 —— 「**写下的引用**对不对」。上面讲的全是这一类。
  B 索引覆盖检查 —— 「**该写的**写了没」。>N 行(默认 300)的 .cpp/.mm 必须有专属
    `###` 小节。A 对这类失效**完全免疫**:它只校验已经存在的引用,查不出整个文件
    从未被收录。2026-08-06 实测 13 个 >300 行的文件零条目,其中 FeatureRenderLayer
    (2192 行)与 TerrainPageStore(1041 行)全文 0 次提及 —— 而当时正有人在
    TerrainPageStore 上改代码。B 的判据干净二元,没有启发式也就没有假阳性。

还有**第三类查不出来的**:内容失真 —— 行号对、条目在,但描述的东西已经被重构走了
(实例:某节描述一个 127 行的 update(),实际该文件已成 33 行纯委托)。只能人读源码。

用法:
    python3 tools/check_ai_index_refs.py [--doc AI_INDEX.md] [--src src]
                                         [--coverage-min-lines 300] [-v]
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


# 顶层常量:`constexpr uint32_t kMaxLevel = 30;` / `static const float kFoo = ...`
_CONST_RE = re.compile(
    r'^(?:static\s+|inline\s+|constexpr\s+|const\s+)+[\w:<>,\s*&]+?'
    r'\b(?P<name>[A-Za-z_]\w*)\s*(?:=|\{|\[)')


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

    # 顶层常量 / 变量定义。文档大量引用 `kMaxLevel` (.cpp:17) 这类锚点,不索引
    # 它们的话这些**完全正确**的引用会被判成漂移 —— 假阳性进了基线就永远躺在那。
    # 常量没有"范围",按单行处理。
    table = collections.defaultdict(list)
    for i, line in enumerate(lines, 1):
        m = _CONST_RE.match(line)
        if m:
            table[m.group('name')].append((i, i))
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
# 反引号包住的标识符,用来给引用找归属符号
_SYMBOL_RE = re.compile(r'`~?(\w+)(?:\(|`|<|/)')


def symbols_in_row(line):
    """本行所有反引号标识符 —— 引用的归属候选集。

    ⚠️ 判据是「引用须落在本行**任一**具名符号的定义范围内」,不是"第一个符号"
    也不是"最近的前一个符号"。两种更紧的配对都试过,都造假阳性:

      首符号   `QuadtreeTilingScheme` equivalent ... (.cpp:16-22)
               首符号是类名,引用指的是成员 tileCountX —— 完全正确却被判漂移
      最近前   `longitudeDegrees` / `latitudeDegrees` | .cpp:14-20
               `.cpp:14` 最近的前一个符号是 latitudeDegrees(18-),但 14 是
               longitudeDegrees —— 一行列多个函数共用一个范围时必错

    实测三种判据在现有文档上:首符号 锚 572,最近前 锚 602,本判据 锚 756 ——
    覆盖最广而假阳性最少。整行没有任何符号能解析时跳过。
    """
    return {m.group(1) for m in _SYMBOL_RE.finditer(line)}


class Ref:
    __slots__ = ('doc_line', 'path', 'line', 'symbols', 'raw')

    def __init__(self, doc_line, path, line, symbols, raw):
        self.doc_line, self.path, self.line = doc_line, path, line
        self.symbols, self.raw = symbols, raw


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
            # 多文件小节(`### TileKey.h / .cpp / TileCacheKey.h / TileID.h / .cpp`)
            # 里的裸 `.cpp:34` 到底指哪个文件,标题本身不足以判断 —— 绑到第一个
            # 会把 TileID.cpp 的引用算到 TileKey.cpp 头上,报出假越界。宁可整节
            # 不查。与上面 section 泄漏是同一类错:**猜文件比不查更糟**。
            if len(set(re.findall(r'\b(\w+)\.(?:cpp|h|mm|hpp)\b', raw))) > 1:
                m = None
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

        symbols = symbols_in_row(raw)
        for fm in found:
            symbol = None  # 归属在 check() 里按整行候选集判定
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
            refs.append(Ref(n, path, int(fm.group('line')), symbols,
                            raw.rstrip()))
    return refs


# ------------------------------------------------------- 覆盖检查(第二类失效)

# 允许"大文件没有专属小节"的例外。**键 = 文件名,值 = 理由,理由不可省**。
# 当前为空:2026-08-06 把 >300 行的缺口从 13 个补到了 0。
#
# 什么时候该往这里加:生成代码、第三方内嵌实现(stb_*)、明确要删的 POC。
# 什么时候不该:"这个文件太难写文档了" —— 那说明它更需要文档。
_COVERAGE_EXEMPT = {}


def check_coverage(doc_path, src_root, min_lines):
    """大文件是否在 AI_INDEX 里有专属 `###` 小节。

    ⚠️ 这是行号守卫**查不出**的一类失效:它只校验已经写下的引用,对"整个文件从未
    被收录"完全免疫。2026-08-06 实测有 13 个 >300 行的文件零条目,其中
    FeatureRenderLayer(2192 行)与 TerrainPageStore(1041 行)全文 **0 次提及** ——
    而当时正有人在 TerrainPageStore 上改代码。

    判据干净且二元:文件行数 > min_lines 且文档里没有 `### …<stem>…` 标题即失败。
    没有启发式,因此也没有假阳性 —— 与"符号搜不搜得到"那个 100% 噪声的思路相反。
    """
    doc = open(doc_path, encoding='utf-8').read()
    missing = []
    for dirpath, _dirnames, filenames in os.walk(src_root):
        for fn in filenames:
            if not fn.endswith(('.cpp', '.mm')):
                continue
            stem = fn.rsplit('.', 1)[0]
            if stem in _COVERAGE_EXEMPT:
                continue
            path = os.path.join(dirpath, fn)
            with open(path, encoding='utf-8', errors='replace') as fh:
                count = sum(1 for _ in fh)
            if count <= min_lines:
                continue
            if not re.search(r'^###[^\n]*\b%s\b' % re.escape(stem), doc, re.M):
                missing.append((count, stem, os.path.relpath(path, src_root)))
    missing.sort(reverse=True)
    return missing


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
            if not line or line.startswith('#'):
                continue
            # 行尾 `# 原因` 是写给人看的判断依据 —— 一条例外没有理由,下一个人就
            # 只能猜它是真欠账还是校验器的局限,基线很快退化成无人敢碰的黑名单。
            keys.add(line.split('#', 1)[0].strip())
    return keys


def check(refs, skips, verbose):
    cache = {}
    failures = []
    anchored = 0

    rows = collections.OrderedDict()
    for r in refs:
        rows.setdefault((r.doc_line, r.path), []).append(r)

    for (doc_line, path), group in rows.items():
        if path not in cache:
            with open(path, encoding='utf-8', errors='replace') as fh:
                lines = fh.read().splitlines()
            cache[path] = (lines, index_definitions(lines))
        lines, table = cache[path]

        for r in group:
            if r.line > len(lines):
                failures.append(
                    (baseline_key(r.path, r.line, None),
                     'AI_INDEX.md:%d  %s:%d 超出文件末尾(共 %d 行)'
                     % (r.doc_line, os.path.basename(path), r.line,
                        len(lines))))

        # 头文件不做锚定,只保留越界检查。理由:声明可以缩进、可以重载、可以在
        # 注释与内联实现里重复出现,"符号出现在第 N 行"根本不构成范围;实测把它
        # 当锚会造出上百条噪声。宁可少查,不要造一条会被学会忽略的常亮警告。
        if path.endswith(('.h', '.hpp')):
            skips['头文件:只做越界检查,不锚定符号'] += 1
            continue

        named = sorted(group[0].symbols)
        resolved = [s for s in named if s in table]
        if not resolved:
            skips['本行没有能在该文件中解析的符号'] += 1
            if verbose and named:
                print('  SKIP AI_INDEX.md:%d 本行符号 %s 均不在 %s 中'
                      % (doc_line, named, os.path.basename(path)))
            continue

        # 定义起点容差:C++ 返回类型常独占一行(`std::optional<X>\nCls::fn(...)`),
        # 索引锚在带函数名的那行,而文档写的是**签名首行**。放宽 3 行吸收这个偏差,
        # 以及紧贴定义的 doc 注释。位移类错误动辄几百上千行,不会被这点容差掩盖。
        ranges = [rg for s in resolved for rg in table[s]]
        for r in group:
            if r.line > len(lines):
                continue  # 已按越界报过
            anchored += 1
            if any(lo - _DEF_LEAD_IN <= r.line <= hi for lo, hi in ranges):
                continue
            failures.append(
                (baseline_key(r.path, r.line, None),
                 'AI_INDEX.md:%d  %s:%d 不在本行任一具名符号的定义范围内'
                 '(候选 %s)'
                 % (doc_line, os.path.basename(path), r.line,
                    ', '.join('%s=%s' % (s, table[s][0]) for s in resolved[:3]))))
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
    ap.add_argument('--coverage-min-lines', type=int, default=300,
                    help='超过这么多行的 .cpp/.mm 必须有专属 ### 小节(默认 300)')
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
        # 保留既有条目的行尾理由与文件头注释。曾经这里是无脑覆写,一次
        # --update-baseline 就把逐条读源码得出的判断全洗掉了 —— 而一条没有理由的
        # 例外,下一个人无法判断它是真欠账还是工具局限,基线就退化成黑名单。
        header, reasons = [], {}
        if os.path.isfile(args.baseline):
            for line in open(args.baseline, encoding='utf-8'):
                line = line.rstrip('\n')
                if line.startswith('#'):
                    header.append(line)
                elif '#' in line:
                    key, why = line.split('#', 1)
                    reasons[key.strip()] = '  # ' + why.strip()
        if not header:
            header = ['# AI_INDEX.md 行号引用校验的例外清单。',
                      '# 每条都应带行尾 `# 理由`:是真欠账,还是校验器锚不住。',
                      '# 本文件只该减不该增。']
        keys = sorted(set(k for k, _ in failures))
        with open(args.baseline, 'w', encoding='utf-8') as fh:
            fh.write('\n'.join(header) + '\n')
            for key in keys:
                fh.write(key + reasons.get(key, '') + '\n')
        missing = [k for k in keys if k not in reasons]
        print('已写入基线 %s:%d 条' % (args.baseline, len(keys)))
        if missing:
            print('  ⚠️ 其中 %d 条**没有理由**,请逐条读源码判明后补上行尾注释:'
                  % len(missing))
            for k in missing:
                print('     ' + k)
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

    # 覆盖检查 —— 与行号检查性质不同:那个查"写下的引用对不对",这个查"该写的写了没"。
    uncovered = check_coverage(doc, src, args.coverage_min_lines)
    print('索引覆盖:>%d 行的 .cpp/.mm 中,无专属小节的 %d 个%s'
          % (args.coverage_min_lines, len(uncovered),
             (';例外 %d 条' % len(_COVERAGE_EXEMPT)) if _COVERAGE_EXEMPT else ''))
    for count, stem, rel in uncovered:
        print('FAIL 索引缺口:%s(%d 行)在 AI_INDEX.md 中没有专属 ### 小节 — %s'
              % (stem, count, rel))

    if fresh or uncovered:
        print('\n%d 处行号引用与源码脱节。修法:grep 出符号的真实位置后改文档,'
              '并逐个回读校验(sed -n "Np")。\n'
              '确实无法当场订正才用 --update-baseline 认账。' % len(fresh))
    if uncovered:
        print('\n%d 个大文件没有索引条目。写一节比加例外便宜得多;确需豁免请在'
              '脚本的 _COVERAGE_EXEMPT 里登记并**写明理由**。' % len(uncovered))
    if fresh or uncovered:
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
