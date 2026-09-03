#!/usr/bin/env python3
"""Analyse a 3DGS .ply: find the background sphere projection and the ground plane.

Runs standalone to print the fit:

    python3 tools/analyze_splat_ply.py scene.ply

Run the same file inside Maya's script editor to also build a confirmation rig
— a locator at the sphere centre, a wireframe sphere on the fitted shell, a
ground plane, and a cull cube around the good data:

    import sys; sys.path.insert(0, ".../tools")
    import analyze_splat_ply as a
    a.build_rig(a.analyze("/path/scene.ply", percent=10))

Both detections are stable on a small fraction of the splats, so `percent`
keeps this fast on multi-million-splat files.
"""
from __future__ import annotations

import argparse
import math

import numpy as np

_NP_TYPE = {'float': '<f4', 'double': '<f8', 'uchar': 'u1', 'uint8': 'u1',
            'char': 'i1', 'short': '<i2', 'ushort': '<u2',
            'int': '<i4', 'uint': '<u4'}


def read_positions(path):
    """Memory-map the vertex block and return an (N, 3) float64 array."""
    with open(path, 'rb') as f:
        header = b''
        while b'end_header' not in header:
            line = f.readline()
            if not line:
                raise ValueError('no end_header — not a PLY?')
            header += line
        offset = f.tell()

    lines = header.decode('ascii').splitlines()
    if not any(l.startswith('format binary_little_endian') for l in lines):
        raise ValueError('only binary_little_endian is supported')

    count, props = 0, []
    for line in lines:
        tok = line.split()
        if not tok:
            continue
        if tok[0] == 'element' and tok[1] == 'vertex':
            count = int(tok[2])
        elif tok[0] == 'property':
            if tok[1] == 'list':
                raise ValueError('list properties are not supported')
            props.append((tok[1], tok[2]))

    dt = np.dtype([(n, _NP_TYPE[ty]) for ty, n in props])
    arr = np.memmap(path, dtype=dt, mode='r', offset=offset, shape=(count,))
    return np.stack([arr['x'], arr['y'], arr['z']], axis=1).astype(np.float64)


def find_shell(xyz):
    """Locate the background sphere projection.

    3DGS training puts distant content on a large sphere around the capture
    origin, leaving a radial void between it and the real scene. Cutting at the
    widest gap in the radius distribution above p95 finds that void without any
    scene-specific threshold.
    """
    centre = np.median(xyz, axis=0)
    radius = np.linalg.norm(xyz - centre, axis=1)

    tail = np.sort(radius)
    tail = tail[tail > np.percentile(radius, 95)]
    gaps = np.diff(tail)
    i = int(np.argmax(gaps))
    cut = 0.5 * (tail[i] + tail[i + 1])

    fit = _fit_sphere(xyz[radius > cut])
    if not fit:
        return dict(cut=float(cut), found=False)
    fit.update(cut=float(cut), gap=float(gaps[i]), n_total=int(len(xyz)))
    return fit


def _fit_sphere(shell):
    """Algebraic fit: |p - c|^2 = r^2 is linear in (c, r^2 - |c|^2)."""
    if len(shell) < 16:
        return None
    A = np.hstack([2 * shell, np.ones((len(shell), 1))])
    sol, *_ = np.linalg.lstsq(A, (shell ** 2).sum(axis=1), rcond=None)
    c = sol[:3]
    r = math.sqrt(sol[3] + float((c ** 2).sum()))
    thickness = float(np.linalg.norm(shell - c, axis=1).std())
    return dict(found=thickness / r < 0.12, centre=c, radius=r,
                thickness=thickness, ratio=thickness / r, n_shell=int(len(shell)))


