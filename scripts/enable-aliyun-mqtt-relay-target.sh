#!/bin/sh

set -eu

MQTT_HOST="${MQTT_HOST:-iot-06z00d8xy9ns00z.mqtt.iothub.aliyuncs.com}"
RELAY_IP="${RELAY_IP:-192.168.37.69}"
HOSTS_PATH="${HOSTS_PATH:-/etc/hosts}"
BACKUP_PATH="${BACKUP_PATH:-/etc/hosts.aliyun-mqtt-relay.bak}"
TMP_PATH="${TMP_PATH:-/tmp/hosts.aliyun-mqtt-relay.$$}"

echo "Applying Aliyun MQTT relay target settings..."
echo "  mqtt host : ${MQTT_HOST}"
echo "  relay ip  : ${RELAY_IP}"
echo "  hosts     : ${HOSTS_PATH}"

if [ ! -f "${BACKUP_PATH}" ]; then
    cp "${HOSTS_PATH}" "${BACKUP_PATH}"
fi

grep -v "[[:space:]]${MQTT_HOST}\$" "${HOSTS_PATH}" > "${TMP_PATH}" || true
echo "${RELAY_IP} ${MQTT_HOST}" >> "${TMP_PATH}"
cat "${TMP_PATH}" > "${HOSTS_PATH}"
rm -f "${TMP_PATH}"

echo
echo "Current hosts entry:"
grep "${MQTT_HOST}" "${HOSTS_PATH}" || true

echo
echo "Checking Windows relay reachability..."
ping -c 4 "${RELAY_IP}"

echo
echo "Aliyun MQTT relay target settings are complete."
