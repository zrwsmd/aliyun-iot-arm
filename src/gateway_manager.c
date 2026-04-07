#include "gateway_manager.h"

#include "iot_ide_app.h"
#include "json_utils.h"
#include "string_builder.h"

#include "core_sha256.h"
#include "core_string.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int gateway_equals_ignore_case(const char *lhs, const char *rhs) {
    if (lhs == NULL || rhs == NULL) {
        return 0;
    }

    while (*lhs != '\0' && *rhs != '\0') {
        if (tolower((unsigned char)*lhs) != tolower((unsigned char)*rhs)) {
            return 0;
        }
        lhs++;
        rhs++;
    }

    return *lhs == '\0' && *rhs == '\0';
}

static const char *gateway_get_sign_method(const GatewaySubDeviceState *sub_device) {
    if (sub_device != NULL && sub_device->config.sign_method[0] != '\0' &&
        !gateway_equals_ignore_case(sub_device->config.sign_method, "hmacsha256") &&
        !gateway_equals_ignore_case(sub_device->config.sign_method, "hmacSha256")) {
        fprintf(stderr,
                "subDevice %s/%s signMethod=%s not supported yet, fallback to hmacSha256\n",
                sub_device->config.product_key,
                sub_device->config.device_name,
                sub_device->config.sign_method);
    }

    return "hmacSha256";
}

static void gateway_build_topo_topic(GatewayManager *manager, const char *suffix, char *topic, size_t topic_size) {
    snprintf(topic,
             topic_size,
             "/sys/%s/%s/thing/topo/%s",
             manager->app->config.product_key,
             manager->app->config.device_name,
             suffix);
}

static void gateway_build_list_found_topic(GatewayManager *manager, char *topic, size_t topic_size) {
    snprintf(topic,
             topic_size,
             "/sys/%s/%s/thing/list/found",
             manager->app->config.product_key,
             manager->app->config.device_name);
}

static void gateway_build_combine_topic(GatewayManager *manager, const char *suffix, char *topic, size_t topic_size) {
    snprintf(topic,
             topic_size,
             "/ext/session/%s/%s/combine/%s",
             manager->app->config.product_key,
             manager->app->config.device_name,
             suffix);
}

static void gateway_build_sub_service_prefix(const GatewaySubDeviceState *sub_device, char *topic, size_t topic_size) {
    snprintf(topic,
             topic_size,
             "/sys/%s/%s/thing/service/",
             sub_device->config.product_key,
             sub_device->config.device_name);
}

static void gateway_build_sub_post_reply_prefix(const GatewaySubDeviceState *sub_device, char *topic, size_t topic_size) {
    snprintf(topic,
             topic_size,
             "/sys/%s/%s/thing/event/",
             sub_device->config.product_key,
             sub_device->config.device_name);
}

static int gateway_find_sub_device_index_locked(GatewayManager *manager, const char *product_key, const char *device_name) {
    size_t index;

    for (index = 0; index < manager->sub_device_count; ++index) {
        GatewaySubDeviceState *sub_device = &manager->sub_devices[index];
        if (strcmp(sub_device->config.product_key, product_key) == 0 &&
            strcmp(sub_device->config.device_name, device_name) == 0) {
            return (int)index;
        }
    }

    return -1;
}

static int gateway_find_sub_device_index(GatewayManager *manager, const char *product_key, const char *device_name) {
    int index;

    pthread_mutex_lock(&manager->lock);
    index = gateway_find_sub_device_index_locked(manager, product_key, device_name);
    pthread_mutex_unlock(&manager->lock);
    return index;
}

static int gateway_sub_device_has_secret(const GatewaySubDeviceState *sub_device) {
    return sub_device != NULL && sub_device->config.device_secret[0] != '\0';
}

static void gateway_update_topo_state(GatewayManager *manager, int index, int topo_added, int disabled, int online) {
    if (manager == NULL || index < 0 || (size_t)index >= manager->sub_device_count) {
        return;
    }

    pthread_mutex_lock(&manager->lock);
    manager->sub_devices[index].topo_added = topo_added;
    manager->sub_devices[index].disabled = disabled;
    manager->sub_devices[index].online = online;
    pthread_mutex_unlock(&manager->lock);
}

