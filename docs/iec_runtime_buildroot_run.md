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
include/libiot_ide.h
include/libiot_ide_gateway.h
iec_runtime.c
```

这两个头文件是编译时用的，运行时目标机目录里不一定需要。

`iec_runtime.c` 是生产级集成参考代码，只保留两层库之间的胶水逻辑：收到 gateway 服务回调后调用 `libiot_ide.so`，收到 `libiot_ide.so` 事件后交给 gateway 上报阿里云。

## 9. 给同事的交付文件总结

给同事集成时建议提供 6 个文件：

```text
libiot_ide.so                  IDE 业务功能动态库：连接、心跳、断开、部署、启动等
libiot_ide_gateway.so          阿里云通信动态库：连接阿里云、订阅服务、解析下发、属性上报、服务回复
include/libiot_ide.h           libiot_ide.so 的 C 接口头文件
include/libiot_ide_gateway.h   libiot_ide_gateway.so 的 C 接口头文件
iec_runtime.c                  生产级集成参考代码，同事可参考/复制主流程
device_id.json                 阿里云设备身份配置
```

## 10. iec_runtime.c 集成时哪些需要改

`iec_runtime.c` 可以直接编译运行，用于验证阿里云链路和两个动态库的调用链路。但同事集成到真实 `iec_runtime` 进程时，不建议不加判断地整文件照搬，需要按他的进程结构做少量适配。

基本不用改的核心逻辑：

```text
创建 gateway：iot_ide_gateway_create()
启动 gateway：iot_ide_gateway_start()
创建业务库：libiot_ide_create()
注册 gateway on_service 回调
注册 libiot_ide on_event/on_log 回调
on_service 里分发 requestConnect/requestDisconnect/ideHeartbeat/deployProject/startProject
on_event 里把 libiot_ide 事件转给 gateway 上报阿里云
退出时 stop/destroy
```

这条主链路建议保留：

```text
阿里云 -> libiot_ide_gateway.so -> iec_runtime.c -> libiot_ide.so
libiot_ide.so -> iec_runtime.c -> libiot_ide_gateway.so -> 阿里云
```

同事大概率需要按真实项目修改的地方：

```text
1. main 函数集成方式
   如果真实 iec_runtime 已经有 main()、初始化流程、主循环和退出逻辑，
   需要把当前文件里的初始化、启动、清理逻辑合并进去，而不是再复制一个 main()。

2. 配置文件路径
   当前默认从命令行读取 device_id.json：
   const char *config_path = argc > 1 ? argv[1] : "./device_id.json";
   真实项目可以改成固定路径，或从自己的配置系统读取。

3. 工作目录
   当前是：
   iot_ide_options.work_dir = ".";
   目前该字段只是保存到 libiot_ide.so 内部上下文，作为后续扩展预留。
   现有部署/启动实际使用阿里云下发参数里的 deployPath/projectName，
   不会自动以 work_dir 作为部署或启动基准目录。

4. 日志系统
   当前直接输出到 stdout/stderr。
   如果真实 iec_runtime 有自己的日志模块，需要替换成项目日志接口。

5. 主循环和退出信号
   当前使用 SIGINT/SIGTERM + while sleep 保持运行。
   如果真实 iec_runtime 已有事件循环、线程模型或守护进程框架，需要并入现有框架。

6. 部署/启动与真实业务衔接
   如果部署和启动完全由 libiot_ide.so 处理，可以保持当前分发逻辑。
   如果还需要通知 PLC runtime、任务调度器或进程管理器，需要在 deployProject/startProject 分支前后加真实业务逻辑。
```

一句话：当前 `iec_runtime.c` 可以直接用于跑通验证；正式接入同事项目时，建议保留回调注册和服务分发主链路，主要改进程入口、配置路径、工作目录、日志、主循环退出方式，以及部署/启动和真实业务的衔接。
