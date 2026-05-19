# Buildroot 上运行 iec_runtime

本文说明把当前项目的 `iec_runtime + libiot_ide_gateway.so + libiot_ide.so` 放到 Buildroot 目标机上运行的完整步骤。

现在分成两层动态库：

```text
libiot_ide_gateway.so   负责连接阿里云、订阅服务、解析 JSON、上报属性、回复服务
libiot_ide.so           负责 IDE 连接锁、心跳、断开、部署、启动等业务逻辑
iec_runtime             负责把 gateway 收到的服务转发给 libiot_ide.so
```

## 1. Windows 上构建 ARM64 产物

在当前项目目录执行：

```powershell
cd E:\wulianwnag\aliyun-iot-arm

$ToolchainRoot = 'E:\VSCode-win32-x64-1.85.1\data\extensions\undefined_publisher.devuni-ide-vscode-0.0.1\tool\iec-runtime-gen-run\gcc-arm-10.3-aarch64-none-linux-gnu'

.\scripts\build-arm64-cross-windows.ps1 -ToolchainRoot $ToolchainRoot
```

构建完成后会生成：

```text
build\arm64-cross\iec_runtime
build\arm64-cross\libiot_ide.so
build\arm64-cross\libiot_ide_gateway.so
```

## 2. 在 Buildroot 上创建运行目录

登录目标机后执行：

```sh
mkdir -p /opt/iec-runtime
```

## 3. 上传运行文件

把下面四个文件上传到 `/opt/iec-runtime`：

```text
build/arm64-cross/iec_runtime
build/arm64-cross/libiot_ide.so
build/arm64-cross/libiot_ide_gateway.so
device_id.json
```

目标机目录最终应该是：

```text
/opt/iec-runtime/
  iec_runtime
  libiot_ide.so
  libiot_ide_gateway.so
  device_id.json
```

`iec_runtime`、`libiot_ide.so`、`libiot_ide_gateway.so` 建议放同一个目录。当前构建已经设置了运行时库搜索路径，程序会优先从自身目录加载这两个 `.so`。

## 4. 启动 iec_runtime

在 Buildroot 上执行：

```sh
cd /opt/iec-runtime
chmod +x ./iec_runtime
./iec_runtime ./device_id.json
```

正常启动后会看到类似日志：

```text
[gateway log:1] iot ide gateway created, productKey=xxx deviceName=xxx mqttHost=xxx.mqtt.iothub.aliyuncs.com
[iot_ide log:1] iot ide runtime created
[iot_ide event] runtime.created {"success":true}
=== iec_runtime ===
config: ./device_id.json
productKey: xxx
deviceName: xxx
mqttHost: xxx.mqtt.iothub.aliyuncs.com
MQTT connect success
[gateway log:1] AIOT_MQTTEVT_CONNECT
[gateway log:1] subscribed: /sys/{productKey}/{deviceName}/thing/service/#
[gateway log:1] suback packet id=1 max_qos=1
```

看到 `AIOT_MQTTEVT_CONNECT` 和 `subscribed`，表示 `libiot_ide_gateway.so` 已经连上阿里云，并且已经订阅服务下发。

日志里的 `heartbeat response` 是 MQTT 协议心跳，表示 `iec_runtime` 和阿里云 MQTT Broker 的连接还活着，不是 IDE 业务心跳。

## 5. 验证连接链路

本地 IDE 侧调用连接后，Buildroot 日志应出现：

```text
[gateway log:1] service=requestConnect ...
[iot_ide event] property.post {"hasIDEConnected":1,...}
[iot_ide event] requestConnect.response {"success":1,"message":"connect accepted"}
service response service=requestConnect rc=0 data={"success":1,"message":"connect accepted"}
```

这表示：

```text
本地 IDE -> 阿里云 -> libiot_ide_gateway.so -> iec_runtime -> libiot_ide.so -> iec_runtime -> libiot_ide_gateway.so -> 阿里云
```

连接链路已经跑通。

## 6. 验证心跳链路

连接成功后，本地 IDE 周期性发送 `ideHeartbeat`，Buildroot 日志应出现：

```text
[gateway log:1] service=ideHeartbeat ...
[iot_ide event] property.post {"IDEHeartbeat":"..."}
[iot_ide event] ideHeartbeat.response {"success":1,"message":"heartbeat updated"}
service response service=ideHeartbeat rc=0 data={"success":1,"message":"heartbeat updated"}
```

这表示当前 IDE 的业务心跳已经被 `libiot_ide.so` 接受并更新。

如果看到：

```text
heartbeat rejected, active client is
```

通常表示还没有先执行成功 `requestConnect`，所以动态库内部还没有 active IDE。

## 7. 验证断开连接

本地 IDE 侧调用：

```text
POST /api/connection/disconnect
```

参数：

```json
{
  "clientId": "ide-client-12345678"
}
```

Buildroot 日志应出现：

```text
[gateway log:1] service=requestDisconnect ...
[iot_ide event] property.post {"hasIDEConnected":0,"IDEInfo":"","IDEHeartbeat":"0"}
[iot_ide event] requestDisconnect.response {"success":1,"message":"disconnect accepted"}
service response service=requestDisconnect rc=0 data={"success":1,"message":"disconnect accepted"}
```

这表示当前 IDE 与 `iec_runtime` 的连接锁已经释放。

## 8. 同事正式集成时的文件位置

如果同事的运行目录是 `/usr/iec-runtime`，运行时建议放：

```text
/usr/iec-runtime/
  iec_runtime
  libiot_ide.so
  libiot_ide_gateway.so
  device_id.json
```

编译集成时还需要给同事：

```text
include/iot_ide_runtime_api.h
include/iot_ide_gateway_api.h
iec_runtime.c
```

这两个头文件是编译时用的，运行时目标机目录里不一定需要。

`iec_runtime.c` 是生产级集成参考代码，只保留两层库之间的胶水逻辑：收到 gateway 服务回调后调用 `libiot_ide.so`，收到 `libiot_ide.so` 事件后交给 gateway 上报阿里云。

## 9. 给同事的交付文件总结

给同事集成时建议提供 6 个文件：

```text
libiot_ide.so                  IDE 业务功能动态库：连接、心跳、断开、部署、启动等
libiot_ide_gateway.so          阿里云通信动态库：连接阿里云、订阅服务、解析下发、属性上报、服务回复
include/iot_ide_runtime_api.h  libiot_ide.so 的 C 接口头文件
include/iot_ide_gateway_api.h  libiot_ide_gateway.so 的 C 接口头文件
iec_runtime.c                  生产级集成参考代码，同事可参考/复制主流程
device_id.json                 阿里云设备身份配置
```
