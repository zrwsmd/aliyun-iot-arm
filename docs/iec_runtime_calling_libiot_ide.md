# iec_runtime 调用 libiot_ide.so 参考说明

本文档给 `iec_runtime` 侧同事参考，说明一个 C 进程如何集成当前项目生成的动态库：

```text
libiot_ide.so
```

当前项目已经提供一个可运行的参考进程：

```text
demos/iec_runtime_simulator.c
```

如果只想看最直观的动态库调用顺序，可以先看：

```text
demos/iec_runtime_libiot_ide_main_example.c
```

这个文件把初始化、连接、心跳、部署、启动、查询、断开这些动态库调用按顺序放在 `main()` 里，并在每一步前写了注释。

它模拟真实 `iec_runtime`：

- 连接阿里云 IoT MQTT
- 接收阿里云服务下发
- 调用 `libiot_ide.so` 的 C API
- 处理动态库 callback
- 把结果回复/属性上报发布回阿里云

## 1. 构建产物

执行：

```powershell
cd E:\wulianwnag\aliyun-iot-arm

.\scripts\build-arm64-cross-windows.ps1 `
  -ToolchainRoot 'E:\VSCode-win32-x64-1.85.1\data\extensions\undefined_publisher.devuni-ide-vscode-0.0.1\tool\iec-runtime-gen-run\gcc-arm-10.3-aarch64-none-linux-gnu'
```

会生成：

```text
build\arm64-cross\libiot_ide.so
build\arm64-cross\iec_runtime_simulator
```

其中：

- `libiot_ide.so` 是动态库
- `iec_runtime_simulator` 是模拟 `iec_runtime` 的参考程序

## 2. 目标机测试

上传到 Buildroot：

```text
/opt/iec-runtime-sim/libiot_ide.so
/opt/iec-runtime-sim/iec_runtime_simulator
/opt/iec-runtime-sim/device_id.json
```

运行：

```sh
cd /opt/iec-runtime-sim
chmod +x ./iec_runtime_simulator
./iec_runtime_simulator ./device_id.json
```

如果目标机找不到 `libiot_ide.so`：

```sh
export LD_LIBRARY_PATH=/opt/iec-runtime-sim:$LD_LIBRARY_PATH
./iec_runtime_simulator ./device_id.json
```

注意：这个模拟进程会真的连接阿里云 MQTT。如果 Buildroot 不能直连阿里云，需要继续使用 Windows MQTT relay。

## 3. 真实 iec_runtime 需要做什么

真实 `iec_runtime` 需要做四件事：

1. 初始化 `libiot_ide.so`
2. 收到阿里云服务下发后，根据服务名调用对应 C API
3. 在 callback 里处理动态库事件
4. 退出时销毁 runtime

## 4. 初始化动态库

头文件：

```c
#include "iot_ide_runtime_api.h"
```

初始化：

```c
static void on_iot_ide_event(void *user_data, const char *event_name, const char *event_json);
static void on_iot_ide_log(void *user_data, int level, const char *message);

IotIdeRuntime *iot_ide = NULL;

IotIdeRuntimeCallbacks callbacks;
memset(&callbacks, 0, sizeof(callbacks));
callbacks.on_event = on_iot_ide_event;
callbacks.on_log = on_iot_ide_log;

IotIdeRuntimeOptions options;
memset(&options, 0, sizeof(options));
options.work_dir = "/opt/iec-runtime";
options.callbacks = &callbacks;
options.user_data = iec_runtime_context;

if (iot_ide_runtime_create(&options, &iot_ide) != IOT_IDE_RUNTIME_OK) {
    /* 初始化失败 */
}
```

## 5. 阿里云服务下发如何转调动态库

真实 `iec_runtime` 收到阿里云服务调用后，一般能拿到：

- `service_name`
- `request_id`
- `params_json`

然后按服务名分发：

```c
char response[8192];
int rc = -1;

