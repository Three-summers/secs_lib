# Rust tonic RPC 多维测试设计

## 目标

用真实 `tonic + prost` 客户端验证 `secs-rpc-server` 的 gRPC/HTTP2 兼容性、RPC 契约、Session 生命周期、HSMS 消息收发、错误语义、超时与并发行为。测试必须验证实际失败原因，不能仅凭“返回了某种错误”判定通过。

本设计先增加测试和测试编排。只有测试暴露服务端缺陷时，才根据明确的失败用例修改生产代码。

## 放置与启用条件

新增独立 Cargo 工程：

```text
integration_tests/rpc/rust_tonic_client/
|-- Cargo.toml
|-- Cargo.lock
|-- build.rs
`-- src/main.rs
```

修改 `tests/test_rpc_hsms_peer.cpp`，增加仅供联调使用的可选参数：延迟 S1F1 回复、丢弃指定 stream/function 的请求。默认行为保持不变，因此现有 Python 互操作测试不受影响。

修改 `integration_tests/CMakeLists.txt` 注册 CTest 用例。测试仅在以下条件同时满足时启用：

- `SECS_ENABLE_RPC=ON`
- `SECS_ENABLE_INTEGRATION_TESTS=ON`
- 找到 `cargo`
- 已构建 `secs-rpc-server` 和 `test_rpc_hsms_peer`

普通 C++ 构建和未启用联调测试的构建不执行 Cargo。

## 进程与数据流

```text
Rust test process
  |-- spawn --> secs-rpc-server
  |                |
  |  tonic/h2      | SessionService creates active HSMS session
  +--------------->|
  |                +---------------- TCP/HSMS ----------------+
  |                                                       test_rpc_hsms_peer
  |  LibraryService / SessionService / MessagingService       |
  +<----------------------------------------------------------+
