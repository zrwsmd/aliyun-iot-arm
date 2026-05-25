# 给同事的 iec_runtime 动态库集成需求说明

本文档用于说明如何把当前项目提供的阿里云 IoT + 本地 IDE 远程控制能力，集成到真实的 `iec_runtime` 工控机运行进程中。

同事可以把本文档直接提供给 AI，让 AI 根据真实 `iec_runtime` 项目结构完成集成。

## 一、交付文件

当前需要提供 6 个文件：

```text
libiot_ide.so
libiot_ide_gateway.so
include/libiot_ide.h
include/libiot_ide_gateway.h
iec_runtime.c
device_id.json
```

其中：

```text
libiot_ide.so                 IDE 业务功能动态库：连接、心跳、断开、部署、启动等
libiot_ide_gateway.so         阿里云通信动态库：连接阿里云、订阅服务、解析下发、属性上报、服务回复
include/libiot_ide.h          libiot_ide.so 的 C 接口头文件
include/libiot_ide_gateway.h  libiot_ide_gateway.so 的 C 接口头文件
iec_runtime.c                 生产级集成参考代码，同事可参考/复制主流程
device_id.json                阿里云设备身份配置
```

## 二、可以直接对 AI 这样描述需求

我有一个 C 语言实现的 `iec_runtime` 工控机运行进程，需要集成一套阿里云 IoT + 本地 IDE 远程控制功能。

现在对方提供了 6 个文件：

1. `libiot_ide.so`

   IDE 业务功能动态库，负责处理 IDE 连接、断开、心跳、部署、启动等业务逻辑。

2. `libiot_ide_gateway.so`

   阿里云通信动态库，负责连接阿里云 IoT 平台、订阅物模型服务、接收阿里云下发、回复服务调用、上报属性。

3. `include/libiot_ide.h`

   `libiot_ide.so` 的 C 接口头文件。

4. `include/libiot_ide_gateway.h`

   `libiot_ide_gateway.so` 的 C 接口头文件。

5. `iec_runtime.c`

   一个生产级集成参考代码，不是必须整体替换我的 `iec_runtime`，而是用于参考如何初始化两个动态库、注册回调、分发服务、转发事件、退出释放资源。

6. `device_id.json`

   阿里云设备身份配置，包含 `productKey`、`deviceName`、`deviceSecret`、`instanceId` 或 `region` 等信息。

我希望你帮我把这套能力集成到我现有的 `iec_runtime` 项目中。

## 三、整体通信链路

本地 IDE 不直接连接工控机，而是通过阿里云物联网平台转发请求。

完整链路是：

```text
本地 IDE
-> 阿里云 IoT 物模型服务
-> 工控机上的 libiot_ide_gateway.so
-> 我的 iec_runtime 进程
-> libiot_ide.so
-> 我的 iec_runtime 进程
-> libiot_ide_gateway.so
-> 阿里云 IoT
-> 本地 IDE
```

## 四、两个动态库的职责

### 1. libiot_ide_gateway.so

`libiot_ide_gateway.so` 负责阿里云通信：

```text
读取 device_id.json
连接阿里云 MQTT
订阅 /sys/{productKey}/{deviceName}/thing/service/#
接收 requestConnect、requestDisconnect、ideHeartbeat、deployProject、startProject 等服务下发
把服务请求通过 on_service 回调给 iec_runtime
把 iec_runtime 返回的结果回复给阿里云
把 libiot_ide.so 产生的 property.post 等事件上报阿里云
```

### 2. libiot_ide.so

`libiot_ide.so` 负责 IDE 业务逻辑：

```text
requestConnect          处理 IDE 请求连接
requestDisconnect       处理 IDE 请求断开
ideHeartbeat            处理 IDE 心跳
deployProject           处理部署请求
startProject            处理启动请求
getConnectionSnapshot   查询当前 IDE 连接状态
```

## 五、真实 iec_runtime 需要集成的内容

请不要直接用提供的 `iec_runtime.c` 覆盖真实项目，而是参考它，把里面的主流程集成进真实 `iec_runtime`。

### 1. 引入头文件

```c
#include "libiot_ide.h"
#include "libiot_ide_gateway.h"
```

### 2. 初始化 libiot_ide_gateway.so

需要完成：

