[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Preset,

    [string]$Target,

    [switch]$Test,

    [string]$TestRegex,

    [string]$LabelRegex,

    [int]$Jobs = 0,

    [switch]$Reconfigure,

    [switch]$ShutdownWslAfter
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot

$windowsPresets = @(
    "windows-msvc-debug",
    "windows-msvc-release"
)

$wslPresetMap = @{
    "linux-debug"                = "linux-debug-mounted"
    "linux-release"              = "linux-release-mounted"
    "linux-asan"                 = "linux-asan-mounted"
    "linux-tsan"                 = "linux-tsan-mounted"
    "linux-debug-mounted"        = "linux-debug-mounted"
    "linux-release-mounted"      = "linux-release-mounted"
    "linux-asan-mounted"         = "linux-asan-mounted"
    "linux-tsan-mounted"         = "linux-tsan-mounted"
}

$buildDirMap = @{
    "windows-msvc-debug"          = "out/build/windows-msvc"
    "windows-msvc-release"        = "out/build/windows-msvc"
    "linux-debug"                 = "out/build/wsl-gcc-debug"
    "linux-release"               = "out/build/wsl-gcc-release"
    "linux-asan"                  = "out/build/wsl-clang-asan"
    "linux-tsan"                  = "out/build/wsl-clang-tsan"
    "linux-debug-mounted"         = "out/build/wsl-gcc-debug"
    "linux-release-mounted"       = "out/build/wsl-gcc-release"
    "linux-asan-mounted"          = "out/build/wsl-clang-asan"
    "linux-tsan-mounted"          = "out/build/wsl-clang-tsan"
}

function Quote-BashLiteral {
    param([Parameter(Mandatory = $true)][string]$Value)
    return "'" + ($Value -replace "'", "'""'""'") + "'"
}

function Resolve-VsDevCmd {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path -LiteralPath $vswhere) {
        $installationPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($LASTEXITCODE -eq 0 -and -not [string]::IsNullOrWhiteSpace($installationPath)) {
            $candidate = Join-Path $installationPath.Trim() "Common7\Tools\VsDevCmd.bat"
            if (Test-Path -LiteralPath $candidate) {
                return $candidate
            }
        }
    }

    $fallbacks = @(
        "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\Common7\Tools\VsDevCmd.bat",
        "C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\Tools\VsDevCmd.bat",
        "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat",
        "C:\Program Files\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat"
    )

    foreach ($candidate in $fallbacks) {
        if (Test-Path -LiteralPath $candidate) {
            return $candidate
        }
    }

    throw "Could not locate VsDevCmd.bat. Install Visual Studio Build Tools or run from an existing Developer PowerShell."
}

