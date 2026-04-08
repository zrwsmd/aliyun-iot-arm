[CmdletBinding()]
param(
    [string]$PidFile = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Resolve-FullPath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    if ([System.IO.Path]::IsPathRooted($Path)) {
        return [System.IO.Path]::GetFullPath($Path)
    }

    return [System.IO.Path]::GetFullPath((Join-Path (Get-Location) $Path))
}

$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
if ([string]::IsNullOrWhiteSpace($PidFile)) {
    $PidFile = Join-Path $scriptRoot 'aliyun-mqtt-relay.pid'
}

$resolvedPidFile = Resolve-FullPath -Path $PidFile

if (-not (Test-Path -Path $resolvedPidFile)) {
    Write-Host "PID file not found: $resolvedPidFile"
    return
}

$pidText = (Get-Content -Path $resolvedPidFile -Raw).Trim()
if (-not $pidText) {
    Remove-Item -Path $resolvedPidFile -Force -ErrorAction SilentlyContinue
    Write-Host "PID file was empty and has been removed."
    return
}

$processId = [int]$pidText

try {
    $process = Get-Process -Id $processId -ErrorAction Stop
    Stop-Process -Id $process.Id -Force
    Write-Host "Stopped Aliyun MQTT relay process: $processId"
} catch {
    Write-Host "Process $processId is not running."
}

Remove-Item -Path $resolvedPidFile -Force -ErrorAction SilentlyContinue
