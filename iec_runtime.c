#include "iot_ide_gateway_api.h"
#include "iot_ide_runtime_api.h"

#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

typedef struct IecRuntime {
    volatile sig_atomic_t running;
    IotIdeGateway *gateway;
    IotIdeRuntime *iot_ide;
} IecRuntime;

static IecRuntime *g_runtime = NULL;

/*
 * Production integration entry.
 *
 * libiot_ide_gateway.so owns Aliyun MQTT connect, service subscription,
 * service JSON parsing, property post, service reply, and trace publish.
 *
 * libiot_ide.so owns IDE business behavior: connect lock, heartbeat,
 * disconnect, deploy, start, and connection snapshot.
 *
 * The real iec_runtime process only needs this glue layer: dispatch gateway
 * service callbacks to libiot_ide.so, then forward libiot_ide.so events back
 * to Aliyun through the gateway.
 */

static void runtime_handle_signal(int signo) {
    (void)signo;
    if (g_runtime != NULL) {
        g_runtime->running = 0;
    }
}

static void runtime_reply_error(IotIdeGateway *gateway,
                                const char *service_name,
                                const char *request_id,
                                const char *message) {
    char response[512];

    snprintf(response, sizeof(response),
             "{\"success\":0,\"message\":\"%s\"}",
             message == NULL ? "error" : message);
    (void)iot_ide_gateway_reply_service(gateway, service_name, request_id, 200, response);
}

static void runtime_on_iot_ide_event(void *user_data, const char *event_name, const char *event_json) {
    IecRuntime *runtime = (IecRuntime *)user_data;

    printf("[iot_ide event] %s %s\n",
           event_name == NULL ? "" : event_name,
           event_json == NULL ? "" : event_json);

    if (runtime == NULL || runtime->gateway == NULL || event_name == NULL) {
        return;
    }

    /*
     * property.post -> gateway reports thing properties to Aliyun.
     * trace.publish -> gateway publishes trace data to Aliyun.
     * service.reply -> gateway replies to Aliyun service calls if a future
     *                  libiot_ide.so API emits centralized replies.
     */
    (void)iot_ide_gateway_forward_event(runtime->gateway, event_name, event_json);
}

static void runtime_on_iot_ide_log(void *user_data, int level, const char *message) {
    (void)user_data;
    printf("[iot_ide log:%d] %s\n", level, message == NULL ? "" : message);
}

static void runtime_on_gateway_log(void *user_data, int level, const char *message) {
    (void)user_data;
    printf("[gateway log:%d] %s\n", level, message == NULL ? "" : message);
}

static void runtime_on_service(void *user_data,
                               IotIdeGateway *gateway,
                               const char *service_name,
                               const char *request_id,
                               const char *params_json) {
    IecRuntime *runtime = (IecRuntime *)user_data;
    char response[8192] = "{\"success\":0,\"message\":\"not handled\"}";
    int rc = IOT_IDE_RUNTIME_ERR_INVALID_ARGUMENT;

    if (runtime == NULL || runtime->iot_ide == NULL || gateway == NULL || service_name == NULL) {
        return;
    }

    /*
     * This is the key production integration block.
     *
     * Aliyun service name + params_json come from libiot_ide_gateway.so.
     * libiot_ide.so executes the IDE business logic and fills response.
     * The response is then sent back to Aliyun through the gateway.
     */
    if (strcmp(service_name, "requestConnect") == 0) {
        rc = iot_ide_runtime_request_connect(runtime->iot_ide, params_json, response, sizeof(response));
    } else if (strcmp(service_name, "requestDisconnect") == 0) {
        rc = iot_ide_runtime_request_disconnect(runtime->iot_ide, params_json, response, sizeof(response));
    } else if (strcmp(service_name, "ideHeartbeat") == 0) {
        rc = iot_ide_runtime_heartbeat(runtime->iot_ide, params_json, response, sizeof(response));
    } else if (strcmp(service_name, "deployProject") == 0) {
        rc = iot_ide_runtime_deploy_project(runtime->iot_ide, params_json, response, sizeof(response));
    } else if (strcmp(service_name, "startProject") == 0) {
        rc = iot_ide_runtime_start_project(runtime->iot_ide, params_json, response, sizeof(response));
    } else if (strcmp(service_name, "property/get") == 0) {
        rc = iot_ide_runtime_get_connection_snapshot(runtime->iot_ide, response, sizeof(response));
    } else if (strcmp(service_name, "property/set") == 0) {
        (void)iot_ide_gateway_post_properties(gateway, params_json == NULL ? "{}" : params_json);
        snprintf(response, sizeof(response), "{\"success\":1,\"message\":\"property set accepted\"}");
        rc = IOT_IDE_RUNTIME_OK;
    } else {
        runtime_reply_error(gateway, service_name, request_id, "unknown service");
        return;
    }

    printf("service response service=%s rc=%d data=%s\n", service_name, rc, response);
    (void)iot_ide_gateway_reply_service(gateway, service_name, request_id, 200, response);
}

