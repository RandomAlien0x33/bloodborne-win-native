#!/usr/bin/env bash
# The map tour build (bbport_tour.h): the GPU library with BB_TOUR=ON in out/tour/gpu and its own
# bb-probe in out/tour. The game's own build (build.sh, out/) is left as it is. Windows (MSYS2
# CLANG64) only, after one normal build.sh (libatrac9, submodules).
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
[[ -f out/libatrac9.a ]] || { echo 'Run build.sh once first.' >&2; exit 1; }
mkdir -p out/tour
cmake -S gpu -B out/tour/gpu -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBB_TOUR=ON -DBB_PGO=off \
    -DBB_LTO=ON -DBB_PGO_DIR="$PWD/pgo" >/dev/null
if ! ninja -C out/tour/gpu bbgpu > out/tour/gpu-build.log 2>&1; then
    grep -v '^\[' out/tour/gpu-build.log | tail -40 >&2
    echo 'GPU library build failed (full log: out/tour/gpu-build.log)' >&2; exit 1
fi
read -r -a includes <<< "$(pkg-config --cflags vulkan sdl3)"
read -r -a libraries <<< "$(pkg-config --libs vulkan sdl3)"
libraries=("${libraries[@]/-mwindows/-mconsole}")
mapfile -t gpu < out/tour/gpu/bbgpu_link.txt
clang -std=gnu11 -D_FILE_OFFSET_BITS=64 -D_WIN32_WINNT=0x0A00 -O2 -g -Wall -Wextra -Werror \
    "${includes[@]}" -I. -Isrc src/probe.c src/runtime*.c src/win32_*.c src/vulkan_smoke.c \
    out/libatrac9.a -lm "${gpu[@]}" "${libraries[@]}" \
    -Wl,--disable-dynamicbase,--disable-high-entropy-va -lwinmm -lws2_32 -lpsapi -flto=thin \
    -o out/tour/bb-probe
cp -f out/bb-gpu-capabilities.exe out/tour/ 2>/dev/null || true
echo "Built $PWD/out/tour/bb-probe.exe"
