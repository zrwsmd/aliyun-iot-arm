#include "iot_ide_runtime_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * This file is a compact reference for iec_runtime developers.
 *
 * It intentionally keeps all libiot_ide.so API calls in main(), so the
 * integration order is easy to read:
 *
 * 1. register callbacks
 * 2. create runtime
 * 3. dispatch Aliyun service params to libiot_ide.so APIs
 * 4. publish API responses back to Aliyun
 * 5. destroy runtime
 *
 * A real iec_runtime should replace the printf calls in callbacks with its
 * own Aliyun MQTT publish/reply functions.
 */

static void on_iot_ide_event(void *user_data, const char *event_name, const char *event_json) {
    (void)user_data;

    printf("[callback:on_event] event=%s json=%s\n",
           event_name == NULL ? "" : event_name,
           event_json == NULL ? "" : event_json);

    /*
     * Real iec_runtime mapping:
     *
     * event_name == "property.post":
     *   publish event_json as params to:
     *   /sys/{productKey}/{deviceName}/thing/event/property/post
     *
     * event_name == "trace.publish":
     *   publish event_json to:
     *   /{productKey}/{deviceName}/user/trace/data
     *
     * event_name == "service.reply":
     *   optional centralized service reply path.
     */
}

static void on_iot_ide_log(void *user_data, int level, const char *message) {
    (void)user_data;
    printf("[callback:on_log] level=%d message=%s\n", level, message == NULL ? "" : message);
}

int main(void) {
    IotIdeRuntime *iot_ide = NULL;
    IotIdeRuntimeCallbacks callbacks;
    IotIdeRuntimeOptions options;
    char response[8192];
    int rc;

    printf("=== iec_runtime calls libiot_ide.so main example ===\n");
    printf("api version: %d\n", iot_ide_runtime_get_api_version());

    /*
     * Step 1:
     * Register callbacks.
     *
     * libiot_ide.so uses callbacks to push events back to iec_runtime.
     * In real iec_runtime, user_data can point to your global runtime context.
     */
    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.on_event = on_iot_ide_event;
    callbacks.on_log = on_iot_ide_log;

    /*
     * Step 2:
     * Create libiot_ide runtime.
     *
     * libiot_ide.so does not connect Aliyun MQTT by itself. The real
     * iec_runtime process owns Aliyun connection and passes service params
     * into these APIs.
     */
    memset(&options, 0, sizeof(options));
    options.work_dir = "/opt/iec-runtime";
    options.callbacks = &callbacks;
    options.user_data = NULL;

    rc = iot_ide_runtime_create(&options, &iot_ide);
    printf("\n[create]\nrc=%d\n", rc);
    if (rc != IOT_IDE_RUNTIME_OK) {
        return 1;
    }

    /*
     * Step 3:
     * Simulate receiving Aliyun service: requestConnect.
     *
     * Real flow:
     *   Aliyun topic:
     *   /sys/{pk}/{dn}/thing/service/requestConnect
     *
     *   iec_runtime extracts payload.params and passes it here.
     */
    memset(response, 0, sizeof(response));
    rc = iot_ide_runtime_request_connect(
        iot_ide,
        "{\"clientId\":\"ide-client-12345678\","
        "\"clientInfo\":{\"platform\":\"nodejs\",\"version\":\"1.0.0\",\"hostname\":\"dev-pc\"}}",
        response,
        sizeof(response));
    printf("\n[requestConnect]\nrc=%d\nresponse=%s\n", rc, response);

    /*
     * Real iec_runtime should publish the response above to:
     * /sys/{pk}/{dn}/thing/service/requestConnect_reply
     *
     * Reply payload:
     * {
     *   "id": "{request_id}",
     *   "code": 200,
     *   "data": response
     * }
     */

    /*
     * Step 4:
     * Simulate receiving Aliyun service: ideHeartbeat.
     *
     * This is business heartbeat from IDE, not MQTT protocol heartbeat.
     */
    memset(response, 0, sizeof(response));
    rc = iot_ide_runtime_heartbeat(
        iot_ide,
        "{\"clientId\":\"ide-client-12345678\"}",
        response,
        sizeof(response));
    printf("\n[ideHeartbeat]\nrc=%d\nresponse=%s\n", rc, response);

    /*
     * Step 5:
     * Simulate receiving Aliyun service: deployProject.
     *
     * This example intentionally keeps downloadUrl empty, so running this
     * sample will not download anything. In production, pass the real zip URL.
     */
    memset(response, 0, sizeof(response));
    rc = iot_ide_runtime_deploy_project(
        iot_ide,
        "{\"clientId\":\"ide-client-12345678\","
        "\"projectName\":\"demo\","
        "\"downloadUrl\":\"\","
        "\"deployPath\":\"/tmp/deploy\","
        "\"deployCommand\":\"\"}",
        response,
        sizeof(response));
    printf("\n[deployProject]\nrc=%d\nresponse=%s\n", rc, response);

    /*
     * Production deployProject params usually look like:
     *
     * {
     *   "clientId": "ide-client-12345678",
     *   "projectName": "demo",
     *   "downloadUrl": "http://example.com/demo.zip",
     *   "deployPath": "/opt/iec-projects",
     *   "deployCommand": "./install.sh"
     * }
     */

    /*
     * Step 6:
     * Simulate receiving Aliyun service: startProject.
     *
     * startProject runs startCommand inside:
     *   {deployPath}/{projectName}
     *
     * The mkdir below only makes this example runnable.
     */
    (void)system("mkdir -p /tmp/deploy/demo");

    memset(response, 0, sizeof(response));
    rc = iot_ide_runtime_start_project(
        iot_ide,
        "{\"clientId\":\"ide-client-12345678\","
        "\"projectName\":\"demo\","
        "\"deployPath\":\"/tmp/deploy\","
        "\"startCommand\":\"echo hello from libiot_ide\"}",
        response,
        sizeof(response));
    printf("\n[startProject]\nrc=%d\nresponse=%s\n", rc, response);

    /*
     * Step 7:
     * Query current connection snapshot.
     *
     * Real iec_runtime can use this when handling property/get or debugging.
     */
    memset(response, 0, sizeof(response));
    rc = iot_ide_runtime_get_connection_snapshot(iot_ide, response, sizeof(response));
    printf("\n[getConnectionSnapshot]\nrc=%d\nresponse=%s\n", rc, response);

    /*
     * Step 8:
     * Simulate receiving Aliyun service: requestDisconnect.
     */
    memset(response, 0, sizeof(response));
    rc = iot_ide_runtime_request_disconnect(
        iot_ide,
        "{\"clientId\":\"ide-client-12345678\"}",
        response,
        sizeof(response));
    printf("\n[requestDisconnect]\nrc=%d\nresponse=%s\n", rc, response);

    /*
     * Step 9:
     * Destroy runtime before iec_runtime exits.
     */
    iot_ide_runtime_destroy(iot_ide);
    iot_ide = NULL;

    sleep(1);
    printf("\nexample finished\n");
    return 0;
}
