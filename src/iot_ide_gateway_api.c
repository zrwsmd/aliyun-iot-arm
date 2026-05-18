#include "iot_ide_gateway_api.h"

#include "aiot_mqtt_api.h"
#include "aiot_state_api.h"
#include "aiot_sysdep_api.h"
#include "device_config.h"
#include "json_utils.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

extern aiot_sysdep_portfile_t g_aiot_sysdep_portfile;
extern const char *ali_ca_cert;

struct IotIdeGateway {
    DeviceConfig config;
    char config_path[512];
    char mqtt_host[256];
    void *mqtt_handle;
    pthread_mutex_t mqtt_lock;
    pthread_t mqtt_process_thread;
    pthread_t mqtt_recv_thread;
    int mqtt_process_thread_running;
    int mqtt_recv_thread_running;
    int mqtt_lock_initialized;
    int started;
    IotIdeGatewayCallbacks callbacks;
    void *user_data;
};

static void gateway_format_time(char *buffer, size_t buffer_size) {
    time_t now;
    struct tm tm_value;

    if (buffer == NULL || buffer_size == 0) {
        return;
    }

    now = time(NULL);
    if (localtime_r(&now, &tm_value) == NULL) {
        snprintf(buffer, buffer_size, "unknown-time");
        return;
    }

    strftime(buffer, buffer_size, "%Y-%m-%d %H:%M:%S", &tm_value);
}

static void gateway_emit_log(IotIdeGateway *gateway, int level, const char *message) {
    if (gateway != NULL && gateway->callbacks.on_log != NULL) {
        gateway->callbacks.on_log(gateway->user_data, level, message == NULL ? "" : message);
        return;
    }

    if (message != NULL && message[0] != '\0') {
        char time_text[32];
        gateway_format_time(time_text, sizeof(time_text));
        fprintf(stderr, "[%s] %s\n", time_text, message);
    }
}

static void gateway_emit_logf(IotIdeGateway *gateway, int level, const char *fmt, ...) {
    char message[512];
    va_list args;

    if (fmt == NULL) {
        return;
    }

    va_start(args, fmt);
    vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);
    gateway_emit_log(gateway, level, message);
}

static int32_t gateway_state_logcb(int32_t code, char *message) {
    char time_text[32];
    (void)code;
    if (message != NULL) {
        gateway_format_time(time_text, sizeof(time_text));
        printf("[%s] %s", time_text, message);
    }
    return 0;
}

static long long gateway_now_ms(void) {
    return (long long)time(NULL) * 1000LL;
}

static int gateway_publish_topic(IotIdeGateway *gateway, const char *topic, const char *payload, uint8_t qos) {
    int32_t res;

    if (gateway == NULL || gateway->mqtt_handle == NULL || topic == NULL || payload == NULL) {
        return IOT_IDE_GATEWAY_ERR_INVALID_ARGUMENT;
    }

    pthread_mutex_lock(&gateway->mqtt_lock);
    res = aiot_mqtt_pub(gateway->mqtt_handle, (char *)topic, (uint8_t *)payload, (uint32_t)strlen(payload), qos);
    pthread_mutex_unlock(&gateway->mqtt_lock);

    if (res < STATE_SUCCESS) {
        gateway_emit_logf(gateway, IOT_IDE_GATEWAY_LOG_ERROR,
                          "mqtt publish failed, topic=%s, code=-0x%04X", topic, -res);
        return IOT_IDE_GATEWAY_ERR_INTERNAL;
    }

    return IOT_IDE_GATEWAY_OK;
}

