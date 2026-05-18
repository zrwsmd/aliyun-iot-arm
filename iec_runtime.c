#include "aiot_mqtt_api.h"
#include "aiot_state_api.h"
#include "aiot_sysdep_api.h"
#include "device_config.h"
#include "iot_ide_runtime_api.h"
#include "json_utils.h"

#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern aiot_sysdep_portfile_t g_aiot_sysdep_portfile;
extern const char *ali_ca_cert;

typedef struct IecRuntime {
    DeviceConfig config;
    char mqtt_host[256];
    void *mqtt_handle;
    pthread_mutex_t mqtt_lock;
    pthread_t mqtt_process_thread;
    pthread_t mqtt_recv_thread;
    int mqtt_process_thread_running;
    int mqtt_recv_thread_running;
    volatile sig_atomic_t running;
    IotIdeRuntime *iot_ide;
} IecRuntime;

static IecRuntime *g_runtime = NULL;

/*
 * Production iec_runtime integration source.
 *
 * This file shows the production flow that iec_runtime should own:
 *
 * 1. connect Aliyun MQTT
 * 2. subscribe /sys/{pk}/{dn}/thing/service/#
 * 3. initialize libiot_ide.so
 * 4. dispatch Aliyun service params to libiot_ide.so C APIs
 * 5. publish libiot_ide.so callback events back to Aliyun
 *
 * Keep the libiot_ide.so API calls and callback mapping, and wire the MQTT
 * helper functions to iec_runtime's Aliyun connection/publish/reply
 * implementation.
 */

static void runtime_handle_signal(int signo) {
    (void)signo;
    if (g_runtime != NULL) {
        g_runtime->running = 0;
    }
}

static int32_t runtime_state_logcb(int32_t code, char *message) {
    (void)code;
    if (message != NULL) {
        printf("%s", message);
    }
    return 0;
}

static int runtime_publish_topic(IecRuntime *runtime, const char *topic, const char *payload, uint8_t qos) {
    int32_t res;

    if (runtime == NULL || runtime->mqtt_handle == NULL || topic == NULL || payload == NULL) {
        return -1;
    }

    pthread_mutex_lock(&runtime->mqtt_lock);
    res = aiot_mqtt_pub(runtime->mqtt_handle, (char *)topic, (uint8_t *)payload, (uint32_t)strlen(payload), qos);
    pthread_mutex_unlock(&runtime->mqtt_lock);

    if (res < STATE_SUCCESS) {
        fprintf(stderr, "mqtt publish failed, topic=%s, code=-0x%04X\n", topic, -res);
        return -1;
    }

    return 0;
}

static int runtime_subscribe_topic(IecRuntime *runtime, const char *topic) {
    int32_t res;

    pthread_mutex_lock(&runtime->mqtt_lock);
    res = aiot_mqtt_sub(runtime->mqtt_handle, (char *)topic, NULL, 1, NULL);
    pthread_mutex_unlock(&runtime->mqtt_lock);

    if (res < STATE_SUCCESS) {
        fprintf(stderr, "mqtt subscribe failed, topic=%s, code=-0x%04X\n", topic, -res);
        return -1;
    }

    printf("subscribed: %s\n", topic);
    return 0;
}

static int runtime_post_properties(IecRuntime *runtime, const char *params_json) {
    char topic[320];
    char *payload;
    size_t payload_size;
    int rc;

    if (runtime == NULL || params_json == NULL) {
        return -1;
    }

    snprintf(topic, sizeof(topic), "/sys/%s/%s/thing/event/property/post",
             runtime->config.product_key,
             runtime->config.device_name);

    payload_size = strlen(params_json) + 256;
    payload = (char *)malloc(payload_size);
    if (payload == NULL) {
        return -1;
    }

    snprintf(payload, payload_size,
             "{\"id\":\"%lld\",\"version\":\"1.0\",\"params\":%s,\"method\":\"thing.event.property.post\"}",
             (long long)time(NULL) * 1000LL,
             params_json);
    rc = runtime_publish_topic(runtime, topic, payload, 0);
    free(payload);
    return rc;
}

