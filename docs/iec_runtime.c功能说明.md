# iec_runtime.c 功能说明

`iec_runtime.c` 现在不是负责所有细节了，它是一个“生产集成胶水层”。

它主要实现这几件事。

## 1. 初始化两个动态库

启动时先创建：

```c
iot_ide_gateway_create(...)
iot_ide_runtime_create(...)
```

对应：

```text
libiot_ide_gateway.so
```

负责阿里云通信。

```text
libiot_ide.so
```

负责 IDE 业务逻辑。

## 2. 注册 gateway 服务回调

```c
gateway_callbacks.on_service = runtime_on_service;
```

意思是：阿里云下发服务后，`libiot_ide_gateway.so` 负责接收 MQTT、解析 JSON，然后回调到 `iec_runtime.c` 的：

```c
runtime_on_service(...)
```

这个函数能拿到：

```text
service_name
request_id
params_json
```

例如：

```text
requestConnect
deployProject
startProject
```

以及对应参数。

## 3. 把阿里云服务分发给 libiot_ide.so

`runtime_on_service(...)` 里面根据服务名调用动态库业务 API：

```c
requestConnect    -> iot_ide_runtime_request_connect(...)
requestDisconnect -> iot_ide_runtime_request_disconnect(...)
ideHeartbeat      -> iot_ide_runtime_heartbeat(...)
deployProject     -> iot_ide_runtime_deploy_project(...)
startProject      -> iot_ide_runtime_start_project(...)
property/get      -> iot_ide_runtime_get_connection_snapshot(...)
```

也就是说，`iec_runtime.c` 决定“阿里云哪个服务，对应调用业务动态库哪个函数”。

## 4. 回复阿里云服务调用

`libiot_ide.so` 返回 `response` 后，`iec_runtime.c` 调：

```c
iot_ide_gateway_reply_service(...)
```

把结果回复给阿里云，比如：

```json
{
  "success": 1,
  "message": "connect accepted"
}
```

或者：

```json
{
  "success": 1,
  "message": "heartbeat updated"
}
```

## 5. 注册 libiot_ide.so 事件回调

```c
iot_ide_callbacks.on_event = runtime_on_iot_ide_event;
```

`libiot_ide.so` 内部状态变化时会回调这里，比如：

```text
property.post
requestConnect.response
ideHeartbeat.response
deployProject.response
```

## 6. 把 libiot_ide.so 的事件交给 gateway 上报阿里云

在：

```c
runtime_on_iot_ide_event(...)
```

里面调用：

```c
iot_ide_gateway_forward_event(...)
```

比如动态库发出：

```json
{
  "hasIDEConnected": 1,
  "IDEInfo": "...",
  "IDEHeartbeat": "..."
}
```

`gateway` 就会把它发到阿里云属性上报 topic。

## 7. 启动阿里云连接

`iec_runtime.c` 调：

```c
iot_ide_gateway_start(...)
```

真正连接阿里云的是 `libiot_ide_gateway.so`，不是 `iec_runtime.c` 自己手写 MQTT 细节。

## 8. 处理退出

收到 `Ctrl+C` 或 `SIGTERM` 后：

```c
iot_ide_gateway_stop(...)
iot_ide_runtime_destroy(...)
iot_ide_gateway_destroy(...)
```

会停止阿里云连接、销毁动态库对象。

## 总结

一句话总结：

```text
iec_runtime.c 不直接实现阿里云 MQTT 细节，也不直接实现部署/启动业务细节。
它负责把 libiot_ide_gateway.so 和 libiot_ide.so 串起来：
阿里云服务 -> gateway -> iec_runtime.c -> libiot_ide.so -> iec_runtime.c -> gateway -> 阿里云
```
