#ifndef LIBIOT_IDE_H
#define LIBIOT_IDE_H

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

/*
 * libiot_ide.so 内部运行对象句柄。
 *
 * 调用方向：
 * iec_runtime 调用 libiot_ide_create() 后拿到这个句柄，后续所有
 * IDE 业务接口都要把该句柄传回 libiot_ide.so。
 *
 * 使用方不需要知道结构体内部字段，也不能直接访问内部字段。
 */
typedef struct IotIdeRuntime IotIdeRuntime;

/*
 * libiot_ide.so -> iec_runtime 的日志级别。
 *
 * 这些级别会通过 IotIdeRuntimeCallbacks.on_log 回调给 iec_runtime，
 * 由 iec_runtime 决定打印到控制台、文件或自己的日志系统。
 */
typedef enum IotIdeRuntimeLogLevel {
    IOT_IDE_LOG_DEBUG = 0,
    IOT_IDE_LOG_INFO = 1,
    IOT_IDE_LOG_WARN = 2,
    IOT_IDE_LOG_ERROR = 3
} IotIdeRuntimeLogLevel;

/*
 * libiot_ide.so 回调 iec_runtime 的函数集合。
 *
 * 注册方向：
 * iec_runtime 在调用 libiot_ide_create() 前填写这个结构体，
 * 然后通过 IotIdeRuntimeOptions.callbacks 交给 libiot_ide.so。
 *
 * 触发方向：
 * libiot_ide.so -> 回调 iec_runtime。
 */
typedef struct IotIdeRuntimeCallbacks {
    /*
     * 业务事件回调。
     *
     * 谁回调谁：
     * libiot_ide.so 回调 iec_runtime。
     *
     * 什么时候回调：
     * IDE 连接、断开、心跳、部署、启动等业务处理后，libiot_ide.so
     * 会产生事件并调用该函数。
     *
     * event_name 常见取值：
     * - property.post: 需要上报物模型属性，例如 IDE 连接状态、心跳时间
     * - requestConnect.response: IDE 连接处理结果
     * - requestDisconnect.response: IDE 断开处理结果
     * - ideHeartbeat.response: IDE 心跳处理结果
     * - deployProject.response: 部署请求接收或处理结果
     * - startProject.response: 启动处理结果
     *
     * event_json 是事件内容，格式为 JSON 字符串。
     */
    void (*on_event)(void *user_data, const char *event_name, const char *event_json);

    /*
     * 日志回调。
     *
     * 谁回调谁：
     * libiot_ide.so 回调 iec_runtime。
     *
     * 用途：
     * 把 libiot_ide.so 内部日志交给 iec_runtime，方便统一接入现场日志系统。
     */
    void (*on_log)(void *user_data, int level, const char *message);
} IotIdeRuntimeCallbacks;

/*
 * 创建 libiot_ide.so 运行对象时传入的配置。
 *
 * 调用方向：
 * iec_runtime -> libiot_ide.so。
 */
typedef struct IotIdeRuntimeOptions {
    /*
     * 工作目录。
     *
     * 部署、启动等业务如果需要相对路径，会以这个目录作为参考。
     * 生产环境建议传入明确目录，例如 /usr/iec-runtime 或项目实际工作目录。
     */
    const char *work_dir;

    /*
     * 回调函数集合。
     *
     * libiot_ide.so 后续通过这里面的 on_event/on_log 回调 iec_runtime。
     * 可以为 NULL，为 NULL 时只是不接收事件/日志回调。
     */
    const IotIdeRuntimeCallbacks *callbacks;

    /*
     * 用户上下文指针。
     *
     * libiot_ide.so 不解析该指针，只会在 on_event/on_log 回调时原样传回。
     * iec_runtime 通常传入自己的运行时结构体指针，方便回调里继续访问 gateway。
     */
    void *user_data;
} IotIdeRuntimeOptions;

/*
 * 查询当前 C API 版本。
 *
 * 调用方向：
 * iec_runtime -> libiot_ide.so。
 *
 * 用途：
 * 集成方可用于判断头文件和动态库是否匹配。
 */
int libiot_ide_get_api_version(void);

/*
 * 创建 IDE 业务运行对象。
 *
 * 调用方向：
 * iec_runtime -> libiot_ide.so。
 *
 * 作用：
 * 初始化 IDE 连接管理、部署管理、启动管理等内部模块，并保存回调配置。
 *
 * 成功后：
 * *runtime 会得到一个 IotIdeRuntime 句柄，后续业务接口都要传入该句柄。
 */
