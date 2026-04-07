[CmdletBinding()]
param(
    [string]$TargetLinkInterfaceAlias = '',
    [string]$TargetLinkManageIp = '192.168.37.25',
    [string]$WanInterfaceAlias = '',
    [string]$NatName = 'BuildrootNat',
    [string]$WindowsNatIp = '192.168.50.1',
    [string]$TargetInterface = 'eth3'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Assert-Administrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'Run this script in an elevated PowerShell window.'
    }
}

function Resolve-TargetLinkInterfaceAlias {
    param(
        [Parameter(Mandatory = $true)]
        [string]$InterfaceAlias,
        [Parameter(Mandatory = $true)]
        [string]$ManageIp
    )

    if ($InterfaceAlias) {
        return $InterfaceAlias
    }

    $ipEntry = Get-NetIPAddress -IPAddress $ManageIp -AddressFamily IPv4 -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if ($null -eq $ipEntry) {
        throw "Could not auto-detect the target-link interface from IP $ManageIp. Pass -TargetLinkInterfaceAlias explicitly."
    }
    return $ipEntry.InterfaceAlias
}

function Resolve-WanInterfaceAlias {
    param(
        [Parameter(Mandatory = $true)]
        [string]$InterfaceAlias,
        [Parameter(Mandatory = $true)]
        [string]$ExcludeAlias
    )

    if ($InterfaceAlias) {
        return $InterfaceAlias
    }

    $route = Get-NetRoute -DestinationPrefix '0.0.0.0/0' -AddressFamily IPv4 -ErrorAction SilentlyContinue |
        Where-Object { $_.InterfaceAlias -ne $ExcludeAlias } |
        Sort-Object RouteMetric, InterfaceMetric |
        Select-Object -First 1
    if ($null -eq $route) {
        throw 'Could not auto-detect the WAN interface. Pass -WanInterfaceAlias explicitly.'
    }
    return $route.InterfaceAlias
}

Assert-Administrator

$TargetLinkInterfaceAlias = Resolve-TargetLinkInterfaceAlias -InterfaceAlias $TargetLinkInterfaceAlias -ManageIp $TargetLinkManageIp
$WanInterfaceAlias = Resolve-WanInterfaceAlias -InterfaceAlias $WanInterfaceAlias -ExcludeAlias $TargetLinkInterfaceAlias

Write-Host ''
Write-Host 'Removing Windows NAT settings...'
Write-Host "Target-link interface: $TargetLinkInterfaceAlias"
Write-Host "WAN interface:         $WanInterfaceAlias"

$nat = Get-NetNat -Name $NatName -ErrorAction SilentlyContinue
if ($null -ne $nat) {
    Write-Host "Remove NAT $NatName"
    Remove-NetNat -Name $NatName -Confirm:$false
} else {
    Write-Host "NAT $NatName does not exist, skip"
}

$natIp = Get-NetIPAddress -InterfaceAlias $TargetLinkInterfaceAlias -AddressFamily IPv4 -ErrorAction SilentlyContinue |
    Where-Object { $_.IPAddress -eq $WindowsNatIp }
if ($null -ne $natIp) {
    Write-Host "Remove IP $WindowsNatIp from $TargetLinkInterfaceAlias"
    Remove-NetIPAddress -InterfaceAlias $TargetLinkInterfaceAlias -IPAddress $WindowsNatIp -Confirm:$false
} else {
    Write-Host "IP $WindowsNatIp does not exist on $TargetLinkInterfaceAlias, skip"
}

$targetIf = Get-NetIPInterface -InterfaceAlias $TargetLinkInterfaceAlias -AddressFamily IPv4 -ErrorAction SilentlyContinue
if ($null -ne $targetIf -and $targetIf.Forwarding -ne 'Disabled') {
    Write-Host "Disable IPv4 forwarding on $TargetLinkInterfaceAlias"
    Set-NetIPInterface -InterfaceAlias $TargetLinkInterfaceAlias -AddressFamily IPv4 -Forwarding Disabled
} else {
    Write-Host "IPv4 forwarding is already disabled on $TargetLinkInterfaceAlias"
}

$wanIf = Get-NetIPInterface -InterfaceAlias $WanInterfaceAlias -AddressFamily IPv4 -ErrorAction SilentlyContinue
if ($null -ne $wanIf -and $wanIf.Forwarding -ne 'Disabled') {
    Write-Host "Disable IPv4 forwarding on $WanInterfaceAlias"
    Set-NetIPInterface -InterfaceAlias $WanInterfaceAlias -AddressFamily IPv4 -Forwarding Disabled
} else {
    Write-Host "IPv4 forwarding is already disabled on $WanInterfaceAlias"
}

$targetAlias = '{0}:1' -f $TargetInterface

Write-Host ''
Write-Host 'Windows side rollback is done.'
Write-Host ''
Write-Host 'Upload and run this script on the Buildroot target:'
Write-Host @"
chmod +x ./disable-buildroot-nat-target.sh
TARGET_INTERFACE=$TargetInterface TARGET_ALIAS=$targetAlias ./disable-buildroot-nat-target.sh
"@

Write-Host ''
Write-Host 'Windows verification commands:'
Write-Host '  Get-NetNat'
Write-Host "  Get-NetIPAddress -InterfaceAlias `"$TargetLinkInterfaceAlias`" -AddressFamily IPv4"
