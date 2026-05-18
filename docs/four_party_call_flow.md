# 本地 IDE、阿里云、iec_runtime、libiot_ide.so 四方流转

本文只说明动态库集成模式下四方之间的职责和调用方向。

```text
本地 IDE / Node 控制端
  -> 阿里云 IoT 平台
  -> iec_runtime 进程
  -> libiot_ide.so
  -> iec_runtime 进程
  -> 阿里云 IoT 平台
  -> 本地 IDE / Node 控制端
```

## 四方职责

本地 IDE / Node 控制端负责发起业务动作，例如连接、心跳、断开、部署、启动。

阿里云 IoT 平台负责中转服务调用，并保存设备上报的属性状态。

iec_runtime 是 Buildroot 上的常驻 C 进程，负责连接阿里云 MQTT，接收服务下发，调用 `libiot_ide.so`，并把动态库 callback 里的属性和服务结果发布回阿里云。

`libiot_ide.so` 是当前项目封装出来的动态库，负责 IDE 连接锁、心跳状态、部署、启动等业务逻辑。它不直接连接阿里云。

## requestConnect

```text
本地 IDE
  -> 调阿里云 requestConnect
阿里云
  -> 下发 /sys/{pk}/{dn}/thing/service/requestConnect
iec_runtime
  -> 调 iot_ide_runtime_request_connect(...)
libiot_ide.so
  -> 判断连接锁，接受或拒绝
  -> callback: property.post
iec_runtime
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
iec_runtime
  -> 调 iot_ide_runtime_heartbeat(...)
libiot_ide.so
  -> 校验 clientId 是否是当前连接 IDE
  -> 更新 IDEHeartbeat
  -> callback: property.post
iec_runtime
  -> 上报 IDEHeartbeat
  -> 回复 ideHeartbeat_reply
```

注意：`ideHeartbeat` 是业务心跳；日志里的 `heartbeat response` 是 MQTT 协议心跳，表示 `iec_runtime` 和阿里云 MQTT Broker 的连接还活着。

## requestDisconnect

```text
本地 IDE
  -> 调阿里云 requestDisconnect
阿里云
  -> 下发 requestDisconnect
iec_runtime
  -> 调 iot_ide_runtime_request_disconnect(...)
libiot_ide.so
  -> 校验 clientId
  -> 清空连接状态
  -> callback: property.post
iec_runtime
  -> 上报 hasIDEConnected=0、IDEInfo=""、IDEHeartbeat="0"
  -> 回复 requestDisconnect_reply
```

## deployProject

```text
本地 IDE
  -> 调阿里云 deployProject，参数包含 clientId、projectName、downloadUrl、deployPath、deployCommand
阿里云
  -> 下发 deployProject
iec_runtime
  -> 调 iot_ide_runtime_deploy_project(...)
libiot_ide.so
  -> 校验 clientId 是否是当前 IDE
  -> 异步下载、解压、执行部署命令
  -> callback: property.post，包含 deployStatus
iec_runtime
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
iec_runtime
  -> 调 iot_ide_runtime_start_project(...)
libiot_ide.so
  -> 校验 clientId 是否是当前 IDE
  -> 进入 {deployPath}/{projectName}
  -> 执行 startCommand
iec_runtime
  -> 回复 startProject_reply
```

如果 `{deployPath}/{projectName}` 不存在，启动会失败并返回 `CHDIR:No such file or directory`。

## 同事要看的代码

直接看这一个文件：

```text
iec_runtime.c
```

这个文件包含阿里云 MQTT 连接、服务下发接收、动态库调用、callback 上报和服务回复。真实 `iec_runtime` 集成时，可以保留里面调用 `libiot_ide.so` 的方式和 callback 映射，把 MQTT 连接、发布、订阅函数替换成 `iec_runtime` 自己已有的实现。
