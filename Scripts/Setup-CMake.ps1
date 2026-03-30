<#
.SYNOPSIS
Configures and builds this project from the terminal (MSVC + CMake).

.DESCRIPTION
This script:
- Finds Visual Studio (via vswhere) and enters the MSVC dev environment.
- Configures CMake into Build/ directory.
- Builds (and optionally installs) with Ninja.

Notes:
- Default generator is single-config Ninja to match CMakePresets.json and VS Code CMake Tools.
- Use Ninja Multi-Config only when you explicitly want the legacy Build/x64 layout.

.EXAMPLE
.\Scripts\Setup-CMake.ps1

.EXAMPLE
.\Scripts\Setup-CMake.ps1 -Config Release

.EXAMPLE
.\Scripts\Setup-CMake.ps1 -ConfigureOnly

.EXAMPLE
.\Scripts\Setup-CMake.ps1 -CleanBuildDir -Config Debug -Build -Install

.EXAMPLE
.\Scripts\Setup-CMake.ps1 -Config Debug -Build -Target RealTimePathTracing -FirstFailureOnly -StopStaleBuildProcesses

.EXAMPLE
.\Scripts\Setup-CMake.ps1 -AllowConcurrentBuildProcesses -BuildArgs "--verbose"
#>

[CmdletBinding()]
param
(
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo', 'MinSizeRel')]
    [string]$Config = 'Debug', # Build configuration (used for multi-config build/install)

    [ValidateSet('Ninja Multi-Config', 'Ninja')]
    [string]$Generator = 'Ninja', # Preferred generator to match presets and VS Code

    [string]$Name = '', # Optional build folder name under Build/

    [switch]$ConfigureOnly, # Only run CMake configure/generate
    [switch]$Build,         # Build the project (default when -ConfigureOnly is not set)
    [switch]$Install,       # Run CMake install step
    [switch]$CleanBuildDir, # Delete the selected build folder before configuring

    [string]$VsWherePath = '',   # Optional explicit path to vswhere.exe
    [string]$VsInstallPath = '', # Optional explicit Visual Studio installation root
    [string]$VsDevCmdPath = '',  # Optional explicit path to VsDevCmd.bat (overrides all detection)

    [string]$CMakeArgs = '',       # Extra args passed to "cmake -S -B ..."
    [string]$BuildArgs = '',       # Extra args passed to "cmake --build ..."
    [string]$InstallArgs = '',     # Extra args passed to "cmake --install ..."
    [string]$Target = '',

    [switch]$StopStaleBuildProcesses,
    [switch]$AllowConcurrentBuildProcesses,
    [switch]$FirstFailureOnly
)

# ----------------------------------------------------------------------------------------------------------------------
# Resolve repo root from this script location.
#

$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot '..')

# Default build folder name.
if ([string]::IsNullOrWhiteSpace($Name))
{
    if ($Generator -eq 'Ninja')
    {
        $Name = "x64-$Config" # Single-config needs separate build dir per config
    }
    else
    {
        $Name = 'x64' # Multi-config can build multiple configs from one build tree
    }
}

$BuildRoot   = Join-Path $RepoRoot "Build\$Name"
$InstallRoot = Join-Path $RepoRoot "Install\$Name"

# ----------------------------------------------------------------------------------------------------------------------
# Tool discovery (vswhere + VsDevCmd).
#

$ResolvedVsDevCmdPath = $null