```

Rust 测试进程负责选择本地空闲端口、启动和回收两个子进程、等待 gRPC channel ready、创建生成的三个 service client，并在任何失败路径中终止子进程和输出其日志。

`build.rs` 直接编译仓库中的 `src/rpc/proto/secs/rpc/v1/secs_rpc.proto`。这保证 Rust 测试使用与服务端相同的 proto2 契约，并实际覆盖 `prost` 对 optional 字段和枚举的生成结果。

## 断言规则

所有应用层成功响应必须同时满足：

- gRPC 调用本身成功；
- `status` 字段存在；
- `status.ok == true`；
- 成功场景要求的 payload 字段存在且内容正确。

所有应用层错误响应必须同时满足：

- gRPC 调用本身成功，除非场景明确测试 transport/deadline；
- `status` 字段存在；
- `status.ok == false`；
- `status.error` 字段存在；
- `category`、`value` 或 `message` 与目标失败原因一致。

错误场景不得只断言“不是成功”。例如非法 message 的测试必须先创建一个真实存在的 session，使服务端越过 session lookup，再验证 `message is required` 或对应的 stream/function 校验错误。

## 测试场景

### 1. Rust 代码生成与连接

- Cargo 使用固定版本的 `tonic`、`prost`、`tokio` 和构建依赖，并提交 `Cargo.lock`。
- 连接 `secs-rpc-server` 的标准 HTTP/2 endpoint。
- 调用 `GetLibraryInfo`，验证版本非空、支持 `HSMS`、`SECS-I`、`session-service-v1`、`messaging-service-v1` 和 `grpc-compatible-protocol`。

### 2. Session 生命周期

- 创建带名称、HSMS transport 和 runtime 配置的 session。
- 验证生成的 session ID、初始状态、transport 和 runtime 回显。
- 使用 ID 执行 `GetSession` 和 `ListSessions`。
- 启动 session，轮询到 `selected_generation > 0`。
- 重复 `StartSession`，验证幂等成功且 ID 不变。
- 停止 session，验证 stopped 状态。
- 重复 `StopSession`，验证幂等成功。
- 删除 session，随后 `GetSession` 必须返回明确的 `session not found` 应用错误。

### 3. 真实 HSMS 消息

- `Send` 发送 S1F3 ASCII Item，并验证 accepted envelope 的 stream/function/body。
- `Request` 发送 S1F1，payload 为 `[ASCII "PING", U4 7]`。
- 验证 peer 返回 S1F2、body 非空、`decoded_item` 存在，内容为 `[ASCII "ACK", [ASCII "PING", U4 7]]`。
- 额外发送 binary、boolean、signed/unsigned integer、F4/F8 和嵌套 list，验证 Rust 生成类型与服务端 SECS-II 编解码的边界。

### 4. 输入校验与错误语义

- CreateSession 缺少 transport。
- HSMS port 为 0 和 65536。
- HSMS session ID 大于 `0x7fff`。
- Get/Start/Stop/Delete 使用不存在的 session ID。
- 对一个已创建但未启动的真实 session 测试：缺少 message、stream 大于 127、function 为 0、Send 使用偶数 function、Request 使用无效 primary。
- 校验场景必须断言目标错误消息，避免被 `session not found` 或 `session is not running` 误判为通过。

### 5. 超时与故障

- 对没有对应 handler 的合法 primary 发起 `Request`，设置短 `timeout_ms`，要求返回存在且非 OK 的 `RpcStatus`。
- 使用 peer 的丢弃回复模式保证应用层 timeout 场景不会收到其他协议错误或快速回复。
- 设置比应用层超时更短的 tonic deadline。tonic 0.14 的本地超时层将其映射为 `Code::Cancelled`、消息 `Timeout expired`，测试同时验证 code 和消息，避免与任意取消混淆。
- 连接未监听端口，要求 tonic channel/connect 失败。
- peer 退出后继续调用，要求应用层返回非 OK status，并能通过 `GetSession.last_error` 或响应错误识别连接故障。
- 服务端退出后调用，要求得到 gRPC transport error，不能返回伪造的应用成功响应。

### 6. 并发与生命周期竞争

- 并发执行至少 32 个 `GetLibraryInfo` 和 `ListSessions`，全部必须带有效 OK status。
- 在已 selected 的 session 上并发执行多个 `Request`，验证每个回复与各自 payload 匹配，不发生串包。
- 使用 peer 的延迟回复模式和较小 `max_pending_requests` 确定性触发 pending 上限，验证超限调用明确失败，已接受调用仍能完成。
- 使用 peer 的延迟或丢弃回复模式，在请求确定进入 pending 后调用 `StopSession`；验证停止不会永久阻塞，挂起请求最终以明确结果收敛，且停止响应包含 OK status。

## 测试编排与清理

- 每组需要独立故障状态的场景使用独立 server/peer，避免前一场景污染 registry 或连接状态。
- 子进程 stdout/stderr 使用管道捕获；失败报告附带最近输出。
- 每个轮询、RPC 和子进程等待都有明确超时。
- Rust 进程退出时通过 RAII guard 终止仍存活的 server/peer，避免残留进程占用端口。
- 测试端口仅绑定 `127.0.0.1`。

## 验证矩阵

实现完成后执行：

```bash
cmake -S . -B build_rpc \
  -DSECS_ENABLE_RPC=ON \
  -DSECS_ENABLE_TESTS=ON \
  -DSECS_ENABLE_INTEGRATION_TESTS=ON \
  -DSECS_BUILD_TOOLS=ON
cmake --build build_rpc -j
ctest --test-dir build_rpc --output-on-failure -R 'rpc_(internal|smoke|python_interop|rust_tonic_interop)'
ctest --test-dir build_rpc --output-on-failure -R rpc_rust_tonic_interop --repeat until-fail:10
```

同时直接执行 Cargo 测试或 runner，确保失败日志在不经过 CTest 时也可读。若环境允许，再用 ASan/UBSan 构建服务端运行 Rust 互操作测试；TSan 仅在 brpc 及其依赖可兼容时启用。

## 完成标准

- Rust 客户端从仓库 proto 成功生成并调用全部三个 RPC service。
- 正常、错误、超时、断连和并发场景均有针对行为与原因的断言。
- C++ RPC 基线、Python gRPC 互操作和 Rust tonic 互操作均通过。
- Rust 互操作测试连续运行 10 次无失败或残留进程。
- 发现的服务端问题有可复现失败用例；修复后保留该回归用例。
