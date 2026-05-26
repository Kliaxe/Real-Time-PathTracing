<#
.SYNOPSIS
Builds the project through the CMake preset used by VS Code.

.DESCRIPTION
This is a thin wrapper around:
  cmake --build --preset x64-<Config> --target <Target>

The wrapper only adds two conveniences:
- it enters the MSVC developer environment before invoking CMake;
- it fails fast when another build for this repo is already running.
#>

[CmdletBinding()]
param
(
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo', 'MinSizeRel')]
    [string]$Config = 'Debug',

    [string]$Preset = '',
    [string]$Target = 'RealTimePathTracing',

    [string]$VsWherePath = '',
    [string]$VsInstallPath = '',
    [string]$VsDevCmdPath = '',

    [switch]$FirstFailureOnly,
    [switch]$DryRun,
    [switch]$AllowConcurrentBuildProcesses,
    [switch]$StopStaleBuildProcesses
)

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if([string]::IsNullOrWhiteSpace($Preset))
{
    $Preset = "x64-$Config"
}

. "$PSScriptRoot\CMake-Preset-Common.ps1"

$VsDevCmd = Resolve-VsDevCmd `
    -VsWherePath $VsWherePath `
    -VsInstallPath $VsInstallPath `
    -VsDevCmdPath $VsDevCmdPath

Test-RepoBuildProcesses `
    -RepoRoot $RepoRoot `
    -BuildRoot (Join-Path $RepoRoot "Build\$Preset") `
    -AllowConcurrent:$AllowConcurrentBuildProcesses `
    -StopScoped:$StopStaleBuildProcesses

$BuildCmd = @('cmake', '--build', '--preset', $Preset)
if(-not [string]::IsNullOrWhiteSpace($Target))
{
    $BuildCmd += @('--target', $Target)
}
if($FirstFailureOnly)
{
    $BuildCmd += @('--parallel', '1')
}
if($DryRun)
{
    $BuildCmd += @('--', '-n')
}
elseif($FirstFailureOnly)
{
    $BuildCmd += @('--', '-k', '1')
}

Write-Host ""
Write-Host "RepoRoot:  $RepoRoot"
Write-Host "Preset:    $Preset"
Write-Host "Target:    $Target"
Write-Host "DryRun:    $DryRun"
Write-Host "FirstFail: $FirstFailureOnly"
Write-Host ""

Invoke-InVsDevCmd -VsDevCmdPath $VsDevCmd -CommandParts $BuildCmd
