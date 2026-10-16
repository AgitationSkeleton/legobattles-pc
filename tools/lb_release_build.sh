#!/usr/bin/env bash
# The build step of legobattles-pc's .github/workflows/release.yml.
#
# A file rather than an inline `bash -lc '...'`: Windows PowerShell 5.1 drops
# embedded double quotes when it passes an argument to a native program, so a
# quoted command line arrives at bash broken. Everything comes in through the
# environment instead:
#
#   LB_WS          the checkout (a Windows path)
#   LB_GENERATED   the lifted title code to build against (a Windows path)
#   NDSRECOMP, IMGUI, LB_TOOLS   as port/runner/build.sh reads them
set -eu
cd "$(cygpath -u "$LB_WS")"
LB_GENERATED_DIR="$(cygpath -u "$LB_GENERATED")"
export LB_GENERATED_DIR
exec bash port/runner/build.sh
