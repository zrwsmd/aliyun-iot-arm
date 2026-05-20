# libiot_ide.so 编译过程

本文说明当前项目里的两个动态库是怎么编译出来的：

```text
libiot_ide.so           IDE 业务动态库
libiot_ide_gateway.so   阿里云通信动态库
```

## 1. 两个动态库的职责

`libiot_ide.so` 负责 IDE 业务逻辑：

```text
requestConnect
requestDisconnect
ideHeartbeat
deployProject
startProject
connection snapshot
```

`libiot_ide_gateway.so` 负责阿里云通信逻辑：

```text
读取 device_id.json
连接阿里云 MQTT
订阅 /sys/{productKey}/{deviceName}/thing/service/#
解析阿里云服务下发 JSON
把 serviceName/requestId/params 回调给 iec_runtime
上报 property.post
回复 service_reply
发布 trace
```

## 2. 相关文件

IDE 业务动态库对外头文件：

```text
include/libiot_ide.h
```

IDE 业务动态库接口实现：

```text
src/libiot_ide.c
```

阿里云通信动态库对外头文件：

```text
include/libiot_ide_gateway.h
```

阿里云通信动态库接口实现：

```text
src/libiot_ide_gateway.c
```

生产集成参考入口：

```text
iec_runtime.c
```

交叉编译脚本：

```text
scripts/build-arm64-cross-windows.ps1
```

## 3. Windows 上执行编译

在 Windows PowerShell 里进入当前项目目录：

```powershell
cd E:\wulianwnag\aliyun-iot-arm
```

设置 ARM64 交叉编译工具链路径：

```powershell
$ToolchainRoot = 'E:\VSCode-win32-x64-1.85.1\data\extensions\undefined_publisher.devuni-ide-vscode-0.0.1\tool\iec-runtime-gen-run\gcc-arm-10.3-aarch64-none-linux-gnu'
```

执行构建脚本：

```powershell
.\scripts\build-arm64-cross-windows.ps1 -ToolchainRoot $ToolchainRoot
```

构建完成后会生成：

```text
build\arm64-cross\libiot_ide.so
build\arm64-cross\libiot_ide_gateway.so
build\arm64-cross\iec_runtime
```

## 4. 构建脚本做了什么

`scripts/build-arm64-cross-windows.ps1` 会定位：

```text
{ToolchainRoot}\bin\gcc.exe
{ToolchainRoot}\aarch64-none-linux-gnu\libc
```

然后收集源码并分成几组：

```text
core/**/*.c                 阿里云 C SDK
external/**/*.c             mbedtls 等依赖
portfiles/aiot_port/**/*.c  SDK 移植层
src/**/*.c                  当前项目业务源码
```

`libiot_ide.so` 使用当前项目业务源码，排除 gateway 接口实现：

```text
src/libiot_ide.c
src/ide_connection_manager.c
src/deploy_manager.c
src/start_manager.c
src/iot_ide_app.c
src/json_utils.c
...
```

`libiot_ide_gateway.so` 使用阿里云 C SDK 和通信相关源码：

```text
core/**/*.c
external/**/*.c
portfiles/aiot_port/**/*.c
src/device_config.c
src/json_utils.c
src/libiot_ide_gateway.c
```

`iec_runtime` 只编译：

```text
iec_runtime.c
```

并链接：

```text
libiot_ide_gateway.so
libiot_ide.so
```

## 5. 动态库编译参数

两个动态库都会使用：

```text
-fPIC
-shared
```

含义：

```text
-fPIC
```

生成位置无关代码，动态库需要这个参数。

```text
-shared
```

告诉编译器输出 `.so` 动态库。

`libiot_ide.so` 设置：

```text
-Wl,-soname,libiot_ide.so
```

`libiot_ide_gateway.so` 设置：

```text
-Wl,-soname,libiot_ide_gateway.so
```

## 6. 编译输出

脚本执行成功后，会生成：

```text
build/arm64-cross/iot-ide
build/arm64-cross/libiot_ide.so
build/arm64-cross/libiot_ide_gateway.so
build/arm64-cross/iec_runtime
```

其中：

```text
iot-ide                 旧的独立设备端程序
libiot_ide.so           IDE 业务动态库
libiot_ide_gateway.so   阿里云通信动态库
iec_runtime             两层动态库的生产集成参考进程
```

## 7. 验证架构

构建脚本最后会自动调用工具链里的 `readelf` 检查架构。

正常输出类似：

```text
libiot_ide.so architecture:
  Class:                             ELF64
  Machine:                           AArch64

libiot_ide_gateway.so architecture:
  Class:                             ELF64
  Machine:                           AArch64

iec_runtime architecture:
  Class:                             ELF64
  Machine:                           AArch64
```

这说明两个 `.so` 和 `iec_runtime` 都是 ARM64 Linux 产物，可以放到 Buildroot ARM64 目标机上使用。

## 8. 同事集成时需要的文件

编译同事自己的 `iec_runtime` 时，需要：

```text
include/libiot_ide.h
include/libiot_ide_gateway.h
libiot_ide.so
libiot_ide_gateway.so
iec_runtime.c
```

运行时目标机需要：

```text
iec_runtime
libiot_ide.so
libiot_ide_gateway.so
device_id.json
```

如果运行目录是 `/usr/iec-runtime`，建议放：

```text
/usr/iec-runtime/
  iec_runtime
  libiot_ide.so
  libiot_ide_gateway.so
  device_id.json
```

两个头文件放在同事源码工程的 `include` 目录即可，运行时不需要放到目标机目录。
