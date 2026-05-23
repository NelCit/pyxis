# RFC 0004 — build the Pyxis Omniverse Hydra delegate (deliverable 1).
#
# Prereq: _tools/omniverse/setup.ps1 has run (acquired nv-usd 25.11 + Python 3.12
# under build/omniverse and wrote usd-deps/paths.ps1). Configure + compile
# pyxis_hydra_omni against Kit's nv-usd, then assemble + stage the extension into
# the Kit app's exts so a launched app loads it. Everything lives under
# <repo>/build/omniverse — no sibling external folder.
#
#   pwsh _tools/omniverse/build.ps1 [-Config Release]

[CmdletBinding()]
param([ValidateSet("Release","RelWithDebInfo")] [string]$Config = "Release")
$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$omniDir  = Join-Path $repoRoot "build\omniverse"
$buildDir = Join-Path $repoRoot "build\omni"          # delegate CMake build (in-repo)
$kitDir   = Join-Path $omniDir "kit-app-template"

# Load the paths setup.ps1 produced.
$pathsFile = Join-Path $omniDir "usd-deps\paths.ps1"
if (-not (Test-Path $pathsFile)) {
    throw "Missing $pathsFile. Run _tools/omniverse/setup.ps1 first."
}
. $pathsFile   # defines $PXR_USD_ROOT, $PYTHON_ROOT (and $PM_PACKAGES_ROOT)

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

# Assemble the complete omni.hydra.pyxis Kit extension from repo sources + the
# prebuilt native delegate + the Pyxis runtime DLLs + shaders.
$extSrc = Join-Path $repoRoot "sources\pyxis_hydra_omni\extension"
$extDir = Join-Path $kitDir "source\extensions\omni.hydra.pyxis"
New-Item -ItemType Directory -Force -Path $extDir | Out-Null
foreach ($leftover in @("premake5.lua", "plugins", "tests.cpp")) {
    Remove-Item (Join-Path $extDir $leftover) -Recurse -Force -ErrorAction SilentlyContinue
}
Copy-Item (Join-Path $extSrc "config") $extDir -Recurse -Force
Copy-Item (Join-Path $extSrc "omni")   $extDir -Recurse -Force
$stageBin = Join-Path $extDir "bin"
$stageRes = Join-Path $stageBin "resources"
New-Item -ItemType Directory -Force -Path $stageRes | Out-Null
Copy-Item (Join-Path $buildDir "pyxis_hydra_omni.dll")    $stageBin -Force
Copy-Item (Join-Path $buildDir "resources\plugInfo.json") $stageRes -Force

# Pyxis runtime DLLs + shaders. CRITICAL: exclude vcpkg's USD 26.3 usd_*.dll /
# tbb12.dll — Kit provides nv-usd 25.11; same-named DLLs would shadow it.
$pyxisReleaseBin = Join-Path $repoRoot "build\dev\bin\Release"
if (Test-Path $pyxisReleaseBin) {
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
# built app's exts dir, so mirror the complete extension there too.
$cfg = $Config.ToLower()
$buildExtsParent = Join-Path $kitDir "_build\windows-x86_64\$cfg\exts"
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
