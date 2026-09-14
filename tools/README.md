# Verification tools

Checks for the renderer that don't need Maya running. They exist mainly to
cover the OpenGL 4.1 path added for macOS, where the shaders and the depth
sort differ from the Windows 4.3 path and a mistake would otherwise only show
up as a blank viewport.

```sh
./tools/run-tests.sh                      # everything that can run here
./tools/run-tests.sh /path/to/scene.ply   # also exercise the PLY loader
```

| Tool | Platform | What it proves |
| ---- | -------- | -------------- |
| `sortcheck.cpp` | any | The CPU radix sort produces the same ordering as a faithful C++ port of `sort.comp`, over randomized inputs including ties and zeros. Also pins the sort direction: ascending view-space Z, i.e. back-to-front. |
| `plycheck.cpp` | any | `PlyLoader` parses a real 3DGS file and returns internally consistent array sizes, SH degree and per-splat strides. |
| `glslcheck.cpp` | macOS | On a headless CGL 4.1 core context: the 4.1 shader variants compile, the program links, and every uniform plus the `a_splatIndex` attribute resolves — so nothing the renderer binds is silently missing. |
| `rendercheck.cpp` | macOS | Full 4.1 draw path end to end — texture buffers, instanced index attribute, uniforms, blending — rendering a synthetic three-splat scene into an FBO and reading the pixels back. |
| `glslang` (via `run-tests.sh`) | any | Spec-validates all six shader variants, **including the `#version 450` SSBO path**, which is the only offline check available for the Windows shaders when working on a Mac. |

## Scene analysis

`analyze_splat_ply.py` finds two things automatically in a 3DGS capture:

```sh
python3 tools/analyze_splat_ply.py scene.ply [--percent 10] [--up X Y Z]
```

- **The background sphere projection** — see below. On the X-29 capture it is
  1.3% of the splats, and removing them shrinks the bounding box by 55-67% per
  axis.
- **The ground plane**, by gravity-constrained RANSAC plus an inlier refit,
  giving the rotation that levels the scene and a flag for whether the capture
  is upside down (Polycam exports are Y-down). The gravity constraint is not
  cosmetic: unconstrained RANSAC returned a *wall* on a bedroom capture — 24%
  inliers, 89 degrees off vertical — because the floor was under furniture
  while a bare wall was fully visible. Candidates more than 40 degrees off the
  Y axis are rejected, a loose bound because Polycam's alignment is only
  approximate (the X-29 sits 13.6 degrees off). Of the two planes that survive
  the constraint, the floor is the one with the scene on its negative side.
  Use `--up X Y Z` to supply the axis by hand when a solve is not
  gravity-aligned at all; a 747 cockpit scan needed it, having produced 1.4%
  inliers at 22.8 degrees.

### Finding the background sphere

3DGS pushes any Gaussian whose depth the optimiser could not constrain out onto
a large sphere around the capture origin. Outdoors that is genuinely distant
content; indoors it is flat painted wall, which gives no parallax — so an
interior can carry a *larger* shell than an exterior. Measured: 6.7% of an
interior bedroom capture against 1.3% of an outdoor walkaround.

**The p95 gap technique.** The original approach, and still the right intuition:
the shell is separated from the real scene by an empty radial band, so sort the
radii, look at the tail above p95, and cut at the widest gap in it. No
scene-specific threshold, and on the X-29 it lands cleanly — a 1.05-wide void
at radius 13.65.

**Why it is no longer used alone.** Searching only above p95 assumes the shell
is smaller than 5% of the scene. On the 6.7% bedroom, p95 already lies *inside*
the shell, so the search saw nothing but shell points, found no void, and
reported no shell at all. The failure is silent — a clean "none detected", not
an error.

**What runs now.** Scan candidate cuts outward and take the first that leaves a
thin, populated shell behind it. Candidates are the largest relative jumps in
the sorted radii — the same gap idea, no longer restricted to the tail — plus a
spread of percentiles from p50 to p99.5 so a shell with no crisp void is still
found. The p95 gap is one of the candidates rather than the only one. Taking
the *smallest* qualifying cut matters: a larger one slices off the shell's
inner face.

A candidate qualifies on three tests together, because thinness alone is not
enough — any handful of stray outliers sits near *some* sphere:

| Test | Threshold | Why |
| ---- | --------- | --- |
| thin | `thickness/radius < 0.05` | Real shells measure 0.005-0.026. A looser 0.12 let the X-29 stop at radius 8.7 and drag the sparse tail into the fit, putting the centre 1.7 units out. |
| populated | `>= 0.1%` of splats, min 200 | Stops a dozen stray points being called a shell. |
| real void | `gap/cut >= 0.02` | Every distribution has a widest gap; this asks whether it is actually empty. |

Run the same module inside Maya to build a confirmation rig — a locator at the
sphere centre, a wireframe shell, the ground plane, and a cull cube:

```python
import sys; sys.path.insert(0, "<repo>/tools")
import analyze_splat_ply as a
a.build_rig(a.analyze("/path/scene.ply", percent=10))
```

`--percent` takes every Nth point (`--percent 10` = every 10th). Measured on
the X-29 capture, the ground tilt holds to within 0.04 degrees all the way down
to 0.3% (5,073 of 1.69M splats), and striding matches random sampling for
spatial uniformity (voxel-occupancy correlation 0.9998 against the full cloud
at 10%).

It is two-stage on purpose. The cut radius and the ground plane are stable at
those rates, but the sphere *centre* is not — a thin sample leaves too few
shell points and the fit wanders by a large fraction of the body height. So the
cut comes from the sample and the sphere is then fitted against every shell
point in the full data, which makes the centre identical at every rate.

## Building a review scene

`build_review_scene.py` runs the analysis and writes a Maya scene with the
capture levelled, cropped and scaled, ready to open:

```sh
mayapy tools/build_review_scene.py scene.ply out.ma \
    --prefix x29 --plugin /path/GaussianSplatPlugin.bundle \
    --percent 10 --scale 500 --splat-scale 0.4 \
    --ref-size 829 436 1466
```

It builds `<prefix>_splat_root_GRP` over a three-group levelling rig — flip
(180 about X when the capture is upside down), level (undo the measured ground
tilt), shift (ground to Y=0) — with the splat node under it, plus a cull box,
and hidden sphere and ground fit shapes to check the analysis against.

`--prefix` is required and every node carries it, the levelling groups
included, so several captures can share one scene without colliding. This
matters more than it sounds: Maya does not error on a name clash, it appends a
digit, so a second capture would quietly connect its splat node to the *first*
capture's cull box and crop a region that looks almost plausible. Each capture
gets its own ground solve — two captures in one scene hold independent
levelling rotations, verified at 0.0 and -13.57 degrees.

| Flag | Notes |
| ---- | ----- |
| `--scale` | Centimetres per PLY unit, on the root above both the rig and the cull box. Scaling the rig alone slides the splats relative to the box and silently changes the crop. |
| `--ref-size W H D` | A wireframe box of known real size, standing on the ground, to calibrate `--scale` against by eye. |
| `--box-trim` | Percentile trimmed from each end when sizing the cull box (default 0.5). The box top sits at p99.5, so a tall subject may need this lowered. |
| `--up X Y Z` | Force the up axis when the solve is not gravity-aligned. |
| `--ground` | `auto`, `level` (snap a near-level fit flat) or `sloped` (trust it). |

**On scale:** do not try to infer it from the geometry. On the X-29 capture
three independent geometric methods returned 1.48, 2.02 and 2.36 metres per
unit against a true 5 — wrong by 2 to 3.4x, and agreeing closely enough with
each other to look trustworthy. These captures cover the whole space rather
than the subject, so anything trying to isolate "the subject" measures the
room. Calibrate against `--ref-size` instead.

`decimate_ply.py` writes a smaller `.ply` by keeping every Nth splat, which
separates "does it render correctly" from "is it fast enough" when testing:

```sh
python3 tools/decimate_ply.py big.ply small.ply --target 150000
```

## Notes

`stub/maya/MGlobal.h` is a minimal stand-in for the two Maya logging calls
`PlyLoader` makes, so `plycheck` can link without the Maya runtime. Linking
the real `libOpenMaya.dylib` outside Maya does not work on macOS: it resolves
dependencies through `@executable_path/../Frameworks`, so it only loads from
inside `Maya.app`.

None of this covers the draw inside Maya's Viewport 2.0 itself — whether the
plugin's GL state cooperates with Maya's reversed-Z depth test and blending.
That still needs the GUI.
