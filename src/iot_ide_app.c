#include "iot_ide_app.h"

#include "aiot_state_api.h"
#include "aiot_sysdep_api.h"
#include "json_utils.h"
#include "string_builder.h"

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

extern aiot_sysdep_portfile_t g_aiot_sysdep_portfile;
extern const char *ali_ca_cert;

static int32_t app_state_logcb(int32_t code, char *message) {
    (void)code;
    if (message != NULL) {
        printf("%s", message);
    }
    return 0;
}

long long app_now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000LL + (long long)tv.tv_usec / 1000LL;
}

int app_publish_topic(AppContext *app, const char *topic, const char *payload, uint8_t qos) {
    int32_t res;

    if (app == NULL || app->mqtt_handle == NULL || topic == NULL || payload == NULL) {
        return -1;
    }

    pthread_mutex_lock(&app->mqtt_lock);
    res = aiot_mqtt_pub(app->mqtt_handle, (char *)topic, (uint8_t *)payload, (uint32_t)strlen(payload), qos);
    pthread_mutex_unlock(&app->mqtt_lock);

    if (res < STATE_SUCCESS) {
        fprintf(stderr, "mqtt publish failed, topic=%s, code=-0x%04X\n", topic, -res);
        return -1;
    }

    return 0;
}

int app_subscribe_topic(AppContext *app, const char *topic) {
    int32_t res;

    pthread_mutex_lock(&app->mqtt_lock);
    res = aiot_mqtt_sub(app->mqtt_handle, (char *)topic, NULL, 1, NULL);
    pthread_mutex_unlock(&app->mqtt_lock);

    if (res < STATE_SUCCESS) {
        fprintf(stderr, "mqtt subscribe failed, topic=%s, code=-0x%04X\n", topic, -res);
        return -1;
    }

    printf("subscribed: %s\n", topic);
    return 0;
}

static int app_build_simple_result_json(int success, const char *message, char *buffer, size_t buffer_size) {
    char escaped[512];

    if (buffer == NULL || buffer_size == 0) {
        return -1;
    }

    if (json_escape_string(message == NULL ? "" : message, escaped, sizeof(escaped)) != 0) {
        return -1;
    }

    return (snprintf(buffer, buffer_size, "{\"success\":%d,\"message\":\"%s\"}", success ? 1 : 0, escaped) >= 0) ? 0 : -1;
}

int app_post_properties(AppContext *app, const char *params_json) {
    char topic[320];
    StringBuilder payload;
    int rc;

    if (app == NULL || params_json == NULL) {
        return -1;
    }

    if (sb_init(&payload, 512) != 0) {
        return -1;
    }

    snprintf(topic, sizeof(topic), "/sys/%s/%s/thing/event/property/post", app->config.product_key, app->config.device_name);
    sb_appendf(&payload,
               "{\"id\":\"%lld\",\"version\":\"1.0\",\"params\":%s,\"method\":\"thing.event.property.post\"}",
               app_now_ms(),
               params_json);
    rc = app_publish_topic(app, topic, payload.data, 0);
    sb_free(&payload);
    return rc;
}

int app_reply_service(AppContext *app, const char *service_path, const char *request_id, int code, const char *data_json) {
    char topic[320];
    StringBuilder payload;
    const char *reply_id = (request_id == NULL || *request_id == '\0') ? "0" : request_id;
    int rc;

    if (app == NULL || service_path == NULL) {
        return -1;
    }

    if (sb_init(&payload, 512) != 0) {
        return -1;
    }

    snprintf(topic, sizeof(topic), "/sys/%s/%s/thing/service/%s_reply", app->config.product_key, app->config.device_name, service_path);
    sb_appendf(&payload,
               "{\"id\":\"%s\",\"code\":%d,\"data\":%s}",
               reply_id,
               code,
               (data_json == NULL || *data_json == '\0') ? "{}" : data_json);
    rc = app_publish_topic(app, topic, payload.data, 0);
    sb_free(&payload);
    return rc;
}

int app_publish_trace(AppContext *app, const char *payload_json) {
    char topic[320];
    snprintf(topic, sizeof(topic), "/%s/%s/user/trace/data", app->config.product_key, app->config.device_name);
    return app_publish_topic(app, topic, payload_json, 0);
}

