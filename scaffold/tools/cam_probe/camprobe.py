#!/usr/bin/env python3
# 解析 CAMPROBE 行,用 VP 矩阵把 anchor(ECEF)投影到像素 = "实际落点",
# 对比 finger 像素 = "本该落点" → 逐帧 anchorErr(px)。按手势聚合出峰值。
#   用法: adb logcat -d -s CAMPROBE | python3 campробe.py [--curve]
import sys, re, math

SHOW_CURVE = "--curve" in sys.argv

def parse(line):
    m = re.search(r'(\w+) finger=\(([\d.\-]+),([\d.\-]+)\) vp=(\d+)x(\d+) '
                  r'(?:eyeAlt=([\d.\-]+) )?'
                  r'anchor=(\d),([\de.\-]+),([\de.\-]+),([\de.\-]+) vpm=([\de.,\-]+)', line)
    if not m: return None
    vpm = [float(x) for x in m.group(11).split(',')]
    return dict(phase=m.group(1), fx=float(m.group(2)), fy=float(m.group(3)),
                vw=int(m.group(4)), vh=int(m.group(5)),
                eyeAlt=(float(m.group(6)) if m.group(6) else None),
                hasA=m.group(7)=='1',
                a=(float(m.group(8)), float(m.group(9)), float(m.group(10))), vpm=vpm)

def project(vpm, a, vw, vh):
    v = (a[0], a[1], a[2], 1.0)
    clip = [sum(vpm[4*c+r]*v[c] for c in range(4)) for r in range(4)]
    cw = clip[3]
    if abs(cw) < 1e-12: return None
    px = (clip[0]/cw*0.5 + 0.5) * vw
    py = (1.0 - (clip[1]/cw*0.5 + 0.5)) * vh
    return px, py

def anchor_err(d):
    pr = project(d['vpm'], d['a'], d['vw'], d['vh'])
    if pr is None: return None, None
    return pr, math.hypot(pr[0]-d['fx'], pr[1]-d['fy'])

# 按手势分组:*Start 开新组
gestures, cur = [], None
for line in sys.stdin:
    d = parse(line)
    if not d: continue
    if d['phase'].endswith('Start'):
        cur = []; gestures.append(cur)
    if cur is None:
        cur = []; gestures.append(cur)
    cur.append(d)

for g in gestures:
    if not g: continue
    kind = g[0]['phase'].replace('Start','')
    errs = []
    for d in g:
        if not d['hasA']:
            if SHOW_CURVE: print(f"  {d['phase']:11s} (no anchor / miss)")
            continue
        pr, e = anchor_err(d)
        errs.append((d['phase'], d['fx'], d['fy'], pr, e))
        if SHOW_CURVE:
            print(f"  {d['phase']:11s} 本该=({d['fx']:7.1f},{d['fy']:7.1f}) "
                  f"实际=({pr[0]:7.1f},{pr[1]:7.1f}) err={e:6.2f}px")
    if not errs:
        print(f"[{kind}] 无有效锚点帧(全 miss)"); continue
    emax = max(errs, key=lambda t: t[4])
    alts = [d['eyeAlt'] for d in g if d.get('eyeAlt') is not None]
    altstr = f" 最低eyeAlt={min(alts):.1f}m" if alts else ""
    # drag 增益:实际位移 / 本该位移(首→末锚点投影 vs 首→末手指)
    s, e = errs[0], errs[-1]
    fdisp = math.hypot(e[1]-s[1], e[2]-s[2])
    adisp = math.hypot(e[3][0]-s[3][0], e[3][1]-s[3][1])
    gain = adisp/fdisp if fdisp > 1e-6 else float('nan')
    print(f"[{kind}] 帧={len(errs)}  anchorErr: 末={e[4]:.2f}px 峰={emax[4]:.2f}px"
          f"(@{emax[0]})  本该位移={fdisp:.1f}px 实际位移={adisp:.1f}px 增益={gain:.3f}{altstr}")
