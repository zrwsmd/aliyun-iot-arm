#include "libiot_ide.h"

#include "app_context.h"
#include "iot_ide_app.h"
#include "json_utils.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct IotIdeRuntime {
    AppContext app;
    int ide_initialized;
    int deploy_initialized;
    int start_initialized;
};

static void runtime_copy_callbacks(AppContext *app, const IotIdeRuntimeOptions *options) {
    app->runtime_library_mode = 1;
    app->runtime_user_data = options == NULL ? NULL : options->user_data;

    if (options != NULL && options->callbacks != NULL) {
        app->runtime_event_cb = options->callbacks->on_event;
        app->runtime_log_cb = options->callbacks->on_log;
    }

    if (options != NULL && options->work_dir != NULL && options->work_dir[0] != '\0') {
        snprintf(app->runtime_work_dir, sizeof(app->runtime_work_dir), "%s", options->work_dir);
    } else {
        snprintf(app->runtime_work_dir, sizeof(app->runtime_work_dir), ".");
    }
}

static int runtime_write_json(char *response_json, size_t response_size, int success, const char *message) {
    char escaped[512];

    if (response_json == NULL || response_size == 0) {
        return IOT_IDE_RUNTIME_ERR_INVALID_ARGUMENT;
    }

    if (json_escape_string(message == NULL ? "" : message, escaped, sizeof(escaped)) != 0) {
        return IOT_IDE_RUNTIME_ERR_INTERNAL;
    }

    if (snprintf(response_json, response_size,
                 "{\"success\":%d,\"message\":\"%s\"}",
                 success ? 1 : 0,
                 escaped) < 0) {
        return IOT_IDE_RUNTIME_ERR_INTERNAL;
    }

    return IOT_IDE_RUNTIME_OK;
}

static int runtime_check_active_client(IotIdeRuntime *runtime, const char *params_json, char *response_json, size_t response_size, const char *operation) {
    char client_id[128] = "";
    char message[256];

    if (runtime == NULL || params_json == NULL) {
        return IOT_IDE_RUNTIME_ERR_INVALID_ARGUMENT;
    }

    (void)json_get_string(params_json, "clientId", client_id, sizeof(client_id));
    if (ide_connection_manager_is_connected_client(&runtime->app.ide_manager, client_id)) {
        return IOT_IDE_RUNTIME_OK;
    }

    snprintf(message, sizeof(message), "%s rejected, clientId is not the active IDE connection", operation);
    (void)runtime_write_json(response_json, response_size, 0, message);
    return IOT_IDE_RUNTIME_ERR_NOT_ACTIVE_CLIENT;
}

int libiot_ide_get_api_version(void) {
    return IOT_IDE_RUNTIME_API_VERSION;
}

int libiot_ide_create(const IotIdeRuntimeOptions *options, IotIdeRuntime **runtime) {
    IotIdeRuntime *created;

    if (runtime == NULL) {
        return IOT_IDE_RUNTIME_ERR_INVALID_ARGUMENT;
    }

    *runtime = NULL;
    created = (IotIdeRuntime *)calloc(1, sizeof(*created));
    if (created == NULL) {
        return IOT_IDE_RUNTIME_ERR_NO_MEMORY;
    }

    created->app.running = 1;
    runtime_copy_callbacks(&created->app, options);

    if (pthread_mutex_init(&created->app.mqtt_lock, NULL) != 0) {
        free(created);
        return IOT_IDE_RUNTIME_ERR_INTERNAL;
    }

    if (ide_connection_manager_init(&created->app.ide_manager, &created->app) != 0) {
        pthread_mutex_destroy(&created->app.mqtt_lock);
        free(created);
        return IOT_IDE_RUNTIME_ERR_INTERNAL;
    }
    created->ide_initialized = 1;

    if (deploy_manager_init(&created->app.deploy_manager, &created->app) != 0) {
        libiot_ide_destroy(created);
        return IOT_IDE_RUNTIME_ERR_INTERNAL;
    }
    created->deploy_initialized = 1;

    if (start_manager_init(&created->app.start_manager, &created->app) != 0) {
        libiot_ide_destroy(created);
        return IOT_IDE_RUNTIME_ERR_INTERNAL;
    }
    created->start_initialized = 1;

    app_emit_runtime_log(&created->app, IOT_IDE_LOG_INFO, "iot ide runtime created");
    app_emit_runtime_event(&created->app, "runtime.created", "{\"success\":true}");

    *runtime = created;
    return IOT_IDE_RUNTIME_OK;
}

void libiot_ide_destroy(IotIdeRuntime *runtime) {
    if (runtime == NULL) {
        return;
    }

    app_emit_runtime_event(&runtime->app, "runtime.destroying", "{\"success\":true}");

    if (runtime->start_initialized) {
        start_manager_cleanup(&runtime->app.start_manager);
    }
    if (runtime->deploy_initialized) {
        deploy_manager_cleanup(&runtime->app.deploy_manager);
    }
    if (runtime->ide_initialized) {
        ide_connection_manager_cleanup(&runtime->app.ide_manager);
    }

    pthread_mutex_destroy(&runtime->app.mqtt_lock);
    free(runtime);
}

