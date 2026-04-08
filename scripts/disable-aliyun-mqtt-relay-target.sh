#!/bin/sh

set -eu

MQTT_HOST="${MQTT_HOST:-iot-06z00d8xy9ns00z.mqtt.iothub.aliyuncs.com}"
HOSTS_PATH="${HOSTS_PATH:-/etc/hosts}"
BACKUP_PATH="${BACKUP_PATH:-/etc/hosts.aliyun-mqtt-relay.bak}"
TMP_PATH="${TMP_PATH:-/tmp/hosts.aliyun-mqtt-relay.$$}"

echo "Removing Aliyun MQTT relay target settings..."
echo "  mqtt host : ${MQTT_HOST}"
echo "  hosts     : ${HOSTS_PATH}"

if [ -f "${BACKUP_PATH}" ]; then
    cp "${BACKUP_PATH}" "${HOSTS_PATH}"
else
    grep -v "[[:space:]]${MQTT_HOST}\$" "${HOSTS_PATH}" > "${TMP_PATH}" || true
    cat "${TMP_PATH}" > "${HOSTS_PATH}"
    rm -f "${TMP_PATH}"
fi

echo
echo "Current hosts entry:"
grep "${MQTT_HOST}" "${HOSTS_PATH}" || true

echo
echo "Aliyun MQTT relay target settings have been removed."
