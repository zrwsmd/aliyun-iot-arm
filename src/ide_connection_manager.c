#include "ide_connection_manager.h"

#include "app_context.h"
#include "iot_ide_app.h"
#include "json_utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define HEARTBEAT_TIMEOUT_MS (2LL * 60LL * 1000LL)
#define HEARTBEAT_CHECK_INTERVAL_SEC 30

static char *ide_escape_alloc(const char *text) {
    size_t input_len;
    size_t buffer_size;
    char *buffer;

    if (text == NULL) {
        text = "";
    }

    input_len = strlen(text);
    buffer_size = input_len * 2 + 32;
    buffer = (char *)malloc(buffer_size);
    if (buffer == NULL) {
        return NULL;
    }

    if (json_escape_string(text, buffer, buffer_size) != 0) {
        free(buffer);
        return NULL;
    }

    return buffer;
}

static int ide_build_result_json(int success, const char *message, char *response_json, size_t response_size) {
    char *escaped;
    int rc;

    if (response_json == NULL || response_size == 0) {
        return -1;
    }

    escaped = ide_escape_alloc(message);
    if (escaped == NULL) {
        return -1;
    }

    rc = snprintf(response_json, response_size,
                  "{\"success\":%d,\"message\":\"%s\"}",
                  success ? 1 : 0,
                  escaped);
    free(escaped);

    return (rc >= 0 && (size_t)rc < response_size) ? 0 : -1;
}

static void ide_report_full_state(IdeConnectionManager *manager, int connected, const char *client_id, const char *client_info, long long heartbeat_ms) {
    char params[4096];

    if (!connected) {
        snprintf(params, sizeof(params),
                 "{\"hasIDEConnected\":0,\"IDEInfo\":\"\",\"IDEHeartbeat\":\"0\"}");
        (void)app_post_properties(manager->app, params);
        return;
    }

    if (client_info != NULL && (client_info[0] == '{' || client_info[0] == '[')) {
        char *escaped_client_id = ide_escape_alloc(client_id);
        char ide_info[2048];
        char *escaped_ide_info;

        if (escaped_client_id == NULL) {
            return;
        }

        if (snprintf(ide_info, sizeof(ide_info),
                     "{\"clientId\":\"%s\",\"clientInfo\":%s,\"connectTime\":%lld}",
                     escaped_client_id,
                     client_info,
                     heartbeat_ms) < 0) {
            free(escaped_client_id);
            return;
        }
        free(escaped_client_id);

        escaped_ide_info = ide_escape_alloc(ide_info);
        if (escaped_ide_info == NULL) {
            return;
        }

        snprintf(params, sizeof(params),
                 "{\"hasIDEConnected\":1,\"IDEInfo\":\"%s\",\"IDEHeartbeat\":\"%lld\"}",
                 escaped_ide_info,
                 heartbeat_ms);
        free(escaped_ide_info);
    } else {
        char *escaped_client_id = ide_escape_alloc(client_id);
        char *escaped_client_info = ide_escape_alloc(client_info == NULL ? "" : client_info);
        char ide_info[2048];
        char *escaped_ide_info;

        if (escaped_client_id == NULL || escaped_client_info == NULL) {
            free(escaped_client_id);
            free(escaped_client_info);
            return;
        }

        if (snprintf(ide_info, sizeof(ide_info),
                     "{\"clientId\":\"%s\",\"clientInfo\":\"%s\",\"connectTime\":%lld}",
                     escaped_client_id,
                     escaped_client_info,
                     heartbeat_ms) < 0) {
            free(escaped_client_id);
            free(escaped_client_info);
            return;
        }
        free(escaped_client_id);
        free(escaped_client_info);

        escaped_ide_info = ide_escape_alloc(ide_info);
        if (escaped_ide_info == NULL) {
            return;
        }

        snprintf(params, sizeof(params),
                 "{\"hasIDEConnected\":1,\"IDEInfo\":\"%s\",\"IDEHeartbeat\":\"%lld\"}",
                 escaped_ide_info,
                 heartbeat_ms);
        free(escaped_ide_info);
    }

    (void)app_post_properties(manager->app, params);
}

