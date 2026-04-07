# iot-ide 部署与启动文档

本文档只对应项目：

```text
E:\wulianwnag\aliyun-iot-arm
```

目标程序名：

```text
iot-ide
```

建议目标机部署目录：

```sh
/opt/iot-ide-release
```

## 1. 本地要准备的文件

### 1.1 可执行文件

本地编译产物：

```text
E:\wulianwnag\aliyun-iot-arm\build\arm64-cross\iot-ide
```

### 1.2 配置文件

运行配置：

```text
E:\wulianwnag\aliyun-iot-arm\device_id.json
```

说明：
- `iot-ide` 启动时会读取 `device_id.json`
- 主设备 MQTT 建连参数、实例 ID、区域、子设备 `subDevice` 都在这里

## 2. 如果要重新编译

在 Windows 上执行：

```powershell
cd E:\wulianwnag\aliyun-iot-arm

.\scripts\build-arm64-cross-windows.ps1 `
  -ToolchainRoot 'E:\VSCode-win32-x64-1.85.1\data\extensions\undefined_publisher.devuni-ide-vscode-0.0.1\tool\iec-runtime-gen-run\gcc-arm-10.3-aarch64-none-linux-gnu'
```

成功后产物在：

```text
E:\wulianwnag\aliyun-iot-arm\build\arm64-cross\iot-ide
```

## 3. 上传到目标机

把下面两个文件上传到目标机：

```text
iot-ide
device_id.json
```

建议上传到：

```sh
/opt/iot-ide-release/
```

目标机上最终应看到：

```sh
/opt/iot-ide-release/iot-ide
/opt/iot-ide-release/device_id.json
```

## 4. 目标机初始化目录

先在目标机执行：

```sh
mkdir -p /opt/iot-ide-release
cd /opt/iot-ide-release
ls -lh
```

## 5. 增加执行权限

```sh
cd /opt/iot-ide-release
chmod +x ./iot-ide
```

## 6. 前台启动

```sh
cd /opt/iot-ide-release
./iot-ide ./device_id.json
```

正常启动后，通常会看到类似输出：

```text
=== iot-ide ===
config: ./device_id.json
productKey: ...
deviceName: ...
mqttHost: ...
```

前台停止：

```sh
Ctrl + C
```

## 7. 后台启动

```sh
cd /opt/iot-ide-release
mkdir -p logs
nohup ./iot-ide ./device_id.json > ./logs/iot-ide.log 2>&1 &
echo $! > ./logs/iot-ide.pid
```

## 8. 查看运行状态

查看 PID：

```sh
cd /opt/iot-ide-release
cat ./logs/iot-ide.pid
```

查看进程：

```sh
ps | grep iot-ide
```

查看日志：

```sh
tail -f /opt/iot-ide-release/logs/iot-ide.log
```

## 9. 停止程序

```sh
cd /opt/iot-ide-release
kill "$(cat ./logs/iot-ide.pid)"
rm -f ./logs/iot-ide.pid
```

## 10. 配置文件说明

配置文件路径：

```sh
/opt/iot-ide-release/device_id.json
```

至少需要保证主设备这几个字段正确：
- `productKey`
- `deviceName`
- `deviceSecret`
- `instanceId` 或 `region`

如果要启用网关子设备能力，可以继续配置：
- `subDevice`

示例：

```json
{
  "productKey": "主设备PK",
  "deviceName": "主设备DN",
  "deviceSecret": "主设备DS",
  "instanceId": "实例ID",
  "region": "cn-shanghai",
  "subDevice": [
    {
      "productKey": "子设备PK",
      "deviceName": "子设备DN",
      "deviceSecret": "子设备DS",
      "signMethod": "hmacsha256"
    }
  ]
}
```

## 11. 当前已支持的能力

这版 `iot-ide` 当前已经包含：
- 阿里云 IoT MQTT TLS 建连
- `requestConnect`
- `requestDisconnect`
- `ideHeartbeat`
- `deployProject`
- `startProject`
- `property/set`
- `property/get`
- 设备影子 `get / control / reported`
- 网关子设备最小链路：
  - `subDevice` 解析
  - `thing.list.found`
  - `thing.topo.get`
  - `thing.topo.add`
  - `combine/login`
  - 子设备 `property/set`
  - 子设备 `property/get`

## 12. 常见排查

### 12.1 程序起不来

先看日志：

```sh
tail -100 /opt/iot-ide-release/logs/iot-ide.log
```

如果是前台启动，直接看终端输出。

### 12.2 MQTT 连不上

优先检查：
- `device_id.json` 的 `productKey/deviceName/deviceSecret`
- `instanceId`
- 目标机网络是否能访问阿里云 IoT 平台
- 系统时间是否异常

### 12.3 目标机断电后目录没了

如果 `/opt/iot-ide-release` 断电后丢失，说明目标机该目录可能不在持久化存储上。

这时重新执行：

```sh
mkdir -p /opt/iot-ide-release
```

然后重新上传：
- `iot-ide`
- `device_id.json`

再重新启动即可。