int libiot_ide_request_connect(IotIdeRuntime *runtime, const char *params_json, char *response_json, size_t response_size) {
    int rc;

    if (runtime == NULL || params_json == NULL) {
        return IOT_IDE_RUNTIME_ERR_INVALID_ARGUMENT;
    }

    rc = ide_connection_manager_handle_request_connect(&runtime->app.ide_manager, params_json, response_json, response_size);
    app_emit_runtime_event(&runtime->app, "requestConnect.response", response_json == NULL ? "{}" : response_json);
    return rc == 0 ? IOT_IDE_RUNTIME_OK : IOT_IDE_RUNTIME_ERR_INTERNAL;
}

int libiot_ide_request_disconnect(IotIdeRuntime *runtime, const char *params_json, char *response_json, size_t response_size) {
    int rc;

    if (runtime == NULL || params_json == NULL) {
        return IOT_IDE_RUNTIME_ERR_INVALID_ARGUMENT;
    }

    rc = ide_connection_manager_handle_disconnect(&runtime->app.ide_manager, params_json, response_json, response_size);
    app_emit_runtime_event(&runtime->app, "requestDisconnect.response", response_json == NULL ? "{}" : response_json);
    return rc == 0 ? IOT_IDE_RUNTIME_OK : IOT_IDE_RUNTIME_ERR_INTERNAL;
}

int libiot_ide_heartbeat(IotIdeRuntime *runtime, const char *params_json, char *response_json, size_t response_size) {
    int rc;

    if (runtime == NULL || params_json == NULL) {
        return IOT_IDE_RUNTIME_ERR_INVALID_ARGUMENT;
    }

    rc = ide_connection_manager_handle_heartbeat(&runtime->app.ide_manager, params_json, response_json, response_size);
    app_emit_runtime_event(&runtime->app, "ideHeartbeat.response", response_json == NULL ? "{}" : response_json);
    return rc == 0 ? IOT_IDE_RUNTIME_OK : IOT_IDE_RUNTIME_ERR_INTERNAL;
}

int libiot_ide_deploy_project(IotIdeRuntime *runtime, const char *params_json, char *response_json, size_t response_size) {
    int rc;

    rc = runtime_check_active_client(runtime, params_json, response_json, response_size, "deploy");
    if (rc != IOT_IDE_RUNTIME_OK) {
        return rc;
    }

    rc = deploy_manager_handle_deploy(&runtime->app.deploy_manager, params_json, response_json, response_size);
    app_emit_runtime_event(&runtime->app, "deployProject.response", response_json == NULL ? "{}" : response_json);
    return rc == 0 ? IOT_IDE_RUNTIME_OK : IOT_IDE_RUNTIME_ERR_INTERNAL;
}

int libiot_ide_start_project(IotIdeRuntime *runtime, const char *params_json, char *response_json, size_t response_size) {
    int rc;

    rc = runtime_check_active_client(runtime, params_json, response_json, response_size, "start");
    if (rc != IOT_IDE_RUNTIME_OK) {
        return rc;
    }

    rc = start_manager_handle_start(&runtime->app.start_manager, params_json, response_json, response_size);
    app_emit_runtime_event(&runtime->app, "startProject.response", response_json == NULL ? "{}" : response_json);
    return rc == 0 ? IOT_IDE_RUNTIME_OK : IOT_IDE_RUNTIME_ERR_INTERNAL;
}

int libiot_ide_get_connection_snapshot(IotIdeRuntime *runtime, char *snapshot_json, size_t snapshot_size) {
    int connected = 0;
    char client_id[128] = "";
    char escaped_client_id[256] = "";
    long long heartbeat_ms = 0;

    if (runtime == NULL || snapshot_json == NULL || snapshot_size == 0) {
        return IOT_IDE_RUNTIME_ERR_INVALID_ARGUMENT;
    }

    ide_connection_manager_get_snapshot(&runtime->app.ide_manager, &connected, client_id, sizeof(client_id), &heartbeat_ms);
    if (json_escape_string(client_id, escaped_client_id, sizeof(escaped_client_id)) != 0) {
        return IOT_IDE_RUNTIME_ERR_INTERNAL;
    }

    if (snprintf(snapshot_json, snapshot_size,
                 "{\"connected\":%d,\"clientId\":\"%s\",\"heartbeatMs\":%lld}",
                 connected ? 1 : 0,
                 escaped_client_id,
                 heartbeat_ms) < 0) {
        return IOT_IDE_RUNTIME_ERR_INTERNAL;
    }

    return IOT_IDE_RUNTIME_OK;
}
