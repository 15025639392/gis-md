#!/usr/bin/env python3
"""加载期 A/B 判定器：从 collect.sh 采到的 logcat 算指标、出结论。

判定归机器不归肉眼。三条二元判据（详见 tools/load_ab/README.md）：

  ① 交互期积压峰值 pendUp      ↓   收益
  ② 停手 → pendUp 归零时长      ↓   收益
  ③ 交互期慢帧率(帧/秒)         不劣化   代价闸

①② 是"停手后暂态"的直接量化，③ 是放开交互期上传要付的帧时代价。
三条同时满足才算过。

⚠️ 用慢帧**计数**而不是均值/最大帧时：真机单次 run 有 ±2× DVFS 调频噪声
   （见 device-dvfs-frame-time-noise），均值类指标会给出随机结论。

用法：
    report.py out/before                 # 单档汇总
    report.py out/before out/after       # A/B 对比 + 判定
"""
import datetime
import re
import statistics
import sys
from pathlib import Path

# FrameLoop 只在 total>=25ms（或心跳/首帧）时打行 —— 所以"慢帧"只能定义成
# 引擎自己已经在记的这个阈值，不能凭空定 16.67ms（那些帧根本没落盘）。
SLOW_FRAME_MS = 25.0

# 判定阈值。①② 是绝对目标（基线 49 / 0.71s），③ 是相对劣化上限。
TH_PEAK = 10
TH_SETTLE_S = 0.20
TH_SLOW_REL = 1.20

GATE_RE = re.compile(
    r'^(\d\d-\d\d \d\d:\d\d:\d\d\.\d+).*LoadGate frame=(\d+) fin=(\d+)/(\d+) '
    r'rasUp=(\d+)/(\d+) ms=[\d.]+/[\d.]+ pend=\d+/(\d+)/\d+ '
    r'net=\S+ inflt=\S+ prog=[\d.]+ mode=(..)')
FRAME_RE = re.compile(
    r'^(\d\d-\d\d \d\d:\d\d:\d\d\.\d+).*FrameLoop frame=(\d+) total=([\d.]+)')


def ts(s):
    return datetime.datetime.strptime('2026-' + s, '%Y-%m-%d %H:%M:%S.%f')


class RunMetrics:
    """一轮的指标。任一项为 None = 该轮无效，report 会剔除并说明原因。"""

    def __init__(self, path):
        self.path = path
        self.error = None
        self.peak_pend = None
        self.settle_s = None
        self.slow_rate = None
        self.interaction_s = None
        self.fin_used_frames = None
        self._parse()

    def _parse(self):
        gates, frames = [], []
        for line in open(self.path, errors='ignore'):
            m = GATE_RE.match(line)
            if m:
                g = m.groups()
                gates.append(dict(t=ts(g[0]), fin=int(g[2]), fin_max=int(g[3]),
                                  pend=int(g[6]), mode=g[7]))
                continue
            m = FRAME_RE.match(line)
            if m:
                frames.append((ts(m.group(1)), float(m.group(3))))

        if not gates:
            self.error = '无 LoadGate 行'
            return

        inter = [g for g in gates if g['mode'][0] == 'I']
        if not inter:
            self.error = '未捕获到交互期（mode=I）—— 输入没送达？'
            return
        i_start, i_end = inter[0]['t'], inter[-1]['t']
        self.interaction_s = (i_end - i_start).total_seconds()

        # ① 交互期积压峰值
        self.peak_pend = max(g['pend'] for g in inter)

        # ② 停手 → 归零。取最后一个 mode=I 之后首次 pend==0。
        after = [g for g in gates if g['t'] >= i_end]
        zero = next((g for g in after if g['pend'] == 0), None)
        if zero is None:
            self.error = f'排空窗口内 pendUp 未归零（末值 {after[-1]["pend"] if after else "?"}）'
            return
        self.settle_s = (zero['t'] - i_end).total_seconds()

        # ③ 交互期慢帧率（帧/秒）。除以时长而非帧数 —— FrameLoop 只落慢帧，
        #    没有"总帧数"分母可用。
        slow = [f for f in frames if i_start <= f[0] <= i_end and f[1] >= SLOW_FRAME_MS]
        self.slow_rate = len(slow) / max(self.interaction_s, 1e-6)

        # 机制信号（不参与判定，用来证明"闸真的放开了"）
        self.fin_used_frames = sum(1 for g in inter if g['fin'] > 0)

    @property
    def valid(self):
        return self.error is None