static int runtime_reply_service(IecRuntime *runtime, const char *service_path, const char *request_id, int code, const char *data_json) {
    char topic[320];
    char *payload;
    size_t payload_size;
    int rc;

    if (runtime == NULL || service_path == NULL) {
        return -1;
    }

    snprintf(topic, sizeof(topic), "/sys/%s/%s/thing/service/%s_reply",
             runtime->config.product_key,
             runtime->config.device_name,
             service_path);

    payload_size = strlen(data_json == NULL ? "{}" : data_json) + strlen(request_id == NULL ? "0" : request_id) + 128;
    payload = (char *)malloc(payload_size);
    if (payload == NULL) {
        return -1;
    }

    snprintf(payload, payload_size,
             "{\"id\":\"%s\",\"code\":%d,\"data\":%s}",
             (request_id == NULL || request_id[0] == '\0') ? "0" : request_id,
             code,
             (data_json == NULL || data_json[0] == '\0') ? "{}" : data_json);
    rc = runtime_publish_topic(runtime, topic, payload, 0);
    free(payload);
    return rc;
}

static int runtime_publish_trace(IecRuntime *runtime, const char *payload_json) {
    char topic[320];

    if (runtime == NULL || payload_json == NULL) {
        return -1;
    }

    snprintf(topic, sizeof(topic), "/%s/%s/user/trace/data",
             runtime->config.product_key,
             runtime->config.device_name);
    return runtime_publish_topic(runtime, topic, payload_json, 0);
}

static void runtime_on_event(void *user_data, const char *event_name, const char *event_json) {
    IecRuntime *runtime = (IecRuntime *)user_data;

    printf("[iot_ide event] %s %s\n", event_name == NULL ? "" : event_name, event_json == NULL ? "" : event_json);

    if (runtime == NULL || event_name == NULL) {
        return;
    }

    if (strcmp(event_name, "property.post") == 0) {
        /*
         * libiot_ide.so asks iec_runtime to report thing properties.
         * Real iec_runtime should publish event_json as params to:
         * /sys/{productKey}/{deviceName}/thing/event/property/post
         */
        (void)runtime_post_properties(runtime, event_json == NULL ? "{}" : event_json);
    } else if (strcmp(event_name, "trace.publish") == 0) {
        /*
         * Optional trace publish path.
         * Real iec_runtime can publish it or map it to its own trace channel.
         */
        (void)runtime_publish_trace(runtime, event_json == NULL ? "{}" : event_json);
    } else if (strcmp(event_name, "service.reply") == 0) {
        /*
         * Optional centralized service reply path.
         * Most service replies are sent directly after API calls
         * in runtime_handle_service(), but this branch shows how to handle
         * a reply requested from callback if future APIs use it.
         */
        char service_path[128] = "";
        char request_id[64] = "0";
        char code_text[32] = "200";
        char data_json[4096] = "{}";

        (void)json_get_string(event_json, "service", service_path, sizeof(service_path));
        (void)json_get_string(event_json, "id", request_id, sizeof(request_id));
        (void)json_get_raw_value(event_json, "code", code_text, sizeof(code_text));
        (void)json_get_raw_value(event_json, "data", data_json, sizeof(data_json));
        if (service_path[0] != '\0') {
            (void)runtime_reply_service(runtime, service_path, request_id, atoi(code_text), data_json);
        }
    }
}

static void runtime_on_log(void *user_data, int level, const char *message) {
    (void)user_data;
    printf("[iot_ide log:%d] %s\n", level, message == NULL ? "" : message);
}

static void runtime_reply_simple_error(IecRuntime *runtime, const char *service_path, const char *request_id, const char *message) {
    char escaped[512];
    char response[768];

    if (json_escape_string(message == NULL ? "" : message, escaped, sizeof(escaped)) != 0) {
        snprintf(escaped, sizeof(escaped), "error");
    }

    snprintf(response, sizeof(response), "{\"success\":0,\"message\":\"%s\"}", escaped);
    (void)runtime_reply_service(runtime, service_path, request_id, 200, response);
}

static int runtime_is_reply_topic(const char *service_path) {
    size_t len;
    const char *suffix = "_reply";
    size_t suffix_len = strlen(suffix);

    if (service_path == NULL) {
        return 0;
    }

    len = strlen(service_path);
    return len >= suffix_len && strcmp(service_path + len - suffix_len, suffix) == 0;
}

