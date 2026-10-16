#!/usr/bin/env bash
# Run the ndsrecomp static lift over every LEGO Battles program.
#
#   ./port/lift.sh audit      validate only, emit nothing
#   ./port/lift.sh emit       write recompiled C into port/generated/
#   ./port/lift.sh compile    emit, then compile every bank to objects
#
# The ARM9 is sharded 16 ways. That is not cosmetic: unsharded it is one
# 191 MB .c file. MetroidPrimeHuntersRecomp uses the same flags, which is
# where they came from.
#
# --validate-live-bytes makes the recompiler check every function against the
# live guest bytes before lowering it. This project assumes nothing about the
# code it lifts, so that is not optional here.
#
# port/generated/ is gitignored: it is the cartridge's own code translated to
# C, and committing it would be redistributing the game.
set -u
MODE="${1:-audit}"
W="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# This port's own recompiler when it has been built (port/recomp/build.sh),
# otherwise the stock upstream one. The port build carries the finder patch
# that makes a declared exact_size the function's extent instead of an upper
# bound -- without it 32% of the declared ARM9 instructions are never lifted
# and the runtime interprets them. See port/recomp/apply_finder_patch.py.
# (Tools are under LB_TOOLS, else a tools/ folder three levels above this
# checkout, as port/runner/build.sh.)
TOOLS="${LB_TOOLS:-$W/../../../tools}"
R="${LB_RECOMPILE:-}"
if [ -z "$R" ]; then
    if [ -x "$W/port/recomp/build/nds_recompile.exe" ]; then
        R="$W/port/recomp/build/nds_recompile.exe"
    else
        R="$TOOLS/ndsrecomp-build/nds_recompile.exe"
    fi
fi
echo "  recompiler: $R"
RTD="${NDSRECOMP:-$TOOLS/ndsrecomp}/recompiler/armv4t"
export PATH="/c/msys64/ucrt64/bin:$PATH"
OUT="$W/port/generated"
OBJ="$W/port/obj"
[ "$MODE" = "audit" ] || mkdir -p "$OUT"
[ "$MODE" = "compile" ] && mkdir -p "$OBJ"

OKC=0; FAILED=0; TOTF=0

run() {  # run <config> <binary> <bank> [shards]
    local cfg="$1" bin="$2" bank="$3" shards="${4:-2}" args
    if [ "$MODE" = "audit" ]; then
        args=(--audit)
    else
        args=(--out "$OUT" --bank "$bank" --validate-live-bytes)
        # SHARDS IS NEVER 1 OR 0. The runner globs bank bodies as
        # "*_[0-9][0-9].c"; an unsharded bank emits "lb_arm9_ov000.c" with no
        # numeric suffix, so CMake silently globs it OUT and only the dispatch
        # object reaches libnds_banks.a - which links as undefined references
        # to every function in the bank. Two shards is the minimum that gets
        # the suffix.
        args+=(--shards "$shards" --stable-address-shards
               --max-function-bytes 512)
    fi
    local o; o="$("$R" --config "$cfg" --bin "$bin" "${args[@]}" 2>&1)"
    if ! echo "$o" | grep -q "verified"; then
        echo "  FAIL $(basename "$cfg")"; echo "$o" | grep -i error | head -2
        FAILED=$((FAILED+1)); return
    fi
    local f u
    f="$(echo "$o" | grep -o 'functions=[0-9]*' | head -1)"
    u="$(echo "$o" | grep -o 'undefined=[0-9]*' | head -1)"
    printf "  ok   %-22s %-16s %s\n" "$bank" "$f" "$u"
    OKC=$((OKC+1)); TOTF=$((TOTF + $(echo "$f" | tr -dc '0-9')))
}

echo "== $MODE =="
run "$W/port/config/lb_arm9.toml"        "$W/out/eur/arm9/section_static_02000000.bin" lb_arm9 16
run "$W/port/config/lb_arm9_itcm.toml"   "$W/out/eur/arm9/section_itcm_01ff8000.bin"   lb_arm9_itcm
run "$W/port/config/lb_arm7_static.toml" "$W/out/eur/arm7/section_static_02380000.bin" lb_arm7_static
run "$W/port/config/lb_arm7_wram.toml"   "$W/out/eur/arm7/section_wram_037f8000.bin"   lb_arm7_wram
run "$W/port/config/lb_arm7_mainram.toml" "$W/out/eur/arm7/section_mainram_027e0000.bin" lb_arm7_mainram
for c in "$W"/port/config/lb_ov*.toml; do
    i="$(basename "$c" .toml)"; i="${i#lb_ov}"
    run "$c" "$W/out/eur/overlays/ov$i.bin" "lb_arm9_ov$i"
done
echo
echo "  $OKC programs ok, $FAILED failed, $TOTF functions"
[ "$FAILED" -eq 0 ] || exit 1

if [ "$MODE" = "compile" ]; then
    echo; echo "== compiling =="
    cfail=0; n=0
    for f in "$OUT"/*.c; do
        b="$(basename "$f" .c)"
        ( gcc -c -O0 -I"$RTD" "$f" -o "$OBJ/$b.o" 2>"$OBJ/$b.err" ) &
        n=$((n+1))
    done
    wait
    for e in "$OBJ"/*.err; do
        if [ -s "$e" ]; then
            echo "  FAIL $(basename "$e" .err)"; head -3 "$e"; cfail=$((cfail+1))
        fi
    done
    echo "  $(ls "$OBJ"/*.o 2>/dev/null | wc -l) of $n objects built, $cfail failed"
    [ "$cfail" -eq 0 ] || exit 1
fi
