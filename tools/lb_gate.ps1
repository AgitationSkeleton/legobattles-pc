# The exactness gate (docs/430; was a session script, verify430c.ps1).
#
# Replays lb-session-1790473886.lbdemo twice -- adaptive (Enhanced) and DS
# Vanilla -- and checks:
#   * the [lb_counts] lines of both equal screenshots\exp-rep416a's
#     (simulation state at four frames: the replay is deterministic);
#   * 240 presented frames of each equal, by decoded pixels, the reference
#     captures: the adaptive run against $AdRef, the Vanilla run against
#     $VanRef (made from the gated builds in the worktrees lbwt2 and lbwt3).
#
#   powershell -ExecutionPolicy Bypass -File tools\lb_gate.ps1
#
# Pass: "adaptive counts IDENTICAL", "Vanilla counts IDENTICAL", and both
# "240 identical by pixels, 0 differ, 0 missing". Run nothing else heavy at
# the same time (it uses harness offset 0, debug port 19842).
#
# The references default to the worktrees beside this repository
# (..\lbwt2 and ..\lbwt3); name others with -AdRef / -VanRef.
param([string]$AdRef = "", [string]$VanRef = "")
$main = Split-Path -Parent $PSScriptRoot
$side = Split-Path -Parent $main
if (-not $AdRef) { $AdRef = Join-Path $side "lbwt2\screenshots\exp-ad430base" }
if (-not $VanRef) { $VanRef = Join-Path $side "lbwt3\screenshots\exp-van430" }
Set-Location $main
$env:LB_HARNESS_OFFSET = '0'
$demo = "$main\lb-session-1790473886.lbdemo"
$px = "$main\tools\lb_pxcompare.py"
$common = @("--demo", $demo, "--env", "LB_WINDOW=1473x830", "--env", "LB_COUNTS_AT=2000,3500,4400,4675", "--env", "LB_DUMP_START=4000", "--env", "LB_DUMP_SEQ=60", "--env", "LB_DUMP_EVERY=1")
$runs = @(@("ad430m", $common), @("van430m", (@("--exe-arg=--adaptive-widescreen", "--exe-arg=none") + $common)))
foreach ($r in $runs) {
    if (Test-Path "screenshots\exp-$($r[0])") { Remove-Item -Recurse -Force "screenshots\exp-$($r[0])" }
    $a = $r[1]
    python tools/lb_exp.py $r[0] @a 2>&1 | Out-Null
    "== $($r[0])"
    Get-Content "screenshots\exp-$($r[0])\fps.txt" -ErrorAction SilentlyContinue | Select-Object -First 1
}
$x = Select-String -Path "screenshots\exp-ad430m\stderr.log" -Pattern "lb_counts\]" | ForEach-Object { $_.Line }
$z = Select-String -Path "screenshots\exp-rep416a\stderr.log" -Pattern "lb_counts\]" | ForEach-Object { $_.Line }
$v = Select-String -Path "screenshots\exp-van430m\stderr.log" -Pattern "lb_counts\]" | ForEach-Object { $_.Line }
if ($x.Count -gt 0 -and ($x -join "`n") -eq ($z -join "`n")) { "adaptive counts IDENTICAL to rep416a" } else { "adaptive counts DIFFER"; $x }
if ($v.Count -gt 0 -and ($v -join "`n") -eq ($z -join "`n")) { "Vanilla counts IDENTICAL to rep416a" } else { "Vanilla counts DIFFER from rep416a"; $v }
py -3.13 $px "$main/screenshots/exp-ad430m" $AdRef '^(presented|screen)-(top|bot)-\d+-\d+x\d+\.png$' 0
py -3.13 $px "$main/screenshots/exp-van430m" $VanRef '^(presented|screen)-(top|bot)-\d+-\d+x\d+\.png$' 0
"done"