static int gateway_subscribe_topic(IotIdeGateway *gateway, const char *topic) {
    int32_t res;

    if (gateway == NULL || gateway->mqtt_handle == NULL || topic == NULL) {
        return IOT_IDE_GATEWAY_ERR_INVALID_ARGUMENT;
    }

    pthread_mutex_lock(&gateway->mqtt_lock);
    res = aiot_mqtt_sub(gateway->mqtt_handle, (char *)topic, NULL, 1, NULL);
    pthread_mutex_unlock(&gateway->mqtt_lock);

    if (res < STATE_SUCCESS) {
        gateway_emit_logf(gateway, IOT_IDE_GATEWAY_LOG_ERROR,
                          "mqtt subscribe failed, topic=%s, code=-0x%04X", topic, -res);
        return IOT_IDE_GATEWAY_ERR_INTERNAL;
    }

    gateway_emit_logf(gateway, IOT_IDE_GATEWAY_LOG_INFO, "subscribed: %s", topic);
    return IOT_IDE_GATEWAY_OK;
}

static int gateway_is_reply_topic(const char *service_name) {
    size_t len;
    const char *suffix = "_reply";
    size_t suffix_len = strlen(suffix);

    if (service_name == NULL) {
        return 0;
    }

    len = strlen(service_name);
    return len >= suffix_len && strcmp(service_name + len - suffix_len, suffix) == 0;
}

static void gateway_handle_service(IotIdeGateway *gateway, const char *topic, const char *payload) {
    char prefix[320];
    const char *service_name;
    char request_id[64] = "0";
    char params[8192] = "{}";

    if (gateway == NULL || topic == NULL || payload == NULL) {
        return;
    }

    snprintf(prefix, sizeof(prefix), "/sys/%s/%s/thing/service/",
             gateway->config.product_key,
             gateway->config.device_name);

    if (strncmp(topic, prefix, strlen(prefix)) != 0) {
        return;
    }

    service_name = topic + strlen(prefix);
    if (gateway_is_reply_topic(service_name)) {
        return;
    }

    (void)json_get_raw_value(payload, "id", request_id, sizeof(request_id));
    if (json_get_object(payload, "params", params, sizeof(params)) != 0) {
        snprintf(params, sizeof(params), "{}");
    }

    gateway_emit_logf(gateway, IOT_IDE_GATEWAY_LOG_INFO,
                      "service=%s id=%s params=%s", service_name, request_id, params);

    if (gateway->callbacks.on_service != NULL) {
        gateway->callbacks.on_service(gateway->user_data, gateway, service_name, request_id, params);
    } else {
        (void)iot_ide_gateway_reply_service(gateway, service_name, request_id, 200,
                                            "{\"success\":0,\"message\":\"no service handler\"}");
    }
}

static void gateway_mqtt_recv_handler(void *handle, const aiot_mqtt_recv_t *packet, void *userdata) {
    IotIdeGateway *gateway = (IotIdeGateway *)userdata;
    (void)handle;

    if (gateway == NULL || packet == NULL) {
        return;
    }

    switch (packet->type) {
        case AIOT_MQTTRECV_HEARTBEAT_RESPONSE:
            gateway_emit_log(gateway, IOT_IDE_GATEWAY_LOG_DEBUG, "heartbeat response");
            break;
        case AIOT_MQTTRECV_SUB_ACK:
            gateway_emit_logf(gateway, IOT_IDE_GATEWAY_LOG_INFO,
                              "suback packet id=%d max_qos=%d",
                              packet->data.sub_ack.packet_id,
                              packet->data.sub_ack.max_qos);
            break;
        case AIOT_MQTTRECV_PUB_ACK:
            gateway_emit_logf(gateway, IOT_IDE_GATEWAY_LOG_DEBUG,
                              "puback packet id=%d", packet->data.pub_ack.packet_id);
            break;
        case AIOT_MQTTRECV_PUB: {
            size_t topic_len = packet->data.pub.topic_len;
            size_t payload_len = packet->data.pub.payload_len;
            char *topic_text = (char *)malloc(topic_len + 1);
            char *payload_text = (char *)malloc(payload_len + 1);

            if (topic_text == NULL || payload_text == NULL) {
                free(topic_text);
                free(payload_text);
                gateway_emit_log(gateway, IOT_IDE_GATEWAY_LOG_ERROR, "failed to allocate mqtt packet buffers");
                return;
            }

            memcpy(topic_text, packet->data.pub.topic, topic_len);
            topic_text[topic_len] = '\0';
            memcpy(payload_text, packet->data.pub.payload, payload_len);
            payload_text[payload_len] = '\0';

            gateway_emit_logf(gateway, IOT_IDE_GATEWAY_LOG_DEBUG, "mqtt pub topic=%s", topic_text);
            gateway_emit_logf(gateway, IOT_IDE_GATEWAY_LOG_DEBUG, "mqtt pub payload=%s", payload_text);
            gateway_handle_service(gateway, topic_text, payload_text);

            free(topic_text);
            free(payload_text);
            break;
        }
        default:
            break;
    }
}

