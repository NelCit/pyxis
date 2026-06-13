# RFC 0006 — run the World Lobby through the Pyxis Hydra DELEGATE (generic Hydra
# path, no omni.hydra.pxr) and copy the resulting BMP to a named output. Used by
# the §25.O.3 ingest-parity check (delegate vs usd_direct).
#
#   pwsh _tools/omniverse/run_lobby.ps1 -Frames 64 -Width 952 -Height 576 `
#        -Camera /World/Cameras/CamLobbyWide -Out delegate_64.bmp

param(
    [int]$Frames = 1,
    [int]$Width = 952,
    [int]$Height = 576,
    [string]$Camera = "/World/Cameras/CamLobbyWide",
    [string]$Scene = "",   # default: World Lobby (resolved below)
    [string]$Out = ""
)

$ErrorActionPreference = "Continue"
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$omniDir  = Join-Path $repoRoot "build\omniverse"

$pathsFile = Join-Path $omniDir "usd-deps\paths.ps1"
if (Test-Path $pathsFile) { . $pathsFile } else {
    $PXR_USD_ROOT = Join-Path $omniDir "usd-deps\usd"
    $PYTHON_ROOT = (Get-ChildItem (Join-Path $omniDir "packman\chk\python") -Directory -ErrorAction SilentlyContinue |
                    Where-Object Name -like "3.12.*" | Sort-Object Name -Descending |
                    Select-Object -First 1).FullName
}
$releaseBin = Join-Path $repoRoot "build\dev\bin\Release"
# RFC 0007 — the lobby harness now builds in build/dev next to pyxis.exe, which
# already stages Resources/shaders + Resources/scenes (the dome's default_sky.exr
# fallback) there, so PyxisEngine ("shaders next to the exe") finds them with no
# extra staging. The harness writes its BMP next to the exe (build/dev/bin/Release).
$lobby = Join-Path $releaseBin "pyxis_hydra_omni_lobby.exe"
if ($Scene -ne "") { $usd = $Scene } else {
    $usd = Join-Path $repoRoot "resources\scenes\world_lobby\World_Lobby.usd"
}

$env:PATH = "$PXR_USD_ROOT\lib;$PXR_USD_ROOT\bin;$PYTHON_ROOT;$releaseBin;" + $env:PATH
# No "$PXR_USD_ROOT\lib\usd" here: the exe loads the usd_*.dll copies staged next to
# it (app dir beats PATH) and USD auto-discovers their <exe-dir>\usd core plugin
# tree. Registering usd-deps\lib\usd too makes Plug's dedup point core plugins at the
# OTHER tree, so a lazy PlugPlugin::Load pulls a second copy of an already-loaded
# usd DLL and busy-spins in its static initializers (see run_smoke.ps1 for the full
# autopsy). Only the second-tier plugin\usd tree (unique DLLs) may be listed.
$env:PXR_PLUGINPATH_NAME = "$PXR_USD_ROOT\plugin\usd"

& $lobby $usd $Camera $Width $Height $Frames
$rc = $LASTEXITCODE

$bmp = Join-Path $releaseBin "pyxis_world_lobby.bmp"
if ($Out -ne "" -and (Test-Path $bmp)) {
    Copy-Item $bmp $Out -Force
    Write-Host "run_lobby: copied $bmp -> $Out"
}
exit $rc
