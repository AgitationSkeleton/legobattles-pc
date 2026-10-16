# Lifting the BIOS banks

The runner links four BIOS banks unconditionally from ndsrecomp's own
`generated/` directory (`runner/CMakeLists.txt` hardcodes them; they are not
`fw_*`-globbed and they are not built for you):

```
export PATH="/c/msys64/ucrt64/bin:$PATH"
N=$NDSRECOMP                                     # upstream ndsrecomp checkout
R=<your ndsrecomp build>/nds_recompile.exe

"$R" --config $N/bios/biosnds7.toml --bin $N/bios/biosnds7.rom \
     --out $N/generated --bank arm7_bios          # 637 functions
"$R" --config $N/bios/biosnds9.toml --bin $N/bios/biosnds9.rom \
     --out $N/generated --bank arm9_bios          # 167 functions
"$R" --config $N/bios/freebios7.toml \
     --bin $N/third_party/freebios/drastic_bios_arm7.bin \
     --out $N/generated --bank freebios_arm7      #  72 functions
"$R" --config $N/bios/freebios9.toml \
     --bin $N/third_party/freebios/drastic_bios_arm9.bin \
     --out $N/generated --bank freebios_arm9      #  70 functions
```

## Do NOT pass `--validate-live-bytes` here

`port/lift.sh` passes it for every cartridge bank and must keep doing so. On the
BIOS banks it makes the port **unbootable**, and silently — see `docs/94`.

`lookup_in()` rejects a validated entry unless
`bus_range_has_write_provenance()` succeeds, and `written_for_ptr()` shadows
only `g_main_ram`, `g_itcm`, `g_dtcm`, `g_shared_wram` and `g_arm7_wram`. BIOS
is not among them and `bus_load_arm7_bios()` is a plain `memcpy`, so no BIOS
address can ever have provenance. Every validated BIOS entry is permanently
inactive, and the first `SWI` the ARM7 executes dies at the vector with

```
[dispatch-miss] cpu=arm7 pc=0x00000008 arm
```

Nothing is lost by omitting it: the runner hashes both images before executing
anything (`[load] arm7 bios: 16384 bytes, SHA-1 ok`), and ROM cannot change
afterwards. A SHA-1 over the whole image is strictly stronger than a four-byte
re-check at `0x08`.

## Which backend runs

Both are linked; `--freebios` selects FreeBIOS (BSD-2-Clause, redistributable)
and the default uses the real dumps. The dumps live outside the repository and
are never committed. **A distributable build must ship the FreeBIOS path**,
which also requires `--boot direct`.

## The FreeBIOS path is verified

A run with `--freebios --boot direct` is **indistinguishable** from the
real-dump run at the same budget: 0 dispatch misses, the same ARM9 PC, the same
`IME`/`IE` on both cores, and the same 85 executed code pages. The distributable
build needs no cartridge-maker BIOS dumps.

```
[bios] FREEBIOS (opt-in, no dumps): arm9 4096 bytes SHA-1 b85d6afd..., arm7 16384 bytes SHA-1 eaab9b21...
```
