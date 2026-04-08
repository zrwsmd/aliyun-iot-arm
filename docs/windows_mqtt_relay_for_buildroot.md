# Buildroot 目标机通过 Windows 中转访问阿里云 MQTT

本文档对应项目：

```text
E:\wulianwnag\aliyun-iot-arm
```

目标：

- 不再要求目标机整机上公网
- 不再依赖 Windows NAT 全局打通
- 只让 `iot-ide` 访问阿里云 MQTT
- 尽量不改 `iot-ide` 源码

当前这版 `device_id.json` 会生成的 MQTT 域名是：

```text
iot-06z00d8xy9ns00z.mqtt.iothub.aliyuncs.com
```

原因见源码：

- `instanceId` 非空时，`iot-ide` 会拼出 `<instanceId>.mqtt.iothub.aliyuncs.com`

## 1. 方案原理

让目标机把阿里云 MQTT 域名解析到你 Windows 电脑当前能稳定访问的地址：

```text
192.168.37.69
```

然后在 Windows 上启动一个本地 TCP 中转：

```text
192.168.37.69:8883  ->  iot-06z00d8xy9ns00z.mqtt.iothub.aliyuncs.com:8883
```

这样：

- 目标机只需要能访问 Windows
- `iot-ide` 仍然使用原始阿里云域名
- TLS 证书校验和 SNI 不需要改代码

## 2. Windows 侧脚本

新增脚本位置：

```text
E:\wulianwnag\aliyun-iot-arm\scripts\start-aliyun-mqtt-relay.ps1
E:\wulianwnag\aliyun-iot-arm\scripts\stop-aliyun-mqtt-relay.ps1
```

### 2.1 前台启动

在 Windows PowerShell 执行：

```powershell
cd E:\wulianwnag\aliyun-iot-arm

.\scripts\start-aliyun-mqtt-relay.ps1
```

默认行为：

- 读取 `device_id.json`
- 自动得到 MQTT 域名
- 监听 `192.168.37.69:8883`
- 转发到阿里云真实 MQTT 域名 `:8883`

### 2.2 后台启动

```powershell
cd E:\wulianwnag\aliyun-iot-arm

.\scripts\start-aliyun-mqtt-relay.ps1 -Background
```

默认生成：

```text
E:\wulianwnag\aliyun-iot-arm\scripts\aliyun-mqtt-relay.pid
E:\wulianwnag\aliyun-iot-arm\scripts\aliyun-mqtt-relay.log
```

### 2.3 停止中转

```powershell
cd E:\wulianwnag\aliyun-iot-arm

.\scripts\stop-aliyun-mqtt-relay.ps1
```

## 3. 目标机侧脚本

新增脚本位置：

```text
E:\wulianwnag\aliyun-iot-arm\scripts\enable-aliyun-mqtt-relay-target.sh
E:\wulianwnag\aliyun-iot-arm\scripts\disable-aliyun-mqtt-relay-target.sh
```

作用：

- 把阿里云 MQTT 域名定向到 Windows
- 不改默认路由
- 不要求目标机能直接访问公网

### 3.1 启用目标机侧域名指向

把脚本上传到目标机后执行：

```sh
chmod +x ./enable-aliyun-mqtt-relay-target.sh

MQTT_HOST=iot-06z00d8xy9ns00z.mqtt.iothub.aliyuncs.com \
RELAY_IP=192.168.37.69 \
./enable-aliyun-mqtt-relay-target.sh
```

脚本会：

- 备份 `/etc/hosts`
- 增加一条：

```text
192.168.37.69 iot-06z00d8xy9ns00z.mqtt.iothub.aliyuncs.com
```

### 3.2 关闭目标机侧域名指向

```sh
chmod +x ./disable-aliyun-mqtt-relay-target.sh

MQTT_HOST=iot-06z00d8xy9ns00z.mqtt.iothub.aliyuncs.com \
./disable-aliyun-mqtt-relay-target.sh
```

## 4. 完整使用流程

### 4.1 Windows 上启动 MQTT 中转

```powershell
cd E:\wulianwnag\aliyun-iot-arm

.\scripts\start-aliyun-mqtt-relay.ps1 -Background
```

### 4.2 目标机上启用 `/etc/hosts` 指向

```sh
cd /opt/iot-ide-release

chmod +x ./enable-aliyun-mqtt-relay-target.sh

MQTT_HOST=iot-06z00d8xy9ns00z.mqtt.iothub.aliyuncs.com \
RELAY_IP=192.168.37.69 \
./enable-aliyun-mqtt-relay-target.sh
```

### 4.3 目标机上启动 `iot-ide`

```sh
cd /opt/iot-ide-release

./iot-ide ./device_id.json
```

或者后台启动：

```sh
cd /opt/iot-ide-release

mkdir -p logs
nohup ./iot-ide ./device_id.json > ./logs/iot-ide.log 2>&1 &
echo $! > ./logs/iot-ide.pid
```

## 5. 验证方法

### 5.1 验证目标机是否命中 Windows 中转

目标机上执行：

```sh
grep 'iot-06z00d8xy9ns00z.mqtt.iothub.aliyuncs.com' /etc/hosts
ping -c 4 192.168.37.69
```

### 5.2 验证 Windows 中转是否已经监听

Windows PowerShell：

```powershell
Get-NetTCPConnection -LocalPort 8883 -State Listen
Get-Content .\scripts\aliyun-mqtt-relay.log -Tail 50
```

### 5.3 验证 `iot-ide` 是否通过中转建连

Windows 中转日志里会出现类似：

```text
accepted 192.168.37.11:xxxxx
connected 192.168.37.11:xxxxx -> <aliyun-ip>:8883
```

目标机上 `iot-ide` 前台日志正常时会看到：

```text
AIOT_MQTTEVT_CONNECT
```

如果网络短暂波动，运行中的进程可能看到：

```text
AIOT_MQTTEVT_RECONNECT
```

## 6. 适用范围和限制

这个方案只解决：

- `iot-ide` 到阿里云 MQTT 的连接

它不解决：

- 目标机整机访问公网
- Kafka 对外网 broker 的透明访问

Kafka 不建议直接套这一层透明 TCP 中转，因为 Kafka broker metadata 可能会返回公网地址给客户端，后续连接仍然会失败。

## 7. 常见问题

### 7.1 为什么这个方案不需要改 `iot-ide` 代码

因为 `iot-ide` 仍然使用原始阿里云域名：

```text
iot-06z00d8xy9ns00z.mqtt.iothub.aliyuncs.com
```

只是目标机通过 `/etc/hosts` 把这个域名先指到 Windows。

客户端 TLS 握手时使用的主机名没有变，所以证书校验逻辑仍然成立。

### 7.2 为什么不用全局 NAT

因为你当前现场网络拓扑不稳定：

- 目标机能稳定碰到 Windows 的 `192.168.37.69`
- 但不能稳定碰到我们给 NAT 设计的 `192.168.50.1`

所以更稳的做法是只中转所需服务。
