<#
.SYNOPSIS
    One-command initialize for a Pyxis dev box: prerequisites -> nv-usd -> configure -> full build.

.DESCRIPTION
    Drives a clean checkout to a runnable pyxis.exe with no manual steps:

      1. prereqs   - run the dev-box doctor; if anything is missing, bootstrap it
                     (_tools/required_install.ps1 -Install self-elevates for the
                     Vulkan SDK install, then clones+bootstraps vcpkg). Wires the
                     VS-bundled CMake/Ninja onto PATH.
      2. usd       - acquire nv-usd 25.11 + Python 3.12 via Packman
                     (_tools/omniverse/setup.ps1) into build/omniverse (multi-GB,
                     one-time). Required because pyxis_material_translation links
                     nv-usd unconditionally (RFC 0006).
      3. scenes    - fetch the optional regression scenes into resources/scenes/:
                     the OpenPBR Shader Playground (ASWF DPEL GitHub, ~1.9 GB,
                     automatic) and the World Lobby (no stable public URL; used
                     from -WorldLobbyUrl / PYXIS_WORLD_LOBBY_URL or an existing
                     Collected_World_Lobby.zip in Downloads). Missing scenes only
                     de-register their fixtures - the build is unaffected.
      4. configure - enter the VS Developer environment (so clang-cl finds the
                     MSVC CRT + Windows SDK), then `cmake --preset dev`. Builds the
                     vcpkg deps and FetchContent's NVRHI/Slang/ImGui. clang-tidy is
                     OFF by default for a fast first build (-WithClangTidy re-enables
                     the §37 CI linter gate).
      5. build     - `cmake --build` the chosen config (Release by default).
      6. verify    - run the built pyxis.exe (--version) as a smoke check.

    Run any single stage with -Stage; the default runs them all in order.

.EXAMPLE
    PS> pwsh _tools/init.ps1                       # full init: prereqs -> build (Release)
    PS> pwsh _tools/init.ps1 -Stage configure      # just (re)configure
    PS> pwsh _tools/init.ps1 -Config Debug          # build Debug instead of Release
    PS> pwsh _tools/init.ps1 -WithClangTidy         # build with the clang-tidy gate on

.NOTES
    Plan refs: §3/§49 (CMake), §4/§5 (deps + Vulkan), RFC 0006 (nv-usd via Packman).
    A UAC prompt appears once, during the prereqs stage, if the Vulkan SDK or vcpkg
    are missing. Approve it to let the install proceed.
#>
[CmdletBinding()]
param(
    [ValidateSet('all', 'prereqs', 'usd', 'scenes', 'configure', 'build', 'verify')]
    [string]$Stage = 'all',
    [ValidateSet('Release', 'Debug', 'RelWithDebInfo')]
    [string]$Config = 'Release',
    [switch]$WithClangTidy,
    # World Lobby source override: NVIDIA distributes Collected_World_Lobby.zip
    # through the Omniverse docs/Launcher with no stable direct URL, so when a
    # team mirror exists pass it here (or set PYXIS_WORLD_LOBBY_URL).
    [string]$WorldLobbyUrl = $env:PYXIS_WORLD_LOBBY_URL
)

$ErrorActionPreference = 'Stop'
$repoRoot   = Split-Path -Parent $PSScriptRoot       # _tools -> repo root
$toolsDir   = $PSScriptRoot
$buildDir   = Join-Path $repoRoot 'build\dev'
Set-Location $repoRoot

function Section([string]$m) { Write-Host ''; Write-Host "==== $m ====" -ForegroundColor Cyan }
function Info([string]$m)    { Write-Host "     $m" -ForegroundColor DarkGray }

# Pull VCPKG_ROOT / VULKAN_SDK from the persisted (User/Machine) environment into
# this process when they aren't already here -- the install stage persists them,
# but the current process started before that and won't see them otherwise.
function Import-PersistedEnv {
    # VULKAN_SDK: keep the process value if present, else pull the persisted one
    # (the LunarG installer sets it at Machine scope).
    if (-not $env:VULKAN_SDK) {
        $v = [Environment]::GetEnvironmentVariable('VULKAN_SDK', 'Machine')
        if (-not $v) { $v = [Environment]::GetEnvironmentVariable('VULKAN_SDK', 'User') }
        if ($v) { $env:VULKAN_SDK = $v }
    }
    # VCPKG_ROOT: PREFER the standalone clone we bootstrapped. Enter-VsDevShell
    # sets VCPKG_ROOT to VS's *bundled* vcpkg, which has no .git and therefore
    # can't resolve our pinned builtin-baseline (it falls back to registry HEAD,
    # pulling the wrong dependency versions). A full git clone resolves the exact
    # baseline, so override whatever the dev shell set when one is present.
    $standalone = [Environment]::GetEnvironmentVariable('VCPKG_ROOT', 'User')
    if (-not $standalone) { $standalone = Join-Path $HOME 'vcpkg' }
    if (Test-Path (Join-Path $standalone '.git')) {
        $env:VCPKG_ROOT = $standalone
    } elseif (-not $env:VCPKG_ROOT -and (Test-Path (Join-Path $standalone 'vcpkg.exe'))) {
        $env:VCPKG_ROOT = $standalone
    }
}

