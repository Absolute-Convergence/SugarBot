<#
.SYNOPSIS
Runs the SugarBot release smoke path against an existing binary directory.

.DESCRIPTION
Verifies the expected binary artifact root, help output, a local sugarbot run,
and a hosted sugar_server/sugar_client loopback run using a checked-in demo
layout plus a lightweight built-in template. Logs are kept under
.sandbox/release_smoke.

.EXAMPLE
.\tools\Run-ReleaseSmoke.ps1 -BinDir out\build\windows-msvc\Debug
#>
[CmdletBinding()]
param(
    [string]$RepoRoot,
    [string]$BinDir,
    [int]$Port = 0,
    [switch]$SkipLocal,
    [switch]$SkipHosted
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Resolve-ScriptPath {
    param([string]$Path)
    return (Resolve-Path -LiteralPath $Path).Path
}

if ([string]::IsNullOrWhiteSpace($RepoRoot)) {
    $RepoRoot = Resolve-ScriptPath (Join-Path $PSScriptRoot "..")
}
else {
    $RepoRoot = Resolve-ScriptPath $RepoRoot
}

if ([string]::IsNullOrWhiteSpace($BinDir)) {
    $BinDir = Join-Path $RepoRoot "out\build\windows-msvc\Debug"
}
if (-not (Test-Path -LiteralPath $BinDir -PathType Container)) {
    throw "Binary directory not found: $BinDir. Build first or pass -BinDir."
}
$BinDir = Resolve-ScriptPath $BinDir

$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$SmokeRoot = Join-Path $RepoRoot ".sandbox\release_smoke\$stamp"
New-Item -ItemType Directory -Force -Path $SmokeRoot | Out-Null

function Write-Smoke {
    param([string]$Message)
    Write-Host "[smoke] $Message"
}

function Convert-ToSafeName {
    param([string]$Name)
    return ($Name -replace "[^A-Za-z0-9_.-]", "_")
}

function Resolve-SmokeBinary {
    param([string]$Name)

    $candidates = @(
        (Join-Path $BinDir "$Name.exe"),
        (Join-Path $BinDir $Name)
    )
    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return Resolve-ScriptPath $candidate
        }
    }
    throw "Required binary not found in ${BinDir}: $Name"
}

function Assert-PathExists {
    param(
        [string]$Path,
        [string]$Label,
        [string]$PathType = "Any"
    )

    $exists = if ($PathType -eq "Leaf") {
        Test-Path -LiteralPath $Path -PathType Leaf
    }
    elseif ($PathType -eq "Container") {
        Test-Path -LiteralPath $Path -PathType Container
    }
    else {
        Test-Path -LiteralPath $Path
    }
    if (-not $exists) {
        throw "Missing ${Label}: $Path"
    }
    Write-Smoke "PASS $Label"
}

function Assert-Contains {
    param(
        [string]$Text,
        [string]$Needle,
        [string]$Label
    )

    if ($Text -notlike "*$Needle*") {
        throw "Expected '$Needle' in $Label"
    }
    Write-Smoke "PASS $Label"
}

function Invoke-SmokeCommand {
    param(
        [string]$Name,
        [string]$FilePath,
        [string[]]$Arguments,
        [switch]$AllowFailure,
        [switch]$Quiet
    )

    $safe = Convert-ToSafeName $Name
    $stdoutPath = Join-Path $SmokeRoot "$safe.out.txt"
    $stderrPath = Join-Path $SmokeRoot "$safe.err.txt"

    if (-not $Quiet) {
        Write-Smoke "RUN $Name"
    }

    & $FilePath @Arguments > $stdoutPath 2> $stderrPath
    $exitCode = $LASTEXITCODE
    $stdout = if (Test-Path -LiteralPath $stdoutPath) {
        Get-Content -LiteralPath $stdoutPath -Raw
    }
    else {
        ""
    }
    $stderr = if (Test-Path -LiteralPath $stderrPath) {
        Get-Content -LiteralPath $stderrPath -Raw
    }
    else {
        ""
    }

    if ($exitCode -ne 0 -and -not $AllowFailure) {
        throw "$Name failed with exit code $exitCode. stdout: $stdoutPath stderr: $stderrPath`n$stderr"
    }

    if (-not $Quiet) {
        Write-Smoke "PASS $Name"
    }

    [pscustomobject]@{
        ExitCode = $exitCode
        Stdout = $stdout
        Stderr = $stderr
        StdoutPath = $stdoutPath
        StderrPath = $stderrPath
    }
}

