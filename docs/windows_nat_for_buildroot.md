# Windows NAT 让 Buildroot 目标机临时访问外网

本文档记录这次已经验证通过的一套方案：

- Windows 主机自己可以上网
- Buildroot 目标机只有一根线连到 Windows
- 不改掉当前 FinalShell 管理地址
- 通过 Windows NAT，让目标机临时访问外网

这套方案适用于：

- `iot-ide`
- `c-kafka-trace-producer`
- 其他需要目标机临时访问公网的程序

## 1. 本次网络拓扑

Windows 侧：

- 上网网卡：`以太网`
  - `192.168.1.69/24`
- 连接目标机的网卡：`以太网 2`
  - 原管理地址：`192.168.37.25/24`
  - 新增 NAT 地址：`192.168.50.1/24`

Buildroot 目标机侧：

- 管理地址：`eth3 = 192.168.37.11/24`
- 新增 NAT 地址：`eth3:1 = 192.168.50.2/24`

说明：

- FinalShell 继续连接 `192.168.37.11`
- 外网流量通过 `192.168.50.1 -> Windows NAT -> 互联网`

## 2. 方案特点

优点：

- 不需要把 Windows 网卡改成 `192.168.137.1`
- 不需要启用 ICS 共享
- 不会把当前 FinalShell 连接直接切断
- 可以保留原来的本地调试网段 `192.168.37.0/24`

缺点：

- 需要管理员 PowerShell
- 需要在 Windows 上创建 NAT 规则

## 3. Windows 侧配置

### 3.1 保留原管理地址

`以太网 2` 保持原来的本地管理地址：

```text
IP 地址：192.168.37.25
子网掩码：255.255.255.0
默认网关：留空
DNS：留空
```

注意：

- 不要给 `以太网 2` 填默认网关
- 目标机管理链路还是靠 `192.168.37.25 <-> 192.168.37.11`

### 3.2 管理员 PowerShell 增加 NAT 内网地址

```powershell
New-NetIPAddress -InterfaceAlias "以太网 2" -IPAddress 192.168.50.1 -PrefixLength 24
```

如果提示已存在，可以跳过。

### 3.3 打开转发

```powershell
Set-NetIPInterface -InterfaceAlias "以太网 2" -Forwarding Enabled
Set-NetIPInterface -InterfaceAlias "以太网" -Forwarding Enabled
```

这里：

- `以太网 2` 是连目标机的
- `以太网` 是 Windows 自己上网的那张

### 3.4 创建 NAT

```powershell
New-NetNat -Name "BuildrootNat" -InternalIPInterfaceAddressPrefix 192.168.50.0/24
```

查看 NAT：

```powershell
Get-NetNat
```

## 4. 目标机侧配置

下面命令在目标机上执行。

### 4.1 保留原管理地址

不要动原来的：

```text
eth3 = 192.168.37.11
```

### 4.2 新增 NAT 地址

```sh
ifconfig eth3:1 192.168.50.2 netmask 255.255.255.0 up
```

查看：

```sh
ifconfig eth3
ifconfig eth3:1
```

### 4.3 测试到 Windows NAT 地址

```sh
ping -c 4 192.168.50.1
```

如果这里不通，先不要继续改默认路由。

### 4.4 把默认路由切到 Windows NAT

```sh
route del default
route add default gw 192.168.50.1 eth3
echo 'nameserver 223.5.5.5' > /etc/resolv.conf
echo 'nameserver 8.8.8.8' >> /etc/resolv.conf
```

查看：

```sh
route -n
cat /etc/resolv.conf
```

## 5. 验证外网是否打通

在目标机执行：

```sh
ping -c 4 192.168.50.1
ping -c 4 223.5.5.5
ping -c 4 iot-06z00d8xy9ns00z.mqtt.iothub.aliyuncs.com
```

本次实际验证结果：

- `192.168.50.1` 可达
- `223.5.5.5` 可达
- `iot-06z00d8xy9ns00z.mqtt.iothub.aliyuncs.com` 可达

说明：

- 目标机已经可以通过 Windows NAT 访问外网
- DNS 也已经正常

补充：

- `47.129.128.147` 的 `ping` 不通，不代表 Kafka 不通
- 该服务器可能禁了 ICMP
- 要用 TCP 方式测 Kafka 端口

例如：

```sh
telnet 47.129.128.147 9092
```

如果看到：

```text
Connected to 47.129.128.147
```

就说明 `9092` 端口是通的。

## 6. 程序如何使用

网络打通后，FinalShell 仍然继续连：

```text
192.168.37.11
```

不需要改到 `192.168.50.2`。

### 6.1 启动 iot-ide

```sh
cd /opt/iot-ide-release
./iot-ide ./device_id.json
```

### 6.2 启动 Kafka 程序

按你原来的 Kafka 文档启动即可。

## 7. 如何禁用 / 回滚

### 7.1 目标机回滚

如果不想让目标机继续通过 Windows 上外网，可以在目标机执行：

```sh
route del default
ifconfig eth3:1 down
```

如果你原来有其他默认网关，再按原来的网关补回去。

管理链路 `192.168.37.11` 仍然保留。

### 7.2 Windows 回滚

管理员 PowerShell 执行：

```powershell
Remove-NetNat -Name "BuildrootNat"
Remove-NetIPAddress -InterfaceAlias "以太网 2" -IPAddress 192.168.50.1 -Confirm:$false
Set-NetIPInterface -InterfaceAlias "以太网 2" -Forwarding Disabled
Set-NetIPInterface -InterfaceAlias "以太网" -Forwarding Disabled
```

然后确认：

```powershell
Get-NetNat
Get-NetIPAddress -InterfaceAlias "以太网 2"
```

正常情况下：

- `BuildrootNat` 不再存在
- `以太网 2` 只保留原来的 `192.168.37.25`

## 8. 常见问题

### 8.1 为什么不用 ICS 共享

因为 ICS 往往会：

- 强制把 Windows 网卡改成 `192.168.137.1`
- 中途很容易把当前 SSH/FinalShell 断掉

而本次 NAT 方案可以保留：

- Windows：`192.168.37.25`
- 目标机：`192.168.37.11`

所以更稳。

### 8.2 本地开了 v2rayN 会不会影响

可能会。

如果发现：

- `192.168.50.1` 能通
- `223.5.5.5` 不通

建议先临时关闭：

- `v2rayN`
- TUN 模式
- 系统代理

再重新测试。

### 8.3 telnet 怎么退出

如果 `telnet 47.129.128.147 9092` 进入连接状态：

1. 按 `Ctrl + ]`
2. 再按 `e`

即可退出。
