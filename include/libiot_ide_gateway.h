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

/*
 * libiot_ide_gateway.so 内部网关对象句柄。
 *
 * 调用方向：
 * iec_runtime 调用 iot_ide_gateway_create() 后拿到这个句柄，后续连接阿里云、
 * 回复服务、上报属性等 gateway 接口都要把该句柄传回 libiot_ide_gateway.so。
 *
 * 使用方不需要知道结构体内部字段，也不能直接访问内部字段。
 */
typedef struct IotIdeGateway IotIdeGateway;

/*
 * libiot_ide_gateway.so -> iec_runtime 的日志级别。
 *
 * 这些级别会通过 IotIdeGatewayCallbacks.on_log 回调给 iec_runtime，
 * 由 iec_runtime 决定打印到控制台、文件或自己的日志系统。
 */
typedef enum IotIdeGatewayLogLevel {
    IOT_IDE_GATEWAY_LOG_DEBUG = 0,
    IOT_IDE_GATEWAY_LOG_INFO = 1,
    IOT_IDE_GATEWAY_LOG_WARN = 2,
    IOT_IDE_GATEWAY_LOG_ERROR = 3
} IotIdeGatewayLogLevel;

/*
 * libiot_ide_gateway.so 回调 iec_runtime 的函数集合。
 *
 * 注册方向：
 * iec_runtime 在调用 iot_ide_gateway_create() 前填写这个结构体，
 * 然后通过 IotIdeGatewayOptions.callbacks 交给 libiot_ide_gateway.so。
 *
 * 触发方向：
 * libiot_ide_gateway.so -> 回调 iec_runtime。
 */
typedef struct IotIdeGatewayCallbacks {
    /*
     * 阿里云服务下发回调。
     *
     * 谁回调谁：
     * libiot_ide_gateway.so 回调 iec_runtime。
     *
     * 什么时候回调：
     * gateway 已连接阿里云并订阅服务 topic 后，如果阿里云下发
     * requestConnect、ideHeartbeat、deployProject、startProject 等服务，
     * gateway 会解析 MQTT topic/payload，并调用该函数。
     *
     * service_name:
     * 服务名，例如 requestConnect、requestDisconnect、ideHeartbeat、
     * deployProject、startProject、property/get、property/set。
     *
     * request_id:
     * 阿里云本次服务调用的 id。iec_runtime 回复服务时要原样传给
     * iot_ide_gateway_reply_service()。
     *
     * params_json:
     * 阿里云服务 payload 里的 params 字段，格式为 JSON 字符串。
     *
     * gateway:
     * 当前 gateway 句柄。iec_runtime 可在回调内直接用它回复服务或上报属性。
     */
    void (*on_service)(void *user_data,
                       IotIdeGateway *gateway,
                       const char *service_name,
                       const char *request_id,
                       const char *params_json);

    /*
     * 日志回调。
     *
     * 谁回调谁：
     * libiot_ide_gateway.so 回调 iec_runtime。
     *
     * 用途：
     * 把 gateway 内部日志交给 iec_runtime，例如阿里云 MQTT 连接状态、
     * 订阅结果、收到服务、MQTT 心跳等。
     */
    void (*on_log)(void *user_data, int level, const char *message);
} IotIdeGatewayCallbacks;

/*
 * 创建 libiot_ide_gateway.so 网关对象时传入的配置。
 *
 * 调用方向：
 * iec_runtime -> libiot_ide_gateway.so。
 */
typedef struct IotIdeGatewayOptions {
    /*
     * 阿里云设备身份配置文件路径。
     *
     * 当前实现会从该 JSON 文件读取 productKey、deviceName、deviceSecret、
     * instanceId 等信息，用于连接阿里云 IoT 平台。
     */
    const char *config_path;

    /*
     * 回调函数集合。
     *
     * libiot_ide_gateway.so 后续通过这里面的 on_service/on_log 回调 iec_runtime。
     */
    const IotIdeGatewayCallbacks *callbacks;

    /*
     * 用户上下文指针。
     *
     * libiot_ide_gateway.so 不解析该指针，只会在 on_service/on_log 回调时原样传回。
     * iec_runtime 通常传入自己的运行时结构体指针，方便回调里继续访问 libiot_ide.so。
     */
    void *user_data;
} IotIdeGatewayOptions;

/*
 * 查询当前 C API 版本。
 *
 * 调用方向：
 * iec_runtime -> libiot_ide_gateway.so。
 *
 * 用途：
 * 集成方可用于判断头文件和动态库是否匹配。
 */
int iot_ide_gateway_get_api_version(void);