static void gateway_mqtt_event_handler(void *handle, const aiot_mqtt_event_t *event, void *userdata) {
    IotIdeGateway *gateway = (IotIdeGateway *)userdata;
    (void)handle;

    if (gateway == NULL || event == NULL) {
        return;
    }

    switch (event->type) {
        case AIOT_MQTTEVT_CONNECT:
            gateway_emit_log(gateway, IOT_IDE_GATEWAY_LOG_INFO, "AIOT_MQTTEVT_CONNECT");
            break;
        case AIOT_MQTTEVT_RECONNECT:
            gateway_emit_log(gateway, IOT_IDE_GATEWAY_LOG_WARN, "AIOT_MQTTEVT_RECONNECT");
            break;
        case AIOT_MQTTEVT_DISCONNECT:
            gateway_emit_log(gateway, IOT_IDE_GATEWAY_LOG_WARN, "AIOT_MQTTEVT_DISCONNECT");
            break;
        default:
            break;
    }
}

static void *gateway_mqtt_process_thread_main(void *arg) {
    IotIdeGateway *gateway = (IotIdeGateway *)arg;

    while (gateway->mqtt_process_thread_running) {
        int32_t res = aiot_mqtt_process(gateway->mqtt_handle);
        if (res == STATE_USER_INPUT_EXEC_DISABLED) {
            break;
        }
        sleep(1);
    }

    return NULL;
}

static void *gateway_mqtt_recv_thread_main(void *arg) {
    IotIdeGateway *gateway = (IotIdeGateway *)arg;

    while (gateway->mqtt_recv_thread_running) {
        int32_t res = aiot_mqtt_recv(gateway->mqtt_handle);
        if (res < STATE_SUCCESS) {
            if (res == STATE_USER_INPUT_EXEC_DISABLED) {
                break;
            }
            sleep(1);
        }
    }

    return NULL;
}

int iot_ide_gateway_get_api_version(void) {
    return IOT_IDE_GATEWAY_API_VERSION;
}

int iot_ide_gateway_create(const IotIdeGatewayOptions *options, IotIdeGateway **gateway) {
    IotIdeGateway *created;
    const char *config_path;

    if (gateway == NULL) {
        return IOT_IDE_GATEWAY_ERR_INVALID_ARGUMENT;
    }

    *gateway = NULL;
    created = (IotIdeGateway *)calloc(1, sizeof(*created));
    if (created == NULL) {
        return IOT_IDE_GATEWAY_ERR_NO_MEMORY;
    }

    config_path = (options != NULL && options->config_path != NULL && options->config_path[0] != '\0')
                      ? options->config_path
                      : "./device_id.json";
    snprintf(created->config_path, sizeof(created->config_path), "%s", config_path);

    if (options != NULL && options->callbacks != NULL) {
        created->callbacks = *options->callbacks;
    }
    created->user_data = options == NULL ? NULL : options->user_data;

    if (pthread_mutex_init(&created->mqtt_lock, NULL) != 0) {
        free(created);
        return IOT_IDE_GATEWAY_ERR_INTERNAL;
    }
    created->mqtt_lock_initialized = 1;

    if (device_config_load(created->config_path, &created->config) != 0 ||
        device_config_build_mqtt_host(&created->config, created->mqtt_host, sizeof(created->mqtt_host)) != 0) {
        iot_ide_gateway_destroy(created);
        return IOT_IDE_GATEWAY_ERR_INTERNAL;
    }

    gateway_emit_logf(created, IOT_IDE_GATEWAY_LOG_INFO,
                      "iot ide gateway created, productKey=%s deviceName=%s mqttHost=%s",
                      created->config.product_key,
                      created->config.device_name,
                      created->mqtt_host);

    *gateway = created;
    return IOT_IDE_GATEWAY_OK;
}

