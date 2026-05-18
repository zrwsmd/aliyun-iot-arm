# iot-ide 动态库封装需求文档

本文档用于描述当前 `aliyun-iot-arm` 项目后续封装为 Buildroot 可调用动态库的需求。

对应项目：

```text
E:\wulianwnag\aliyun-iot-arm
```

本地 IDE / 控制端项目：

```text
E:\windsurf-project\iot-controller-nodejs
```

目标运行环境：

```text
Buildroot ARM64
```

## 1. 背景

当前项目已经可以编译出一个独立的 ARM64 可执行程序：

```text
build/arm64-cross/iot-ide
```

它作为独立进程运行时，会自己读取 `device_id.json`、连接阿里云 IoT MQTT、订阅服务请求，并处理：

- IDE 连接请求
- IDE 断开连接
- IDE 心跳
- 项目部署
- 项目启动
- 属性上报
- 设备影子
- 网关子设备相关逻辑

现在新的集成方式发生变化：Buildroot 系统上会有一个主进程，例如：

```text
iec_runtime
```

后续希望由 `iec_runtime` 进程负责和阿里云 IoT 平台交互。阿里云下发的服务请求先到达 `iec_runtime`，然后 `iec_runtime` 再调用当前项目封装出来的动态库能力。

因此当前项目不再只作为独立后台服务运行，而是需要额外产出一个可被 C 程序调用的动态库。

## 2. 总体目标

把当前项目中的核心业务能力封装成 Buildroot/Linux 可调用的 `.so` 动态库。

Linux / Buildroot 目标产物：

```text
libiot_ide.so
```

Windows 上同类概念是 `.dll`，但本项目当前目标是 Buildroot ARM64，所以主要产物是 `.so`。

最终关系如下：

```text
本地 IDE / Node 控制端
        |
        | 调用阿里云 IoT OpenAPI
        v
阿里云物联网平台
        |
        | MQTT / 服务调用下发
        v
Buildroot 上的 iec_runtime 进程
        |
        | C 接口调用
        v
libiot_ide.so
        |
        | 执行连接锁、部署、启动等业务逻辑
        v
返回结果 / 通过回调通知 iec_runtime
```

## 3. 架构选择

当前需求采用以下架构：

```text
阿里云已经转发到 iec_runtime，iec_runtime 再调用 libiot_ide.so
```

也就是说：

- `iec_runtime` 负责常驻运行
- `iec_runtime` 负责接收阿里云下发的请求
- `iec_runtime` 负责调用动态库接口
- 当前项目封装出的 `.so` 负责业务处理能力
- 当前项目的 `.so` 不再自己作为独立后台服务启动

不推荐在同一台目标机上同时启动：

```text
iec_runtime
iot-ide 后台服务
```

否则可能出现两个进程同时处理同一类 IoT 业务、重复登录、重复维护 IDE 连接状态等问题。

## 4. 需要保留的当前能力

动态库中需要优先保留当前项目已经实现的核心能力：

- IDE 连接锁管理
- 一次只允许一个 IDE 连接
- 同一个 IDE 重连允许
- 旧 IDE 心跳超时后允许新 IDE 接管
- IDE 断开连接
- IDE 心跳续约
- 部署项目
- 启动项目
- 状态事件通知
- 日志事件通知

其中“一次只能有一个 IDE 连接”的逻辑，当前项目中已经由 `ide_connection_manager` 实现，后续封装动态库时应复用这部分逻辑。

## 5. 动态库接口要求

真实调用方 `iec_runtime` 是 C 语言实现，因此动态库必须对外提供稳定的 C ABI。

对外头文件不能暴露 C++ 类、C++ 模板、C++ 引用等内容。

如果以后内部实现使用 C++，对外头文件也必须保持：

```c
#ifdef __cplusplus
extern "C" {
#endif

/* C API */

#ifdef __cplusplus
}
#endif
```

当前项目本身是 C 实现，所以建议直接提供纯 C 头文件。

建议新增对外头文件：

```text
include/iot_ide_runtime_api.h
```

建议新增实现文件：

```text
src/iot_ide_runtime_api.c
```

## 6. 调用方向

### 6.1 iec_runtime 调用 libiot_ide.so

这是主调用方向。

典型接口包括：

```c
int iot_ide_runtime_create(...);
int iot_ide_runtime_destroy(...);
int iot_ide_runtime_request_connect(...);
int iot_ide_runtime_request_disconnect(...);
int iot_ide_runtime_heartbeat(...);
int iot_ide_runtime_deploy_project(...);
int iot_ide_runtime_start_project(...);
int iot_ide_runtime_get_connection_snapshot(...);
```

`iec_runtime` 收到阿里云下发请求后，将请求参数传给这些接口。

### 6.2 libiot_ide.so 回调 iec_runtime

这是反向通知方向。

动态库内部发生事件时，需要通知 `iec_runtime`，例如：

- IDE 连接状态变化
- IDE 心跳更新
- 部署任务开始
- 部署任务成功
- 部署任务失败
- 启动任务成功
- 启动任务失败
- 日志输出

建议采用 callback 方式。

`iec_runtime` 初始化动态库时注册一组回调函数，动态库需要通知外部时调用这些函数。

示例结构：

```c
typedef struct IotIdeRuntimeCallbacks {
    void (*on_event)(void *user_data, const char *event_name, const char *event_json);
    void (*on_log)(void *user_data, int level, const char *message);
} IotIdeRuntimeCallbacks;
```

其中：

- `user_data` 由 `iec_runtime` 传入，动态库只透传
- `event_name` 表示事件名称
- `event_json` 表示事件内容
- `on_log` 用于输出动态库内部日志

## 7. 建议的 C API 草案

下面只是需求阶段的接口草案，后续实现时可以根据当前代码结构微调。