static void ide_report_heartbeat(IdeConnectionManager *manager, long long heartbeat_ms) {
    char params[256];
    snprintf(params, sizeof(params), "{\"IDEHeartbeat\":\"%lld\"}", heartbeat_ms);
    (void)app_post_properties(manager->app, params);
}

static void ide_unlock_internal(IdeConnectionManager *manager) {
    manager->connected = 0;
    manager->current_client_id[0] = '\0';
    manager->current_client_info[0] = '\0';
    manager->last_heartbeat_ms = 0;
}

static void *ide_heartbeat_thread(void *arg) {
    IdeConnectionManager *manager = (IdeConnectionManager *)arg;

    while (manager->heartbeat_thread_running) {
        int should_release = 0;

        sleep(HEARTBEAT_CHECK_INTERVAL_SEC);
        if (!manager->heartbeat_thread_running) {
            break;
        }

        pthread_mutex_lock(&manager->lock);
        if (manager->connected && app_now_ms() - manager->last_heartbeat_ms > HEARTBEAT_TIMEOUT_MS) {
            ide_unlock_internal(manager);
            should_release = 1;
        }
        pthread_mutex_unlock(&manager->lock);

        if (should_release) {
            ide_report_full_state(manager, 0, "", "", 0);
        }
    }

    return NULL;
}

int ide_connection_manager_init(IdeConnectionManager *manager, struct AppContext *app) {
    if (manager == NULL || app == NULL) {
        return -1;
    }

    memset(manager, 0, sizeof(*manager));
    manager->app = app;
    if (pthread_mutex_init(&manager->lock, NULL) != 0) {
        return -1;
    }

    manager->heartbeat_thread_running = 1;
    if (pthread_create(&manager->heartbeat_thread, NULL, ide_heartbeat_thread, manager) != 0) {
        pthread_mutex_destroy(&manager->lock);
        return -1;
    }

    return 0;
}

void ide_connection_manager_cleanup(IdeConnectionManager *manager) {
    if (manager == NULL) {
        return;
    }

    manager->heartbeat_thread_running = 0;
    pthread_join(manager->heartbeat_thread, NULL);
    pthread_mutex_destroy(&manager->lock);
}

void ide_connection_manager_clear_cloud_state(IdeConnectionManager *manager) {
    if (manager == NULL) {
        return;
    }

    pthread_mutex_lock(&manager->lock);
    ide_unlock_internal(manager);
    pthread_mutex_unlock(&manager->lock);
    ide_report_full_state(manager, 0, "", "", 0);
}

int ide_connection_manager_handle_request_connect(IdeConnectionManager *manager, const char *params_json, char *response_json, size_t response_size) {
    char client_id[128] = "unknown";
    char client_info[512] = "";
    int success = 0;
    char message[256];
    long long now_ms;

    if (manager == NULL || params_json == NULL) {
        return -1;
    }

    (void)json_get_string(params_json, "clientId", client_id, sizeof(client_id));
    (void)json_get_raw_value(params_json, "clientInfo", client_info, sizeof(client_info));
    now_ms = app_now_ms();

    pthread_mutex_lock(&manager->lock);
    if (!manager->connected) {
        manager->connected = 1;
        manager->last_heartbeat_ms = now_ms;
        snprintf(manager->current_client_id, sizeof(manager->current_client_id), "%s", client_id);
        snprintf(manager->current_client_info, sizeof(manager->current_client_info), "%s", client_info);
        snprintf(message, sizeof(message), "connect accepted");
        success = 1;
    } else if (strcmp(manager->current_client_id, client_id) == 0) {
        manager->last_heartbeat_ms = now_ms;
        snprintf(manager->current_client_info, sizeof(manager->current_client_info), "%s", client_info);
        snprintf(message, sizeof(message), "reconnect accepted");
        success = 1;
    } else if (now_ms - manager->last_heartbeat_ms > HEARTBEAT_TIMEOUT_MS) {
        manager->connected = 1;
        manager->last_heartbeat_ms = now_ms;
        snprintf(manager->current_client_id, sizeof(manager->current_client_id), "%s", client_id);
        snprintf(manager->current_client_info, sizeof(manager->current_client_info), "%s", client_info);
        snprintf(message, sizeof(message), "previous IDE heartbeat timed out, lock transferred");
        success = 1;
    } else {
        snprintf(message, sizeof(message), "device is occupied by %s", manager->current_client_id);
        success = 0;
    }
    pthread_mutex_unlock(&manager->lock);

    if (success) {
        ide_report_full_state(manager, 1, client_id, client_info, now_ms);
    }

    return ide_build_result_json(success, message, response_json, response_size);
}

