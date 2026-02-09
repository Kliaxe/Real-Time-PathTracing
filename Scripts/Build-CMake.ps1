<#
.SYNOPSIS
Builds this project from an existing CMake build directory (MSVC + CMake).

.DESCRIPTION
This script:
- Finds Visual Studio (via vswhere or fallback) and enters the MSVC dev environment.
- Runs only `cmake --build` against an existing Build/ folder.

Use Setup-CMake.ps1 when you need configure/generate or dependency setup.

.EXAMPLE
.\Scripts\Build-CMake.ps1

.EXAMPLE
.\Scripts\Build-CMake.ps1 -Config Release

.EXAMPLE
.\Scripts\Build-CMake.ps1 -Name x64 -BuildArgs "--target RealTimePathTracing"
#>

[CmdletBinding()]
param
(
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo', 'MinSizeRel')]
    [string]$Config = 'Debug',

    [string]$Name = 'x64',

    [string]$VsWherePath = '',
    [string]$VsInstallPath = '',
    [string]$VsDevCmdPath = '',

    [string]$BuildArgs = ''
)

$RepoRoot  = Resolve-Path (Join-Path $PSScriptRoot '..')
$BuildRoot = Join-Path $RepoRoot "Build\$Name"

if (-not (Test-Path -LiteralPath $BuildRoot))
{
    throw "Build folder not found: $BuildRoot`nRun .\Scripts\Setup-CMake.ps1 first."
}

# Determine generator from CMake cache if available.
$Generator = ''
$CacheFile = Join-Path $BuildRoot 'CMakeCache.txt'
if (Test-Path -LiteralPath $CacheFile)
{
    $GeneratorLine = Get-Content -LiteralPath $CacheFile | Where-Object { $_ -like 'CMAKE_GENERATOR:*=' } | Select-Object -First 1
    if (-not [string]::IsNullOrWhiteSpace($GeneratorLine))
    {
        $Generator = ($GeneratorLine -split '=', 2)[1].Trim()
    }
}
if ([string]::IsNullOrWhiteSpace($Generator))
{
    $Generator = 'Ninja Multi-Config'
}

$ResolvedVsDevCmdPath = $null

if (-not [string]::IsNullOrWhiteSpace($VsDevCmdPath))
{
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

        $CandidateVsWherePaths += @(
            (Join-Path ${Env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'),
            (Join-Path $Env:ProgramFiles 'Microsoft Visual Studio\Installer\vswhere.exe')
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
            $VsDevCmdCandidates =
            @(
                Get-ChildItem -Path (Join-Path $Env:ProgramFiles 'Microsoft Visual Studio\*\*\Common7\Tools\VsDevCmd.bat') -ErrorAction SilentlyContinue
                Get-ChildItem -Path (Join-Path ${Env:ProgramFiles(x86)} 'Microsoft Visual Studio\*\*\Common7\Tools\VsDevCmd.bat') -ErrorAction SilentlyContinue
            ) | Where-Object { $_ } | Select-Object -ExpandProperty FullName

            if ($VsDevCmdCandidates.Count -gt 0)
            {
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

if (-not (Get-Command cmake -ErrorAction SilentlyContinue))
{
    throw "CMake not found in PATH. Install CMake 3.22+ and restart your terminal."
}

$BuildCmd =
@(
    'cmake'
    '--build', "`"$BuildRoot`""
    '--parallel'
)

if ($Generator -ne 'Ninja')
{
    $BuildCmd += @('--config', $Config)
}

if (-not [string]::IsNullOrWhiteSpace($BuildArgs))
{
    $BuildCmd += $BuildArgs
}

Write-Host ""
Write-Host "RepoRoot:  $RepoRoot"
Write-Host "BuildRoot: $BuildRoot"
Write-Host "Generator: $Generator"
Write-Host "Config:    $Config"
Write-Host ""

$CmdLine = "`"$ResolvedVsDevCmdPath`" -no_logo -arch=amd64 -host_arch=amd64 && " + ($BuildCmd -join ' ')

Write-Host "Running:"
Write-Host "  $CmdLine"
Write-Host ""

& cmd.exe /s /c $CmdLine
if ($LASTEXITCODE -ne 0)
{
    throw "Build-CMake.ps1 failed (exit code: $LASTEXITCODE)."
}

Write-Host ""
Write-Host "Done."
