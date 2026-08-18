#!/usr/bin/env python3
# 解析 CAMPROBE 行:
#  - anchorErr(C-V1/C-V3):VP 投影 anchor=实际落点,对比 finger=本该落点。
#  - 轴隔离(C-V2):锚点 ENU 帧的 (heading,pitch,range) 前后差,看只动哪个轴。
#  - 惯性收敛(C-V4):tick 期 inertiaVel 衰减曲线,单调降到停。
#   用法: adb logcat -d -s CAMPROBE | python3 camprobe.py [--curve]
import sys, re, math

SHOW_CURVE = "--curve" in sys.argv

LINE = re.compile(
    r'(?P<phase>\w+) finger=\((?P<fx>[\d.\-]+),(?P<fy>[\d.\-]+)\) '
    r'vp=(?P<vw>\d+)x(?P<vh>\d+) '
    r'(?:eyeAlt=(?P<eyeAlt>[\d.\-]+) )?'
    r'(?:hdg=(?P<hdg>[\d.\-]+) pit=(?P<pit>[\d.\-]+) roll=(?P<roll>[\d.\-]+) '
    r'range=(?P<range>[\d.\-]+) inertiaVel=(?P<iv>[\de.\-]+)'
    r'(?: nearVel=(?P<nv>[\de.\-]+))? )?'
    r'anchor=(?P<hasA>\d),(?P<ax>[\de.\-]+),(?P<ay>[\de.\-]+),(?P<az>[\de.\-]+) '
    r'vpm=(?P<vpm>[\de.,\-]+)')

def fnum(s): return float(s) if s is not None else None

def parse(line):
    m = LINE.search(line)
    if not m: return None
    g = m.groupdict()
    return dict(phase=g['phase'], fx=float(g['fx']), fy=float(g['fy']),
                vw=int(g['vw']), vh=int(g['vh']), eyeAlt=fnum(g['eyeAlt']),
                hdg=fnum(g['hdg']), pit=fnum(g['pit']), range=fnum(g['range']),
                iv=fnum(g['iv']), nv=fnum(g.get('nv')), hasA=g['hasA']=='1',
                a=(float(g['ax']), float(g['ay']), float(g['az'])),
                vpm=[float(x) for x in g['vpm'].split(',')])

def project(vpm, a, vw, vh):
    v = (a[0], a[1], a[2], 1.0)
    clip = [sum(vpm[4*c+r]*v[c] for c in range(4)) for r in range(4)]
    cw = clip[3]
    if abs(cw) < 1e-12: return None
    return ((clip[0]/cw*0.5 + 0.5) * vw, (1.0 - (clip[1]/cw*0.5 + 0.5)) * vh)

def dhdg(a, b):
    d = (b - a + 180.0) % 360.0 - 180.0   # 环绕归一到 (-180,180]
    return d

# 按手势分组:*Start 开新组;inertia 帧附到当前组(fling 尾巴)
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
    kind = g[0]['phase'].replace('Start', '')
    anchored = [d for d in g if d['hasA']]
    inertia = [d for d in g if d['phase'] == 'inertia' and d['iv'] is not None]

    # ---- C-V1/C-V3 anchorErr + 增益 ----
    if anchored:
        errs = []
        for d in anchored:
            pr = project(d['vpm'], d['a'], d['vw'], d['vh'])
            if pr is None: continue
            e = math.hypot(pr[0]-d['fx'], pr[1]-d['fy'])
            errs.append((d['phase'], d['fx'], d['fy'], pr, e))
            if SHOW_CURVE:
                print(f"  {d['phase']:11s} 本该=({d['fx']:7.1f},{d['fy']:7.1f}) "
                      f"实际=({pr[0]:7.1f},{pr[1]:7.1f}) err={e:6.2f}px")
        if errs:
            emax = max(errs, key=lambda t: t[4])
            s, e = errs[0], errs[-1]
            fdisp = math.hypot(e[1]-s[1], e[2]-s[2])
            adisp = math.hypot(e[3][0]-s[3][0], e[3][1]-s[3][1])
            gain = adisp/fdisp if fdisp > 1e-6 else float('nan')
            alts = [d['eyeAlt'] for d in g if d.get('eyeAlt') is not None]
            altstr = f" 最低eyeAlt={min(alts):.1f}m" if alts else ""
            print(f"[{kind}] 帧={len(errs)} anchorErr:末={e[4]:.2f} 峰={emax[4]:.2f}px"
                  f"(@{emax[0]}) 本该位移={fdisp:.1f} 实际位移={adisp:.1f}px "
                  f"增益={gain:.3f}{altstr}")

        # ---- C-V2 轴隔离:锚点帧首→末的 Δhdg/Δpit/Δrange ----
        pose = [d for d in anchored if d['hdg'] is not None]
        if len(pose) >= 2:
            s, e = pose[0], pose[-1]
            dH = dhdg(s['hdg'], e['hdg'])
            dP = e['pit'] - s['pit']
            dR = e['range'] - s['range']
            rpct = 100.0*dR/s['range'] if s['range'] > 1 else float('nan')
            axes = [('heading', abs(dH)/2.0), ('pitch', abs(dP)/2.0),
                    ('range', abs(rpct)/2.0)]  # 各按"显著阈值"归一
            dom = max(axes, key=lambda t: t[1])[0]
            print(f"       轴Δ: Δheading={dH:+.3f}° Δpitch={dP:+.3f}° "
                  f"Δrange={dR:+.1f}m({rpct:+.2f}%)  主轴≈{dom}")

    # ---- C-V4 惯性收敛(两条路径)----
    # flick(Space):inertiaVel 指数衰减到停;近地(NearGround):nearVel 恒速滑行停
    flick = [d['iv'] for d in g if d['phase'] == 'inertia' and d['iv'] is not None]
    near = [d['nv'] for d in g if d['phase'] == 'inertiaNear' and d.get('nv') is not None]
    if flick and max(flick) > 0:
        nm = sum(1 for i in range(1, len(flick)) if flick[i] > flick[i-1] + 1e-9)
        print(f"[{kind}→flick惯性] 帧={len(flick)} 峰={max(flick):.4f}rad/s 末={flick[-1]:.4f} "
              f"单调降={'是' if nm==0 else f'否({nm}处回升)'} 收敛停={'是' if flick[-1]==0 else '否'}")
        if SHOW_CURVE: print("       vel: " + " ".join(f"{v:.3f}" for v in flick))
    if near and max(near) > 0:
        # 近地恒速:检查不回升(锁向只缩不放)+ 末帧后停(nearInertiaActive 转 false)
        rise = sum(1 for i in range(1, len(near)) if near[i] > near[i-1] + 1e-6)
        print(f"[{kind}→近地惯性] 帧={len(near)} 峰={max(near):.1f}px/s 末={near[-1]:.1f} "
              f"不回升={'是' if rise==0 else f'否({rise}处)'} (恒速滑行→horizon裁剪停)")
        if SHOW_CURVE: print("       vel: " + " ".join(f"{v:.1f}" for v in near))
