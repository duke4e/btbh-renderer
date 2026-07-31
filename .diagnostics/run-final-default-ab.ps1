[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $GameRoot,

    [Parameter(Mandatory = $true)]
    [string] $OutputRoot,

    [ValidateRange(1, 100)]
    [int] $RunsPerBackend = 10,

    [ValidateRange(120, 3600)]
    [int] $ProfileFrames = 360,

    [ValidateRange(0, 20)]
    [int] $WarmupWindows = 1,

    [ValidateRange(0, 60)]
    [int] $CooldownSeconds = 10,

    [ValidateSet("display-refresh", "uncapped")]
    [string] $FrameRateMode = "display-refresh",

    [switch] $HighPriority
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$framesPerTelemetryWindow = 60
if (($ProfileFrames % $framesPerTelemetryWindow) -ne 0) {
    throw "ProfileFrames must be divisible by $framesPerTelemetryWindow"
}
$warmupFrames = $WarmupWindows * $framesPerTelemetryWindow
$measurementFrameCount = $ProfileFrames - $warmupFrames
if ($measurementFrameCount -lt 1) {
    throw "WarmupWindows must leave at least one measured frame"
}
$expectedPhaseWindows = 1
$measurementWindowCount = 1

function Get-LowerMedian {
    param([double[]] $Values)

    if ($Values.Count -eq 0) {
        throw "Cannot compute a median from an empty sample"
    }
    $sorted = @($Values | Sort-Object)
    return [double] $sorted[[Math]::Floor(($sorted.Count - 1) / 2)]
}

function Convert-PhaseLine {
    param([string] $Line)

    $record = @{}
    foreach ($token in ($Line -split " ")) {
        if ($token -match "^(?<name>[a-z0-9_]+)=(?<value>[0-9]+(?:\.[0-9]+)?)$") {
            $record[$Matches.name] = [double]::Parse(
                $Matches.value,
                [Globalization.CultureInfo]::InvariantCulture
            )
        }
    }
    return $record
}

function Convert-KeyValueLine {
    param([string] $Line)

    $record = @{}
    foreach ($token in ($Line -split " ")) {
        if ($token -match "^(?<name>[a-z0-9_]+)=(?<value>[^ ]+)$") {
            $record[$Matches.name] = $Matches.value
        }
    }
    return $record
}

function New-GameProcessStartInfo {
    param(
        [string] $Executable,
        [string] $WorkingDirectory,
        [ValidateSet("gpu", "legacy")]
        [string] $Backend,
        [int] $Frames,
        [ValidateSet("display-refresh", "uncapped")]
        [string] $RateMode
    )

    $startInfo = [Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $Executable
    $startInfo.WorkingDirectory = $WorkingDirectory
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $false
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $startInfo.ArgumentList.Add("--game")
    $startInfo.ArgumentList.Add("--projectile-shadows=enabled")

    foreach ($name in @($startInfo.Environment.Keys)) {
        if ($name.StartsWith("BTBH_", [StringComparison]::OrdinalIgnoreCase)) {
            $startInfo.Environment.Remove($name) | Out-Null
        }
    }

    $startInfo.Environment["PATH"] = (
        (Join-Path $WorkingDirectory "lib") + ";" +
        $startInfo.Environment["PATH"]
    )
    $startInfo.Environment["BTBH_RENDER_BACKEND"] = $Backend
    $startInfo.Environment["BTBH_GAMEPLAY_PHASE_LOG"] = "1"
    $startInfo.Environment["BTBH_GAMEPLAY_PHASE_WARMUP_FRAMES"] =
        "$warmupFrames"
    $startInfo.Environment["BTBH_GAMEPLAY_PHASE_REPORT_FRAMES"] =
        "$measurementFrameCount"
    $startInfo.Environment["BTBH_PROFILE_SEED"] = "424242"
    $startInfo.Environment["BTBH_START_PROFILE_FRAME"] = "courser_frame"
    $startInfo.Environment["BTBH_START_PROFILE_FRAMES"] = "$Frames"
    if ($RateMode -eq "uncapped") {
        $startInfo.Environment["BTBH_START_PROFILE_UNCAPPED"] = "1"
    }
    $startInfo.Environment["BTBH_RENDER_PERF_LOG"] = "0"
    $startInfo.Environment["BTBH_RENDER_DIAGNOSTICS"] = "0"
    return $startInfo
}

function Invoke-GameRun {
    param(
        [ValidateSet("gpu", "legacy")]
        [string] $Backend,
        [string] $Label,
        [string] $RunDirectory,
        [bool] $Scored
    )

    if (Get-Process -Name "Main" -ErrorAction SilentlyContinue) {
        throw "A Main.exe process is already running before $Label"
    }

    New-Item -ItemType Directory -Path $RunDirectory | Out-Null
    $stdoutPath = Join-Path $RunDirectory "stdout.log"
    $stderrPath = Join-Path $RunDirectory "stderr.log"
    $startInfo = New-GameProcessStartInfo `
        -Executable $executable `
        -WorkingDirectory $resolvedGameRoot `
        -Backend $Backend `
        -Frames $ProfileFrames `
        -RateMode $FrameRateMode

    $startedAt = [DateTimeOffset]::Now
    Write-Host "AB_START label=$Label backend=$Backend scored=$Scored"
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $startInfo
    if (-not $process.Start()) {
        throw "Could not launch Main.exe for $Label"
    }
    if ($HighPriority) {
        $process.PriorityClass = [Diagnostics.ProcessPriorityClass]::High
    }

    $stdoutTask = $process.StandardOutput.ReadToEndAsync()
    $stderrTask = $process.StandardError.ReadToEndAsync()
    $process.WaitForExit()
    $stdout = $stdoutTask.GetAwaiter().GetResult()
    $stderr = $stderrTask.GetAwaiter().GetResult()
    [IO.File]::WriteAllText($stdoutPath, $stdout)
    [IO.File]::WriteAllText($stderrPath, $stderr)
    $exitCode = $process.ExitCode
    $process.Dispose()

    if (Get-Process -Name "Main" -ErrorAction SilentlyContinue) {
        throw "A Main.exe process remained after $Label"
    }

    $phaseLines = @(
        $stdout -split "`r?`n" |
            Where-Object {
                $_ -match (
                    "^BTBH_GAMEPLAY_PHASE_METRICS frames=" +
                    "$measurementFrameCount "
                )
            }
    )
    $records = @($phaseLines | ForEach-Object { Convert-PhaseLine $_ })
    $selected = @($records)
    $finishLines = @(
        $stdout -split "`r?`n" |
            Where-Object { $_ -match "^BTBH_START_PROFILE_FINISHED " }
    )
    $finishRecord = if ($finishLines.Count -eq 1) {
        Convert-KeyValueLine $finishLines[0]
    } else {
        @{}
    }
    $errorPattern = (
        "BTBH_(?:LAUNCH|START_PROFILE|PROFILE)_FAILED|" +
        "BTBH_GPU_[A-Z_]*(?:FAILED|ERROR)|" +
        "fatal|device lost|validation error|assertion failed"
    )
    $errors = @(
        ($stdout + "`n" + $stderr) -split "`r?`n" |
            Where-Object { $_ -match $errorPattern }
    )
    $invalidWindows = @(
        $selected | Where-Object {
            -not $_.ContainsKey("scene_submit_median_ms") -or
            -not $_.ContainsKey("update_median_ms") -or
            -not $_.ContainsKey("cadence_median_ms") -or
            -not $_.ContainsKey("cadence_p95_ms") -or
            $_["scene_submit_median_ms"] -le 0 -or
            $_["update_median_ms"] -lt 0.05 -or
            $_["cadence_median_ms"] -le 0 -or
            $_["cadence_p95_ms"] -le 0
        }
    )

    $finishContractValid = (
        $finishLines.Count -eq 1 -and
        $finishRecord.ContainsKey("seed") -and
        $finishRecord["seed"] -eq "424242" -and
        $finishRecord.ContainsKey("frame") -and
        $finishRecord["frame"] -eq "courser_frame" -and
        $finishRecord.ContainsKey("backend") -and
        $finishRecord["backend"] -eq $Backend -and
        $finishRecord.ContainsKey("completed_frames") -and
        $finishRecord["completed_frames"] -eq "$ProfileFrames"
    )
    $signatureKeys = @(
        "seed",
        "frame",
        "completed_frames",
        "simulated_seconds",
        "growth",
        "projectiles",
        "projectile_proxies",
        "beams",
        "fields",
        "effects",
        "enemies",
        "hostile_projectiles",
        "active_hazards"
    )
    $signatureComplete = $finishContractValid
    foreach ($key in $signatureKeys) {
        if (-not $finishRecord.ContainsKey($key)) {
            $signatureComplete = $false
        }
    }
    $workloadSignature = if ($signatureComplete) {
        [string]::Join(" ", @(
            $signatureKeys | ForEach-Object { "$_=$($finishRecord[$_])" }
        ))
    } else {
        ""
    }
    $wallSeconds = if ($finishRecord.ContainsKey("wall_seconds")) {
        [double]::Parse(
            $finishRecord["wall_seconds"],
            [Globalization.CultureInfo]::InvariantCulture
        )
    } else {
        [double]::NaN
    }
    $wallMillisecondsPerFrame = if ($finishRecord.ContainsKey("wall_ms_per_frame")) {
        [double]::Parse(
            $finishRecord["wall_ms_per_frame"],
            [Globalization.CultureInfo]::InvariantCulture
        )
    } else {
        [double]::NaN
    }
    $wallFPS = if ($finishRecord.ContainsKey("wall_fps")) {
        [double]::Parse(
            $finishRecord["wall_fps"],
            [Globalization.CultureInfo]::InvariantCulture
        )
    } else {
        [double]::NaN
    }
    $finishTimingValid =
        $wallSeconds -gt 0 -and
        $wallMillisecondsPerFrame -gt 0 -and
        $wallFPS -gt 0

    $valid = (
        $exitCode -eq 0 -and
        $records.Count -eq $expectedPhaseWindows -and
        $selected.Count -eq $measurementWindowCount -and
        $invalidWindows.Count -eq 0 -and
        $errors.Count -eq 0 -and
        $signatureComplete -and
        $finishTimingValid
    )
    $activeMedian = if ($selected.Count -eq $measurementWindowCount) {
        Get-LowerMedian @($selected | ForEach-Object { $_["active_median_ms"] })
    } else {
        [double]::NaN
    }
    $activeP95 = if ($selected.Count -eq $measurementWindowCount) {
        Get-LowerMedian @($selected | ForEach-Object { $_["active_p95_ms"] })
    } else {
        [double]::NaN
    }
    $cadenceMedian = if ($selected.Count -eq $measurementWindowCount) {
        Get-LowerMedian @($selected | ForEach-Object { $_["cadence_median_ms"] })
    } else {
        [double]::NaN
    }
    $cadenceP95 = if ($selected.Count -eq $measurementWindowCount) {
        Get-LowerMedian @($selected | ForEach-Object { $_["cadence_p95_ms"] })
    } else {
        [double]::NaN
    }

    $result = [pscustomobject]@{
        label = $Label
        backend = $Backend
        frame_rate_mode = $FrameRateMode
        actual_backend = if ($finishRecord.ContainsKey("backend")) { $finishRecord["backend"] } else { "" }
        scored = $Scored
        started_at = $startedAt.ToString("o")
        exit_code = $exitCode
        phase_windows = $records.Count
        warmup_windows = $WarmupWindows
        warmup_frames = $warmupFrames
        measured_frames = $measurementFrameCount
        selected_windows = $selected.Count
        invalid_windows = $invalidWindows.Count
        finish_records = $finishLines.Count
        error_lines = $errors.Count
        active_median_ms = $activeMedian
        active_p95_ms = $activeP95
        cadence_median_ms = $cadenceMedian
        cadence_p95_ms = $cadenceP95
        effective_fps = if ($cadenceMedian -gt 0) { 1000.0 / $cadenceMedian } else { [double]::NaN }
        wall_seconds = $wallSeconds
        wall_ms_per_frame = $wallMillisecondsPerFrame
        wall_fps = $wallFPS
        workload_signature = $workloadSignature
        valid = $valid
        stdout = $stdoutPath
        stderr = $stderrPath
    }
    $result | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $RunDirectory "result.json")
    Write-Host (
        "AB_RESULT label={0} backend={1} valid={2} windows={3}/{4} median_ms={5:F3} p95_ms={6:F3} fps={7:F1}" -f
        $Label,
        $Backend,
        $valid,
        $selected.Count,
        $records.Count,
        $cadenceMedian,
        $cadenceP95,
        $result.effective_fps
    )
    if (-not $valid) {
        throw "Invalid benchmark run $Label; preserve its logs and rerun the full pair"
    }
    return $result
}