static void app_handle_property_set(AppContext *app, const char *request_id, const char *payload) {
    char params[2048] = "{}";
    char adas_value[64];

    if (json_get_object(payload, "params", params, sizeof(params)) != 0) {
        snprintf(params, sizeof(params), "{}");
    }

    if (json_get_raw_value(params, "ADASSwitch", adas_value, sizeof(adas_value)) == 0) {
        app->adas_switch = atoi(adas_value) != 0;
    }

    (void)app_post_properties(app, params);
    (void)device_shadow_manager_update_reported(&app->shadow_manager, params);
    (void)app_reply_service(app, "property/set", request_id, 200, "{}");
}

static void app_handle_property_get(AppContext *app, const char *request_id) {
    int connected = 0;
    char client_id[128] = "";
    char escaped_client_id[256] = "";
    char data_json[1024];
    long long heartbeat_ms = 0;

    ide_connection_manager_get_snapshot(&app->ide_manager, &connected, client_id, sizeof(client_id), &heartbeat_ms);
    (void)json_escape_string(client_id, escaped_client_id, sizeof(escaped_client_id));

    snprintf(data_json, sizeof(data_json),
             "{\"ADASSwitch\":%d,\"hasIDEConnected\":%d,\"IDEInfo\":\"%s\",\"IDEHeartbeat\":\"%lld\"}",
             app->adas_switch ? 1 : 0,
             connected ? 1 : 0,
             escaped_client_id,
             heartbeat_ms);
    (void)app_reply_service(app, "property/get", request_id, 200, data_json);
}

static void app_handle_connect_service(AppContext *app, const char *request_id, const char *payload) {
    char params[2048] = "{}";
    char response[512] = "{\"success\":0,\"message\":\"requestConnect failed\"}";

    if (json_get_object(payload, "params", params, sizeof(params)) == 0) {
        (void)ide_connection_manager_handle_request_connect(&app->ide_manager, params, response, sizeof(response));
    }
    (void)app_reply_service(app, "requestConnect", request_id, 200, response);
}

static void app_handle_disconnect_service(AppContext *app, const char *request_id, const char *payload) {
    char params[1024] = "{}";
    char response[512] = "{\"success\":0,\"message\":\"requestDisconnect failed\"}";

    if (json_get_object(payload, "params", params, sizeof(params)) == 0) {
        (void)ide_connection_manager_handle_disconnect(&app->ide_manager, params, response, sizeof(response));
    }
    (void)app_reply_service(app, "requestDisconnect", request_id, 200, response);
}

static void app_handle_heartbeat_service(AppContext *app, const char *request_id, const char *payload) {
    char params[1024] = "{}";
    char response[512] = "{\"success\":0,\"message\":\"ideHeartbeat failed\"}";

    if (json_get_object(payload, "params", params, sizeof(params)) == 0) {
        (void)ide_connection_manager_handle_heartbeat(&app->ide_manager, params, response, sizeof(response));
    }
    (void)app_reply_service(app, "ideHeartbeat", request_id, 200, response);
}

static void app_handle_deploy_service(AppContext *app, const char *request_id, const char *payload) {
    char params[4096] = "{}";
    char client_id[128] = "";
    char response[1024] = "{\"success\":0,\"message\":\"deployProject failed\",\"deployLog\":\"\"}";

    if (json_get_object(payload, "params", params, sizeof(params)) != 0) {
        (void)app_reply_service(app, "deployProject", request_id, 200, response);
        return;
    }

    (void)json_get_string(params, "clientId", client_id, sizeof(client_id));
    if (!ide_connection_manager_is_connected_client(&app->ide_manager, client_id)) {
        (void)app_build_simple_result_json(0, "deploy rejected, clientId is not the active IDE connection", response, sizeof(response));
        (void)app_reply_service(app, "deployProject", request_id, 200, response);
        return;
    }

    (void)deploy_manager_handle_deploy(&app->deploy_manager, params, response, sizeof(response));
    (void)app_reply_service(app, "deployProject", request_id, 200, response);
}

