# 本地 IDE、阿里云、iec_runtime、libiot_ide.so 四方调用流转

本文档说明当前动态库集成模式下，四个角色之间如何调用和流转。

四个角色是：

```text
本地 IDE / Node 控制端
阿里云 IoT 平台
iec_runtime 进程
libiot_ide.so 动态库
```

当前项目中，`iec_runtime` 还没有真实源码接入，因此用参考程序模拟：

```text
demos/iec_runtime_simulator.c
```

动态库接口定义在：

```text
include/iot_ide_runtime_api.h
```

## 1. 总体架构

动态库模式下，`libiot_ide.so` 不直接连接阿里云。

完整链路是：

```text
本地 IDE / Node 控制端
        |
        | 调用阿里云 OpenAPI / invokeThingService
        v
阿里云 IoT 平台
        |
        | MQTT 服务下发
        v
iec_runtime 进程
        |
        | C API 调用
        v
libiot_ide.so
        |
        | callback 事件
        v
iec_runtime 进程
        |
        | MQTT 发布属性 / 回复服务调用
        v
阿里云 IoT 平台
        |
        | 查询属性 / 服务调用结果
        v
本地 IDE / Node 控制端
```

当前模拟进程 `iec_runtime_simulator` 做的事情就是：

- 读取 `device_id.json`
- 连接阿里云 MQTT
- 订阅 `/sys/{productKey}/{deviceName}/thing/service/#`
- 收到服务调用后调用 `libiot_ide.so`
- 处理动态库 callback
- 把属性和服务回复发布回阿里云

## 2. 职责边界

### 2.1 本地 IDE / Node 控制端

本地控制端负责发起用户动作，例如：

- 连接目标机
- 发送 IDE 心跳
- 断开连接
- 部署项目
- 启动项目
- 查询设备属性

在当前 Node 项目中，对应项目是：

```text
E:\windsurf-project\iot-controller-nodejs
```

它通过阿里云 OpenAPI 调用设备服务，例如：

```text
requestConnect
requestDisconnect
ideHeartbeat
deployProject
startProject
```

### 2.2 阿里云 IoT 平台

阿里云负责中转：

- 接收本地控制端的 OpenAPI 服务调用
- 把服务请求下发到设备 MQTT topic
- 接收设备侧服务回复
- 接收设备属性上报
- 保存物模型属性状态

设备侧收到的服务 topic 类似：

```text
/sys/{productKey}/{deviceName}/thing/service/requestConnect
/sys/{productKey}/{deviceName}/thing/service/ideHeartbeat
/sys/{productKey}/{deviceName}/thing/service/deployProject
```

设备侧回复 topic 类似：

```text
/sys/{productKey}/{deviceName}/thing/service/requestConnect_reply
```

属性上报 topic 是：

```text
/sys/{productKey}/{deviceName}/thing/event/property/post
```

### 2.3 iec_runtime 进程

真实环境中，`iec_runtime` 是 Buildroot 上的常驻 C 进程。

它负责：

- 连接阿里云 MQTT
- 接收阿里云服务下发
- 解析 `serviceName`、`id`、`params`
- 调用 `libiot_ide.so` 的 C API
- 接收 `libiot_ide.so` 的 callback
- 把 callback 事件发布回阿里云

当前参考实现是：

```text
demos/iec_runtime_simulator.c
```

### 2.4 libiot_ide.so 动态库

动态库负责当前项目的核心业务逻辑：

- IDE 连接锁
- 同一时间只允许一个 IDE 连接
- IDE 心跳续约
- IDE 断开连接
- 部署项目
- 启动项目
- 生成响应 JSON
- 通过 callback 通知状态变化

动态库不负责：

- 不直接连接阿里云
- 不直接订阅 MQTT
- 不直接读取 `device_id.json`
- 不作为后台服务启动

## 3. 初始化流程

### 3.1 iec_runtime 初始化动态库

`iec_runtime` 启动后先初始化动态库：

```c
IotIdeRuntimeCallbacks callbacks;
memset(&callbacks, 0, sizeof(callbacks));
callbacks.on_event = on_iot_ide_event;
callbacks.on_log = on_iot_ide_log;

IotIdeRuntimeOptions options;
memset(&options, 0, sizeof(options));
options.work_dir = "/opt/iec-runtime";
options.callbacks = &callbacks;
options.user_data = iec_runtime_context;

iot_ide_runtime_create(&options, &iot_ide);
```

