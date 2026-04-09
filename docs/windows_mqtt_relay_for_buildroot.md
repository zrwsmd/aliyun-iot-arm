# Buildroot 目标机通过 Windows 中转访问阿里云 MQTT

本文档对应项目：

```text
E:\wulianwnag\aliyun-iot-arm
```

本文档只讲一件事：

- 当目标机不能直连外网时，如何借助 Windows 电脑中转，让 `iot-ide` 访问阿里云 MQTT

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

## 2. 用到的脚本

Windows 侧脚本：

```text
E:\wulianwnag\aliyun-iot-arm\scripts\start-aliyun-mqtt-relay.ps1
E:\wulianwnag\aliyun-iot-arm\scripts\stop-aliyun-mqtt-relay.ps1
```

目标机侧脚本：

```text
E:\wulianwnag\aliyun-iot-arm\scripts\enable-aliyun-mqtt-relay-target.sh
E:\wulianwnag\aliyun-iot-arm\scripts\disable-aliyun-mqtt-relay-target.sh
```

各自作用：

- `start-aliyun-mqtt-relay.ps1`
  在 Windows 上启动本地 TCP 中转，把 `192.168.37.69:8883` 转发到真实阿里云 MQTT `:8883`
- `stop-aliyun-mqtt-relay.ps1`
  停止 Windows 上的中转进程
- `enable-aliyun-mqtt-relay-target.sh`
  在目标机修改 `/etc/hosts`，把阿里云 MQTT 域名指到 Windows 的 `192.168.37.69`
- `disable-aliyun-mqtt-relay-target.sh`
  取消目标机 `/etc/hosts` 里的这条临时映射

## 3. 启动方式

### 3.1 Windows 上启动 MQTT 中转

前台启动：

```powershell
cd E:\wulianwnag\aliyun-iot-arm

.\scripts\start-aliyun-mqtt-relay.ps1
```

后台启动：

```powershell
cd E:\wulianwnag\aliyun-iot-arm

.\scripts\start-aliyun-mqtt-relay.ps1 -Background
```

默认行为：

- 读取 `device_id.json`
- 自动得到 MQTT 域名
- 监听 `192.168.37.69:8883`
- 转发到阿里云真实 MQTT 域名 `:8883`

后台模式默认生成：

```text
E:\wulianwnag\aliyun-iot-arm\scripts\aliyun-mqtt-relay.pid
E:\wulianwnag\aliyun-iot-arm\scripts\aliyun-mqtt-relay.log
```

停止中转：

```powershell
cd E:\wulianwnag\aliyun-iot-arm

.\scripts\stop-aliyun-mqtt-relay.ps1
```

### 3.2 目标机上启用域名指向

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

关闭目标机侧域名指向：

```sh
chmod +x ./disable-aliyun-mqtt-relay-target.sh

MQTT_HOST=iot-06z00d8xy9ns00z.mqtt.iothub.aliyuncs.com \
./disable-aliyun-mqtt-relay-target.sh
```

### 3.3 启动 `iot-ide`

前台启动：

```sh
cd /opt/iot-ide-release

./iot-ide ./device_id.json
```

后台启动：

```sh
cd /opt/iot-ide-release

mkdir -p logs
nohup ./iot-ide ./device_id.json > ./logs/iot-ide.log 2>&1 &
echo $! > ./logs/iot-ide.pid
```

## 4. 完整使用流程

### 4.1 Windows 上启动 relay

```powershell
cd E:\wulianwnag\aliyun-iot-arm

.\scripts\start-aliyun-mqtt-relay.ps1 -Background
```

### 4.2 目标机上启用 `/etc/hosts` 映射

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

## 6. 重启后的恢复流程

### 6.1 Windows 重启后

Windows 重启后，relay 进程不会自动恢复，所以需要重新执行：

```powershell
cd E:\wulianwnag\aliyun-iot-arm

.\scripts\start-aliyun-mqtt-relay.ps1
```

如果你平时就是后台方式运行，也可以继续用：

```powershell
.\scripts\start-aliyun-mqtt-relay.ps1 -Background
```

### 6.2 目标机重启后

目标机重启后，不一定每次都要重新执行 `enable-aliyun-mqtt-relay-target.sh`，关键看 `/etc/hosts` 里的映射是否还在。

先检查：

```sh
grep iot-06z00d8xy9ns00z.mqtt.iothub.aliyuncs.com /etc/hosts
```

如果还能看到类似这一行：

```text
192.168.37.69 iot-06z00d8xy9ns00z.mqtt.iothub.aliyuncs.com
```

说明映射还在，这次可以不重新执行 `enable-aliyun-mqtt-relay-target.sh`。

如果没有这条映射，就重新执行：

```sh
MQTT_HOST=iot-06z00d8xy9ns00z.mqtt.iothub.aliyuncs.com \
RELAY_IP=192.168.37.69 \
./enable-aliyun-mqtt-relay-target.sh
```

### 6.3 最稳妥的恢复顺序

如果你懒得判断，最稳妥的恢复顺序就是：

1. Windows 上启动 relay
2. 目标机检查 `/etc/hosts`
3. 如果没有映射，就执行 `enable-aliyun-mqtt-relay-target.sh`
4. 启动 `iot-ide`

## 7. 什么时候不再需要这套 relay

如果后续目标机已经能直接访问阿里云公网：

- 不需要执行 `start-aliyun-mqtt-relay.ps1`
- 不需要执行 `enable-aliyun-mqtt-relay-target.sh`
- 直接启动 `./iot-ide ./device_id.json` 即可

如果之前启用过 relay 映射，最好先在目标机执行：

```sh
MQTT_HOST=iot-06z00d8xy9ns00z.mqtt.iothub.aliyuncs.com \
./disable-aliyun-mqtt-relay-target.sh
```

这样可以避免目标机继续绕到 Windows。

## 8. 适用范围和限制

这个方案只解决：

- `iot-ide` 到阿里云 MQTT 的连接

它不解决：

- 目标机整机访问公网
- Kafka 对外网 broker 的透明访问

Kafka 不建议直接套这一层透明 TCP 中转，因为 Kafka broker metadata 可能会返回公网地址给客户端，后续连接仍然会失败。

## 9. 常见问题

### 9.1 为什么这个方案不需要改 `iot-ide` 代码

因为 `iot-ide` 仍然使用原始阿里云域名：

```text
iot-06z00d8xy9ns00z.mqtt.iothub.aliyuncs.com
```

只是目标机通过 `/etc/hosts` 把这个域名先指到 Windows。

客户端 TLS 握手时使用的主机名没有变，所以证书校验逻辑仍然成立。

### 9.2 为什么不用全局 NAT

因为你当前现场网络拓扑不稳定：

- 目标机能稳定碰到 Windows 的 `192.168.37.69`
- 但不能稳定碰到我们给 NAT 设计的 `192.168.50.1`

所以更稳的做法是只中转所需服务。
