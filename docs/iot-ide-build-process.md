# iot-ide 编译产物生成说明

本文档只说明一件事：

`iot-ide` 这个 ARM64 可执行文件，当前到底是怎么从源码编译出来的。

对应项目目录：

```text
E:\wulianwnag\aliyun-iot-arm
```

最终产物路径：

```text
E:\wulianwnag\aliyun-iot-arm\build\arm64-cross\iot-ide
```

## 1. 当前实际走的是哪条构建链路

当前项目里有两套构建思路：

1. `Makefile`
2. `scripts/build-arm64-cross-windows.ps1`

但你现在真正用于 Buildroot ARM64 目标机的，是第 2 套：

```text
E:\wulianwnag\aliyun-iot-arm\scripts\build-arm64-cross-windows.ps1
```

也就是说：

- 当前 ARM64 交叉编译产物 `iot-ide`，不是通过 `make` 生成的
- 当前 ARM64 交叉编译过程，不读取 `Makefile`
- 当前 ARM64 交叉编译过程，是 PowerShell 脚本自己收集源码、自己拼接 gcc 参数、自己直接链接出可执行文件

## 2. 使用的是哪一个 ARM64 编译器

你传给脚本的工具链根目录是：

```text
E:\VSCode-win32-x64-1.85.1\data\extensions\undefined_publisher.devuni-ide-vscode-0.0.1\tool\iec-runtime-gen-run\gcc-arm-10.3-aarch64-none-linux-gnu
```

脚本实际会从这个目录下面取出：

- `bin\gcc.exe`
- `bin\readelf.exe`
- `aarch64-none-linux-gnu\libc` 作为 `sysroot`

也就是说，真正参与编译和链接的 gcc 是：

```text
E:\VSCode-win32-x64-1.85.1\data\extensions\undefined_publisher.devuni-ide-vscode-0.0.1\tool\iec-runtime-gen-run\gcc-arm-10.3-aarch64-none-linux-gnu\bin\gcc.exe
```

## 3. 你执行的编译命令

在 Windows PowerShell 里执行：

```powershell
cd E:\wulianwnag\aliyun-iot-arm

$ToolchainRoot = 'E:\VSCode-win32-x64-1.85.1\data\extensions\undefined_publisher.devuni-ide-vscode-0.0.1\tool\iec-runtime-gen-run\gcc-arm-10.3-aarch64-none-linux-gnu'

.\scripts\build-arm64-cross-windows.ps1 -ToolchainRoot $ToolchainRoot
```

## 4. 脚本内部的完整流程

### 4.1 解析工具链路径

脚本先根据 `-ToolchainRoot` 解析出以下几个关键路径：

- ARM64 gcc
- ARM64 readelf
- ARM64 sysroot

其中 sysroot 路径为：

```text
E:\VSCode-win32-x64-1.85.1\data\extensions\undefined_publisher.devuni-ide-vscode-0.0.1\tool\iec-runtime-gen-run\gcc-arm-10.3-aarch64-none-linux-gnu\aarch64-none-linux-gnu\libc
```

这个 sysroot 里包含目标机侧要用到的头文件和基础运行库。

### 4.2 创建两个构建目录

脚本默认创建这两个目录：

```text
E:\wulianwnag\aliyun-iot-arm\build\arm64-cross
E:\wulianwnag\aliyun-iot-arm\build\toolchain-shim
```

用途分别是：

- `build\arm64-cross`：放最终产物 `iot-ide`
- `build\toolchain-shim`：放交叉链接时临时要用到的基础库副本

### 4.3 从 sysroot 复制基础库到 toolchain-shim

脚本会从工具链的 sysroot 中查找并复制以下库到：

```text
E:\wulianwnag\aliyun-iot-arm\build\toolchain-shim
```

复制的库包括：

- `libpthread.so`
- `libdl.so`
- `libm.so`
- `librt.so`

这个目录不是业务产物目录，而是交叉链接时使用的辅助目录。

### 4.4 手动指定头文件目录

脚本不会读取 `Makefile` 里的 `-I` 配置，而是自己指定头文件搜索路径。

当前纳入编译的头文件目录主要包括：

- 项目根目录
- `core`
- `core/sysdep`
- `core/utils`
- `external`
- `external/mbedtls/include`
- `portfiles/aiot_port`
- `src`

## 5. 哪些源码参与了编译

脚本会主动收集以下源码：

- `core` 目录下所有 `.c`
- `external` 目录下所有 `.c`
- `portfiles/aiot_port` 目录下所有 `.c`
- `src` 目录下所有 `.c`
- `demos/iot_ide_demo.c`

也就是说，当前 `iot-ide` 的构成大致是：

1. 阿里云 Link SDK 4.x 核心源码
2. TLS/mbedtls 等外部依赖源码
3. 端口适配层源码
4. 你当前项目的业务源码
5. 程序入口 `iot_ide_demo.c`

## 6. 是否先生成 libaiot.a

当前这条 ARM64 交叉编译链路，**不会**先生成 `libaiot.a`。

也就是说，当前流程不是：

1. 先编出 `libaiot.a`
2. 再用 `libaiot.a` 链接出 `iot-ide`

当前真实流程是：

1. PowerShell 脚本直接收集所有 `.c`
2. 直接调用 ARM64 gcc
3. 一次性完成编译和链接
4. 最终直接输出 `iot-ide`

所以目前你机器上看到的是：

```text
E:\wulianwnag\aliyun-iot-arm\build\arm64-cross\iot-ide
```

而不是：

```text
E:\wulianwnag\aliyun-iot-arm\output\lib\libaiot.a
```

## 7. gcc 链接时大概做了什么

脚本最终会把这些内容一起交给 ARM64 gcc：

- `--sysroot=...`
- `-std=c11`
- `-O2`
- `-Wall`
- 多个 `-I` 头文件目录
- 所有参与编译的 `.c` 文件
- `-L build\toolchain-shim`
- `-lpthread`
- `-ldl`
- `-lm`
- `-lrt`
- `-o build\arm64-cross\iot-ide`

效果就是：

- 先把各个 `.c` 编译成目标代码
- 然后立刻完成链接
- 最终输出一个可在 Buildroot ARM64 系统上运行的 ELF 可执行文件

## 8. 最终生成的文件在哪里

当前你最关心的几个路径如下：

### 8.1 最终可执行文件

```text
E:\wulianwnag\aliyun-iot-arm\build\arm64-cross\iot-ide
```

### 8.2 交叉编译辅助库目录

```text
E:\wulianwnag\aliyun-iot-arm\build\toolchain-shim
```

### 8.3 配置文件

```text
E:\wulianwnag\aliyun-iot-arm\device_id.json
```

## 9. 一句话总结

`iot-ide` 当前的生成方式是：

使用你指定的 ARM64 交叉编译器 `gcc-arm-10.3-aarch64-none-linux-gnu`，由 `scripts/build-arm64-cross-windows.ps1` 直接收集 `core + external + portfiles + src + demos/iot_ide_demo.c`，一次性编译并链接成：

```text
E:\wulianwnag\aliyun-iot-arm\build\arm64-cross\iot-ide
```

不是 `Makefile` 参与出来的，也不是先生成 `libaiot.a` 再二次链接出来的。