void iot_ide_gateway_destroy(IotIdeGateway *gateway) {
    if (gateway == NULL) {
        return;
    }

    iot_ide_gateway_stop(gateway);
    if (gateway->mqtt_lock_initialized) {
        pthread_mutex_destroy(&gateway->mqtt_lock);
    }
    free(gateway);
}

int iot_ide_gateway_start(IotIdeGateway *gateway) {
    aiot_sysdep_network_cred_t cred;
    uint16_t port = 8883;
    int32_t res;
    char service_topic[320];

    if (gateway == NULL) {
        return IOT_IDE_GATEWAY_ERR_INVALID_ARGUMENT;
    }
    if (gateway->started) {
        return IOT_IDE_GATEWAY_OK;
    }

    aiot_sysdep_set_portfile(&g_aiot_sysdep_portfile);
    aiot_state_set_logcb(gateway_state_logcb);

    memset(&cred, 0, sizeof(cred));
    cred.option = AIOT_SYSDEP_NETWORK_CRED_SVRCERT_CA;
    cred.max_tls_fragment = 16384;
    cred.sni_enabled = 1;
    cred.x509_server_cert = ali_ca_cert;
    cred.x509_server_cert_len = strlen(ali_ca_cert);

    gateway->mqtt_handle = aiot_mqtt_init();
    if (gateway->mqtt_handle == NULL) {
        return IOT_IDE_GATEWAY_ERR_INTERNAL;
    }

    aiot_mqtt_setopt(gateway->mqtt_handle, AIOT_MQTTOPT_HOST, (void *)gateway->mqtt_host);
    aiot_mqtt_setopt(gateway->mqtt_handle, AIOT_MQTTOPT_PORT, (void *)&port);
    aiot_mqtt_setopt(gateway->mqtt_handle, AIOT_MQTTOPT_PRODUCT_KEY, (void *)gateway->config.product_key);
    aiot_mqtt_setopt(gateway->mqtt_handle, AIOT_MQTTOPT_DEVICE_NAME, (void *)gateway->config.device_name);
    aiot_mqtt_setopt(gateway->mqtt_handle, AIOT_MQTTOPT_DEVICE_SECRET, (void *)gateway->config.device_secret);
    aiot_mqtt_setopt(gateway->mqtt_handle, AIOT_MQTTOPT_NETWORK_CRED, (void *)&cred);
    aiot_mqtt_setopt(gateway->mqtt_handle, AIOT_MQTTOPT_RECV_HANDLER, (void *)gateway_mqtt_recv_handler);
    aiot_mqtt_setopt(gateway->mqtt_handle, AIOT_MQTTOPT_EVENT_HANDLER, (void *)gateway_mqtt_event_handler);
    aiot_mqtt_setopt(gateway->mqtt_handle, AIOT_MQTTOPT_USERDATA, (void *)gateway);

    res = aiot_mqtt_connect(gateway->mqtt_handle);
    if (res < STATE_SUCCESS) {
        gateway_emit_logf(gateway, IOT_IDE_GATEWAY_LOG_ERROR, "aiot_mqtt_connect failed: -0x%04X", -res);
        aiot_mqtt_deinit(&gateway->mqtt_handle);
        return IOT_IDE_GATEWAY_ERR_INTERNAL;
    }

    snprintf(service_topic, sizeof(service_topic), "/sys/%s/%s/thing/service/#",
             gateway->config.product_key,
             gateway->config.device_name);
    if (gateway_subscribe_topic(gateway, service_topic) != IOT_IDE_GATEWAY_OK) {
        aiot_mqtt_disconnect(gateway->mqtt_handle);
        aiot_mqtt_deinit(&gateway->mqtt_handle);
        return IOT_IDE_GATEWAY_ERR_INTERNAL;
    }

    gateway->mqtt_process_thread_running = 1;
    gateway->mqtt_recv_thread_running = 1;
    if (pthread_create(&gateway->mqtt_process_thread, NULL, gateway_mqtt_process_thread_main, gateway) != 0) {
        gateway->mqtt_process_thread_running = 0;
        gateway->mqtt_recv_thread_running = 0;
        aiot_mqtt_disconnect(gateway->mqtt_handle);
        aiot_mqtt_deinit(&gateway->mqtt_handle);
        return IOT_IDE_GATEWAY_ERR_INTERNAL;
    }
    if (pthread_create(&gateway->mqtt_recv_thread, NULL, gateway_mqtt_recv_thread_main, gateway) != 0) {
        gateway->mqtt_recv_thread_running = 0;
        gateway->mqtt_process_thread_running = 0;
        aiot_mqtt_disconnect(gateway->mqtt_handle);
        pthread_join(gateway->mqtt_process_thread, NULL);
        aiot_mqtt_deinit(&gateway->mqtt_handle);
        return IOT_IDE_GATEWAY_ERR_INTERNAL;
    }

    gateway->started = 1;
    return IOT_IDE_GATEWAY_OK;
}