def find_ground(core, seed=0, tol=0.05, iters=500):
    """RANSAC the dominant plane, then refit on its inliers.

    The refit matters: a plane taken straight from three random points inherits
    their noise, and the tilt is what gets used to level the scene.
    """
    rng = np.random.default_rng(seed)
    sub = core if len(core) <= 200000 else core[rng.choice(len(core), 200000, replace=False)]

    best = (0, None, None)
    for _ in range(iters):
        p = sub[rng.choice(len(sub), 3, replace=False)]
        n = np.cross(p[1] - p[0], p[2] - p[0])
        ln = np.linalg.norm(n)
        if ln < 1e-9:
            continue
        n = n / ln
        d = -float(n.dot(p[0]))
        inl = int((np.abs(sub @ n + d) < tol).sum())
        if inl > best[0]:
            best = (inl, n, d)

    n, d = best[1], best[2]
    for _ in range(8):
        pts = sub[np.abs(sub @ n + d) < tol]
        if len(pts) < 3:
            break
        c = pts.mean(axis=0)
        n = np.linalg.svd(pts - c, full_matrices=False)[2][2]
        n /= np.linalg.norm(n)
        d = -float(n.dot(c))
    if n[1] < 0:
        n, d = -n, -d

    signed = sub @ n + d
    # The scene body sits on whichever side has the long tail. If that is the
    # -n side then n points downward and the capture is upside down.
    upside_down = abs(np.percentile(signed, 1)) > abs(np.percentile(signed, 99))

    return dict(normal=n, d=d,
                inliers=int((np.abs(signed) < tol).sum()), n_sub=int(len(sub)),
                y_at_origin=float(-d / n[1]),
                rot_x=float(math.degrees(math.atan2(n[2], n[1]))),
                rot_z=float(math.degrees(math.atan2(-n[0], n[1]))),
                tilt=float(math.degrees(math.acos(np.clip(n[1], -1, 1)))),
                upside_down=bool(upside_down),
                body_height=float(abs(np.percentile(signed, 0.5))))


def analyze(path, percent=100.0, seed=0):
    """Analyse `percent` of the splats, taken as every Nth point.

    percent=10 keeps every 10th splat, percent=100 keeps all. Striding rather
    than random sampling costs nothing to compute and is reproducible; the two
    agree closely here because 3DGS output is not ordered periodically.

    Two-stage on purpose. The cut radius and the ground plane are stable from a
    few thousand points. The sphere *centre* is not: a thin sample leaves only
    a handful of shell points and the fit wanders by a large fraction of the
    body height. So the cut comes from the sample, then every shell point in
    the full data is collected for the sphere fit — one extra pass over the
    radii plus a fit over ~1% of the splats.
    """
    full = read_positions(path)
    full_n = len(full)
    stride = max(1, int(round(100.0 / max(percent, 1e-9))))
    xyz = full[::stride] if stride > 1 else full

    shell = find_shell(xyz)
    centre = np.median(xyz, axis=0)

    if stride > 1:
        full_r = np.linalg.norm(full - centre, axis=1)
        refit = _fit_sphere(full[full_r > shell['cut']])
        if refit:
            shell.update(refit)
            shell['n_total'] = full_n
        core = full[full_r <= shell['cut']][::stride]
    else:
        core = xyz[np.linalg.norm(xyz - centre, axis=1) <= shell['cut']]

    ground = find_ground(core, seed=seed)

    n, d = ground['normal'], ground['d']
    box = None
    if shell.get('found'):
        h = float(shell['centre'] @ n + d)
        shell['height_above_ground'] = -h
        shell['frac_of_body_height'] = abs(h) / ground['body_height'] if ground['body_height'] else 0.0

    lo = np.percentile(core, 0.5, axis=0)
    hi = np.percentile(core, 99.5, axis=0)
    box = dict(centre=(lo + hi) / 2, size=hi - lo)

    return dict(path=path, n_total=full_n, analysed=len(xyz), stride=stride,
                shell=shell, ground=ground, box=box, core_aabb=(lo, hi),
                aabb_before=(xyz.min(axis=0), xyz.max(axis=0)))


