"""D2 线段纹素场:单纹素单线容量(P7)量化 + 烘焙侧仲裁(方案 B)对照。

回答一个问题:**每个纹素只能记一条线段,真实路网上因此丢掉多少线?**
以及:**不加显存、不加 FS 开销的烘焙侧仲裁能补回多少?**

忠实复刻实际实现(改了实现请同步改这里,否则量出来的数没有意义):
  烘焙  LineFieldRasterizer.cpp  —— 逐段 scatter,纹素留 min-dist 胜者,
        RGBA8 = 偏移(±4 texel) | 方向角 θ∈[0,π) | fwd|back 端点余量(0.1 步长, ≤1.5)
  解算  PageStoreSamplingGLSL.h  —— 2×2 gather,各自解析胶囊距离取 min,无插值

结论(2026-08-15,详见 docs/northstar/vector.md C.6):**P7 不修**。
判据必须带 ±0.5px AA 容差——FS 本来就用 smoothstep 抗锯齿,
用硬阈值统计会把亚像素偏差算成漏画,虚高一个数量级(0.040% vs 0.380%)。

⚠️ 方案 B(烘焙侧仲裁,零显存零 FS)**已实测否掉:0.040% → 0.359%,负收益 9 倍**。
留在代码里是为了让后来者一跑就能复现这个否定结论,不是待启用的开关。
死因:要素级冗余 ≠ 几何级冗余——邻居纹素记的是同一条路的另一段,
复现不出本地几何;换冠军是零和的,让出去的比补回来的多。

用法:  python3 tools/line_field_capacity_sim.py [mbtiles] \\
           [--tiles N] [--texel-px 2^d] [--half-px 半宽]
"""
import argparse
import gzip
import math
import sqlite3
import sys

import numpy as np

# ---- 与 LineFieldRasterizer.h 对齐的常量 ----
OFF_RANGE = 4.0    # kLineFieldOffsetRangeTexels
CLAMP_MAX = 1.5    # kLineFieldClampMaxTexels
CLAMP_STEP = 0.1   # kLineFieldClampStepTexels
FIELD_SIZE = 256   # 场页边长(纹素)
MVT_EXTENT = 4096

ROAD_LAYERS = {'roads', 'road', 'transportation', 'highway', 'lines'}


# ============================ MVT 解码 ============================

def _varint(b, i):
    r = 0
    s = 0
    while True:
        x = b[i]
        i += 1
        r |= (x & 0x7F) << s
        s += 7
        if not x & 0x80:
            return r, i


def _fields(buf):
    """产出 (field_number, wire_type, payload) —— payload 为 bytes 或 int。"""
    i = 0
    n = len(buf)
    while i < n:
        key, i = _varint(buf, i)
        fn, wt = key >> 3, key & 7
        if wt == 2:
            ln, i = _varint(buf, i)
            yield fn, wt, buf[i:i + ln]
            i += ln
        elif wt == 0:
            v, i = _varint(buf, i)
            yield fn, wt, v
        elif wt == 5:
            i += 4
        elif wt == 1:
            i += 8
        else:
            raise ValueError(f'wire type {wt}')


def _decode_geometry(buf, extent):
    """MVT 几何命令 → 折线列表(单位:tile 坐标)。"""
    vals = []
    i = 0
    while i < len(buf):
        v, i = _varint(buf, i)
        vals.append(v)
    lines = []
    cur = []
    x = y = 0
    k = 0
    while k < len(vals):
        cmd = vals[k] & 7
        cnt = vals[k] >> 3
        k += 1
        if cmd == 1:      # MoveTo
            for _ in range(cnt):
                dx = (vals[k] >> 1) ^ -(vals[k] & 1)
                dy = (vals[k + 1] >> 1) ^ -(vals[k + 1] & 1)
                k += 2
                x += dx
                y += dy
                if len(cur) > 1:
                    lines.append(cur)
                cur = [(x, y)]
        elif cmd == 2:    # LineTo
            for _ in range(cnt):
                dx = (vals[k] >> 1) ^ -(vals[k] & 1)
                dy = (vals[k + 1] >> 1) ^ -(vals[k + 1] & 1)
                k += 2
                x += dx
                y += dy
                cur.append((x, y))
        else:             # ClosePath
            if cur:
                cur.append(cur[0])
    if len(cur) > 1:
        lines.append(cur)
    return lines