$resolvedGameRoot = (Resolve-Path -LiteralPath $GameRoot).Path
$executable = Join-Path $resolvedGameRoot ".build\release\Main.exe"
if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
    throw "Release executable not found: $executable"
}
if (Test-Path -LiteralPath $OutputRoot) {
    throw "Output directory already exists: $OutputRoot"
}
if (Get-Process -Name "Main" -ErrorAction SilentlyContinue) {
    throw "A Main.exe process is already running"
}

New-Item -ItemType Directory -Path $OutputRoot | Out-Null
$resolvedOutputRoot = (Resolve-Path -LiteralPath $OutputRoot).Path
$manifest = [ordered]@{
    started_at = [DateTimeOffset]::Now.ToString("o")
    game_root = $resolvedGameRoot
    executable = $executable
    executable_sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $executable).Hash
    game_branch = (& git -C $resolvedGameRoot branch --show-current).Trim()
    game_head = (& git -C $resolvedGameRoot rev-parse HEAD).Trim()
    renderer_branch = (& git -C (Join-Path $resolvedGameRoot "Vendor\BTBHRenderer") branch --show-current).Trim()
    renderer_head = (& git -C (Join-Path $resolvedGameRoot "Vendor\BTBHRenderer") rev-parse HEAD).Trim()
    runs_per_backend = $RunsPerBackend
    profile_frames = $ProfileFrames
    simulation_delta_seconds = 1.0 / 60.0
    telemetry_window_frames = $measurementFrameCount
    expected_phase_windows = $expectedPhaseWindows
    warmup_windows = $WarmupWindows
    warmup_frames = $warmupFrames
    measured_frames = $measurementFrameCount
    measured_windows = $measurementWindowCount
    cooldown_seconds = $CooldownSeconds
    frame_rate_mode = $FrameRateMode
    resolution = "1280x720-windowed"
    seed = 424242
    player_frame = "courser_frame"
    uncapped = $FrameRateMode -eq "uncapped"
    projectile_shadows = "enabled"
    city_chunk_override = $null
    gpu_shadow_period_override = $null
    gpu_frames_in_flight_override = $null
    gpu_shader_directory_override = $null
    render_perf_log = $false
    render_diagnostics = $false
    high_priority = [bool] $HighPriority
    selection_contract = "fixed-frame-after-warmup"
}
$manifest | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $resolvedOutputRoot "manifest.json")