function Get-FreeLoopbackPort {
    $listener = [System.Net.Sockets.TcpListener]::new(
        [System.Net.IPAddress]::Parse("127.0.0.1"),
        0)
    $listener.Start()
    try {
        return [int]$listener.LocalEndpoint.Port
    }
    finally {
        $listener.Stop()
    }
}

function Wait-ForHostedServer {
    param(
        [string]$ClientExe,
        [int]$ServerPort
    )

    for ($attempt = 1; $attempt -le 30; ++$attempt) {
        $probe = Invoke-SmokeCommand `
            -Name "hosted_wait_connect_$attempt" `
            -FilePath $ClientExe `
            -Arguments @("connect", "--port", "$ServerPort", "--socket-timeout-ms", "1000", "--compact") `
            -AllowFailure `
            -Quiet
        if ($probe.ExitCode -eq 0) {
            try {
                $json = $probe.Stdout | ConvertFrom-Json
                if ($json.ok -and -not [string]::IsNullOrWhiteSpace($json.session_id)) {
                    Invoke-SmokeCommand `
                        -Name "hosted_wait_disconnect_$attempt" `
                        -FilePath $ClientExe `
                        -Arguments @("disconnect", "--port", "$ServerPort", "--session", $json.session_id, "--socket-timeout-ms", "1000", "--compact") `
                        -AllowFailure `
                        -Quiet | Out-Null
                    Write-Smoke "PASS hosted server accepted loopback connection"
                    return
                }
            }
            catch {
                # Keep polling until the server returns parseable hosted JSON.
            }
        }
        Start-Sleep -Milliseconds 250
    }

    throw "Timed out waiting for sugar_server on 127.0.0.1:$ServerPort"
}

Write-Smoke "repo root: $RepoRoot"
Write-Smoke "binary root: $BinDir"
Write-Smoke "logs: $SmokeRoot"

$SugarbotExe = Resolve-SmokeBinary "sugarbot"
$SbExe = Resolve-SmokeBinary "SB"
$ContractsExe = Resolve-SmokeBinary "sugar_contracts"
$ServerExe = Resolve-SmokeBinary "sugar_server"
$ClientExe = Resolve-SmokeBinary "sugar_client"

Assert-PathExists -Path $BinDir -Label "binary artifact root" -PathType Container
Assert-PathExists -Path $SugarbotExe -Label "sugarbot binary" -PathType Leaf
Assert-PathExists -Path $SbExe -Label "SB launcher binary" -PathType Leaf
Assert-PathExists -Path $ContractsExe -Label "sugar_contracts binary" -PathType Leaf
Assert-PathExists -Path $ServerExe -Label "sugar_server binary" -PathType Leaf
Assert-PathExists -Path $ClientExe -Label "sugar_client binary" -PathType Leaf

$layoutPath = Join-Path $RepoRoot "graphs\demos\nvda_momentum_volatility.layout.json"
$logicPath = Join-Path $RepoRoot "graphs\templates\gated_diff_cross.json"
$dataPath = Join-Path $RepoRoot "data\BATS_NVDA, 1D.csv"
Assert-PathExists -Path $layoutPath -Label "example layout" -PathType Leaf
Assert-PathExists -Path $logicPath -Label "example logic" -PathType Leaf
Assert-PathExists -Path $dataPath -Label "example data" -PathType Leaf