def load_tile_polylines(blob, only_roads=True):
    """返回 [(feature_id, [(x,y)...]), ...],坐标已归一到场纹素。"""
    data = gzip.decompress(blob) if blob[:2] == b'\x1f\x8b' else blob
    out = []
    fid = 0
    for fn, _, payload in _fields(data):
        if fn != 3:
            continue
        name = None
        extent = MVT_EXTENT
        feats = []
        for lfn, lwt, lp in _fields(payload):
            if lfn == 1 and lwt == 2:
                name = lp.decode('utf-8', 'replace')
            elif lfn == 5 and lwt == 0:
                extent = lp
            elif lfn == 2 and lwt == 2:
                feats.append(lp)
        if only_roads and name not in ROAD_LAYERS:
            continue
        scale = FIELD_SIZE / float(extent)
        for f in feats:
            gtype = 0
            geom = None
            for ffn, fwt, fp in _fields(f):
                if ffn == 3 and fwt == 0:
                    gtype = fp
                elif ffn == 4 and fwt == 2:
                    geom = fp
            if gtype != 2 or geom is None:   # 只要 LineString
                continue
            for line in _decode_geometry(geom, extent):
                out.append((fid, [(px * scale, py * scale) for px, py in line]))
            fid += 1
    return out


# ============================ 烘焙 ============================

