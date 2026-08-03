#!/usr/bin/env python3
"""金字塔层间嵌套误差 ε 实测。

背景(无缝北极星,docs/issues/terrain-seamless-northstar-2026-08-03.md §4.5):
机制 B 边吸附把细瓦片边顶点吸到**自数据**的 2^k 间距插值;它与粗邻居真正显示
的高度之差 = 父层瓦片与子层降采样在同一位置的差,即金字塔层间重采样差 ε。
现有管线(dem_test/build_raster_dem_tiles_fast.py)每层独立从源 VRT warp,
层间不嵌套 → ε ≠ 0。本工具从**线上源**(真机渲染的那份数据)实拉父子瓦片,
量出 ε 的真实分布,回答:
  - ε 有多大(米)?值不值得为它改数据管线?
  - 若改嵌套(父 = 子的精确 2× 降采样),边界残缝理论归零。

对齐约定:514 cell-registered + 1px 重叠环;像素 p∈[1,512] = cell p-1 中心;
父 cell i 中心对齐子 mosaic 的 2i+0.5(双轴)→ 父值 vs 子 2×2 均值。
Terrain-RGB 解码:h = -10000 + 0.1·(R·65536 + G·256 + B)。

用法:
    pyramid_eps.py [z x y]...     # 缺省:重庆山区一组代表瓦片
"""
import io
import sys
import urllib.request

import numpy as np
from PIL import Image

URL = 'https://mapoverlay.xinzhi.space/3dterrain/nasa/tiles/{z}/{x}/{y}.png'


def fetch(z, x, y):
    with urllib.request.urlopen(URL.format(z=z, x=x, y=y), timeout=30) as r:
        arr = np.asarray(
            Image.open(io.BytesIO(r.read())).convert('RGB'), dtype=np.float64)
    return -10000.0 + 0.1 * (
        arr[..., 0] * 65536 + arr[..., 1] * 256 + arr[..., 2])


def interior(h):
    """去掉 1px 重叠环,留 512×512 本体 cell。"""
    return h[1:513, 1:513]


def eps_stats(z, x, y):
    parent = interior(fetch(z, x, y))
    kids = np.empty((1024, 1024))
    for dy in (0, 1):
        for dx in (0, 1):
            kids[dy * 512:(dy + 1) * 512, dx * 512:(dx + 1) * 512] = \
                interior(fetch(z + 1, 2 * x + dx, 2 * y + dy))
    # 父 cell i 中心 = 子 mosaic (2i, 2i+1) 之间 → 2×2 均值
    down = kids.reshape(512, 2, 512, 2).mean(axis=(1, 3))
    eps = np.abs(parent - down)
    # 边界带(外圈 1 cell):吸附真正碰到的是边上的差
    edge = np.concatenate([eps[0, :], eps[-1, :], eps[:, 0], eps[:, -1]])
    return dict(
        z=z, x=x, y=y,
        relief=float(parent.max() - parent.min()),
        mean=float(eps.mean()), p95=float(np.percentile(eps, 95)),
        mx=float(eps.max()),
        edge_mean=float(edge.mean()), edge_p95=float(np.percentile(edge, 95)),
        edge_max=float(edge.max()))


def main():
    if len(sys.argv) > 1:
        args = [int(v) for v in sys.argv[1:]]
        tiles = [tuple(args[i:i + 3]) for i in range(0, len(args), 3)]
    else:
        # 重庆(106.5E, 29.6N)山区:z 越深瓦片越小。webmerc x = (lon+180)/360·2^z
        tiles = [
            (10, 815, 419),   # z10→11
            (11, 1630, 838),  # z11→12(demo 最深地形层)
            (11, 1631, 839),
        ]
    print(f'{"tile":>16s} {"relief":>7s} | {"全片 mean/p95/max":>24s} | '
          f'{"边界 mean/p95/max":>24s}  (米)')
    for z, x, y in tiles:
        try:
            s = eps_stats(z, x, y)
        except Exception as e:  # noqa: BLE001 —— 单瓦片失败不拦全局
            print(f'  {z}/{x}/{y}: 拉取失败 {e}')
            continue
        print(f'  {s["z"]:>3d}/{s["x"]}/{s["y"]}  {s["relief"]:7.0f} | '
              f'{s["mean"]:7.2f} {s["p95"]:7.2f} {s["mx"]:8.2f} | '
              f'{s["edge_mean"]:7.2f} {s["edge_p95"]:7.2f} {s["edge_max"]:8.2f}')


if __name__ == '__main__':
    main()