对应参考代码：

```text
demos/iec_runtime_simulator.c
```

函数：

```c
main(...)
```

### 3.2 iec_runtime 连接阿里云

`iec_runtime` 使用自己的阿里云 MQTT 逻辑连接云端。

当前模拟进程中对应：

```c
simulator_start_mqtt(...)
```

它订阅：

```text
/sys/{productKey}/{deviceName}/thing/service/#
```

启动成功后会看到类似日志：

```text
AIOT_MQTTEVT_CONNECT
subscribed: /sys/.../.../thing/service/#
```

## 4. requestConnect 连接流程

### 4.1 本地 IDE 发起连接

本地 Node 控制端调用阿里云服务：

```text
requestConnect
```

参数类似：

```json
{
  "clientId": "ide-client-12345678",
  "clientInfo": {
    "platform": "nodejs",
    "version": "1.0.0",
    "hostname": "PC-20221206MUVL",
    "pid": 24000
  }
}
```

### 4.2 阿里云下发到 iec_runtime

阿里云下发 MQTT topic：

```text
/sys/{productKey}/{deviceName}/thing/service/requestConnect
```

payload 类似：

```json
{
  "method": "thing.service.requestConnect",
  "id": "648021301",
  "params": {
    "clientId": "ide-client-12345678",
    "clientInfo": "{\"platform\":\"nodejs\",\"version\":\"1.0.0\"}"
  },
  "version": "1.0.0"
}
```

### 4.3 iec_runtime 调用动态库

`iec_runtime` 解析出：

```text
service_path = requestConnect
request_id = 648021301
params_json = {...}
```

然后调用：

```c
iot_ide_runtime_request_connect(iot_ide, params_json, response, sizeof(response));
```

参考代码：

```c
simulator_handle_service(...)
```

### 4.4 libiot_ide.so 内部处理

动态库内部会进入连接管理逻辑：

```text
ide_connection_manager_handle_request_connect
```

判断规则：

- 当前没有 IDE 连接：接受
- 当前是同一个 `clientId` 重连：接受
- 原连接心跳超时：允许新 IDE 接管
- 其他 IDE 正在连接：拒绝

接受后，动态库更新内部状态：

```text
connected = 1
current_client_id = clientId
last_heartbeat_ms = now
```

### 4.5 动态库 callback 给 iec_runtime

连接成功后，动态库发出 callback：

```text
event_name = property.post
```

内容类似：

```json
{
  "hasIDEConnected": 1,
  "IDEInfo": "...",
  "IDEHeartbeat": "1779107111811"
}
```

`iec_runtime` 收到后发布属性上报：

```text
/sys/{productKey}/{deviceName}/thing/event/property/post
```

参考代码：

```c
simulator_on_event(...)
simulator_post_properties(...)
```

### 4.6 iec_runtime 回复阿里云服务调用

动态库返回：

```json
{
  "success": 1,
  "message": "connect accepted"
}
```

`iec_runtime` 发布到：

```text
/sys/{productKey}/{deviceName}/thing/service/requestConnect_reply
```

payload：

```json
{
  "id": "648021301",
  "code": 200,
  "data": {
    "success": 1,
    "message": "connect accepted"
  }
}
```

### 4.7 本地 IDE 判断连接成功

本地控制端一般会结合两类结果判断连接成功：

- `requestConnect_reply` 返回成功
- 查询阿里云属性，看到 `hasIDEConnected=1` 且 `IDEInfo.clientId` 匹配当前 IDE

## 5. ideHeartbeat 业务心跳流程

注意：这里说的是 IDE 业务心跳，不是 MQTT 协议心跳。

### 5.1 本地 IDE 周期性发心跳

本地控制端周期性调用阿里云服务：

```text
ideHeartbeat
```

参数：

```json
{
  "clientId": "ide-client-12345678"
}
```

### 5.2 阿里云下发到 iec_runtime

topic：

```text
/sys/{productKey}/{deviceName}/thing/service/ideHeartbeat
```

### 5.3 iec_runtime 调用动态库