class Baker:
    """留 top-2 的 scatter。冠军 = 实际实现的行为;亚军仅方案 B 用。"""

    def __init__(self, size=FIELD_SIZE):
        self.size = size
        n = size * size
        big = np.full(n, 1e18)
        # 两套记录:0=冠军 1=亚军。每套 (dist, ox, oy, ux, uy, fwd, back, feat)
        self.dist = [big.copy(), big.copy()]
        self.par = [np.zeros((n, 6)) for _ in range(2)]
        self.feat = [np.full(n, -1, dtype=np.int64) for _ in range(2)]

    def stamp(self, x0, y0, x1, y1, feat):
        size = self.size
        dx, dy = x1 - x0, y1 - y0
        L2 = dx * dx + dy * dy
        if L2 < 1e-12:
            return
        L = math.sqrt(L2)
        ux, uy = dx / L, dy / L
        # 方向规范化(与 stampSegment 一致):翻转须同步换端
        if uy < 0.0 or (uy == 0.0 and ux < 0.0):
            x0, y0, x1, y1 = x1, y1, x0, y0
            ux, uy = -ux, -uy

        lo_x = max(0, int(math.floor(min(x0, x1) - OFF_RANGE)))
        hi_x = min(size - 1, int(math.ceil(max(x0, x1) + OFF_RANGE)))
        lo_y = max(0, int(math.floor(min(y0, y1) - OFF_RANGE)))
        hi_y = min(size - 1, int(math.ceil(max(y0, y1) + OFF_RANGE)))
        if hi_x < lo_x or hi_y < lo_y:
            return

        px, py = np.meshgrid(np.arange(lo_x, hi_x + 1) + 0.5,
                             np.arange(lo_y, hi_y + 1) + 0.5)
        t = np.clip((px - x0) * ux + (py - y0) * uy, 0.0, L)
        qx = x0 + t * ux
        qy = y0 + t * uy
        d = np.hypot(qx - px, qy - py)
        m = d <= OFF_RANGE
        if not m.any():
            return
        fwd = L - t
        back = t

        idx = ((np.arange(lo_y, hi_y + 1)[:, None]) * self.size
               + np.arange(lo_x, hi_x + 1)[None, :])
        ii = idx[m]
        dd = d[m]
        rec = np.stack([(qx - px)[m], (qy - py)[m],
                        np.full(dd.shape, ux), np.full(dd.shape, uy),
                        fwd[m], back[m]], axis=-1)

        # 冠军/亚军插入(同要素不占两槽:同一条路的相邻段互为冗余)
        cur0 = self.dist[0][ii]
        f0 = self.feat[0][ii]
        win = dd < cur0
        same = f0 == feat
        # 情况1:成为新冠军 → 旧冠军降为亚军(若不同要素)
        w = win & ~same
        if w.any():
            jj = ii[w]
            self.dist[1][jj] = self.dist[0][jj]
            self.par[1][jj] = self.par[0][jj]
            self.feat[1][jj] = self.feat[0][jj]
        w2 = win
        if w2.any():
            jj = ii[w2]
            self.dist[0][jj] = dd[w2]
            self.par[0][jj] = rec[w2]
            self.feat[0][jj] = feat
        # 情况2:没赢冠军但可能赢亚军(且与冠军不同要素)
        lose = (~win) & (~same)
        if lose.any():
            jj = ii[lose]
            better = dd[lose] < self.dist[1][jj]
            if better.any():
                kk = jj[better]
                self.dist[1][kk] = dd[lose][better]
                self.par[1][kk] = rec[lose][better]
                self.feat[1][kk] = feat

    # ---- 方案 B:2×2 块覆盖仲裁 ----
    #
    # 换冠军是零和的:让给亚军 = 从冠军手里拿走。所以只在**可证无害**时换。
    # 像素 p 用的 2×2 块 = floor(p-0.5) 起的四格,故纹素 t 参与 4 个块
    # (t 分别当左上/右上/左下/右下)。判据:
    #   安全 = 这 4 个块**每一个**里,除 t 外还有别的纹素属于冠军要素
    #          → 换掉 t 不会让任何块失去冠军
    #   有益 = 这 4 个块里**至少一个**完全没有亚军要素
    #          → 该块里本该显示亚军的像素现在解不出来
    # 逐奇偶四趟应用:同一 2×2 块内四格奇偶各异,保证一块不会同时掉两个主。
    def _block_offsets(self):
        # 4 个块,各给出"块内除 t 外的 3 个偏移"
        for bx in (-1, 0):
            for by in (-1, 0):
                yield [(bx + i, by + j) for j in (0, 1) for i in (0, 1)
                       if (bx + i, by + j) != (0, 0)]

    def arbitrate(self):
        size = self.size
        total = 0
        for parity in ((0, 0), (0, 1), (1, 0), (1, 1)):
            feat0 = self.feat[0].reshape(size, size)
            feat1 = self.feat[1].reshape(size, size)
            d1 = self.dist[1].reshape(size, size)
            cand = (feat1 >= 0) & (d1 <= OFF_RANGE)
            ys, xs = np.mgrid[0:size, 0:size]
            cand &= (xs % 2 == parity[0]) & (ys % 2 == parity[1])
            if not cand.any():
                continue
            pad = np.pad(feat0, 2, constant_values=-2)

            def nb(dx, dy):
                return pad[2 + dy:2 + dy + size, 2 + dx:2 + dx + size]

            safe = np.ones((size, size), dtype=bool)
            gain = np.zeros((size, size), dtype=bool)
            for offs in self._block_offsets():
                has_owner = np.zeros((size, size), dtype=bool)
                has_runner = np.zeros((size, size), dtype=bool)
                for dx, dy in offs:
                    n = nb(dx, dy)
                    has_owner |= (n == feat0)
                    has_runner |= (n == feat1)
                safe &= has_owner
                gain |= ~has_runner
            swap = cand & safe & gain
            if not swap.any():
                continue
            s = swap.reshape(-1)
            for a in (self.dist, self.feat):
                a[0][s], a[1][s] = a[1][s].copy(), a[0][s].copy()
            p0, p1 = self.par[0][s].copy(), self.par[1][s].copy()
            self.par[0][s], self.par[1][s] = p1, p0
            total += int(swap.sum())
        return total

    def encode(self, quantize=True, clamp=True):
        """→ FS 实际看到的参数(解码后)。quantize/clamp 关掉用于误差归因。"""
        size = self.size
        live = self.dist[0] <= OFF_RANGE
        p = self.par[0]
        ox, oy, ux, uy, fwd, back = (p[:, k].copy() for k in range(6))
        th = np.mod(np.arctan2(uy, ux), math.pi)
        if clamp:
            fwd = np.minimum(fwd, CLAMP_MAX)
            back = np.minimum(back, CLAMP_MAX)
        if quantize:
            q = lambda v: (np.round(np.clip(v / OFF_RANGE * 0.5 + 0.5, 0, 1)
                                    * 255) / 255.0 * 2 - 1) * OFF_RANGE
            ox, oy = q(ox), q(oy)
            th = np.clip(np.round(th / math.pi * 255), 0, 255) / 255.0 * math.pi
            fwd = np.clip(np.round(fwd / CLAMP_STEP), 0, 15) * CLAMP_STEP
            back = np.clip(np.round(back / CLAMP_STEP), 0, 15) * CLAMP_STEP
            deg = live & (fwd == 0) & (back == 0)   # A==0 是空哨兵 → 提升 1 级
            back = np.where(deg, CLAMP_STEP, back)
        r = lambda v: v.reshape(size, size)
        return dict(ox=r(ox), oy=r(oy), th=r(th), fwd=r(fwd), back=r(back),
                    live=r(live))


# ============================ 解算(复刻 FS)============================

