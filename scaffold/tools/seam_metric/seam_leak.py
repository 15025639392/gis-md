#!/usr/bin/env python3
"""「漏天缝」客观检测：数地平线以下的天色像素。

原理：一列像素从上往下扫，第一个非天色像素就是该列的地平线。地平线以下再出现
天色像素 = 地面里开了个洞/缝，天(或雾)从缝里透出来。这把"接缝观感"从肉眼判断
变成可计数指标，跨变体可比。

天色取每张图顶部 5% 的中位色（雾/天随视角变，不能写死常量）。

⚠️ 前置闸（2026-08-08 补）：上面这个天色估计器**只在画面里真有天空时成立**。
若相机姿态没到掠视、画面全是地面，顶部 5% 取到的是山体色，于是整屏地面被判
成"漏天"——实测一次 15 万像素/帧的假阳性，比真值大三个数量级，而工具照常
输出一个漂亮的数字。这正是"健康态与故障态读数相同的指标比没有更糟"。

故每帧先过两条闸，任一不过判 **invalid**（不是记 0，也不是照常计数）：
  ① 天带纯度   顶部 5% 内匹配中位色的比例 ≥ SKY_PURITY_MIN。天/雾是平滑的
               （纯度≈1），山体是有纹理的（纯度低）。
  ② 地平线覆盖 找得到地平线的列占比 ≥ HORIZON_COLS_MIN。整屏皆天时无地面
               可测，记 0 是把"没测"伪装成"没缝"。
"""
import sys
from pathlib import Path
from PIL import Image

# 天带纯度下限。真天空/雾几乎恒为 1.0；实测误报帧（顶部是山体）在 0.3 以下，
# 中间地带很空，阈值取在 0.85 不敏感。
SKY_PURITY_MIN = 0.85
# 有地平线的列占比下限。正常掠视构图每列都有地平线（≈1.0）。
HORIZON_COLS_MIN = 0.50


def sky_band(px, w, h):
    return [px[x, y] for y in range(0, max(1, h // 20), 3) for x in range(0, w, 37)]


def analyze(path, tol=14, skip_top=0.10):
    im = Image.open(path).convert('RGB')
    w, h = im.size
    px = im.load()
    band = sky_band(px, w, h)
    ordered = sorted(band, key=lambda c: c[0] + c[1] + c[2])
    sr, sg, sb = ordered[len(ordered) // 2][:3]

    def is_sky(c):
        return (abs(c[0] - sr) <= tol and abs(c[1] - sg) <= tol
                and abs(c[2] - sb) <= tol)

    purity = sum(1 for c in band if is_sky(c)) / max(len(band), 1)

    y0 = int(h * skip_top)          # 跳过顶部纯天区
    leak = 0
    cols = 0
    sampled = 0
    for x in range(0, w, 2):        # 隔列采样，够用且快 2×
        sampled += 1
        horizon = None
        for y in range(y0, h):
            if not is_sky(px[x, y]):
                horizon = y
                break
        if horizon is None:
            continue                # 整列都是天（视野外），不计
        cols += 1
        # 地平线以下 30px 内不算：地平线本身有抗锯齿与雾过渡
        n = sum(1 for y in range(horizon + 30, h) if is_sky(px[x, y]))
        leak += n
    col_ratio = cols / max(sampled, 1)
    reason = None
    if purity < SKY_PURITY_MIN:
        reason = f'天带不纯 {purity:.2f}<{SKY_PURITY_MIN}(画面无天空?)'
    elif col_ratio < HORIZON_COLS_MIN:
        reason = f'地平线覆盖 {col_ratio:.2f}<{HORIZON_COLS_MIN}(画面无地面?)'
    return dict(path=Path(path).name, leak=leak, cols=cols,
                per_col=leak / cols if cols else 0.0, sky=(sr, sg, sb),
                purity=purity, col_ratio=col_ratio,
                valid=reason is None, reason=reason)


def main():
    for d in sys.argv[1:]:
        p = Path(d)
        shots = sorted(p.glob('s*.png')) if p.is_dir() else [p]
        rows = [analyze(s) for s in shots]
        good = [r for r in rows if r['valid']]
        bad = [r for r in rows if not r['valid']]
        tot = sum(r['leak'] for r in good)
        worst = max(good, key=lambda r: r['leak']) if good else None
        print(f'{p.name:10s} n={len(good):2d}/{len(rows):<2d}  漏天像素合计={tot:7d}  '
              f'每帧均值={tot/max(len(good),1):8.1f}  '
              f'最差={worst["path"] if worst else "-"}({worst["leak"] if worst else 0})')
        if bad:
            # 只报第一条原因:同一轮里失效原因几乎恒同源(姿态错了就整轮错),
            # 逐帧刷屏只会淹没它。
            print(f'{"":10s} ⚠️ {len(bad)} 帧无效 —— {bad[0]["reason"]}'
                  f'(首帧 {bad[0]["path"]})')


if __name__ == '__main__':
    main()
