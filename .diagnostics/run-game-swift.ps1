[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, Position = 0)]
    [string] $GameRoot,

    [Parameter(
        Mandatory = $true,
        Position = 1,
        ValueFromRemainingArguments = $true
    )]
    [string[]] $SwiftArguments,

    [string] $OutputPattern
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Copy-EnvironmentCaseInsensitively {
    param([Collections.IDictionary] $Source)

    $result = [Collections.Generic.Dictionary[string, string]]::new(
        [StringComparer]::OrdinalIgnoreCase
    )
    foreach ($entry in $Source.GetEnumerator()) {
        $name = [string] $entry.Key
        if (-not $result.ContainsKey($name)) {
            $result[$name] = [string] $entry.Value
        }
    }
    return $result
}

function Invoke-CapturedProcess {
    param([Diagnostics.ProcessStartInfo] $StartInfo)

    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $StartInfo
    if (-not $process.Start()) {
        throw "Could not launch $($StartInfo.FileName)"
    }
    $stdoutTask = $process.StandardOutput.ReadToEndAsync()
    $stderrTask = $process.StandardError.ReadToEndAsync()
    $process.WaitForExit()
    return [pscustomobject]@{
        ExitCode = $process.ExitCode
        StandardOutput = $stdoutTask.GetAwaiter().GetResult()
        StandardError = $stderrTask.GetAwaiter().GetResult()
    }
}

$resolvedGameRoot = (Resolve-Path -LiteralPath $GameRoot).Path
$baseEnvironment = Copy-EnvironmentCaseInsensitively (
    [Environment]::GetEnvironmentVariables()
)
$vsDevCmd = Join-Path $env:ProgramFiles (
    "Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat"
)
if (-not (Test-Path -LiteralPath $vsDevCmd -PathType Leaf)) {
    throw "Visual Studio developer environment is missing: $vsDevCmd"
}

$environmentStartInfo = [Diagnostics.ProcessStartInfo]::new()
$environmentStartInfo.FileName = "cmd.exe"
$environmentStartInfo.Arguments = "/d /s /c `"`"$vsDevCmd`" -arch=x64 >nul && set`""
$environmentStartInfo.WorkingDirectory = $resolvedGameRoot
$environmentStartInfo.UseShellExecute = $false
$environmentStartInfo.RedirectStandardOutput = $true
$environmentStartInfo.RedirectStandardError = $true
$environmentStartInfo.Environment.Clear()
foreach ($entry in $baseEnvironment.GetEnumerator()) {
    $environmentStartInfo.Environment[$entry.Key] = $entry.Value
}
$environmentResult = Invoke-CapturedProcess $environmentStartInfo
if ($environmentResult.ExitCode -ne 0) {
    throw "VsDevCmd failed: $($environmentResult.StandardError)"
}

$visualStudioEnvironment = [Collections.Generic.Dictionary[string, string]]::new(
    [StringComparer]::OrdinalIgnoreCase
)
foreach ($line in $environmentResult.StandardOutput -split "`r?`n") {
    $separator = $line.IndexOf("=")
    if ($separator -le 0) { continue }
    $name = $line.Substring(0, $separator)
    $value = $line.Substring($separator + 1)
    $visualStudioEnvironment[$name] = $value
}

$rendererBuildRoot = if ($env:BTBH_RENDERER_BUILD_ROOT) {
    $env:BTBH_RENDERER_BUILD_ROOT
} else {
    Join-Path $env:LOCALAPPDATA "btbh-renderer-gpu"
}
$visualStudioEnvironment["BTBH_ENABLE_GPU_BACKEND"] = "1"
$visualStudioEnvironment["LIB"] = [string]::Join(";", @(
    (Join-Path $rendererBuildRoot "lib"),
    (Join-Path $resolvedGameRoot "lib"),
    $visualStudioEnvironment["LIB"]
))
$visualStudioEnvironment["PATH"] = [string]::Join(";", @(
    (Join-Path $rendererBuildRoot "bin"),
    (Join-Path $resolvedGameRoot "lib"),
    $visualStudioEnvironment["PATH"]
))

$swift = [Diagnostics.ProcessStartInfo]::new()
$swift.FileName = Join-Path $env:LOCALAPPDATA (
    "Programs\Swift\Toolchains\6.2.3+Asserts\usr\bin\swift.exe"
)
$swift.WorkingDirectory = $resolvedGameRoot
$swift.UseShellExecute = $false
$swift.RedirectStandardOutput = $true
$swift.RedirectStandardError = $true
$swift.Environment.Clear()
foreach ($entry in $visualStudioEnvironment.GetEnumerator()) {
    $swift.Environment[$entry.Key] = $entry.Value
}
foreach ($argument in $SwiftArguments) {
    $swift.ArgumentList.Add($argument)
}

$result = Invoke-CapturedProcess $swift
if ($OutputPattern) {
    foreach ($line in (
        ($result.StandardOutput + "`n" + $result.StandardError) -split "`r?`n"
    )) {
        if ($line -match $OutputPattern) {
            [Console]::Out.WriteLine($line)
        }
    }
} else {
    [Console]::Out.Write($result.StandardOutput)
    [Console]::Error.Write($result.StandardError)
}
exit $result.ExitCode
