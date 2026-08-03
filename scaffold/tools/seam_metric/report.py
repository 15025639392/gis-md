#!/usr/bin/env python3
"""接缝 A/B 判定器:聚合 collect.sh 的多轮截图,出指标与结论。

每轮三个指标:
  ① steady    稳态残余 = 序列尾段(后 40%)漏天像素最大值。**必须为 0**——
              这是"缝不存在"的定义(机制 B 后已达成,回归即 FAIL)。
  ② transSum  暂态总量 = 全序列漏天像素合计(爆发窗口的面积)。
  ③ transPeak 暂态峰值 = 单帧最大。

A/B 判定:
  ① after 任一轮 steady > 0            → FAIL(稳态回归,一票否决)
  ② transSum 中位数差 + 置换检验:超基线且 p<0.05 → FAIL;
     超基线但 p≥0.05 → INCONC(暂态随加载节奏剧烈波动,样本撑不住时
     不硬判,同 load_ab 的教训:硬发 FAIL 的判定器比没有更坏)。

用法:
    report.py out/before              # 单档汇总
    report.py out/before out/after    # A/B
"""
import itertools
import random
import statistics
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from seam_leak import analyze  # noqa: E402

TH_TRANS_REL = 1.20   # 暂态总量相对基线的劣化上限


def run_metrics(rundir):
    shots = sorted(Path(rundir).glob('s*.png'))
    if not shots:
        return None
    leaks = [analyze(p)['leak'] for p in shots]
    tail = leaks[int(len(leaks) * 0.6):]
    return dict(steady=max(tail) if tail else 0,
                trans_sum=sum(leaks), trans_peak=max(leaks), n=len(leaks))


def load_variant(d):
    runs = []
    for rd in sorted(Path(d).glob('run*')):
        if not rd.is_dir():
            continue
        if (rd / 'contaminated').exists():
            runs.append((rd.name, None, '真人触摸污染'))
            continue
        m = run_metrics(rd)
        runs.append((rd.name, m, None if m else '无截图'))
    return runs


def perm_p(xs, ys, iters=20000):
    """中位数差双侧置换检验(同 load_ab;小样本穷举精确 p)。"""
    obs = abs(statistics.median(ys) - statistics.median(xs))
    pool = list(xs) + list(ys)
    n = len(xs)
    total = 1
    for i in range(n):
        total = total * (len(pool) - i) // (i + 1)
    hits = tot = 0
    if total <= iters:
        for idx in itertools.combinations(range(len(pool)), n):
            sel = set(idx)
            a = [pool[i] for i in sel]
            b = [pool[i] for i in range(len(pool)) if i not in sel]
            hits += abs(statistics.median(b) - statistics.median(a)) >= obs - 1e-9
            tot += 1
        return hits / tot, True
    rng = random.Random(20260803)
    for _ in range(iters):
        rng.shuffle(pool)
        a, b = pool[:n], pool[n:]
        hits += abs(statistics.median(b) - statistics.median(a)) >= obs - 1e-9
    return hits / iters, False


def summarize(label, d):
    runs = load_variant(d)
    print(f'\n=== 档位 {label}  ({d}) ===')
    vals = {'steady': [], 'trans_sum': [], 'trans_peak': []}
    for name, m, err in runs:
        if m is None:
            print(f'  {name}: 剔除 —— {err}')
            continue
        print(f'  {name}: steady={m["steady"]:4d}  transSum={m["trans_sum"]:6d}  '
              f'transPeak={m["trans_peak"]:5d}  (n={m["n"]})')
        for k in vals:
            vals[k].append(m[k])
    if not vals['steady']:
        print('  ⚠️ 无有效轮')
        return None
    if len(vals['steady']) < 3:
        print(f'  ⚠️ 有效轮 {len(vals["steady"])} < 3,分布不可信')
    for k, v in vals.items():
        print(f'  {k:10s} 中位={statistics.median(v):8.1f}  [{min(v)}–{max(v)}]')
    return vals


def verdict(a, b):
    print('\n=== 判定(before → after) ===')
    ok = True
    bad_steady = [v for v in b['steady'] if v > 0]
    if bad_steady:
        print(f'  FAIL   ① 稳态残余    after 有 {len(bad_steady)} 轮 > 0 '
              f'(max={max(bad_steady)}) —— 稳态回归,一票否决')
        ok = False
    else:
        print('  PASS   ① 稳态残余    after 全轮 = 0')

    sa, sb = statistics.median(a['trans_sum']), statistics.median(b['trans_sum'])
    p, exact = perm_p(a['trans_sum'], b['trans_sum'])
    tag = '精确' if exact else '近似'
    if sb <= sa * TH_TRANS_REL:
        print(f'  PASS   ② 暂态总量    {sa:.0f} → {sb:.0f}  (≤{TH_TRANS_REL}× 基线)')
    elif p < 0.05:
        print(f'  FAIL   ② 暂态总量    {sa:.0f} → {sb:.0f}  超阈值且 p={p:.3f}{tag}')
        ok = False
    else:
        print(f'  INCONC ② 暂态总量    {sa:.0f} → {sb:.0f}  超阈值但 p={p:.3f}{tag} '
              f'—— 样本撑不住,加轮数(collect.sh 自动续号)')
        ok = None
    if ok is True:
        print('\n  总判定:通过')
    elif ok is False:
        print('\n  总判定:不通过')
    else:
        print('\n  总判定:未定')
    return ok is True


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    a = summarize('before', sys.argv[1])
    if len(sys.argv) < 3:
        return 0 if a else 1
    b = summarize('after', sys.argv[2])
    if not a or not b:
        return 1
    return 0 if verdict(a, b) else 1


if __name__ == '__main__':
    sys.exit(main())
