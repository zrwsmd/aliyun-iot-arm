#include "device_shadow_manager.h"

#include "iot_ide_app.h"
#include "json_utils.h"
#include "string_builder.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void device_shadow_manager_build_get_topic(DeviceShadowManager *manager, char *topic, size_t topic_size) {
    snprintf(topic,
             topic_size,
             "/shadow/get/%s/%s",
             manager->app->config.product_key,
             manager->app->config.device_name);
}

static void device_shadow_manager_build_update_topic(DeviceShadowManager *manager, char *topic, size_t topic_size) {
    snprintf(topic,
             topic_size,
             "/shadow/update/%s/%s",
             manager->app->config.product_key,
             manager->app->config.device_name);
}

static int device_shadow_manager_get_next_version(DeviceShadowManager *manager) {
    int version;

    pthread_mutex_lock(&manager->lock);
    version = (manager->version > 0) ? (manager->version + 1) : 0;
    pthread_mutex_unlock(&manager->lock);
    return version;
}

static void device_shadow_manager_update_version(DeviceShadowManager *manager, int version) {
    if (version <= 0) {
        return;
    }

    pthread_mutex_lock(&manager->lock);
    manager->version = version;
    pthread_mutex_unlock(&manager->lock);
}

static int device_shadow_manager_publish(DeviceShadowManager *manager, const char *payload_json) {
    char topic[320];

    if (manager == NULL || payload_json == NULL) {
        return -1;
    }

    device_shadow_manager_build_update_topic(manager, topic, sizeof(topic));
    return app_publish_topic(manager->app, topic, payload_json, 0);
}

int device_shadow_manager_init(DeviceShadowManager *manager, struct AppContext *app) {
    if (manager == NULL || app == NULL) {
        return -1;
    }

    memset(manager, 0, sizeof(*manager));
    manager->app = app;
    if (pthread_mutex_init(&manager->lock, NULL) != 0) {
        return -1;
    }

    return 0;
}

void device_shadow_manager_cleanup(DeviceShadowManager *manager) {
    if (manager == NULL) {
        return;
    }

    pthread_mutex_destroy(&manager->lock);
}

int device_shadow_manager_start(DeviceShadowManager *manager) {
    char topic[320];

    if (manager == NULL) {
        return -1;
    }

    device_shadow_manager_build_get_topic(manager, topic, sizeof(topic));
    if (app_subscribe_topic(manager->app, topic) != 0) {
        return -1;
    }

    return device_shadow_manager_get(manager);
}

int device_shadow_manager_get(DeviceShadowManager *manager) {
    return device_shadow_manager_publish(manager, "{\"method\":\"get\"}");
}

int device_shadow_manager_update_reported(DeviceShadowManager *manager, const char *reported_json) {
    StringBuilder payload;
    int version;
    int rc;

    if (manager == NULL || reported_json == NULL) {
        return -1;
    }

    if (sb_init(&payload, 256) != 0) {
        return -1;
    }

    version = device_shadow_manager_get_next_version(manager);
    if (version > 0) {
        sb_appendf(&payload,
                   "{\"method\":\"update\",\"state\":{\"reported\":%s},\"version\":%d}",
                   reported_json,
                   version);
    } else {
        sb_appendf(&payload,
                   "{\"method\":\"update\",\"state\":{\"reported\":%s}}",
                   reported_json);
    }

    rc = device_shadow_manager_publish(manager, payload.data);
    sb_free(&payload);
    return rc;
}

int device_shadow_manager_delete_key(DeviceShadowManager *manager, const char *key) {
    StringBuilder payload;
    char escaped_key[256];
    int version;
    int rc;

    if (manager == NULL || key == NULL) {
        return -1;
    }

    if (json_escape_string(key, escaped_key, sizeof(escaped_key)) != 0) {
        return -1;
    }

    if (sb_init(&payload, 256) != 0) {
        return -1;
    }

    version = device_shadow_manager_get_next_version(manager);
    if (version > 0) {
        sb_appendf(&payload,
                   "{\"method\":\"delete\",\"state\":{\"reported\":{\"%s\":\"null\"}},\"version\":%d}",
                   escaped_key,
                   version);
    } else {
        sb_appendf(&payload,
                   "{\"method\":\"delete\",\"state\":{\"reported\":{\"%s\":\"null\"}}}",
                   escaped_key);
    }

    rc = device_shadow_manager_publish(manager, payload.data);
    sb_free(&payload);
    return rc;
}

int device_shadow_manager_delete_all(DeviceShadowManager *manager) {
    StringBuilder payload;
    int version;
    int rc;

    if (manager == NULL) {
        return -1;
    }

    if (sb_init(&payload, 128) != 0) {
        return -1;
    }

    version = device_shadow_manager_get_next_version(manager);
    if (version > 0) {
        sb_appendf(&payload,
                   "{\"method\":\"delete\",\"state\":{\"reported\":\"null\"},\"version\":%d}",
                   version);
    } else {
        sb_append(&payload, "{\"method\":\"delete\",\"state\":{\"reported\":\"null\"}}");
    }

    rc = device_shadow_manager_publish(manager, payload.data);
    sb_free(&payload);
    return rc;
}

int device_shadow_manager_handle_message(DeviceShadowManager *manager, const char *topic, const char *payload) {
    char expected_topic[320];
    char method[64] = "";
    char version_raw[64];

    if (manager == NULL || topic == NULL || payload == NULL) {
        return 0;
    }

    device_shadow_manager_build_get_topic(manager, expected_topic, sizeof(expected_topic));
    if (strcmp(topic, expected_topic) != 0) {
        return 0;
    }

    if (json_get_raw_value(payload, "version", version_raw, sizeof(version_raw)) == 0) {
        device_shadow_manager_update_version(manager, atoi(version_raw));
    }

    (void)json_get_string(payload, "method", method, sizeof(method));
    printf("shadow message method=%s payload=%s\n", method[0] == '\0' ? "unknown" : method, payload);

    if (strcmp(method, "control") == 0) {
        char payload_json[4096];
        char state_json[3072];
        char desired_json[2048];
        char adas_value[64];

        if (json_get_object(payload, "payload", payload_json, sizeof(payload_json)) == 0 &&
            json_get_object(payload_json, "state", state_json, sizeof(state_json)) == 0 &&
            json_get_object(state_json, "desired", desired_json, sizeof(desired_json)) == 0) {
            if (json_get_raw_value(desired_json, "ADASSwitch", adas_value, sizeof(adas_value)) == 0) {
                manager->app->adas_switch = atoi(adas_value) != 0;
                (void)app_post_properties(manager->app, desired_json);
            }
            (void)device_shadow_manager_update_reported(manager, desired_json);
        }
    }

    return 1;
}
