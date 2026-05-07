<#
.SYNOPSIS
    Bootstrap a fresh Windows dev box with everything required to build Pyxis.

.DESCRIPTION
    Idempotent installer for the Pyxis toolchain (plan §49 / §50 + getting_started).
    Uses winget where possible (Windows 10 1809+ / Windows 11). Run from an
    elevated PowerShell.

    Components:
      - Git
      - Visual Studio 2022 Build Tools  (MSVC linker + Windows SDK; clang-cl
                                         needs link.exe even when the front-end
                                         is clang.)
      - LLVM toolchain (clang-cl 17+)
      - CMake 3.27+
      - Ninja
      - Python 3.11+
      - Vulkan SDK 1.3.x
      - vcpkg (cloned to %USERPROFILE%\vcpkg unless -VcpkgRoot is given)

    Re-running is safe: each step skips when its target is already present.
    Pass -DryRun to print the install plan without changing anything.

.EXAMPLE
    PS> .\_tools\required_install.ps1
    PS> .\_tools\required_install.ps1 -DryRun
    PS> .\_tools\required_install.ps1 -VcpkgRoot D:\src\vcpkg

.NOTES
    Plan references:
      - §4   third-party deps
      - §5   Windows + Vulkan SDK requirements
      - §49  CMake architecture detail (clang-cl toolchain, vcpkg manifest mode)
      - §50  getting_started.md target
#>

[CmdletBinding()]
param(
    [string]$VcpkgRoot   = (Join-Path $HOME 'vcpkg'),
    [switch]$DryRun,
    [switch]$SkipVulkan,
    [switch]$SkipVcpkg
)

$ErrorActionPreference = 'Stop'
$ProgressPreference    = 'SilentlyContinue'

function Write-Step([string]$msg) {
    Write-Host "==> $msg" -ForegroundColor Cyan
}

function Test-IsAdmin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($id)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

if (-not (Test-IsAdmin) -and -not $DryRun) {
    Write-Warning "This script installs system-wide tools and should be run from an elevated PowerShell."
    Write-Warning "Re-run from an Administrator prompt, or pass -DryRun to print the install plan."
    exit 1
}

if (-not (Get-Command winget -ErrorAction SilentlyContinue)) {
    Write-Error "winget not found. Install 'App Installer' from the Microsoft Store first (Windows 10 1809+ / Windows 11)."
    exit 1
}

# ----- winget package table -------------------------------------------------

# Each entry: id (winget) + DisplayName (logging) + ExecutableProbe (skip-if-present).
$Packages = @(
    @{ Id = 'Git.Git';                        Name = 'Git';                          Probe = 'git'    },
    @{ Id = 'Microsoft.VisualStudio.2022.BuildTools'; Name = 'Visual Studio 2022 Build Tools'; Probe = '' },
    @{ Id = 'LLVM.LLVM';                      Name = 'LLVM (clang-cl)';              Probe = 'clang-cl' },
    @{ Id = 'Kitware.CMake';                  Name = 'CMake';                        Probe = 'cmake'  },
    @{ Id = 'Ninja-build.Ninja';              Name = 'Ninja';                        Probe = 'ninja'  },
    @{ Id = 'Python.Python.3.11';             Name = 'Python 3.11';                  Probe = 'python' }
)

if (-not $SkipVulkan) {
    $Packages += @{ Id = 'KhronosGroup.VulkanSDK'; Name = 'Vulkan SDK'; Probe = '' }
}

function Install-Winget([hashtable]$pkg) {
    if ($pkg.Probe -ne '' -and (Get-Command $pkg.Probe -ErrorAction SilentlyContinue)) {
        Write-Step "$($pkg.Name): already on PATH ($($pkg.Probe)) — skipping."
        return
    }
    Write-Step "$($pkg.Name): installing via winget ($($pkg.Id))…"
    if ($DryRun) {
        Write-Host "    [dry-run] winget install --id $($pkg.Id) --silent --accept-package-agreements --accept-source-agreements"
        return
    }
    & winget install --id $pkg.Id --silent --accept-package-agreements --accept-source-agreements --disable-interactivity
    if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne -1978335189) {
        # -1978335189 = APPINSTALLER_CLI_ERROR_PACKAGE_ALREADY_INSTALLED
        throw "winget install $($pkg.Id) failed with exit code $LASTEXITCODE"
    }
}

foreach ($pkg in $Packages) {
    Install-Winget $pkg
}

# ----- VS Build Tools workload (clang-cl needs MSVC linker + Windows SDK) ---

Write-Step "Visual Studio Build Tools: ensuring C++ workload + Windows SDK…"
$vsInstaller = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vs_installer.exe"
if (Test-Path $vsInstaller) {
    if ($DryRun) {
        Write-Host '    [dry-run] vs_installer.exe modify --add Microsoft.VisualStudio.Workload.VCTools …'
    } else {
        & $vsInstaller modify `
            --installPath "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools" `
            --add Microsoft.VisualStudio.Workload.VCTools `
            --add Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
            --add Microsoft.VisualStudio.Component.Windows11SDK.22621 `
            --quiet --norestart --nocache
        if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne 3010) {
            Write-Warning "vs_installer modify exited $LASTEXITCODE — verify VCTools workload installed."
        }
    }
} else {
    Write-Warning "Visual Studio Installer not found at $vsInstaller — VCTools workload may need manual install."
}

# ----- vcpkg ----------------------------------------------------------------

if (-not $SkipVcpkg) {
    Write-Step "vcpkg: cloning / updating in $VcpkgRoot…"
    if ($DryRun) {
        Write-Host "    [dry-run] git clone https://github.com/microsoft/vcpkg.git $VcpkgRoot"
        Write-Host "    [dry-run] $VcpkgRoot\bootstrap-vcpkg.bat"
    } else {
        if (-not (Test-Path (Join-Path $VcpkgRoot '.git'))) {
            New-Item -ItemType Directory -Path $VcpkgRoot -Force | Out-Null
            git clone https://github.com/microsoft/vcpkg.git $VcpkgRoot
        } else {
            Push-Location $VcpkgRoot
            git fetch --tags
            Pop-Location
        }
        $bootstrap = Join-Path $VcpkgRoot 'bootstrap-vcpkg.bat'
        if (Test-Path $bootstrap) {
            & $bootstrap -disableMetrics
            if ($LASTEXITCODE -ne 0) {
                throw "vcpkg bootstrap failed with exit code $LASTEXITCODE"
            }
        }
        # Persist VCPKG_ROOT in user environment for downstream tooling.
        [Environment]::SetEnvironmentVariable('VCPKG_ROOT', $VcpkgRoot, 'User')
        Write-Step "VCPKG_ROOT pinned to $VcpkgRoot (user env). Re-open shells to pick it up."
    }
}

# ----- Summary --------------------------------------------------------------

Write-Host ''
Write-Step 'All done.'
Write-Host '   Next steps:'
Write-Host '     1. Re-open your shell (PATH / VCPKG_ROOT changes only take effect in new terminals).'
Write-Host '     2. cd to the repo and run:  cmake --preset dev'
Write-Host '     3. Build:                   cmake --build --preset dev'
Write-Host '     4. Run unit tests:          ctest --preset dev'
Write-Host ''
Write-Host '   Plan references: §4 (deps), §5 (Vulkan), §49 (CMake), §50 (docs).'