```c
iot_ide_runtime_heartbeat(iot_ide, params_json, response, sizeof(response));
```

### 5.4 libiot_ide.so 更新心跳

动态库检查：

```text
params.clientId == 当前连接的 clientId
```

如果匹配，则更新：

```text
last_heartbeat_ms = now
```

并返回：

```json
{
  "success": 1,
  "message": "heartbeat updated"
}
```

### 5.5 动态库 callback 上报心跳属性

动态库发出：

```text
event_name = property.post
```

内容：

```json
{
  "IDEHeartbeat": "1779107143299"
}
```

`iec_runtime` 发布到阿里云属性上报 topic。

### 5.6 iec_runtime 回复 ideHeartbeat

回复 topic：

```text
/sys/{productKey}/{deviceName}/thing/service/ideHeartbeat_reply
```

payload：

```json
{
  "id": "907956714",
  "code": 200,
  "data": {
    "success": 1,
    "message": "heartbeat updated"
  }
}
```

## 6. requestDisconnect 断开连接流程

### 6.1 本地 IDE 发起断开

服务名：

```text
requestDisconnect
```

参数：

```json
{
  "clientId": "ide-client-12345678"
}
```

### 6.2 iec_runtime 调用动态库

```c
iot_ide_runtime_request_disconnect(iot_ide, params_json, response, sizeof(response));
```

### 6.3 libiot_ide.so 判断是否允许断开

规则：

- 当前没有连接：返回成功，表示无连接可断开
- `clientId` 不是当前 IDE：拒绝
- `clientId` 是当前 IDE：清空连接状态

成功断开后，动态库发出：

```text
property.post
```

内容：

```json
{
  "hasIDEConnected": 0,
  "IDEInfo": "",
  "IDEHeartbeat": "0"
}
```

### 6.4 iec_runtime 回复阿里云

回复：

```json
{
  "success": 1,
  "message": "disconnect accepted"
}
```

## 7. deployProject 部署流程

### 7.1 本地 IDE 发起部署

服务名：

```text
deployProject
```

参数通常包含：

```json
{
  "clientId": "ide-client-12345678",
  "projectName": "demo",
  "downloadUrl": "http://example.com/demo.zip",
  "deployPath": "/tmp/deploy",
  "deployCommand": "./install.sh"
}
```

### 7.2 iec_runtime 调用动态库

```c
iot_ide_runtime_deploy_project(iot_ide, params_json, response, sizeof(response));
```

### 7.3 libiot_ide.so 先校验连接锁

动态库会先检查：

```text
clientId 是否等于当前已连接 IDE
```

如果不是当前 IDE，直接返回：

```json
{
  "success": 0,
  "message": "deploy rejected, clientId is not the active IDE connection"
}
```

### 7.4 libiot_ide.so 异步执行部署

如果校验通过，动态库会创建部署线程，异步执行：

- 检查 `downloadUrl`
- 创建部署目录
- 使用 `curl` 或 `wget` 下载 zip
- 使用 `unzip` 或 `busybox unzip` 解压
- 如果有 `deployCommand`，进入项目目录执行部署命令

立即返回：

```json
{
  "success": 1,
  "message": "deploy task received, executing asynchronously",
  "deployLog": ""
}
```

### 7.5 部署完成后 callback 上报结果

部署线程结束后，动态库会发：

```text
property.post
```

内容包含：

```json
{
  "deployStatus": "..."
}
```

`deployStatus` 内部包含：

- `success`
- `message`
- `deployLog`
- `timestamp`
- `projectName`
- `deployPath`

### 7.6 网络注意事项

动态库不连接阿里云 MQTT，但部署时会访问：

```text
downloadUrl
```

所以如果 `downloadUrl` 是外网地址，Buildroot 仍然需要能访问该下载地址。

## 8. startProject 启动流程

### 8.1 本地 IDE 发起启动

服务名：

```text
startProject
```

参数：

```json
{
  "clientId": "ide-client-12345678",
  "projectName": "demo",
  "deployPath": "/tmp/deploy",
  "startCommand": "./start.sh"
}
```

### 8.2 iec_runtime 调用动态库

```c
iot_ide_runtime_start_project(iot_ide, params_json, response, sizeof(response));
```

