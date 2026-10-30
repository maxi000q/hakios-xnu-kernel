#!/bin/bash
# Build and run the demo-kernel screen dumper (tests/dump_demo_screen.cpp):
# the same core the desktop rig compiles, one run, the framebuffer as PPM.
#
#   tests/dump_demo_screen.sh                      # final state -> demo-screen.ppm
#   tests/dump_demo_screen.sh 40000000 mid.ppm     # stop mid-show (the boot screen)
set -eu
cd "$(dirname "$0")/.."
ROOT=$(pwd)
OUT=${OUT:-$ROOT/../build/tests}
CXX=${CXX:-g++}
CXXFLAGS="-std=c++17 -O2 -I$ROOT/native/include"
mkdir -p "$OUT"

sources=""
for d in native/memory native/cpu native/image native/gpu native/core; do
    for f in "$d"/*.cpp; do
        [ -e "$f" ] && sources="$sources $f"
    done
done

# shellcheck disable=SC2086
$CXX $CXXFLAGS tests/dump_demo_screen.cpp $sources -o "$OUT/dump_demo_screen"

if [ $# -eq 0 ]; then
    "$OUT/dump_demo_screen" - "$OUT/demo-screen.ppm"
else
    "$OUT/dump_demo_screen" - "$2" "$1"
fi
