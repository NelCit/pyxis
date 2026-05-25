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
$lobby = Join-Path $repoRoot "build\omni\pyxis_hydra_omni_lobby.exe"
$releaseBin = Join-Path $repoRoot "build\dev\bin\Release"
if ($Scene -ne "") { $usd = $Scene } else {
    $usd = Join-Path $repoRoot "resources\scenes\world_lobby\World_Lobby.usd"
}

# Stage shaders next to the exe (PyxisEngine searches next to the executable).
$shaderSrc = Join-Path $releaseBin "Resources\shaders"
if (-not (Test-Path $shaderSrc)) { $shaderSrc = Join-Path $repoRoot "build\dev\bin\Debug\Resources\shaders" }
$shaderDst = Join-Path $repoRoot "build\omni\Resources\shaders"
New-Item -ItemType Directory -Force -Path $shaderDst | Out-Null
Copy-Item (Join-Path $shaderSrc "*.spv") $shaderDst -Force
# default_sky.exr fallback for the dome light.
$sceneSrc = Join-Path $releaseBin "Resources\scenes"
$sceneDst = Join-Path $repoRoot "build\omni\Resources\scenes"
New-Item -ItemType Directory -Force -Path $sceneDst | Out-Null
if (Test-Path $sceneSrc) { Copy-Item (Join-Path $sceneSrc "*") $sceneDst -Recurse -Force }

$env:PATH = "$PXR_USD_ROOT\lib;$PXR_USD_ROOT\bin;$PYTHON_ROOT;$releaseBin;" + $env:PATH
$env:PXR_PLUGINPATH_NAME = "$PXR_USD_ROOT\lib\usd;$PXR_USD_ROOT\plugin\usd"

& $lobby $usd $Camera $Width $Height $Frames
$rc = $LASTEXITCODE

$bmp = Join-Path $repoRoot "build\omni\pyxis_world_lobby.bmp"
if ($Out -ne "" -and (Test-Path $bmp)) {
    Copy-Item $bmp $Out -Force
    Write-Host "run_lobby: copied $bmp -> $Out"
}
exit $rc
