[CmdletBinding()]
param(
    [string] $RendererRoot = (Split-Path -Parent $PSScriptRoot),
    [string] $RendererBuildDirectory = (
        Join-Path $env:LOCALAPPDATA "btbh-renderer-gpu"
    ),
    [string] $Filter
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$rendererRootPath = (Resolve-Path -LiteralPath $RendererRoot).Path
$swiftExecutable = Join-Path $env:LOCALAPPDATA (
    "Programs\Swift\Toolchains\6.2.3+Asserts\usr\bin\swift.exe"
)
$vsDevCmd = Join-Path $env:ProgramFiles (
    "Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat"
)

foreach ($requiredPath in @($swiftExecutable, $vsDevCmd)) {
    if (-not (Test-Path -LiteralPath $requiredPath -PathType Leaf)) {
        throw "Required tool not found: $requiredPath"
    }
}

$environmentProbe = [Diagnostics.ProcessStartInfo]::new()
$environmentProbe.FileName = "cmd.exe"
$environmentProbe.UseShellExecute = $false
$environmentProbe.RedirectStandardOutput = $true
$environmentProbe.RedirectStandardError = $true
$environmentProbe.Arguments = (
    '/d /s /c ""' + $vsDevCmd + '" -arch=x64 >nul && set"'
)
$environmentProcess = [Diagnostics.Process]::Start($environmentProbe)
$environmentOutput = $environmentProcess.StandardOutput.ReadToEnd()
$environmentError = $environmentProcess.StandardError.ReadToEnd()
$environmentProcess.WaitForExit()
if ($environmentProcess.ExitCode -ne 0) {
    throw "VsDevCmd failed: $environmentError"
}

$environment = [Collections.Generic.Dictionary[string, string]]::new(
    [StringComparer]::OrdinalIgnoreCase
)
foreach ($entry in [Environment]::GetEnvironmentVariables().GetEnumerator()) {
    $name = [string] $entry.Key
    if (-not $environment.ContainsKey($name)) {
        $environment[$name] = [string] $entry.Value
    }
}
foreach ($line in $environmentOutput -split "`r?`n") {
    $separator = $line.IndexOf("=")
    if ($separator -gt 0) {
        $environment[$line.Substring(0, $separator)] = (
            $line.Substring($separator + 1)
        )
    }
}

$nativeLibraryDirectory = Join-Path $RendererBuildDirectory "lib"
$nativeBinaryDirectory = Join-Path $RendererBuildDirectory "bin"
$swiftToolchainDirectory = Split-Path -Parent $swiftExecutable
$shaderDirectory = Join-Path $RendererBuildDirectory (
    "generated\btbh_gpu\shaders"
)
$environment["BTBH_ENABLE_GPU_TESTS"] = "1"
$environment["BTBH_GPU_TEST_SHADER_DIR"] = $shaderDirectory
$environment["BTBH_GPU_SHADER_DIR"] = $shaderDirectory
$environment["LIB"] = (
    $nativeLibraryDirectory + ";" + $environment["LIB"]
)
$environment["PATH"] = (
    $swiftToolchainDirectory + ";" +
    $nativeBinaryDirectory + ";" +
    $environment["PATH"]
)

$startInfo = [Diagnostics.ProcessStartInfo]::new()
$startInfo.FileName = $swiftExecutable
$startInfo.WorkingDirectory = $rendererRootPath
$startInfo.UseShellExecute = $false
$startInfo.ArgumentList.Add("test")
if ($Filter) {
    $startInfo.ArgumentList.Add("--filter")
    $startInfo.ArgumentList.Add($Filter)
}
$startInfo.Environment.Clear()
foreach ($entry in $environment.GetEnumerator()) {
    $startInfo.Environment[$entry.Key] = $entry.Value
}

$process = [Diagnostics.Process]::Start($startInfo)
$process.WaitForExit()
exit $process.ExitCode
