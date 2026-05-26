<#
.SYNOPSIS
Configures the project through the CMake preset used by VS Code.

.DESCRIPTION
This is a thin wrapper around:
  cmake --preset x64-<Config>

Use -Build when the configure step should be followed by the matching build
preset.
#>

[CmdletBinding()]
param
(
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo', 'MinSizeRel')]
    [string]$Config = 'Debug',

    [string]$Preset = '',
    [string]$Target = 'RealTimePathTracing',

    [switch]$Build,
    [switch]$CleanBuildDir,
    [switch]$FirstFailureOnly,
    [switch]$AllowConcurrentBuildProcesses,
    [switch]$StopStaleBuildProcesses,

    [string]$VsWherePath = '',
    [string]$VsInstallPath = '',
    [string]$VsDevCmdPath = ''
)

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if([string]::IsNullOrWhiteSpace($Preset))
{
    $Preset = "x64-$Config"
}

. "$PSScriptRoot\CMake-Preset-Common.ps1"

$BuildRoot = Join-Path $RepoRoot "Build\$Preset"
$VsDevCmd = Resolve-VsDevCmd `
    -VsWherePath $VsWherePath `
    -VsInstallPath $VsInstallPath `
    -VsDevCmdPath $VsDevCmdPath

Test-RepoBuildProcesses `
    -RepoRoot $RepoRoot `
    -BuildRoot $BuildRoot `
    -AllowConcurrent:$AllowConcurrentBuildProcesses `
    -StopScoped:$StopStaleBuildProcesses

if($CleanBuildDir -and (Test-Path -LiteralPath $BuildRoot))
{
    $resolvedBuildRoot = (Resolve-Path -LiteralPath $BuildRoot).Path
    if(-not $resolvedBuildRoot.StartsWith((Join-Path $RepoRoot 'Build'), [System.StringComparison]::OrdinalIgnoreCase))
    {
        throw "Refusing to delete a build folder outside this repo: $resolvedBuildRoot"
    }

    Remove-Item -LiteralPath $resolvedBuildRoot -Recurse -Force
}

Write-Host ""
Write-Host "RepoRoot:  $RepoRoot"
Write-Host "Preset:    $Preset"
Write-Host "BuildRoot: $BuildRoot"
Write-Host "Build:     $Build"
Write-Host ""

Invoke-InVsDevCmd -VsDevCmdPath $VsDevCmd -CommandParts @('cmake', '--preset', $Preset)

if($Build)
{
    & "$PSScriptRoot\Build-CMake.ps1" `
        -Preset $Preset `
        -Target $Target `
        -FirstFailureOnly:$FirstFailureOnly `
        -AllowConcurrentBuildProcesses:$AllowConcurrentBuildProcesses `
        -StopStaleBuildProcesses:$StopStaleBuildProcesses `
        -VsDevCmdPath $VsDevCmd

    if($LASTEXITCODE -ne 0)
    {
        throw "Build-CMake.ps1 failed with exit code $LASTEXITCODE."
    }
}
