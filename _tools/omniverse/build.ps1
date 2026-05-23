# RFC 0004 — build the Pyxis Omniverse Hydra delegate (deliverable 1).
#
# Prereq: _tools/omniverse/setup.ps1 has run (acquired nv-usd 25.11 + Python 3.12
# and wrote usd-deps/paths.ps1). This script is the reproducible build step:
# configure + compile pyxis_hydra_omni against Kit's nv-usd, then stage the DLL +
# plugInfo into the omni.hydra.pyxis Kit extension so a Kit app can load it.
#
#   pwsh _tools/omniverse/build.ps1 [-ExternalDir D:\pyxis_external] [-Config Release]

[CmdletBinding()]
param(
    [string]$ExternalDir = "D:\pyxis_external",
    [ValidateSet("Release","RelWithDebInfo")] [string]$Config = "Release"
)
$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$buildDir = Join-Path $repoRoot "build\omni"

# Load the paths setup.ps1 produced.
$pathsFile = Join-Path $ExternalDir "usd-deps\paths.ps1"
if (-not (Test-Path $pathsFile)) {
    throw "Missing $pathsFile. Run _tools/omniverse/setup.ps1 first."
}
. $pathsFile   # defines $PXR_USD_ROOT and $PYTHON_ROOT

Write-Host "[build] nv-usd : $PXR_USD_ROOT"
Write-Host "[build] python : $PYTHON_ROOT"

# nv-usd is a Release/MD build — match it. clang-cl is the project compiler.
$env:CC = "clang-cl"; $env:CXX = "clang-cl"
cmake -S (Join-Path $repoRoot "sources\pyxis_hydra_omni") -B $buildDir -G Ninja `
    "-DCMAKE_BUILD_TYPE=$Config" `
    "-DPXR_USD_ROOT=$PXR_USD_ROOT" `
    "-DPYXIS_OMNI_PYTHON_ROOT=$PYTHON_ROOT"
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed" }
cmake --build $buildDir
if ($LASTEXITCODE -ne 0) { throw "Build failed" }

# Assemble the complete omni.hydra.pyxis Kit extension: the in-repo extension
# definition (extension.toml + Python startup that registers the delegate's
# plugInfo) + the prebuilt native delegate + plugInfo. The whole extension is
# reproduced from repo sources here, so it doesn't depend on the wizard scaffold.
$extSrc = Join-Path $repoRoot "sources\pyxis_hydra_omni\extension"
$extDir = Join-Path $ExternalDir "kit-app-template\source\extensions\omni.hydra.pyxis"
New-Item -ItemType Directory -Force -Path $extDir | Out-Null
# Remove wizard-scaffold leftovers (C++ premake plugin/tests) — our extension is
# a prebuilt-native + Python registration extension, not a Kit-premake-built one.
foreach ($leftover in @("premake5.lua", "plugins", "tests.cpp")) {
    Remove-Item (Join-Path $extDir $leftover) -Recurse -Force -ErrorAction SilentlyContinue
}
Copy-Item (Join-Path $extSrc "config") $extDir -Recurse -Force
Copy-Item (Join-Path $extSrc "omni")   $extDir -Recurse -Force
$stageBin = Join-Path $extDir "bin"
$stageRes = Join-Path $stageBin "resources"
New-Item -ItemType Directory -Force -Path $stageRes | Out-Null
Copy-Item (Join-Path $buildDir "pyxis_hydra_omni.dll")        $stageBin -Force
Copy-Item (Join-Path $buildDir "resources\plugInfo.json")     $stageRes -Force

# RFC 0004 C4: the delegate links the Pyxis renderer, so stage its runtime DLLs
# (pyxis_renderer/platform + their deps: spdlog, flecs, tracy, ...) next to the
# delegate, plus the Slang-compiled shaders the path tracer loads. Without these
# the extension loads but PyxisEngine::Initialize fails to create the renderer.
$pyxisReleaseBin = Join-Path $repoRoot "build\dev\bin\Release"
if (Test-Path $pyxisReleaseBin) {
    # Stage pyxis_renderer/platform + their NON-USD deps. CRITICAL: exclude the
    # vcpkg USD 26.3 DLLs (usd_*.dll) and tbb12.dll — Kit provides nv-usd 25.11 +
    # its own tbb, and shipping vcpkg's same-named usd_*.dll would shadow Kit's
    # and break the delegate (entry-point mismatch vs the 25.11 ABI it links).
    # Scrub any stale vcpkg USD / tbb DLLs a previous (pre-fix) stage left behind
    # — they would shadow Kit's nv-usd 25.11 and break the delegate.
    Remove-Item (Join-Path $stageBin "usd_*.dll") -Force -ErrorAction SilentlyContinue
    Remove-Item (Join-Path $stageBin "tbb12.dll") -Force -ErrorAction SilentlyContinue
    Get-ChildItem $pyxisReleaseBin -Filter *.dll |
        Where-Object { $_.Name -notlike 'usd_*' -and $_.Name -ne 'tbb12.dll' } |
        Copy-Item -Destination $stageBin -Force
    $shaderSrc = Join-Path $pyxisReleaseBin "Resources\shaders"
    if (Test-Path $shaderSrc) {
        $shaderDst = Join-Path $stageBin "Resources\shaders"
        New-Item -ItemType Directory -Force -Path $shaderDst | Out-Null
        Copy-Item (Join-Path $shaderSrc "*.spv") $shaderDst -Force
    }
    Write-Host "[build] Staged Pyxis runtime DLLs + shaders -> $stageBin"
} else {
    Write-Host "[build] WARN: $pyxisReleaseBin not found; build pyxis_renderer Release first." -ForegroundColor Yellow
}
Write-Host "[build] Assembled extension -> $extDir" -ForegroundColor Green

# repo.bat build does NOT link our prebuilt (no-premake) extension into the
# built app's exts dir, so a launched app would load an empty stub. Mirror the
# complete extension there too (config-specific dir).
$cfg = $Config.ToLower()
$buildExtsParent = Join-Path $ExternalDir "kit-app-template\_build\windows-x86_64\$cfg\exts"
if (Test-Path $buildExtsParent) {
    $buildExt = Join-Path $buildExtsParent "omni.hydra.pyxis"
    Remove-Item $buildExt -Recurse -Force -ErrorAction SilentlyContinue
    robocopy $extDir $buildExt /E /NFL /NDL /NJH /NJS /NP | Out-Null
    Write-Host "[build] Mirrored extension -> $buildExt"
} else {
    Write-Host "[build] (build exts dir not found at $buildExtsParent; run repo.bat build once.)"
}

Write-Host ""
Write-Host "[build] DONE." -ForegroundColor Green
Write-Host "  delegate DLL : $buildDir\pyxis_hydra_omni.dll"
Write-Host "  plugInfo     : $buildDir\resources\plugInfo.json"
