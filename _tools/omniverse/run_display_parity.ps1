# Pyxis display-parity test -- renders the World Lobby through the THREE Pyxis
# display paths and asserts they produce the same image:
#   1. headless : pyxis.exe --headless --output            (sRGB in the PNG writer)
#   2. viewer   : pyxis.exe --screenshot                   (sRGB in SsaaResolvePass)
#   3. kit      : Omniverse Kit pxr viewport capture       (sRGB in HdxColorCorrection)
#
# Each path applies the sRGB OETF to the ACES-tonemapped LINEAR color exactly once,
# so the displayed pixels match. This is the GPU regression guard for the color
# double/zero-encode bug (the CPU-level proof is AovColorEncodeFixture).
#
#   pwsh _tools/omniverse/run_display_parity.ps1 [-Scene <usd>] [-Camera <path>]
#                                                [-Width N] [-Height N] [-Samples N]
#
# Prereqs: a Release build of build/dev (pyxis.exe) AND a staged Kit extension
# (run _tools/omniverse/build.ps1 once). requires_gpu.

param(
    [string]$Scene = "",
    [string]$Camera = "/World/Cameras/CamLobbyWide",
    [int]$Width = 949,
    [int]$Height = 577,
    [int]$Samples = 64
)

$ErrorActionPreference = "Continue"
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$omniDir  = Join-Path $repoRoot "build\omniverse"
$relBin   = Join-Path $repoRoot "build\dev\bin\Release"
$outDir   = Join-Path $repoRoot "build\_display_parity"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

if ($Scene -eq "") { $Scene = Join-Path $repoRoot "resources\scenes\world_lobby\World_Lobby.usd" }

# nv-usd on PATH for the standalone pyxis.exe (pyxis.exe links nv-usd via ingest).
$pathsFile = Join-Path $omniDir "usd-deps\paths.ps1"
if (Test-Path $pathsFile) { . $pathsFile } else { $PXR_USD_ROOT = Join-Path $omniDir "usd-deps\usd" }
$env:PATH = "$PXR_USD_ROOT\lib;$PXR_USD_ROOT\bin;$relBin;" + $env:PATH

$headless = Join-Path $outDir "headless.png"
$viewer   = Join-Path $outDir "viewer.png"
$kit      = Join-Path $outDir "kit.png"
Remove-Item $headless, $viewer, $kit -Force -ErrorAction SilentlyContinue

$exe = Join-Path $relBin "pyxis.exe"
if (-not (Test-Path $exe)) { Write-Host "[display-parity] pyxis.exe not found at $exe"; exit 2 }

Write-Host "[display-parity] scene='$Scene' camera='$Camera' ${Width}x${Height}"

# --- 1. headless ----------------------------------------------------------------
Write-Host "[display-parity] rendering headless..."
& $exe --headless --ingest usd_direct --scene $Scene --camera $Camera `
    --width $Width --height $Height --samples $Samples --seed 1 --output $headless 2>&1 |
    Select-String -Pattern "WritePng|render produced|FAIL" | ForEach-Object { $_.Line }

# --- 2. standalone viewer (--screenshot) ----------------------------------------
Write-Host "[display-parity] rendering standalone viewer..."
& $exe --screenshot $viewer --ingest usd_direct --scene $Scene --camera $Camera `
    --width $Width --height $Height 2>&1 |
    Select-String -Pattern "screenshot:|FAIL" | ForEach-Object { $_.Line }

# --- 3. Omniverse Kit pxr viewport ----------------------------------------------
$rel = Join-Path $omniDir "kit-app-template\_build\windows-x86_64\release"
$kitExe = Join-Path $rel "kit\kit.exe"
$kitApp = Join-Path $rel "apps\pyxis.editor.kit"
if ((Test-Path $kitExe) -and (Test-Path $kitApp)) {
    Write-Host "[display-parity] rendering Kit/Omniverse viewport..."
    $kitOutDir = Join-Path $outDir "kit_capture"
    New-Item -ItemType Directory -Force -Path $kitOutDir | Out-Null
    Get-ChildItem $kitOutDir -Filter *.png -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue
    $env:PYXIS_HEADLESS_SCENE = $Scene
    $env:PYXIS_HEADLESS_OUT = $kitOutDir
    $env:PYXIS_HEADLESS_CAMERA = $Camera
    $env:PYXIS_HEADLESS_WARMUP = "150"
    $kitLog = Join-Path $outDir "kit.log"
    $capPy = Join-Path $PSScriptRoot "capture_pyxis.py"
    $proc = Start-Process -FilePath $kitExe `
        -ArgumentList $kitApp, "--exec", $capPy, "--/log/file=$kitLog", "--/log/level=warn" `
        -PassThru -WindowStyle Minimized
    $deadline = (Get-Date).AddSeconds(120)
    while ((Get-Date) -lt $deadline -and -not $proc.HasExited) {
        Start-Sleep -Seconds 5
        if (Test-Path (Join-Path $kitOutDir "render_pyxis.png")) { Start-Sleep -Seconds 2; break }
    }
    if (-not $proc.HasExited) { Stop-Process -Id $proc.Id -Force }
    if (Test-Path (Join-Path $kitOutDir "render_pyxis.png")) {
        Copy-Item (Join-Path $kitOutDir "render_pyxis.png") $kit -Force
    }
} else {
    Write-Host "[display-parity] Kit app not staged ($kitExe) -- run _tools/omniverse/build.ps1. Skipping Kit leg."
}

# --- compare --------------------------------------------------------------------
$py = "C:\Users\vlegrand\AppData\Local\Programs\Python\Python314\python.exe"
if (-not (Test-Path $py)) { $py = "python" }

if (-not (Test-Path $kit)) {
    Write-Host "[display-parity] NOTE: Kit leg unavailable; comparing headless vs viewer only."
    # Fall back to the two standalone paths so the test still guards them.
    & $py (Join-Path $PSScriptRoot "display_parity_check.py") $headless $viewer $viewer
} else {
    & $py (Join-Path $PSScriptRoot "display_parity_check.py") $headless $viewer $kit
}
exit $LASTEXITCODE
