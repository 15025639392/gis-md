"""标量距离场 vs 向量距离场:重建保真度数值对照。

忠实复刻实际实现:
  烘焙  标量: value = clamp(1 - dist/band, 0, 1),量化 R8
        向量: v/band 映射到 [0,1],量化 RG8(每分量 8bit)
  解算  标量: dist = (1 - v) * band
        向量: dist = |v| * band
  重建  双线性插值(GPU 纹理采样)
band = 8 texel。texelPx = 每个场纹素占多少屏幕像素(= 2^d)。
"""
import numpy as np

BAND = 8.0
SS = 8  # 每个场纹素细分成 SS×SS 个"屏幕像素"→ texelPx = SS

def true_distance_field(h, w, segments, scale):
    """真值:高分辨率网格上到最近线段的欧氏距离(单位=场纹素)。"""
    ys, xs = np.mgrid[0:h, 0:w]
    px = (xs + 0.5) / scale
    py = (ys + 0.5) / scale
    best = np.full((h, w), 1e9)
    bvx = np.zeros((h, w)); bvy = np.zeros((h, w))
    for (x0, y0, x1, y1) in segments:
        dx, dy = x1 - x0, y1 - y0
        L2 = dx * dx + dy * dy
        t = ((px - x0) * dx + (py - y0) * dy) / max(L2, 1e-12)
        t = np.clip(t, 0.0, 1.0)
        qx = x0 + t * dx - px
        qy = y0 + t * dy - py
        d = np.hypot(qx, qy)
        upd = d < best
        best = np.where(upd, d, best)
        bvx = np.where(upd, qx, bvx)   # 指向最近点的偏移向量
        bvy = np.where(upd, qy, bvy)
    return best, bvx, bvy

def bilinear(field, h, w, scale):
    """把低分场(texel 中心采样)双线性插值到高分屏幕网格。"""
    th, tw = field.shape[:2]
    ys, xs = np.mgrid[0:h, 0:w]
    fx = (xs + 0.5) / scale - 0.5     # 目标点在 texel 坐标系
    fy = (ys + 0.5) / scale - 0.5
    x0 = np.floor(fx).astype(int); y0 = np.floor(fy).astype(int)
    ax = (fx - x0)[..., None]; ay = (fy - y0)[..., None]
    c = lambda a, n: np.clip(a, 0, n - 1)
    f = field if field.ndim == 3 else field[..., None]
    v00 = f[c(y0, th), c(x0, tw)]; v10 = f[c(y0, th), c(x0 + 1, tw)]
    v01 = f[c(y0 + 1, th), c(x0, tw)]; v11 = f[c(y0 + 1, th), c(x0 + 1, tw)]
    top = v00 * (1 - ax) + v10 * ax
    bot = v01 * (1 - ax) + v11 * ax
    return top * (1 - ay) + bot * ay

def run_case(name, segments, tex=48):
    h = w = tex * SS
    # ---- 真值(屏幕分辨率)----
    dist_true, _, _ = true_distance_field(h, w, segments, SS)

    # ---- 烘焙(场纹素分辨率)----
    d_tex, vx_tex, vy_tex = true_distance_field(tex, tex, segments, 1.0)

    # 标量编码 + R8 量化
    scal = np.clip(1.0 - d_tex / BAND, 0, 1)
    scal = np.round(scal * 255) / 255.0
    # 向量编码 + RG8 量化(分量裁到 ±BAND)
    vx = np.clip(vx_tex / BAND, -1, 1); vy = np.clip(vy_tex / BAND, -1, 1)
    vx = np.round((vx * 0.5 + 0.5) * 255) / 255.0 * 2 - 1
    vy = np.round((vy * 0.5 + 0.5) * 255) / 255.0 * 2 - 1
    vec = np.stack([vx, vy], axis=-1)

    # ---- 重建(双线性)----
    d_scal = (1.0 - bilinear(scal, h, w, SS)[..., 0]) * BAND
    v_rec = bilinear(vec, h, w, SS)
    d_vec = np.hypot(v_rec[..., 0], v_rec[..., 1]) * BAND

    # ---- 只在关心的区域比较:真实距离 < 1.5 texel(线宽阈值量级)----
    m = dist_true < 1.5
    e_s = (d_scal - dist_true)[m]
    e_v = (d_vec - dist_true)[m]
    px = lambda e: e * SS  # texel 误差 → 屏幕像素(texelPx=SS)
    print(f'--- {name} (texelPx={SS}) ---')
    print(f'  标量场  偏差 均值{px(e_s).mean():+6.2f}px  |最大|{px(np.abs(e_s)).max():5.2f}px  '
          f'高估率{100*(e_s>0).mean():4.0f}%')
    print(f'  向量场  偏差 均值{px(e_v).mean():+6.2f}px  |最大|{px(np.abs(e_v)).max():5.2f}px  '
          f'低估率{100*(e_v<0).mean():4.0f}%')
    return dist_true, d_scal, d_vec

CASES = [
    ('单直线', [(4, 24, 44, 24)]),
    ('缓弯 R≈20texel', [(4 + i, 24 + (i - 20) ** 2 / 40.0, 5 + i,
                        24 + (i - 19) ** 2 / 40.0) for i in range(40)]),
    ('十字交叉', [(4, 24, 44, 24), (24, 4, 24, 44)]),
    ('尖角 90°', [(24, 4, 24, 24), (24, 24, 44, 24)]),
    ('平行近线 间距2texel', [(4, 23, 44, 23), (4, 25, 44, 25)]),
]
for nm, segs in CASES:
    run_case(nm, segs)
