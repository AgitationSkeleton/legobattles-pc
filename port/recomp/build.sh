#!/usr/bin/env bash
# Build this port's own nds_recompile without touching upstream ndsrecomp.
#
#   ./port/recomp/build.sh
#
# Same contract as port/runner/build.sh: $NDSRECOMP is READ-ONLY, the tree is
# assembled here, apply_finder_patch.py patches the copy, and the patched
# recompiler lands at port/recomp/build/nds_recompile.exe. port/lift.sh picks
# that up automatically when it exists (LB_RECOMPILE overrides).
#
# tree/ and build/ are gitignored: assembled output, not source. The patch
# itself is the reviewable artefact and lives in apply_finder_patch.py.
set -eu

W="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
NDSRECOMP="${NDSRECOMP:-${LB_TOOLS:-$W/../../../tools}/ndsrecomp}"
TREE="$W/port/recomp/tree"
BUILD="$W/port/recomp/build"
export PATH="/c/msys64/ucrt64/bin:$PATH"

[ -d "$NDSRECOMP/recompiler" ] || { echo "missing: $NDSRECOMP/recompiler"; exit 1; }

echo "== assembling recompiler tree =="
rm -rf "$TREE"
mkdir -p "$TREE"
# recompiler/CMakeLists.txt includes ../cmake/*.cmake, so cmake/ must sit
# beside it. recompiler/third_party/toml.hpp ships upstream, so the toml++
# staging step in NdsTomlPlusPlus.cmake finds it and does not reach the
# network.
cp -rp "$NDSRECOMP/cmake"       "$TREE/cmake"
cp -rp "$NDSRECOMP/recompiler"  "$TREE/recompiler"
echo "  copied cmake recompiler"

echo
echo "== applying finder patch =="
python "$W/port/recomp/apply_finder_patch.py" "$TREE/recompiler"

echo
echo "== configuring =="
mkdir -p "$BUILD"
cmake -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_CXX_COMPILER=/c/msys64/ucrt64/bin/g++.exe \
      -DCMAKE_C_COMPILER=/c/msys64/ucrt64/bin/gcc.exe \
      -S "$TREE/recompiler" -B "$BUILD" > "$BUILD/configure.log" 2>&1 || {
    echo "configure FAILED - tail of $BUILD/configure.log:"
    tail -25 "$BUILD/configure.log"; exit 1; }
echo "  ok"

echo
echo "== building =="
cmake --build "$BUILD" --target nds_recompile -j "${LB_JOBS:-6}" \
      > "$BUILD/build.log" 2>&1 || {
    echo "build FAILED - errors from $BUILD/build.log:"
    grep -iE "error" "$BUILD/build.log" | head -20; exit 1; }
echo "  ok"
ls -la "$BUILD/nds_recompile.exe"
