<#
.SYNOPSIS
Clears CMake-generated build artifacts for this project.

.DESCRIPTION
Deletes build output folders that are safe to regenerate:
- Build/ (CMake build trees)
- Install/ (CMake install outputs)
- Intermediate/ (library archives and MSBuild intermediates)
- out/ (if used by CMake presets or IDE integration)
- .vs/ (Visual Studio workspace cache)
- _install/ (CMake install prefix, if present)

Optionally deletes Binaries/ as well.

.EXAMPLE
.\Scripts\Clean-CMake.ps1 -WhatIf

.EXAMPLE
.\Scripts\Clean-CMake.ps1 -All -Confirm

.EXAMPLE
.\Scripts\Clean-CMake.ps1 -All -FailFast -Confirm
#>

[CmdletBinding(SupportsShouldProcess = $true, ConfirmImpact = 'High')]
param
(
    [switch]$All,             # Also delete Binaries/
    [switch]$KeepBinaries,    # Never delete Binaries/ (overrides -All)
    [switch]$KeepVsCache,     # Keep .vs/ (sometimes useful)
    [switch]$FailFast         # Abort on first failure (locked file, permissions, etc.)
)

# Resolve repo root from this script location.
$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot '..')

# Build list of folders to delete.
$PathsToDelete = @()

$PathsToDelete += (Join-Path $RepoRoot 'Build')        # CMake build tree
$PathsToDelete += (Join-Path $RepoRoot 'Install')      # CMake install outputs
$PathsToDelete += (Join-Path $RepoRoot 'Intermediate') # Library archives + MSBuild intermediates
$PathsToDelete += (Join-Path $RepoRoot 'out')          # CMake/IDE generated outputs
$PathsToDelete += (Join-Path $RepoRoot '_install')     # CMake install prefix (if used)

if (-not $KeepVsCache)
{
    $PathsToDelete += (Join-Path $RepoRoot '.vs')      # Visual Studio cache
}

if ($All -and (-not $KeepBinaries))
{
    $PathsToDelete += (Join-Path $RepoRoot 'Binaries') # Runtime outputs (optional)
}

# Delete safely.
$FailedPaths = @()

foreach ($Path in $PathsToDelete)
{
    if (-not (Test-Path -LiteralPath $Path))
    {
        continue
    }

    if ($PSCmdlet.ShouldProcess($Path, 'Remove directory recursively'))
    {
        try
        {
            Remove-Item -LiteralPath $Path -Recurse -Force -ErrorAction Stop
        }
        catch
        {
            $FailedPaths += $Path

            Write-Warning "Failed to delete: $Path"
            Write-Warning "Reason: $($_.Exception.Message)"
            Write-Warning "Tip: close Cursor/Visual Studio (and any file indexers) then rerun."

            if ($FailFast)
            {
                throw
            }
        }
    }
}

if ($FailedPaths.Count -gt 0)
{
    Write-Host ""
    Write-Host "Done (partial). Some paths could not be deleted:"
    foreach ($Failed in ($FailedPaths | Sort-Object -Unique))
    {
        Write-Host " - $Failed"
    }
    Write-Host ""
    Write-Host "If you want the script to fail fast instead, rerun with -FailFast."
}
else
{
    Write-Host "Done. Cleaned CMake artifacts under: $RepoRoot"
}

