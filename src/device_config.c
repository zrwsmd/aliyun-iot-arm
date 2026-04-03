#include "device_config.h"
#include "json_utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *device_config_read_file(const char *path) {
    FILE *file;
    char *buffer;
    long length;

    file = fopen(path, "rb");
    if (file == NULL) {
        return NULL;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }

    length = ftell(file);
    if (length < 0) {
        fclose(file);
        return NULL;
    }
    rewind(file);

    buffer = (char *)malloc((size_t)length + 1);
    if (buffer == NULL) {
        fclose(file);
        return NULL;
    }

    if (fread(buffer, 1, (size_t)length, file) != (size_t)length) {
        free(buffer);
        fclose(file);
        return NULL;
    }

    buffer[length] = '\0';
    fclose(file);
    return buffer;
}

int device_config_load(const char *path, DeviceConfig *config) {
    char *json;
    int rc = -1;

    if (path == NULL || config == NULL) {
        return -1;
    }

    json = device_config_read_file(path);
    if (json == NULL) {
        fprintf(stderr, "failed to read config file: %s\n", path);
        return -1;
    }

    memset(config, 0, sizeof(*config));

    if (json_get_string(json, "productKey", config->product_key, sizeof(config->product_key)) != 0 ||
        json_get_string(json, "deviceName", config->device_name, sizeof(config->device_name)) != 0 ||
        json_get_string(json, "deviceSecret", config->device_secret, sizeof(config->device_secret)) != 0) {
        fprintf(stderr, "device_id.json missing productKey/deviceName/deviceSecret\n");
        goto cleanup;
    }

    (void)json_get_string(json, "instanceId", config->instance_id, sizeof(config->instance_id));
    (void)json_get_string(json, "region", config->region, sizeof(config->region));
    rc = 0;

cleanup:
    free(json);
    return rc;
}

int device_config_build_mqtt_host(const DeviceConfig *config, char *host, size_t host_size) {
    if (config == NULL || host == NULL || host_size == 0) {
        return -1;
    }

    if (config->instance_id[0] != '\0') {
        return (snprintf(host, host_size, "%s.mqtt.iothub.aliyuncs.com", config->instance_id) >= 0) ? 0 : -1;
    }

    if (config->region[0] == '\0') {
        fprintf(stderr, "instanceId and region are both empty, cannot build mqtt host\n");
        return -1;
    }

    return (snprintf(host, host_size, "%s.iot-as-mqtt.%s.aliyuncs.com", config->product_key, config->region) >= 0) ? 0 : -1;
}
