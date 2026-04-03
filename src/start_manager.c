#include "start_manager.h"

#include "app_context.h"
#include "json_utils.h"
#include "shell_utils.h"
#include "string_builder.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *start_escape_alloc(const char *text) {
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

static int start_build_result_json(int success, const char *message, char *response_json, size_t response_size) {
    char *escaped = start_escape_alloc(message);
    int rc;

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

int start_manager_init(StartManager *manager, struct AppContext *app) {
    if (manager == NULL || app == NULL) {
        return -1;
    }

    memset(manager, 0, sizeof(*manager));
    manager->app = app;
    return pthread_mutex_init(&manager->lock, NULL) == 0 ? 0 : -1;
}

void start_manager_cleanup(StartManager *manager) {
    if (manager == NULL) {
        return;
    }
    pthread_mutex_destroy(&manager->lock);
}

int start_manager_handle_start(StartManager *manager, const char *params_json, char *response_json, size_t response_size) {
    char project_name[128] = "project";
    char deploy_path[512] = "/tmp/deploy";
    char start_command[1024] = "";
    char target_path[768];
    char message[512];
    int started_in_background = 0;
    int exit_code = -1;
    int child_pid = -1;
    int rc;
    StringBuilder log;

    if (manager == NULL || params_json == NULL) {
        return -1;
    }

    (void)json_get_string(params_json, "projectName", project_name, sizeof(project_name));
    (void)json_get_string(params_json, "deployPath", deploy_path, sizeof(deploy_path));
    (void)json_get_string(params_json, "startCommand", start_command, sizeof(start_command));

    if (start_command[0] == '\0') {
        return start_build_result_json(0, "startCommand is empty", response_json, response_size);
    }

    if (snprintf(target_path, sizeof(target_path), "%s/%s", deploy_path, project_name) < 0) {
        return start_build_result_json(0, "failed to build target path", response_json, response_size);
    }

    pthread_mutex_lock(&manager->lock);
    if (manager->busy) {
        pthread_mutex_unlock(&manager->lock);
        return start_build_result_json(0, "another start task is still running", response_json, response_size);
    }
    manager->busy = 1;
    pthread_mutex_unlock(&manager->lock);

    if (sb_init(&log, 1024) != 0) {
        pthread_mutex_lock(&manager->lock);
        manager->busy = 0;
        pthread_mutex_unlock(&manager->lock);
        return start_build_result_json(0, "failed to allocate log buffer", response_json, response_size);
    }

    sb_appendf(&log, "targetPath=%s\n", target_path);
    sb_appendf(&log, "command=%s\n", start_command);
    rc = start_background_command(start_command, target_path, 2, &log, &started_in_background, &exit_code, &child_pid);

    pthread_mutex_lock(&manager->lock);
    manager->busy = 0;
    pthread_mutex_unlock(&manager->lock);

    if (rc == 0) {
        if (started_in_background) {
            snprintf(message, sizeof(message), "service started in background, pid=%d", child_pid);
        } else {
            snprintf(message, sizeof(message), "start command finished successfully");
        }
        sb_free(&log);
        return start_build_result_json(1, message, response_json, response_size);
    }

    snprintf(message, sizeof(message), "start failed, exitCode=%d, log=%s", exit_code, log.data == NULL ? "" : log.data);
    sb_free(&log);
    return start_build_result_json(0, message, response_json, response_size);
}