static void app_handle_start_service(AppContext *app, const char *request_id, const char *payload) {
    char params[2048] = "{}";
    char client_id[128] = "";
    char response[1024] = "{\"success\":0,\"message\":\"startProject failed\"}";

    if (json_get_object(payload, "params", params, sizeof(params)) != 0) {
        (void)app_reply_service(app, "startProject", request_id, 200, response);
        return;
    }

    (void)json_get_string(params, "clientId", client_id, sizeof(client_id));
    if (!ide_connection_manager_is_connected_client(&app->ide_manager, client_id)) {
        (void)app_build_simple_result_json(0, "start rejected, clientId is not the active IDE connection", response, sizeof(response));
        (void)app_reply_service(app, "startProject", request_id, 200, response);
        return;
    }

    (void)start_manager_handle_start(&app->start_manager, params, response, sizeof(response));
    (void)app_reply_service(app, "startProject", request_id, 200, response);
}

static void app_dispatch_service(AppContext *app, const char *topic, const char *payload) {
    char prefix[320];
    const char *service_path;
    char request_id[64] = "0";

    snprintf(prefix, sizeof(prefix), "/sys/%s/%s/thing/service/", app->config.product_key, app->config.device_name);
    if (strncmp(topic, prefix, strlen(prefix)) != 0) {
        return;
    }

    service_path = topic + strlen(prefix);
    (void)json_get_raw_value(payload, "id", request_id, sizeof(request_id));

    if (strcmp(service_path, "property/set") == 0) {
        app_handle_property_set(app, request_id, payload);
    } else if (strcmp(service_path, "property/get") == 0) {
        app_handle_property_get(app, request_id);
    } else if (strcmp(service_path, "requestConnect") == 0) {
        app_handle_connect_service(app, request_id, payload);
    } else if (strcmp(service_path, "requestDisconnect") == 0) {
        app_handle_disconnect_service(app, request_id, payload);
    } else if (strcmp(service_path, "ideHeartbeat") == 0) {
        app_handle_heartbeat_service(app, request_id, payload);
    } else if (strcmp(service_path, "deployProject") == 0) {
        app_handle_deploy_service(app, request_id, payload);
    } else if (strcmp(service_path, "startProject") == 0) {
        app_handle_start_service(app, request_id, payload);
    } else {
        char response[256] = "{\"success\":0,\"message\":\"unknown service\"}";
        (void)app_reply_service(app, service_path, request_id, 200, response);
    }
}

static void app_mqtt_recv_handler(void *handle, const aiot_mqtt_recv_t *packet, void *userdata) {
    AppContext *app = (AppContext *)userdata;
    (void)handle;

    if (app == NULL || packet == NULL) {
        return;
    }

    switch (packet->type) {
        case AIOT_MQTTRECV_HEARTBEAT_RESPONSE:
            printf("heartbeat response\n");
            break;
        case AIOT_MQTTRECV_SUB_ACK:
            printf("suback packet id=%d max_qos=%d\n", packet->data.sub_ack.packet_id, packet->data.sub_ack.max_qos);
            break;
        case AIOT_MQTTRECV_PUB_ACK:
            printf("puback packet id=%d\n", packet->data.pub_ack.packet_id);
            break;
        case AIOT_MQTTRECV_PUB: {
            size_t topic_len = packet->data.pub.topic_len;
            size_t payload_len = packet->data.pub.payload_len;
            char *topic = (char *)malloc(topic_len + 1);
            char *payload = (char *)malloc(payload_len + 1);
            if (topic == NULL || payload == NULL) {
                free(topic);
                free(payload);
                return;
            }

            memcpy(topic, packet->data.pub.topic, topic_len);
            topic[topic_len] = '\0';
            memcpy(payload, packet->data.pub.payload, payload_len);
            payload[payload_len] = '\0';

            printf("mqtt pub topic=%s\n", topic);
            printf("mqtt pub payload=%s\n", payload);
            if (!device_shadow_manager_handle_message(&app->shadow_manager, topic, payload) &&
                !gateway_manager_handle_message(&app->gateway_manager, topic, payload)) {
                app_dispatch_service(app, topic, payload);
            }

            free(topic);
            free(payload);
            break;
        }
        default:
            break;
    }
}

