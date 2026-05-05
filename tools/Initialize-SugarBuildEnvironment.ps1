[CmdletBinding()]
param(
    [switch]$Force,
    [switch]$Quiet
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$userPresetsPath = Join-Path $repoRoot "CMakeUserPresets.json"

function Convert-WindowsPathToWsl {
    param([Parameter(Mandatory = $true)][string]$WindowsPath)
    $fullPath = [System.IO.Path]::GetFullPath($WindowsPath)
    if ($fullPath.Length -lt 3 -or $fullPath[1] -ne ":") {
        throw "Expected a drive-qualified Windows path, got '$fullPath'."
    }

    $drive = [char]::ToLowerInvariant($fullPath[0])
    $suffix = $fullPath.Substring(2).Replace("\", "/")
    return "/mnt/$drive$suffix"
}

function New-MountedConfigurePreset {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$DisplayName,
        [Parameter(Mandatory = $true)][string]$BasePreset
    )

    return [ordered]@{
        name = $Name
        displayName = $DisplayName
        description = "Build WSL/Linux from the mounted Windows repo path; do not copy sources into /home/<user>/.vs."
        inherits = @($BasePreset, "linux-mounted-source-base")
    }
}

function New-LinuxHostCondition {
    return [ordered]@{
        type = "equals"
        lhs = '${hostSystemName}'
        rhs = "Linux"
    }
}

function New-MountedBuildPreset {
    param([Parameter(Mandatory = $true)][string]$Name)
    return [ordered]@{
        name = $Name
        configurePreset = $Name
        condition = New-LinuxHostCondition
    }
}

function New-MountedTestPreset {
    param([Parameter(Mandatory = $true)][string]$Name)
    return [ordered]@{
        name = $Name
        configurePreset = $Name
        condition = New-LinuxHostCondition
        output = [ordered]@{
            outputOnFailure = $true
        }
    }
}

$mountedSourceRoot = Convert-WindowsPathToWsl -WindowsPath $repoRoot

if (Test-Path -LiteralPath $userPresetsPath) {
    $existing = Get-Content -Raw -LiteralPath $userPresetsPath
    if (-not $Force -and $existing -notmatch "linux-mounted-source-base") {
        throw "CMakeUserPresets.json exists and does not look like SugarBot's generated mounted-preset file. Re-run with -Force if you want to replace it."
    }
}

$presetData = [ordered]@{
    version = 8
    configurePresets = @(
        [ordered]@{
            name = "linux-mounted-source-base"
            hidden = $true
            description = "Visual Studio WSL remote settings for SugarBot's mounted Windows repo source tree."
            vendor = [ordered]@{
                "microsoft.com/VisualStudioRemoteSettings/CMake/1.0" = [ordered]@{
                    sourceDir = $mountedSourceRoot
                    copySources = $false
                }
            }
        },
        (New-MountedConfigurePreset -Name "linux-debug-mounted" -DisplayName "Linux Debug (Mounted Repo)" -BasePreset "linux-debug"),
        (New-MountedConfigurePreset -Name "linux-release-mounted" -DisplayName "Linux Release (Mounted Repo)" -BasePreset "linux-release"),
        (New-MountedConfigurePreset -Name "linux-asan-mounted" -DisplayName "Linux ASan (Mounted Repo)" -BasePreset "linux-asan"),
        (New-MountedConfigurePreset -Name "linux-tsan-mounted" -DisplayName "Linux TSan (Mounted Repo)" -BasePreset "linux-tsan")
    )
    buildPresets = @(
        (New-MountedBuildPreset -Name "linux-debug-mounted"),
        (New-MountedBuildPreset -Name "linux-release-mounted"),
        (New-MountedBuildPreset -Name "linux-asan-mounted"),
        (New-MountedBuildPreset -Name "linux-tsan-mounted")
    )
    testPresets = @(
        (New-MountedTestPreset -Name "linux-debug-mounted"),
        (New-MountedTestPreset -Name "linux-release-mounted"),
        (New-MountedTestPreset -Name "linux-asan-mounted"),
        (New-MountedTestPreset -Name "linux-tsan-mounted")
    )
}

$json = $presetData | ConvertTo-Json -Depth 20
Set-Content -LiteralPath $userPresetsPath -Value ($json + [Environment]::NewLine) -Encoding utf8

if (-not $Quiet) {
    Write-Host "[sugar-env] Wrote $userPresetsPath"
    Write-Host "[sugar-env] Mounted WSL source root: $mountedSourceRoot"
}