static void gateway_update_online_state(GatewayManager *manager, int index, int online) {
    if (manager == NULL || index < 0 || (size_t)index >= manager->sub_device_count) {
        return;
    }

    pthread_mutex_lock(&manager->lock);
    manager->sub_devices[index].online = online;
    pthread_mutex_unlock(&manager->lock);
}

static void gateway_store_properties(GatewayManager *manager, int index, const char *properties_json) {
    if (manager == NULL || properties_json == NULL || index < 0 || (size_t)index >= manager->sub_device_count) {
        return;
    }

    pthread_mutex_lock(&manager->lock);
    snprintf(manager->sub_devices[index].last_property_json,
             sizeof(manager->sub_devices[index].last_property_json),
             "%s",
             properties_json);
    pthread_mutex_unlock(&manager->lock);
}

static void gateway_get_properties(GatewayManager *manager, int index, char *out, size_t out_size) {
    if (manager == NULL || out == NULL || out_size == 0 || index < 0 || (size_t)index >= manager->sub_device_count) {
        return;
    }

    pthread_mutex_lock(&manager->lock);
    snprintf(out, out_size, "%s", manager->sub_devices[index].last_property_json);
    pthread_mutex_unlock(&manager->lock);
}

static int gateway_build_sign(const GatewaySubDeviceState *sub_device,
                              const char *client_id,
                              const char *timestamp,
                              char *sign_out,
                              size_t sign_out_size) {
    uint8_t sign[CORE_SHA256_DIGEST_LENGTH];
    char sign_string[CORE_SHA256_DIGEST_STRING_LENGTH];
    StringBuilder plain_text;
    int rc = -1;

    if (sub_device == NULL || client_id == NULL || timestamp == NULL || sign_out == NULL || sign_out_size == 0) {
        return -1;
    }

    if (sb_init(&plain_text, 128) != 0) {
        return -1;
    }

    sb_appendf(&plain_text,
               "clientId%sdeviceName%sproductKey%stimestamp%s",
               client_id,
               sub_device->config.device_name,
               sub_device->config.product_key,
               timestamp);
    core_hmac_sha256((const uint8_t *)plain_text.data,
                     (uint32_t)strlen(plain_text.data),
                     (const uint8_t *)sub_device->config.device_secret,
                     (uint32_t)strlen(sub_device->config.device_secret),
                     sign);
    core_hex2str(sign, CORE_SHA256_DIGEST_LENGTH, sign_string, 0);
    snprintf(sign_out, sign_out_size, "%s", sign_string);
    rc = 0;

    sb_free(&plain_text);
    return rc;
}

static int gateway_request_list_found(GatewayManager *manager) {
    char topic[320];
    StringBuilder payload;
    size_t index;
    int rc;

    if (manager == NULL || manager->sub_device_count == 0) {
        return 0;
    }

    gateway_build_list_found_topic(manager, topic, sizeof(topic));
    if (sb_init(&payload, 512) != 0) {
        return -1;
    }

    sb_appendf(&payload,
               "{\"id\":\"%lld\",\"version\":\"1.0\",\"params\":[",
               app_now_ms());
    for (index = 0; index < manager->sub_device_count; ++index) {
        GatewaySubDeviceState *sub_device = &manager->sub_devices[index];
        if (index > 0) {
            sb_append(&payload, ",");
        }
        sb_appendf(&payload,
                   "{\"productKey\":\"%s\",\"deviceName\":\"%s\"}",
                   sub_device->config.product_key,
                   sub_device->config.device_name);
    }
    sb_append(&payload, "],\"method\":\"thing.list.found\"}");

    rc = app_publish_topic(manager->app, topic, payload.data, 0);
    sb_free(&payload);
    return rc;
}

static int gateway_request_topo_get(GatewayManager *manager) {
    char topic[320];
    char payload[256];

    if (manager == NULL || manager->sub_device_count == 0) {
        return 0;
    }

    gateway_build_topo_topic(manager, "get", topic, sizeof(topic));
    snprintf(payload,
             sizeof(payload),
             "{\"id\":\"%lld\",\"version\":\"1.0\",\"params\":{},\"method\":\"thing.topo.get\"}",
             app_now_ms());
    return app_publish_topic(manager->app, topic, payload, 0);
}

