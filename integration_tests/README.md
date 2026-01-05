# integration_tests

日期：2026-01-05  
执行者：Codex  

本目录存放 **联调/连通性测试**（与 `tests/` 单元测试分离），覆盖：

- **HSMS-SS（跨实现互通）**：`secs_lib` ↔ `secs4net`（Select + Linktest + Data 请求-响应 + SECS-II Item 编解码）
  - 优先跑 **真实 TCP**（`integration_hsms_tcp_with_secs4net`）
  - 扩展用例（真实 TCP，覆盖更多边界）：`integration_hsms_tcp_extended_with_secs4net`
    - 大 payload 回显（覆盖 framing/分片/缓冲边界）
    - 乱序应答（覆盖 system-bytes 匹配能力）
    - DeviceId 不匹配触发 `S9F1`（覆盖错误路径互通）
  - 若环境禁用 `socket()`（常见于受限沙箱），则会自动 **Skip** TCP 用例，并改用 **stdin/stdout 双向管道**承载 HSMS 帧（`integration_hsms_pipe_with_secs4net`，仍是“真实字节流”，可覆盖分包/重组逻辑）
- **SECS-I（E4，串口）**：优先使用 Linux `pty` 构造“虚拟串口线”；若环境无 `/dev/ptmx` 权限则自动降级为 `socketpair()` 字节流链路，验证 `secs_lib` 的 SECS-I 状态机/协议层可用性
  - 额外提供 `integration_secs1_pty_required`：强制要求 `pty` 可用；若不可用则会 Skip（返回码 77）

## 前置条件

- Linux（SECS-I 用例：`pty` 优先，必要时会降级为 `socketpair()`）
- `dotnet`（HSMS 用例需要构建并运行 .NET 对端 `HsmsPeer`，其内部依赖 `secs4net`）

## 构建与运行（推荐：走 CMake/CTest）

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DSECS_ENABLE_TESTS=ON -DSECS_ENABLE_INTEGRATION_TESTS=ON
cmake --build build -j
ctest --test-dir build --output-on-failure -R integration_
```

## 说明

- `third_party/secs4net/`：为适配当前沙箱“不可写外部目录”的限制，复制自 `/home/say/github_project/secs4net` 的最小源码子集（仅用于联调）。
- HSMS 用例使用 `integration_tests/hsms/dotnet_peer/Program.cs` 作为 .NET 对端：stdout 仅输出 HSMS 帧，stderr 输出诊断信息（避免污染二进制流）。