def load_variant(d):
    d = Path(d)
    runs = []
    for log in sorted(d.glob('run*.log')):
        if log.with_suffix('').with_name(log.stem + '.contaminated').exists():
            runs.append((log.name, None, '真人触摸污染'))
            continue
        m = RunMetrics(log)
        runs.append((log.name, m if m.valid else None, m.error))
    return runs


def agg(runs, attr):
    vals = [getattr(m, attr) for _, m, _ in runs if m is not None]
    if not vals:
        return None
    return dict(median=statistics.median(vals), lo=min(vals), hi=max(vals), n=len(vals))


def fmt(a, unit='', prec=2):
    if a is None:
        return 'n/a'
    return (f'{a["median"]:.{prec}f}{unit} '
            f'[{a["lo"]:.{prec}f}–{a["hi"]:.{prec}f}] n={a["n"]}')


def summarize(label, d):
    runs = load_variant(d)
    print(f'\n=== 档位 {label}  ({d}) ===')
    for name, m, err in runs:
        if m is None:
            print(f'  {name}: 剔除 —— {err}')
        else:
            print(f'  {name}: 交互 {m.interaction_s:.2f}s  峰值 {m.peak_pend:3d}  '
                  f'排空 {m.settle_s:.2f}s  慢帧 {m.slow_rate:.2f}/s  '
                  f'（交互期 fin>0 的帧 {m.fin_used_frames}）')
    ok = sum(1 for _, m, _ in runs if m is not None)
    if ok == 0:
        print('  ⚠️ 全部无效，无法汇总')
        return None
    if ok < 3:
        print(f'  ⚠️ 有效轮只有 {ok} 轮，分布不可信（建议 ≥5）')
    res = {k: agg(runs, k) for k in
           ('peak_pend', 'settle_s', 'slow_rate', 'fin_used_frames')}
    print(f'  ① 积压峰值   {fmt(res["peak_pend"], "", 1)}')
    print(f'  ② 排空时长   {fmt(res["settle_s"], "s")}')
    print(f'  ③ 慢帧率     {fmt(res["slow_rate"], "/s")}')
    print(f'     交互期 fin>0 帧数 {fmt(res["fin_used_frames"], "", 1)}  '
          f'(基线应为 0 —— 非 0 说明交互期上传闸已放开)')
    return res


def verdict(a, b):
    print('\n=== 判定（before → after） ===')
    rows = []
    p_a, p_b = a['peak_pend']['median'], b['peak_pend']['median']
    rows.append(('① 积压峰值', f'{p_a:.0f} → {p_b:.0f}',
                 p_b <= TH_PEAK, f'目标 ≤{TH_PEAK}'))
    s_a, s_b = a['settle_s']['median'], b['settle_s']['median']
    rows.append(('② 排空时长', f'{s_a:.2f}s → {s_b:.2f}s',
                 s_b <= TH_SETTLE_S, f'目标 ≤{TH_SETTLE_S}s'))
    f_a, f_b = a['slow_rate']['median'], b['slow_rate']['median']
    if f_a <= 1e-9:
        ok3 = f_b <= 1e-9
        note = '基线慢帧率为 0，任何新增慢帧都算劣化'
    else:
        ok3 = f_b <= f_a * TH_SLOW_REL
        note = f'目标 ≤{TH_SLOW_REL:.2f}× 基线 ({f_a * TH_SLOW_REL:.2f}/s)'
    rows.append(('③ 慢帧率', f'{f_a:.2f}/s → {f_b:.2f}/s', ok3, note))

    for name, delta, ok, note in rows:
        print(f'  {"PASS" if ok else "FAIL"}  {name:10s} {delta:24s} {note}')

    allok = all(r[2] for r in rows)
    print(f'\n  总判定：{"通过 —— 收益拿到且未付超额帧时代价" if allok else "不通过"}')
    if not allok and rows[2][2] is False:
        print('  → ③ 挂了 = 涓流额度给太宽，收紧后重跑（不要直接放弃修法）')
    if not allok and not rows[0][2]:
        print('  → ① 挂了 = 闸没真放开，先看"交互期 fin>0 帧数"是否仍为 0')
    print('\n  剩下唯一留给人的：三条全过后自己拖一下，若手感明显变粘 = ③ 阈值定松了。')
    return allok


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    a = summarize('before', sys.argv[1])
    if len(sys.argv) < 3:
        return 0 if a else 1
    b = summarize('after', sys.argv[2])
    if not a or not b:
        print('\n有档位无有效数据，无法判定')
        return 1
    return 0 if verdict(a, b) else 1


if __name__ == '__main__':
    sys.exit(main())