static int gateway_request_topo_add(GatewayManager *manager, int index) {
    GatewaySubDeviceState *sub_device;
    char topic[320];
    char client_id[256];
    char timestamp[64];
    char sign[CORE_SHA256_DIGEST_STRING_LENGTH];
    char payload[1024];

    if (manager == NULL || index < 0 || (size_t)index >= manager->sub_device_count) {
        return -1;
    }

    sub_device = &manager->sub_devices[index];
    if (!gateway_sub_device_has_secret(sub_device)) {
        fprintf(stderr,
                "skip topo add for subDevice %s/%s, deviceSecret missing\n",
                sub_device->config.product_key,
                sub_device->config.device_name);
        return -1;
    }

    snprintf(client_id, sizeof(client_id), "%s&%s", sub_device->config.product_key, sub_device->config.device_name);
    snprintf(timestamp, sizeof(timestamp), "%lld", app_now_ms());
    if (gateway_build_sign(sub_device, client_id, timestamp, sign, sizeof(sign)) != 0) {
        return -1;
    }

    gateway_build_topo_topic(manager, "add", topic, sizeof(topic));
    snprintf(payload,
             sizeof(payload),
             "{\"id\":\"%lld\",\"version\":\"1.0\",\"params\":[{\"productKey\":\"%s\",\"deviceName\":\"%s\","
             "\"sign\":\"%s\",\"signmethod\":\"%s\",\"timestamp\":\"%s\",\"clientId\":\"%s\"}],"
             "\"method\":\"thing.topo.add\"}",
             app_now_ms(),
             sub_device->config.product_key,
             sub_device->config.device_name,
             sign,
             gateway_get_sign_method(sub_device),
             timestamp,
             client_id);
    printf("gateway topo add request for %s/%s\n", sub_device->config.product_key, sub_device->config.device_name);
    return app_publish_topic(manager->app, topic, payload, 0);
}

static int gateway_request_login(GatewayManager *manager, int index) {
    GatewaySubDeviceState *sub_device;
    char topic[320];
    char client_id[256];
    char timestamp[64];
    char sign[CORE_SHA256_DIGEST_STRING_LENGTH];
    char payload[1024];

    if (manager == NULL || index < 0 || (size_t)index >= manager->sub_device_count) {
        return -1;
    }

    sub_device = &manager->sub_devices[index];
    if (!gateway_sub_device_has_secret(sub_device)) {
        fprintf(stderr,
                "skip login for subDevice %s/%s, deviceSecret missing\n",
                sub_device->config.product_key,
                sub_device->config.device_name);
        return -1;
    }

    snprintf(client_id, sizeof(client_id), "%s&%s", sub_device->config.product_key, sub_device->config.device_name);
    snprintf(timestamp, sizeof(timestamp), "%lld", app_now_ms());
    if (gateway_build_sign(sub_device, client_id, timestamp, sign, sizeof(sign)) != 0) {
        return -1;
    }

    gateway_build_combine_topic(manager, "login", topic, sizeof(topic));
    snprintf(payload,
             sizeof(payload),
             "{\"id\":\"%lld\",\"params\":{\"productKey\":\"%s\",\"deviceName\":\"%s\",\"clientId\":\"%s\","
             "\"timestamp\":\"%s\",\"signMethod\":\"%s\",\"sign\":\"%s\",\"cleanSession\":\"false\"}}",
             app_now_ms(),
             sub_device->config.product_key,
             sub_device->config.device_name,
             client_id,
             timestamp,
             gateway_get_sign_method(sub_device),
             sign);
    printf("gateway login request for %s/%s\n", sub_device->config.product_key, sub_device->config.device_name);
    return app_publish_topic(manager->app, topic, payload, 0);
}

static int gateway_reply_notify(GatewayManager *manager, const char *suffix, const char *request_id) {
    char topic[320];
    char payload[256];
    const char *reply_id = (request_id == NULL || *request_id == '\0') ? "0" : request_id;

    if (manager == NULL) {
        return -1;
    }

    gateway_build_topo_topic(manager, suffix, topic, sizeof(topic));
    snprintf(payload, sizeof(payload), "{\"id\":\"%s\",\"code\":200,\"data\":{}}", reply_id);
    return app_publish_topic(manager->app, topic, payload, 0);
}

