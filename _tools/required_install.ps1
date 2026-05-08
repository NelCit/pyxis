<#
.SYNOPSIS
    Doctor + headless bootstrap for a Pyxis dev box.

.DESCRIPTION
    Default mode: read-only doctor. Probes every required tool (git, clang-cl,
    clang-tidy, CMake, Ninja, Python, Vulkan SDK, vcpkg, MSVC build tools,
    optional gh CLI) and prints a green/red checklist. Exits 0 if every
    "required" check is green; 1 otherwise.

    With -Install, performs unattended installs of every missing piece:
      - Simple winget components (git, LLVM, CMake, Ninja, Python, gh):
          winget install <id> --silent --accept-package-agreements
      - VS Build Tools 2022 with the "Desktop development with C++" workload:
          winget install Microsoft.VisualStudio.2022.BuildTools
            --override "--quiet --wait --norestart --nocache
                        --add Microsoft.VisualStudio.Workload.VCTools
                        --add Microsoft.VisualStudio.Component.VC.Tools.x86.x64
                        --add Microsoft.VisualStudio.Component.Windows11SDK.22621"
        (winget --override passes the trailing string verbatim to the
         underlying VS installer - the documented headless path.)
      - Vulkan SDK 1.3.x via the LunarG installer's documented unattended mode:
          VulkanSDK-<version>-Installer.exe --accept-licenses --default-answer
                                            --confirm-command install
      - vcpkg: clone + bootstrap-vcpkg.bat -disableMetrics; persist VCPKG_ROOT.

    With -DryRun, prints the install plan without executing anything.

    Run from elevated PowerShell when -Install is set; doctor mode runs
    unprivileged.

.EXAMPLE
    PS> .\_tools\required_install.ps1                         # doctor only
    PS> .\_tools\required_install.ps1 -Install                # install missing
    PS> .\_tools\required_install.ps1 -Install -DryRun        # show the plan

.NOTES
    Plan refs: plan-4 (deps), plan-5 (Vulkan SDK 1.3.x), plan-30.1 (clang-cl 17+),
    plan-36.2 / plan-37 (clang-tidy), plan-49 (CMake architecture).

    The Vulkan SDK version is pinned in $VULKAN_SDK_VERSION below. Bump it
    by editing the constant; the LunarG installer URL is built from it.
#>

[CmdletBinding()]
param(
    [string]$VcpkgRoot           = (Join-Path $HOME 'vcpkg'),
    [string]$VulkanSdkVersion    = '1.3.296.0',
    [switch]$Install,
    [switch]$DryRun
)

$ErrorActionPreference = 'Stop'
$ProgressPreference    = 'SilentlyContinue'

# Vulkan SDK download URL - LunarG's stable pattern.
$VULKAN_INSTALLER_URL = "https://sdk.lunarg.com/sdk/download/$VulkanSdkVersion/windows/VulkanSDK-$VulkanSdkVersion-Installer.exe"

# VS Build Tools args - Microsoft's documented unattended workload set
# (https://learn.microsoft.com/en-us/visualstudio/install/use-command-line-parameters-to-install-visual-studio).
$VS_BUILDTOOLS_ARGS = @(
    '--quiet', '--wait', '--norestart', '--nocache',
    '--add', 'Microsoft.VisualStudio.Workload.VCTools',
    '--add', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64',
    '--add', 'Microsoft.VisualStudio.Component.Windows11SDK.22621',
    '--add', 'Microsoft.VisualStudio.Component.Windows11SDK.26100'
) -join ' '

# ============================================================================
# Helpers
# ============================================================================

function Write-Heading([string]$msg) { Write-Host ""; Write-Host "== $msg ==" -ForegroundColor Cyan }
function Write-Ok     ([string]$msg) { Write-Host "  ok    $msg" -ForegroundColor Green }
function Write-Warn   ([string]$msg) { Write-Host "  warn  $msg" -ForegroundColor Yellow }
function Write-Bad    ([string]$msg) { Write-Host "  miss  $msg" -ForegroundColor Red }
function Write-Info   ([string]$msg) { Write-Host "        $msg" -ForegroundColor DarkGray }

