# Buildroot 后台服务启动说明

本文档说明如何把当前项目的 `iot-ide` 做成 Buildroot 目标机上的开机服务。

对应项目目录：

```text
E:\wulianwnag\aliyun-iot-arm
```

目标机推荐部署目录：

```sh
/opt/iot-ide-release
```

服务脚本目标路径：

```sh
/etc/init.d/S99iot-ide
```

## 1. 后台运行和服务运行的区别

临时后台运行一般是：

```sh
cd /opt/iot-ide-release
nohup ./iot-ide ./device_id.json > ./logs/iot-ide.log 2>&1 &
echo $! > ./logs/iot-ide.pid
```

这种方式适合调试，但目标机重启后不会自动恢复。

做成 Buildroot 服务后，系统启动时会自动执行：

```sh
/etc/init.d/S99iot-ide start
```

并且可以用统一命令管理：

```sh
/etc/init.d/S99iot-ide start
/etc/init.d/S99iot-ide stop
/etc/init.d/S99iot-ide restart
/etc/init.d/S99iot-ide status
```

注意：下面这个 init 脚本负责开机启动和启停管理。如果程序运行中异常退出，它不会像 supervisor 那样无限自动拉起；如果后面需要崩溃自动重启，可以再加 watchdog 或 `/etc/inittab` respawn 方案。

## 2. 目标机目录准备

在 Buildroot 目标机上执行：

```sh
mkdir -p /opt/iot-ide-release
mkdir -p /opt/iot-ide-release/logs
```

把 Windows 上这两个文件上传到目标机：

```text
E:\wulianwnag\aliyun-iot-arm\build\arm64-cross\iot-ide
E:\wulianwnag\aliyun-iot-arm\device_id.json
```

目标机上最终应该是：

```sh
/opt/iot-ide-release/iot-ide
/opt/iot-ide-release/device_id.json
```

然后加执行权限：

```sh
chmod +x /opt/iot-ide-release/iot-ide
```

## 3. 创建服务脚本

在目标机上创建文件：

```sh
vi /etc/init.d/S99iot-ide
```

内容如下：

```sh
#!/bin/sh

APP_DIR="/opt/iot-ide-release"
DAEMON="$APP_DIR/iot-ide"
CONFIG="$APP_DIR/device_id.json"
LOG_DIR="$APP_DIR/logs"
LOG_FILE="$LOG_DIR/iot-ide.log"
PID_FILE="$LOG_DIR/iot-ide.pid"

is_running() {
    [ -f "$PID_FILE" ] || return 1

    pid="$(cat "$PID_FILE" 2>/dev/null)"
    [ -n "$pid" ] || return 1

    kill -0 "$pid" 2>/dev/null
}

start() {
    if is_running; then
        echo "iot-ide already running: $(cat "$PID_FILE")"
        return 0
    fi

    if [ ! -x "$DAEMON" ]; then
        echo "iot-ide executable not found or not executable: $DAEMON"
        return 1
    fi

    if [ ! -f "$CONFIG" ]; then
        echo "iot-ide config not found: $CONFIG"
        return 1
    fi

    mkdir -p "$LOG_DIR"

    cd "$APP_DIR" || return 1

    echo "starting iot-ide..."
    nohup "$DAEMON" "$CONFIG" >> "$LOG_FILE" 2>&1 &
    pid="$!"
    echo "$pid" > "$PID_FILE"

    sleep 1
    if kill -0 "$pid" 2>/dev/null; then
        echo "iot-ide started: $pid"
        return 0
    fi

    echo "iot-ide failed to start, see log: $LOG_FILE"
    rm -f "$PID_FILE"
    return 1
}

stop() {
    if ! is_running; then
        echo "iot-ide is not running"
        rm -f "$PID_FILE"
        return 0
    fi

    pid="$(cat "$PID_FILE" 2>/dev/null)"
    echo "stopping iot-ide: $pid"
    kill "$pid" 2>/dev/null

    count=0
    while kill -0 "$pid" 2>/dev/null; do
        count=$((count + 1))
        if [ "$count" -ge 10 ]; then
            echo "iot-ide did not stop in time, killing: $pid"
            kill -9 "$pid" 2>/dev/null
            break
        fi
        sleep 1
    done

    rm -f "$PID_FILE"
    echo "iot-ide stopped"
}

restart() {
    stop
    start
}

status() {
    if is_running; then
        echo "iot-ide is running: $(cat "$PID_FILE")"
        return 0
    fi

    echo "iot-ide is stopped"
    return 3
}

case "$1" in
    start)
        start
        ;;
    stop)
        stop
        ;;
    restart)
        restart
        ;;
    status)
        status
        ;;
    *)
        echo "Usage: $0 {start|stop|restart|status}"
        exit 1
        ;;
esac
```