static int gateway_post_sub_properties(GatewayManager *manager, int index, const char *properties_json) {
    GatewaySubDeviceState *sub_device;
    char topic[320];
    StringBuilder payload;
    int rc;

    if (manager == NULL || properties_json == NULL || index < 0 || (size_t)index >= manager->sub_device_count) {
        return -1;
    }

    sub_device = &manager->sub_devices[index];
    snprintf(topic,
             sizeof(topic),
             "/sys/%s/%s/thing/event/property/post",
             sub_device->config.product_key,
             sub_device->config.device_name);

    if (sb_init(&payload, 256) != 0) {
        return -1;
    }

    sb_appendf(&payload,
               "{\"id\":\"%lld\",\"version\":\"1.0\",\"params\":%s,\"method\":\"thing.event.property.post\"}",
               app_now_ms(),
               properties_json);
    rc = app_publish_topic(manager->app, topic, payload.data, 0);
    sb_free(&payload);
    return rc;
}

static int gateway_reply_sub_service(GatewayManager *manager,
                                     int index,
                                     const char *service_path,
                                     const char *request_id,
                                     const char *data_json) {
    char topic[320];
    char payload[1536];
    GatewaySubDeviceState *sub_device;
    const char *reply_id = (request_id == NULL || *request_id == '\0') ? "0" : request_id;

    if (manager == NULL || service_path == NULL || index < 0 || (size_t)index >= manager->sub_device_count) {
        return -1;
    }

    sub_device = &manager->sub_devices[index];
    snprintf(topic,
             sizeof(topic),
             "/sys/%s/%s/thing/service/%s_reply",
             sub_device->config.product_key,
             sub_device->config.device_name,
             service_path);
    snprintf(payload,
             sizeof(payload),
             "{\"id\":\"%s\",\"code\":200,\"data\":%s}",
             reply_id,
             (data_json == NULL || *data_json == '\0') ? "{}" : data_json);
    return app_publish_topic(manager->app, topic, payload, 0);
}

int gateway_manager_init(GatewayManager *manager, struct AppContext *app) {
    size_t index;

    if (manager == NULL || app == NULL) {
        return -1;
    }

    memset(manager, 0, sizeof(*manager));
    manager->app = app;
    manager->sub_device_count = app->config.sub_device_count;
    for (index = 0; index < manager->sub_device_count; ++index) {
        manager->sub_devices[index].config = app->config.sub_devices[index];
        snprintf(manager->sub_devices[index].last_property_json,
                 sizeof(manager->sub_devices[index].last_property_json),
                 "{}");
    }

    if (pthread_mutex_init(&manager->lock, NULL) != 0) {
        return -1;
    }

    return 0;
}

void gateway_manager_cleanup(GatewayManager *manager) {
    if (manager == NULL) {
        return;
    }

    pthread_mutex_destroy(&manager->lock);
}

int gateway_manager_start(GatewayManager *manager) {
    char topic[320];
    size_t index;

    if (manager == NULL) {
        return -1;
    }

    if (manager->sub_device_count == 0) {
        printf("gateway manager: no subDevice configured\n");
        return 0;
    }

    gateway_build_topo_topic(manager, "#", topic, sizeof(topic));
    if (app_subscribe_topic(manager->app, topic) != 0) {
        return -1;
    }

    gateway_build_list_found_topic(manager, topic, sizeof(topic));
    strncat(topic, "_reply", sizeof(topic) - strlen(topic) - 1);
    if (app_subscribe_topic(manager->app, topic) != 0) {
        return -1;
    }

    gateway_build_combine_topic(manager, "#", topic, sizeof(topic));
    if (app_subscribe_topic(manager->app, topic) != 0) {
        return -1;
    }

    for (index = 0; index < manager->sub_device_count; ++index) {
        GatewaySubDeviceState *sub_device = &manager->sub_devices[index];

        snprintf(topic,
                 sizeof(topic),
                 "/sys/%s/%s/thing/service/#",
                 sub_device->config.product_key,
                 sub_device->config.device_name);
        if (app_subscribe_topic(manager->app, topic) != 0) {
            return -1;
        }

        snprintf(topic,
                 sizeof(topic),
                 "/sys/%s/%s/thing/event/+/post_reply",
                 sub_device->config.product_key,
                 sub_device->config.device_name);
        if (app_subscribe_topic(manager->app, topic) != 0) {
            return -1;
        }
    }

    if (gateway_request_list_found(manager) != 0) {
        return -1;
    }

    return gateway_request_topo_get(manager);
}

