#ifndef DEVICE_SHADOW_MANAGER_H
#define DEVICE_SHADOW_MANAGER_H

#include <pthread.h>

struct AppContext;

typedef struct DeviceShadowManager {
    struct AppContext *app;
    pthread_mutex_t lock;
    int version;
} DeviceShadowManager;

int device_shadow_manager_init(DeviceShadowManager *manager, struct AppContext *app);
void device_shadow_manager_cleanup(DeviceShadowManager *manager);
int device_shadow_manager_start(DeviceShadowManager *manager);
int device_shadow_manager_handle_message(DeviceShadowManager *manager, const char *topic, const char *payload);
int device_shadow_manager_get(DeviceShadowManager *manager);
int device_shadow_manager_update_reported(DeviceShadowManager *manager, const char *reported_json);
int device_shadow_manager_delete_key(DeviceShadowManager *manager, const char *key);
int device_shadow_manager_delete_all(DeviceShadowManager *manager);

#endif
