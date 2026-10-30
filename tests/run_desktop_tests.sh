#!/bin/bash
# The desktop test rig for the hakios native core.
#
# Builds with the host g++ -- the same C++ the NDK compiles for Android -- and
# runs every tests/test_*.cpp against the whole core. No framework, no
# dependencies: a stage that does not pass here does not get pushed.
#
#   ./tests/run_desktop_tests.sh          # build + run, nonzero exit on failure
#   CXX=clang++ ./tests/run_desktop_tests.sh

set -u
cd "$(dirname "$0")/.."
ROOT=$(pwd)
OUT=${OUT:-$ROOT/../build/tests}
CXX=${CXX:-g++}
CXXFLAGS="-std=c++17 -O2 -Wall -Wextra -Werror -I$ROOT/native/include"

mkdir -p "$OUT"

# every source of the core links into every test
sources=""
for d in native/memory native/cpu native/image native/gpu native/core; do
    for f in "$d"/*.cpp; do
        [ -e "$f" ] && sources="$sources $f"
    done
done
rc=0

for t in tests/test_*.cpp; do
    name=$(basename "$t" .cpp)
    echo "== $name"
    # shellcheck disable=SC2086
    if ! $CXX $CXXFLAGS "$t" $sources -o "$OUT/$name"; then
        echo "build failed: $name"
        rc=1
        continue
    fi
    if ! "$OUT/$name" "$OUT"; then
        rc=1
    fi
done

if [ $rc -eq 0 ]; then
    echo "desktop tests: OK"
else
    echo "desktop tests: FAILED"
fi
exit $rc
