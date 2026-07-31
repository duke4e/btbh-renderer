[CmdletBinding()]
param(
    [string]$BuildDirectory = "",
    [string]$OutputDirectory = "",
    [uint32]$Width = 1920,
    [uint32]$Height = 1080,
    [uint32]$WarmupFrames = 5,
    [uint32]$MeasuredFrames = 30
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($BuildDirectory)) {
    $BuildDirectory = Join-Path $repoRoot "out/build/x64-gpu-debug"
}
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $repoRoot "out/captures/phase2-windows"
}

$metricsExecutable = Join-Path $BuildDirectory "bin/btbh_gpu_phase2_metrics.exe"
if (-not (Test-Path -LiteralPath $metricsExecutable -PathType Leaf)) {
    throw "Missing Phase 2 metrics executable: $metricsExecutable"
}
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null

function Invoke-BTBHPhase2Series {
    param([string]$Name)

    $capturePath = Join-Path $OutputDirectory "$Name.bmp"
    $output = & $metricsExecutable `
        $capturePath `
        $Width `
        $Height `
        $WarmupFrames `
        $MeasuredFrames
    if ($LASTEXITCODE -ne 0) {
        throw "Phase 2 capture series $Name failed with exit code $LASTEXITCODE"
    }
    $jsonLine = $output | Select-Object -Last 1
    $report = $jsonLine | ConvertFrom-Json
    $jsonPath = Join-Path $OutputDirectory "$Name.json"
    Set-Content -LiteralPath $jsonPath -Value $jsonLine -Encoding utf8
    return @{
        Capture = $capturePath
        CaptureHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $capturePath).Hash
        Report = $report
    }
}

$seriesA = Invoke-BTBHPhase2Series -Name "series-a"
$seriesB = Invoke-BTBHPhase2Series -Name "series-b"

if ($seriesA.Report.status -ne "ok" -or $seriesB.Report.status -ne "ok") {
    throw "One or both Phase 2 capture series did not report status=ok"
}
if ($seriesA.Report.driver -ne "vulkan" -or $seriesB.Report.driver -ne "vulkan") {
    throw "Windows Phase 2 capture must use exactly the Vulkan backend"
}
if ($seriesA.Report.resources_per_frame -ne 30 -or
    $seriesB.Report.resources_per_frame -ne 30) {
    throw "Phase 2 capture did not retire exactly 30 resources per iteration"
}
if ($seriesA.Report.width -ne $Width -or $seriesA.Report.height -ne $Height -or
    $seriesB.Report.width -ne $Width -or $seriesB.Report.height -ne $Height) {
    throw "Phase 2 capture dimensions do not match the requested dimensions"
}
if ($seriesA.Report.scene_signature -ne $seriesB.Report.scene_signature -or
    $seriesA.Report.final_image_checksum -ne $seriesB.Report.final_image_checksum -or
    $seriesA.CaptureHash -ne $seriesB.CaptureHash) {
    throw "Phase 2 capture series changed signature, checksum, or output bytes"
}

function Get-BTBHPercentDelta {
    param([uint64]$Left, [uint64]$Right)
    if ($Left -eq 0) {
        return 0.0
    }
    return [Math]::Round(
        ([Math]::Abs([double]$Right - [double]$Left) / [double]$Left) * 100.0,
        2
    )
}

$timingDelta = [ordered]@{
    cpu_submit_median_percent = Get-BTBHPercentDelta `
        $seriesA.Report.cpu_submit_ns.median `
        $seriesB.Report.cpu_submit_ns.median
    cpu_submit_p95_percent = Get-BTBHPercentDelta `
        $seriesA.Report.cpu_submit_ns.p95 `
        $seriesB.Report.cpu_submit_ns.p95
    gpu_wait_median_percent = Get-BTBHPercentDelta `
        $seriesA.Report.gpu_wait_ns.median `
        $seriesB.Report.gpu_wait_ns.median
    gpu_wait_p95_percent = Get-BTBHPercentDelta `
        $seriesA.Report.gpu_wait_ns.p95 `
        $seriesB.Report.gpu_wait_ns.p95
}

$summary = [ordered]@{
    status = "ok"
    driver = $seriesA.Report.driver
    width = $Width
    height = $Height
    warmup_frames = $WarmupFrames
    measured_frames = $MeasuredFrames
    scene_signature = $seriesA.Report.scene_signature
    final_image_checksum = $seriesA.Report.final_image_checksum
    capture_sha256 = $seriesA.CaptureHash
    timing_delta_percent = $timingDelta
    timing_delta_over_five_percent = @(
        $timingDelta.Values | Where-Object { $_ -gt 5.0 }
    ).Count -gt 0
}
$summaryJson = $summary | ConvertTo-Json -Depth 4 -Compress
Set-Content `
    -LiteralPath (Join-Path $OutputDirectory "summary.json") `
    -Value $summaryJson `
    -Encoding utf8
$summaryJson
