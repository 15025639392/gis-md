#!/usr/bin/env python3
"""「漏天缝」客观检测：数地平线以下的天色像素。

原理：一列像素从上往下扫，第一个非天色像素就是该列的地平线。地平线以下再出现
天色像素 = 地面里开了个洞/缝，天(或雾)从缝里透出来。这把"接缝观感"从肉眼判断
变成可计数指标，跨变体可比。

天色取每张图顶部 5% 的中位色（雾/天随视角变，不能写死常量）。
"""
import sys
from pathlib import Path
from PIL import Image


def sky_ref(px, w, h):
    band = [px[x, y] for y in range(0, max(1, h // 20), 3) for x in range(0, w, 37)]
    band.sort(key=lambda c: c[0] + c[1] + c[2])
    return band[len(band) // 2][:3]


def analyze(path, tol=14, skip_top=0.10):
    im = Image.open(path).convert('RGB')
    w, h = im.size
    px = im.load()
    sr, sg, sb = sky_ref(px, w, h)

    def is_sky(c):
        return (abs(c[0] - sr) <= tol and abs(c[1] - sg) <= tol
                and abs(c[2] - sb) <= tol)

    y0 = int(h * skip_top)          # 跳过顶部纯天区
    leak = 0
    cols = 0
    for x in range(0, w, 2):        # 隔列采样，够用且快 2×
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
    return dict(path=Path(path).name, leak=leak, cols=cols,
                per_col=leak / cols if cols else 0.0, sky=(sr, sg, sb))


def main():
    for d in sys.argv[1:]:
        p = Path(d)
        shots = sorted(p.glob('s*.png')) if p.is_dir() else [p]
        rows = [analyze(s) for s in shots]
        tot = sum(r['leak'] for r in rows)
        worst = max(rows, key=lambda r: r['leak']) if rows else None
        print(f'{p.name:10s} n={len(rows):2d}  漏天像素合计={tot:7d}  '
              f'每帧均值={tot/max(len(rows),1):8.1f}  '
              f'最差={worst["path"] if worst else "-"}({worst["leak"] if worst else 0})')


if __name__ == '__main__':
    main()