/*
 * 创建阿里云 gateway 对象。
 *
 * 调用方向：
 * iec_runtime -> libiot_ide_gateway.so。
 *
 * 作用：
 * 读取 device_id.json，初始化阿里云 MQTT 客户端配置，并保存回调函数。
 *
 * 注意：
 * 该函数只创建对象，不会真正连接阿里云。真正连接发生在
 * iot_ide_gateway_start()。
 */
int iot_ide_gateway_create(const IotIdeGatewayOptions *options, IotIdeGateway **gateway);

/*
 * 销毁阿里云 gateway 对象。
 *
 * 调用方向：
 * iec_runtime -> libiot_ide_gateway.so。
 *
 * 作用：
 * 停止阿里云连接并释放 iot_ide_gateway_create() 创建的内部资源。
 */
void iot_ide_gateway_destroy(IotIdeGateway *gateway);

/*
 * 启动 gateway。
 *
 * 调用方向：
 * iec_runtime -> libiot_ide_gateway.so -> 阿里云。
 *
 * 作用：
 * 连接阿里云 MQTT，并订阅服务下发 topic：
 * /sys/{productKey}/{deviceName}/thing/service/#
 *
 * 启动成功后：
 * 阿里云下发服务时，会触发 IotIdeGatewayCallbacks.on_service 回调。
 */
int iot_ide_gateway_start(IotIdeGateway *gateway);

/*
 * 停止 gateway。
 *
 * 调用方向：
 * iec_runtime -> libiot_ide_gateway.so -> 阿里云。
 *
 * 作用：
 * 停止 MQTT 收发线程，并断开与阿里云的连接。
 */
void iot_ide_gateway_stop(IotIdeGateway *gateway);

/*
 * 上报物模型属性。
 *
 * 调用方向：
 * iec_runtime -> libiot_ide_gateway.so -> 阿里云。
 *
 * 典型来源：
 * libiot_ide.so 通过 on_event 回调给 iec_runtime 一个 property.post 事件，
 * iec_runtime 再调用本函数上报到阿里云。
 *
 * params_json:
 * 物模型属性 JSON，例如：
 * {"hasIDEConnected":1,"IDEHeartbeat":"..."}
 */
int iot_ide_gateway_post_properties(IotIdeGateway *gateway, const char *params_json);

/*
 * 回复阿里云服务调用。
 *
 * 调用方向：
 * iec_runtime -> libiot_ide_gateway.so -> 阿里云。
 *
 * 典型流转：
 * 阿里云下发服务 -> gateway 回调 on_service -> iec_runtime 调用 libiot_ide.so
 * 处理业务 -> iec_runtime 调用本函数回复 {service}_reply。
 *
 * service_name:
 * 当前服务名，例如 requestConnect、ideHeartbeat、deployProject。
 *
 * request_id:
 * on_service 回调中收到的 request_id，回复时必须保持一致。
 *
 * code:
 * 阿里云服务回复 code，通常业务处理成功/失败都用 200，具体成功失败放在 data_json。
 *
 * data_json:
 * 回复给阿里云的 data 字段 JSON，例如 {"success":1,"message":"connect accepted"}。
 */
int iot_ide_gateway_reply_service(IotIdeGateway *gateway,
                                  const char *service_name,
                                  const char *request_id,
                                  int code,
                                  const char *data_json);

/*
 * 发布 trace 数据。
 *
 * 调用方向：
 * iec_runtime -> libiot_ide_gateway.so -> 阿里云。
 *
 * 用途：
 * 如果业务层需要把调试轨迹、运行日志或诊断信息通过阿里云 topic 上报，
 * 可以调用该函数。
 */
int iot_ide_gateway_publish_trace(IotIdeGateway *gateway, const char *payload_json);

/*
 * 转发 libiot_ide.so 产生的事件。
 *
 * 调用方向：
 * libiot_ide.so -> 回调 iec_runtime -> iec_runtime 调用本函数
 * -> libiot_ide_gateway.so -> 阿里云。
 *
 * 作用：
 * iec_runtime 不需要自己判断所有 event_name 的发布 topic，可以把业务库事件
 * 统一交给 gateway。gateway 会根据 event_name 做分发：
 * - property.post: 调用 iot_ide_gateway_post_properties()
 * - trace.publish: 调用 iot_ide_gateway_publish_trace()
 * - service.reply: 调用 iot_ide_gateway_reply_service()
 */
int iot_ide_gateway_forward_event(IotIdeGateway *gateway, const char *event_name, const char *event_json);

/*
 * 获取当前设备连接信息。
 *
 * 调用方向：
 * iec_runtime -> libiot_ide_gateway.so。
 *
 * 作用：
 * 从 gateway 内部读取 productKey、deviceName、mqttHost，主要用于启动日志、
 * 运行状态展示或现场排查。
 */
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
