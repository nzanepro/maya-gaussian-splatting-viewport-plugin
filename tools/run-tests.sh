#!/usr/bin/env bash
# Builds and runs the checks in this directory. See tools/README.md.
#
#   ./tools/run-tests.sh [maya-devkit-path]
#
# sortcheck and plycheck are portable. glslcheck and rendercheck need a macOS
# CGL context and are skipped elsewhere. glslang, if installed, validates both
# shader variants including the OpenGL 4.3 one that cannot run on macOS.
set -uo pipefail
cd "$(dirname "$0")/.."

# Homebrew supplies glew and glslang but is not always on a non-login PATH.
for d in /opt/homebrew/bin /usr/local/bin; do
    [ -d "$d" ] && case ":$PATH:" in *":$d:"*) ;; *) PATH="$PATH:$d" ;; esac
done
export PATH

OUT="${TMPDIR:-/tmp}/gs-tools"
mkdir -p "$OUT"
CXX="${CXX:-clang++}"
STD="-std=c++17 -O2"
fail=0

run() { # name, then command
    local name="$1"; shift
    echo "──────── $name"
    if "$@"; then echo "✓ $name"; else echo "✗ $name"; fail=1; fi
    echo
}

# ---- portable: CPU sort matches the compute-shader bitonic ordering --------
$CXX $STD -o "$OUT/sortcheck" tools/sortcheck.cpp || exit 1
run "sortcheck  (CPU radix sort vs sort.comp reference)" "$OUT/sortcheck"

# ---- portable: PlyLoader parses a real 3DGS file --------------------------
if [ $# -ge 1 ] && [ -n "${1:-}" ]; then PLY="$1"; else PLY=""; fi
$CXX $STD -o "$OUT/plycheck" tools/plycheck.cpp src/PlyLoader.cpp \
    third_party/tinyply/tinyply.cpp -Isrc -Ithird_party/tinyply -Itools/stub || exit 1
if [ -n "$PLY" ]; then
    run "plycheck   ($(basename "$PLY"))" "$OUT/plycheck" "$PLY"
else
    echo "──────── plycheck"; echo "- skipped: pass a .ply path as the first argument"; echo
fi

# ---- spec validation of every shader variant, both paths ------------------
if command -v glslangValidator >/dev/null 2>&1; then
    echo "──────── glslang (all shader variants)"
    G="$OUT/glsl"; mkdir -p "$G"; rm -f "$G"/*
    { printf '#version 450\n#define GS_USE_SSBO 1\n#line 1\n'; cat src/shaders/gaussian.vert; } > "$G/ssbo.vert"
    { printf '#version 450\n#define GS_USE_SSBO 1\n#line 1\n'; cat src/shaders/gaussian.frag; } > "$G/ssbo.frag"
    { printf '#version 410 core\n#line 1\n';                   cat src/shaders/gaussian.vert; } > "$G/gl41.vert"
    { printf '#version 410 core\n#line 1\n';                   cat src/shaders/gaussian.frag; } > "$G/gl41.frag"
    { printf '#version 450\n#line 1\n';                        cat src/shaders/depth.comp;    } > "$G/depth.comp"
    { printf '#version 450\n#line 1\n';                        cat src/shaders/sort.comp;     } > "$G/sort.comp"
    ok=1
    for f in "$G"/*; do
        if out=$(glslangValidator "$f" 2>&1); then printf "  %-12s OK\n" "$(basename "$f")"
        else printf "  %-12s FAILED\n%s\n" "$(basename "$f")" "$out"; ok=0; fi
    done
    [ $ok -eq 1 ] && echo "✓ glslang" || { echo "✗ glslang"; fail=1; }
    echo
else
    echo "──────── glslang"; echo "- skipped: brew install glslang"; echo
fi

# ---- macOS only: compile and render against a real 4.1 context ------------
if [ "$(uname)" = "Darwin" ]; then
    GLEW_PREFIX="$(brew --prefix glew 2>/dev/null || echo /usr/local)"
    GLFLAGS="-I$GLEW_PREFIX/include -L$GLEW_PREFIX/lib -lGLEW -framework OpenGL -DGL_SILENCE_DEPRECATION"
    $CXX $STD -o "$OUT/glslcheck"   tools/glslcheck.cpp   $GLFLAGS || exit 1
    $CXX $STD -o "$OUT/rendercheck" tools/rendercheck.cpp $GLFLAGS || exit 1
    run "glslcheck  (4.1 program links, uniforms resolve)" "$OUT/glslcheck"   src/shaders/
    run "rendercheck (synthetic scene renders to an FBO)"  "$OUT/rendercheck" src/shaders/
else
    echo "──────── glslcheck / rendercheck"; echo "- skipped: macOS only (CGL)"; echo
fi

[ $fail -eq 0 ] && echo "All checks passed." || echo "Some checks FAILED."
exit $fail
