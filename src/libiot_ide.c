#include "libiot_ide.h"

#include "app_context.h"
#include "iot_ide_app.h"
#include "json_utils.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct LibIotIde {
    AppContext app;
    int ide_initialized;
    int deploy_initialized;
    int start_initialized;
};

static void libiot_ide_copy_callbacks(AppContext *app, const LibIotIdeOptions *options) {
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

static int libiot_ide_write_json(char *response_json, size_t response_size, int success, const char *message) {
    char escaped[512];

    if (response_json == NULL || response_size == 0) {
        return LIBIOT_IDE_ERR_INVALID_ARGUMENT;
    }

    if (json_escape_string(message == NULL ? "" : message, escaped, sizeof(escaped)) != 0) {
        return LIBIOT_IDE_ERR_INTERNAL;
    }

    if (snprintf(response_json, response_size,
                 "{\"success\":%d,\"message\":\"%s\"}",
                 success ? 1 : 0,
                 escaped) < 0) {
        return LIBIOT_IDE_ERR_INTERNAL;
    }

    return LIBIOT_IDE_OK;
}

static int libiot_ide_check_active_client(LibIotIde *ide, const char *params_json, char *response_json, size_t response_size, const char *operation) {
    char client_id[128] = "";
    char message[256];

    if (ide == NULL || params_json == NULL) {
        return LIBIOT_IDE_ERR_INVALID_ARGUMENT;
    }

    (void)json_get_string(params_json, "clientId", client_id, sizeof(client_id));
    if (ide_connection_manager_is_connected_client(&ide->app.ide_manager, client_id)) {
        return LIBIOT_IDE_OK;
    }

    snprintf(message, sizeof(message), "%s rejected, clientId is not the active IDE connection", operation);
    (void)libiot_ide_write_json(response_json, response_size, 0, message);
    return LIBIOT_IDE_ERR_NOT_ACTIVE_CLIENT;
}

int libiot_ide_get_api_version(void) {
    return LIBIOT_IDE_API_VERSION;
}

int libiot_ide_create(const LibIotIdeOptions *options, LibIotIde **ide) {
    LibIotIde *created;

    if (ide == NULL) {
        return LIBIOT_IDE_ERR_INVALID_ARGUMENT;
    }

    *ide = NULL;
    created = (LibIotIde *)calloc(1, sizeof(*created));
    if (created == NULL) {
        return LIBIOT_IDE_ERR_NO_MEMORY;
    }

    created->app.running = 1;
    libiot_ide_copy_callbacks(&created->app, options);

    if (pthread_mutex_init(&created->app.mqtt_lock, NULL) != 0) {
        free(created);
        return LIBIOT_IDE_ERR_INTERNAL;
    }

    if (ide_connection_manager_init(&created->app.ide_manager, &created->app) != 0) {
        pthread_mutex_destroy(&created->app.mqtt_lock);
        free(created);
        return LIBIOT_IDE_ERR_INTERNAL;
    }
    created->ide_initialized = 1;

    if (deploy_manager_init(&created->app.deploy_manager, &created->app) != 0) {
        libiot_ide_destroy(created);
        return LIBIOT_IDE_ERR_INTERNAL;
    }
    created->deploy_initialized = 1;

    if (start_manager_init(&created->app.start_manager, &created->app) != 0) {
        libiot_ide_destroy(created);
        return LIBIOT_IDE_ERR_INTERNAL;
    }
    created->start_initialized = 1;

    app_emit_runtime_log(&created->app, IOT_IDE_LOG_INFO, "iot ide runtime created");
    app_emit_runtime_event(&created->app, "runtime.created", "{\"success\":true}");

    *ide = created;
    return LIBIOT_IDE_OK;
}

void libiot_ide_destroy(LibIotIde *ide) {
    if (ide == NULL) {
        return;
    }

    app_emit_runtime_event(&ide->app, "runtime.destroying", "{\"success\":true}");

    if (ide->start_initialized) {
        start_manager_cleanup(&ide->app.start_manager);
    }
    if (ide->deploy_initialized) {
        deploy_manager_cleanup(&ide->app.deploy_manager);
    }
    if (ide->ide_initialized) {
        ide_connection_manager_cleanup(&ide->app.ide_manager);
    }

    pthread_mutex_destroy(&ide->app.mqtt_lock);
    free(ide);
}

int libiot_ide_request_connect(LibIotIde *ide, const char *params_json, char *response_json, size_t response_size) {
    int rc;

    if (ide == NULL || params_json == NULL) {
        return LIBIOT_IDE_ERR_INVALID_ARGUMENT;
    }

    rc = ide_connection_manager_handle_request_connect(&ide->app.ide_manager, params_json, response_json, response_size);
    app_emit_runtime_event(&ide->app, "requestConnect.response", response_json == NULL ? "{}" : response_json);
    return rc == 0 ? LIBIOT_IDE_OK : LIBIOT_IDE_ERR_INTERNAL;
}

int libiot_ide_request_disconnect(LibIotIde *ide, const char *params_json, char *response_json, size_t response_size) {
    int rc;

    if (ide == NULL || params_json == NULL) {
        return LIBIOT_IDE_ERR_INVALID_ARGUMENT;
    }

    rc = ide_connection_manager_handle_disconnect(&ide->app.ide_manager, params_json, response_json, response_size);
    app_emit_runtime_event(&ide->app, "requestDisconnect.response", response_json == NULL ? "{}" : response_json);
    return rc == 0 ? LIBIOT_IDE_OK : LIBIOT_IDE_ERR_INTERNAL;
}

int libiot_ide_heartbeat(LibIotIde *ide, const char *params_json, char *response_json, size_t response_size) {
    int rc;

    if (ide == NULL || params_json == NULL) {
        return LIBIOT_IDE_ERR_INVALID_ARGUMENT;
    }

    rc = ide_connection_manager_handle_heartbeat(&ide->app.ide_manager, params_json, response_json, response_size);
    app_emit_runtime_event(&ide->app, "ideHeartbeat.response", response_json == NULL ? "{}" : response_json);
    return rc == 0 ? LIBIOT_IDE_OK : LIBIOT_IDE_ERR_INTERNAL;
}

int libiot_ide_deploy_project(LibIotIde *ide, const char *params_json, char *response_json, size_t response_size) {
    int rc;

    rc = libiot_ide_check_active_client(ide, params_json, response_json, response_size, "deploy");
    if (rc != LIBIOT_IDE_OK) {
        return rc;
    }

    rc = deploy_manager_handle_deploy(&ide->app.deploy_manager, params_json, response_json, response_size);
    app_emit_runtime_event(&ide->app, "deployProject.response", response_json == NULL ? "{}" : response_json);
    return rc == 0 ? LIBIOT_IDE_OK : LIBIOT_IDE_ERR_INTERNAL;
}

int libiot_ide_start_project(LibIotIde *ide, const char *params_json, char *response_json, size_t response_size) {
    int rc;

    rc = libiot_ide_check_active_client(ide, params_json, response_json, response_size, "start");
    if (rc != LIBIOT_IDE_OK) {
        return rc;
    }

    rc = start_manager_handle_start(&ide->app.start_manager, params_json, response_json, response_size);
    app_emit_runtime_event(&ide->app, "startProject.response", response_json == NULL ? "{}" : response_json);
    return rc == 0 ? LIBIOT_IDE_OK : LIBIOT_IDE_ERR_INTERNAL;
}

int libiot_ide_get_connection_snapshot(LibIotIde *ide, char *snapshot_json, size_t snapshot_size) {
    int connected = 0;
    char client_id[128] = "";
    char escaped_client_id[256] = "";
    long long heartbeat_ms = 0;

    if (ide == NULL || snapshot_json == NULL || snapshot_size == 0) {
        return LIBIOT_IDE_ERR_INVALID_ARGUMENT;
    }

    ide_connection_manager_get_snapshot(&ide->app.ide_manager, &connected, client_id, sizeof(client_id), &heartbeat_ms);
    if (json_escape_string(client_id, escaped_client_id, sizeof(escaped_client_id)) != 0) {
        return LIBIOT_IDE_ERR_INTERNAL;
    }

    if (snprintf(snapshot_json, snapshot_size,
                 "{\"connected\":%d,\"clientId\":\"%s\",\"heartbeatMs\":%lld}",
                 connected ? 1 : 0,
                 escaped_client_id,
                 heartbeat_ms) < 0) {
        return LIBIOT_IDE_ERR_INTERNAL;
    }

    return LIBIOT_IDE_OK;
}
