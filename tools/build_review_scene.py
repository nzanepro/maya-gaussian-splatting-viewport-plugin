#!/usr/bin/env python3
"""Build a Maya scene for reviewing a 3DGS capture: analysed, levelled, cropped.

    mayapy tools/build_review_scene.py scene.ply out.ma [--percent 10]

Creates a gaussianSplatNode under a levelling rig derived from the detected
ground plane, wires an auto-fitted cull cube into it, and drops the sphere-fit
confirmation shapes alongside.
"""
import argparse
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))


def build(ply, out, plugin, percent=10.0, display_percent=100.0, box_trim=0.5,
          scale=1.0, splat_scale=1.0, ref_size=None, ref_name='ref',
          ground='auto'):
    import maya.standalone
    maya.standalone.initialize(name='python')
    import maya.cmds as cmds
    import numpy as np
    import analyze_splat_ply as A

    print("[review] analysing %s" % ply)
    res = A.analyze(ply, percent=percent, ground=ground)
    A.report(res)
    g, s, b = res['ground'], res['shell'], res['box']

    cmds.loadPlugin(plugin)
    cmds.file(new=True, force=True)
    cmds.loadPlugin(plugin, quiet=True)

    # ---- levelling rig -----------------------------------------------------
    # Nested transforms rather than one baked matrix, so each correction stays
    # legible and tweakable in the channel box:
    #   flip  : 180 about X when the capture is upside down (Polycam is Y-down)
    #   level : undo the measured ground tilt
    #   shift : bring the ground plane to Y = 0
    # A single root above everything, including the cull box, so a unit-scale
    # correction can be dialled in on one node without the box drifting out of
    # register with the splats.
    root = cmds.group(empty=True, name="splat_root_GRP")
    for ax in "XYZ":
        cmds.setAttr(root + ".scale" + ax, float(scale))
    flip = cmds.group(empty=True, name="splat_flip_GRP", parent=root)
    if g['upside_down']:
        cmds.setAttr(flip + ".rotateX", 180.0)
    level = cmds.group(empty=True, name="splat_level_GRP", parent=flip)
    cmds.setAttr(level + ".rotateX", -g['rot_x'])
    cmds.setAttr(level + ".rotateZ", -g['rot_z'])
    shift = cmds.group(empty=True, name="splat_shift_GRP", parent=level)
    cmds.setAttr(shift + ".translateY", -g['y_at_origin'])

    # ---- the splat node ----------------------------------------------------
    shape = cmds.createNode("gaussianSplatNode", name="X29_splatShape", parent=shift)
    xform = cmds.listRelatives(shape, parent=True, fullPath=False)[0]
    cmds.setAttr(shape + ".filePath", ply, type="string")
    cmds.setAttr(shape + ".displayPercent", display_percent)
    cmds.setAttr(shape + ".splatScale", float(splat_scale))
    cmds.setAttr(shape + ".cullEnabled", 1)

    # ---- cull cube, aligned to the ground ---------------------------------
    # Built in levelled world space and left at the scene root, so its axes are
    # the ground's axes: the base sits on the ground and it rises from there,
    # rather than being an axis-aligned box in the capture's tilted frame.
    # The plug-in composes the splat's own world matrix, so the box is free to
    # live anywhere in the outliner.
    #
    # X and Z span the trimmed extent of the good data; `box_trim` is the
    # percentile cut at each end, so the default 0.5 keeps the middle 99% and
    # discards floaters that would otherwise inflate the box enormously.
    # The cull box is a child of root, so its transform values live in root's
    # child space. Divide the root out of the splat's world matrix, or the box
    # is authored in already-scaled coordinates and root scales it a second time.
    rootW = np.array(cmds.getAttr(root + ".worldMatrix[0]")).reshape(4, 4)
    rig = (np.array(cmds.getAttr(shape + ".worldMatrix[0]")).reshape(4, 4)
           @ np.linalg.inv(rootW))
    pts = A.read_positions(ply)[::37]
    pts = pts[np.linalg.norm(pts - np.median(pts, axis=0), axis=1) <= s['cut']]
    lev = (np.hstack([pts, np.ones((len(pts), 1))]) @ rig)[:, :3]

    lo = np.percentile(lev, box_trim, axis=0)
    hi = np.percentile(lev, 100.0 - box_trim, axis=0)
    # Anchor the base on the ground rather than on a percentile, and drop a
    # little below it so the ground splats themselves are kept.
    base = min(0.0, float(lo[1]))
    top = float(hi[1])
    bsize = np.array([hi[0] - lo[0], top - base, hi[2] - lo[2]])
    bcentre = np.array([(lo[0] + hi[0]) / 2.0, (base + top) / 2.0, (lo[2] + hi[2]) / 2.0])
    print("[review] cull box (levelled world): base Y %+.4f  top Y %+.4f" % (base, top))

    cube = cmds.polyCube(name="X29_cullBox", w=1, h=1, d=1, ch=False)[0]
    cmds.parent(cube, root, relative=True)
    for ax, tv, sv in zip("XYZ", bcentre, bsize):
        cmds.setAttr(cube + ".translate" + ax, float(tv))
        cmds.setAttr(cube + ".rotate" + ax, 0.0)
        cmds.setAttr(cube + ".scale" + ax, float(sv))
    _wire(cube)
    cmds.connectAttr(cube + ".worldMatrix[0]", shape + ".cullBoxMatrix", force=True)

    # ---- sphere-fit confirmation ------------------------------------------
    made = []
    if s.get('found'):
        loc = cmds.spaceLocator(name="X29_sphereCentre")[0]
        _adopt(loc, shift, t=s['centre'])
        for ax in "XYZ":
            cmds.setAttr(loc + ".localScale" + ax, s['radius'] * 0.05)
        sph = cmds.polySphere(name="X29_shellFit", radius=float(s['radius']),
                              sx=24, sy=16, ch=False)[0]
        _adopt(sph, shift, t=s['centre'])
        _wire(sph)
        cmds.setAttr(sph + ".visibility", 0)   # on tap, off by default
        made += [loc, sph]

    ground = cmds.polyPlane(name="X29_groundFit", w=1, h=1, sx=1, sy=1, ch=False)[0]
    span = float(max(b['size'])) * 2.0
    _adopt(ground, shift,
           t=(0.0, g['y_at_origin'], 0.0),
           r=(g['rot_x'], 0.0, g['rot_z']),
           s=(span, 1.0, span))
    _wire(ground)
    cmds.setAttr(ground + ".visibility", 0)

    # ---- verify the rig against real geometry ------------------------------
    # Checking the transformed normal alone is ambiguous: the body sits on the
    # -n side, so a correct rig sends n to -Y, which reads like a failure. Push
    # actual splats through the world matrix instead and look at where they land.
    # Checks run in root-child space (scale divided out) so the tolerances below
    # are in PLY units and stay meaningful whatever --scale is.
    m = (np.array(cmds.getAttr(shape + ".worldMatrix[0]")).reshape(4, 4)
         @ np.linalg.inv(rootW))
    pts = A.read_positions(ply)[::997]
    core = pts[np.linalg.norm(pts - np.median(pts, axis=0), axis=1) <= s['cut']]
    w = (np.hstack([core, np.ones((len(core), 1))]) @ m)[:, :3]

    up = -np.array(g['normal'], dtype=float)
    up4 = (np.append(up, 0.0) @ m)[:3]
    up4 /= np.linalg.norm(up4)
    tilt = math.degrees(math.acos(min(1.0, max(-1.0, float(up4[1])))))

    ground_y = float(np.percentile(w[:, 1], 1))
    print("\n[review] rig check")
    print("  scene 'up' maps to      : [%+.4f %+.4f %+.4f]  (%.4f deg off world +Y)"
          % (*up4, tilt))
    print("  ground sits at Y        : %+.4f  (p1 of splat heights)" % ground_y)
    print("  splat heights p50 / p99 : %+.4f / %+.4f" % (np.percentile(w[:, 1], 50),
                                                         np.percentile(w[:, 1], 99)))
    above = float((w[:, 1] > ground_y).mean())
    print("  %.1f%% of splats above the ground" % (100 * above))

    # The cull box must share the splats' frame, or it crops the wrong region.
    cmw = (np.array(cmds.getAttr(cube + ".worldMatrix[0]")).reshape(4, 4)
           @ np.linalg.inv(rootW))
    def _basis(mm):
        a = mm[:3, :3].astype(float).copy()
        for i in range(3):
            ln = np.linalg.norm(a[i])
            if ln > 1e-9:
                a[i] /= ln
        return a
    # The box is deliberately aligned to levelled world space, not to the
    # capture's tilted frame, so its base lies flat on the ground.
    aligned = bool(np.allclose(_basis(cmw), np.eye(3), atol=1e-3))
    print("  cull box is level with the ground: %s" % aligned)
    print("  box base Y / top Y               : %+.4f / %+.4f"
          % (cmw[3, 1] - cmw[1, 1] / 2.0, cmw[3, 1] + cmw[1, 1] / 2.0))

    # And it must actually contain the bulk of the data.
    inv = np.linalg.inv(cmw)
    local = (np.hstack([w, np.ones((len(w), 1))]) @ inv)[:, :3]
    frac = float((np.abs(local) <= 0.5).all(axis=1).mean())
    print("  splats inside the cull box      : %.1f%%" % (100 * frac))

    # Reproduce the matrix GaussianDrawOverride hands the shader and confirm it
    # agrees. The shader tests OBJECT-space positions, so the matrix has to be
    # splatWorld * boxWorld^-1; feeding boxWorld^-1 alone culls everything the
    # moment the splat node carries any transform of its own, which is exactly
    # what a levelling rig gives it.
    shader_local = (np.hstack([core, np.ones((len(core), 1))]) @ (m @ inv))[:, :3]
    shader_frac = float((np.abs(shader_local) <= 0.5).all(axis=1).mean())
    print("  same via the shader's matrix    : %.1f%%%s"
          % (100 * shader_frac,
             "" if abs(shader_frac - frac) < 1e-6 else "   <-- MISMATCH"))

    ok = (tilt < 0.5 and abs(ground_y) < 0.25 and above > 0.9 and aligned
          and frac > 0.9 and abs(shader_frac - frac) < 1e-6)
    print("  -> %s" % ("LEVELLED, UPRIGHT, BOX ALIGNED" if ok else "RIG IS WRONG"))

    # ---- scale reference ---------------------------------------------------
    # A box of known real-world size, to calibrate `scale` against by eye. The
    # capture covers the whole space rather than just the subject, so picking
    # the subject out of the statistics is unreliable; comparing it to a
    # correctly sized box is not.
    if ref_size:
        ref = cmds.polyCube(name=ref_name + "_sizeRef", w=1, h=1, d=1, ch=False)[0]
        for ax, v in zip("XYZ", ref_size):
            cmds.setAttr(ref + ".scale" + ax, float(v))
        cmds.setAttr(ref + ".translateY", float(ref_size[1]) / 2.0)  # stand it on the ground
        _wire(ref)
        print("[review] size reference %s: %.2f x %.2f x %.2f cm"
              % (ref, ref_size[0], ref_size[1], ref_size[2]))

    # Report the scene at the chosen scale so the number can be sanity-checked
    # against something known without opening Maya.
    print("[review] at scale %g (cm per PLY unit) the cropped scene measures "
          "%.2f x %.2f x %.2f m" % (scale, bsize[0] * scale / 100.0,
                                    bsize[1] * scale / 100.0, bsize[2] * scale / 100.0))

    cmds.select(clear=True)
    cmds.file(rename=out)
    cmds.file(save=True, type="mayaAscii", force=True)
    print("[review] wrote %s" % out)
    return out


