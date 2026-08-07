[CmdletBinding()]
param(
    [string]$BuildDirectory = "",
    [int]$Port = 52100,
    [int]$Fps = 120,
    [int]$Bitrate = 60000000,
    [int]$Display = -1
)

$ErrorActionPreference = "Stop"
if (-not $BuildDirectory) {
    $BuildDirectory = Join-Path $PSScriptRoot "..\build"
}

$candidates = @(
    (Join-Path $BuildDirectory "Release\padbridge_host.exe"),
    (Join-Path $BuildDirectory "padbridge_host.exe")
)
$hostExe = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $hostExe) {
    throw "padbridge_host.exe was not found. Build windows-host first."
}

$arguments = @("--host", "127.0.0.1", "--port", $Port, "--fps", $Fps, "--bitrate", $Bitrate)
if ($Display -ge 0) { $arguments += @("--display", $Display) }
& $hostExe @arguments