# clang-cl needs the MSVC toolchain + Windows SDK on INCLUDE/LIB/PATH. Enter the
# VS Developer environment once per process so `cmake --preset dev` (clang-cl +
# Ninja) resolves <vector>, the CRT, and the Windows SDK.
function Enter-PyxisDevShell {
    if ($env:__PYXIS_DEVSHELL -eq '1') { return }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) { throw 'vswhere.exe not found - is Visual Studio installed?' }
    $vsPath = & $vswhere -latest -products '*' `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath | Select-Object -First 1
    if (-not $vsPath) { throw 'No VS install with the Desktop C++ workload found.' }
    $devShell = Join-Path $vsPath 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll'
    Import-Module $devShell
    Enter-VsDevShell -VsInstallPath $vsPath -SkipAutomaticLocation `
        -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
    Set-Location $repoRoot     # DevShell changes the location; put it back.
    $env:__PYXIS_DEVSHELL = '1'
    Info "VS dev environment: $vsPath"
}

# Run a sibling .ps1 in a separate process so its `exit` can't terminate us, and
# surface its exit code. Output stays inline (-NoNewWindow).
function Invoke-Script([string]$path, [string[]]$scriptArgs) {
    $a = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $path) + $scriptArgs
    $p = Start-Process -FilePath 'powershell.exe' -ArgumentList $a -Wait -PassThru -NoNewWindow
    return $p.ExitCode
}

function Invoke-Prereqs {
    Section 'Stage 1/6 - prerequisites (dev-box doctor + bootstrap)'
    $doctor = Join-Path $toolsDir 'required_install.ps1'
    $code = Invoke-Script $doctor @()
    if ($code -eq 0) {
        Info 'All prerequisites already present.'
        Import-PersistedEnv
        return
    }
    Info 'Missing prerequisites - bootstrapping (Vulkan SDK + vcpkg). Approve the UAC prompt.'
    $null = Invoke-Script $doctor @('-Install')
    Import-PersistedEnv
    $code = Invoke-Script $doctor @()
    if ($code -ne 0) {
        throw 'Prerequisites still missing after install. Inspect the install transcript (printed above).'
    }
}

function Invoke-UsdSetup {
    Section 'Stage 2/6 - nv-usd 25.11 + Python 3.12 (Packman, multi-GB, one-time)'
    $pxrHeader = Join-Path $repoRoot 'build\omniverse\usd-deps\usd\include\pxr\pxr.h'
    if (Test-Path $pxrHeader) {
        Info 'nv-usd already present under build/omniverse - skipping.'
        return
    }
    $code = Invoke-Script (Join-Path $toolsDir 'omniverse\setup.ps1') @()
    if ($code -ne 0) { throw "nv-usd Packman setup failed (exit $code)." }
    if (-not (Test-Path $pxrHeader)) { throw "nv-usd setup ran but $pxrHeader is missing." }
}

# Download $url to $dst with curl.exe (ships with Windows 10+; far faster than
# Invoke-WebRequest on multi-GB files and follows GitHub's codeload redirects).
function Get-RemoteFile([string]$url, [string]$dst) {
    & curl.exe -L --retry 3 --fail --silent --show-error -o $dst $url
    return ($LASTEXITCODE -eq 0 -and (Test-Path $dst) -and (Get-Item $dst).Length -gt 1MB)
}

# Extract $zip into $dest with tar.exe (bsdtar — ships with Windows 10+, much
# faster than Expand-Archive on multi-GB archives).
function Expand-ZipFast([string]$zip, [string]$dest) {
    New-Item -ItemType Directory -Force -Path $dest | Out-Null
    & tar.exe -xf $zip -C $dest
    return ($LASTEXITCODE -eq 0)
}