### 8.3 libiot_ide.so 校验连接锁

同部署一样，先检查：

```text
clientId 是否等于当前已连接 IDE
```

不匹配则拒绝。

### 8.4 libiot_ide.so 执行启动命令

动态库拼接项目目录：

```text
{deployPath}/{projectName}
```

然后在这个目录下执行：

```text
startCommand
```

如果目录不存在，会返回类似：

```text
CHDIR:No such file or directory
```

所以启动前需要确保项目已经部署到：

```text
{deployPath}/{projectName}
```

成功时返回：

```json
{
  "success": 1,
  "message": "service started in background, pid=..."
}
```

或：

```json
{
  "success": 1,
  "message": "start command finished successfully"
}
```

## 9. property/get 属性查询流程

### 9.1 本地控制端查询属性

本地 Node 控制端可以通过阿里云查询属性状态。

常见查询内容包括：

- `hasIDEConnected`
- `IDEInfo`
- `IDEHeartbeat`

### 9.2 阿里云保存属性状态

这些属性不是本地 IDE 直接写入的，而是设备侧通过：

```text
thing.event.property.post
```

上报给阿里云后，由阿里云保存。

### 9.3 iec_runtime 收到 property/get

如果阿里云下发：

```text
property/get
```

当前模拟进程会调用：

```c
iot_ide_runtime_get_connection_snapshot(iot_ide, response, sizeof(response));
```

返回动态库内部当前连接快照：

```json
{
  "connected": 1,
  "clientId": "ide-client-12345678",
  "heartbeatMs": 1779107143299
}
```

真实 `iec_runtime` 可以根据自己的物模型格式包装后回复。

## 10. MQTT 心跳和 IDE 心跳的区别

日志里的：

```text
heartbeat response
```

是 MQTT 协议层心跳。

方向是：

```text
iec_runtime -> 阿里云 MQTT Broker
阿里云 MQTT Broker -> iec_runtime
```

作用是保持 MQTT 长连接。

而：

```text
ideHeartbeat
```

是业务心跳。

方向是：

```text
本地 IDE -> 阿里云 -> iec_runtime -> libiot_ide.so
```

作用是保持当前 IDE 对目标机的连接占用锁。

## 11. 当前已经验证通过的链路

你当前日志已经验证：

```text
requestConnect
ideHeartbeat
property.post
service reply
MQTT heartbeat
```

都已经跑通。

从日志看，完整闭环是：

```text
阿里云下发 requestConnect
iec_runtime_simulator 收到
调用 libiot_ide.so
动态库接受连接
callback: property.post
iec_runtime_simulator 上报 hasIDEConnected / IDEInfo / IDEHeartbeat
iec_runtime_simulator 回复 requestConnect_reply
阿里云返回 property/post_reply 成功
```

心跳闭环是：

```text
阿里云下发 ideHeartbeat
iec_runtime_simulator 收到
调用 libiot_ide.so
动态库更新 IDEHeartbeat
callback: property.post
iec_runtime_simulator 上报 IDEHeartbeat
iec_runtime_simulator 回复 ideHeartbeat_reply
阿里云返回 property/post_reply 成功
```

## 12. 真实 iec_runtime 接入时的最小实现清单

真实 `iec_runtime` 侧至少需要实现：

- 连接阿里云 MQTT
- 订阅 `/sys/{pk}/{dn}/thing/service/#`
- 初始化 `libiot_ide.so`
- 保存 `IotIdeRuntime *iot_ide`
- 收到 `requestConnect` 时调用 `iot_ide_runtime_request_connect`
- 收到 `requestDisconnect` 时调用 `iot_ide_runtime_request_disconnect`
- 收到 `ideHeartbeat` 时调用 `iot_ide_runtime_heartbeat`
- 收到 `deployProject` 时调用 `iot_ide_runtime_deploy_project`
- 收到 `startProject` 时调用 `iot_ide_runtime_start_project`
- 在 callback 里处理 `property.post`
- 在 callback 里处理 `trace.publish`
- 必要时处理 `service.reply`
- 把 API 返回值发布到 `{service}_reply`
- 退出时调用 `iot_ide_runtime_destroy`

参考实现：

```text
demos/iec_runtime_simulator.c
```