```text
创建 IotIdeGatewayOptions
设置 config_path，指向 device_id.json
注册 on_service 回调
注册 on_log 回调
调用 iot_ide_gateway_create()
调用 iot_ide_gateway_start()
```

注意：

```text
iot_ide_gateway_create()
只负责创建 gateway 对象、读取配置、注册回调。
不会连接阿里云。

iot_ide_gateway_start()
才是真正连接阿里云 MQTT、订阅服务。
```

### 3. 初始化 libiot_ide.so

需要完成：

```text
创建 LibIotIdeOptions
设置 work_dir
注册 on_event 回调
注册 on_log 回调
调用 libiot_ide_create()
```

### 4. 实现 gateway 的 on_service 回调

阿里云下发服务后，`libiot_ide_gateway.so` 会回调真实 `iec_runtime`。

真实 `iec_runtime` 需要根据 `service_name` 分发：

```text
requestConnect     -> libiot_ide_request_connect()
requestDisconnect  -> libiot_ide_request_disconnect()
ideHeartbeat       -> libiot_ide_heartbeat()
deployProject      -> libiot_ide_deploy_project()
startProject       -> libiot_ide_start_project()
property/get       -> libiot_ide_get_connection_snapshot()
```

然后把 `libiot_ide.so` 返回的 `response_json` 通过：

```c
iot_ide_gateway_reply_service()
```

回复给阿里云。

### 5. 实现 libiot_ide.so 的 on_event 回调

`libiot_ide.so` 内部状态变化时会回调真实 `iec_runtime`。

可能出现的事件包括：

```text
property.post
requestConnect.response
ideHeartbeat.response
requestDisconnect.response
startProject.response
deployProject.response
```

其中 `property.post` 需要通过：

```c
iot_ide_gateway_forward_event()
```

交给 `libiot_ide_gateway.so` 上报阿里云。

### 6. 保持 iec_runtime 主进程运行

只要进程在运行，gateway 内部 MQTT 收发线程就会持续工作。

进程退出时需要正确释放资源：

```c
iot_ide_gateway_stop()
libiot_ide_destroy()
iot_ide_gateway_destroy()
```

## 六、连接互斥规则

连接互斥逻辑已经在 `libiot_ide.so` 内部实现。

规则是：

```text
没有 IDE 连接 -> 接受连接
同一个 clientId 再连接 -> 认为是重连，接受
不同 clientId 再连接 -> 拒绝，返回设备已被其他 IDE 占用
```

因此不要在真实 `iec_runtime` 里重复实现连接锁，只需要把 `requestConnect` 转发给：

```c
libiot_ide_request_connect()
```

## 七、部署和启动

`deployProject` 和 `startProject` 也需要走 `libiot_ide.so`。

调用前 `libiot_ide.so` 会校验 `clientId` 是否是当前 active IDE。

如果不是当前连接 IDE，会拒绝部署或启动。

## 八、编译链接要求

真实 `iec_runtime` 是 C 项目，需要链接这两个动态库。

示例链接方式：

```bash
gcc xxx.c \
  -I./include \
  -L./lib \
  -liot_ide_gateway \
  -liot_ide \
  -o iec_runtime
```

运行时需要保证这两个 `.so` 能被找到。可以把它们放到可执行文件同目录，并设置 `rpath` 或 `LD_LIBRARY_PATH`。

## 九、部署目录建议

如果目标目录是 `/usr/iec-runtime`，建议最终运行目录类似：

```text
/usr/iec-runtime/iec_runtime
/usr/iec-runtime/libiot_ide.so
/usr/iec-runtime/libiot_ide_gateway.so
/usr/iec-runtime/device_id.json
```

头文件用于编译阶段，建议放到源码项目的 `include` 目录：

```text
include/libiot_ide.h
include/libiot_ide_gateway.h
```

## 十、最终集成目标

请根据真实 `iec_runtime` 代码结构，把上述逻辑集成进去。

要求：

```text
尽量复用现有主循环
尽量复用现有日志系统
尽量复用现有信号处理
尽量复用现有配置管理
不要破坏原有业务逻辑
不要直接用参考 iec_runtime.c 覆盖真实进程
```

提供的 `iec_runtime.c` 只是参考样例，里面已经演示了完整调用流程，请优先参考它的实现方式。
