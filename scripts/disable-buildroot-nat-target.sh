#!/bin/sh

set -eu

TARGET_INTERFACE="${TARGET_INTERFACE:-eth3}"
TARGET_ALIAS="${TARGET_ALIAS:-${TARGET_INTERFACE}:1}"

echo "Removing target-side NAT settings..."
echo "  target interface: ${TARGET_INTERFACE}"
echo "  target alias:     ${TARGET_ALIAS}"

route del default 2>/dev/null || true
ifconfig "${TARGET_ALIAS}" down 2>/dev/null || true

echo
echo "Current route table:"
route -n

echo
echo "Current interface status:"
ifconfig "${TARGET_INTERFACE}"

echo
echo "Target-side NAT rollback is complete."
