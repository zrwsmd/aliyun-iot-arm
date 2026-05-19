#include "iot_ide_gateway_api.h"
#include "iot_ide_runtime_api.h"

#include <stdarg.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct IecRuntime {
    volatile sig_atomic_t running;
    IotIdeGateway *gateway;
    IotIdeRuntime *iot_ide;
} IecRuntime;

static IecRuntime *g_runtime = NULL;

static void runtime_format_time(char *buffer, size_t buffer_size) {
    time_t now;
    struct tm tm_value;

    if (buffer == NULL || buffer_size == 0) {
        return;
    }

    now = time(NULL);
    if (localtime_r(&now, &tm_value) == NULL) {
        snprintf(buffer, buffer_size, "unknown-time");
        return;
    }

    strftime(buffer, buffer_size, "%Y-%m-%d %H:%M:%S", &tm_value);
}

static void runtime_log_stdout(const char *fmt, ...) {
    char time_text[32];
    va_list args;

    runtime_format_time(time_text, sizeof(time_text));
    printf("[%s] ", time_text);

    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
}

static void runtime_log_stderr(const char *fmt, ...) {
    char time_text[32];
    va_list args;

    runtime_format_time(time_text, sizeof(time_text));
    fprintf(stderr, "[%s] ", time_text);

    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
}

/*
 * 生产集成入口。
 *
 * 流转方向：
 * 本地 IDE -> 阿里云 -> libiot_ide_gateway.so -> iec_runtime.c
 *          -> libiot_ide.so -> iec_runtime.c -> libiot_ide_gateway.so
 *          -> 阿里云 -> 本地 IDE
 *
 * libiot_ide_gateway.so 负责阿里云 MQTT 连接、服务订阅、JSON 解析、
 * 属性上报、服务回复和 trace 发布。
 *
 * libiot_ide.so 负责 IDE 业务逻辑：连接锁、心跳、断开、部署、启动、
 * 连接快照。
 *
 * iec_runtime.c 只做胶水层：两个动态库不直接互相调用，所有业务流转都经过
 * iec_runtime.c 中转。
 *
 * 下行服务请求方向：
 * 阿里云 -> libiot_ide_gateway.so -> 回调 iec_runtime.c 的 runtime_on_service()
 *        -> iec_runtime.c 调用 libiot_ide.so 的业务 API。
 *
 * 上行业务事件方向：
 * libiot_ide.so -> 回调 iec_runtime.c 的 runtime_on_iot_ide_event()
 *               -> iec_runtime.c 调用 libiot_ide_gateway.so 上报阿里云。
 */

/*
 * 系统信号 -> iec_runtime.c
 *
 * 当用户按 Ctrl+C 或系统发送 SIGTERM 时，只修改 running 标志。
 * main() 的主循环检测到 running=0 后，会按顺序停止 gateway、销毁
 * libiot_ide.so runtime，并退出进程。
 */
static void runtime_handle_signal(int signo) {
    (void)signo;
    if (g_runtime != NULL) {
        g_runtime->running = 0;
    }
}

/*
 * iec_runtime.c -> libiot_ide_gateway.so -> 阿里云
 *
 * 当 gateway 收到未知服务，或者 iec_runtime.c 无法处理某个服务时，
 * 用这个函数组装一个失败响应，并通过 gateway 回复到阿里云的
 * {service}_reply topic。
 */
static void runtime_reply_error(IotIdeGateway *gateway,
                                const char *service_name,
                                const char *request_id,
                                const char *message) {
    char response[512];

    snprintf(response, sizeof(response),
             "{\"success\":0,\"message\":\"%s\"}",
             message == NULL ? "error" : message);
    (void)iot_ide_gateway_reply_service(gateway, service_name, request_id, 200, response);
}