static void app_mqtt_event_handler(void *handle, const aiot_mqtt_event_t *event, void *userdata) {
    (void)handle;
    (void)userdata;

    if (event == NULL) {
        return;
    }

    switch (event->type) {
        case AIOT_MQTTEVT_CONNECT:
            printf("AIOT_MQTTEVT_CONNECT\n");
            break;
        case AIOT_MQTTEVT_RECONNECT:
            printf("AIOT_MQTTEVT_RECONNECT\n");
            break;
        case AIOT_MQTTEVT_DISCONNECT:
            printf("AIOT_MQTTEVT_DISCONNECT\n");
            break;
        default:
            break;
    }
}

static void *app_mqtt_process_thread_main(void *arg) {
    AppContext *app = (AppContext *)arg;

    while (app->mqtt_process_thread_running) {
        int32_t res = aiot_mqtt_process(app->mqtt_handle);
        if (res == STATE_USER_INPUT_EXEC_DISABLED) {
            break;
        }
        sleep(1);
    }
    return NULL;
}

static void *app_mqtt_recv_thread_main(void *arg) {
    AppContext *app = (AppContext *)arg;

    while (app->mqtt_recv_thread_running) {
        int32_t res = aiot_mqtt_recv(app->mqtt_handle);
        if (res < STATE_SUCCESS) {
            if (res == STATE_USER_INPUT_EXEC_DISABLED) {
                break;
            }
            sleep(1);
        }
    }
    return NULL;
}

int app_init(AppContext *app, const char *config_path) {
    int ide_initialized = 0;
    int deploy_initialized = 0;
    int start_initialized = 0;
    int shadow_initialized = 0;
    int gateway_initialized = 0;

    if (app == NULL || config_path == NULL) {
        return -1;
    }

    memset(app, 0, sizeof(*app));
    app->running = 1;
    app->adas_switch = 0;

    if (pthread_mutex_init(&app->mqtt_lock, NULL) != 0) {
        return -1;
    }

    if (device_config_load(config_path, &app->config) != 0) {
        pthread_mutex_destroy(&app->mqtt_lock);
        return -1;
    }

    if (device_config_build_mqtt_host(&app->config, app->mqtt_host, sizeof(app->mqtt_host)) != 0) {
        pthread_mutex_destroy(&app->mqtt_lock);
        return -1;
    }

    if (ide_connection_manager_init(&app->ide_manager, app) != 0) {
        pthread_mutex_destroy(&app->mqtt_lock);
        return -1;
    }
    ide_initialized = 1;

    if (deploy_manager_init(&app->deploy_manager, app) != 0) {
        goto init_failed;
    }
    deploy_initialized = 1;

    if (start_manager_init(&app->start_manager, app) != 0) {
        goto init_failed;
    }
    start_initialized = 1;

    if (device_shadow_manager_init(&app->shadow_manager, app) != 0) {
        goto init_failed;
    }
    shadow_initialized = 1;

    if (gateway_manager_init(&app->gateway_manager, app) != 0) {
        goto init_failed;
    }
    gateway_initialized = 1;

    if (trace_simulator_init(&app->trace_simulator, app) != 0) {
        goto init_failed;
    }

    return 0;

init_failed:
    if (gateway_initialized) {
        gateway_manager_cleanup(&app->gateway_manager);
    }
    if (shadow_initialized) {
        device_shadow_manager_cleanup(&app->shadow_manager);
    }
    if (start_initialized) {
        start_manager_cleanup(&app->start_manager);
    }
    if (deploy_initialized) {
        deploy_manager_cleanup(&app->deploy_manager);
    }
    if (ide_initialized) {
        ide_connection_manager_cleanup(&app->ide_manager);
    }
    pthread_mutex_destroy(&app->mqtt_lock);
    return -1;
}