def reconstruct(enc, ss):
    """2×2 gather-min。返回屏幕网格上的重建距离(单位:texel)。"""
    size = enc['ox'].shape[0]
    n = size * ss
    coord = (np.arange(n) + 0.5) / ss           # 屏幕采样点的 texel 坐标
    ptx, pty = np.meshgrid(coord, coord)
    g0x = np.floor(ptx - 0.5).astype(np.int64)
    g0y = np.floor(pty - 0.5).astype(np.int64)
    best = np.full((n, n), 1e9)
    for j in range(2):
        for i in range(2):
            tx = np.clip(g0x + i, 0, size - 1)
            ty = np.clip(g0y + j, 0, size - 1)
            live = enc['live'][ty, tx]
            qx = tx + 0.5 + enc['ox'][ty, tx]
            qy = ty + 0.5 + enc['oy'][ty, tx]
            th = enc['th'][ty, tx]
            dx, dy = np.cos(th), np.sin(th)
            fwd = enc['fwd'][ty, tx]
            bck = enc['back'][ty, tx]
            pqx, pqy = ptx - qx, pty - qy
            d_tan = dx * pqx + dy * pqy
            d_nrm = -dy * pqx + dx * pqy
            ex = np.maximum(np.maximum(d_tan - fwd, -d_tan - bck), 0.0)
            d = np.hypot(d_nrm, ex)
            best = np.where(live, np.minimum(best, d), best)
    return best


def truth(polylines, size, ss):
    """真值距离(texel)+ 最近要素 id,逐段局部窗口累积 min。"""
    n = size * ss
    best = np.full((n, n), 1e9)
    who = np.full((n, n), -1, dtype=np.int64)
    for feat, pts in polylines:
        for k in range(len(pts) - 1):
            x0, y0 = pts[k]
            x1, y1 = pts[k + 1]
            dx, dy = x1 - x0, y1 - y0
            L2 = dx * dx + dy * dy
            if L2 < 1e-12:
                continue
            lo_x = max(0, int((min(x0, x1) - OFF_RANGE) * ss))
            hi_x = min(n - 1, int((max(x0, x1) + OFF_RANGE) * ss) + 1)
            lo_y = max(0, int((min(y0, y1) - OFF_RANGE) * ss))
            hi_y = min(n - 1, int((max(y0, y1) + OFF_RANGE) * ss) + 1)
            if hi_x < lo_x or hi_y < lo_y:
                continue
            px, py = np.meshgrid((np.arange(lo_x, hi_x + 1) + 0.5) / ss,
                                 (np.arange(lo_y, hi_y + 1) + 0.5) / ss)
            t = np.clip(((px - x0) * dx + (py - y0) * dy) / L2, 0.0, 1.0)
            d = np.hypot(x0 + t * dx - px, y0 + t * dy - py)
            w = best[lo_y:hi_y + 1, lo_x:hi_x + 1]
            u = who[lo_y:hi_y + 1, lo_x:hi_x + 1]
            upd = d < w
            u[upd] = feat
            np.minimum(w, d, out=w)
    return best, who


# ============================ 主流程 ============================

def build(polylines, arbitrate=False):
    bk = Baker()
    for feat, pts in polylines:
        for k in range(len(pts) - 1):
            bk.stamp(pts[k][0], pts[k][1], pts[k + 1][0], pts[k + 1][1], feat)
    swapped = bk.arbitrate() if arbitrate else 0
    return bk.encode_rgba8(), swapped


def attribute(dist_true, dist_rec, who, baker, ss, half_texel, tol=0.0):
    """漏画归因:真最近要素**是否出现在**该像素的 2×2 块里。

    不在 → 容量(纹素被别的线垄断,P7 的定义)
    在   → 编码/端点(要素在场但参数解不出正确距离)
    """
    size = baker.size
    feat0 = baker.feat[0].reshape(size, size)
    miss = (dist_true <= half_texel - tol) & (dist_rec > half_texel + tol)
    if not miss.any():
        return 0.0, 0.0
    n = size * ss
    coord = (np.arange(n) + 0.5) / ss
    ptx, pty = np.meshgrid(coord, coord)
    g0x = np.floor(ptx - 0.5).astype(np.int64)
    g0y = np.floor(pty - 0.5).astype(np.int64)
    present = np.zeros_like(miss)
    for j in range(2):
        for i in range(2):
            tx = np.clip(g0x + i, 0, size - 1)
            ty = np.clip(g0y + j, 0, size - 1)
            present |= (feat0[ty, tx] == who)
    tot = int(miss.sum())
    inbox = int((miss & present).sum())
    return 100.0 * (tot - inbox) / tot, 100.0 * inbox / tot