/*
 * libiot_ide.so -> 回调 iec_runtime.c -> libiot_ide_gateway.so -> 阿里云
 *
 * 谁回调谁：
 * libiot_ide.so 回调 iec_runtime.c 里的 runtime_on_iot_ide_event()。
 *
 * 什么时候回调：
 * libiot_ide.so 内部状态变化时会调用这里，例如 IDE 连接成功、心跳更新、
 * 断开连接、部署状态变化。
 *
 * event_name 表示事件类型：
 * - property.post: 需要上报物模型属性到阿里云
 * - trace.publish: 需要发布 trace 数据到阿里云
 * - service.reply: 预留的集中服务回复路径
 *
 * 后续怎么走：
 * iec_runtime.c 不直接发 MQTT，而是把事件转给 gateway，由
 * libiot_ide_gateway.so 负责真正发布到阿里云。
 */
static void runtime_on_iot_ide_event(void *user_data, const char *event_name, const char *event_json) {
    IecRuntime *runtime = (IecRuntime *)user_data;

    runtime_log_stdout("[iot_ide event] %s %s\n",
                       event_name == NULL ? "" : event_name,
                       event_json == NULL ? "" : event_json);

    if (runtime == NULL || runtime->gateway == NULL || event_name == NULL) {
        return;
    }

    /*
     * 这里是 iec_runtime.c 主动调用 libiot_ide_gateway.so。
     *
     * 注意：不是 libiot_ide.so 直接调用 libiot_ide_gateway.so。
     * libiot_ide.so 只负责把事件回调给 iec_runtime.c，真正上报阿里云
     * 是由 iec_runtime.c 调用 gateway 完成的。
     *
     * property.post  -> gateway 上报物模型属性到阿里云。
     * trace.publish  -> gateway 发布 trace 数据到阿里云。
     * service.reply  -> 预留的集中服务回复路径。
     */
    (void)iot_ide_gateway_forward_event(runtime->gateway, event_name, event_json);
}

/*
 * libiot_ide.so -> iec_runtime.c -> 本地日志
 *
 * 这是业务动态库的日志回调。只负责把 libiot_ide.so 的内部日志打印出来，
 * 不参与阿里云消息流转。
 */
static void runtime_on_iot_ide_log(void *user_data, int level, const char *message) {
    (void)user_data;
    runtime_log_stdout("[iot_ide log:%d] %s\n", level, message == NULL ? "" : message);
}

/*
 * libiot_ide_gateway.so -> iec_runtime.c -> 本地日志
 *
 * 这是 gateway 动态库的日志回调。gateway 连接阿里云、订阅 topic、
 * 收到服务下发、MQTT 心跳等日志会从这里打印出来。
 */
static void runtime_on_gateway_log(void *user_data, int level, const char *message) {
    (void)user_data;
    runtime_log_stdout("[gateway log:%d] %s\n", level, message == NULL ? "" : message);
}

/*
 * 阿里云 -> libiot_ide_gateway.so -> 回调 iec_runtime.c -> libiot_ide.so
 * libiot_ide.so -> 回调 iec_runtime.c -> libiot_ide_gateway.so -> 阿里云
 *
 * 这是最关键的服务分发回调。
 *
 * 谁回调谁：
 * libiot_ide_gateway.so 回调 iec_runtime.c 里的 runtime_on_service()。
 *
 * 什么时候回调：
 * 阿里云下发 requestConnect、ideHeartbeat、deployProject、startProject
 * 等服务时，gateway 收到 MQTT 消息并解析完成后，会调用这里。
 *
 * libiot_ide_gateway.so 已经完成了：
 * - 接收阿里云 MQTT 服务下发
 * - 解析 topic 和 payload
 * - 过滤 _reply 消息
 * - 提取 service_name、request_id、params_json
 *
 * iec_runtime.c 在这里根据 service_name 调用 libiot_ide.so 对应的业务 API。
 * libiot_ide.so 填充 response 后，iec_runtime.c 再调用 gateway 把响应回复给阿里云。
 */