$results = [Collections.Generic.List[object]]::new()
foreach ($backend in @("gpu", "legacy")) {
    $prewarm = Invoke-GameRun `
        -Backend $backend `
        -Label "prewarm-$backend" `
        -RunDirectory (Join-Path $resolvedOutputRoot "prewarm\$backend") `
        -Scored $false
    $results.Add($prewarm)
    if ($CooldownSeconds -gt 0) {
        Start-Sleep -Seconds $CooldownSeconds
    }
}

for ($pair = 1; $pair -le $RunsPerBackend; $pair++) {
    $order = if (($pair % 2) -eq 1) {
        @("gpu", "legacy")
    } else {
        @("legacy", "gpu")
    }
    foreach ($backend in $order) {
        $label = "pair{0:D2}-{1}" -f $pair, $backend
        $result = Invoke-GameRun `
            -Backend $backend `
            -Label $label `
            -RunDirectory (Join-Path $resolvedOutputRoot ("pairs\pair{0:D2}\{1}" -f $pair, $backend)) `
            -Scored $true
        $results.Add($result)
        $results | Export-Csv -NoTypeInformation -LiteralPath (Join-Path $resolvedOutputRoot "runs.csv")
        if ($CooldownSeconds -gt 0 -and -not ($pair -eq $RunsPerBackend -and $backend -eq $order[-1])) {
            Start-Sleep -Seconds $CooldownSeconds
        }
    }
}