function Test-IsAdmin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    (New-Object Security.Principal.WindowsPrincipal($id)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Test-CommandExists([string]$name) {
    $null -ne (Get-Command $name -ErrorAction SilentlyContinue)
}

function Get-CommandPath([string]$name) {
    $cmd = Get-Command $name -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source } else { return $null }
}

function Compare-Versions([string]$have, [string]$min) {
    try { return [version]$have -ge [version]$min } catch { return $false }
}

function Invoke-WingetSimple([string]$id) {
    Write-Info "winget install $id"
    if ($DryRun) { Write-Info '[dry-run] (skipped)'; return }
    & winget install --id $id --silent --accept-package-agreements --accept-source-agreements --disable-interactivity
    if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne -1978335189) {
        Write-Warn "winget exit $LASTEXITCODE (continuing)"
    }
}

function Invoke-WingetVsBuildTools {
    Write-Info "winget install Microsoft.VisualStudio.2022.BuildTools --override `"$VS_BUILDTOOLS_ARGS`""
    if ($DryRun) { Write-Info '[dry-run] (skipped)'; return }
    & winget install --id Microsoft.VisualStudio.2022.BuildTools `
        --silent --accept-package-agreements --accept-source-agreements --disable-interactivity `
        --override $VS_BUILDTOOLS_ARGS
    if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne -1978335189 -and $LASTEXITCODE -ne 3010) {
        Write-Warn "VS Build Tools install exit $LASTEXITCODE (continuing)"
    }
}

function Invoke-VulkanSdkInstall {
    $tempDir = Join-Path $env:TEMP "pyxis-vulkan-$VulkanSdkVersion"
    New-Item -ItemType Directory -Path $tempDir -Force | Out-Null
    $installer = Join-Path $tempDir "VulkanSDK-$VulkanSdkVersion-Installer.exe"

    Write-Info "downloading $VULKAN_INSTALLER_URL"
    Write-Info "          → $installer"
    if ($DryRun) {
        Write-Info '[dry-run] Invoke-WebRequest …'
        Write-Info "[dry-run] $installer --accept-licenses --default-answer --confirm-command install"
        return
    }

    if (-not (Test-Path $installer)) {
        try {
            Invoke-WebRequest -Uri $VULKAN_INSTALLER_URL -OutFile $installer -UseBasicParsing
        } catch {
            Write-Bad "Vulkan SDK download failed: $_"
            return
        }
    }

    Write-Info "running $installer in unattended mode"
    & $installer --accept-licenses --default-answer --confirm-command install
    if ($LASTEXITCODE -ne 0) {
        Write-Warn "Vulkan SDK installer exit $LASTEXITCODE (re-open shell + verify VULKAN_SDK is set)"
    }
}

# ============================================================================
# Probes
# ============================================================================

function Test-ComponentGit {
    $path = Get-CommandPath 'git'
    if ($path) {
        $version = (& git --version) -replace 'git version ', ''
        Write-Ok "git $version  ($path)"
        return @{ ok=$true; required=$true; install_kind='none' }
    }
    Write-Bad 'git - not on PATH'
    return @{ ok=$false; required=$true; install_kind='winget'; install_id='Git.Git' }
}

function Test-ComponentLlvm {
    $clangCl   = Get-CommandPath 'clang-cl'
    $clangTidy = Get-CommandPath 'clang-tidy'
    if (-not $clangCl) {
        Write-Bad 'clang-cl - not on PATH (LLVM not installed?)'
        return @{ ok=$false; required=$true; install_kind='winget'; install_id='LLVM.LLVM' }
    }
    if (-not $clangTidy) {
        Write-Bad 'clang-tidy - not on PATH (LLVM partial install?)'
        return @{ ok=$false; required=$true; install_kind='winget'; install_id='LLVM.LLVM' }
    }
    $verLine = (& clang-cl --version 2>&1 | Select-Object -First 1) -as [string]
    if ($verLine -match 'version ([\d\.]+)') {
        $ver = $Matches[1]
        if (Compare-Versions $ver '17.0') {
            Write-Ok "LLVM $ver  ($clangCl)"
            Write-Info "clang-tidy: $clangTidy"
            return @{ ok=$true; required=$true; install_kind='none' }
        }
        Write-Bad "LLVM $ver - too old (need 17+ per plan-30.1)"
        return @{ ok=$false; required=$true; install_kind='winget'; install_id='LLVM.LLVM' }
    }
    Write-Warn "LLVM version unparseable - '$verLine'"
    return @{ ok=$true; required=$true; install_kind='none' }
}

function Test-ComponentCMake {
    $path = Get-CommandPath 'cmake'
    if (-not $path) {
        Write-Bad 'cmake - not on PATH'
        return @{ ok=$false; required=$true; install_kind='winget'; install_id='Kitware.CMake' }
    }
    $verLine = (& cmake --version 2>&1 | Select-Object -First 1) -as [string]
    if ($verLine -match 'version ([\d\.]+)') {
        $ver = $Matches[1]
        if (Compare-Versions $ver '3.27') {
            Write-Ok "CMake $ver  ($path)"
            return @{ ok=$true; required=$true; install_kind='none' }
        }
        Write-Bad "CMake $ver - too old (need 3.27+ per CMakeLists.txt)"
        return @{ ok=$false; required=$true; install_kind='winget'; install_id='Kitware.CMake' }
    }
    Write-Warn 'CMake version unparseable'
    return @{ ok=$true; required=$true; install_kind='none' }
}

function Test-ComponentNinja {
    $path = Get-CommandPath 'ninja'
    if ($path) {
        $ver = (& ninja --version) -as [string]
        Write-Ok "ninja $ver  ($path)"
        return @{ ok=$true; required=$true; install_kind='none' }
    }
    Write-Bad 'ninja - not on PATH'
    return @{ ok=$false; required=$true; install_kind='winget'; install_id='Ninja-build.Ninja' }
}

function Test-ComponentPython {
    $path = Get-CommandPath 'python'
    if ($path) {
        $verLine = (& python --version 2>&1) -as [string]
        if ($verLine -match 'Python ([\d\.]+)') {
            $ver = $Matches[1]
            if (Compare-Versions $ver '3.10') {
                Write-Ok "Python $ver  ($path)"
                return @{ ok=$true; required=$true; install_kind='none' }
            }
            Write-Bad "Python $ver - too old (need 3.10+)"
            return @{ ok=$false; required=$true; install_kind='winget'; install_id='Python.Python.3.12' }
        }
    }
    Write-Bad 'python - not on PATH'
    return @{ ok=$false; required=$true; install_kind='winget'; install_id='Python.Python.3.12' }
}

function Test-ComponentVulkanSdk {
    $sdkDir = $env:VULKAN_SDK
    if (-not $sdkDir -or -not (Test-Path $sdkDir)) {
        Write-Bad "Vulkan SDK - VULKAN_SDK env var unset or path missing"
        return @{ ok=$false; required=$true; install_kind='vulkan' }
    }
    $loaderLib = Join-Path $sdkDir 'Lib\vulkan-1.lib'
    if (-not (Test-Path $loaderLib)) {
        Write-Bad "Vulkan SDK - vulkan-1.lib missing under $sdkDir\Lib"
        return @{ ok=$false; required=$true; install_kind='vulkan' }
    }
    $leaf = Split-Path -Leaf $sdkDir
    if ($leaf -match '^(\d+\.\d+\.\d+(?:\.\d+)?)$') {
        $ver = $Matches[1]
        $minor = ([version]$ver).Minor
        if ($minor -eq 3) {
            Write-Ok "Vulkan SDK $ver  ($sdkDir)"
            return @{ ok=$true; required=$true; install_kind='none' }
        } elseif ($minor -gt 3) {
            Write-Warn "Vulkan SDK $ver - newer than the plan-5-pinned 1.3.x; M0 builds OK but expect drift later"
            return @{ ok=$true; required=$true; install_kind='none' }
        } else {
            Write-Bad "Vulkan SDK $ver - too old (need 1.3.x per plan-5)"
            return @{ ok=$false; required=$true; install_kind='vulkan' }
        }
    }
    Write-Warn "Vulkan SDK at $sdkDir - version unparseable from path"
    return @{ ok=$true; required=$true; install_kind='none' }
}

function Test-ComponentMsvcToolchain {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) {
        Write-Bad 'vswhere.exe missing - VS Installer not present'
        return @{ ok=$false; required=$true; install_kind='vs-buildtools' }
    }

    $instances = & $vswhere -latest -products '*' `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -format json | ConvertFrom-Json
    if (-not $instances) {
        Write-Bad 'No VS install with the Desktop C++ workload found'
        return @{ ok=$false; required=$true; install_kind='vs-buildtools' }
    }
    $vs = $instances[0]
    Write-Ok "MSVC toolchain - $($vs.displayName) $($vs.installationVersion)"
    Write-Info "installPath: $($vs.installationPath)"
    $hasSdk = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.Windows11SDK.22621 -property installationVersion 2>$null
    if (-not $hasSdk) {
        $hasSdk = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.Windows11SDK.26100 -property installationVersion 2>$null
    }
    if ($hasSdk) {
        Write-Info 'Windows 11 SDK component present.'
        return @{ ok=$true; required=$true; install_kind='none' }
    } else {
        Write-Warn 'No Windows 11 SDK component detected.'
        return @{ ok=$false; required=$true; install_kind='vs-buildtools' }
    }
}

