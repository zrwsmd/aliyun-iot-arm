#include "iot_ide_runtime_api.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void on_event(void *user_data, const char *event_name, const char *event_json) {
    (void)user_data;
    printf("[event] %s %s\n", event_name == NULL ? "" : event_name, event_json == NULL ? "" : event_json);
}

static void on_log(void *user_data, int level, const char *message) {
    (void)user_data;
    printf("[log:%d] %s\n", level, message == NULL ? "" : message);
}

static void print_call(const char *name, int rc, const char *response) {
    printf("\n== %s ==\n", name);
    printf("rc=%d\n", rc);
    printf("response=%s\n", response == NULL ? "" : response);
}

int main(void) {
    IotIdeRuntime *runtime = NULL;
    IotIdeRuntimeCallbacks callbacks;
    IotIdeRuntimeOptions options;
    char response[4096];
    int rc;

    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.on_event = on_event;
    callbacks.on_log = on_log;

    memset(&options, 0, sizeof(options));
    options.work_dir = ".";
    options.callbacks = &callbacks;

    printf("iot ide runtime api version: %d\n", iot_ide_runtime_get_api_version());

    rc = iot_ide_runtime_create(&options, &runtime);
    print_call("create", rc, rc == 0 ? "created" : "failed");
    if (rc != 0) {
        return 1;
    }

    rc = iot_ide_runtime_request_connect(
        runtime,
        "{\"clientId\":\"ide-a\",\"clientInfo\":{\"name\":\"api-test\",\"host\":\"buildroot\"}}",
        response,
        sizeof(response));
    print_call("requestConnect ide-a", rc, response);

    rc = iot_ide_runtime_request_connect(
        runtime,
        "{\"clientId\":\"ide-b\",\"clientInfo\":{\"name\":\"second-ide\"}}",
        response,
        sizeof(response));
    print_call("requestConnect ide-b rejected", rc, response);

    rc = iot_ide_runtime_heartbeat(
        runtime,
        "{\"clientId\":\"ide-a\"}",
        response,
        sizeof(response));
    print_call("ideHeartbeat ide-a", rc, response);

    rc = iot_ide_runtime_get_connection_snapshot(runtime, response, sizeof(response));
    print_call("snapshot", rc, response);

    rc = iot_ide_runtime_deploy_project(
        runtime,
        "{\"clientId\":\"ide-b\",\"projectName\":\"demo\",\"downloadUrl\":\"\",\"deployPath\":\"/tmp/deploy\"}",
        response,
        sizeof(response));
    print_call("deployProject inactive ide-b", rc, response);

    rc = iot_ide_runtime_start_project(
        runtime,
        "{\"clientId\":\"ide-a\",\"projectName\":\"demo\",\"deployPath\":\"/tmp/deploy\",\"startCommand\":\"echo hello from iot ide runtime\"}",
        response,
        sizeof(response));
    print_call("startProject ide-a", rc, response);

    rc = iot_ide_runtime_request_disconnect(
        runtime,
        "{\"clientId\":\"ide-a\"}",
        response,
        sizeof(response));
    print_call("requestDisconnect ide-a", rc, response);

    sleep(1);
    iot_ide_runtime_destroy(runtime);
    return 0;
}