```c
#ifndef IOT_IDE_RUNTIME_API_H
#define IOT_IDE_RUNTIME_API_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

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
```

## 8. 参数格式

为了和当前阿里云服务调用参数保持一致，第一阶段建议接口继续使用 JSON 字符串作为参数。

例如连接请求：

```json
{
  "clientId": "ide-client-001",
  "clientInfo": {
    "name": "local-ide",
    "host": "developer-pc"
  }
}
```

调用：

```c
char response[1024];
iot_ide_runtime_request_connect(runtime, params_json, response, sizeof(response));
```

返回：

```json
{
  "success": 1,
  "message": "connect accepted"
}
```

部署请求继续保持类似当前 `deployProject` 参数：

```json
{
  "projectName": "demo_project",
  "downloadUrl": "http://example.com/demo_project.zip",
  "deployPath": "/opt/iec-projects",
  "deployCommand": "./install.sh"
}
```

这样做的好处：

- `iec_runtime` 可以直接透传阿里云下发参数
- 当前项目已有 JSON 解析逻辑可以复用
- 第一阶段改动较小

## 9. 测试方式

当前 Buildroot 目标机上暂时没有真实 `iec_runtime` 进程，因此需要提供一个 C 测试程序来模拟 `iec_runtime`。

建议新增测试程序：

```text
demos/iot_ide_runtime_api_test.c
```

测试产物：

```text
build/arm64-cross/iot_ide_runtime_api_test
```

测试目标机上放置：

```sh
/opt/iot-ide-test/libiot_ide.so
/opt/iot-ide-test/iot_ide_runtime_api_test
```

测试程序负责：

- 创建 runtime
- 注册 callback
- 调用 requestConnect
- 再调用 heartbeat
- 测试重复 IDE 连接被拒绝
- 测试同一 IDE disconnect
- 调用 deployProject
- 调用 startProject
- 打印所有返回 JSON
- 打印动态库回调事件

注意：`.so` 文件本身不能直接运行，必须由测试 main 或真实 `iec_runtime` 加载和调用。

## 10. 构建产物要求

后续构建脚本建议同时支持生成：

```text
build/arm64-cross/libiot_ide.so
build/arm64-cross/iot_ide_runtime_api_test
```

原来的独立程序可以保留为兼容/调试用途：

```text
build/arm64-cross/iot-ide
```

但在最终 iec_runtime 集成模式下，目标机只需要启动：

```text
iec_runtime
```

不再需要单独启动：

```text
iot-ide
```

## 11. 部署方式

测试阶段建议目标机目录：

```sh
/opt/iot-ide-test
```

放置：

```sh
/opt/iot-ide-test/libiot_ide.so
/opt/iot-ide-test/iot_ide_runtime_api_test
```

如果使用动态链接运行测试程序，可能需要设置：

```sh
export LD_LIBRARY_PATH=/opt/iot-ide-test:$LD_LIBRARY_PATH
```

然后运行：

```sh
cd /opt/iot-ide-test
./iot_ide_runtime_api_test
```

正式集成阶段，`libiot_ide.so` 放到 `iec_runtime` 可加载的位置，例如：

```sh
/usr/lib/libiot_ide.so
```

或者放到 `iec_runtime` 同目录，并由 `iec_runtime` 设置加载路径。

## 12. 当前独立服务文档的关系

之前的 Buildroot 服务文档：

```text
docs/buildroot_service.md
```

适用于当前项目作为独立 `iot-ide` 进程运行的模式。

本需求文档描述的是新的动态库集成模式。

两种模式区别如下：

```text
独立服务模式：
阿里云 -> iot-ide 进程

动态库模式：
阿里云 -> iec_runtime 进程 -> libiot_ide.so
```

后续如果采用动态库模式，正式部署时不需要再启动 `S99iot-ide` 服务。

## 13. 第一阶段实现范围

第一阶段建议只实现动态库最小闭环：

- 新增 C API 头文件
- 新增 C API 实现文件
- 复用连接管理逻辑
- 复用部署逻辑
- 复用启动逻辑
- 支持事件 callback
- 编译生成 `libiot_ide.so`
- 编译生成 C 测试程序
- 在 Buildroot 上通过测试程序验证动态库可调用

第一阶段暂不要求：

- `iec_runtime` 真实接入
- 完整替换现有独立 `iot-ide` 程序
- 动态库内部继续连接阿里云 MQTT
- 动态库内部维护 `device_id.json`
- 把动态库做成后台服务

## 14. 待确认问题

后续编码前需要确认以下问题：

1. `iec_runtime` 调用 `.so` 时，是编译期链接 `-liot_ide`，还是运行时 `dlopen` 加载？
2. `iec_runtime` 传给 `.so` 的参数是否完全沿用阿里云服务调用 JSON？
3. 动态库回调事件是否统一用 JSON，还是部分事件用结构体？
4. 部署、启动这些异步操作的最终结果，是只通过 callback 返回，还是也需要 `query` 接口查询？
5. `deployProject` 下载、解压、执行命令这些动作是否仍然由动态库负责？
6. `startProject` 最终是由动态库直接执行 shell 命令，还是通过 callback 请求 `iec_runtime` 执行？
7. 正式部署时 `libiot_ide.so` 放在 `/usr/lib`，还是放在 `iec_runtime` 同目录？

## 15. 当前理解结论

当前需求的核心结论是：

```text
Buildroot 上最终常驻的是 iec_runtime。
当前 aliyun-iot-arm 项目需要封装成 libiot_ide.so。
iec_runtime 用纯 C 接口调用 libiot_ide.so。
libiot_ide.so 通过 callback 向 iec_runtime 推送事件。
测试阶段先写一个 C main 程序模拟 iec_runtime 调用动态库。
```