if (strcmp(service_name, "requestConnect") == 0) {
    rc = iot_ide_runtime_request_connect(iot_ide, params_json, response, sizeof(response));
} else if (strcmp(service_name, "requestDisconnect") == 0) {
    rc = iot_ide_runtime_request_disconnect(iot_ide, params_json, response, sizeof(response));
} else if (strcmp(service_name, "ideHeartbeat") == 0) {
    rc = iot_ide_runtime_heartbeat(iot_ide, params_json, response, sizeof(response));
} else if (strcmp(service_name, "deployProject") == 0) {
    rc = iot_ide_runtime_deploy_project(iot_ide, params_json, response, sizeof(response));
} else if (strcmp(service_name, "startProject") == 0) {
    rc = iot_ide_runtime_start_project(iot_ide, params_json, response, sizeof(response));
} else if (strcmp(service_name, "property/get") == 0) {
    rc = iot_ide_runtime_get_connection_snapshot(iot_ide, response, sizeof(response));
}
```

然后 `iec_runtime` 需要把 `response` 回复给阿里云服务调用。

参考代码见：

```text
demos/iec_runtime_simulator.c
```

函数：

```c
simulator_handle_service(...)
```

## 6. callback 事件怎么处理

动态库会通过：

```c
callbacks.on_event
```

推事件给 `iec_runtime`。

真实 `iec_runtime` 至少需要处理这几个事件：

```text
property.post
trace.publish
service.reply
```

### 6.1 property.post

含义：动态库希望上报设备属性。

`event_json` 就是属性 `params`，例如：

```json
{
  "hasIDEConnected": 1,
  "IDEInfo": "...",
  "IDEHeartbeat": "1779105620056"
}
```

`iec_runtime` 应该发布到：

```text
/sys/{productKey}/{deviceName}/thing/event/property/post
```

payload 格式：

```json
{
  "id": "xxx",
  "version": "1.0",
  "params": { ... },
  "method": "thing.event.property.post"
}
```

参考函数：

```c
simulator_post_properties(...)
```

### 6.2 service.reply

含义：动态库希望回复某个阿里云服务调用。

当前模拟进程支持这个事件，但目前主要服务调用返回值已经由 `simulator_handle_service()` 直接回复。

如果真实 `iec_runtime` 选择统一从 callback 回复，也可以处理：

```text
service.reply
```

参考函数：

```c
simulator_reply_service(...)
```

### 6.3 trace.publish

含义：动态库希望上报 trace 数据。

发布 topic：

```text
/{productKey}/{deviceName}/user/trace/data
```

参考函数：

```c
simulator_publish_trace(...)
```

## 7. 退出时销毁

真实 `iec_runtime` 退出前调用：

```c
iot_ide_runtime_destroy(iot_ide);
```

注意：如果部署任务还在运行，`destroy` 会等待部署任务结束后释放资源。

## 8. 编译真实 iec_runtime 时如何链接

如果使用编译期链接：

```sh
aarch64-none-linux-gnu-gcc \
  -I/path/to/aliyun-iot-arm/include \
  -L/path/to/lib \
  -o iec_runtime \
  your_sources.c \
  -liot_ide \
  -lpthread -ldl -lm -lrt
```

运行时保证能找到：

```text
libiot_ide.so
```

可以放到：

```text
/usr/lib/libiot_ide.so
```

或者和 `iec_runtime` 放同目录，并设置：

```sh
export LD_LIBRARY_PATH=/opt/iec-runtime:$LD_LIBRARY_PATH
```

## 9. 当前参考进程和真实 iec_runtime 的区别

`iec_runtime_simulator` 只是参考实现，真实 `iec_runtime` 不一定要照搬 MQTT 代码。

真实 `iec_runtime` 只需要照搬这几类逻辑：

- 初始化 `iot_ide_runtime_create`
- 服务名到动态库 API 的分发
- callback 事件处理
- 退出时 `iot_ide_runtime_destroy`

如果真实 `iec_runtime` 已经有自己的阿里云 MQTT 连接和发布函数，只需要把：

```c
simulator_post_properties(...)
simulator_reply_service(...)
simulator_publish_trace(...)
```

替换成真实 `iec_runtime` 的发布/回复函数即可。