static void runtime_on_service(void *user_data,
                               IotIdeGateway *gateway,
                               const char *service_name,
                               const char *request_id,
                               const char *params_json) {
    IecRuntime *runtime = (IecRuntime *)user_data;
    char response[8192] = "{\"success\":0,\"message\":\"not handled\"}";
    int rc = IOT_IDE_RUNTIME_ERR_INVALID_ARGUMENT;

    if (runtime == NULL || runtime->iot_ide == NULL || gateway == NULL || service_name == NULL) {
        return;
    }

    /*
     * 服务名 -> 业务动态库 API 的映射关系：
     *
     * requestConnect    -> IDE 请求连接
     * requestDisconnect -> IDE 请求断开
     * ideHeartbeat      -> IDE 业务心跳
     * deployProject     -> 部署项目
     * startProject      -> 启动项目
     * property/get      -> 查询当前 IDE 连接快照
     * property/set      -> 当前示例直接转成属性上报
     */
    if (strcmp(service_name, "requestConnect") == 0) {
        rc = iot_ide_runtime_request_connect(runtime->iot_ide, params_json, response, sizeof(response));
    } else if (strcmp(service_name, "requestDisconnect") == 0) {
        rc = iot_ide_runtime_request_disconnect(runtime->iot_ide, params_json, response, sizeof(response));
    } else if (strcmp(service_name, "ideHeartbeat") == 0) {
        rc = iot_ide_runtime_heartbeat(runtime->iot_ide, params_json, response, sizeof(response));
    } else if (strcmp(service_name, "deployProject") == 0) {
        rc = iot_ide_runtime_deploy_project(runtime->iot_ide, params_json, response, sizeof(response));
    } else if (strcmp(service_name, "startProject") == 0) {
        rc = iot_ide_runtime_start_project(runtime->iot_ide, params_json, response, sizeof(response));
    } else if (strcmp(service_name, "property/get") == 0) {
        rc = iot_ide_runtime_get_connection_snapshot(runtime->iot_ide, response, sizeof(response));
    } else if (strcmp(service_name, "property/set") == 0) {
        (void)iot_ide_gateway_post_properties(gateway, params_json == NULL ? "{}" : params_json);
        snprintf(response, sizeof(response), "{\"success\":1,\"message\":\"property set accepted\"}");
        rc = IOT_IDE_RUNTIME_OK;
    } else {
        runtime_reply_error(gateway, service_name, request_id, "unknown service");
        return;
    }

    runtime_log_stdout("service response service=%s rc=%d data=%s\n", service_name, rc, response);
    (void)iot_ide_gateway_reply_service(gateway, service_name, request_id, 200, response);
}

/*
 * iec_runtime 进程启动入口。
 *
 * 初始化方向：
 * 1. 创建 libiot_ide_gateway.so 对象，注册 on_service 回调。
 * 2. 创建 libiot_ide.so 对象，注册 on_event/on_log 回调。
 * 3. 启动 gateway，让 gateway 连接阿里云并订阅服务下发。
 * 4. main() 保持运行，后续消息都通过回调流转。
 *
 * 退出方向：
 * 系统信号 -> runtime_handle_signal -> main 主循环退出
 * -> 停止 gateway -> 销毁 libiot_ide.so -> 销毁 gateway。
 */