# If $entryRel is not at $root but exactly one top-level folder holds it (zip
# wrapped everything in e.g. OpenPBRShaderPlayground-1.0/), hoist that folder's
# contents up into $root.
function Move-WrapperDirUp([string]$root, [string]$entryRel) {
    if (Test-Path (Join-Path $root $entryRel)) { return }
    $tops = @(Get-ChildItem $root -Directory -ErrorAction SilentlyContinue)
    foreach ($t in $tops) {
        if (Test-Path (Join-Path $t.FullName $entryRel)) {
            Get-ChildItem $t.FullName -Force | Move-Item -Destination $root -Force
            Remove-Item $t.FullName -Recurse -Force -ErrorAction SilentlyContinue
            return
        }
    }
}

function Invoke-Scenes {
    Section 'Stage 3/6 - optional regression scenes (OpenPBR playground + World Lobby)'
    $scenesRoot = Join-Path $repoRoot 'resources\scenes'
    $dlDir      = Join-Path $env:TEMP 'pyxis-scenes'
    New-Item -ItemType Directory -Force -Path $scenesRoot, $dlDir | Out-Null

    # --- OpenPBR Shader Playground (ASWF DPEL, ~1.9 GB) -----------------------
    # Stable source: the v1.0 tag archive of the DPEL GitHub repo (no Git LFS,
    # so the archive carries the full content). Unpacks to
    # OpenPBRShaderPlayground-1.0/ShdrPlygrnd/... — hoisted one level so the
    # entry-point lands where tests/integration/CMakeLists.txt expects it.
    $pbrRoot  = Join-Path $scenesRoot 'openpbr_playground'
    $pbrEntry = 'ShdrPlygrnd\ShdrPlygrnd_OpenPBR.usda'
    if (Test-Path (Join-Path $pbrRoot $pbrEntry)) {
        Info 'OpenPBR playground already present - skipping.'
    } else {
        $pbrUrl = 'https://github.com/DigitalProductionExampleLibrary/OpenPBRShaderPlayground/archive/refs/tags/v1.0.zip'
        $pbrZip = Join-Path $dlDir 'OpenPBRShaderPlayground-1.0.zip'
        if (-not (Test-Path $pbrZip) -or (Get-Item $pbrZip).Length -lt 1GB) {
            Info "downloading OpenPBR Shader Playground (~1.9 GB) from $pbrUrl"
            if (-not (Get-RemoteFile $pbrUrl $pbrZip)) {
                Write-Host 'OpenPBR playground download failed - fixture stays disabled.' -ForegroundColor Yellow
                $pbrZip = $null
            }
        } else { Info "using cached $pbrZip" }
        if ($pbrZip) {
            Info "extracting -> $pbrRoot"
            if (Expand-ZipFast $pbrZip $pbrRoot) {
                Move-WrapperDirUp $pbrRoot $pbrEntry
                if (Test-Path (Join-Path $pbrRoot $pbrEntry)) {
                    Info 'OpenPBR playground ready.'
                } else {
                    Write-Host "extracted, but $pbrEntry not found under $pbrRoot - layout changed upstream?" -ForegroundColor Yellow
                }
            } else {
                Write-Host 'OpenPBR playground extraction failed.' -ForegroundColor Yellow
            }
        }
    }

    # --- World Lobby (NVIDIA Omniverse sample, ~3.4 GB zip) -------------------
    # NVIDIA ships Collected_World_Lobby.zip via the Omniverse docs/Launcher with
    # NO stable direct-download URL, so this step is automatic only when a source
    # is reachable: -WorldLobbyUrl / PYXIS_WORLD_LOBBY_URL, or a zip already in
    # Downloads / the repo root / %TEMP%\pyxis-scenes. Otherwise it prints how to
    # fetch it and the fixture simply stays de-registered (the tests are
    # conditionally added, so nothing breaks).
    $lobbyRoot  = Join-Path $scenesRoot 'world_lobby'
    $lobbyEntry = 'World_Lobby.usd'
    if (Test-Path (Join-Path $lobbyRoot $lobbyEntry)) {
        Info 'World Lobby already present - skipping.'
        return
    }
    $lobbyZip = $null
    foreach ($cand in @((Join-Path $dlDir 'Collected_World_Lobby.zip'),
                        (Join-Path $HOME 'Downloads\Collected_World_Lobby.zip'),
                        (Join-Path $repoRoot 'Collected_World_Lobby.zip'))) {
        if ((Test-Path $cand) -and (Get-Item $cand).Length -gt 100MB) { $lobbyZip = $cand; break }
    }
    if (-not $lobbyZip -and $WorldLobbyUrl) {
        $dst = Join-Path $dlDir 'Collected_World_Lobby.zip'
        Info "downloading World Lobby from $WorldLobbyUrl"
        if (Get-RemoteFile $WorldLobbyUrl $dst) { $lobbyZip = $dst }
        else { Write-Host 'World Lobby download failed.' -ForegroundColor Yellow }
    }
    if ($lobbyZip) {
        Info "extracting $lobbyZip -> $lobbyRoot"
        if (Expand-ZipFast $lobbyZip $lobbyRoot) {
            Move-WrapperDirUp $lobbyRoot $lobbyEntry
            if (Test-Path (Join-Path $lobbyRoot $lobbyEntry)) { Info 'World Lobby ready.' }
            else { Write-Host "extracted, but $lobbyEntry not found under $lobbyRoot." -ForegroundColor Yellow }
        } else {
            Write-Host 'World Lobby extraction failed.' -ForegroundColor Yellow
        }
    } else {
        Write-Host 'World Lobby: no source available - fixture stays disabled (build is unaffected).' -ForegroundColor Yellow
        Info 'To enable: download Collected_World_Lobby.zip (Omniverse Residential Lobby,'
        Info 'https://docs.omniverse.nvidia.com/usd/latest/usd_content_samples/res_lobby.html)'
        Info 'into Downloads\, or pass -WorldLobbyUrl / set PYXIS_WORLD_LOBBY_URL, then re-run'
        Info 'pwsh _tools/init.ps1 -Stage scenes && -Stage configure.'
    }
}