def score(dist_true, dist_rec, half_texel, tol=0.0):
    """tol = 容差(texel)。FS 用 smoothstep(w+0.5px, w-0.5px) 做 AA,
    落在过渡带内的差异本就不可见,所以只统计**穿透 AA 带**的错误。"""
    inside = dist_true <= half_texel - tol
    outside = dist_true > half_texel + tol
    miss = inside & (dist_rec > half_texel + tol)
    ghost = outside & (dist_rec <= half_texel - tol)
    npx = max(int((dist_true <= half_texel).sum()), 1)
    return (100.0 * miss.sum() / npx,
            100.0 * ghost.sum() / npx,
            npx)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('mbtiles', nargs='?',
                    default='tmp/osm/chongqing/chongqing.mbtiles')
    ap.add_argument('--tiles', type=int, default=3, help='取最密的前 N 个 z14 瓦')
    ap.add_argument('--texel-px', type=int, default=4,
                    help='一个场纹素占多少屏幕像素(=2^d,真机 92% 画面 d=2)')
    ap.add_argument('--half-px', type=float, default=2.0, help='线半宽(屏幕像素)')
    args = ap.parse_args()

    ss = args.texel_px
    half_texel = args.half_px / args.texel_px
    con = sqlite3.connect(args.mbtiles)
    rows = con.execute(
        'select tile_data from tiles where zoom_level=14 '
        'order by length(tile_data) desc limit ?', (args.tiles,)).fetchall()

    print(f'场 {FIELD_SIZE}² | texelPx={ss} | 半宽 {args.half_px}px '
          f'= {half_texel:.3f} texel | 屏幕网格 {FIELD_SIZE*ss}²\n'
          f'判据:只统计穿透 ±0.5px AA 带的错误')
    tot = np.zeros(5)
    for ti, (blob,) in enumerate(rows):
        pls = load_tile_polylines(blob)
        if not pls:
            pls = load_tile_polylines(blob, only_roads=False)
        nseg = sum(len(p) - 1 for _, p in pls)
        if nseg == 0:
            print(f'  瓦{ti}: 无线要素,跳过')
            continue
        dt, who = truth(pls, FIELD_SIZE, ss)
        bk = Baker()
        for feat, pts in pls:
            for k in range(len(pts) - 1):
                bk.stamp(pts[k][0], pts[k][1],
                         pts[k + 1][0], pts[k + 1][1], feat)
        print(f'  瓦{ti}: {len(pls)}线 {nseg}段', end='')
        bk_b = Baker()
        for feat, pts in pls:
            for k in range(len(pts) - 1):
                bk_b.stamp(pts[k][0], pts[k][1],
                           pts[k + 1][0], pts[k + 1][1], feat)
        swapped = bk_b.arbitrate()
        base = None
        bm = 0.0
        for tag, qz, cl, who_bk in (('实现现状', True, True, bk),
                                    ('去量化  ', False, True, bk),
                                    ('去端点夹', True, False, bk),
                                    ('二者全去', False, False, bk),
                                    ('仲裁B  ', True, True, bk_b)):
            rec = reconstruct(who_bk.encode(quantize=qz, clamp=cl), ss)
            m, g, npx = score(dt, rec, half_texel, tol=0.5 / ss)
            if base is None:
                base = (m, g, npx)
                print(f' | 应画像素 {npx}')
                cap, en = attribute(dt, rec, who, bk, ss, half_texel,
                                    tol=0.5 / ss)
            extra = f'   改判纹素 {swapped}' if who_bk is bk_b else ''
            if who_bk is bk_b:
                bm = m
            print(f'      {tag}  漏画 {m:6.3f}%   幽灵 {g:6.3f}%{extra}')
        ma, ga, npx = base
        print(f'      现状漏画归因: 容量(要素不在 2×2 内) {cap:5.1f}%   '
              f'其余(要素在但解不准) {en:5.1f}%')
        tot += np.array([ma * npx, ga * npx, bm * npx, 0, npx])
    if tot[4] > 0:
        print(f'  ── 加权合计: 现状漏画 {tot[0]/tot[4]:.3f}%  '
              f'幽灵 {tot[1]/tot[4]:.3f}%  |  仲裁B 漏画 {tot[2]/tot[4]:.3f}%')


if __name__ == '__main__':
    sys.exit(main())