function Test-ComponentVcpkg {
    $root = $env:VCPKG_ROOT
    if (-not $root) { $root = $VcpkgRoot }
    if (-not (Test-Path (Join-Path $root '.git'))) {
        Write-Bad "vcpkg - not cloned at $root"
        return @{ ok=$false; required=$true; install_kind='vcpkg' }
    }
    $vcpkgExe = Join-Path $root 'vcpkg.exe'
    if (-not (Test-Path $vcpkgExe)) {
        Write-Bad "vcpkg - cloned but not bootstrapped (no vcpkg.exe at $vcpkgExe)"
        return @{ ok=$false; required=$true; install_kind='vcpkg' }
    }
    $ver = (& $vcpkgExe version 2>&1 | Select-Object -First 1) -as [string]
    Write-Ok "vcpkg - $ver"
    Write-Info "VCPKG_ROOT: $root"
    if (-not $env:VCPKG_ROOT) {
        Write-Warn 'VCPKG_ROOT not set in this shell. Persist with:  setx VCPKG_ROOT "<root>"'
    }
    return @{ ok=$true; required=$true; install_kind='none' }
}

function Test-ComponentGh {
    $path = Get-CommandPath 'gh'
    if ($path) {
        $verLine = (& gh --version 2>&1 | Select-Object -First 1) -as [string]
        Write-Ok "gh CLI - $verLine  ($path)"
        return @{ ok=$true; required=$false; install_kind='none' }
    }
    Write-Warn 'gh CLI - not on PATH (optional; needed for the PR-per-milestone workflow)'
    return @{ ok=$false; required=$false; install_kind='winget'; install_id='GitHub.cli' }
}

