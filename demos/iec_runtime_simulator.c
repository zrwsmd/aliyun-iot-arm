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

typedef struct IecRuntimeSimulator {
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
} IecRuntimeSimulator;

static IecRuntimeSimulator *g_simulator = NULL;

static void simulator_handle_signal(int signo) {
    (void)signo;
    if (g_simulator != NULL) {
        g_simulator->running = 0;
    }
}

static int32_t simulator_state_logcb(int32_t code, char *message) {
    (void)code;
    if (message != NULL) {
        printf("%s", message);
    }
    return 0;
}

static int simulator_publish_topic(IecRuntimeSimulator *sim, const char *topic, const char *payload, uint8_t qos) {
    int32_t res;

    if (sim == NULL || sim->mqtt_handle == NULL || topic == NULL || payload == NULL) {
        return -1;
    }

    pthread_mutex_lock(&sim->mqtt_lock);
    res = aiot_mqtt_pub(sim->mqtt_handle, (char *)topic, (uint8_t *)payload, (uint32_t)strlen(payload), qos);
    pthread_mutex_unlock(&sim->mqtt_lock);

    if (res < STATE_SUCCESS) {
        fprintf(stderr, "mqtt publish failed, topic=%s, code=-0x%04X\n", topic, -res);
        return -1;
    }

    return 0;
}

static int simulator_subscribe_topic(IecRuntimeSimulator *sim, const char *topic) {
    int32_t res;

    pthread_mutex_lock(&sim->mqtt_lock);
    res = aiot_mqtt_sub(sim->mqtt_handle, (char *)topic, NULL, 1, NULL);
    pthread_mutex_unlock(&sim->mqtt_lock);

    if (res < STATE_SUCCESS) {
        fprintf(stderr, "mqtt subscribe failed, topic=%s, code=-0x%04X\n", topic, -res);
        return -1;
    }

    printf("subscribed: %s\n", topic);
    return 0;
}

static int simulator_post_properties(IecRuntimeSimulator *sim, const char *params_json) {
    char topic[320];
    char *payload;
    size_t payload_size;
    int rc;

    if (sim == NULL || params_json == NULL) {
        return -1;
    }

    snprintf(topic, sizeof(topic), "/sys/%s/%s/thing/event/property/post",
             sim->config.product_key,
             sim->config.device_name);

    payload_size = strlen(params_json) + 256;
    payload = (char *)malloc(payload_size);
    if (payload == NULL) {
        return -1;
    }

    snprintf(payload, payload_size,
             "{\"id\":\"%lld\",\"version\":\"1.0\",\"params\":%s,\"method\":\"thing.event.property.post\"}",
             (long long)time(NULL) * 1000LL,
             params_json);
    rc = simulator_publish_topic(sim, topic, payload, 0);
    free(payload);
    return rc;
}

