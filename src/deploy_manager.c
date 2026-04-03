#include "deploy_manager.h"

#include "app_context.h"
#include "iot_ide_app.h"
#include "json_utils.h"
#include "shell_utils.h"
#include "string_builder.h"

#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DEPLOY_DOWNLOAD_TIMEOUT_SEC 300
#define DEPLOY_COMMAND_TIMEOUT_SEC 600

typedef struct DeployTask {
    DeployManager *manager;
    char project_name[128];
    char download_url[1024];
    char deploy_path[512];
    char deploy_command[1024];
} DeployTask;

static char *deploy_escape_alloc(const char *text) {
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

static int deploy_build_result_json(int success, const char *message, const char *deploy_log, char *response_json, size_t response_size) {
    char *escaped_message;
    char *escaped_log;
    int rc;

    if (response_json == NULL || response_size == 0) {
        return -1;
    }

    escaped_message = deploy_escape_alloc(message);
    escaped_log = deploy_escape_alloc(deploy_log == NULL ? "" : deploy_log);
    if (escaped_message == NULL || escaped_log == NULL) {
        free(escaped_message);
        free(escaped_log);
        return -1;
    }

    rc = snprintf(response_json, response_size,
                  "{\"success\":%d,\"message\":\"%s\",\"deployLog\":\"%s\"}",
                  success ? 1 : 0,
                  escaped_message,
                  escaped_log);
    free(escaped_message);
    free(escaped_log);

    return (rc >= 0 && (size_t)rc < response_size) ? 0 : -1;
}

static void deploy_report_status(DeployManager *manager, int success, const char *message, const char *deploy_log,
                                 const char *project_name, const char *deploy_path) {
    char *escaped_message = NULL;
    char *escaped_log = NULL;
    char *escaped_project = NULL;
    char *escaped_path = NULL;
    char *escaped_status = NULL;
    StringBuilder status_json;
    StringBuilder params_json;

    if (manager == NULL || manager->app == NULL) {
        return;
    }

    if (sb_init(&status_json, 1024) != 0 || sb_init(&params_json, 1024) != 0) {
        sb_free(&status_json);
        sb_free(&params_json);
        return;
    }

    escaped_message = deploy_escape_alloc(message);
    escaped_log = deploy_escape_alloc(deploy_log == NULL ? "" : deploy_log);
    escaped_project = deploy_escape_alloc(project_name == NULL ? "" : project_name);
    escaped_path = deploy_escape_alloc(deploy_path == NULL ? "" : deploy_path);
    if (escaped_message == NULL || escaped_log == NULL || escaped_project == NULL || escaped_path == NULL) {
        goto cleanup;
    }

    sb_appendf(&status_json,
               "{\"success\":%s,\"message\":\"%s\",\"deployLog\":\"%s\",\"timestamp\":%lld,\"projectName\":\"%s\",\"deployPath\":\"%s\"}",
               success ? "true" : "false",
               escaped_message,
               escaped_log,
               app_now_ms(),
               escaped_project,
               escaped_path);

    escaped_status = (char *)malloc(status_json.length * 2 + 32);
    if (escaped_status == NULL) {
        goto cleanup;
    }
    if (json_escape_string(status_json.data, escaped_status, status_json.length * 2 + 32) != 0) {
        goto cleanup;
    }

    sb_appendf(&params_json, "{\"deployStatus\":\"%s\"}", escaped_status);
    (void)app_post_properties(manager->app, params_json.data);

cleanup:
    free(escaped_message);
    free(escaped_log);
    free(escaped_project);
    free(escaped_path);
    free(escaped_status);
    sb_free(&status_json);
    sb_free(&params_json);
}

static void deploy_sanitize_name(const char *input, char *output, size_t output_size) {
    size_t idx = 0;

    if (output == NULL || output_size == 0) {
        return;
    }

    if (input == NULL || *input == '\0') {
        snprintf(output, output_size, "project");
        return;
    }

    while (*input != '\0' && idx + 1 < output_size) {
        char ch = *input++;
        if ((ch >= 'a' && ch <= 'z') ||
            (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') ||
            ch == '-' || ch == '_') {
            output[idx++] = ch;
        } else {
            output[idx++] = '_';
        }
    }

    output[idx] = '\0';
    if (idx == 0) {
        snprintf(output, output_size, "project");
    }
}

static int deploy_download_zip(const DeployTask *task, const char *zip_path, StringBuilder *log) {
    char quoted_url[2048];
    char quoted_zip[PATH_MAX * 2];
    char command[65536];
    int exit_code = -1;

    if (shell_quote(task->download_url, quoted_url, sizeof(quoted_url)) != 0 ||
        shell_quote(zip_path, quoted_zip, sizeof(quoted_zip)) != 0) {
        return -1;
    }

    snprintf(command, sizeof(command),
             "if command -v curl >/dev/null 2>&1; then curl -L --fail -o %s %s; "
             "elif command -v wget >/dev/null 2>&1; then wget -O %s %s; "
             "else echo 'curl or wget is required for deployProject' >&2; exit 127; fi",
             quoted_zip,
             quoted_url,
             quoted_zip,
             quoted_url);

    sb_append(log, "=== download ===\n");
    sb_appendf(log, "url=%s\n", task->download_url);
    if (run_command_capture(command, NULL, DEPLOY_DOWNLOAD_TIMEOUT_SEC, log, &exit_code) != 0 || exit_code != 0) {
        return -1;
    }

    return 0;
}

static int deploy_extract_zip(const char *zip_path, const char *target_path, StringBuilder *log) {
    char quoted_zip[PATH_MAX * 2];
    char quoted_target[PATH_MAX * 2];
    char command[65536];
    int exit_code = -1;

    if (remove_tree(target_path) != 0 && access(target_path, F_OK) == 0) {
        return -1;
    }
    if (ensure_directory(target_path) != 0) {
        return -1;
    }

    if (shell_quote(zip_path, quoted_zip, sizeof(quoted_zip)) != 0 ||
        shell_quote(target_path, quoted_target, sizeof(quoted_target)) != 0) {
        return -1;
    }

    snprintf(command, sizeof(command),
             "if command -v unzip >/dev/null 2>&1; then unzip -oq %s -d %s; "
             "elif command -v busybox >/dev/null 2>&1; then busybox unzip -o %s -d %s; "
             "else echo 'unzip or busybox unzip is required for deployProject' >&2; exit 127; fi",
             quoted_zip,
             quoted_target,
             quoted_zip,
             quoted_target);

    sb_append(log, "=== extract ===\n");
    if (run_command_capture(command, NULL, DEPLOY_COMMAND_TIMEOUT_SEC, log, &exit_code) != 0 || exit_code != 0) {
        return -1;
    }

    return 0;
}

static void *deploy_worker(void *arg) {
    DeployTask *task = (DeployTask *)arg;
    char sanitized_name[128];
    char zip_path[PATH_MAX];
    char target_path[PATH_MAX];
    StringBuilder log;
    int exit_code = -1;

    if (task == NULL) {
        return NULL;
    }

    if (sb_init(&log, 2048) != 0) {
        free(task);
        return NULL;
    }

    deploy_sanitize_name(task->project_name, sanitized_name, sizeof(sanitized_name));
    snprintf(zip_path, sizeof(zip_path), "/tmp/%s-%lld.zip", sanitized_name, app_now_ms());
    snprintf(target_path, sizeof(target_path), "%s/%s", task->deploy_path, task->project_name);

    sb_appendf(&log, "projectName=%s\n", task->project_name);
    sb_appendf(&log, "deployPath=%s\n", task->deploy_path);
    sb_appendf(&log, "targetPath=%s\n", target_path);

    if (ensure_directory(task->deploy_path) != 0) {
        sb_append(&log, "failed to create deploy root directory\n");
        deploy_report_status(task->manager, 0, "failed to create deploy root directory", log.data, task->project_name, target_path);
        goto cleanup;
    }

    if (deploy_download_zip(task, zip_path, &log) != 0) {
        sb_append(&log, "download step failed\n");
        deploy_report_status(task->manager, 0, "download step failed", log.data, task->project_name, target_path);
        goto cleanup;
    }

    if (deploy_extract_zip(zip_path, target_path, &log) != 0) {
        sb_append(&log, "extract step failed\n");
        deploy_report_status(task->manager, 0, "extract step failed", log.data, task->project_name, target_path);
        goto cleanup;
    }

    if (task->deploy_command[0] != '\0') {
        sb_append(&log, "=== deploy command ===\n");
        sb_appendf(&log, "$ %s\n", task->deploy_command);
        if (run_command_capture(task->deploy_command, target_path, DEPLOY_COMMAND_TIMEOUT_SEC, &log, &exit_code) != 0 || exit_code != 0) {
            sb_appendf(&log, "deploy command failed, exitCode=%d\n", exit_code);
            deploy_report_status(task->manager, 0, "deploy command failed", log.data, task->project_name, target_path);
            goto cleanup;
        }
    }

    sb_append(&log, "deploy finished successfully\n");
    deploy_report_status(task->manager, 1, "deploy success", log.data, task->project_name, target_path);

cleanup:
    unlink(zip_path);
    pthread_mutex_lock(&task->manager->lock);
    task->manager->busy = 0;
    pthread_mutex_unlock(&task->manager->lock);
    sb_free(&log);
    free(task);
    return NULL;
}

int deploy_manager_init(DeployManager *manager, struct AppContext *app) {
    if (manager == NULL || app == NULL) {
        return -1;
    }

    memset(manager, 0, sizeof(*manager));
    manager->app = app;
    return pthread_mutex_init(&manager->lock, NULL) == 0 ? 0 : -1;
}

void deploy_manager_cleanup(DeployManager *manager) {
    if (manager == NULL) {
        return;
    }
    pthread_mutex_destroy(&manager->lock);
}

int deploy_manager_handle_deploy(DeployManager *manager, const char *params_json, char *response_json, size_t response_size) {
    DeployTask *task;
    pthread_t worker;

    if (manager == NULL || params_json == NULL) {
        return -1;
    }

    task = (DeployTask *)calloc(1, sizeof(*task));
    if (task == NULL) {
        return deploy_build_result_json(0, "failed to allocate deploy task", "", response_json, response_size);
    }

    task->manager = manager;
    snprintf(task->project_name, sizeof(task->project_name), "project");
    snprintf(task->deploy_path, sizeof(task->deploy_path), "/tmp/deploy");
    (void)json_get_string(params_json, "projectName", task->project_name, sizeof(task->project_name));
    (void)json_get_string(params_json, "downloadUrl", task->download_url, sizeof(task->download_url));
    (void)json_get_string(params_json, "deployPath", task->deploy_path, sizeof(task->deploy_path));
    (void)json_get_string(params_json, "deployCommand", task->deploy_command, sizeof(task->deploy_command));

    if (task->download_url[0] == '\0') {
        deploy_report_status(manager, 0, "downloadUrl is empty", "", task->project_name, task->deploy_path);
        free(task);
        return deploy_build_result_json(0, "downloadUrl is empty", "", response_json, response_size);
    }

    pthread_mutex_lock(&manager->lock);
    if (manager->busy) {
        pthread_mutex_unlock(&manager->lock);
        free(task);
        return deploy_build_result_json(0, "another deploy task is still running", "", response_json, response_size);
    }
    manager->busy = 1;
    pthread_mutex_unlock(&manager->lock);

    if (pthread_create(&worker, NULL, deploy_worker, task) != 0) {
        pthread_mutex_lock(&manager->lock);
        manager->busy = 0;
        pthread_mutex_unlock(&manager->lock);
        free(task);
        return deploy_build_result_json(0, "failed to create deploy worker", "", response_json, response_size);
    }

    pthread_detach(worker);
    return deploy_build_result_json(1, "deploy task received, executing asynchronously", "", response_json, response_size);
}

