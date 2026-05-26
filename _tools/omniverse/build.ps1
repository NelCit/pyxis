# RFC 0007 — build the Pyxis Hydra delegate (in build/dev) and stage the
# omni.hydra.pyxis Kit extension.
#
# RFC 0007 retired the separate pyxis_hydra_omni C++ module + its out-of-tree
# build/omni: both build/dev and Kit are nv-usd 25.11, so the SINGLE delegate
# pyxis_hydra.dll (built in build/dev) loads directly in Kit. This script:
#   1. builds pyxis_hydra (+ the headless test exes) in build/dev via the normal
#      CMake preset (configure once if build/dev is absent);
#   2. assembles the omni.hydra.pyxis extension = the Python-only extension tree
#      (_tools/omniverse/extension) + the prebuilt pyxis_hydra.dll + its staged
#      plugInfo + the Pyxis runtime DLLs, copied into the Kit app's exts.
# There is NO compiled C++ in the extension (no native IExt, no viewport panel):
# the Python __init__.py registers the delegate's plugInfo with USD's
# PlugRegistry and drives omni.hydra.pxr.
#
#   pwsh _tools/omniverse/build.ps1 [-Config Release]

[CmdletBinding()]
param([ValidateSet("Release","RelWithDebInfo")] [string]$Config = "Release")
$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$omniDir  = Join-Path $repoRoot "build\omniverse"
$buildDir = Join-Path $repoRoot "build\dev"           # the single Pyxis build tree
$kitDir   = Join-Path $omniDir "kit-app-template"

# Load the paths setup.ps1 produced (nv-usd + Python roots) — informational; the
# build/dev CMake resolves nv-usd itself via _cmake/PyxisNvUsd.cmake.
$pathsFile = Join-Path $omniDir "usd-deps\paths.ps1"
if (Test-Path $pathsFile) {
    . $pathsFile   # defines $PXR_USD_ROOT, $PYTHON_ROOT (and $PM_PACKAGES_ROOT)
    Write-Host "[build] nv-usd : $PXR_USD_ROOT"
    Write-Host "[build] python : $PYTHON_ROOT"
} else {
    Write-Host "[build] WARN: $pathsFile not found; run _tools/omniverse/setup.ps1 first." -ForegroundColor Yellow
}

# clang-cl is the project compiler.
$env:CC = "clang-cl"; $env:CXX = "clang-cl"

# 1. Build the delegate + the headless test exes in build/dev (Release). Configure
#    first if the build tree doesn't exist yet.
if (-not (Test-Path (Join-Path $buildDir "CMakeCache.txt"))) {
    Write-Host "[build] configuring build/dev (preset 'dev')..."
    cmake --preset dev
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed" }
}
$buildPreset = if ($Config -eq "Release") { "dev-release" } else { "dev" }
cmake --build --preset $buildPreset --target `
    pyxis_hydra pyxis_hydra_omni_smoke pyxis_hydra_omni_lobby `
    pyxis_multicycle_vram_test pyxis_plugin_load_test
if ($LASTEXITCODE -ne 0) { throw "Build failed" }

$pyxisReleaseBin = Join-Path $buildDir "bin\$Config"
if (-not (Test-Path (Join-Path $pyxisReleaseBin "pyxis_hydra.dll"))) {
    throw "pyxis_hydra.dll not found in $pyxisReleaseBin after build"
}

# 2. Assemble the omni.hydra.pyxis Kit extension: the Python extension tree + the
#    prebuilt delegate + its plugInfo + the Pyxis runtime DLLs.
$extSrc = Join-Path $repoRoot "_tools\omniverse\extension"
$extDir = Join-Path $kitDir "source\extensions\omni.hydra.pyxis"
New-Item -ItemType Directory -Force -Path $extDir | Out-Null
# Stage config/ + the omni/ Python module (registers the delegate plugInfo +
# drives omni.hydra.pxr). Remove any stale copies first.
foreach ($sub in @("config", "omni", "bin")) {
    Remove-Item (Join-Path $extDir $sub) -Recurse -Force -ErrorAction SilentlyContinue
}
Copy-Item (Join-Path $extSrc "config") $extDir -Recurse -Force
if (Test-Path (Join-Path $extSrc "omni")) {
    Copy-Item (Join-Path $extSrc "omni") $extDir -Recurse -Force
}