static int simulator_reply_service(IecRuntimeSimulator *sim, const char *service_path, const char *request_id, int code, const char *data_json) {
    char topic[320];
    char *payload;
    size_t payload_size;
    int rc;

    if (sim == NULL || service_path == NULL) {
        return -1;
    }

    snprintf(topic, sizeof(topic), "/sys/%s/%s/thing/service/%s_reply",
             sim->config.product_key,
             sim->config.device_name,
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
    rc = simulator_publish_topic(sim, topic, payload, 0);
    free(payload);
    return rc;
}

static int simulator_publish_trace(IecRuntimeSimulator *sim, const char *payload_json) {
    char topic[320];

    if (sim == NULL || payload_json == NULL) {
        return -1;
    }

    snprintf(topic, sizeof(topic), "/%s/%s/user/trace/data",
             sim->config.product_key,
             sim->config.device_name);
    return simulator_publish_topic(sim, topic, payload_json, 0);
}

static void simulator_on_event(void *user_data, const char *event_name, const char *event_json) {
    IecRuntimeSimulator *sim = (IecRuntimeSimulator *)user_data;

    printf("[iot_ide event] %s %s\n", event_name == NULL ? "" : event_name, event_json == NULL ? "" : event_json);

    if (sim == NULL || event_name == NULL) {
        return;
    }

    if (strcmp(event_name, "property.post") == 0) {
        (void)simulator_post_properties(sim, event_json == NULL ? "{}" : event_json);
    } else if (strcmp(event_name, "trace.publish") == 0) {
        (void)simulator_publish_trace(sim, event_json == NULL ? "{}" : event_json);
    } else if (strcmp(event_name, "service.reply") == 0) {
        char service_path[128] = "";
        char request_id[64] = "0";
        char code_text[32] = "200";
        char data_json[4096] = "{}";

        (void)json_get_string(event_json, "service", service_path, sizeof(service_path));
        (void)json_get_string(event_json, "id", request_id, sizeof(request_id));
        (void)json_get_raw_value(event_json, "code", code_text, sizeof(code_text));
        (void)json_get_raw_value(event_json, "data", data_json, sizeof(data_json));
        if (service_path[0] != '\0') {
            (void)simulator_reply_service(sim, service_path, request_id, atoi(code_text), data_json);
        }
    }
}

static void simulator_on_log(void *user_data, int level, const char *message) {
    (void)user_data;
    printf("[iot_ide log:%d] %s\n", level, message == NULL ? "" : message);
}

static void simulator_reply_simple_error(IecRuntimeSimulator *sim, const char *service_path, const char *request_id, const char *message) {
    char escaped[512];
    char response[768];

    if (json_escape_string(message == NULL ? "" : message, escaped, sizeof(escaped)) != 0) {
        snprintf(escaped, sizeof(escaped), "error");
    }

    snprintf(response, sizeof(response), "{\"success\":0,\"message\":\"%s\"}", escaped);
    (void)simulator_reply_service(sim, service_path, request_id, 200, response);
}

static int simulator_is_reply_topic(const char *service_path) {
    size_t len;
    const char *suffix = "_reply";
    size_t suffix_len = strlen(suffix);

    if (service_path == NULL) {
        return 0;
    }

    len = strlen(service_path);
    return len >= suffix_len && strcmp(service_path + len - suffix_len, suffix) == 0;
}

static void simulator_handle_service(IecRuntimeSimulator *sim, const char *topic, const char *payload) {
    char prefix[320];
    const char *service_path;
    char request_id[64] = "0";
    char params[8192] = "{}";
    char response[8192] = "{\"success\":0,\"message\":\"not handled\"}";
    int rc = -1;

    if (sim == NULL || topic == NULL || payload == NULL) {
        return;
    }

    snprintf(prefix, sizeof(prefix), "/sys/%s/%s/thing/service/",
             sim->config.product_key,
             sim->config.device_name);

    if (strncmp(topic, prefix, strlen(prefix)) != 0) {
        return;
    }

    service_path = topic + strlen(prefix);
    if (simulator_is_reply_topic(service_path)) {
        return;
    }

    (void)json_get_raw_value(payload, "id", request_id, sizeof(request_id));
    if (json_get_object(payload, "params", params, sizeof(params)) != 0) {
        snprintf(params, sizeof(params), "{}");
    }

    printf("service=%s id=%s params=%s\n", service_path, request_id, params);

    if (strcmp(service_path, "requestConnect") == 0) {
        rc = iot_ide_runtime_request_connect(sim->iot_ide, params, response, sizeof(response));
    } else if (strcmp(service_path, "requestDisconnect") == 0) {
        rc = iot_ide_runtime_request_disconnect(sim->iot_ide, params, response, sizeof(response));
    } else if (strcmp(service_path, "ideHeartbeat") == 0) {
        rc = iot_ide_runtime_heartbeat(sim->iot_ide, params, response, sizeof(response));
    } else if (strcmp(service_path, "deployProject") == 0) {
        rc = iot_ide_runtime_deploy_project(sim->iot_ide, params, response, sizeof(response));
    } else if (strcmp(service_path, "startProject") == 0) {
        rc = iot_ide_runtime_start_project(sim->iot_ide, params, response, sizeof(response));
    } else if (strcmp(service_path, "property/get") == 0) {
        rc = iot_ide_runtime_get_connection_snapshot(sim->iot_ide, response, sizeof(response));
    } else if (strcmp(service_path, "property/set") == 0) {
        (void)simulator_post_properties(sim, params);
        snprintf(response, sizeof(response), "{\"success\":1,\"message\":\"property set accepted\"}");
        rc = 0;
    } else {
        simulator_reply_simple_error(sim, service_path, request_id, "unknown service");
        return;
    }

    printf("service response rc=%d data=%s\n", rc, response);
    (void)simulator_reply_service(sim, service_path, request_id, 200, response);
}

static void simulator_mqtt_recv_handler(void *handle, const aiot_mqtt_recv_t *packet, void *userdata) {
    IecRuntimeSimulator *sim = (IecRuntimeSimulator *)userdata;
    (void)handle;

    if (sim == NULL || packet == NULL) {
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
            simulator_handle_service(sim, topic_text, payload_text);

            free(topic_text);
            free(payload_text);
            break;
        }
        default:
            break;
    }
}

