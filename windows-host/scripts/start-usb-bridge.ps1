[CmdletBinding()]
param(
    [int]$Port = 52100,
    [string]$IproxyPath = ""
)

$ErrorActionPreference = "Stop"

if (-not $IproxyPath) {
    $localTool = Join-Path $PSScriptRoot "..\tools\iproxy.exe"
    $resolved = Get-Command iproxy.exe -ErrorAction SilentlyContinue
    if (Test-Path $localTool) {
        $IproxyPath = $localTool
    } elseif ($resolved) {
        $IproxyPath = $resolved.Source
    } else {
        throw "iproxy.exe was not found. Put it in windows-host\tools or add it to PATH."
    }
}

Write-Host "Forwarding Windows localhost:$Port to the iPad app over USB."
Write-Host "Keep this window open; press Ctrl+C to stop."
& $IproxyPath $Port $Port

