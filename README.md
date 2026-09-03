# Maya Gaussian Splatting Viewport Plugin

[![Build Plugin](https://github.com/chordee/maya-gaussian-splatting-viewport-plugin/actions/workflows/build.yml/badge.svg)](https://github.com/chordee/maya-gaussian-splatting-viewport-plugin/actions/workflows/build.yml)

A C++ Maya plugin for real-time 3D Gaussian Splatting (`.ply`) rendering
in Autodesk Maya Viewport 2.0.

## Features

- Load standard 3DGS `.ply` files (position, rotation, scale, opacity, SH coefficients)
- View-dependent color via SH degrees 0–3 (auto-detected from PLY, runtime-capped via attribute)
- EWA Splatting: full GPU projection of 3D covariance to 2D ellipses
- GPU Bitonic Sort: depth sort skipped when camera is static (performance optimization)
- Maya scene integration: Reversed-Z depth test compatible, non-destructive
- Maya node attributes: `filePath`, `splatScale`, `opacityMult`, `shDegree`, `sRGBToLinear`, `gamma`

## Requirements

| Tool | Version |
| ---- | ------- |
| Autodesk Maya | 2024, 2025, 2026, or 2027 |
| Maya DevKit | matching the Maya version |
| Compiler | MSVC 2022 (or 2019 for Maya 2024–2026) on Windows; Xcode/Apple Clang on macOS |
| CMake | ≥ 3.20 |
| vcpkg | Windows only |
| GLEW | Windows: via vcpkg. macOS: `brew install glew` |

> **Windows is the only platform where this plugin renders.** The build is
> cross-platform and macOS produces a loadable `.bundle`, but the renderer
> requires OpenGL 4.3 compute shaders and SSBOs, which Apple's OpenGL
> implementation does not provide — see
> [Platform support](#platform-support).

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
   not render under DirectX 11 (Maya's default on Windows):
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
- `gamma` — display-gamma curve, `0.1`–`5.0` (default `1.0`). Independent
  of `sRGBToLinear`; the shader applies `pow(color, 1/gamma)` on top of
  the sRGB stage. Values above `1.0` brighten, values below `1.0` darken.

## Architecture

```text
plugin.cpp              → initializePlugin / uninitializePlugin
GaussianNode            → MPxLocatorNode, owns SplatData
GaussianDrawOverride    → MPxDrawOverride, manages OpenGL state
GaussianRenderer        → OpenGL VAO/SSBO, shader management, sort, draw
PlyLoader               → tinyply wrapper, parses .ply, converts scale/opacity
src/shaders/
  gaussian.vert/frag    → EWA splatting + pre-multiplied alpha
  depth.comp            → per-splat camera depth calculation
  sort.comp             → GPU Bitonic Sort
third_party/tinyply     → PLY parsing library
```

## Platform support

| Platform | Builds | Renders |
| -------- | ------ | ------- |
| Windows | yes | yes |
| macOS | yes (`.bundle`) | **no** |

Apple's OpenGL implementation is frozen at 4.1 Core and will not advance;
`GL_ARB_compute_shader` and `GL_ARB_shader_storage_buffer_object` are both
absent. This renderer needs them for three things that have no 4.1 fallback:

- `depth.comp` and `sort.comp` are compute shaders (`#version 450`)
- the GPU bitonic depth sort dispatches those compute shaders per frame
- every per-splat buffer (positions, rotations, scales, SH coefficients,
  sorted indices) is bound as an SSBO in `gaussian.vert`

So on macOS the plugin compiles, links, and loads into Maya, but the shader
programs fail to build and nothing is drawn. Porting to macOS means replacing
the compute-shader sort with a CPU or transform-feedback sort and replacing
the SSBOs with texture buffers — or moving the renderer to Metal.

## Known Limitations

- Large scenes (3M+ splats) require ~250 GPU dispatches per frame when the camera
  moves, which may trigger TDR on lower-end GPUs.

## License

MIT — see [LICENSE](LICENSE)
