# 使用示例

> 文档更新：2026-01-19（Codex）

本目录的示例已做“降噪”整理：只保留 HSMS / SECS-I 的 server/client/loopback 示例，每种协议各两套写法（自定义请求-响应 / SMLX），并提供 C 与 C++ 的功能对应版本。

## 编译

```bash
cmake -S . -B build -DSECS_BUILD_EXAMPLES=ON
cmake --build build --target examples
```

## 主示例集合（C++ / C 一一对应）

- C++：
  - `hsms_custom` / `hsms_smlx`
  - `secs1_custom` / `secs1_smlx`
- C（C API）：
  - `c_api_hsms_custom` / `c_api_hsms_smlx`
  - `c_api_secs1_custom` / `c_api_secs1_smlx`
- 共享 SMLX 模板：`ceid_demo.sml`（构建后会复制到 `build/examples/`，便于直接运行）

所有主示例均支持 `--role <server|client|loopback>`，业务统一为：
- Host 发送 `S6F11(W=1)`：`<L[3] <U2 DATAID> <U2 CEID> <L PARAMS>>`
- Equipment 返回 `S6F12`：`<L <U2 DATAID> <U2 CEID> <L ...DATA>>`
- CEID：`0x1001..0x1004` 四种响应分支（设备状态/温度/报警/生产统计）

## 运行

### HSMS（TCP）

```bash
# custom（C++）
./build/examples/hsms_custom --role server --listen 0.0.0.0 --port 5000 --session-id 0x0001
./build/examples/hsms_custom --role client --connect 127.0.0.1 --port 5000 --session-id 0x0001
./build/examples/hsms_custom --role loopback

# smlx（C++）
./build/examples/hsms_smlx --role server --listen 0.0.0.0 --port 5000 --session-id 0x0001 --sml ceid_demo.sml
./build/examples/hsms_smlx --role client --connect 127.0.0.1 --port 5000 --session-id 0x0001 --sml ceid_demo.sml
./build/examples/hsms_smlx --role loopback --sml ceid_demo.sml

# 对应 C API（loopback 快速验证）
./build/examples/c_api_hsms_custom --role loopback
./build/examples/c_api_hsms_smlx   --role loopback --sml ceid_demo.sml
```

### SECS-I（串口 / 虚拟串口）

> SECS-I 没有“监听端口”的概念；示例里 server/client 分别代表 Equipment/Host 两端。
> 需要一对互联串口：Windows 推荐 com0com（COM5 <-> COM6）；Linux 可用 socat 创建 pty pair。

```bash
# custom（C++）
./build/examples/secs1_custom --role server --serial COM5 --baud 9600 --device-id 0x0001
./build/examples/secs1_custom --role client --serial COM6 --baud 9600 --device-id 0x0001
./build/examples/secs1_custom --role loopback --device-id 0x0001

# smlx（C++）
./build/examples/secs1_smlx --role server --serial COM5 --baud 9600 --device-id 0x0001 --sml ceid_demo.sml
./build/examples/secs1_smlx --role client --serial COM6 --baud 9600 --device-id 0x0001 --sml ceid_demo.sml
./build/examples/secs1_smlx --role loopback --device-id 0x0001 --sml ceid_demo.sml

# 对应 C API（loopback 快速验证）
./build/examples/c_api_secs1_custom --role loopback --device-id 0x0001
./build/examples/c_api_secs1_smlx   --role loopback --device-id 0x0001 --sml ceid_demo.sml
```

## Legacy（旧示例归档）

旧示例已迁移到 `examples/legacy/`，默认不再构建，避免目录噪声；如需参考可自行打开源码阅读。

## HSMS（pipe）示例（测试依赖，UNIX）

`hsms_pipe_server` / `hsms_pipe_client` 通过 stdin/stdout 传输 HSMS 帧（便于在禁用 socket 的环境联调）。
对应测试会自动启动两个示例程序并交叉管道连接：

```bash
ctest --test-dir build -R hsms_pipe_examples --output-on-failure
```