static void runtime_handle_service(IecRuntime *runtime, const char *topic, const char *payload) {
    char prefix[320];
    const char *service_path;
    char request_id[64] = "0";
    char params[8192] = "{}";
    char response[8192] = "{\"success\":0,\"message\":\"not handled\"}";
    int rc = -1;

    if (runtime == NULL || topic == NULL || payload == NULL) {
        return;
    }

    snprintf(prefix, sizeof(prefix), "/sys/%s/%s/thing/service/",
             runtime->config.product_key,
             runtime->config.device_name);

    if (strncmp(topic, prefix, strlen(prefix)) != 0) {
        return;
    }

    service_path = topic + strlen(prefix);
    if (runtime_is_reply_topic(service_path)) {
        return;
    }

    (void)json_get_raw_value(payload, "id", request_id, sizeof(request_id));
    if (json_get_object(payload, "params", params, sizeof(params)) != 0) {
        snprintf(params, sizeof(params), "{}");
    }

    printf("service=%s id=%s params=%s\n", service_path, request_id, params);

    /*
     * This is the most important integration block for real iec_runtime.
     *
     * Real iec_runtime receives Aliyun serviceName + params, then calls the
     * matching libiot_ide.so API below. The response must be sent back to
     * /sys/{pk}/{dn}/thing/service/{serviceName}_reply.
     */
    if (strcmp(service_path, "requestConnect") == 0) {
        rc = iot_ide_runtime_request_connect(runtime->iot_ide, params, response, sizeof(response));
    } else if (strcmp(service_path, "requestDisconnect") == 0) {
        rc = iot_ide_runtime_request_disconnect(runtime->iot_ide, params, response, sizeof(response));
    } else if (strcmp(service_path, "ideHeartbeat") == 0) {
        rc = iot_ide_runtime_heartbeat(runtime->iot_ide, params, response, sizeof(response));
    } else if (strcmp(service_path, "deployProject") == 0) {
        rc = iot_ide_runtime_deploy_project(runtime->iot_ide, params, response, sizeof(response));
    } else if (strcmp(service_path, "startProject") == 0) {
        rc = iot_ide_runtime_start_project(runtime->iot_ide, params, response, sizeof(response));
    } else if (strcmp(service_path, "property/get") == 0) {
        rc = iot_ide_runtime_get_connection_snapshot(runtime->iot_ide, response, sizeof(response));
    } else if (strcmp(service_path, "property/set") == 0) {
        (void)runtime_post_properties(runtime, params);
        snprintf(response, sizeof(response), "{\"success\":1,\"message\":\"property set accepted\"}");
        rc = 0;
    } else {
        runtime_reply_simple_error(runtime, service_path, request_id, "unknown service");
        return;
    }

    printf("service response rc=%d data=%s\n", rc, response);
    (void)runtime_reply_service(runtime, service_path, request_id, 200, response);
}

static void runtime_mqtt_recv_handler(void *handle, const aiot_mqtt_recv_t *packet, void *userdata) {
    IecRuntime *runtime = (IecRuntime *)userdata;
    (void)handle;

    if (runtime == NULL || packet == NULL) {
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
            char *topic_text = (char *)malloc(topic_len + 1);
            char *payload_text = (char *)malloc(payload_len + 1);

            if (topic_text == NULL || payload_text == NULL) {
                free(topic_text);
                free(payload_text);
                return;
            }

            memcpy(topic_text, packet->data.pub.topic, topic_len);
            topic_text[topic_len] = '\0';
            memcpy(payload_text, packet->data.pub.payload, payload_len);
            payload_text[payload_len] = '\0';

            printf("mqtt pub topic=%s\n", topic_text);
            printf("mqtt pub payload=%s\n", payload_text);
            runtime_handle_service(runtime, topic_text, payload_text);

            free(topic_text);
            free(payload_text);
            break;
        }
        default:
            break;
    }
}

