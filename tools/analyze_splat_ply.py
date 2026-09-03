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

    3DGS pushes anything whose depth the optimiser cannot constrain out onto a
    large sphere around the capture origin. Outdoors that is genuinely distant
    content; indoors it is flat painted wall, which gives no parallax, so an
    interior can carry a *larger* shell than an exterior — measured at 6.7% of
    an interior bedroom capture against 1.3% of an outdoor one.

    That range is why the cut is not taken from a fixed percentile. Searching
    the tail above p95 assumes the shell is smaller than 5% of the scene; on
    the bedroom p95 already lies inside the shell, so the search saw only shell
    points, found no void, and reported nothing.

    Instead scan candidate cuts outward and take the first that leaves a thin,
    populated shell behind it. The smallest such cut is the right one: it keeps
    the shell whole, where a larger cut would slice off its inner face.
    """
    centre = np.median(xyz, axis=0)
    radius = np.linalg.norm(xyz - centre, axis=1)
    n_total = len(xyz)

    # Candidates: every large relative jump in the sorted radii, plus a spread
    # of percentiles so a shell with no crisp void is still found.
    order = np.sort(radius)
    lo_i = int(0.5 * len(order))
    tail = order[lo_i:]
    with np.errstate(divide='ignore', invalid='ignore'):
        ratio = np.diff(tail) / np.maximum(tail[:-1], 1e-9)
    cuts = [0.5 * (tail[i] + tail[i + 1]) for i in np.argsort(ratio)[-12:]]
    cuts += list(np.percentile(radius, np.linspace(50, 99.5, 40)))

    best = None
    for cut in sorted(set(float(c) for c in cuts)):
        gap = _void_width(order, cut)
        fit = _fit_sphere(xyz[radius > cut], n_total, cut, gap)
        if fit and fit['found']:
            fit.update(cut=float(cut), gap=float(gap), n_total=int(n_total))
            best = fit
            break

    if best is None:
        cut = float(cuts[0]) if cuts else float(np.percentile(radius, 99))
        gap = _void_width(order, cut)
        base = dict(cut=cut, gap=gap, n_total=int(n_total), found=False)
        fit = _fit_sphere(xyz[radius > cut], n_total, cut, gap)
        if fit:
            base.update(fit)
            base['found'] = False
        return base
    return best


def _void_width(sorted_radii, cut):
    """Width of the empty band the cut sits in — how isolated the shell is."""
    i = int(np.searchsorted(sorted_radii, cut))
    lo = sorted_radii[i - 1] if i > 0 else sorted_radii[0]
    hi = sorted_radii[i] if i < len(sorted_radii) else sorted_radii[-1]
    return float(hi - lo)


def _fit_sphere(shell, n_total, cut, gap):
    """Algebraic fit: |p - c|^2 = r^2 is linear in (c, r^2 - |c|^2).

    Thinness alone is not enough to call something a background sphere. Any
    handful of stray outliers sits near *some* sphere, and every scene has a
    widest gap in its radius tail, so an interior capture with no background at
    all would otherwise "detect" a shell made of a dozen points. Require the
    candidate to be populated and to sit beyond a real void as well.
    """
    if len(shell) < 16:
        return None
    A = np.hstack([2 * shell, np.ones((len(shell), 1))])
    sol, *_ = np.linalg.lstsq(A, (shell ** 2).sum(axis=1), rcond=None)
    c = sol[:3]
    r = math.sqrt(sol[3] + float((c ** 2).sum()))
    thickness = float(np.linalg.norm(shell - c, axis=1).std())

    # 0.05, not 0.12. A real shell is very thin — 0.005 to 0.026 across the
    # captures measured — and a loose bound let the X-29 stop at a cut of 8.7
    # with ratio 0.117, dragging the sparse tail in front of the shell into the
    # fit and putting the sphere 1.7 units off.
    thin = thickness / r < 0.05
    populated = len(shell) >= max(200, 0.001 * n_total)   # >= 0.1% of the scene
    real_void = cut > 0 and gap / cut >= 0.02
    return dict(found=bool(thin and populated and real_void),
                centre=c, radius=r, thickness=thickness, ratio=thickness / r,
                n_shell=int(len(shell)), thin=bool(thin),
                populated=bool(populated), real_void=bool(real_void),
                void_ratio=float(gap / cut) if cut else 0.0)


def find_ground(core, seed=0, tol=0.05, iters=800, max_tilt=40.0, ground='auto',
                up_axis=None):
    """RANSAC the ground plane, then refit on its inliers.

    Two constraints, both needed on real captures:

    Gravity. The largest plane in a scene is often not the floor — in an
    interior the floor is hidden under furniture while a bare wall is fully
    visible, so unconstrained RANSAC returns a wall (measured on a bedroom
    capture: a wall with 24% inliers, 89 degrees off vertical). Polycam exports
    are roughly gravity-aligned, so candidates whose normal is more than
    `max_tilt` from the Y axis are rejected. The bound is loose because the
    alignment is only approximate — the X-29 capture sits 13.6 degrees off.

    Floor, not ceiling. Both satisfy the gravity constraint. With the normal
    oriented +Y, the floor is the one with the scene on its negative side.

    The inlier refit matters too: a plane taken straight from three random
    points inherits their noise, and the tilt is what levels the scene.
    """
    rng = np.random.default_rng(seed)
    sub = core if len(core) <= 200000 else core[rng.choice(len(core), 200000, replace=False)]

    Y = np.array([0.0, 1.0, 0.0])

    # Explicit override. Needed when the solve itself is not gravity-aligned:
    # no geometric heuristic recovers the floor reliably in that case, because
    # the largest, flattest and densest planes in a cluttered space are often
    # walls rather than the floor.
    if up_axis is not None:
        n = np.asarray(up_axis, dtype=float)
        n /= np.linalg.norm(n)
        if n[1] < 0:
            n = -n
        proj = sub @ n
        below_all = float((proj < np.median(proj)).mean())
        edge = np.percentile(proj, 99.5 if below_all >= 0.5 else 0.5)
        d = -float(edge)
        signed = sub @ n + d
        below = float((signed < 0).mean())
        up = -n if below >= 0.5 else n
        return dict(normal=n, d=d, gravity_constrained=True, up=up,
                    scene_below_frac=below, fitted_tilt=0.0,
                    levelled_to_gravity=False, user_up=True,
                    inliers=int((np.abs(signed) < tol).sum()), n_sub=int(len(sub)),
                    y_at_origin=float(-d / n[1]) if abs(n[1]) > 1e-9 else 0.0,
                    rot_x=float(math.degrees(math.atan2(n[2], n[1]))),
                    rot_z=float(math.degrees(math.atan2(-n[0], n[1]))),
                    tilt=float(math.degrees(math.acos(np.clip(n[1], -1, 1)))),
                    upside_down=bool(up[1] < 0),
                    body_height=float(abs(np.percentile(signed, 0.5))))
    cos_limit = math.cos(math.radians(max_tilt))
    best = (0, None, None)          # gravity-aligned, floor-like
    fallback = (0, None, None)      # best of anything, if nothing qualifies
    for _ in range(iters):
        p = sub[rng.choice(len(sub), 3, replace=False)]
        n = np.cross(p[1] - p[0], p[2] - p[0])
        ln = np.linalg.norm(n)
        if ln < 1e-9:
            continue
        n = n / ln
        d = -float(n.dot(p[0]))
        if n[1] < 0:
            n, d = -n, -d
        inl = int((np.abs(sub @ n + d) < tol).sum())
        if inl > fallback[0]:
            fallback = (inl, n, d)
        if abs(float(n.dot(Y))) < cos_limit:
            continue
        # A floor or ceiling is a boundary: nearly all the scene lies on one
        # side of it. Accept either and work out which afterwards, rather than
        # assuming the Y-down convention here and then "deducing" it later.
        below = float((sub @ n + d < 0).mean())
        if 0.4 < below < 0.6:
            continue
        if inl > best[0]:
            best = (inl, n, d)

    constrained = best[1] is not None
    if not constrained:
        best = fallback
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

    # Refine against the floor band only. RANSAC scores by inlier count, so in
    # a furnished room it settles on a mixture of floor, bed and wall — on a
    # bedroom capture that read 5.4 degrees of tilt for a floor that is level.
    # The floor is the scene's lower boundary, so refitting to just the slab at
    # that end removes everything standing on it: same capture, 0.57 degrees.
    proj = sub @ n
    below0 = float((sub @ n + d < 0).mean())
    floor_at_max = below0 >= 0.5      # scene on the -n side => floor at high n
    edge = np.percentile(proj, 99.5 if floor_at_max else 0.5)
    width = 0.03 * float(np.percentile(proj, 99.5) - np.percentile(proj, 0.5))
    band = sub[np.abs(proj - edge) < max(width, 1e-6)]
    if len(band) >= 200:
        bn, bc = n, band.mean(axis=0)
        for _ in range(6):
            bd = -float(bn.dot(bc))
            keep = band[np.abs(band @ bn + bd) < max(width / 3.0, 1e-6)]
            if len(keep) < 50:
                break
            bc = keep.mean(axis=0)
            bn = np.linalg.svd(keep - bc, full_matrices=False)[2][2]
            bn /= np.linalg.norm(bn)
            if bn[1] < 0:
                bn = -bn
        # Only accept it if it stayed gravity-aligned; a wild swing means the
        # band caught something other than floor.
        if abs(float(bn.dot(Y))) >= cos_limit:
            n, d = bn, -float(bn.dot(bc))

    # Level or genuinely sloped?
    #
    # Polycam output is gravity-aligned via ARKit, so on a level floor the
    # fitted tilt is just fit noise and snapping to the gravity axis is more
    # accurate than trusting it. On sloping ground the tilt is real and must be
    # kept, or the scene is levelled to gravity when the subject is not.
    #
    # The two are far apart in practice: a level bedroom floor fits to 1.7
    # degrees, an aircraft parked on a hill to 13.5. 'auto' splits them at 3.
    fitted_tilt = math.degrees(math.acos(min(1.0, max(-1.0, abs(float(n[1]))))))
    snapped = False
    if ground == 'level' or (ground == 'auto' and fitted_tilt < 3.0):
        keep_d = -float(np.array([0.0, 1.0, 0.0]).dot(-d * n / max(n.dot(n), 1e-12)))
        n = np.array([0.0, 1.0, 0.0])
        d = keep_d
        snapped = True

    signed = sub @ n + d
    # The scene sits on one side of its ground plane, and that side is up.
    # n is oriented +Y, so if the scene is on the -n side then up is -Y in
    # capture space and the whole thing has to be turned over.
    below = float((signed < 0).mean())
    up = -n if below >= 0.5 else n
    upside_down = bool(up[1] < 0)

    return dict(normal=n, d=d, gravity_constrained=bool(constrained),
                up=up, scene_below_frac=below,
                fitted_tilt=float(fitted_tilt), levelled_to_gravity=bool(snapped),
                inliers=int((np.abs(signed) < tol).sum()), n_sub=int(len(sub)),
                y_at_origin=float(-d / n[1]),
                rot_x=float(math.degrees(math.atan2(n[2], n[1]))),
                rot_z=float(math.degrees(math.atan2(-n[0], n[1]))),
                tilt=float(math.degrees(math.acos(np.clip(n[1], -1, 1)))),
                upside_down=bool(upside_down),
                body_height=float(abs(np.percentile(signed, 0.5))))


def analyze(path, percent=100.0, seed=0, ground='auto', up_axis=None):
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
        refit = _fit_sphere(full[full_r > shell['cut']], full_n,
                            shell['cut'], shell.get('gap', 0.0))
        if refit:
            shell.update(refit)
            shell['n_total'] = full_n
        core = full[full_r <= shell['cut']][::stride]
    else:
        core = xyz[np.linalg.norm(xyz - centre, axis=1) <= shell['cut']]

    ground = find_ground(core, seed=seed, ground=ground, up_axis=up_axis)

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
        why = []
        if 'thin' in s:
            if not s['thin']:
                why.append("not a thin shell (thickness/radius %.3f)" % s['ratio'])
            if not s['populated']:
                why.append("only %d splats beyond the cut" % s['n_shell'])
            if not s['real_void']:
                why.append("no real void (gap/cut %.4f)" % s['void_ratio'])
        else:
            why.append("fewer than 16 splats beyond the cut")
        print("  none detected — %s" % "; ".join(why))
        print("  (expected for interiors: a closed room has no distant content "
              "to project onto a sphere)")

    print("\nGROUND PLANE")
    print("  normal  [%+.5f %+.5f %+.5f]   %d/%d inliers (%.1f%%)"
          % (*g['normal'], g['inliers'], g['n_sub'], 100.0 * g['inliers'] / g['n_sub']))
    print("  crosses Y at %.4f" % g['y_at_origin'])
    print("  rotate X %.4f deg   rotate Z %.4f deg   (tilt %.4f off +Y)"
          % (g['rot_x'], g['rot_z'], g['tilt']))
    if g.get('user_up'):
        print("  up axis supplied by hand; no fit was used")
    elif g.get('levelled_to_gravity'):
        print("  fitted tilt was %.4f deg — treated as level and snapped to the "
              "gravity axis" % g['fitted_tilt'])
        print("  (pass --ground sloped if the ground really is at that angle)")
    else:
        print("  ground treated as genuinely sloped at %.4f deg" % g['fitted_tilt'])
        print("  (pass --ground level to snap it flat instead)")
    print("  capture is %s" % ("UPSIDE DOWN — add 180 about X" if g['upside_down']
                               else "right way up"))
    if not g.get('gravity_constrained', True):
        print("  WARNING: no gravity-aligned floor-like plane found; this is the "
              "largest plane of any orientation and may be a wall")

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
    ap.add_argument('--ground', choices=('auto', 'level', 'sloped'), default='auto',
                    help="'auto' (default) snaps a fitted tilt under 3 degrees to "
                         "the gravity axis and keeps anything larger; 'level' always "
                         "snaps; 'sloped' always trusts the fit")
    ap.add_argument('--up', nargs=3, type=float, default=None, metavar=('X','Y','Z'),
                    help='force the up axis in capture space, bypassing the fit. '
                         'Use when the solve is not gravity-aligned.')
    args = ap.parse_args()
    report(analyze(args.ply, percent=args.percent, ground=args.ground, up_axis=args.up))


if __name__ == '__main__':
    main()
