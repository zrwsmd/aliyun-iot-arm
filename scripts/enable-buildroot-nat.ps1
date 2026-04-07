[CmdletBinding()]
param(
    [string]$TargetLinkInterfaceAlias = '',
    [string]$TargetLinkManageIp = '192.168.37.25',
    [string]$WanInterfaceAlias = '',
    [string]$NatName = 'BuildrootNat',
    [string]$NatPrefix = '192.168.50.0/24',
    [string]$WindowsNatIp = '192.168.50.1',
    [int]$WindowsNatPrefixLength = 24,
    [string]$TargetManageIp = '192.168.37.11',
    [string]$TargetInterface = 'eth3',
    [string]$TargetNatIp = '192.168.50.2',
    [string[]]$DnsServers = @('223.5.5.5', '8.8.8.8')
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

function Assert-InterfaceExists {
    param(
        [Parameter(Mandatory = $true)]
        [string]$InterfaceAlias
    )

    $adapter = Get-NetAdapter -Name $InterfaceAlias -ErrorAction SilentlyContinue
    if ($null -eq $adapter) {
        throw "Could not find interface: $InterfaceAlias"
    }
}

function Ensure-IpAddress {
    param(
        [Parameter(Mandatory = $true)]
        [string]$InterfaceAlias,
        [Parameter(Mandatory = $true)]
        [string]$IpAddress,
        [Parameter(Mandatory = $true)]
        [int]$PrefixLength
    )

    $existing = Get-NetIPAddress -InterfaceAlias $InterfaceAlias -AddressFamily IPv4 -ErrorAction SilentlyContinue |
        Where-Object { $_.IPAddress -eq $IpAddress }

    if ($null -eq $existing) {
        Write-Host "Add IPv4 $IpAddress/$PrefixLength to $InterfaceAlias"
        New-NetIPAddress -InterfaceAlias $InterfaceAlias -IPAddress $IpAddress -PrefixLength $PrefixLength | Out-Null
    } else {
        Write-Host "IPv4 $IpAddress/$PrefixLength already exists on $InterfaceAlias"
    }
}

function Ensure-ForwardingEnabled {
    param(
        [Parameter(Mandatory = $true)]
        [string]$InterfaceAlias
    )

    $ipIf = Get-NetIPInterface -InterfaceAlias $InterfaceAlias -AddressFamily IPv4 -ErrorAction SilentlyContinue
    if ($null -eq $ipIf) {
        throw "Could not find IPv4 interface settings for $InterfaceAlias"
    }

    if ($ipIf.Forwarding -ne 'Enabled') {
        Write-Host "Enable IPv4 forwarding on $InterfaceAlias"
        Set-NetIPInterface -InterfaceAlias $InterfaceAlias -AddressFamily IPv4 -Forwarding Enabled
    } else {
        Write-Host "IPv4 forwarding is already enabled on $InterfaceAlias"
    }
}

function Ensure-NatRule {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Name,
        [Parameter(Mandatory = $true)]
        [string]$Prefix
    )

    $existingByName = Get-NetNat -Name $Name -ErrorAction SilentlyContinue
    if ($null -ne $existingByName) {
        if ($existingByName.InternalIPInterfaceAddressPrefix -ne $Prefix) {
            throw "NAT $Name already exists, but its prefix is $($existingByName.InternalIPInterfaceAddressPrefix) instead of $Prefix"
        }
        Write-Host "NAT $Name already exists with prefix $Prefix"
        return
    }

    $existingByPrefix = Get-NetNat -ErrorAction SilentlyContinue |
        Where-Object { $_.InternalIPInterfaceAddressPrefix -eq $Prefix } |
        Select-Object -First 1

    if ($null -ne $existingByPrefix) {
        throw "NAT $($existingByPrefix.Name) already uses prefix $Prefix. Remove it first or choose a different prefix."
    }

    Write-Host "Create NAT $Name with prefix $Prefix"
    New-NetNat -Name $Name -InternalIPInterfaceAddressPrefix $Prefix | Out-Null
}

Assert-Administrator

$TargetLinkInterfaceAlias = Resolve-TargetLinkInterfaceAlias -InterfaceAlias $TargetLinkInterfaceAlias -ManageIp $TargetLinkManageIp
$WanInterfaceAlias = Resolve-WanInterfaceAlias -InterfaceAlias $WanInterfaceAlias -ExcludeAlias $TargetLinkInterfaceAlias

Assert-InterfaceExists -InterfaceAlias $TargetLinkInterfaceAlias
Assert-InterfaceExists -InterfaceAlias $WanInterfaceAlias

$Dns1 = if ($DnsServers.Count -ge 1) { $DnsServers[0] } else { '223.5.5.5' }
$Dns2 = if ($DnsServers.Count -ge 2) { $DnsServers[1] } else { $Dns1 }

Write-Host ''
Write-Host 'Applying Windows NAT settings...'
Write-Host "Target-link interface: $TargetLinkInterfaceAlias"
Write-Host "WAN interface:         $WanInterfaceAlias"

Ensure-IpAddress -InterfaceAlias $TargetLinkInterfaceAlias -IpAddress $WindowsNatIp -PrefixLength $WindowsNatPrefixLength
Ensure-ForwardingEnabled -InterfaceAlias $TargetLinkInterfaceAlias
Ensure-ForwardingEnabled -InterfaceAlias $WanInterfaceAlias
Ensure-NatRule -Name $NatName -Prefix $NatPrefix

$targetAlias = '{0}:1' -f $TargetInterface

Write-Host ''
Write-Host 'Windows side is ready.'
Write-Host "Management link: Windows $TargetLinkManageIp <-> Target $TargetManageIp"
Write-Host "NAT link:        Windows $WindowsNatIp <-> Target $TargetNatIp"
Write-Host ''
Write-Host 'Upload and run this script on the Buildroot target:'
Write-Host @"
chmod +x ./enable-buildroot-nat-target.sh
WINDOWS_NAT_IP=$WindowsNatIp TARGET_INTERFACE=$TargetInterface TARGET_ALIAS=$targetAlias TARGET_NAT_IP=$TargetNatIp DNS1=$Dns1 DNS2=$Dns2 ./enable-buildroot-nat-target.sh
"@

Write-Host ''
Write-Host 'Windows verification commands:'
Write-Host "  Get-NetNat -Name `"$NatName`""
Write-Host "  Get-NetIPAddress -InterfaceAlias `"$TargetLinkInterfaceAlias`" -AddressFamily IPv4"
Write-Host "  Get-NetIPInterface -InterfaceAlias `"$TargetLinkInterfaceAlias`",`"$WanInterfaceAlias`" -AddressFamily IPv4"