static void gateway_handle_topo_get_reply(GatewayManager *manager, const char *payload) {
    char code_raw[64];
    char data_array[4096];
    int item_count;
    int index;

    if (json_get_raw_value(payload, "code", code_raw, sizeof(code_raw)) != 0 || atoi(code_raw) != 200) {
        fprintf(stderr, "gateway topo get failed: %s\n", payload);
        return;
    }

    pthread_mutex_lock(&manager->lock);
    for (index = 0; (size_t)index < manager->sub_device_count; ++index) {
        manager->sub_devices[index].topo_added = 0;
        manager->sub_devices[index].disabled = 0;
        manager->sub_devices[index].online = 0;
    }
    pthread_mutex_unlock(&manager->lock);

    if (json_get_array(payload, "data", data_array, sizeof(data_array)) == 0) {
        item_count = json_array_size(data_array);
        for (index = 0; index < item_count; ++index) {
            char item_json[512];
            char product_key[DEVICE_CONFIG_TEXT_SIZE];
            char device_name[DEVICE_CONFIG_TEXT_SIZE];
            int sub_index;

            if (json_array_get_item(data_array, (size_t)index, item_json, sizeof(item_json)) != 0) {
                continue;
            }
            if (json_get_string(item_json, "productKey", product_key, sizeof(product_key)) != 0 ||
                json_get_string(item_json, "deviceName", device_name, sizeof(device_name)) != 0) {
                continue;
            }

            sub_index = gateway_find_sub_device_index(manager, product_key, device_name);
            if (sub_index >= 0) {
                gateway_update_topo_state(manager, sub_index, 1, 0, 0);
            }
        }
    }

    for (index = 0; (size_t)index < manager->sub_device_count; ++index) {
        GatewaySubDeviceState snapshot;

        pthread_mutex_lock(&manager->lock);
        snapshot = manager->sub_devices[index];
        pthread_mutex_unlock(&manager->lock);

        if (snapshot.disabled) {
            continue;
        }

        if (snapshot.topo_added) {
            if (gateway_sub_device_has_secret(&snapshot)) {
                (void)gateway_request_login(manager, index);
            } else {
                fprintf(stderr,
                        "subDevice %s/%s already in topo but deviceSecret missing, skip login\n",
                        snapshot.config.product_key,
                        snapshot.config.device_name);
            }
        } else if (gateway_sub_device_has_secret(&snapshot)) {
            (void)gateway_request_topo_add(manager, index);
        } else {
            fprintf(stderr,
                    "subDevice %s/%s not in topo and deviceSecret missing, skip topo add\n",
                    snapshot.config.product_key,
                    snapshot.config.device_name);
        }
    }
}

static void gateway_handle_topo_add_reply(GatewayManager *manager, const char *payload) {
    char code_raw[64];
    char data_array[4096];
    int item_count;
    int index;

    if (json_get_raw_value(payload, "code", code_raw, sizeof(code_raw)) != 0 || atoi(code_raw) != 200) {
        fprintf(stderr, "gateway topo add failed: %s\n", payload);
        return;
    }

    if (json_get_array(payload, "data", data_array, sizeof(data_array)) != 0) {
        return;
    }

    item_count = json_array_size(data_array);
    for (index = 0; index < item_count; ++index) {
        char item_json[512];
        char product_key[DEVICE_CONFIG_TEXT_SIZE];
        char device_name[DEVICE_CONFIG_TEXT_SIZE];
        int sub_index;

        if (json_array_get_item(data_array, (size_t)index, item_json, sizeof(item_json)) != 0) {
            continue;
        }
        if (json_get_string(item_json, "productKey", product_key, sizeof(product_key)) != 0 ||
            json_get_string(item_json, "deviceName", device_name, sizeof(device_name)) != 0) {
            continue;
        }

        sub_index = gateway_find_sub_device_index(manager, product_key, device_name);
        if (sub_index >= 0) {
            gateway_update_topo_state(manager, sub_index, 1, 0, 0);
            (void)gateway_request_login(manager, sub_index);
        }
    }
}

