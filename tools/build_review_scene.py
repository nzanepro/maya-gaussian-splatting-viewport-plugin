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


def build(ply, out, plugin, percent=10.0, display_percent=100.0):
    import maya.standalone
    maya.standalone.initialize(name='python')
    import maya.cmds as cmds
    import numpy as np
    import analyze_splat_ply as A

    print("[review] analysing %s" % ply)
    res = A.analyze(ply, percent=percent)
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
    flip = cmds.group(empty=True, name="splat_flip_GRP")
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
    cmds.setAttr(shape + ".cullEnabled", 1)

    # ---- cull cube, in the splat's own (unlevelled) space -------------------
    # It is parented under the same rig so it travels with the data; its scale
    # is the p0.5..p99.5 extent of the core, i.e. the good data minus the
    # background sphere.
    cube = cmds.polyCube(name="X29_cullBox", w=1, h=1, d=1, ch=False)[0]
    cmds.parent(cube, shift)
    cmds.xform(cube, os=True, t=[float(v) for v in b['centre']])
    for ax, v in zip("XYZ", b['size']):
        cmds.setAttr(cube + ".scale" + ax, float(v))
    _wire(cube)
    cmds.connectAttr(cube + ".worldMatrix[0]", shape + ".cullBoxMatrix", force=True)

    # ---- sphere-fit confirmation ------------------------------------------
    made = []
    if s.get('found'):
        loc = cmds.spaceLocator(name="X29_sphereCentre")[0]
        cmds.parent(loc, shift)
        cmds.xform(loc, os=True, t=[float(v) for v in s['centre']])
        for ax in "XYZ":
            cmds.setAttr(loc + ".localScale" + ax, s['radius'] * 0.05)
        sph = cmds.polySphere(name="X29_shellFit", radius=float(s['radius']),
                              sx=24, sy=16, ch=False)[0]
        cmds.parent(sph, shift)
        cmds.xform(sph, os=True, t=[float(v) for v in s['centre']])
        _wire(sph)
        cmds.setAttr(sph + ".visibility", 0)   # on tap, off by default
        made += [loc, sph]

    ground = cmds.polyPlane(name="X29_groundFit", w=1, h=1, sx=1, sy=1, ch=False)[0]
    cmds.parent(ground, shift)
    span = float(max(b['size'])) * 2.0
    cmds.setAttr(ground + ".scaleX", span)
    cmds.setAttr(ground + ".scaleZ", span)
    cmds.setAttr(ground + ".translateY", g['y_at_origin'])
    cmds.setAttr(ground + ".rotateX", g['rot_x'])
    cmds.setAttr(ground + ".rotateZ", g['rot_z'])
    _wire(ground)
    cmds.setAttr(ground + ".visibility", 0)

    # ---- verify the rig against real geometry ------------------------------
    # Checking the transformed normal alone is ambiguous: the body sits on the
    # -n side, so a correct rig sends n to -Y, which reads like a failure. Push
    # actual splats through the world matrix instead and look at where they land.
    m = np.array(cmds.getAttr(shape + ".worldMatrix[0]")).reshape(4, 4)
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
    ok = tilt < 0.5 and abs(ground_y) < 0.25 and above > 0.9
    print("  %.1f%% of splats above the ground" % (100 * above))
    print("  -> %s" % ("LEVELLED AND UPRIGHT" if ok else "RIG IS WRONG"))

    cmds.select(clear=True)
    cmds.file(rename=out)
    cmds.file(save=True, type="mayaAscii", force=True)
    print("[review] wrote %s" % out)
    return out


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
    a = ap.parse_args()
    build(a.ply, a.out, a.plugin, a.percent, a.display_percent)