static void runtime_mqtt_event_handler(void *handle, const aiot_mqtt_event_t *event, void *userdata) {
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

static void *runtime_mqtt_process_thread_main(void *arg) {
    IecRuntime *runtime = (IecRuntime *)arg;

    while (runtime->mqtt_process_thread_running) {
        int32_t res = aiot_mqtt_process(runtime->mqtt_handle);
        if (res == STATE_USER_INPUT_EXEC_DISABLED) {
            break;
        }
        sleep(1);
    }

    return NULL;
}

static void *runtime_mqtt_recv_thread_main(void *arg) {
    IecRuntime *runtime = (IecRuntime *)arg;

    while (runtime->mqtt_recv_thread_running) {
        int32_t res = aiot_mqtt_recv(runtime->mqtt_handle);
        if (res < STATE_SUCCESS) {
            if (res == STATE_USER_INPUT_EXEC_DISABLED) {
                break;
            }
            sleep(1);
        }
    }

    return NULL;
}

static int runtime_start_mqtt(IecRuntime *runtime) {
    aiot_sysdep_network_cred_t cred;
    uint16_t port = 8883;
    int32_t res;
    char service_topic[320];

    aiot_sysdep_set_portfile(&g_aiot_sysdep_portfile);
    aiot_state_set_logcb(runtime_state_logcb);

    memset(&cred, 0, sizeof(cred));
    cred.option = AIOT_SYSDEP_NETWORK_CRED_SVRCERT_CA;
    cred.max_tls_fragment = 16384;
    cred.sni_enabled = 1;
    cred.x509_server_cert = ali_ca_cert;
    cred.x509_server_cert_len = strlen(ali_ca_cert);

    runtime->mqtt_handle = aiot_mqtt_init();
    if (runtime->mqtt_handle == NULL) {
        return -1;
    }

    aiot_mqtt_setopt(runtime->mqtt_handle, AIOT_MQTTOPT_HOST, (void *)runtime->mqtt_host);
    aiot_mqtt_setopt(runtime->mqtt_handle, AIOT_MQTTOPT_PORT, (void *)&port);
    aiot_mqtt_setopt(runtime->mqtt_handle, AIOT_MQTTOPT_PRODUCT_KEY, (void *)runtime->config.product_key);
    aiot_mqtt_setopt(runtime->mqtt_handle, AIOT_MQTTOPT_DEVICE_NAME, (void *)runtime->config.device_name);
    aiot_mqtt_setopt(runtime->mqtt_handle, AIOT_MQTTOPT_DEVICE_SECRET, (void *)runtime->config.device_secret);
    aiot_mqtt_setopt(runtime->mqtt_handle, AIOT_MQTTOPT_NETWORK_CRED, (void *)&cred);
    aiot_mqtt_setopt(runtime->mqtt_handle, AIOT_MQTTOPT_RECV_HANDLER, (void *)runtime_mqtt_recv_handler);
    aiot_mqtt_setopt(runtime->mqtt_handle, AIOT_MQTTOPT_EVENT_HANDLER, (void *)runtime_mqtt_event_handler);
    aiot_mqtt_setopt(runtime->mqtt_handle, AIOT_MQTTOPT_USERDATA, (void *)runtime);

    res = aiot_mqtt_connect(runtime->mqtt_handle);
    if (res < STATE_SUCCESS) {
        fprintf(stderr, "aiot_mqtt_connect failed: -0x%04X\n", -res);
        return -1;
    }

    snprintf(service_topic, sizeof(service_topic), "/sys/%s/%s/thing/service/#",
             runtime->config.product_key,
             runtime->config.device_name);
    if (runtime_subscribe_topic(runtime, service_topic) != 0) {
        return -1;
    }

    runtime->mqtt_process_thread_running = 1;
    runtime->mqtt_recv_thread_running = 1;
    if (pthread_create(&runtime->mqtt_process_thread, NULL, runtime_mqtt_process_thread_main, runtime) != 0) {
        return -1;
    }
    if (pthread_create(&runtime->mqtt_recv_thread, NULL, runtime_mqtt_recv_thread_main, runtime) != 0) {
        runtime->mqtt_process_thread_running = 0;
        pthread_join(runtime->mqtt_process_thread, NULL);
        return -1;
    }

    return 0;
}

static void runtime_stop_mqtt(IecRuntime *runtime) {
    int join_process;
    int join_recv;

    if (runtime == NULL) {
        return;
    }

    join_process = runtime->mqtt_process_thread_running;
    join_recv = runtime->mqtt_recv_thread_running;
    runtime->mqtt_process_thread_running = 0;
    runtime->mqtt_recv_thread_running = 0;

    if (runtime->mqtt_handle != NULL) {
        aiot_mqtt_disconnect(runtime->mqtt_handle);
    }

    if (join_process) {
        pthread_join(runtime->mqtt_process_thread, NULL);
    }
    if (join_recv) {
        pthread_join(runtime->mqtt_recv_thread, NULL);
    }

    if (runtime->mqtt_handle != NULL) {
        aiot_mqtt_deinit(&runtime->mqtt_handle);
    }
}

int main(int argc, char **argv) {
    const char *config_path = argc > 1 ? argv[1] : "./device_id.json";
    IecRuntime runtime;
    IotIdeRuntimeCallbacks callbacks;
    IotIdeRuntimeOptions options;

    memset(&runtime, 0, sizeof(runtime));
    memset(&callbacks, 0, sizeof(callbacks));
    memset(&options, 0, sizeof(options));

    g_runtime = &runtime;
    runtime.running = 1;
    signal(SIGINT, runtime_handle_signal);
    signal(SIGTERM, runtime_handle_signal);

    /*
     * Step 1:
     * Load device identity for the Aliyun MQTT connection owned by iec_runtime.
     *
     * In the real iec_runtime, this may come from its own config system rather
     * than this project's device_id.json.
     */
    if (pthread_mutex_init(&runtime.mqtt_lock, NULL) != 0) {
        fprintf(stderr, "failed to init mqtt lock\n");
        return 1;
    }

    if (device_config_load(config_path, &runtime.config) != 0 ||
        device_config_build_mqtt_host(&runtime.config, runtime.mqtt_host, sizeof(runtime.mqtt_host)) != 0) {
        pthread_mutex_destroy(&runtime.mqtt_lock);
        return 1;
    }

    /*
     * Step 2:
     * Register libiot_ide.so callbacks.
     *
     * libiot_ide.so does not publish to Aliyun directly. It sends events back
     * to iec_runtime via callbacks. The real iec_runtime should map these
     * callbacks to its own Aliyun property/report/reply functions.
     */
    callbacks.on_event = runtime_on_event;
    callbacks.on_log = runtime_on_log;
    options.work_dir = ".";
    options.callbacks = &callbacks;
    options.user_data = &runtime;

    /*
     * Step 3:
     * Initialize libiot_ide.so.
     *
     * After this, service calls received from Aliyun can be dispatched to
     * iot_ide_runtime_request_connect(), iot_ide_runtime_deploy_project(), etc.
     */
    if (iot_ide_runtime_create(&options, &runtime.iot_ide) != IOT_IDE_RUNTIME_OK) {
        fprintf(stderr, "iot_ide_runtime_create failed\n");
        pthread_mutex_destroy(&runtime.mqtt_lock);
        return 1;
    }

    printf("=== iec_runtime ===\n");
    printf("config: %s\n", config_path);
    printf("productKey: %s\n", runtime.config.product_key);
    printf("deviceName: %s\n", runtime.config.device_name);
    printf("mqttHost: %s\n", runtime.mqtt_host);

    /*
     * Step 4:
     * Connect Aliyun MQTT and subscribe service topics.
     *
     * This is required in production because iec_runtime is the process that
     * receives Aliyun service calls and publishes replies/properties.
     */
    if (runtime_start_mqtt(&runtime) != 0) {
        fprintf(stderr, "runtime_start_mqtt failed\n");
        iot_ide_runtime_destroy(runtime.iot_ide);
        pthread_mutex_destroy(&runtime.mqtt_lock);
        return 1;
    }

    while (runtime.running) {
        sleep(1);
    }

    /*
     * Step 5:
     * Clean shutdown.
     */
    runtime_stop_mqtt(&runtime);
    iot_ide_runtime_destroy(runtime.iot_ide);
    pthread_mutex_destroy(&runtime.mqtt_lock);
    printf("iec_runtime exit\n");
    return 0;
}
