# libiot_ide.so 集成说明

本文档说明当前项目封装出的动态库如何集成到 C 语言实现的 `iec_runtime` 进程里。

## 1. 当前产物

在 Windows 上执行：

```powershell
cd E:\wulianwnag\aliyun-iot-arm

.\scripts\build-arm64-cross-windows.ps1 `
  -ToolchainRoot 'E:\VSCode-win32-x64-1.85.1\data\extensions\undefined_publisher.devuni-ide-vscode-0.0.1\tool\iec-runtime-gen-run\gcc-arm-10.3-aarch64-none-linux-gnu'
```

会生成：

```text
build\arm64-cross\iot-ide
build\arm64-cross\libiot_ide.so
build\arm64-cross\iot_ide_runtime_api_test
```

其中：

- `iot-ide`：原来的独立测试程序，仍然保留
- `libiot_ide.so`：给 `iec_runtime` 调用的动态库
- `iot_ide_runtime_api_test`：模拟 `iec_runtime` 的 C 测试程序

对外 C API 头文件：

```text
include\iot_ide_runtime_api.h
```

## 2. 运行模型

动态库模式下，目标机不需要再启动独立的 `iot-ide` 后台服务。

最终运行关系是：

```text
阿里云 IoT 平台
        |
        v
iec_runtime 进程
        |
        | 调用 C API
        v
libiot_ide.so
```

`libiot_ide.so` 本身不能直接运行，它必须被 `iec_runtime` 或测试程序加载。

## 3. iec_runtime 编译期链接方式

推荐第一阶段使用编译期链接。

`iec_runtime` 源码中引用：

```c
#include "iot_ide_runtime_api.h"
```

编译时增加：

```sh
-I/path/to/iot_ide/include -L/path/to/iot_ide/lib -liot_ide
```

运行时目标机需要能找到 `libiot_ide.so`。可以放到：

```sh
/usr/lib/libiot_ide.so
```

或者放到 `iec_runtime` 同目录，并设置：

```sh
export LD_LIBRARY_PATH=/opt/iec-runtime:$LD_LIBRARY_PATH
```

## 4. C 调用示例

```c
#include "iot_ide_runtime_api.h"

#include <stdio.h>
#include <string.h>

static void on_event(void *user_data, const char *event_name, const char *event_json) {
    (void)user_data;
    printf("event: %s %s\n", event_name, event_json);
}

static void on_log(void *user_data, int level, const char *message) {
    (void)user_data;
    printf("log[%d]: %s\n", level, message);
}

int main(void) {
    IotIdeRuntime *runtime = NULL;
    IotIdeRuntimeCallbacks callbacks;
    IotIdeRuntimeOptions options;
    char response[1024];

    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.on_event = on_event;
    callbacks.on_log = on_log;

    memset(&options, 0, sizeof(options));
    options.work_dir = "/opt/iec-runtime";
    options.callbacks = &callbacks;

    if (iot_ide_runtime_create(&options, &runtime) != 0) {
        return 1;
    }

    iot_ide_runtime_request_connect(
        runtime,
        "{\"clientId\":\"ide-a\",\"clientInfo\":{\"name\":\"local-ide\"}}",
        response,
        sizeof(response));

    printf("connect response: %s\n", response);

    iot_ide_runtime_destroy(runtime);
    return 0;
}
```

## 5. 回调事件

`iec_runtime` 初始化动态库时注册 callback：

```c
typedef struct IotIdeRuntimeCallbacks {
    void (*on_event)(void *user_data, const char *event_name, const char *event_json);
    void (*on_log)(void *user_data, int level, const char *message);
} IotIdeRuntimeCallbacks;
```

当前动态库会通过 `on_event` 通知：

- `runtime.created`
- `runtime.destroying`
- `property.post`
- `service.reply`
- `trace.publish`
- `mqtt.publish`
- `requestConnect.response`
- `requestDisconnect.response`
- `ideHeartbeat.response`
- `deployProject.response`
- `startProject.response`

其中 `property.post` 对应原来独立程序里要上报到阿里云的属性内容。动态库模式下不会自己连 MQTT，而是把事件交给 `iec_runtime`，由 `iec_runtime` 决定是否上报阿里云。

## 6. Buildroot 上测试动态库

当前目标机没有真实 `iec_runtime` 时，可以用测试程序模拟。

上传这两个文件到目标机同一目录：

```text
build\arm64-cross\libiot_ide.so
build\arm64-cross\iot_ide_runtime_api_test
```

例如放到：

```sh
/opt/iot-ide-test/
```

执行：

```sh
cd /opt/iot-ide-test
chmod +x ./iot_ide_runtime_api_test
./iot_ide_runtime_api_test
```

测试程序已经带了 `$ORIGIN` rpath，所以只要 `libiot_ide.so` 和 `iot_ide_runtime_api_test` 在同一个目录，通常不需要额外设置 `LD_LIBRARY_PATH`。

如果目标机仍提示找不到动态库，则执行：

```sh
export LD_LIBRARY_PATH=/opt/iot-ide-test:$LD_LIBRARY_PATH
./iot_ide_runtime_api_test
```

## 7. 当前 API

头文件：

```text
include\iot_ide_runtime_api.h
```

主要接口：

```c
int iot_ide_runtime_get_api_version(void);

int iot_ide_runtime_create(const IotIdeRuntimeOptions *options, IotIdeRuntime **runtime);
void iot_ide_runtime_destroy(IotIdeRuntime *runtime);

int iot_ide_runtime_request_connect(IotIdeRuntime *runtime, const char *params_json, char *response_json, size_t response_size);
int iot_ide_runtime_request_disconnect(IotIdeRuntime *runtime, const char *params_json, char *response_json, size_t response_size);
int iot_ide_runtime_heartbeat(IotIdeRuntime *runtime, const char *params_json, char *response_json, size_t response_size);

int iot_ide_runtime_deploy_project(IotIdeRuntime *runtime, const char *params_json, char *response_json, size_t response_size);
int iot_ide_runtime_start_project(IotIdeRuntime *runtime, const char *params_json, char *response_json, size_t response_size);

int iot_ide_runtime_get_connection_snapshot(IotIdeRuntime *runtime, char *snapshot_json, size_t snapshot_size);
```

## 8. 注意事项

- `libiot_ide.so` 是动态库，不是服务，不需要 `/etc/init.d/S99iot-ide`
- 真实集成时只启动 `iec_runtime`
- `iec_runtime` 负责接收阿里云下发请求
- `iec_runtime` 负责调用动态库接口
- 动态库通过 callback 把事件推回 `iec_runtime`
- 部署任务是异步执行的，最终结果会通过 `property.post` callback 通知
- `iot_ide_runtime_destroy()` 会等待正在执行的部署任务结束后再释放资源