int main(int argc, char **argv) {
    const char *config_path = argc > 1 ? argv[1] : "./device_id.json";
    IecRuntime runtime;
    IotIdeGatewayCallbacks gateway_callbacks;
    IotIdeGatewayOptions gateway_options;
    IotIdeRuntimeCallbacks iot_ide_callbacks;
    IotIdeRuntimeOptions iot_ide_options;
    char product_key[128] = "";
    char device_name[128] = "";
    char mqtt_host[256] = "";

    memset(&runtime, 0, sizeof(runtime));
    memset(&gateway_callbacks, 0, sizeof(gateway_callbacks));
    memset(&gateway_options, 0, sizeof(gateway_options));
    memset(&iot_ide_callbacks, 0, sizeof(iot_ide_callbacks));
    memset(&iot_ide_options, 0, sizeof(iot_ide_options));

    g_runtime = &runtime;
    runtime.running = 1;
    signal(SIGINT, runtime_handle_signal);
    signal(SIGTERM, runtime_handle_signal);

    gateway_callbacks.on_service = runtime_on_service;
    gateway_callbacks.on_log = runtime_on_gateway_log;
    gateway_options.config_path = config_path;
    gateway_options.callbacks = &gateway_callbacks;
    gateway_options.user_data = &runtime;

    if (iot_ide_gateway_create(&gateway_options, &runtime.gateway) != IOT_IDE_GATEWAY_OK) {
        fprintf(stderr, "iot_ide_gateway_create failed\n");
        return 1;
    }

    iot_ide_callbacks.on_event = runtime_on_iot_ide_event;
    iot_ide_callbacks.on_log = runtime_on_iot_ide_log;
    iot_ide_options.work_dir = ".";
    iot_ide_options.callbacks = &iot_ide_callbacks;
    iot_ide_options.user_data = &runtime;

    if (iot_ide_runtime_create(&iot_ide_options, &runtime.iot_ide) != IOT_IDE_RUNTIME_OK) {
        fprintf(stderr, "iot_ide_runtime_create failed\n");
        iot_ide_gateway_destroy(runtime.gateway);
        return 1;
    }

    (void)iot_ide_gateway_get_device_info(runtime.gateway,
                                          product_key,
                                          sizeof(product_key),
                                          device_name,
                                          sizeof(device_name),
                                          mqtt_host,
                                          sizeof(mqtt_host));

    printf("=== iec_runtime ===\n");
    printf("config: %s\n", config_path);
    printf("productKey: %s\n", product_key);
    printf("deviceName: %s\n", device_name);
    printf("mqttHost: %s\n", mqtt_host);

    if (iot_ide_gateway_start(runtime.gateway) != IOT_IDE_GATEWAY_OK) {
        fprintf(stderr, "iot_ide_gateway_start failed\n");
        iot_ide_runtime_destroy(runtime.iot_ide);
        iot_ide_gateway_destroy(runtime.gateway);
        return 1;
    }

    while (runtime.running) {
        sleep(1);
    }

    iot_ide_gateway_stop(runtime.gateway);
    iot_ide_runtime_destroy(runtime.iot_ide);
    iot_ide_gateway_destroy(runtime.gateway);
    printf("iec_runtime exit\n");
    return 0;
}