# ============================================================================
# Doctor pass
# ============================================================================

Write-Heading "Pyxis dev-box doctor (Install=$Install, DryRun=$DryRun)"
Write-Info "$env:USERNAME on $env:COMPUTERNAME - admin: $(Test-IsAdmin)"
Write-Info "Vulkan SDK target: $VulkanSdkVersion (override with -VulkanSdkVersion)"

$results = [ordered]@{
    'git'         = (Test-ComponentGit)
    'LLVM'        = (Test-ComponentLlvm)
    'CMake'       = (Test-ComponentCMake)
    'Ninja'       = (Test-ComponentNinja)
    'Python'      = (Test-ComponentPython)
    'Vulkan SDK'  = (Test-ComponentVulkanSdk)
    'MSVC + SDK'  = (Test-ComponentMsvcToolchain)
    'vcpkg'       = (Test-ComponentVcpkg)
    'gh CLI'      = (Test-ComponentGh)
}

# ============================================================================
# Install pass
# ============================================================================

if ($Install) {
    Write-Heading 'Install pass'
    if (-not (Test-IsAdmin) -and -not $DryRun) {
        Write-Warn 'Not running as Administrator. winget + VS installs will likely fail.'
        Write-Warn 'Re-run from an elevated PowerShell.'
    }
    if (-not (Test-CommandExists winget)) {
        Write-Bad 'winget not found. Install "App Installer" from the Microsoft Store first.'
        exit 1
    }

    foreach ($entry in $results.GetEnumerator()) {
        $r = $entry.Value
        if ($r.ok) { continue }
        switch ($r.install_kind) {
            'winget' {
                Write-Heading "Install $($entry.Name)"
                Invoke-WingetSimple $r.install_id
            }
            'vs-buildtools' {
                Write-Heading "Install $($entry.Name) - VS Build Tools 2022 (unattended)"
                Invoke-WingetVsBuildTools
            }
            'vulkan' {
                Write-Heading "Install $($entry.Name) - LunarG installer (unattended)"
                Invoke-VulkanSdkInstall
            }
            'vcpkg' {
                Write-Heading 'Install vcpkg'
                if (-not (Test-Path (Join-Path $VcpkgRoot '.git'))) {
                    if ($DryRun) {
                        Write-Info "[dry-run] git clone https://github.com/microsoft/vcpkg.git $VcpkgRoot"
                    } else {
                        New-Item -ItemType Directory -Path $VcpkgRoot -Force | Out-Null
                        git clone https://github.com/microsoft/vcpkg.git $VcpkgRoot
                        if ($LASTEXITCODE -ne 0) { Write-Bad 'git clone vcpkg failed'; continue }
                    }
                }
                $bootstrap = Join-Path $VcpkgRoot 'bootstrap-vcpkg.bat'
                if (Test-Path $bootstrap) {
                    if ($DryRun) {
                        Write-Info "[dry-run] $bootstrap -disableMetrics"
                    } else {
                        & $bootstrap -disableMetrics
                    }
                }
                if (-not $DryRun) {
                    [Environment]::SetEnvironmentVariable('VCPKG_ROOT', $VcpkgRoot, 'User')
                    Write-Info "Persisted VCPKG_ROOT = $VcpkgRoot (user env). Re-open shells to pick it up."
                }
            }
            default {
                Write-Warn "Unknown install kind '$($r.install_kind)' for $($entry.Name)"
            }
        }
    }

    Write-Heading 'Install pass - done'
    Write-Info 'Re-run this script (no -Install) to verify everything is now green.'
}

# ============================================================================
# Summary
# ============================================================================

Write-Heading 'Summary'
$allOk = $true
foreach ($entry in $results.GetEnumerator()) {
    $r = $entry.Value
    $tag = if ($r.ok) { 'ok  ' } elseif ($r.required) { 'MISS' } else { 'opt ' }
    Write-Host ("  [{0}] {1}" -f $tag, $entry.Name)
    if (-not $r.ok -and $r.required) { $allOk = $false }
}

Write-Host ''
if ($allOk) {
    Write-Host 'All required components present. Pyxis builds with `cmake --preset dev`.' -ForegroundColor Green
    exit 0
} else {
    if (-not $Install) {
        Write-Host 'Required components missing. Re-run with -Install (elevated) to bootstrap.' -ForegroundColor Red
    } else {
        Write-Host 'Required components still missing after the install pass. Open a new shell and re-run the doctor.' -ForegroundColor Red
    }
    exit 1
}
