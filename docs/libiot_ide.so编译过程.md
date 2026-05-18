# libiot_ide.so 编译过程

本文说明当前项目里的 `libiot_ide.so` 是怎么编译出来的。

`libiot_ide.so` 是给 Buildroot ARM64 目标机使用的 Linux 动态库。真实 `iec_runtime` 进程可以通过 C 接口调用它，实现 IDE 连接、心跳、断开、部署、启动等功能。

## 1. 相关文件

动态库对外头文件：

```text
include/iot_ide_runtime_api.h
```

动态库接口实现：

```text
src/iot_ide_runtime_api.c
```

交叉编译脚本：

```text
scripts/build-arm64-cross-windows.ps1
```

最终生成文件：

```text
build/arm64-cross/libiot_ide.so
```

## 2. Windows 上执行编译

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

构建完成后，动态库会生成到：

```text
build\arm64-cross\libiot_ide.so
```

## 3. 构建脚本做了什么

`scripts/build-arm64-cross-windows.ps1` 会先定位交叉编译器：

```text
{ToolchainRoot}\bin\gcc.exe
```

然后定位 sysroot：

```text
{ToolchainRoot}\aarch64-none-linux-gnu\libc
```

再收集项目源码：

```text
core/**/*.c
external/**/*.c
portfiles/aiot_port/**/*.c
src/**/*.c
```

这些源码里包含：

```text
src/iot_ide_runtime_api.c
src/ide_connection_manager.c
src/deploy_manager.c
src/iot_ide_app.c
src/json_utils.c
```

其中 `src/iot_ide_runtime_api.c` 负责把当前项目能力封装成 C ABI 接口，供外部 C 进程调用。

## 4. 动态库编译参数

脚本编译 `libiot_ide.so` 时，核心参数是：

```text
-fPIC
-shared
-Wl,-soname,libiot_ide.so
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

```text
-Wl,-soname,libiot_ide.so
```

设置动态库的 soname。

脚本本质上会执行类似下面的交叉编译命令：

```sh
aarch64-none-linux-gnu-gcc \
  --sysroot=... \
  -std=c11 \
  -O2 \
  -Wall \
  -fPIC \
  -shared \
  -Wl,-soname,libiot_ide.so \
  -I include \
  -I src \
  -I core \
  -I external \
  -I portfiles/aiot_port \
  core/**/*.c \
  external/**/*.c \
  portfiles/aiot_port/**/*.c \
  src/**/*.c \
  -o build/arm64-cross/libiot_ide.so \
  -lpthread \
  -ldl \
  -lm \
  -lrt
```

实际脚本里会根据 Windows 路径和工具链 sysroot 自动展开具体文件列表。

## 5. 编译输出

脚本执行成功后，会生成：

```text
build/arm64-cross/iot-ide
build/arm64-cross/libiot_ide.so
build/arm64-cross/iec_runtime
```

其中动态库是：

```text
build/arm64-cross/libiot_ide.so
```

`iot-ide` 是旧的独立设备端程序。

`iec_runtime` 是当前项目提供的生产级参考进程，用来演示真实 `iec_runtime` 如何连接阿里云并调用 `libiot_ide.so`。

## 6. 验证动态库架构

构建脚本最后会自动调用工具链里的 `readelf` 检查架构。

正常输出类似：

```text
libiot_ide.so architecture:
  Class:                             ELF64
  Machine:                           AArch64
```

这说明 `libiot_ide.so` 是 ARM64 Linux 动态库，可以放到 Buildroot ARM64 目标机上使用。

## 7. 同事集成时需要的文件

编译同事自己的 `iec_runtime` 时，需要：

```text
include/iot_ide_runtime_api.h
libiot_ide.so
```

运行时目标机需要：

```text
iec_runtime
libiot_ide.so
device_id.json
```

如果运行目录是 `/usr/iec-runtime`，建议放：

```text
/usr/iec-runtime/
  iec_runtime
  libiot_ide.so
  device_id.json
```

头文件 `include/iot_ide_runtime_api.h` 放在同事源码工程的 `include` 目录即可，运行时不需要放到目标机目录。