int main(int argc, char **argv) {
    const char *config_path = argc > 1 ? argv[1] : "./device_id.json";
    IecRuntime runtime;
    IotIdeGatewayCallbacks gateway_callbacks;
    IotIdeGatewayOptions gateway_options;
    IotIdeRuntimeCallbacks iot_ide_callbacks;
    IotIdeRuntimeOptions iot_ide_options;
    char product_key[128] = "";
    char device_name[128] = "";
    char mqtt_host[256] = "";

    memset(&runtime, 0, sizeof(runtime));
    memset(&gateway_callbacks, 0, sizeof(gateway_callbacks));
    memset(&gateway_options, 0, sizeof(gateway_options));
    memset(&iot_ide_callbacks, 0, sizeof(iot_ide_callbacks));
    memset(&iot_ide_options, 0, sizeof(iot_ide_options));

    g_runtime = &runtime;
    runtime.running = 1;
    signal(SIGINT, runtime_handle_signal);
    signal(SIGTERM, runtime_handle_signal);

    /*
     * 注册 gateway 服务回调：
     * iec_runtime.c 把 runtime_on_service() 的函数地址交给 libiot_ide_gateway.so。
     *
     * 创建 gateway 前先注册服务回调。阿里云下发 requestConnect、
     * deployProject、startProject 等服务时，gateway 会回调 runtime_on_service。
     *
     * 这一行不是立即调用 runtime_on_service()，只是告诉 gateway：
     * 以后你收到阿里云下发的服务请求后，就回调这个函数。
     *
     * 真正触发时的方向：
     * 阿里云 -> libiot_ide_gateway.so -> runtime_on_service() -> libiot_ide.so
     */
    gateway_callbacks.on_service = runtime_on_service;
    gateway_callbacks.on_log = runtime_on_gateway_log;
    gateway_options.config_path = config_path;
    gateway_options.callbacks = &gateway_callbacks;
    gateway_options.user_data = &runtime;

    if (iot_ide_gateway_create(&gateway_options, &runtime.gateway) != IOT_IDE_GATEWAY_OK) {
        runtime_log_stderr("iot_ide_gateway_create failed\n");
        return 1;
    }

    /*
     * 注册业务事件回调：
     * iec_runtime.c 把 runtime_on_iot_ide_event() 的函数地址交给 libiot_ide.so。
     *
     * 这一行不是立即调用 runtime_on_iot_ide_event()，只是告诉业务动态库：
     * 以后你产生 property.post、deployProject.response、startProject.response
     * 等事件后，就回调这个函数。
     *
     * 真正触发时的方向：
     * libiot_ide.so -> runtime_on_iot_ide_event() -> libiot_ide_gateway.so -> 阿里云
     */
    iot_ide_callbacks.on_event = runtime_on_iot_ide_event;
    iot_ide_callbacks.on_log = runtime_on_iot_ide_log;
    iot_ide_options.work_dir = ".";
    iot_ide_options.callbacks = &iot_ide_callbacks;
    iot_ide_options.user_data = &runtime;

    if (iot_ide_runtime_create(&iot_ide_options, &runtime.iot_ide) != IOT_IDE_RUNTIME_OK) {
        runtime_log_stderr("iot_ide_runtime_create failed\n");
        iot_ide_gateway_destroy(runtime.gateway);
        return 1;
    }

    (void)iot_ide_gateway_get_device_info(runtime.gateway,
                                          product_key,
                                          sizeof(product_key),
                                          device_name,
                                          sizeof(device_name),
                                          mqtt_host,
                                          sizeof(mqtt_host));

    runtime_log_stdout("=== iec_runtime ===\n");
    runtime_log_stdout("config: %s\n", config_path);
    runtime_log_stdout("productKey: %s\n", product_key);
    runtime_log_stdout("deviceName: %s\n", device_name);
    runtime_log_stdout("mqttHost: %s\n", mqtt_host);

    /*
     * iec_runtime.c -> libiot_ide_gateway.so -> 阿里云
     *
     * 启动 gateway 后，gateway 内部会连接阿里云 MQTT，并订阅：
     * /sys/{productKey}/{deviceName}/thing/service/#
     */
    if (iot_ide_gateway_start(runtime.gateway) != IOT_IDE_GATEWAY_OK) {
        runtime_log_stderr("iot_ide_gateway_start failed\n");
        iot_ide_runtime_destroy(runtime.iot_ide);
        iot_ide_gateway_destroy(runtime.gateway);
        return 1;
    }

    while (runtime.running) {
        sleep(1);
    }

    /*
     * iec_runtime.c -> libiot_ide_gateway.so / libiot_ide.so
     *
     * 退出时先停止阿里云连接，再销毁业务动态库和 gateway 动态库对象。
     */
    iot_ide_gateway_stop(runtime.gateway);
    iot_ide_runtime_destroy(runtime.iot_ide);
    iot_ide_gateway_destroy(runtime.gateway);
    runtime_log_stdout("iec_runtime exit\n");
    return 0;
}