void iot_ide_gateway_stop(IotIdeGateway *gateway) {
    int join_process;
    int join_recv;

    if (gateway == NULL) {
        return;
    }

    join_process = gateway->mqtt_process_thread_running;
    join_recv = gateway->mqtt_recv_thread_running;
    gateway->mqtt_process_thread_running = 0;
    gateway->mqtt_recv_thread_running = 0;

    if (gateway->mqtt_handle != NULL) {
        aiot_mqtt_disconnect(gateway->mqtt_handle);
    }

    if (join_process && !pthread_equal(pthread_self(), gateway->mqtt_process_thread)) {
        pthread_join(gateway->mqtt_process_thread, NULL);
    }
    if (join_recv && !pthread_equal(pthread_self(), gateway->mqtt_recv_thread)) {
        pthread_join(gateway->mqtt_recv_thread, NULL);
    }

    if (gateway->mqtt_handle != NULL) {
        aiot_mqtt_deinit(&gateway->mqtt_handle);
    }

    gateway->started = 0;
}

int iot_ide_gateway_post_properties(IotIdeGateway *gateway, const char *params_json) {
    char topic[320];
    char *payload;
    size_t payload_size;
    int rc;

    if (gateway == NULL || params_json == NULL) {
        return IOT_IDE_GATEWAY_ERR_INVALID_ARGUMENT;
    }
    if (gateway->mqtt_handle == NULL) {
        return IOT_IDE_GATEWAY_ERR_NOT_STARTED;
    }

    snprintf(topic, sizeof(topic), "/sys/%s/%s/thing/event/property/post",
             gateway->config.product_key,
             gateway->config.device_name);

    payload_size = strlen(params_json) + 256;
    payload = (char *)malloc(payload_size);
    if (payload == NULL) {
        return IOT_IDE_GATEWAY_ERR_NO_MEMORY;
    }

    snprintf(payload, payload_size,
             "{\"id\":\"%lld\",\"version\":\"1.0\",\"params\":%s,\"method\":\"thing.event.property.post\"}",
             gateway_now_ms(),
             params_json);
    rc = gateway_publish_topic(gateway, topic, payload, 0);
    free(payload);
    return rc;
}

