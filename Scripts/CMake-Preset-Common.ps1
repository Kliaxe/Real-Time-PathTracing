function Resolve-VsDevCmd
{
    param(
        [string]$VsWherePath = '',
        [string]$VsInstallPath = '',
        [string]$VsDevCmdPath = ''
    )

    if(-not [string]::IsNullOrWhiteSpace($VsDevCmdPath))
    {
        if(-not (Test-Path -LiteralPath $VsDevCmdPath))
        {
            throw "VsDevCmd.bat not found: $VsDevCmdPath"
        }
        return (Resolve-Path -LiteralPath $VsDevCmdPath).Path
    }

    $installPath = $VsInstallPath
    if([string]::IsNullOrWhiteSpace($installPath))
    {
        $candidates = @()
        if(-not [string]::IsNullOrWhiteSpace($VsWherePath))
        {
            $candidates += $VsWherePath
        }

        $fromPath = Get-Command vswhere.exe -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty Source
        if($fromPath)
        {
            $candidates += $fromPath
        }

        $candidates += @(
            (Join-Path ${Env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'),
            (Join-Path $Env:ProgramFiles 'Microsoft Visual Studio\Installer\vswhere.exe')
        )

        $vswhere = $candidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
        if(-not $vswhere)
        {
            throw "vswhere.exe was not found. Install Visual Studio Build Tools or pass -VsDevCmdPath."
        }

        $installPath =
            & $vswhere `
                -latest `
                -products * `
                -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
                -property installationPath
    }

    if([string]::IsNullOrWhiteSpace($installPath))
    {
        throw "Visual Studio with MSVC tools was not found."
    }

    $devCmd = Join-Path $installPath 'Common7\Tools\VsDevCmd.bat'
    if(-not (Test-Path -LiteralPath $devCmd))
    {
        throw "VsDevCmd.bat not found: $devCmd"
    }

    return $devCmd
}

function Get-RepoBuildProcesses
{
    param(
        [string]$RepoRoot,
        [string]$BuildRoot
    )

    $names = @('cmake.exe', 'ninja.exe', 'cl.exe', 'link.exe')
    Get-CimInstance Win32_Process |
        Where-Object {
            $names -contains $_.Name -and
            ($_.CommandLine -like "*$RepoRoot*" -or $_.CommandLine -like "*$BuildRoot*")
        }
}

function Test-RepoBuildProcesses
{
    param(
        [string]$RepoRoot,
        [string]$BuildRoot,
        [switch]$AllowConcurrent,
        [switch]$StopScoped
    )

    $processes = @(Get-RepoBuildProcesses -RepoRoot $RepoRoot -BuildRoot $BuildRoot)
    if($processes.Count -eq 0)
    {
        return
    }

    if($StopScoped)
    {
        Write-Host "Stopping build processes for this repo:"
        foreach($process in $processes)
        {
            Write-Host "  $($process.Name) [$($process.ProcessId)]"
            Stop-Process -Id $process.ProcessId -Force
        }
        return
    }

    if($AllowConcurrent)
    {
        Write-Warning "Continuing while this repo already has build processes running."
        return
    }

    $list = ($processes | ForEach-Object { "$($_.Name) [$($_.ProcessId)]" }) -join ', '
    throw "Build processes for this repo are already running: $list"
}

function Invoke-InVsDevCmd
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$VsDevCmdPath,

        [Parameter(Mandatory = $true)]
        [string[]]$CommandParts
    )

    if(-not (Get-Command cmake -ErrorAction SilentlyContinue))
    {
        throw "CMake was not found in PATH."
    }

    $commandLine = "`"$VsDevCmdPath`" -no_logo -arch=amd64 -host_arch=amd64 && " + ($CommandParts -join ' ')

    Write-Host "Running:"
    Write-Host "  $commandLine"
    Write-Host ""

    & cmd.exe /s /c $commandLine
    if($LASTEXITCODE -ne 0)
    {
        throw "Command failed with exit code $LASTEXITCODE."
    }
}
