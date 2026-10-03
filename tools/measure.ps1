param(
    [string]$Executable = "$PSScriptRoot/../build/release/better-net.exe"
)

$ErrorActionPreference = 'Stop'
$executablePath = (Resolve-Path -LiteralPath $Executable).Path
$reportPath = Join-Path (Split-Path -Parent $executablePath) 'performance.json'

if (Get-Process -Name 'better-net' -ErrorAction SilentlyContinue)
{
    throw 'Exit better-net before measuring its startup and idle usage.'
}

$stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
$startOptions = @{
    FilePath = $executablePath
    ArgumentList = @('--diagnostics', ('"' + $reportPath + '"'))
    WindowStyle = 'Hidden'
    PassThru = $true
}
$process = Start-Process @startOptions

if (-not $process.WaitForInputIdle(10000))
{
    throw 'The launcher did not become interactive within ten seconds.'
}

$startupMilliseconds = $stopwatch.Elapsed.TotalMilliseconds
Start-Sleep -Milliseconds 1500
$process.Refresh()
$cpuBefore = $process.TotalProcessorTime.TotalMilliseconds
$sampleStart = [System.Diagnostics.Stopwatch]::StartNew()
Start-Sleep -Seconds 5
$process.Refresh()
$cpuMilliseconds = $process.TotalProcessorTime.TotalMilliseconds - $cpuBefore
$elapsedMilliseconds = $sampleStart.Elapsed.TotalMilliseconds
$workingSet = $process.WorkingSet64
$privateBytes = $process.PrivateMemorySize64

if (-not $process.WaitForExit(15000))
{
    throw 'The diagnostic launcher did not exit cleanly.'
}

if ($process.ExitCode -ne 0)
{
    throw "The diagnostic launcher exited with code $($process.ExitCode)."
}

$report = Get-Content -LiteralPath $reportPath -Raw | ConvertFrom-Json
$startupRounded = [Math]::Round($startupMilliseconds, 3)
$elapsedRounded = [Math]::Round($elapsedMilliseconds, 3)
$cpuPercent = [Math]::Round(100 * $cpuMilliseconds / $elapsedMilliseconds, 4)
$report | Add-Member -NotePropertyName startup_to_input_idle_ms -NotePropertyValue $startupRounded
$report | Add-Member -NotePropertyName idle_sample_ms -NotePropertyValue $elapsedRounded
$report | Add-Member -NotePropertyName idle_cpu_ms -NotePropertyValue $cpuMilliseconds
$report | Add-Member -NotePropertyName idle_one_core_percent -NotePropertyValue $cpuPercent
$report | Add-Member -NotePropertyName working_set_bytes -NotePropertyValue $workingSet
$report | Add-Member -NotePropertyName private_bytes -NotePropertyValue $privateBytes
$report | ConvertTo-Json | Set-Content -LiteralPath $reportPath -Encoding utf8
$report | ConvertTo-Json
