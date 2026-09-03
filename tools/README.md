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