function Enter-VsDevShellEnvironment {
    param(
        [Parameter(Mandatory = $true)][string]$VsDevCmdPath,
        [Parameter(Mandatory = $true)][string]$Arch
    )

    $shellArch = if ($Arch -eq "x64") { "amd64" } else { "arm64" }
    $cmdLine = "`"$VsDevCmdPath`" -arch=$shellArch -host_arch=amd64 -no_logo && set"
    $environmentLines = & $env:ComSpec /s /c $cmdLine
    if ($LASTEXITCODE -ne 0) {
        throw "VsDevCmd.bat failed for arch '$shellArch'."
    }

    foreach ($line in $environmentLines) {
        $separator = $line.IndexOf("=")
        if ($separator -le 0) {
            continue
        }

        $name = $line.Substring(0, $separator)
        $value = $line.Substring($separator + 1)
        [Environment]::SetEnvironmentVariable($name, $value, "Process")
    }
}

function Convert-WindowsPathToWsl {
    param([Parameter(Mandatory = $true)][string]$WindowsPath)
    $fullPath = [System.IO.Path]::GetFullPath($WindowsPath)
    $drive = [char]::ToLowerInvariant($fullPath[0])
    $suffix = $fullPath.Substring(2).Replace("\", "/")
    return "/mnt/$drive$suffix"
}

function Invoke-WslCommand {
    param([Parameter(Mandatory = $true)][string]$Command)
    & wsl bash -lc $Command
    if ($LASTEXITCODE -ne 0) {
        throw ("WSL command failed with exit code {0}: {1}" -f $LASTEXITCODE, $Command)
    }
}

function Get-BuildDirForPreset {
    param([Parameter(Mandatory = $true)][string]$PresetName)
    if (-not $buildDirMap.ContainsKey($PresetName)) {
        throw "No build directory mapping is defined for preset '$PresetName'."
    }

    return Join-Path $repoRoot $buildDirMap[$PresetName]
}

function Get-CMakeCacheValue {
    param(
        [Parameter(Mandatory = $true)][string]$CachePath,
        [Parameter(Mandatory = $true)][string]$Name
    )

    if (-not (Test-Path -LiteralPath $CachePath)) {
        return $null
    }

    $prefix = $Name + ":"
    foreach ($line in Get-Content -LiteralPath $CachePath) {
        if ($line.StartsWith($prefix)) {
            $separator = $line.IndexOf("=")
            if ($separator -gt 0) {
                return $line.Substring($separator + 1)
            }
        }
    }

    return $null
}

function ConvertTo-CMakeComparablePath {
    param([AllowNull()][string]$PathValue)
    if ([string]::IsNullOrWhiteSpace($PathValue)) {
        return ""
    }

    return $PathValue.Replace("\", "/").TrimEnd("/").ToLowerInvariant()
}

function Test-CMakeCacheMatches {
    param(
        [Parameter(Mandatory = $true)][string]$CachePath,
        [Parameter(Mandatory = $true)][string]$ExpectedSourceDir,
        [Parameter(Mandatory = $true)][string]$ExpectedBuildDir
    )

    $actualSourceDir = Get-CMakeCacheValue -CachePath $CachePath -Name "CMAKE_HOME_DIRECTORY"
    $actualBuildDir = Get-CMakeCacheValue -CachePath $CachePath -Name "CMAKE_CACHEFILE_DIR"
    if ([string]::IsNullOrWhiteSpace($actualSourceDir) -or [string]::IsNullOrWhiteSpace($actualBuildDir)) {
        return $false
    }

    $expectedSource = ConvertTo-CMakeComparablePath -PathValue $ExpectedSourceDir
    $expectedBuild = ConvertTo-CMakeComparablePath -PathValue $ExpectedBuildDir
    $actualSource = ConvertTo-CMakeComparablePath -PathValue $actualSourceDir
    $actualBuild = ConvertTo-CMakeComparablePath -PathValue $actualBuildDir

    return $actualSource -eq $expectedSource -and $actualBuild -eq $expectedBuild
}

function Update-MountedUserPresets {
    $initScript = Join-Path $PSScriptRoot "Initialize-SugarBuildEnvironment.ps1"
    if (-not (Test-Path -LiteralPath $initScript)) {
        throw "Could not locate $initScript."
    }

    & $initScript -Quiet
}

if ($Jobs -lt 0) {
    throw "-Jobs must be zero or a positive integer."
}

if (-not $Test -and (-not [string]::IsNullOrWhiteSpace($TestRegex) -or -not [string]::IsNullOrWhiteSpace($LabelRegex))) {
    $Test = $true
}

Push-Location $repoRoot
try {
    if ($windowsPresets -contains $Preset) {
        $vsDevCmd = Resolve-VsDevCmd
        $arch = "x64"
        $buildDir = Get-BuildDirForPreset -PresetName $Preset
        $cachePath = Join-Path $buildDir "CMakeCache.txt"

        Write-Host "[sugar-preset] Windows preset: $Preset"
        Enter-VsDevShellEnvironment -VsDevCmdPath $vsDevCmd -Arch $arch
        Set-Location -LiteralPath $repoRoot

        $cacheMatches = Test-CMakeCacheMatches -CachePath $cachePath -ExpectedSourceDir $repoRoot -ExpectedBuildDir $buildDir
        if ($Reconfigure -or -not $cacheMatches) {
            $configureArgs = @("--preset", $Preset)
            if ((Test-Path -LiteralPath $cachePath) -and -not $cacheMatches) {
                Write-Host "[sugar-preset] Existing CMake cache does not match this repo path; reconfiguring with --fresh."
                $configureArgs = @("--fresh") + $configureArgs
            }
            & cmake @configureArgs
            if ($LASTEXITCODE -ne 0) {
                throw "cmake --preset $Preset failed."
            }
        }

        $buildArgs = @("--build", "--preset", $Preset)
        if ($Jobs -gt 0) {
            Write-Host "[sugar-preset] Ignoring -Jobs for Windows MSVC preset '$Preset'; the preset pins MSBuild to a single node for reliability."
        }
        if (-not [string]::IsNullOrWhiteSpace($Target)) {
            $buildArgs += @("--target", $Target)
        }
        & cmake @buildArgs
        if ($LASTEXITCODE -ne 0) {
            throw "cmake --build --preset $Preset failed."
        }

        if ($Test) {
            $testArgs = @("--preset", $Preset, "--output-on-failure")
            if (-not [string]::IsNullOrWhiteSpace($TestRegex)) {
                $testArgs += @("-R", $TestRegex)
            }
            if (-not [string]::IsNullOrWhiteSpace($LabelRegex)) {
                $testArgs += @("-L", $LabelRegex)
            }
            & ctest @testArgs
            if ($LASTEXITCODE -ne 0) {
                throw "ctest --preset $Preset failed."
            }
        }
        return
    }

    if ($wslPresetMap.ContainsKey($Preset)) {
        Update-MountedUserPresets

        $effectivePreset = $wslPresetMap[$Preset]
        $wslRepoRoot = Convert-WindowsPathToWsl -WindowsPath $repoRoot
        $buildDir = Get-BuildDirForPreset -PresetName $effectivePreset
        $wslBuildDir = Convert-WindowsPathToWsl -WindowsPath $buildDir
        $cachePath = Join-Path $buildDir "CMakeCache.txt"

        $steps = @()
        $steps += ("cd " + (Quote-BashLiteral $wslRepoRoot))
        $cacheMatches = Test-CMakeCacheMatches -CachePath $cachePath -ExpectedSourceDir $wslRepoRoot -ExpectedBuildDir $wslBuildDir
        if ($Reconfigure -or -not $cacheMatches) {
            $configureStep = "cmake --preset " + (Quote-BashLiteral $effectivePreset)
            if ((Test-Path -LiteralPath $cachePath) -and -not $cacheMatches) {
                Write-Host "[sugar-preset] Existing CMake cache does not match this repo path; reconfiguring with --fresh."
                $configureStep = "cmake --fresh --preset " + (Quote-BashLiteral $effectivePreset)
            }
            $steps += $configureStep
        }

        $buildStep = "cmake --build --preset " + (Quote-BashLiteral $effectivePreset)
        if ($Jobs -gt 0) {
            $buildStep += " --parallel $Jobs"
        }
        if (-not [string]::IsNullOrWhiteSpace($Target)) {
            $buildStep += " --target " + (Quote-BashLiteral $Target)
        }
        $steps += $buildStep

        if ($Test) {
            $testStep = "ctest --preset " + (Quote-BashLiteral $effectivePreset) + " --output-on-failure"
            if (-not [string]::IsNullOrWhiteSpace($TestRegex)) {
                $testStep += " -R " + (Quote-BashLiteral $TestRegex)
            }
            if (-not [string]::IsNullOrWhiteSpace($LabelRegex)) {
                $testStep += " -L " + (Quote-BashLiteral $LabelRegex)
            }
            $steps += $testStep
        }

        $command = $steps -join " && "
        Write-Host "[sugar-preset] WSL preset request: $Preset"
        if ($effectivePreset -ne $Preset) {
            Write-Host "[sugar-preset] Using mounted preset on Windows host: $effectivePreset"
        }
        Invoke-WslCommand -Command $command

        if ($ShutdownWslAfter) {
            Write-Host "[sugar-preset] Shutting down WSL after command completion"
            & wsl --shutdown
            if ($LASTEXITCODE -ne 0) {
                throw "wsl --shutdown failed with exit code $LASTEXITCODE."
            }
        }
        return
    }

    throw "Unsupported preset '$Preset'. Use windows-msvc-debug/windows-msvc-release or a Linux/WSL preset from CMakePresets.json/CMakeUserPresets.json."
}
finally {
    Pop-Location
}
