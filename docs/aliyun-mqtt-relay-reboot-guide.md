# 阿里云 MQTT 中转重启恢复说明

本文档对应项目：

```text
E:\wulianwnag\aliyun-iot-arm
```

这份文档只说明一个场景：

- 目标机不能直接连接外网
- `iot-ide` 需要借助 Windows 电脑中转访问阿里云 MQTT

不涉及 Kafka。

## 1. 用到的脚本

Windows 侧：

```text
E:\wulianwnag\aliyun-iot-arm\scripts\start-aliyun-mqtt-relay.ps1
E:\wulianwnag\aliyun-iot-arm\scripts\stop-aliyun-mqtt-relay.ps1
```

目标机侧：

```text
enable-aliyun-mqtt-relay-target.sh
disable-aliyun-mqtt-relay-target.sh
```

## 2. 这几个脚本分别是干嘛的

`start-aliyun-mqtt-relay.ps1`

- 在 Windows 上启动一个本地 TCP 中转
- 把 `192.168.37.69:8883` 转发到真正的阿里云 MQTT `:8883`

`stop-aliyun-mqtt-relay.ps1`

- 停止 Windows 上的中转进程

`enable-aliyun-mqtt-relay-target.sh`

- 在目标机上修改 `/etc/hosts`
- 把阿里云 MQTT 域名指到 Windows 地址 `192.168.37.69`

`disable-aliyun-mqtt-relay-target.sh`

- 取消目标机 `/etc/hosts` 里的这条临时映射

## 3. 正常启动顺序

如果目标机不能直接上外网，正常顺序是：

1. 在 Windows 上启动 relay
2. 在目标机上启用 `/etc/hosts` 映射
3. 在目标机上启动 `iot-ide`

对应命令如下。

Windows：

```powershell
cd E:\wulianwnag\aliyun-iot-arm
.\scripts\start-aliyun-mqtt-relay.ps1
```

目标机：

```sh
chmod +x ./enable-aliyun-mqtt-relay-target.sh

MQTT_HOST=iot-06z00d8xy9ns00z.mqtt.iothub.aliyuncs.com \
RELAY_IP=192.168.37.69 \
./enable-aliyun-mqtt-relay-target.sh
```

然后再启动：

```sh
./iot-ide ./device_id.json
```

## 4. Windows 重启后要不要重新执行

要。

因为 Windows 重启后，`start-aliyun-mqtt-relay.ps1` 启动出来的 relay 进程不会自动恢复，所以每次 Windows 重启后，都需要重新执行：

```powershell
cd E:\wulianwnag\aliyun-iot-arm
.\scripts\start-aliyun-mqtt-relay.ps1
```

## 5. 目标机要不要每次都重新执行 enable

不一定。

关键看目标机 `/etc/hosts` 里的映射还在不在。

先检查：

```sh
grep iot-06z00d8xy9ns00z.mqtt.iothub.aliyuncs.com /etc/hosts
```

如果还能看到类似这一行：

```text
192.168.37.69 iot-06z00d8xy9ns00z.mqtt.iothub.aliyuncs.com
```

说明映射还在，这次可以不用重新执行 `enable-aliyun-mqtt-relay-target.sh`。

如果没有这条映射，就需要重新执行：

```sh
MQTT_HOST=iot-06z00d8xy9ns00z.mqtt.iothub.aliyuncs.com \
RELAY_IP=192.168.37.69 \
./enable-aliyun-mqtt-relay-target.sh
```

## 6. 最稳妥的恢复流程

如果你懒得判断，最稳妥的做法就是每次按下面顺序来：

1. Windows 启动 relay
2. 目标机检查 `/etc/hosts`
3. 没有映射就执行 `enable-aliyun-mqtt-relay-target.sh`
4. 启动 `iot-ide`

## 7. 目标机重启后的处理

如果目标机也重启了，需要再次检查 `/etc/hosts`。

有些系统重启后还保留 `/etc/hosts`，有些不会保留。  
所以目标机重启后，也建议先执行：

```sh
grep iot-06z00d8xy9ns00z.mqtt.iothub.aliyuncs.com /etc/hosts
```

如果没有映射，就重新执行 `enable-aliyun-mqtt-relay-target.sh`。

## 8. 什么时候不需要这套中转

如果后面你的目标机已经能直接访问阿里云公网：

- 不需要执行 `start-aliyun-mqtt-relay.ps1`
- 不需要执行 `enable-aliyun-mqtt-relay-target.sh`
- 直接启动 `./iot-ide ./device_id.json` 即可

如果之前启用过 relay 映射，最好先在目标机执行：

```sh
MQTT_HOST=iot-06z00d8xy9ns00z.mqtt.iothub.aliyuncs.com \
./disable-aliyun-mqtt-relay-target.sh
```

这样可以避免目标机继续绕到 Windows。

## 9. 一句话结论

在“目标机不能直连外网，只能借 Windows 中转”的前提下：

- Windows 重启后，基本都要重新执行 `start-aliyun-mqtt-relay.ps1`
- 目标机不一定每次都要重新执行 `enable-aliyun-mqtt-relay-target.sh`
- 是否需要重新 `enable`，取决于 `/etc/hosts` 那条映射还在不在