static void gateway_handle_topo_add_notify(GatewayManager *manager, const char *payload) {
    char request_id[64] = "0";
    char params_array[4096];
    int item_count;
    int index;

    (void)json_get_raw_value(payload, "id", request_id, sizeof(request_id));
    (void)gateway_reply_notify(manager, "add/notify_reply", request_id);

    if (json_get_array(payload, "params", params_array, sizeof(params_array)) != 0) {
        return;
    }

    item_count = json_array_size(params_array);
    for (index = 0; index < item_count; ++index) {
        char item_json[512];
        char product_key[DEVICE_CONFIG_TEXT_SIZE];
        char device_name[DEVICE_CONFIG_TEXT_SIZE];
        int sub_index;

        if (json_array_get_item(params_array, (size_t)index, item_json, sizeof(item_json)) != 0) {
            continue;
        }
        if (json_get_string(item_json, "productKey", product_key, sizeof(product_key)) != 0 ||
            json_get_string(item_json, "deviceName", device_name, sizeof(device_name)) != 0) {
            continue;
        }

        sub_index = gateway_find_sub_device_index(manager, product_key, device_name);
        if (sub_index < 0) {
            fprintf(stderr, "topo add notify for unknown subDevice %s/%s\n", product_key, device_name);
            continue;
        }
        if (!gateway_sub_device_has_secret(&manager->sub_devices[sub_index])) {
            fprintf(stderr, "topo add notify skip %s/%s, deviceSecret missing\n", product_key, device_name);
            continue;
        }
        (void)gateway_request_topo_add(manager, sub_index);
    }
}

static void gateway_handle_topo_change(GatewayManager *manager, const char *payload) {
    char request_id[64] = "0";
    char params_json[4096];
    char status_raw[64];
    char sub_list[4096];
    int status;
    int item_count;
    int index;

    (void)json_get_raw_value(payload, "id", request_id, sizeof(request_id));
    (void)gateway_reply_notify(manager, "change_reply", request_id);

    if (json_get_object(payload, "params", params_json, sizeof(params_json)) != 0 ||
        json_get_raw_value(params_json, "status", status_raw, sizeof(status_raw)) != 0 ||
        json_get_array(params_json, "subList", sub_list, sizeof(sub_list)) != 0) {
        return;
    }

    status = atoi(status_raw);
    item_count = json_array_size(sub_list);
    for (index = 0; index < item_count; ++index) {
        char item_json[512];
        char product_key[DEVICE_CONFIG_TEXT_SIZE];
        char device_name[DEVICE_CONFIG_TEXT_SIZE];
        int sub_index;

        if (json_array_get_item(sub_list, (size_t)index, item_json, sizeof(item_json)) != 0) {
            continue;
        }
        if (json_get_string(item_json, "productKey", product_key, sizeof(product_key)) != 0 ||
            json_get_string(item_json, "deviceName", device_name, sizeof(device_name)) != 0) {
            continue;
        }

        sub_index = gateway_find_sub_device_index(manager, product_key, device_name);
        if (sub_index < 0) {
            continue;
        }

        if (status == 0 || status == 2) {
            gateway_update_topo_state(manager, sub_index, 1, 0, 0);
            if (gateway_sub_device_has_secret(&manager->sub_devices[sub_index])) {
                (void)gateway_request_login(manager, sub_index);
            }
        } else if (status == 1) {
            gateway_update_topo_state(manager, sub_index, 0, 0, 0);
        } else if (status == 8) {
            gateway_update_topo_state(manager, sub_index, 1, 1, 0);
        }
    }
}

static void gateway_handle_combine_reply(GatewayManager *manager, const char *topic, const char *payload) {
    char code_raw[64];
    char data_json[512];
    char product_key[DEVICE_CONFIG_TEXT_SIZE];
    char device_name[DEVICE_CONFIG_TEXT_SIZE];
    int sub_index;
    int is_login;

    if (json_get_raw_value(payload, "code", code_raw, sizeof(code_raw)) != 0 || atoi(code_raw) != 200) {
        fprintf(stderr, "gateway combine reply failed topic=%s payload=%s\n", topic, payload);
        return;
    }

    if (json_get_object(payload, "data", data_json, sizeof(data_json)) != 0 ||
        json_get_string(data_json, "productKey", product_key, sizeof(product_key)) != 0 ||
        json_get_string(data_json, "deviceName", device_name, sizeof(device_name)) != 0) {
        return;
    }

    sub_index = gateway_find_sub_device_index(manager, product_key, device_name);
    if (sub_index < 0) {
        return;
    }

    is_login = strstr(topic, "/login_reply") != NULL;
    gateway_update_online_state(manager, sub_index, is_login ? 1 : 0);
    printf("gateway %s success for %s/%s\n",
           is_login ? "login" : "logout",
           product_key,
           device_name);
}

