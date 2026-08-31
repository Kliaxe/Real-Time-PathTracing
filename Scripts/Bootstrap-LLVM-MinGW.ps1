<#
.SYNOPSIS
Creates a stable LLVM MinGW path for the CMake presets.

.DESCRIPTION
WinGet installs LLVM MinGW in a versioned package directory. This script
creates %LOCALAPPDATA%\LLVM-MinGW as a directory junction to the newest
installed UCRT toolchain so CMake Tools can locate Clang without relying on
the VS Code process PATH.
#>

[CmdletBinding()]
param()

$packageRoot = Join-Path $env:LOCALAPPDATA 'Microsoft\WinGet\Packages'
$toolchains = @(
    Get-ChildItem -Path $packageRoot -Directory -Filter 'MartinStorsjo.LLVM-MinGW.UCRT_*' -ErrorAction SilentlyContinue |
        ForEach-Object {
            Get-ChildItem -Path $_.FullName -Directory -Filter 'llvm-mingw-*-ucrt-x86_64' -ErrorAction SilentlyContinue
        } |
        Sort-Object Name -Descending
)

if($toolchains.Count -eq 0)
{
    throw 'LLVM MinGW (UCRT) was not found. Install MartinStorsjo.LLVM-MinGW.UCRT with WinGet first.'
}

$toolchain = $toolchains[0]
$compiler = Join-Path $toolchain.FullName 'bin\x86_64-w64-mingw32-clang.exe'
if(-not (Test-Path -LiteralPath $compiler))
{
    throw "LLVM MinGW compiler was not found: $compiler"
}

$stablePath = Join-Path $env:LOCALAPPDATA 'LLVM-MinGW'
if(Test-Path -LiteralPath $stablePath)
{
    $existing = Get-Item -LiteralPath $stablePath
    if($existing.LinkType -eq 'Junction' -and $existing.Target -contains $toolchain.FullName)
    {
        Write-Host "LLVM MinGW path is already current: $stablePath"
        return
    }

    throw "Refusing to replace the existing path: $stablePath"
}

New-Item -ItemType Junction -Path $stablePath -Target $toolchain.FullName | Out-Null
Write-Host "Created LLVM MinGW path: $stablePath"