int app_start(AppContext *app) {
    aiot_sysdep_network_cred_t cred;
    uint16_t port = 8883;
    int32_t res;
    char sub_topic_reply[320];
    char sub_topic_service[320];

    if (app == NULL) {
        return -1;
    }

    aiot_sysdep_set_portfile(&g_aiot_sysdep_portfile);
    aiot_state_set_logcb(app_state_logcb);

    memset(&cred, 0, sizeof(cred));
    cred.option = AIOT_SYSDEP_NETWORK_CRED_SVRCERT_CA;
    cred.max_tls_fragment = 16384;
    cred.sni_enabled = 1;
    cred.x509_server_cert = ali_ca_cert;
    cred.x509_server_cert_len = strlen(ali_ca_cert);

    app->mqtt_handle = aiot_mqtt_init();
    if (app->mqtt_handle == NULL) {
        return -1;
    }

    aiot_mqtt_setopt(app->mqtt_handle, AIOT_MQTTOPT_HOST, (void *)app->mqtt_host);
    aiot_mqtt_setopt(app->mqtt_handle, AIOT_MQTTOPT_PORT, (void *)&port);
    aiot_mqtt_setopt(app->mqtt_handle, AIOT_MQTTOPT_PRODUCT_KEY, (void *)app->config.product_key);
    aiot_mqtt_setopt(app->mqtt_handle, AIOT_MQTTOPT_DEVICE_NAME, (void *)app->config.device_name);
    aiot_mqtt_setopt(app->mqtt_handle, AIOT_MQTTOPT_DEVICE_SECRET, (void *)app->config.device_secret);
    aiot_mqtt_setopt(app->mqtt_handle, AIOT_MQTTOPT_NETWORK_CRED, (void *)&cred);
    aiot_mqtt_setopt(app->mqtt_handle, AIOT_MQTTOPT_RECV_HANDLER, (void *)app_mqtt_recv_handler);
    aiot_mqtt_setopt(app->mqtt_handle, AIOT_MQTTOPT_EVENT_HANDLER, (void *)app_mqtt_event_handler);
    aiot_mqtt_setopt(app->mqtt_handle, AIOT_MQTTOPT_USERDATA, (void *)app);

    res = aiot_mqtt_connect(app->mqtt_handle);
    if (res < STATE_SUCCESS) {
        fprintf(stderr, "aiot_mqtt_connect failed: -0x%04X\n", -res);
        return -1;
    }

    snprintf(sub_topic_reply, sizeof(sub_topic_reply), "/sys/%s/%s/thing/event/+/post_reply", app->config.product_key, app->config.device_name);
    snprintf(sub_topic_service, sizeof(sub_topic_service), "/sys/%s/%s/thing/service/#", app->config.product_key, app->config.device_name);

    if (app_subscribe_topic(app, sub_topic_reply) != 0 || app_subscribe_topic(app, sub_topic_service) != 0) {
        return -1;
    }

    app->mqtt_process_thread_running = 1;
    app->mqtt_recv_thread_running = 1;
    if (pthread_create(&app->mqtt_process_thread, NULL, app_mqtt_process_thread_main, app) != 0) {
        return -1;
    }
    if (pthread_create(&app->mqtt_recv_thread, NULL, app_mqtt_recv_thread_main, app) != 0) {
        return -1;
    }

    ide_connection_manager_clear_cloud_state(&app->ide_manager);
    (void)app_post_properties(app, "{\"ADASSwitch\":0}");
    (void)device_shadow_manager_update_reported(&app->shadow_manager, "{\"ADASSwitch\":0}");
    if (device_shadow_manager_start(&app->shadow_manager) != 0) {
        return -1;
    }
    if (gateway_manager_start(&app->gateway_manager) != 0) {
        return -1;
    }
    return 0;
}

void app_shutdown(AppContext *app) {
    int join_process = 0;
    int join_recv = 0;

    if (app == NULL) {
        return;
    }

    app->running = 0;
    trace_simulator_stop(&app->trace_simulator);

    join_process = app->mqtt_process_thread_running;
    join_recv = app->mqtt_recv_thread_running;
    app->mqtt_process_thread_running = 0;
    app->mqtt_recv_thread_running = 0;

    if (app->mqtt_handle != NULL) {
        aiot_mqtt_disconnect(app->mqtt_handle);
    }

    if (join_process) {
        pthread_join(app->mqtt_process_thread, NULL);
    }
    if (join_recv) {
        pthread_join(app->mqtt_recv_thread, NULL);
    }

    if (app->mqtt_handle != NULL) {
        aiot_mqtt_deinit(&app->mqtt_handle);
    }

    ide_connection_manager_cleanup(&app->ide_manager);
    deploy_manager_cleanup(&app->deploy_manager);
    start_manager_cleanup(&app->start_manager);
    gateway_manager_cleanup(&app->gateway_manager);
    device_shadow_manager_cleanup(&app->shadow_manager);
    pthread_mutex_destroy(&app->mqtt_lock);
}

