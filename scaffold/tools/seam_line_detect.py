#!/usr/bin/env python3
"""地形瓦界亮线检测器(T-V10 用)。

为什么需要它:这类接缝的**逐像素对比度只有 1~7 个亮度单位**,肉眼能看见是因为
线很长很连贯,但任何逐像素阈值都测不到 —— 实测阈值 12 时对一条肉眼清晰的线
返回空,而"测不到"和"没有线"长得一模一样。

判据改为**沿线方向平均**:对每一列 x,取 lum(x) - (lum(x-6)+lum(x+6))/2 在数百
行上的均值。噪声被平掉,系统性的 +1.2 就浮出来;再按整幅的 σ 定显著性。

标定(2026-08-16,真机 1240x2772 截图):一条肉眼可见的竖线 → x=363,
超出 6.68,6.1σ。**改动本脚本后必须重跑这个阳性对照**,否则又回到
"空输出不是证据"的坑。

用法:
    python3 tools/seam_line_detect.py <png> [<png> ...]
    python3 tools/seam_line_detect.py --horizontal <png>   # 找横线

输出每幅图的显著峰 (x, 相对邻列超出, σ 倍数)。
"""
import os
import statistics as st
import sys

try:
    from PIL import Image
except ImportError:
    sys.exit("需要 Pillow: pip install Pillow")

# 显著性门槛(σ 倍)。4.0 能抓到标定线(6.1σ)与 9.8σ 的强线,
# 同时不把地形自身的亮条纹误报成接缝。
SIGMA_THRESHOLD = 4.0
# 与邻列比较的间距(px)。取 6 是因为线宽量级 ≈ 一个网格单元(z7 下约 7px),
# 间距太小会把线自己当成"邻居"从而自我抵消。
NEIGHBOR_OFFSET = 6


def _lum(p):
    return 0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2]


def detect(path, horizontal=False):
    im = Image.open(path).convert("RGB")
    if horizontal:
        im = im.transpose(Image.ROTATE_90)
    px = im.load()
    w, h = im.size
    # 上下各裁掉 ~18%/12%:避开罗盘、齿轮按钮与调试面板,它们是纯色块,
    # 会以极高显著性盖过真正的接缝。
    rows = list(range(int(h * 0.18), int(h * 0.88), 3))
    prof = []
    for x in range(NEIGHBOR_OFFSET + 2, w - NEIGHBOR_OFFSET - 2):
        d = [
            _lum(px[x, y])
            - (_lum(px[x - NEIGHBOR_OFFSET, y]) + _lum(px[x + NEIGHBOR_OFFSET, y])) / 2
            for y in rows
        ]
        prof.append((x, st.mean(d)))
    vals = [v for _, v in prof]
    med = st.median(vals)
    sd = st.pstdev(vals) or 1e-9
    hits = [(x, v) for x, v in prof if (v - med) / sd > SIGMA_THRESHOLD]
    runs = []
    for x, v in hits:
        if runs and x - runs[-1][-1][0] <= 4:
            runs[-1].append((x, v))
        else:
            runs.append([(x, v)])
    peaks = [max(r, key=lambda t: t[1]) for r in runs]
    return [(x, round(v, 2), round((v - med) / sd, 1)) for x, v in peaks], len(rows), sd


def main(argv):
    horizontal = "--horizontal" in argv
    files = [a for a in argv if not a.startswith("--")]
    if not files:
        sys.exit(__doc__)
    for f in files:
        peaks, n, sd = detect(f, horizontal)
        axis = "横线(y" if horizontal else "竖线(x"
        print(
            "%-22s n行=%d σ=%.2f → %s,超出,σ): %s"
            % (os.path.basename(f), n, sd, axis, peaks[:6] or "无")
        )


if __name__ == "__main__":
    main(sys.argv[1:])
