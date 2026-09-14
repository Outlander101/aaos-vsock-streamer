#!/usr/bin/env bash
set -euo pipefail
# x86 build
cmake -S . -B build/x86 -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/x86 -j
# arm64 cross build (NO toolchain folder needed)
cmake -S . -B build/arm64 -G Ninja -DCMAKE_BUILD_TYPE=Release \
 -DCMAKE_CXX_COMPILER=aarch64-linux-gnu-g++
cmake --build build/arm64 -j
echo "DONE:"
echo "  x86   -> build/x86/fram_broker"
echo "  arm64 -> build/arm64/fram_broker"