# 本地 IDE、阿里云、iec_runtime、动态库流转

本文说明当前集成模式下，本地 IDE、阿里云、`iec_runtime`、`libiot_ide_gateway.so`、`libiot_ide.so` 之间怎么流转。

```text
本地 IDE / Node 控制端
  -> 阿里云 IoT 平台
  -> libiot_ide_gateway.so
  -> iec_runtime
  -> libiot_ide.so
  -> iec_runtime
  -> libiot_ide_gateway.so
  -> 阿里云 IoT 平台
  -> 本地 IDE / Node 控制端
```

## 职责

本地 IDE / Node 控制端负责发起业务动作，例如连接、心跳、断开、部署、启动。

阿里云 IoT 平台负责中转服务调用，并保存设备上报的属性状态。

`libiot_ide_gateway.so` 负责连接阿里云 MQTT、订阅服务下发、解析服务 JSON、上报属性、回复服务调用。

`iec_runtime` 是 Buildroot 上的常驻 C 进程。现在它只负责把 gateway 收到的服务分发给 `libiot_ide.so`，再把 `libiot_ide.so` 的事件交给 gateway 上报阿里云。

`libiot_ide.so` 负责 IDE 连接锁、心跳状态、断开、部署、启动等业务逻辑。它不直接连接阿里云。

## requestConnect

```text
本地 IDE
  -> 调阿里云 requestConnect
阿里云
  -> 下发 /sys/{pk}/{dn}/thing/service/requestConnect
libiot_ide_gateway.so
  -> 收到 MQTT 消息
  -> 解析 serviceName=requestConnect、requestId、params
  -> 回调 iec_runtime
iec_runtime
  -> 调 iot_ide_runtime_request_connect(...)
libiot_ide.so
  -> 判断连接锁，接受或拒绝
  -> callback: property.post
iec_runtime
  -> 调 iot_ide_gateway_forward_event(...)
libiot_ide_gateway.so
  -> 上报 hasIDEConnected / IDEInfo / IDEHeartbeat
  -> 回复 requestConnect_reply
阿里云
  -> 保存属性，并把服务结果返回给本地 IDE
```

## ideHeartbeat

```text
本地 IDE
  -> 周期性调阿里云 ideHeartbeat
阿里云
  -> 下发 /sys/{pk}/{dn}/thing/service/ideHeartbeat
libiot_ide_gateway.so
  -> 解析 serviceName=ideHeartbeat、requestId、params
  -> 回调 iec_runtime
iec_runtime
  -> 调 iot_ide_runtime_heartbeat(...)
libiot_ide.so
  -> 校验 clientId 是否是当前连接 IDE
  -> 更新 IDEHeartbeat
  -> callback: property.post
iec_runtime
  -> 调 iot_ide_gateway_forward_event(...)
libiot_ide_gateway.so
  -> 上报 IDEHeartbeat
  -> 回复 ideHeartbeat_reply
```

注意：`ideHeartbeat` 是业务心跳；日志里的 `heartbeat response` 是 MQTT 协议心跳，表示 `libiot_ide_gateway.so` 和阿里云 MQTT Broker 的连接还活着。

## requestDisconnect

```text
本地 IDE
  -> 调阿里云 requestDisconnect
阿里云
  -> 下发 requestDisconnect
libiot_ide_gateway.so
  -> 解析服务并回调 iec_runtime
iec_runtime
  -> 调 iot_ide_runtime_request_disconnect(...)
libiot_ide.so
  -> 校验 clientId
  -> 清空连接状态
  -> callback: property.post
iec_runtime
  -> 调 iot_ide_gateway_forward_event(...)
libiot_ide_gateway.so
  -> 上报 hasIDEConnected=0、IDEInfo=""、IDEHeartbeat="0"
  -> 回复 requestDisconnect_reply
```

## deployProject

```text
本地 IDE
  -> 调阿里云 deployProject，参数包含 clientId、projectName、downloadUrl、deployPath、deployCommand
阿里云
  -> 下发 deployProject
libiot_ide_gateway.so
  -> 解析服务并回调 iec_runtime
iec_runtime
  -> 调 iot_ide_runtime_deploy_project(...)
libiot_ide.so
  -> 校验 clientId 是否是当前 IDE
  -> 异步下载、解压、执行部署命令
  -> callback: property.post，包含 deployStatus
libiot_ide_gateway.so
  -> 先回复 deployProject_reply，表示任务已接收
  -> 部署完成后上报 deployStatus
```

部署会访问 `downloadUrl`。如果 `downloadUrl` 是外网地址，Buildroot 需要能访问该地址。

## startProject

```text
本地 IDE
  -> 调阿里云 startProject，参数包含 clientId、projectName、deployPath、startCommand
阿里云
  -> 下发 startProject
libiot_ide_gateway.so
  -> 解析服务并回调 iec_runtime
iec_runtime
  -> 调 iot_ide_runtime_start_project(...)
libiot_ide.so
  -> 校验 clientId 是否是当前 IDE
  -> 进入 {deployPath}/{projectName}
  -> 执行 startCommand
libiot_ide_gateway.so
  -> 回复 startProject_reply
```

如果 `{deployPath}/{projectName}` 不存在，启动会失败并返回 `CHDIR:No such file or directory`。

## 同事要看的代码

直接看这一个文件：

```text
iec_runtime.c
```

这个文件现在不再包含阿里云 C SDK 细节，只展示生产集成里的胶水逻辑：

```text
gateway service callback -> 调 libiot_ide.so
libiot_ide.so event callback -> 调 gateway 上报阿里云
```

同事需要编译集成的头文件：

```text
include/libiot_ide_gateway.h
include/libiot_ide.h
```