def report(res):
    s, g, b = res['shell'], res['ground'], res['box']
    print("%s\n  %d splats (analysed %d — every %s)"
          % (res['path'], res['n_total'], res['analysed'],
             "point" if res['stride'] == 1 else "%dth point" % res['stride']))

    print("\nSPHERE PROJECTION")
    if s.get('found'):
        print("  detected — cut radius %.4f, %d splats beyond (%.2f%%)"
              % (s['cut'], s['n_shell'], 100.0 * s['n_shell'] / s['n_total']))
        print("  centre  [%+.4f %+.4f %+.4f]   radius %.4f" % (*s['centre'], s['radius']))
        print("  shell thickness/radius %.4f (radial void %.3f wide)" % (s['ratio'], s['gap']))
        if 'height_above_ground' in s:
            print("  centre sits %+.4f above the ground plane (%.0f%% of body height)"
                  % (s['height_above_ground'], 100 * s['frac_of_body_height']))
    else:
        print("  none detected (no clean shell above p95)")

    print("\nGROUND PLANE")
    print("  normal  [%+.5f %+.5f %+.5f]   %d/%d inliers (%.1f%%)"
          % (*g['normal'], g['inliers'], g['n_sub'], 100.0 * g['inliers'] / g['n_sub']))
    print("  crosses Y at %.4f" % g['y_at_origin'])
    print("  rotate X %.4f deg   rotate Z %.4f deg   (tilt %.4f off +Y)"
          % (g['rot_x'], g['rot_z'], g['tilt']))
    print("  capture is %s" % ("UPSIDE DOWN — add 180 about X" if g['upside_down']
                               else "right way up"))

    print("\nSUGGESTED CULL CUBE (world space, before levelling)")
    print("  translate (%.4f, %.4f, %.4f)" % tuple(b['centre']))
    print("  scale     (%.4f, %.4f, %.4f)" % tuple(b['size']))


def build_rig(res, name="splatFit"):
    """Inside Maya: build locator / sphere / ground plane / cull cube to eyeball the fit."""
    import maya.cmds as cmds

    s, g, b = res['shell'], res['ground'], res['box']
    grp = cmds.group(empty=True, name=name + "_GRP")
    made = {'group': grp}

    if s.get('found'):
        loc = cmds.spaceLocator(name=name + "_sphereCentre")[0]
        cmds.xform(loc, ws=True, t=[float(v) for v in s['centre']])
        cmds.setAttr(loc + ".localScaleX", s['radius'] * 0.05)
        cmds.setAttr(loc + ".localScaleY", s['radius'] * 0.05)
        cmds.setAttr(loc + ".localScaleZ", s['radius'] * 0.05)
        cmds.parent(loc, grp)
        made['locator'] = loc

        sph = cmds.polySphere(name=name + "_shell", radius=float(s['radius']),
                              sx=24, sy=16, ch=False)[0]
        cmds.xform(sph, ws=True, t=[float(v) for v in s['centre']])
        _wireframe(sph)
        cmds.parent(sph, grp)
        made['shell'] = sph

    pl = cmds.polyPlane(name=name + "_ground", w=1, h=1, sx=1, sy=1, ch=False)[0]
    span = float(max(b['size'])) * 2.0
    cmds.setAttr(pl + ".scaleX", span)
    cmds.setAttr(pl + ".scaleZ", span)
    cmds.setAttr(pl + ".translateY", g['y_at_origin'])
    cmds.setAttr(pl + ".rotateX", g['rot_x'])
    cmds.setAttr(pl + ".rotateZ", g['rot_z'])
    _wireframe(pl)
    cmds.parent(pl, grp)
    made['ground'] = pl

    cube = cmds.polyCube(name=name + "_cullBox", w=1, h=1, d=1, ch=False)[0]
    cmds.xform(cube, ws=True, t=[float(v) for v in b['centre']])
    cmds.setAttr(cube + ".scaleX", float(b['size'][0]))
    cmds.setAttr(cube + ".scaleY", float(b['size'][1]))
    cmds.setAttr(cube + ".scaleZ", float(b['size'][2]))
    _wireframe(cube)
    cmds.parent(cube, grp)
    made['cullBox'] = cube

    print("[analyze_splat_ply] built: " + ", ".join(sorted(made)))
    return made


def _wireframe(node):
    """Wireframe display so the fit shapes never occlude the splats."""
    import maya.cmds as cmds
    for shp in cmds.listRelatives(node, shapes=True, fullPath=True) or []:
        cmds.setAttr(shp + ".overrideEnabled", 1)
        cmds.setAttr(shp + ".overrideShading", 0)
        cmds.setAttr(shp + ".overrideTexturing", 0)
        cmds.setAttr(shp + ".castsShadows", 0)
        cmds.setAttr(shp + ".receiveShadows", 0)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('ply')
    ap.add_argument('--percent', type=float, default=100.0,
                    help='percentage of splats to analyse, taken as every Nth '
                         'point (10 = every 10th). Default 100. The ground fit '
                         'holds down to well under 1 percent.')
    args = ap.parse_args()
    report(analyze(args.ply, percent=args.percent))


if __name__ == '__main__':
    main()