int libiot_ide_create(const IotIdeRuntimeOptions *options, IotIdeRuntime **runtime);

/*
 * 销毁 IDE 业务运行对象。
 *
 * 调用方向：
 * iec_runtime -> libiot_ide.so。
 *
 * 作用：
 * 释放 libiot_ide_create() 创建的对象和内部资源。
 */
void libiot_ide_destroy(IotIdeRuntime *runtime);

/*
 * 处理 IDE 连接请求。
 *
 * 调用方向：
 * iec_runtime -> libiot_ide.so。
 *
 * 典型流转：
 * 阿里云 requestConnect -> gateway -> iec_runtime -> 本函数。
 *
 * 作用：
 * 建立 IDE 连接锁。当前只允许一个 IDE 连接，如果已有 IDE 占用，
 * 会返回拒绝信息。
 *
 * params_json 常见字段：
 * - clientId: IDE 客户端 ID
 * - clientInfo: IDE 信息 JSON 字符串
 *
 * response_json 返回处理结果 JSON。
 */
int libiot_ide_request_connect(IotIdeRuntime *runtime, const char *params_json, char *response_json, size_t response_size);

/*
 * 处理 IDE 断开请求。
 *
 * 调用方向：
 * iec_runtime -> libiot_ide.so。
 *
 * 典型流转：
 * 阿里云 requestDisconnect -> gateway -> iec_runtime -> 本函数。
 *
 * 作用：
 * 校验 clientId，如果是当前 active IDE，则释放 IDE 连接锁并上报连接状态。
 */
int libiot_ide_request_disconnect(IotIdeRuntime *runtime, const char *params_json, char *response_json, size_t response_size);

/*
 * 处理 IDE 业务心跳。
 *
 * 调用方向：
 * iec_runtime -> libiot_ide.so。
 *
 * 典型流转：
 * 阿里云 ideHeartbeat -> gateway -> iec_runtime -> 本函数。
 *
 * 作用：
 * 校验 clientId 是否为当前 active IDE，校验通过后更新 IDEHeartbeat 属性。
 *
 * 注意：
 * 这是 IDE 业务心跳，不是 MQTT 协议层 heartbeat response。
 */
int libiot_ide_heartbeat(IotIdeRuntime *runtime, const char *params_json, char *response_json, size_t response_size);

/*
 * 处理项目部署请求。
 *
 * 调用方向：
 * iec_runtime -> libiot_ide.so。
 *
 * 典型流转：
 * 阿里云 deployProject -> gateway -> iec_runtime -> 本函数。
 *
 * 作用：
 * 校验当前 IDE 连接权限后，按 params_json 中的部署参数执行部署逻辑。
 *
 * 当前实现主要解析：
 * - clientId: IDE 客户端 ID
 * - projectName: 项目名称
 * - downloadUrl: 项目 zip 下载地址
 * - deployPath: 部署根目录
 * - deployCommand: 解压后执行的部署命令，可为空
 *
 * 部署状态会通过 on_event 产生 property.post/deployProject.response 事件。
 */
int libiot_ide_deploy_project(IotIdeRuntime *runtime, const char *params_json, char *response_json, size_t response_size);

/*
 * 处理项目启动请求。
 *
 * 调用方向：
 * iec_runtime -> libiot_ide.so。
 *
 * 典型流转：
 * 阿里云 startProject -> gateway -> iec_runtime -> 本函数。
 *
 * 作用：
 * 校验当前 IDE 连接权限后，进入部署目录并执行启动命令。
 *
 * 当前实现主要解析：
 * - clientId: IDE 客户端 ID
 * - projectName: 项目名称
 * - deployPath: 部署根目录
 * - startCommand: 启动命令
 *
 * 启动结果会通过 response_json 和 on_event 返回。
 */
int libiot_ide_start_project(IotIdeRuntime *runtime, const char *params_json, char *response_json, size_t response_size);

/*
 * 查询当前 IDE 连接快照。
 *
 * 调用方向：
 * iec_runtime -> libiot_ide.so。
 *
 * 作用：
 * 返回当前是否有 IDE 连接、当前 clientId、最近心跳时间等状态。
 *
 * 常用于阿里云 property/get 或本地调试。
 */
int libiot_ide_get_connection_snapshot(IotIdeRuntime *runtime, char *snapshot_json, size_t snapshot_size);

#ifdef __cplusplus
}
#endif

#endif
