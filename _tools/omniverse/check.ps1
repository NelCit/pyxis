# RFC 0004 -- environment prerequisite checker for the Pyxis Omniverse delegate.
#
# Verifies everything needed to build + run the Kit Hydra delegate, grouped by
# stage. Reports OK / MISSING per item with a hint, and exits non-zero if any
# REQUIRED item is missing. Run any time:
#
#   pwsh _tools/omniverse/check.ps1
#
# Categories: [host] tools to build, [sdk] Packman-pulled deps (setup.ps1),
# [pyxis] the prebuilt USD-free renderer, [run] runtime bits.

$ErrorActionPreference = "Continue"
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$omniDir  = Join-Path $repoRoot "build\omniverse"

$rows = New-Object System.Collections.Generic.List[object]
$missingRequired = 0

function Check([string]$cat, [string]$name, [bool]$ok, [string]$detail, [bool]$required = $true) {
    $rows.Add([pscustomobject]@{ Cat = $cat; Item = $name; Status = $(if ($ok) { "OK" } else { if ($required) { "MISSING" } else { "absent" } }); Detail = $detail })
    if (-not $ok -and $required) { $script:missingRequired++ }
}

function Have([string]$cmd) { return [bool](Get-Command $cmd -ErrorAction SilentlyContinue) }
function Tool([string]$cmd, [string]$verArg = "--version") {
    if (Have $cmd) { try { return (& $cmd $verArg 2>&1 | Select-Object -First 1) } catch { return "present" } }
    return $null
}

# ---- [host] build tools ---------------------------------------------------
$clang = Get-Command clang-cl -ErrorAction SilentlyContinue
Check "host" "clang-cl (LLVM)" ([bool]$clang) $(if ($clang) { (& clang-cl --version 2>&1 | Select-Object -First 1) } else { "install LLVM; needed to compile the delegate" })
$cmake = Tool "cmake"
Check "host" "cmake (>=3.24)" ([bool]$cmake) $(if ($cmake) { "$cmake" } else { "install CMake" })
$ninja = Tool "ninja"
Check "host" "ninja" ([bool]$ninja) $(if ($ninja) { "$ninja" } else { "install Ninja" })
$git = Tool "git"
Check "host" "git" ([bool]$git) $(if ($git) { "$git" } else { "install Git (setup.ps1 clones the Kit SDK)" })
$vcpkg = $env:VCPKG_ROOT
Check "host" "VCPKG_ROOT" ([bool]($vcpkg -and (Test-Path $vcpkg))) $(if ($vcpkg) { $vcpkg } else { "set VCPKG_ROOT (main Pyxis build)" })

# ---- GPU + Vulkan + external-memory interop -------------------------------
$gpu = $null
if (Have "nvidia-smi") { $gpu = (& nvidia-smi --query-gpu=name,driver_version --format=csv,noheader 2>&1 | Select-Object -First 1) }
Check "host" "NVIDIA GPU + driver" ([bool]$gpu) $(if ($gpu) { "$gpu" } else { "RTX GPU + driver required (RT pipeline + external memory)" })
$vk = $null; $extMem = $false
if (Have "vulkaninfo") {
    $vkOut = & vulkaninfo --summary 2>&1
    $vk = ($vkOut | Select-String "Vulkan Instance Version" | Select-Object -First 1)
    $extMem = [bool]($vkOut | Select-String "external_memory_capabilities")
}
Check "host" "Vulkan runtime" ([bool]$vk) $(if ($vk) { ("$vk" -replace '\s+', ' ') } else { "install the Vulkan SDK/runtime" })
Check "host" "VK external_memory_capabilities" $extMem $(if ($extMem) { "advertised (GPU interop OK)" } else { "needed for the zero-copy handoff" })

# ---- [sdk] Packman-pulled deps (setup.ps1) --------------------------------
$pathsFile = Join-Path $omniDir "usd-deps\paths.ps1"
$PXR_USD_ROOT = $null; $PYTHON_ROOT = $null
if (Test-Path $pathsFile) { . $pathsFile }
if (-not $PXR_USD_ROOT) { $PXR_USD_ROOT = Join-Path $omniDir "usd-deps\usd" }

$kit = Join-Path $omniDir "kit-app-template\_build\windows-x86_64\release\kit\kit.exe"
Check "sdk" "Kit SDK (kit.exe)" (Test-Path $kit) $(if (Test-Path $kit) { "Kit 110 present" } else { "run setup.ps1 (clones + bootstraps Kit)" })
$pxrH = Join-Path $PXR_USD_ROOT "include\pxr\pxr.h"
Check "sdk" "nv-usd 25.11 (pxr.h)" (Test-Path $pxrH) $(if (Test-Path $pxrH) { $PXR_USD_ROOT } else { "run setup.ps1 (packman-pulls nv-usd)" })
$usdHd = Join-Path $PXR_USD_ROOT "lib\usd_hd.lib"
Check "sdk" "nv-usd import libs (usd_hd.lib)" (Test-Path $usdHd) $(if (Test-Path $usdHd) { "present" } else { "nv-usd pull incomplete" })
$pyOk = $PYTHON_ROOT -and (Test-Path (Join-Path $PYTHON_ROOT "include\pyconfig.h")) -and (Test-Path (Join-Path $PYTHON_ROOT "libs\python312.lib"))
Check "sdk" "Python 3.12 (pyconfig.h + python312.lib)" ([bool]$pyOk) $(if ($pyOk) { $PYTHON_ROOT } else { "nv-usd is a py312 build; run setup.ps1" })

# ---- [pyxis] prebuilt USD-free renderer -----------------------------------
$rndLib = Join-Path $repoRoot "build\dev\lib\Release\pyxis_renderer.lib"
Check "pyxis" "pyxis_renderer.lib (Release)" (Test-Path $rndLib) $(if (Test-Path $rndLib) { "present" } else { "cmake --build build/dev --config Release --target pyxis_renderer pyxis_platform" })
$rndDll = Join-Path $repoRoot "build\dev\bin\Release\pyxis_renderer.dll"
Check "run" "pyxis_renderer.dll (Release runtime)" (Test-Path $rndDll) $(if (Test-Path $rndDll) { "present" } else { "build pyxis_renderer Release" })
$spv = Join-Path $repoRoot "build\dev\bin\Release\Resources\shaders\raygen.spv"
if (-not (Test-Path $spv)) { $spv = Join-Path $repoRoot "build\dev\bin\Debug\Resources\shaders\raygen.spv" }
Check "run" "path-tracer shaders (.spv)" (Test-Path $spv) $(if (Test-Path $spv) { "present" } else { "build pyxis_renderer_shaders (ShaderMake)" }) $false

# ---- [run] built delegate + extension (RFC 0007: single build/dev delegate) -
$dll = Join-Path $repoRoot "build\dev\bin\Release\pyxis_hydra.dll"
Check "run" "pyxis_hydra.dll (delegate)" (Test-Path $dll) $(if (Test-Path $dll) { "built" } else { "pwsh _tools/omniverse/build.ps1" }) $false

# ---- report ---------------------------------------------------------------
$rows | Format-Table -AutoSize Cat, Item, Status, Detail | Out-Host
if ($missingRequired -gt 0) {
    Write-Host "[check] $missingRequired required item(s) MISSING -- see hints above." -ForegroundColor Yellow
    Write-Host "[check] Typical fix order: setup.ps1 (SDK) -> build pyxis_renderer Release -> build.ps1" -ForegroundColor Yellow
    exit 1
}
Write-Host "[check] All required prerequisites present. Build with _tools/omniverse/build.ps1." -ForegroundColor Green
exit 0
