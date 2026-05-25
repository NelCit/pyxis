# RFC 0006 §25.O.3 — automatic ingest-parity regression test.
#
# Renders ONE scene through BOTH ingest adapters and asserts they match:
#   * Hydra DELEGATE  : pyxis_hydra_omni_lobby.exe   (ingests via StageWalker)
#   * USD-DIRECT      : pyxis.exe --ingest usd_direct (StageWalker)
#
# Because the delegate now ingests the stage through the SAME StageWalker as the
# standalone, the GpuScene — geometry, face-subsets, materials, lights, analytic
# geom, instancing, camera — is byte-identical by construction, so the render
# matches to within sRGB output quantization. parity_check.py HARD-GATES the
# lights + materials (byte-identical) and the image (tight MAD / correlation
# tolerance). Any divergence fails the test — this is the regression guard.
#
# Prereqs: build pyxis.exe (build/dev) and the omni lobby target (build/omni).
#   pwsh _tools/omniverse/run_parity.ps1 [-Scene <usd>] [-Camera <path>]
#                                        [-Width N] [-Height N] [-Name <id>]
# No -Scene => the World Lobby (the comprehensive fixture).

param(
    [string]$Scene = "",
    [string]$Camera = "/World/Cameras/CamLobbyWide",
    [int]$Width = 952,
    [int]$Height = 576,
    [int]$Frames = 1,
    [string]$Name = "world_lobby"
)

$ErrorActionPreference = "Continue"
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$out = Join-Path $repoRoot "build\_parity_out\$Name"
New-Item -ItemType Directory -Force -Path $out | Out-Null
$relBin = Join-Path $repoRoot "build\dev\bin\Release"
if ($Scene -ne "") { $usdScene = $Scene } else {
    $usdScene = Join-Path $repoRoot "resources\scenes\world_lobby\World_Lobby.usd"
}

Write-Host "[parity] fixture='$Name' scene='$usdScene' camera='$Camera' ${Width}x${Height}"

# --- 1. USD-DIRECT (pyxis.exe) -------------------------------------------------
$env:PYXIS_OMNI_LIGHTDUMP = "1"; $env:PYXIS_OMNI_MATDUMP = "1"
& "$relBin\pyxis.exe" --headless --ingest usd_direct --scene $usdScene --camera $Camera `
    --width $Width --height $Height --samples $Frames --seed 1 `
    --output (Join-Path $out "usd_direct.png") 2>&1 |
    Where-Object { $_ -match '^(LIGHTDUMP|MATDUMP)' } |
    Out-File -Encoding utf8 -Width 1000 (Join-Path $out "usd_dump.txt")
Remove-Item Env:\PYXIS_OMNI_LIGHTDUMP, Env:\PYXIS_OMNI_MATDUMP -ErrorAction SilentlyContinue

# --- 2. Hydra DELEGATE (lobby harness, generic Hydra path) ---------------------
$env:PYXIS_OMNI_LIGHTDUMP = "1"; $env:PYXIS_OMNI_MATDUMP = "1"
& (Join-Path $PSScriptRoot "run_lobby.ps1") -Scene $usdScene -Frames $Frames `
    -Width $Width -Height $Height -Camera $Camera -Out (Join-Path $out "delegate.bmp") 2>&1 |
    Where-Object { $_ -match '^(LIGHTDUMP|MATDUMP)' } |
    Out-File -Encoding utf8 -Width 1000 (Join-Path $out "hyd_dump.txt")
Remove-Item Env:\PYXIS_OMNI_LIGHTDUMP, Env:\PYXIS_OMNI_MATDUMP -ErrorAction SilentlyContinue

# --- 3. Compare (hard gate) ----------------------------------------------------
$py = "C:\Users\vlegrand\AppData\Local\Programs\Python\Python314\python.exe"
if (-not (Test-Path $py)) { $py = "python" }
& $py (Join-Path $PSScriptRoot "parity_check.py") `
    (Join-Path $out "hyd_dump.txt") (Join-Path $out "usd_dump.txt") `
    (Join-Path $out "hyd_dump.txt") (Join-Path $out "usd_dump.txt") `
    (Join-Path $out "delegate.bmp") (Join-Path $out "usd_direct.png")
exit $LASTEXITCODE
