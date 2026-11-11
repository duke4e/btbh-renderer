#!/bin/sh
# Local release check for the renderer on macOS (and Linux): builds the C core, every production shader
# (SPIR-V, MSL, and metallib on macOS) and the native tests with the vendored SDL_shadercross, runs the tests
# and compiles the public headers on their own.
#
#   tools/check.sh [build-dir]        SKIP_GPU_TESTS=1 skips the device, window, scene-target and font smoke tests
#
# Needs CMake and a C/C++ compiler (Xcode's command line tools on macOS); uses Ninja when it is installed. A cold build compiles DXC
# and takes a while; later runs are incremental. Submodules must be initialised
# (git submodule update --init --recursive).
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
build=${1:-"${TMPDIR:-/tmp}/btbh-renderer-check"}

generator="Unix Makefiles"
if command -v ninja >/dev/null 2>&1; then generator=Ninja; fi
jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)

echo "== configure and build ($build, $generator)"
cmake -S "$root" -B "$build" -G "$generator" -DCMAKE_BUILD_TYPE=Release \
    -DR3D_ASSIMP_VENDORED=ON -DBTBH_BUILD_GPU_SPIKE=ON -DBTBH_BUILD_GPU_NATIVE_TESTS=ON \
    -DBTBH_BUILD_GPU_PACKAGE_RUNTIME=ON
cmake --build "$build" --parallel "$jobs"

echo "== shader artifacts"
out="$build/generated/btbh_gpu/shaders"
names=$(grep -oE '"production_[a-z0-9_]+\.(vert|frag|comp)\|' "$root/CMakeLists.txt" | tr -d '"|' | sort -u)
missing=0
count=0
for name in $names; do
    count=$((count + 1))
    for extension in spv msl; do
        [ -f "$out/$name.$extension" ] || { echo "missing $name.$extension"; missing=1; }
    done
    if [ "$(uname)" = "Darwin" ]; then
        case "$name" in
            # Not built for Metal yet (CMakeLists.txt); FSR 3 falls back to FSR 1 there.
            production_fsr3_accumulate.comp|production_fsr3_luma_instability.comp|production_fsr3_luma_pyramid.comp|            production_fsr3_prepare_inputs.comp|production_fsr3_prepare_reactivity.comp|production_fsr3_shading_change_pyramid.comp) ;;
            *) [ -f "$out/$name.metallib" ] || { echo "missing $name.metallib"; missing=1; } ;;
        esac
    fi
done
[ "$missing" -eq 0 ] || exit 1
echo "$count shaders present"

echo "== native tests"
if [ "${SKIP_GPU_TESTS:-0}" = "1" ]; then
    ctest --test-dir "$build" --output-on-failure -E "btbh_gpu_device_smoke|btbh_gpu_window_smoke|btbh_gpu_scene_target_smoke|btbh_gpu_font_smoke"
else
    ctest --test-dir "$build" --output-on-failure
fi

echo "== public headers on their own (C11, C++17, -Werror)"
check="$build/header-check"
mkdir -p "$check"
printf '#include <btbh_gpu/btbh_gpu.h>\n#include <btbh_gpu/btbh_gpu_resources.h>\nint main(void){return 0;}\n' > "$check/check.c"
cp "$check/check.c" "$check/check.cpp"
cc -std=c11 -Wall -Wextra -Werror -pedantic -I"$root/include" -c "$check/check.c" -o "$check/check_c.o"
c++ -std=c++17 -Wall -Wextra -Werror -I"$root/include" -c "$check/check.cpp" -o "$check/check_cpp.o"

echo "== all checks passed"
