# Maya Gaussian Splatting Viewport Plugin

[![Build Plugin](https://github.com/chordee/maya-gaussian-splatting-viewport-plugin/actions/workflows/build.yml/badge.svg)](https://github.com/chordee/maya-gaussian-splatting-viewport-plugin/actions/workflows/build.yml)

A C++ Maya plugin for real-time 3D Gaussian Splatting (`.ply`) rendering
in Autodesk Maya Viewport 2.0.

## Features

- Load standard 3DGS `.ply` files (position, rotation, scale, opacity, SH coefficients)
- View-dependent color via SH degrees 0–3 (auto-detected from PLY, runtime-capped via attribute)
- EWA Splatting: full GPU projection of 3D covariance to 2D ellipses
- Depth sort skipped when the camera is static; GPU bitonic sort on OpenGL 4.3, CPU radix sort on the 4.1 fallback
- Maya scene integration: Reversed-Z depth test compatible, non-destructive
- Oriented cull box driven by any Maya transform, plus display thinning by percentage
- Automatic detection of the background sphere projection and the ground plane (`tools/`)
- Maya node attributes: `filePath`, `splatScale`, `opacityMult`, `shDegree`, `sRGBToLinear`, `gamma`, `cullEnabled`, `cullBoxMatrix`, `cullInvert`, `displayPercent`

## Requirements

| Tool | Version |
| ---- | ------- |
| Autodesk Maya | 2024, 2025, 2026, or 2027 |
| Maya DevKit | matching the Maya version |
| Compiler | MSVC 2022 (or 2019 for Maya 2024–2026) on Windows; Xcode/Apple Clang on macOS |
| CMake | ≥ 3.20 |
| vcpkg | Windows only |
| GLEW | Windows: via vcpkg. macOS: `brew install glew` |

The renderer picks its path from the OpenGL context at runtime — a 4.3 compute
path on Windows, a 4.1 fallback on macOS. See
[Platform support](#platform-support).

## Pre-built binaries

Each tagged release on the
[Releases page](https://github.com/chordee/maya-gaussian-splatting-viewport-plugin/releases)
ships a per-version zip:

- `GaussianSplatPlugin-maya2024-<tag>.zip`
- `GaussianSplatPlugin-maya2025-<tag>.zip`
- `GaussianSplatPlugin-maya2026-<tag>.zip`
- `GaussianSplatPlugin-maya2027-<tag>.zip`

Download the zip matching your Maya version, unzip, and skip to
[Installation](#installation). Compiling from source (below) is only needed
if you want to modify the plugin or target a Maya version that is not in
the release matrix.

## Build

### Windows

GLEW is declared in `vcpkg.json` and installed automatically on first configure
when `VCPKG_ROOT` is set. It is linked statically — no `glew32.dll` needed at runtime.

```bat
cmake -B build ^
    -DMAYA_DEVKIT="C:/Users/you/devkitBase2024" ^
    -DCMAKE_TOOLCHAIN_FILE="%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake" ^
    -DVCPKG_TARGET_TRIPLET="x64-windows-static-md"
cmake --build build --config Release
```

Output is placed in `build/Release/`:

- `GaussianSplatPlugin.mll`
- `shaders/` directory (copied automatically by CMake)

### macOS

vcpkg is not used; GLEW comes from Homebrew. `MAYA_DEVKIT` points at an
extracted DevKit (the folder containing `include/` and `lib/`), or at a Maya
installation such as `/Applications/Autodesk/maya2024` — CMake falls back to
the dylibs inside `Maya.app/Contents/MacOS` when there is no `lib/`.

```sh
brew install cmake glew
cmake -B build -G Ninja -DMAYA_DEVKIT="$HOME/maya-devkit/2024"
cmake --build build
```

Output is placed in `build/`:

- `GaussianSplatPlugin.bundle`
- `shaders/` directory (copied automatically by CMake)

The bundle is built for the host architecture. Maya 2024 runs natively on
Apple Silicon, so an arm64 build is correct there; if you run Maya under
Rosetta, configure with `-DCMAKE_OSX_ARCHITECTURES=x86_64` and use an
x86_64 GLEW. The bundle links GLEW dynamically from the Homebrew prefix, so
it is not self-contained — a machine loading it needs `brew install glew`.

### Both platforms

For Maya 2027, append `-DCMAKE_CXX_STANDARD=20` to the `cmake -B build`
line — Maya 2027's DevKit requires C++20. Maya 2024–2026 default to C++17.

## Installation

1. Copy `GaussianSplatPlugin.mll` (`.bundle` on macOS) and `shaders/` to your
   Maya plug-ins directory (or load directly from where they sit). The binary
   is built against a specific Maya version's DevKit and is **not
   interchangeable** across Maya versions — use the zip or source build
   matching your Maya.
2. **Set Viewport 2.0 to OpenGL Core Profile** — this plugin uses OpenGL and will
   not render under DirectX 11 (Maya's default on Windows). macOS is always
   OpenGL Core Profile, so there is nothing to change there:
   **Windows → Settings/Preferences → Preferences → Display → Viewport 2.0**
   → set *Rendering engine* to **OpenGL Core Profile (Compatibility)**.
   Restart Maya after changing this setting.
3. In Maya: **Windows → Settings/Preferences → Plug-in Manager**
   → load `GaussianSplatPlugin.mll`.

## Usage

```python
import maya.cmds as cmds
node = cmds.createNode("gaussianSplatNode")
cmds.setAttr(node + ".filePath", "/path/to/scene.ply", type="string")
```

Adjustable attributes:

- `splatScale` — global size multiplier (default `1.0`)
- `opacityMult` — opacity multiplier (default `1.0`)
- `shDegree`   — SH evaluation cap, `0`–`3` (default `3`). Capped at the highest
  degree present in the loaded PLY; lower it to trade view-dependent fidelity
  for vertex-shader cost.
- `sRGBToLinear` — bool (default `on`). When enabled, applies a fixed
  `pow(color, 2.2)` after SH evaluation, undoing the sRGB encoding most
  3DGS training pipelines bake into the SH coefficients. Turn off when
  your Maya viewport is not in linear-workflow mode.
- `cullEnabled` / `cullBoxMatrix` / `cullInvert` — crop to a box. Connect any
  transform's `worldMatrix[0]` into `cullBoxMatrix`; the node inverts it and
  treats the box as the unit cube on the origin, so a default 1×1×1 Maya cube
  maps 1:1 and its translate/rotate/scale manipulators all work. The test runs
  in the vertex shader, so it updates live while you drag the box and costs
  nothing per frame. `cullInvert` keeps what is *outside* the box instead.
- `displayPercent` — draw only a fraction of the splats, `0.1`–`100` (default
  `100`). `10` draws every 10th. Striding is spatially uniform on real captures,
  and on the OpenGL 4.1 path the thinned splats are dropped before the depth
  sort, so the sort shrinks proportionally too.
- `gamma` — display-gamma curve, `0.1`–`5.0` (default `1.0`). Independent
  of `sRGBToLinear`; the shader applies `pow(color, 1/gamma)` on top of
  the sRGB stage. Values above `1.0` brighten, values below `1.0` darken.

## Architecture

```text
plugin.cpp              → initializePlugin / uninitializePlugin
GaussianNode            → MPxLocatorNode, owns SplatData
GaussianDrawOverride    → MPxDrawOverride, manages OpenGL state
GaussianRenderer        → OpenGL buffers, shader management, sort, draw;
                          selects the 4.3 or 4.1 path from context capabilities
PlyLoader               → tinyply wrapper, parses .ply, converts scale/opacity
src/shaders/
  gaussian.vert/frag    → EWA splatting + pre-multiplied alpha; the vertex
                          shader covers both paths via GS_USE_SSBO
  depth.comp            → per-splat camera depth calculation (4.3 only)
  sort.comp             → GPU Bitonic Sort (4.3 only)
third_party/tinyply     → PLY parsing library
```

## Platform support

| Platform | Plug-in | Depth sort | Splat data |
| -------- | ------- | ---------- | ---------- |
| Windows | `.mll` | GPU bitonic (compute shader) | SSBO |
| macOS | `.bundle` | CPU radix | Texture buffers |

Apple's OpenGL implementation is frozen at 4.1 Core and will not advance, so
`GL_ARB_compute_shader` and `GL_ARB_shader_storage_buffer_object` are both
absent. The renderer detects this on the first draw and falls back:

- **Depth sort** moves to the CPU — an LSD radix sort over the same
  view-space Z keys `depth.comp` computes, producing the identical
  back-to-front order. It still only runs when the camera has moved.
- **Per-splat buffers** are bound as texture buffers and read with
  `texelFetch` instead of as SSBOs. The buffer objects are the same; only the
  view differs.
- **The sorted index** arrives as an instanced vertex attribute rather than
  an indexed SSBO read.

`gaussian.vert` serves both paths from one source, switched on a `GS_USE_SSBO`
define, so the EWA projection and SH evaluation cannot drift apart. The
`#version` line is supplied by the loader rather than the file.

The 4.1 path is confirmed working in Maya 2024 on macOS 26 / Apple M1.
`tools/` holds checks that cover it without needing Maya open — see
[tools/README.md](tools/README.md).

The sort is single-threaded and its result is uploaded whenever the camera
moves, where the Windows path keeps everything resident on the GPU. Measured on
an M1 (depth transform plus radix sort, the whole per-move CPU cost):

| Splats | Sort |
| ------ | ---- |
| 150k | 1.7 ms |
| 500k | 4.4 ms |
| 1.7M | 14.9 ms |

So the sort is not usually the limiting factor — rasterising that many
overlapping alpha-blended splats is. Texture-buffer fetches are also somewhat
slower than SSBO reads.

## Known Limitations

- Large scenes (3M+ splats) require ~250 GPU dispatches per frame when the camera
  moves, which may trigger TDR on lower-end GPUs.
- On macOS the depth sort is single-threaded on the CPU, so frame time during
  camera movement scales with splat count.

## License

MIT — see [LICENSE](LICENSE)
