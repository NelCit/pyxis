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
# Export PM_PACKAGES_ROOT so CMake can glob the Packman boost-preprocessor the
# viewport panel needs (omni.ui's Callback.h pulls boost/preprocessor.hpp).
if ($PM_PACKAGES_ROOT) { $env:PM_PACKAGES_ROOT = $PM_PACKAGES_ROOT }

# The viewport panel links omni.ui.lib, which ships inside the omni.ui EXTENSION's
# bin (registry-pulled, not the base install). Locate it under the editor's
# extscache; if absent, the editor's exts haven't been resolved yet.
$extsCache = Join-Path $kitDir "_build\windows-x86_64\$($Config.ToLower())\extscache"
$omniUiLib = Get-ChildItem -Path (Join-Path $extsCache "omni.ui-*\bin\omni.ui.lib") -ErrorAction SilentlyContinue |
             Select-Object -First 1
if (-not $omniUiLib) {
    Write-Host "[build] WARN: omni.ui.lib not found under $extsCache." -ForegroundColor Yellow
    Write-Host "[build]       Run 'repo.bat build' (or launch pyxis.editor once) to resolve the" -ForegroundColor Yellow
    Write-Host "[build]       editor's extensions so omni.ui is pulled, then re-run build.ps1." -ForegroundColor Yellow
} else {
    Write-Host "[build] omni.ui : $($omniUiLib.FullName)"
}

cmake -S (Join-Path $repoRoot "sources\pyxis_hydra_omni") -B $buildDir -G Ninja `
    "-DCMAKE_BUILD_TYPE=$Config" `
    "-DPXR_USD_ROOT=$PXR_USD_ROOT" `
    "-DPYXIS_OMNI_PYTHON_ROOT=$PYTHON_ROOT" `
    $(if ($omniUiLib) { "-DPYXIS_OMNI_UI_LIB=$($omniUiLib.FullName -replace '\\','/')" })
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
# Stage config/ + the small omni/ Python startup module (RFC 0006: forces the pxr
# engine to load the Pyxis delegate at launch instead of defaulting to Storm).
Copy-Item (Join-Path $extSrc "config") $extDir -Recurse -Force
Remove-Item (Join-Path $extDir "omni") -Recurse -Force -ErrorAction SilentlyContinue
if (Test-Path (Join-Path $extSrc "omni")) {
    Copy-Item (Join-Path $extSrc "omni") $extDir -Recurse -Force
}
$stageBin = Join-Path $extDir "bin"
$stageRes = Join-Path $stageBin "resources"
New-Item -ItemType Directory -Force -Path $stageRes | Out-Null
Copy-Item (Join-Path $buildDir "pyxis_hydra_omni.dll")            $stageBin -Force
Copy-Item (Join-Path $buildDir "omni.hydra.pyxis.plugin.dll")     $stageBin -Force
Copy-Item (Join-Path $buildDir "resources\plugInfo.json")         $stageRes -Force
# NOTE: the omni.ui viewport panel (omni.hydra.pyxis.panel.plugin.dll) is built
# but intentionally NOT staged. Pyxis now integrates as a real viewport renderer
# via omni.hydra.pxr (selectable in the Render menu), which supersedes the custom
# panel. Remove a stale copy so the [[native.plugin]] glob doesn't load it.
Remove-Item (Join-Path $stageBin "omni.hydra.pyxis.panel.plugin.dll") -Force -ErrorAction SilentlyContinue

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
    # /R:2 /W:2 so a transient lock (e.g. a still-running kit.exe holding a
    # staged DLL) fails fast instead of robocopy's default ~1M retries (which
    # hangs the build indefinitely).
    robocopy $extDir $buildExt /E /R:2 /W:2 /NFL /NDL /NJH /NJS /NP | Out-Null
    # robocopy uses exit codes 0-7 for success (>=8 is failure). Don't let its
    # "files copied" code (1) leak as this script's failure status.
    if ($LASTEXITCODE -ge 8) { throw "robocopy mirror failed ($LASTEXITCODE)" }
    $global:LASTEXITCODE = 0
    Write-Host "[build] Mirrored extension -> $buildExt"
} else {
    Write-Host "[build] (build exts dir not found at $buildExtsParent; run repo.bat build once.)"
}

# The built app references its .kit via <release>/apps (a link to source/apps that
# repo.bat creates). A pre-consolidation build may have left it dangling (pointing
# at the old sibling pyxis_external). Repair it in-repo so the editor launches.
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
Write-Host "  delegate DLL : $buildDir\pyxis_hydra_omni.dll"