if (-not [string]::IsNullOrWhiteSpace($VsDevCmdPath))
{
    # Explicit override (most robust).
    if (-not (Test-Path -LiteralPath $VsDevCmdPath))
    {
        throw "VsDevCmd.bat not found at explicit path: $VsDevCmdPath"
    }

    $ResolvedVsDevCmdPath = (Resolve-Path -LiteralPath $VsDevCmdPath).Path
}
else
{
    $ResolvedVsInstallPath = $null

    if (-not [string]::IsNullOrWhiteSpace($VsInstallPath))
    {
        if (-not (Test-Path -LiteralPath $VsInstallPath))
        {
            throw "Visual Studio install path not found at explicit path: $VsInstallPath"
        }

        $ResolvedVsInstallPath = (Resolve-Path -LiteralPath $VsInstallPath).Path
    }
    else
    {
        # Detect vswhere via (1) explicit path, (2) PATH, (3) common installer locations.
        $CandidateVsWherePaths = @()

        if (-not [string]::IsNullOrWhiteSpace($VsWherePath))
        {
            $CandidateVsWherePaths += $VsWherePath
        }

        $VsWhereFromPath = (Get-Command vswhere.exe -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty Source)
        if (-not [string]::IsNullOrWhiteSpace($VsWhereFromPath))
        {
            $CandidateVsWherePaths += $VsWhereFromPath
        }

        # NOTE: Use ${Env:ProgramFiles(x86)} (PowerShell syntax for env vars with parentheses).
        $CandidateVsWherePaths += @(
            (Join-Path ${Env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'),
            (Join-Path $Env:ProgramFiles        'Microsoft Visual Studio\Installer\vswhere.exe')
        )

        $ResolvedVsWherePath = $CandidateVsWherePaths | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1

        if ($ResolvedVsWherePath)
        {
            $ResolvedVsInstallPath =
            & $ResolvedVsWherePath `
                -latest `
                -products * `
                -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
                -property installationPath

            if ([string]::IsNullOrWhiteSpace($ResolvedVsInstallPath))
            {
                throw "Visual Studio with MSVC tools not found (vswhere returned empty installationPath)."
            }
        }
        else
        {
            # Fallback: locate VsDevCmd.bat directly using common Visual Studio install patterns.
            $VsDevCmdCandidates =
            @(
                Get-ChildItem -Path (Join-Path $Env:ProgramFiles 'Microsoft Visual Studio\*\*\Common7\Tools\VsDevCmd.bat') -ErrorAction SilentlyContinue
                Get-ChildItem -Path (Join-Path ${Env:ProgramFiles(x86)} 'Microsoft Visual Studio\*\*\Common7\Tools\VsDevCmd.bat') -ErrorAction SilentlyContinue
            ) | Where-Object { $_ } | Select-Object -ExpandProperty FullName

            if ($VsDevCmdCandidates.Count -gt 0)
            {
                # Prefer higher "year" installs (e.g., 2026 over 2022) if the path includes a 4-digit year.
                $ResolvedVsDevCmdPath =
                    $VsDevCmdCandidates |
                    Sort-Object -Descending -Property @(
                        @{ Expression = {
                            if ($_ -match '\\Microsoft Visual Studio\\(?<Year>\d{4})\\')
                            {
                                return [int]$Matches['Year']
                            }
                            return 0
                        } },
                        @{ Expression = { $_ } }
                    ) |
                    Select-Object -First 1
            }
            else
            {
                throw "vswhere.exe not found, and VsDevCmd.bat could not be located. Install Visual Studio (or Build Tools) with MSVC x64/x86, or pass -VsDevCmdPath explicitly."
            }
        }
    }

    if (-not $ResolvedVsDevCmdPath)
    {
        $ResolvedVsDevCmdPath = Join-Path $ResolvedVsInstallPath 'Common7\Tools\VsDevCmd.bat'
    }

    if (-not (Test-Path -LiteralPath $ResolvedVsDevCmdPath))
    {
        throw "VsDevCmd.bat not found at: $ResolvedVsDevCmdPath"
    }
}

# ----------------------------------------------------------------------------------------------------------------------
# CMake command setup.
#

if (-not (Get-Command cmake -ErrorAction SilentlyContinue))
{
    throw "CMake not found in PATH. Install CMake 3.22+ and restart your terminal."
}

try
{
    $ExistingBuildProcesses = @(Get-Process -Name cmake, ninja, cl, link -ErrorAction Stop)
}
catch
{
    $ExistingBuildProcesses = @()
}

if ($ExistingBuildProcesses.Count -gt 0)
{
    if ($StopStaleBuildProcesses)
    {
        Write-Host "Stopping existing build-related processes:"
        $ExistingBuildProcesses | ForEach-Object {
            Write-Host "  $($_.ProcessName) [$($_.Id)]"
            Stop-Process -Id $_.Id -Force
        }
    }
    elseif ($AllowConcurrentBuildProcesses)
    {
        Write-Warning "Continuing even though build-related processes are already running:"
        $ExistingBuildProcesses | ForEach-Object {
            Write-Warning "  $($_.ProcessName) [$($_.Id)]"
        }
    }
    else
    {
        $ProcessList =
            ($ExistingBuildProcesses |
                ForEach-Object { "$($_.ProcessName) [$($_.Id)]" }) -join ', '

        throw "Existing cmake/ninja/cl/link processes are already running: $ProcessList`nUse -StopStaleBuildProcesses to clear interrupted builds, or -AllowConcurrentBuildProcesses if you intentionally want overlap."
    }
}

if ($CleanBuildDir -and (Test-Path -LiteralPath $BuildRoot))
{
    Write-Host "Deleting build folder: $BuildRoot"
    Remove-Item -LiteralPath $BuildRoot -Recurse -Force
}

if (-not $ConfigureOnly)
{
    $Build = $true # Default behavior: configure + build
}

# Configure arguments.
$ConfigureCmd =
@(
    'cmake'
    '-S', "`"$RepoRoot`""
    '-B', "`"$BuildRoot`""
    '-G', "`"$Generator`""
    "-DCMAKE_INSTALL_PREFIX=`"$InstallRoot`""
)

if ($Generator -eq 'Ninja')
{
    $ConfigureCmd += "-DCMAKE_BUILD_TYPE=$Config" # Single-config generators require this at configure time
}

if (-not [string]::IsNullOrWhiteSpace($CMakeArgs))
{
    $ConfigureCmd += $CMakeArgs
}

# Build arguments.
$BuildCmd =
@(
    'cmake'
    '--build', "`"$BuildRoot`""
    '--parallel'
)

if ($Generator -ne 'Ninja')
{
    $BuildCmd += @('--config', $Config) # Multi-config build type is chosen at build time
}

if (-not [string]::IsNullOrWhiteSpace($Target))
{
    $BuildCmd += @('--target', $Target)
}

if ($FirstFailureOnly)
{
    $BuildCmd += @('--parallel', '1')
}

if (-not [string]::IsNullOrWhiteSpace($BuildArgs))
{
    $BuildCmd += $BuildArgs
}

if ($FirstFailureOnly -and $Generator -eq 'Ninja')
{
    $BuildCmd += @('--', '-k', '1')
}

# Install arguments.
$InstallCmd =
@(
    'cmake'
    '--install', "`"$BuildRoot`""
)

if ($Generator -ne 'Ninja')
{
    $InstallCmd += @('--config', $Config) # Multi-config install uses --config
}

if (-not [string]::IsNullOrWhiteSpace($InstallArgs))
{
    $InstallCmd += $InstallArgs
}

# ----------------------------------------------------------------------------------------------------------------------
# Execute everything inside the MSVC dev environment (batch file).
#

Write-Host ""
Write-Host "RepoRoot:    $RepoRoot"
Write-Host "BuildRoot:   $BuildRoot"
Write-Host "InstallRoot: $InstallRoot"
Write-Host "Generator:   $Generator"
Write-Host "Config:      $Config"
Write-Host "Target:      $Target"
Write-Host "FirstFail:   $FirstFailureOnly"
Write-Host ""

# Join commands for cmd.exe chaining.
$CmdChain = @()
$CmdChain += "`"$ResolvedVsDevCmdPath`" -no_logo -arch=amd64 -host_arch=amd64"
$CmdChain += ($ConfigureCmd -join ' ')

if ($Build)
{
    $CmdChain += ($BuildCmd -join ' ')
}

if ($Install)
{
    $CmdChain += ($InstallCmd -join ' ')
}

$CmdLine = ($CmdChain -join ' && ')

Write-Host "Running:"
Write-Host "  $CmdLine"
Write-Host ""

& cmd.exe /s /c $CmdLine
if ($LASTEXITCODE -ne 0)
{
    throw "Setup-CMake.ps1 failed (exit code: $LASTEXITCODE)."
}

Write-Host ""
Write-Host "Done."