保存后执行：

```sh
chmod +x /etc/init.d/S99iot-ide
```

## 4. 手动测试服务

先手动启动：

```sh
/etc/init.d/S99iot-ide start
```

查看状态：

```sh
/etc/init.d/S99iot-ide status
```

查看日志：

```sh
tail -f /opt/iot-ide-release/logs/iot-ide.log
```

正常情况下，日志里应该能看到类似：

```text
=== iot-ide ===
config: /opt/iot-ide-release/device_id.json
productKey: ...
deviceName: ...
mqttHost: ...
AIOT_MQTTEVT_CONNECT
```

停止服务：

```sh
/etc/init.d/S99iot-ide stop
```

重启服务：

```sh
/etc/init.d/S99iot-ide restart
```

## 5. 开机自启

Buildroot 默认会按 `/etc/init.d/S*` 顺序执行启动脚本。只要脚本放在：

```sh
/etc/init.d/S99iot-ide
```

并且有执行权限：

```sh
chmod +x /etc/init.d/S99iot-ide
```

下次目标机启动时就会自动执行：

```sh
/etc/init.d/S99iot-ide start
```

如果你的目标机根文件系统断电后会还原，记得把 `/etc/init.d/S99iot-ide` 和 `/opt/iot-ide-release` 放到持久化分区，或者合入 Buildroot rootfs 镜像。

## 6. 如果目标机需要走 Windows MQTT relay

如果目标机不能直接访问阿里云 MQTT，需要先保证 Windows relay 已经启动：

```powershell
cd E:\wulianwnag\aliyun-iot-arm
.\scripts\start-aliyun-mqtt-relay.ps1 -Background
```

目标机上也要保证 `/etc/hosts` 里有阿里云 MQTT 域名到 Windows IP 的映射。

当前项目对应的域名通常是：

```text
iot-06z00d8xy9ns00z.mqtt.iothub.aliyuncs.com
```

执行：

```sh
grep iot-06z00d8xy9ns00z.mqtt.iothub.aliyuncs.com /etc/hosts
```

如果没有映射，就先执行：

```sh
cd /opt/iot-ide-release

MQTT_HOST=iot-06z00d8xy9ns00z.mqtt.iothub.aliyuncs.com \
RELAY_IP=192.168.37.69 \
./enable-aliyun-mqtt-relay-target.sh
```

然后再启动服务：

```sh
/etc/init.d/S99iot-ide start
```

## 7. 常见排查

查看进程：

```sh
ps | grep iot-ide
```

查看 PID：

```sh
cat /opt/iot-ide-release/logs/iot-ide.pid
```

查看最近日志：

```sh
tail -100 /opt/iot-ide-release/logs/iot-ide.log
```

如果启动失败，优先检查：

- `/opt/iot-ide-release/iot-ide` 是否存在并有执行权限
- `/opt/iot-ide-release/device_id.json` 是否存在
- `device_id.json` 里的 `productKey`、`deviceName`、`deviceSecret`、`instanceId` 是否正确
- 目标机是否能访问阿里云 MQTT，或者 Windows relay 是否已经启动
- 目标机系统时间是否明显错误