# bin/: the delegate DLL + its plugInfo (Resources/usd/hdPyxis/resources, the
# SAME layout build/dev produces, so plugInfo's LibraryPath "../../../pyxis_hydra.dll"
# resolves to <ext>/bin/pyxis_hydra.dll) + the runtime dep DLLs + shaders.
$stageBin = Join-Path $extDir "bin"
New-Item -ItemType Directory -Force -Path $stageBin | Out-Null
# The delegate plugInfo staged by the pyxis_hydra target.
$pluginSrc = Join-Path $pyxisReleaseBin "Resources\usd\hdPyxis\resources\plugInfo.json"
$pluginDst = Join-Path $stageBin "Resources\usd\hdPyxis\resources"
New-Item -ItemType Directory -Force -Path $pluginDst | Out-Null
Copy-Item $pluginSrc $pluginDst -Force
# All Pyxis runtime DLLs (pyxis_hydra/renderer/platform/usd_ingest + transitive
# nvrhi/flecs/spdlog/...). EXCLUDE any vcpkg USD usd_*.dll / tbb12.dll — Kit
# provides nv-usd 25.11; same-named DLLs would shadow it. (build/dev is nv-usd, so
# there should be none, but exclude defensively to match the old build/omni rule.)
Get-ChildItem $pyxisReleaseBin -Filter *.dll |
    Where-Object { $_.Name -notlike 'usd_*' -and $_.Name -ne 'tbb12.dll' } |
    Copy-Item -Destination $stageBin -Force
# Path-tracer shaders next to the delegate (PyxisEngine searches next to the DLL).
$shaderSrc = Join-Path $pyxisReleaseBin "Resources\shaders"
if (Test-Path $shaderSrc) {
    $shaderDst = Join-Path $stageBin "Resources\shaders"
    New-Item -ItemType Directory -Force -Path $shaderDst | Out-Null
    Copy-Item (Join-Path $shaderSrc "*.spv") $shaderDst -Force
}
# default_sky.exr fallback for the dome light.
$sceneSrc = Join-Path $pyxisReleaseBin "Resources\scenes"
if (Test-Path $sceneSrc) {
    $sceneDst = Join-Path $stageBin "Resources\scenes"
    New-Item -ItemType Directory -Force -Path $sceneDst | Out-Null
    Copy-Item (Join-Path $sceneSrc "*") $sceneDst -Recurse -Force
}
Write-Host "[build] Assembled extension -> $extDir" -ForegroundColor Green

# repo.bat build does NOT link our prebuilt (no-premake) extension into the built
# app's exts dir, so mirror the complete extension there too.
$cfg = $Config.ToLower()
$buildExtsParent = Join-Path $kitDir "_build\windows-x86_64\$cfg\exts"
if (Test-Path $buildExtsParent) {
    $buildExt = Join-Path $buildExtsParent "omni.hydra.pyxis"
    Remove-Item $buildExt -Recurse -Force -ErrorAction SilentlyContinue
    # /R:2 /W:2 so a transient lock (e.g. a still-running kit.exe holding a staged
    # DLL) fails fast instead of robocopy's default ~1M retries.
    robocopy $extDir $buildExt /E /R:2 /W:2 /NFL /NDL /NJH /NJS /NP | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "robocopy mirror failed ($LASTEXITCODE)" }
    $global:LASTEXITCODE = 0
    Write-Host "[build] Mirrored extension -> $buildExt"
} else {
    Write-Host "[build] (build exts dir not found at $buildExtsParent; run repo.bat build once.)"
}

# The built app references its .kit via <release>/apps (a junction repo.bat
# creates). Repair it in-repo if dangling so the editor launches.
$relRoot  = Join-Path $kitDir "_build\windows-x86_64\$cfg"
$appsLink = Join-Path $relRoot "apps"
$appsTgt  = Join-Path $kitDir "source\apps"
if (Test-Path $relRoot) {
    $needFix = -not (Test-Path (Join-Path $appsLink "pyxis.editor.kit"))
    if ($needFix -and (Test-Path $appsTgt)) {
        if (Test-Path $appsLink) { cmd /c rmdir "$appsLink" 2>$null }
        cmd /c mklink /J "$appsLink" "$appsTgt" | Out-Null
        Write-Host "[build] Repaired apps junction -> $appsTgt"
    }
}

Write-Host ""
Write-Host "[build] DONE." -ForegroundColor Green
Write-Host "  delegate DLL : $pyxisReleaseBin\pyxis_hydra.dll"
Write-Host "  extension    : $extDir"
