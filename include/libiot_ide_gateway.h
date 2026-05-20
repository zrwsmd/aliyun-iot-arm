#ifndef LIBIOT_IDE_GATEWAY_H
#define LIBIOT_IDE_GATEWAY_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IOT_IDE_GATEWAY_API_VERSION 1

#define IOT_IDE_GATEWAY_OK 0
#define IOT_IDE_GATEWAY_ERR_INVALID_ARGUMENT -1
#define IOT_IDE_GATEWAY_ERR_NO_MEMORY -2
#define IOT_IDE_GATEWAY_ERR_INTERNAL -3
#define IOT_IDE_GATEWAY_ERR_NOT_STARTED -4

typedef struct IotIdeGateway IotIdeGateway;

typedef enum IotIdeGatewayLogLevel {
    IOT_IDE_GATEWAY_LOG_DEBUG = 0,
    IOT_IDE_GATEWAY_LOG_INFO = 1,
    IOT_IDE_GATEWAY_LOG_WARN = 2,
    IOT_IDE_GATEWAY_LOG_ERROR = 3
} IotIdeGatewayLogLevel;

typedef struct IotIdeGatewayCallbacks {
    void (*on_service)(void *user_data,
                       IotIdeGateway *gateway,
                       const char *service_name,
                       const char *request_id,
                       const char *params_json);
    void (*on_log)(void *user_data, int level, const char *message);
} IotIdeGatewayCallbacks;

typedef struct IotIdeGatewayOptions {
    const char *config_path;
    const IotIdeGatewayCallbacks *callbacks;
    void *user_data;
} IotIdeGatewayOptions;

int iot_ide_gateway_get_api_version(void);

int iot_ide_gateway_create(const IotIdeGatewayOptions *options, IotIdeGateway **gateway);
void iot_ide_gateway_destroy(IotIdeGateway *gateway);

int iot_ide_gateway_start(IotIdeGateway *gateway);
void iot_ide_gateway_stop(IotIdeGateway *gateway);

int iot_ide_gateway_post_properties(IotIdeGateway *gateway, const char *params_json);
int iot_ide_gateway_reply_service(IotIdeGateway *gateway,
                                  const char *service_name,
                                  const char *request_id,
                                  int code,
                                  const char *data_json);
int iot_ide_gateway_publish_trace(IotIdeGateway *gateway, const char *payload_json);
int iot_ide_gateway_forward_event(IotIdeGateway *gateway, const char *event_name, const char *event_json);

int iot_ide_gateway_get_device_info(IotIdeGateway *gateway,
                                    char *product_key,
                                    size_t product_key_size,
                                    char *device_name,
                                    size_t device_name_size,
                                    char *mqtt_host,
                                    size_t mqtt_host_size);

#ifdef __cplusplus
}
#endif

#endif
