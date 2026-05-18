# iot-ide

This directory contains the formal Aliyun IoT C device-side project for your Buildroot ARM64 target.

What it currently includes:
- MQTT device connect over TLS using `device_id.json`
- Thing service handling for `requestConnect`, `requestDisconnect`, `ideHeartbeat`, `deployProject`, and `startProject`
- Property handling for `property/set` and `property/get`
- Device shadow sync: startup `get`, downstream `control`, and reported-state update
- Gateway sub-device support: `subDevice` config parsing, topo query/report, topo add, and sub-device login
- Sub-device property proxy handling for `property/set` and `property/get`
- IDE connection lock state reporting with `hasIDEConnected`, `IDEInfo`, and `IDEHeartbeat`
- Async deploy execution and project start execution

Current scope:
- this is the formal C port workspace for the Java IoT IDE device project
- `TraceSimulator` is intentionally not ported here for now because that data path is already handled by the Kafka project

Important:
- device-side MQTT authentication uses `productKey`, `deviceName`, `deviceSecret`, and `instanceId` from `device_id.json`
- copy `device_id.example.json` to `device_id.json` locally and fill in your real device credentials
- `subDevice` is optional; if configured, the current C port will try to complete gateway topo sync and sub-device online flow
- the current gateway signing implementation supports `hmacSha256` / `hmacsha256`
- `deployProject` needs `curl` or `wget` on the target
- ZIP extraction needs `unzip` or `busybox unzip` on the target

## Files

- `device_id.example.json`: template for local device credentials and instance config
- `iec_runtime.c`: production C entry that glues `libiot_ide_gateway.so` service callbacks to `libiot_ide.so`
- `include/iot_ide_runtime_api.h`: C API for the IDE business dynamic library
- `include/iot_ide_gateway_api.h`: C API for the Aliyun gateway dynamic library
- `demos/iot_ide_demo.c`: program entry
- `src/`: formal device-side C modules
- `scripts/build-arm64-cross-windows.ps1`: Windows cross-build script for your ARM64 toolchain
- `docs/buildroot_service.md`: Buildroot init service setup for auto-starting `iot-ide`
- `docs/iec_runtime_buildroot_run.md`: Buildroot run steps for `iec_runtime + libiot_ide_gateway.so + libiot_ide.so`
- `docs/iec_runtime.c功能说明.md`: explains what `iec_runtime.c` does in the two-library integration
- `docs/libiot_ide.so编译过程.md`: build process for `libiot_ide.so` and `libiot_ide_gateway.so`
- `docs/four_party_call_flow.md`: flow notes for local IDE, Aliyun, `iec_runtime`, and the two dynamic libraries

## Build on Windows for ARM64

```powershell
cd E:\wulianwnag\aliyun-iot-arm

$ToolchainRoot = 'E:\VSCode-win32-x64-1.85.1\data\extensions\undefined_publisher.devuni-ide-vscode-0.0.1\tool\iec-runtime-gen-run\gcc-arm-10.3-aarch64-none-linux-gnu'

.\scripts\build-arm64-cross-windows.ps1 -ToolchainRoot $ToolchainRoot
```

The output binary will be generated at:

```text
build\arm64-cross\iot-ide
```

The same build also generates the dynamic libraries and `iec_runtime` entry:

```text
build\arm64-cross\libiot_ide.so
build\arm64-cross\libiot_ide_gateway.so
build\arm64-cross\iec_runtime
```

## Run on the Buildroot target

Upload these two files to the target:
- `build/arm64-cross/iot-ide`
- `device_id.json`

Prepare the local runtime config first:

```powershell
Copy-Item .\device_id.example.json .\device_id.json
```

Run:

```sh
chmod +x ./iot-ide
./iot-ide ./device_id.json
```
