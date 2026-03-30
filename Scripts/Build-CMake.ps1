<#
.SYNOPSIS
Builds this project from an existing CMake build directory (MSVC + CMake).

.DESCRIPTION
This script:
- Finds Visual Studio (via vswhere or fallback) and enters the MSVC dev environment.
- Prefers the matching CMake build preset (same path VS Code CMake Tools uses).
- Falls back to `cmake --build <Build/...>` when `-Name` is passed explicitly.

Use Setup-CMake.ps1 when you need configure/generate or dependency setup.

.EXAMPLE
.\Scripts\Build-CMake.ps1

.EXAMPLE
.\Scripts\Build-CMake.ps1 -Config Release

.EXAMPLE
.\\Scripts\\Build-CMake.ps1 -Target RealTimePathTracing -FirstFailureOnly -StopStaleBuildProcesses

.EXAMPLE
.\Scripts\Build-CMake.ps1 -Name x64 -BuildArgs "--target RealTimePathTracing"

.EXAMPLE
.\Scripts\Build-CMake.ps1 -AllowConcurrentBuildProcesses -BuildArgs "--verbose"
#>

[CmdletBinding()]
param
(
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo', 'MinSizeRel')]
    [string]$Config = 'Debug',

    [string]$Name = '',

    [string]$Preset = '',

    [string]$VsWherePath = '',
    [string]$VsInstallPath = '',
    [string]$VsDevCmdPath = '',

    [string]$BuildArgs = '',

    [string]$Target = '',

    [switch]$StopStaleBuildProcesses,
    [switch]$AllowConcurrentBuildProcesses,
    [switch]$RefreshCMakeApiReply,
    [switch]$FirstFailureOnly,

    [switch]$DryRun
)

$RepoRoot  = Resolve-Path (Join-Path $PSScriptRoot '..')

$PresetFile         = Join-Path $RepoRoot 'CMakePresets.json'
$DefaultPresetName  = "x64-$Config"
$UsePresetBuild     = $false
$ResolvedBuildRoot  = $null
$ResolvedGenerator  = ''

function Get-GeneratorFromCache
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$BuildRoot
    )

    $CacheFile = Join-Path $BuildRoot 'CMakeCache.txt'
    if (-not (Test-Path -LiteralPath $CacheFile))
    {
        return ''
    }

    $GeneratorLine = Get-Content -LiteralPath $CacheFile | Where-Object { $_ -like 'CMAKE_GENERATOR:INTERNAL=*' } | Select-Object -First 1
    if ([string]::IsNullOrWhiteSpace($GeneratorLine))
    {
        return ''
    }

    return ($GeneratorLine -split '=', 2)[1].Trim()
}

if ([string]::IsNullOrWhiteSpace($Preset))
{
    $Preset = $DefaultPresetName
}

if ([string]::IsNullOrWhiteSpace($Name) -and (Test-Path -LiteralPath $PresetFile))
{
    $PresetJson = Get-Content -LiteralPath $PresetFile -Raw | ConvertFrom-Json
    $HasBuildPreset = @($PresetJson.buildPresets | Where-Object { $_.name -eq $Preset }).Count -gt 0
    if ($HasBuildPreset)
    {
        $UsePresetBuild    = $true
        $ResolvedBuildRoot = Join-Path $RepoRoot "Build\$Preset"
        $ResolvedGenerator = Get-GeneratorFromCache -BuildRoot $ResolvedBuildRoot
    }
}

if (-not $UsePresetBuild)
{
    if ([string]::IsNullOrWhiteSpace($Name))
    {
        $Name = $DefaultPresetName
    }

    $ResolvedBuildRoot = Join-Path $RepoRoot "Build\$Name"

    if (-not (Test-Path -LiteralPath $ResolvedBuildRoot))
    {
        throw "Build folder not found: $ResolvedBuildRoot`nRun .\Scripts\Setup-CMake.ps1 first."
    }

    $ResolvedGenerator = Get-GeneratorFromCache -BuildRoot $ResolvedBuildRoot
    if ([string]::IsNullOrWhiteSpace($ResolvedGenerator))
    {
        $ResolvedGenerator = 'Ninja'
    }
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

if ($RefreshCMakeApiReply)
{
    $ReplyDir = Join-Path $ResolvedBuildRoot '.cmake\api\v1\reply'
    if (Test-Path -LiteralPath $ReplyDir)
    {
        Write-Host "Clearing CMake API reply cache: $ReplyDir"
        Remove-Item -LiteralPath $ReplyDir -Recurse -Force
    }

    if ($UsePresetBuild)
    {
        $RefreshCmd = @('cmake', '--preset', $Preset)
    }
    else
    {
        $RefreshCmd = @('cmake', '-S', "`"$RepoRoot`"", '-B', "`"$ResolvedBuildRoot`"")
    }
}

if ($UsePresetBuild)
{
    $BuildCmd =
    @(
        'cmake'
        '--build'
        '--preset', $Preset
    )
}
else
{
    $BuildCmd =
    @(
        'cmake'
        '--build', "`"$ResolvedBuildRoot`""
        '--parallel'
    )

    if ($ResolvedGenerator -ne 'Ninja')
    {
        $BuildCmd += @('--config', $Config)
    }
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

if ($DryRun)
{
    $BuildCmd += @('--', '-n')
}
elseif ($FirstFailureOnly -and $ResolvedGenerator -eq 'Ninja')
{
    $BuildCmd += @('--', '-k', '1')
}

Write-Host ""
Write-Host "RepoRoot:  $RepoRoot"
Write-Host "Config:    $Config"
if ($UsePresetBuild)
{
    Write-Host "Mode:      preset"
    Write-Host "Preset:    $Preset"
    Write-Host "BuildRoot: $ResolvedBuildRoot"
}
else
{
    Write-Host "Mode:      legacy"
    Write-Host "BuildRoot: $ResolvedBuildRoot"
    Write-Host "Generator: $ResolvedGenerator"
}
Write-Host "DryRun:    $DryRun"
Write-Host "Target:    $Target"
Write-Host "Generator: $ResolvedGenerator"
Write-Host "FirstFail: $FirstFailureOnly"
Write-Host ""

$CmdChain = @("`"$ResolvedVsDevCmdPath`" -no_logo -arch=amd64 -host_arch=amd64")
if ($RefreshCMakeApiReply)
{
    $CmdChain += ($RefreshCmd -join ' ')
}
$CmdChain += ($BuildCmd -join ' ')
$CmdLine = $CmdChain -join ' && '

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