static int gateway_handle_sub_service_message(GatewayManager *manager, const char *topic, const char *payload) {
    size_t index;

    for (index = 0; index < manager->sub_device_count; ++index) {
        GatewaySubDeviceState *sub_device = &manager->sub_devices[index];
        char prefix[320];
        const char *service_path;
        char request_id[64] = "0";

        gateway_build_sub_service_prefix(sub_device, prefix, sizeof(prefix));
        if (strncmp(topic, prefix, strlen(prefix)) != 0) {
            continue;
        }

        service_path = topic + strlen(prefix);
        (void)json_get_raw_value(payload, "id", request_id, sizeof(request_id));

        if (strcmp(service_path, "property/set") == 0) {
            char params_json[1024] = "{}";

            if (json_get_object(payload, "params", params_json, sizeof(params_json)) == 0) {
                gateway_store_properties(manager, (int)index, params_json);
                (void)gateway_post_sub_properties(manager, (int)index, params_json);
            }
            (void)gateway_reply_sub_service(manager, (int)index, service_path, request_id, "{}");
        } else if (strcmp(service_path, "property/get") == 0) {
            char cached_properties[1024];

            gateway_get_properties(manager, (int)index, cached_properties, sizeof(cached_properties));
            (void)gateway_reply_sub_service(manager, (int)index, service_path, request_id, cached_properties);
        } else {
            (void)gateway_reply_sub_service(manager, (int)index, service_path, request_id, "{}");
        }

        return 1;
    }

    return 0;
}

static int gateway_handle_sub_post_reply(GatewayManager *manager, const char *topic, const char *payload) {
    size_t index;

    for (index = 0; index < manager->sub_device_count; ++index) {
        GatewaySubDeviceState *sub_device = &manager->sub_devices[index];
        char prefix[320];

        gateway_build_sub_post_reply_prefix(sub_device, prefix, sizeof(prefix));
        if (strncmp(topic, prefix, strlen(prefix)) == 0 && strstr(topic, "/post_reply") != NULL) {
            printf("subDevice post reply topic=%s payload=%s\n", topic, payload);
            return 1;
        }
    }

    return 0;
}

int gateway_manager_handle_message(GatewayManager *manager, const char *topic, const char *payload) {
    char expected_topic[320];

    if (manager == NULL || topic == NULL || payload == NULL || manager->sub_device_count == 0) {
        return 0;
    }

    gateway_build_topo_topic(manager, "get_reply", expected_topic, sizeof(expected_topic));
    if (strcmp(topic, expected_topic) == 0) {
        gateway_handle_topo_get_reply(manager, payload);
        return 1;
    }

    gateway_build_topo_topic(manager, "add_reply", expected_topic, sizeof(expected_topic));
    if (strcmp(topic, expected_topic) == 0) {
        gateway_handle_topo_add_reply(manager, payload);
        return 1;
    }

    gateway_build_topo_topic(manager, "add/notify", expected_topic, sizeof(expected_topic));
    if (strcmp(topic, expected_topic) == 0) {
        gateway_handle_topo_add_notify(manager, payload);
        return 1;
    }

    gateway_build_topo_topic(manager, "change", expected_topic, sizeof(expected_topic));
    if (strcmp(topic, expected_topic) == 0) {
        gateway_handle_topo_change(manager, payload);
        return 1;
    }

    gateway_build_list_found_topic(manager, expected_topic, sizeof(expected_topic));
    strncat(expected_topic, "_reply", sizeof(expected_topic) - strlen(expected_topic) - 1);
    if (strcmp(topic, expected_topic) == 0) {
        printf("gateway list/found reply payload=%s\n", payload);
        return 1;
    }

    gateway_build_combine_topic(manager, "login_reply", expected_topic, sizeof(expected_topic));
    if (strcmp(topic, expected_topic) == 0) {
        gateway_handle_combine_reply(manager, topic, payload);
        return 1;
    }

    gateway_build_combine_topic(manager, "logout_reply", expected_topic, sizeof(expected_topic));
    if (strcmp(topic, expected_topic) == 0) {
        gateway_handle_combine_reply(manager, topic, payload);
        return 1;
    }

    if (gateway_handle_sub_service_message(manager, topic, payload)) {
        return 1;
    }

    if (gateway_handle_sub_post_reply(manager, topic, payload)) {
        return 1;
    }

    return 0;
}
