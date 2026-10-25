# Copies the fallback server to its machine, ready to run (docs/442):
#   powershell -ExecutionPolicy Bypass -File deploy.ps1 -Target <server folder>
# (or set LB_FALLBACK_TARGET once; the folder is usually a network share).
# The Worker's current source (../master-server-cloudflare/src) goes in as
# worker/, and node_modules is installed fresh in a staging folder first, so
# the target needs no npm. The target's config.json, data/ and logs/ are never
# touched. Restart it afterwards on that machine: pm2 restart legobattles-master-fallback
param([string]$Target = $env:LB_FALLBACK_TARGET)
$ErrorActionPreference = 'Stop'
if (-not $Target) { throw 'Name the server folder: -Target <folder>, or set LB_FALLBACK_TARGET.' }
$here = $PSScriptRoot
$stage = Join-Path $env:TEMP 'lb-fallback-stage'
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force $stage | Out-Null
foreach ($f in 'server.mjs', 'package.json', 'ecosystem.config.cjs', 'config.example.json', 'README.md', 'start.bat') {
    Copy-Item (Join-Path $here $f) $stage
}
New-Item -ItemType Directory -Force (Join-Path $stage 'worker') | Out-Null
Copy-Item (Join-Path $here '..\master-server-cloudflare\src\*.js') (Join-Path $stage 'worker')
Push-Location $stage
try {
    npm install --omit=dev --no-audit --no-fund
    if ($LASTEXITCODE -ne 0) { throw "npm install failed ($LASTEXITCODE)" }
} finally { Pop-Location }
if (-not (Test-Path $Target)) { New-Item -ItemType Directory -Force $Target | Out-Null }
# /MIR within the copied parts only; config.json, data and logs stay as they are.
robocopy $stage $Target /MIR /XD data logs /XF config.json /NFL /NDL /NJH /NP | Out-Null
if ($LASTEXITCODE -ge 8) { throw "robocopy failed ($LASTEXITCODE)" }
if (-not (Test-Path (Join-Path $Target 'config.json'))) {
    Copy-Item (Join-Path $Target 'config.example.json') (Join-Path $Target 'config.json')
    "config.json created from the example: fill in DISCORD_WEBHOOK and ADMIN_KEY there."
}
"Deployed to $Target"