int ide_connection_manager_handle_disconnect(IdeConnectionManager *manager, const char *params_json, char *response_json, size_t response_size) {
    char client_id[128] = "unknown";
    int success = 0;
    char message[256];
    int should_clear = 0;

    if (manager == NULL || params_json == NULL) {
        return -1;
    }

    (void)json_get_string(params_json, "clientId", client_id, sizeof(client_id));

    pthread_mutex_lock(&manager->lock);
    if (!manager->connected) {
        snprintf(message, sizeof(message), "no active IDE connection");
        success = 1;
    } else if (strcmp(manager->current_client_id, client_id) != 0) {
        snprintf(message, sizeof(message), "disconnect rejected, active client is %s", manager->current_client_id);
        success = 0;
    } else {
        ide_unlock_internal(manager);
        snprintf(message, sizeof(message), "disconnect accepted");
        success = 1;
        should_clear = 1;
    }
    pthread_mutex_unlock(&manager->lock);

    if (should_clear) {
        ide_report_full_state(manager, 0, "", "", 0);
    }

    return ide_build_result_json(success, message, response_json, response_size);
}

int ide_connection_manager_handle_heartbeat(IdeConnectionManager *manager, const char *params_json, char *response_json, size_t response_size) {
    char client_id[128] = "unknown";
    int success = 0;
    char message[256];
    long long now_ms;

    if (manager == NULL || params_json == NULL) {
        return -1;
    }

    (void)json_get_string(params_json, "clientId", client_id, sizeof(client_id));
    now_ms = app_now_ms();

    pthread_mutex_lock(&manager->lock);
    if (!manager->connected || strcmp(manager->current_client_id, client_id) != 0) {
        snprintf(message, sizeof(message), "heartbeat rejected, active client is %s", manager->current_client_id);
        success = 0;
    } else {
        manager->last_heartbeat_ms = now_ms;
        snprintf(message, sizeof(message), "heartbeat updated");
        success = 1;
    }
    pthread_mutex_unlock(&manager->lock);

    if (success) {
        ide_report_heartbeat(manager, now_ms);
    }

    return ide_build_result_json(success, message, response_json, response_size);
}

int ide_connection_manager_is_connected_client(IdeConnectionManager *manager, const char *client_id) {
    int matched;

    if (manager == NULL || client_id == NULL || *client_id == '\0') {
        return 0;
    }

    pthread_mutex_lock(&manager->lock);
    matched = manager->connected && strcmp(manager->current_client_id, client_id) == 0;
    pthread_mutex_unlock(&manager->lock);
    return matched;
}

void ide_connection_manager_get_snapshot(IdeConnectionManager *manager, int *connected, char *client_id, size_t client_id_size, long long *heartbeat_ms) {
    if (manager == NULL) {
        return;
    }

    pthread_mutex_lock(&manager->lock);
    if (connected != NULL) {
        *connected = manager->connected;
    }
    if (client_id != NULL && client_id_size > 0) {
        snprintf(client_id, client_id_size, "%s", manager->current_client_id);
    }
    if (heartbeat_ms != NULL) {
        *heartbeat_ms = manager->last_heartbeat_ms;
    }
    pthread_mutex_unlock(&manager->lock);
}
