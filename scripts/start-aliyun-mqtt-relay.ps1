[CmdletBinding()]
param(
    [string]$ConfigPath = '',
    [string]$ListenAddress = '192.168.37.69',
    [int]$ListenPort = 8883,
    [string]$PidFile = '',
    [string]$LogFile = '',
    [switch]$Background,
    [switch]$ChildProcess
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

function Get-MqttHostFromConfig {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $config = Get-Content -Path $Path -Raw | ConvertFrom-Json
    if (-not [string]::IsNullOrWhiteSpace($config.instanceId)) {
        return '{0}.mqtt.iothub.aliyuncs.com' -f $config.instanceId.Trim()
    }

    if (-not [string]::IsNullOrWhiteSpace($config.region) -and -not [string]::IsNullOrWhiteSpace($config.productKey)) {
        return '{0}.iot-as-mqtt.{1}.aliyuncs.com' -f $config.productKey.Trim(), $config.region.Trim()
    }

    throw 'device_id.json missing instanceId, or region/productKey.'
}

function Test-ProcessRunning {
    param(
        [Parameter(Mandatory = $true)]
        [int]$ProcessId
    )

    try {
        $null = Get-Process -Id $ProcessId -ErrorAction Stop
        return $true
    } catch {
        return $false
    }
}

$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
if ([string]::IsNullOrWhiteSpace($ConfigPath)) {
    $ConfigPath = Join-Path $scriptRoot '..\device_id.json'
}
if ([string]::IsNullOrWhiteSpace($PidFile)) {
    $PidFile = Join-Path $scriptRoot 'aliyun-mqtt-relay.pid'
}
if ([string]::IsNullOrWhiteSpace($LogFile)) {
    $LogFile = Join-Path $scriptRoot 'aliyun-mqtt-relay.log'
}

$resolvedConfigPath = Resolve-FullPath -Path $ConfigPath
$resolvedPidFile = Resolve-FullPath -Path $PidFile
$resolvedLogFile = Resolve-FullPath -Path $LogFile

if (-not (Test-Path -Path $resolvedConfigPath)) {
    throw "Config file not found: $resolvedConfigPath"
}

$mqttHost = Get-MqttHostFromConfig -Path $resolvedConfigPath

if ($Background -and -not $ChildProcess) {
    if (Test-Path -Path $resolvedPidFile) {
        $existingPidText = (Get-Content -Path $resolvedPidFile -Raw).Trim()
        if ($existingPidText) {
            $existingPid = [int]$existingPidText
            if (Test-ProcessRunning -ProcessId $existingPid) {
                throw "MQTT relay is already running with PID $existingPid"
            }
        }
        Remove-Item -Path $resolvedPidFile -Force -ErrorAction SilentlyContinue
    }

    $arguments = @(
        '-NoProfile'
        '-ExecutionPolicy'
        'Bypass'
        '-File'
        $PSCommandPath
        '-ConfigPath'
        $resolvedConfigPath
        '-ListenAddress'
        $ListenAddress
        '-ListenPort'
        $ListenPort.ToString()
        '-PidFile'
        $resolvedPidFile
        '-LogFile'
        $resolvedLogFile
        '-ChildProcess'
    )

    $process = Start-Process -FilePath 'powershell.exe' -ArgumentList $arguments -WindowStyle Hidden -PassThru
    Start-Sleep -Milliseconds 500
    Set-Content -Path $resolvedPidFile -Value $process.Id -Encoding ascii

    Write-Host "Aliyun MQTT relay started in background. PID: $($process.Id)"
    Write-Host "Listen: $ListenAddress`:$ListenPort"
    Write-Host "Remote: $mqttHost`:8883"
    Write-Host "PID file: $resolvedPidFile"
    Write-Host "Log file: $resolvedLogFile"
    return
}

$logDirectory = Split-Path -Path $resolvedLogFile -Parent
if (-not [string]::IsNullOrWhiteSpace($logDirectory)) {
    New-Item -ItemType Directory -Force -Path $logDirectory | Out-Null
}

if (-not ('SimpleTcpRelayHost' -as [type])) {
    Add-Type -Language CSharp -TypeDefinition @"
using System;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Threading.Tasks;

public static class SimpleTcpRelayHost
{
    private static readonly object SyncRoot = new object();

    private static void Log(string path, string message)
    {
        lock (SyncRoot)
        {
            File.AppendAllText(path, DateTime.Now.ToString("O") + " " + message + Environment.NewLine);
        }
    }

    private static async Task PumpAsync(NetworkStream input, NetworkStream output)
    {
        var buffer = new byte[81920];
        while (true)
        {
            var read = await input.ReadAsync(buffer, 0, buffer.Length).ConfigureAwait(false);
            if (read <= 0)
            {
                break;
            }

            await output.WriteAsync(buffer, 0, read).ConfigureAwait(false);
            await output.FlushAsync().ConfigureAwait(false);
        }
    }

    private static async Task RelayConnectionAsync(TcpClient inboundClient, string remoteHost, int remotePort, string logPath)
    {
        var inboundEndpoint = inboundClient.Client.RemoteEndPoint == null ? "<unknown>" : inboundClient.Client.RemoteEndPoint.ToString();
        using (inboundClient)
        using (var outboundClient = new TcpClient())
        {
            inboundClient.NoDelay = true;
            outboundClient.NoDelay = true;

            try
            {
                Log(logPath, "accepted " + inboundEndpoint);
                await outboundClient.ConnectAsync(remoteHost, remotePort).ConfigureAwait(false);
                var remoteEndpoint = outboundClient.Client.RemoteEndPoint == null ? remoteHost + ":" + remotePort : outboundClient.Client.RemoteEndPoint.ToString();
                Log(logPath, "connected " + inboundEndpoint + " -> " + remoteEndpoint);

                using (var inboundStream = inboundClient.GetStream())
                using (var outboundStream = outboundClient.GetStream())
                {
                    var upstream = PumpAsync(inboundStream, outboundStream);
                    var downstream = PumpAsync(outboundStream, inboundStream);
                    await Task.WhenAny(upstream, downstream).ConfigureAwait(false);
                }

                Log(logPath, "closed " + inboundEndpoint);
            }
            catch (Exception ex)
            {
                Log(logPath, "error " + inboundEndpoint + " -> " + ex.GetType().Name + ": " + ex.Message);
            }
        }
    }

    public static void Run(string listenAddress, int listenPort, string remoteHost, int remotePort, string logPath)
    {
        var listener = new TcpListener(IPAddress.Parse(listenAddress), listenPort);
        listener.Start(100);
        Log(logPath, "relay started listen=" + listenAddress + ":" + listenPort + " remote=" + remoteHost + ":" + remotePort);

        while (true)
        {
            var inboundClient = listener.AcceptTcpClient();
            Task.Run(() => RelayConnectionAsync(inboundClient, remoteHost, remotePort, logPath));
        }
    }
}
"@
}

Set-Content -Path $resolvedPidFile -Value $PID -Encoding ascii
Add-Content -Path $resolvedLogFile -Value ("{0} relay bootstrap listen={1}:{2} remote={3}:8883" -f (Get-Date -Format o), $ListenAddress, $ListenPort, $mqttHost)

Write-Host "Aliyun MQTT relay is starting..."
Write-Host "Listen: $ListenAddress`:$ListenPort"
Write-Host "Remote: $mqttHost`:8883"
Write-Host "PID file: $resolvedPidFile"
Write-Host "Log file: $resolvedLogFile"

[SimpleTcpRelayHost]::Run($ListenAddress, $ListenPort, $mqttHost, 8883, $resolvedLogFile)