$scored = @($results | Where-Object { $_.scored })
$workloadSignatures = @($scored.workload_signature | Sort-Object -Unique)
if ($workloadSignatures.Count -ne 1 -or [string]::IsNullOrWhiteSpace($workloadSignatures[0])) {
    throw "Scored runs did not produce one identical gameplay workload signature"
}

$summary = foreach ($backend in @("gpu", "legacy")) {
    $backendRuns = @($scored | Where-Object { $_.backend -eq $backend })
    [pscustomobject]@{
        backend = $backend
        runs = $backendRuns.Count
        aggregate_median_ms = Get-LowerMedian @($backendRuns.cadence_median_ms)
        median_run_p95_ms = Get-LowerMedian @($backendRuns.cadence_p95_ms)
        worst_run_p95_ms = ($backendRuns.cadence_p95_ms | Measure-Object -Maximum).Maximum
        mean_median_ms = ($backendRuns.cadence_median_ms | Measure-Object -Average).Average
        mean_p95_ms = ($backendRuns.cadence_p95_ms | Measure-Object -Average).Average
        min_median_ms = ($backendRuns.cadence_median_ms | Measure-Object -Minimum).Minimum
        max_median_ms = ($backendRuns.cadence_median_ms | Measure-Object -Maximum).Maximum
        min_p95_ms = ($backendRuns.cadence_p95_ms | Measure-Object -Minimum).Minimum
        max_p95_ms = ($backendRuns.cadence_p95_ms | Measure-Object -Maximum).Maximum
        aggregate_wall_ms_per_frame = Get-LowerMedian @($backendRuns.wall_ms_per_frame)
        aggregate_wall_fps = Get-LowerMedian @($backendRuns.wall_fps)
        aggregate_active_median_ms = Get-LowerMedian @($backendRuns.active_median_ms)
        aggregate_active_p95_ms = Get-LowerMedian @($backendRuns.active_p95_ms)
    }
}
$summary | Export-Csv -NoTypeInformation -LiteralPath (Join-Path $resolvedOutputRoot "summary.csv")
$summary | Format-Table | Out-String | Write-Output
Write-Output "AB_WORKLOAD_SIGNATURE $($workloadSignatures[0])"
Write-Output "AB_COMPLETE output=$resolvedOutputRoot"