def _adopt(node, parent, t=(0, 0, 0), r=(0, 0, 0), s=(1, 1, 1)):
    """Reparent into the levelling rig and set the local transform outright.

    cmds.parent() preserves world position by default, which bakes the inverse
    of the rig's rotation into the child. The child then keeps its original
    world orientation while the splats are levelled, so a cull box authored in
    splat space ends up axis-aligned to the world and crops the wrong region.
    relative=True keeps the local values instead, and every channel is then set
    explicitly so nothing is inherited by accident.
    """
    import maya.cmds as cmds
    cmds.parent(node, parent, relative=True)
    for ax, tv, rv, sv in zip("XYZ", t, r, s):
        cmds.setAttr(node + ".translate" + ax, float(tv))
        cmds.setAttr(node + ".rotate" + ax, float(rv))
        cmds.setAttr(node + ".scale" + ax, float(sv))
    return node


def _wire(node):
    import maya.cmds as cmds
    for shp in cmds.listRelatives(node, shapes=True, fullPath=True) or []:
        cmds.setAttr(shp + ".overrideEnabled", 1)
        cmds.setAttr(shp + ".overrideShading", 0)
        cmds.setAttr(shp + ".overrideTexturing", 0)
        cmds.setAttr(shp + ".castsShadows", 0)
        cmds.setAttr(shp + ".receiveShadows", 0)


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('ply')
    ap.add_argument('out')
    ap.add_argument('--plugin', required=True)
    ap.add_argument('--percent', type=float, default=10.0,
                    help='fraction of splats used for the analysis')
    ap.add_argument('--display-percent', type=float, default=100.0,
                    help='initial displayPercent on the node')
    ap.add_argument('--box-trim', type=float, default=0.5, dest='box_trim',
                    help='percentile trimmed from each end when sizing the cull '
                         'box (default 0.5, i.e. keep the middle 99%%)')
    ap.add_argument('--scale', type=float, default=1.0,
                    help='centimetres per PLY unit, applied to splat_root_GRP. '
                         'Polycam exports are not reliably metric; calibrate '
                         'against --ref-size rather than assuming.')
    ap.add_argument('--splat-scale', type=float, default=1.0, dest='splat_scale',
                    help='initial splatScale on the node')
    ap.add_argument('--ref-size', nargs=3, type=float, default=None, dest='ref_size',
                    metavar=('W', 'H', 'D'),
                    help='build a wireframe box of this real size in cm, standing '
                         'on the ground, to calibrate --scale against by eye')
    ap.add_argument('--ref-name', default='ref', dest='ref_name')
    ap.add_argument('--ground', choices=('auto', 'level', 'sloped'), default='auto',
                    help="see analyze_splat_ply.py --ground")
    a = ap.parse_args()
    build(a.ply, a.out, a.plugin, a.percent, a.display_percent, a.box_trim,
          a.scale, a.splat_scale, a.ref_size, a.ref_name, a.ground)
