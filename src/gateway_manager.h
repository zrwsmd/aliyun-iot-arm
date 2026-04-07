#ifndef GATEWAY_MANAGER_H
#define GATEWAY_MANAGER_H

#include <pthread.h>

#include "device_config.h"

struct AppContext;

typedef struct GatewaySubDeviceState {
    SubDeviceConfig config;
    int topo_added;
    int online;
    int disabled;
    char last_property_json[1024];
} GatewaySubDeviceState;

typedef struct GatewayManager {
    struct AppContext *app;
    pthread_mutex_t lock;
    size_t sub_device_count;
    GatewaySubDeviceState sub_devices[DEVICE_CONFIG_MAX_SUB_DEVICES];
} GatewayManager;

int gateway_manager_init(GatewayManager *manager, struct AppContext *app);
void gateway_manager_cleanup(GatewayManager *manager);
int gateway_manager_start(GatewayManager *manager);
int gateway_manager_handle_message(GatewayManager *manager, const char *topic, const char *payload);

#endif