function Invoke-Configure {
    Section 'Stage 4/6 - configure (cmake --preset dev)'
    Enter-PyxisDevShell
    Import-PersistedEnv
    if (-not $env:VCPKG_ROOT) { throw 'VCPKG_ROOT is not set - run the prereqs stage first.' }
    if (-not $env:VULKAN_SDK) { throw 'VULKAN_SDK is not set - run the prereqs stage first.' }
    Info "VCPKG_ROOT = $env:VCPKG_ROOT"
    Info "VULKAN_SDK = $env:VULKAN_SDK"
    $tidy = if ($WithClangTidy) { 'ON' } else { 'OFF' }
    Info "clang-tidy gate: $tidy"
    # Clear cached find_package(Vulkan) results so a (re)configure always picks
    # up the current VULKAN_SDK -- e.g. after the SDK is bumped. No-op on a fresh
    # tree; on a reconfigure it forces Vulkan to be re-found against the new SDK.
    & cmake --preset dev '-UVulkan_*' "-DPYXIS_ENABLE_CLANG_TIDY=$tidy"
    if ($LASTEXITCODE -ne 0) { throw "cmake configure failed (exit $LASTEXITCODE)." }
}

function Invoke-Build {
    Section "Stage 5/6 - build ($Config)"
    Enter-PyxisDevShell
    Import-PersistedEnv
    if (-not (Test-Path (Join-Path $buildDir 'CMakeCache.txt'))) {
        Info 'build/dev not configured yet - running configure first.'
        Invoke-Configure
    }
    if ($Config -eq 'Release') {
        & cmake --build --preset dev-release
    } elseif ($Config -eq 'Debug') {
        & cmake --build --preset dev
    } else {
        & cmake --build $buildDir --config $Config
    }
    if ($LASTEXITCODE -ne 0) { throw "build failed (exit $LASTEXITCODE)." }
}

function Invoke-Verify {
    Section 'Stage 6/6 - verify'
    $exe = Join-Path $buildDir "bin\$Config\pyxis.exe"
    if (-not (Test-Path $exe)) {
        Write-Host "pyxis.exe not found at $exe." -ForegroundColor Yellow
        Write-Host 'Built binaries:' -ForegroundColor Yellow
        Get-ChildItem (Join-Path $buildDir "bin\$Config") -Filter *.exe -ErrorAction SilentlyContinue |
            ForEach-Object { Info $_.Name }
        return
    }
    Info "Built: $exe"
    & $exe --version
    Write-Host ''
    Write-Host "Pyxis initialized. Run the viewer with:  $exe" -ForegroundColor Green
}

switch ($Stage) {
    'prereqs'   { Invoke-Prereqs }
    'usd'       { Invoke-UsdSetup }
    'scenes'    { Invoke-Scenes }
    'configure' { Invoke-Configure }
    'build'     { Invoke-Build }
    'verify'    { Invoke-Verify }
    'all'       { Invoke-Prereqs; Invoke-UsdSetup; Invoke-Scenes; Invoke-Configure; Invoke-Build; Invoke-Verify }
}
