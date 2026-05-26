# RFC 0004 C4 — run the headless HdEngine smoke for the Pyxis delegate.
#
# Drives a USD stage (mesh + material + light + camera) through the delegate via
# HdEngine (as a Kit viewport would) and verifies geometry/material/light reach
# GpuScene, the frame renders, AND it composites into the host's bound
# HdRenderBuffer — all without Kit. nv-usd resolves under build/omniverse.
# Prereqs: setup.ps1, then a Release build of the smoke target (RFC 0007: it now
# lives in build/dev/bin/Release alongside the delegate + pyxis.exe):
#   cmake --build build/dev --config Release --target pyxis_hydra_omni_smoke
#
#   pwsh _tools/omniverse/run_smoke.ps1

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
# RFC 0007 — the smoke exe now builds in build/dev next to pyxis.exe, which
# already stages Resources/shaders there, so PyxisEngine ("shaders next to the
# exe") finds them with no extra staging.
$smoke = Join-Path $releaseBin "pyxis_hydra_omni_smoke.exe"

# nv-usd 25.11 FIRST so its usd_*.dll / tbb12.dll win over any same-named DLL in
# the Pyxis Release bin (pyxis_renderer is USD-free, so it won't grab nv-usd's).
$env:PATH = "$PXR_USD_ROOT\lib;$PXR_USD_ROOT\bin;$PYTHON_ROOT;$releaseBin;" + $env:PATH
$env:PXR_PLUGINPATH_NAME = "$PXR_USD_ROOT\lib\usd;$PXR_USD_ROOT\plugin\usd"

& $smoke
exit $LASTEXITCODE
