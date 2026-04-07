#!/bin/sh

set -eu

TARGET_INTERFACE="${TARGET_INTERFACE:-eth3}"
TARGET_ALIAS="${TARGET_ALIAS:-${TARGET_INTERFACE}:1}"
TARGET_NAT_IP="${TARGET_NAT_IP:-192.168.50.2}"
TARGET_NETMASK="${TARGET_NETMASK:-255.255.255.0}"
WINDOWS_NAT_IP="${WINDOWS_NAT_IP:-192.168.50.1}"
DNS1="${DNS1:-223.5.5.5}"
DNS2="${DNS2:-8.8.8.8}"
VERIFY_HOST="${VERIFY_HOST:-www.baidu.com}"
VERIFY_PUBLIC_IP="${VERIFY_PUBLIC_IP:-223.5.5.5}"

echo "Applying target-side NAT settings..."
echo "  target interface: ${TARGET_INTERFACE}"
echo "  target alias:     ${TARGET_ALIAS}"
echo "  target NAT IP:    ${TARGET_NAT_IP}"
echo "  windows NAT IP:   ${WINDOWS_NAT_IP}"

ifconfig "${TARGET_ALIAS}" "${TARGET_NAT_IP}" netmask "${TARGET_NETMASK}" up

echo
echo "Checking Windows NAT gateway..."
ping -c 4 "${WINDOWS_NAT_IP}"

echo
echo "Updating default route..."
route del default 2>/dev/null || true
route add default gw "${WINDOWS_NAT_IP}" "${TARGET_INTERFACE}"

echo
echo "Updating DNS..."
{
    echo "nameserver ${DNS1}"
    echo "nameserver ${DNS2}"
} > /etc/resolv.conf

echo
echo "Current route table:"
route -n

echo
echo "Current resolv.conf:"
cat /etc/resolv.conf

echo
echo "Verifying connectivity..."
ping -c 4 "${WINDOWS_NAT_IP}"
ping -c 4 "${VERIFY_PUBLIC_IP}"
ping -c 4 "${VERIFY_HOST}"

echo
echo "Target-side NAT setup is complete."