static void simulator_mqtt_event_handler(void *handle, const aiot_mqtt_event_t *event, void *userdata) {
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

static void *simulator_mqtt_process_thread_main(void *arg) {
    IecRuntimeSimulator *sim = (IecRuntimeSimulator *)arg;

    while (sim->mqtt_process_thread_running) {
        int32_t res = aiot_mqtt_process(sim->mqtt_handle);
        if (res == STATE_USER_INPUT_EXEC_DISABLED) {
            break;
        }
        sleep(1);
    }

    return NULL;
}

static void *simulator_mqtt_recv_thread_main(void *arg) {
    IecRuntimeSimulator *sim = (IecRuntimeSimulator *)arg;

    while (sim->mqtt_recv_thread_running) {
        int32_t res = aiot_mqtt_recv(sim->mqtt_handle);
        if (res < STATE_SUCCESS) {
            if (res == STATE_USER_INPUT_EXEC_DISABLED) {
                break;
            }
            sleep(1);
        }
    }

    return NULL;
}

static int simulator_start_mqtt(IecRuntimeSimulator *sim) {
    aiot_sysdep_network_cred_t cred;
    uint16_t port = 8883;
    int32_t res;
    char service_topic[320];

    aiot_sysdep_set_portfile(&g_aiot_sysdep_portfile);
    aiot_state_set_logcb(simulator_state_logcb);

    memset(&cred, 0, sizeof(cred));
    cred.option = AIOT_SYSDEP_NETWORK_CRED_SVRCERT_CA;
    cred.max_tls_fragment = 16384;
    cred.sni_enabled = 1;
    cred.x509_server_cert = ali_ca_cert;
    cred.x509_server_cert_len = strlen(ali_ca_cert);

    sim->mqtt_handle = aiot_mqtt_init();
    if (sim->mqtt_handle == NULL) {
        return -1;
    }

    aiot_mqtt_setopt(sim->mqtt_handle, AIOT_MQTTOPT_HOST, (void *)sim->mqtt_host);
    aiot_mqtt_setopt(sim->mqtt_handle, AIOT_MQTTOPT_PORT, (void *)&port);
    aiot_mqtt_setopt(sim->mqtt_handle, AIOT_MQTTOPT_PRODUCT_KEY, (void *)sim->config.product_key);
    aiot_mqtt_setopt(sim->mqtt_handle, AIOT_MQTTOPT_DEVICE_NAME, (void *)sim->config.device_name);
    aiot_mqtt_setopt(sim->mqtt_handle, AIOT_MQTTOPT_DEVICE_SECRET, (void *)sim->config.device_secret);
    aiot_mqtt_setopt(sim->mqtt_handle, AIOT_MQTTOPT_NETWORK_CRED, (void *)&cred);
    aiot_mqtt_setopt(sim->mqtt_handle, AIOT_MQTTOPT_RECV_HANDLER, (void *)simulator_mqtt_recv_handler);
    aiot_mqtt_setopt(sim->mqtt_handle, AIOT_MQTTOPT_EVENT_HANDLER, (void *)simulator_mqtt_event_handler);
    aiot_mqtt_setopt(sim->mqtt_handle, AIOT_MQTTOPT_USERDATA, (void *)sim);

    res = aiot_mqtt_connect(sim->mqtt_handle);
    if (res < STATE_SUCCESS) {
        fprintf(stderr, "aiot_mqtt_connect failed: -0x%04X\n", -res);
        return -1;
    }

    snprintf(service_topic, sizeof(service_topic), "/sys/%s/%s/thing/service/#",
             sim->config.product_key,
             sim->config.device_name);
    if (simulator_subscribe_topic(sim, service_topic) != 0) {
        return -1;
    }

    sim->mqtt_process_thread_running = 1;
    sim->mqtt_recv_thread_running = 1;
    if (pthread_create(&sim->mqtt_process_thread, NULL, simulator_mqtt_process_thread_main, sim) != 0) {
        return -1;
    }
    if (pthread_create(&sim->mqtt_recv_thread, NULL, simulator_mqtt_recv_thread_main, sim) != 0) {
        sim->mqtt_process_thread_running = 0;
        pthread_join(sim->mqtt_process_thread, NULL);
        return -1;
    }

    return 0;
}

static void simulator_stop_mqtt(IecRuntimeSimulator *sim) {
    int join_process;
    int join_recv;

    if (sim == NULL) {
        return;
    }

    join_process = sim->mqtt_process_thread_running;
    join_recv = sim->mqtt_recv_thread_running;
    sim->mqtt_process_thread_running = 0;
    sim->mqtt_recv_thread_running = 0;

    if (sim->mqtt_handle != NULL) {
        aiot_mqtt_disconnect(sim->mqtt_handle);
    }

    if (join_process) {
        pthread_join(sim->mqtt_process_thread, NULL);
    }
    if (join_recv) {
        pthread_join(sim->mqtt_recv_thread, NULL);
    }

    if (sim->mqtt_handle != NULL) {
        aiot_mqtt_deinit(&sim->mqtt_handle);
    }
}

int main(int argc, char **argv) {
    const char *config_path = argc > 1 ? argv[1] : "./device_id.json";
    IecRuntimeSimulator sim;
    IotIdeRuntimeCallbacks callbacks;
    IotIdeRuntimeOptions options;

    memset(&sim, 0, sizeof(sim));
    memset(&callbacks, 0, sizeof(callbacks));
    memset(&options, 0, sizeof(options));

    g_simulator = &sim;
    sim.running = 1;
    signal(SIGINT, simulator_handle_signal);
    signal(SIGTERM, simulator_handle_signal);

    if (pthread_mutex_init(&sim.mqtt_lock, NULL) != 0) {
        fprintf(stderr, "failed to init mqtt lock\n");
        return 1;
    }

    if (device_config_load(config_path, &sim.config) != 0 ||
        device_config_build_mqtt_host(&sim.config, sim.mqtt_host, sizeof(sim.mqtt_host)) != 0) {
        pthread_mutex_destroy(&sim.mqtt_lock);
        return 1;
    }

    callbacks.on_event = simulator_on_event;
    callbacks.on_log = simulator_on_log;
    options.work_dir = ".";
    options.callbacks = &callbacks;
    options.user_data = &sim;

    if (iot_ide_runtime_create(&options, &sim.iot_ide) != IOT_IDE_RUNTIME_OK) {
        fprintf(stderr, "iot_ide_runtime_create failed\n");
        pthread_mutex_destroy(&sim.mqtt_lock);
        return 1;
    }

    printf("=== iec_runtime_simulator ===\n");
    printf("config: %s\n", config_path);
    printf("productKey: %s\n", sim.config.product_key);
    printf("deviceName: %s\n", sim.config.device_name);
    printf("mqttHost: %s\n", sim.mqtt_host);

    if (simulator_start_mqtt(&sim) != 0) {
        fprintf(stderr, "simulator_start_mqtt failed\n");
        iot_ide_runtime_destroy(sim.iot_ide);
        pthread_mutex_destroy(&sim.mqtt_lock);
        return 1;
    }

    while (sim.running) {
        sleep(1);
    }

    simulator_stop_mqtt(&sim);
    iot_ide_runtime_destroy(sim.iot_ide);
    pthread_mutex_destroy(&sim.mqtt_lock);
    printf("iec_runtime_simulator exit\n");
    return 0;
}
