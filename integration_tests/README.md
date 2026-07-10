# integration_tests

日期：2026-01-19  
执行者：Codex  

本目录存放 **联调/连通性测试**（与 `tests/` 单元测试分离），覆盖：

- **HSMS-SS（跨实现互通）**：`secs_lib` ↔ `secs4net`（Select + Linktest + Data 请求-响应 + SECS-II Item 编解码）
  - 优先跑 **真实 TCP**（`integration_hsms_tcp_with_secs4net`）
  - 扩展用例（真实 TCP，覆盖更多边界）：`integration_hsms_tcp_extended_with_secs4net`
    - 大 payload 回显（覆盖 framing/分片/缓冲边界）
    - 乱序应答（覆盖 system-bytes 匹配能力）
    - DeviceId 不匹配触发 `S9F1`（覆盖错误路径互通）
  - 若环境禁用 `socket()`（常见于受限沙箱），则会自动 **Skip** TCP 用例，并改用 **stdin/stdout 双向管道**承载 HSMS 帧（`integration_hsms_pipe_with_secs4net`，仍是“真实字节流”，可覆盖分包/重组逻辑）
- **HSMS-SS（纯 C++ loopback）**：不依赖 dotnet，用于在“单机/同实现”场景下做连通性与临界测试
  - `integration_hsms_tcp_loopback`：真实 TCP（127.0.0.1:0），覆盖 SELECT/LINKTEST、乱序应答、T3 超时 + late response、SEPARATE 断线收敛、inbound 队列溢出断线、pending 上限（非致命）、stop 中断挂起事务（cancelled 收敛）、auto_reconnect（主动/被动重连）
    - 若环境禁用 `socket()` 会返回 77 并由 CTest 标记为 Skip
  - `integration_hsms_pipe_loopback`：pipe 全双工字节流（无 socket 依赖），覆盖 SELECT/LINKTEST、乱序应答、T3 超时 + late response、SEPARATE 断线收敛、inbound 队列溢出断线、pending 上限（非致命）、stop 中断挂起事务（cancelled 收敛）
- **SECS-I（E4，串口）**：优先使用 Linux `pty` 构造“虚拟串口线”；若环境无 `/dev/ptmx` 权限则自动降级为 `socketpair()` 字节流链路，验证 `secs_lib` 的 SECS-I 状态机/协议层可用性
  - 额外提供 `integration_secs1_pty_required`：强制要求 `pty` 可用；若不可用则会 Skip（返回码 77）
- **RPC（Rust 跨语言互通）**：使用 `tonic + prost` 从仓库 proto2 契约生成客户端，真实启动 `secs-rpc-server` 与 HSMS peer
  - 覆盖 Library/Session/Messaging 三个 service
  - 覆盖严格 `RpcStatus`、生命周期、全部 SECS-II Item 类型、应用超时、tonic deadline、断连、并发、pending 上限和停止竞争
  - tonic 0.14 的客户端本地 deadline 到期表现为 `Code::Cancelled` 与 `Timeout expired`；服务端应用超时则通过非 OK `RpcStatus` 返回

## 前置条件

- Linux（SECS-I 用例：`pty` 优先，必要时会降级为 `socketpair()`）
- `dotnet`（HSMS 用例需要构建并运行 .NET 对端 `HsmsPeer`，其内部依赖 `secs4net`）
- `cargo`（Rust RPC 互操作用例）
- RPC 用例还需要配置 `SECS_ENABLE_RPC=ON`、`SECS_ENABLE_TESTS=ON` 和 `SECS_BUILD_TOOLS=ON`

## 构建与运行（推荐：走 CMake/CTest）

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DSECS_ENABLE_TESTS=ON -DSECS_ENABLE_INTEGRATION_TESTS=ON
cmake --build build -j
ctest --test-dir build --output-on-failure -R integration_
```

Rust RPC 互操作测试：

```bash
cmake -S . -B build_rpc \
  -DSECS_ENABLE_RPC=ON \
  -DSECS_ENABLE_TESTS=ON \
  -DSECS_ENABLE_INTEGRATION_TESTS=ON \
  -DSECS_BUILD_TOOLS=ON
cmake --build build_rpc -j
ctest --test-dir build_rpc --output-on-failure -R rpc_rust_tonic_interop
```

直接运行 Rust 客户端：

```bash
CARGO_HOME=/tmp/secs_rpc_cargo_home \
cargo run --locked --manifest-path \
  integration_tests/rpc/rust_tonic_client/Cargo.toml -- \
  --server build_rpc/tools/secs-rpc-server \
  --peer build_rpc/tests/test_rpc_hsms_peer
```

## 说明

- `third_party/secs4net/`：为适配当前沙箱“不可写外部目录”的限制，复制自 `/home/say/github_project/secs4net` 的最小源码子集（仅用于联调）。
- HSMS 用例使用 `integration_tests/hsms/dotnet_peer/Program.cs` 作为 .NET 对端：stdout 仅输出 HSMS 帧，stderr 输出诊断信息（避免污染二进制流）。
