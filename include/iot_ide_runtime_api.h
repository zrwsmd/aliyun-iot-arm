#ifndef IOT_IDE_RUNTIME_API_H
#define IOT_IDE_RUNTIME_API_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IOT_IDE_RUNTIME_API_VERSION 1

#define IOT_IDE_RUNTIME_OK 0
#define IOT_IDE_RUNTIME_ERR_INVALID_ARGUMENT -1
#define IOT_IDE_RUNTIME_ERR_NO_MEMORY -2
#define IOT_IDE_RUNTIME_ERR_INTERNAL -3
#define IOT_IDE_RUNTIME_ERR_NOT_ACTIVE_CLIENT -4

typedef struct IotIdeRuntime IotIdeRuntime;

typedef enum IotIdeRuntimeLogLevel {
    IOT_IDE_LOG_DEBUG = 0,
    IOT_IDE_LOG_INFO = 1,
    IOT_IDE_LOG_WARN = 2,
    IOT_IDE_LOG_ERROR = 3
} IotIdeRuntimeLogLevel;

typedef struct IotIdeRuntimeCallbacks {
    void (*on_event)(void *user_data, const char *event_name, const char *event_json);
    void (*on_log)(void *user_data, int level, const char *message);
} IotIdeRuntimeCallbacks;

typedef struct IotIdeRuntimeOptions {
    const char *work_dir;
    const IotIdeRuntimeCallbacks *callbacks;
    void *user_data;
} IotIdeRuntimeOptions;

int iot_ide_runtime_get_api_version(void);

int iot_ide_runtime_create(const IotIdeRuntimeOptions *options, IotIdeRuntime **runtime);
void iot_ide_runtime_destroy(IotIdeRuntime *runtime);

int iot_ide_runtime_request_connect(IotIdeRuntime *runtime, const char *params_json, char *response_json, size_t response_size);
int iot_ide_runtime_request_disconnect(IotIdeRuntime *runtime, const char *params_json, char *response_json, size_t response_size);
int iot_ide_runtime_heartbeat(IotIdeRuntime *runtime, const char *params_json, char *response_json, size_t response_size);

int iot_ide_runtime_deploy_project(IotIdeRuntime *runtime, const char *params_json, char *response_json, size_t response_size);
int iot_ide_runtime_start_project(IotIdeRuntime *runtime, const char *params_json, char *response_json, size_t response_size);

int iot_ide_runtime_get_connection_snapshot(IotIdeRuntime *runtime, char *snapshot_json, size_t snapshot_size);

#ifdef __cplusplus
}
#endif

#endif