$help = Invoke-SmokeCommand -Name "sugarbot_help" -FilePath $SugarbotExe -Arguments @("--help")
Assert-Contains -Text $help.Stdout -Needle "sugarbot run <layout.json> <logic.json> <data.csv>" -Label "sugarbot help documents local run"

$clientHelp = Invoke-SmokeCommand -Name "sugar_client_help" -FilePath $ClientExe -Arguments @("--help")
Assert-Contains -Text $clientHelp.Stdout -Needle "sugar_client run --layout" -Label "sugar_client help documents hosted run"

$serverHelp = Invoke-SmokeCommand -Name "sugar_server_help" -FilePath $ServerExe -Arguments @("--help")
Assert-Contains -Text $serverHelp.Stdout -Needle "--service-profile" -Label "sugar_server help documents service profiles"

if (-not $SkipLocal) {
    $localRun = Invoke-SmokeCommand `
        -Name "sugarbot_local_run" `
        -FilePath $SugarbotExe `
        -Arguments @("run", $layoutPath, $logicPath, $dataPath)
    Assert-Contains -Text $localRun.Stdout -Needle "PnL:" -Label "local run emits execution metrics"
    Assert-Contains -Text $localRun.Stdout -Needle "Run provenance" -Label "local run emits provenance"
}
else {
    Write-Smoke "SKIP local CLI run"
}

if (-not $SkipHosted) {
    if ($Port -le 0) {
        $Port = Get-FreeLoopbackPort
    }

    $hostedStorage = Join-Path $SmokeRoot "hosted_storage"
    New-Item -ItemType Directory -Force -Path $hostedStorage | Out-Null
    $serverOut = Join-Path $SmokeRoot "sugar_server.out.txt"
    $serverErr = Join-Path $SmokeRoot "sugar_server.err.txt"
    $server = $null

    try {
        Write-Smoke "RUN sugar_server on 127.0.0.1:$Port"
        $server = Start-Process `
            -FilePath $ServerExe `
            -ArgumentList @("--port", "$Port", "--workers", "2", "--storage-root", $hostedStorage, "--source-root", $RepoRoot) `
            -PassThru `
            -WindowStyle Hidden `
            -RedirectStandardOutput $serverOut `
            -RedirectStandardError $serverErr

        Wait-ForHostedServer -ClientExe $ClientExe -ServerPort $Port

        $hostedRun = Invoke-SmokeCommand `
            -Name "sugar_client_hosted_run" `
            -FilePath $ClientExe `
            -Arguments @(
                "run",
                "--port", "$Port",
                "--layout", $layoutPath,
                "--logic", $logicPath,
                "--data", $dataPath,
                "--poll-ms", "50",
                "--wait-timeout-ms", "60000",
                "--compact"
            )
        $hostedJson = $hostedRun.Stdout | ConvertFrom-Json
        if (-not $hostedJson.ok -or $hostedJson.op -ne "retrieve_results") {
            throw "Hosted run did not return an ok retrieve_results payload. stdout: $($hostedRun.StdoutPath)"
        }
        if ($null -eq $hostedJson.job_result -or $null -eq $hostedJson.job_result.metadata) {
            throw "Hosted run did not return retained job_result metadata. stdout: $($hostedRun.StdoutPath)"
        }
        Write-Smoke "PASS hosted run returns final result JSON"

        $storedItems = Get-ChildItem -LiteralPath $hostedStorage -Recurse -Force -ErrorAction SilentlyContinue
        if ($storedItems.Count -eq 0) {
            throw "Hosted storage root is empty after hosted run: $hostedStorage"
        }
        Write-Smoke "PASS hosted storage root contains artifacts"
    }
    finally {
        if ($null -ne $server -and -not $server.HasExited) {
            Stop-Process -Id $server.Id -Force
            $server.WaitForExit(5000) | Out-Null
        }
    }
}
else {
    Write-Smoke "SKIP hosted loopback run"
}

Write-Smoke "PASS release smoke complete"
Write-Smoke "logs kept at $SmokeRoot"
