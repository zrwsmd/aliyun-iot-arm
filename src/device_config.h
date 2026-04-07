#ifndef DEVICE_CONFIG_H
#define DEVICE_CONFIG_H

#include <stddef.h>

#define DEVICE_CONFIG_TEXT_SIZE 128
#define DEVICE_CONFIG_REGION_SIZE 64
#define DEVICE_CONFIG_SIGN_METHOD_SIZE 32
#define DEVICE_CONFIG_MAX_SUB_DEVICES 8

typedef struct SubDeviceConfig {
    char product_key[DEVICE_CONFIG_TEXT_SIZE];
    char device_name[DEVICE_CONFIG_TEXT_SIZE];
    char device_secret[DEVICE_CONFIG_TEXT_SIZE];
    char sign_method[DEVICE_CONFIG_SIGN_METHOD_SIZE];
} SubDeviceConfig;

typedef struct DeviceConfig {
    char product_key[DEVICE_CONFIG_TEXT_SIZE];
    char device_name[DEVICE_CONFIG_TEXT_SIZE];
    char device_secret[DEVICE_CONFIG_TEXT_SIZE];
    char instance_id[DEVICE_CONFIG_TEXT_SIZE];
    char region[DEVICE_CONFIG_REGION_SIZE];
    size_t sub_device_count;
    SubDeviceConfig sub_devices[DEVICE_CONFIG_MAX_SUB_DEVICES];
} DeviceConfig;

int device_config_load(const char *path, DeviceConfig *config);
int device_config_build_mqtt_host(const DeviceConfig *config, char *host, size_t host_size);

#endif