int iot_ide_gateway_reply_service(IotIdeGateway *gateway,
                                  const char *service_name,
                                  const char *request_id,
                                  int code,
                                  const char *data_json) {
    char topic[320];
    char *payload;
    size_t payload_size;
    int rc;

    if (gateway == NULL || service_name == NULL) {
        return IOT_IDE_GATEWAY_ERR_INVALID_ARGUMENT;
    }
    if (gateway->mqtt_handle == NULL) {
        return IOT_IDE_GATEWAY_ERR_NOT_STARTED;
    }

    snprintf(topic, sizeof(topic), "/sys/%s/%s/thing/service/%s_reply",
             gateway->config.product_key,
             gateway->config.device_name,
             service_name);

    payload_size = strlen(data_json == NULL ? "{}" : data_json) +
                   strlen(request_id == NULL ? "0" : request_id) + 128;
    payload = (char *)malloc(payload_size);
    if (payload == NULL) {
        return IOT_IDE_GATEWAY_ERR_NO_MEMORY;
    }

    snprintf(payload, payload_size,
             "{\"id\":\"%s\",\"code\":%d,\"data\":%s}",
             (request_id == NULL || request_id[0] == '\0') ? "0" : request_id,
             code,
             (data_json == NULL || data_json[0] == '\0') ? "{}" : data_json);
    rc = gateway_publish_topic(gateway, topic, payload, 0);
    free(payload);
    return rc;
}

int iot_ide_gateway_publish_trace(IotIdeGateway *gateway, const char *payload_json) {
    char topic[320];

    if (gateway == NULL || payload_json == NULL) {
        return IOT_IDE_GATEWAY_ERR_INVALID_ARGUMENT;
    }
    if (gateway->mqtt_handle == NULL) {
        return IOT_IDE_GATEWAY_ERR_NOT_STARTED;
    }

    snprintf(topic, sizeof(topic), "/%s/%s/user/trace/data",
             gateway->config.product_key,
             gateway->config.device_name);
    return gateway_publish_topic(gateway, topic, payload_json, 0);
}

int iot_ide_gateway_forward_event(IotIdeGateway *gateway, const char *event_name, const char *event_json) {
    if (gateway == NULL || event_name == NULL) {
        return IOT_IDE_GATEWAY_ERR_INVALID_ARGUMENT;
    }

    if (strcmp(event_name, "property.post") == 0) {
        return iot_ide_gateway_post_properties(gateway, event_json == NULL ? "{}" : event_json);
    }

    if (strcmp(event_name, "trace.publish") == 0) {
        return iot_ide_gateway_publish_trace(gateway, event_json == NULL ? "{}" : event_json);
    }

    if (strcmp(event_name, "service.reply") == 0) {
        char service_name[128] = "";
        char request_id[64] = "0";
        char code_text[32] = "200";
        char data_json[4096] = "{}";

        if (event_json == NULL) {
            return IOT_IDE_GATEWAY_ERR_INVALID_ARGUMENT;
        }

        (void)json_get_string(event_json, "service", service_name, sizeof(service_name));
        (void)json_get_string(event_json, "id", request_id, sizeof(request_id));
        (void)json_get_raw_value(event_json, "code", code_text, sizeof(code_text));
        (void)json_get_raw_value(event_json, "data", data_json, sizeof(data_json));
        if (service_name[0] == '\0') {
            return IOT_IDE_GATEWAY_ERR_INVALID_ARGUMENT;
        }
        return iot_ide_gateway_reply_service(gateway, service_name, request_id, atoi(code_text), data_json);
    }

    return IOT_IDE_GATEWAY_OK;
}

int iot_ide_gateway_get_device_info(IotIdeGateway *gateway,
                                    char *product_key,
                                    size_t product_key_size,
                                    char *device_name,
                                    size_t device_name_size,
                                    char *mqtt_host,
                                    size_t mqtt_host_size) {
    if (gateway == NULL) {
        return IOT_IDE_GATEWAY_ERR_INVALID_ARGUMENT;
    }

    if (product_key != NULL && product_key_size > 0) {
        snprintf(product_key, product_key_size, "%s", gateway->config.product_key);
    }
    if (device_name != NULL && device_name_size > 0) {
        snprintf(device_name, device_name_size, "%s", gateway->config.device_name);
    }
    if (mqtt_host != NULL && mqtt_host_size > 0) {
        snprintf(mqtt_host, mqtt_host_size, "%s", gateway->mqtt_host);
    }

    return IOT_IDE_GATEWAY_OK;
}
