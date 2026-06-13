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

# nv-usd 25.11 on PATH as the FALLBACK import source (only used when the staged
# copies next to the exe are absent — the app dir always wins the DLL search).
$env:PATH = "$PXR_USD_ROOT\lib;$PXR_USD_ROOT\bin;$PYTHON_ROOT;$releaseBin;" + $env:PATH
# PXR_PLUGINPATH_NAME must NOT include "$PXR_USD_ROOT\lib\usd": the exe loads the
# usd_*.dll copies STAGED NEXT TO IT (pyxis_app POST_BUILD; app dir beats PATH), and
# USD auto-discovers the matching <exe-dir>\usd core plugin tree relative to those
# loaded modules. Listing usd-deps\lib\usd here registers a SECOND core tree whose
# records win Plug's name dedup, so the first lazy PlugPlugin::Load (e.g. plugin
# 'usdImaging' inside UsdImagingCreateSceneIndices) LoadLibrary's a SECOND COPY of an
# already-loaded usd DLL by a different path — its static initializers self-deadlock
# USD's registry and busy-spin (SwitchToThread) forever. pyxis.exe is immune because
# Application.cpp::EnsureUsdPluginPath overwrites this var with <exe-dir>\usd; the
# hydra test exes inherit it, so only second-tier plugins (plugin\usd: usdAbc,
# hioOiio, ... — unique DLLs, never duplicated by the staging) may be listed.
$env:PXR_PLUGINPATH_NAME = "$PXR_USD_ROOT\plugin\usd"

& $smoke
exit $LASTEXITCODE
