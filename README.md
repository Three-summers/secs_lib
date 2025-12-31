# SECS Library（C++20）

[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](https://en.cppreference.com/w/cpp/20)
[![CMake](https://img.shields.io/badge/CMake-3.20+-green.svg)](https://cmake.org/)

基于 C++20 与 standalone Asio 协程的 SECS-I / SECS-II / HSMS (HSMS-SS) 协议栈实现，面向半导体设备通信场景。

本文档聚焦两件事：
1. 当前代码的分层架构（模块职责、依赖关系、数据流）
2. 可直接上手的使用说明（构建/链接、收发消息、写 handler、SML 用法）

---

## 架构概览（当前实现）

本项目按“基础设施 → SECS-II → 传输 → 协议层 →（可选）规则语言”分层，目标是让你可以按需选用模块：

```
┌────────────────────────────────────────────────────────────┐
│                        你的业务代码                          │
│  - 设备/主机应用逻辑                                         │
│  - 将 SECS-II Item ↔ 业务结构体                              │
└───────────────▲───────────────────────────────▲────────────┘
                │                               │
                │                               │（可选）基于规则的自动响应/定时发送
┌───────────────┴───────────────────────────────┴────────────┐
│                   secs::protocol（协议层）                   │
│  protocol::Session                                           │
│  - SystemBytes 分配/复用、请求-响应匹配（T3）                 │
│  - Router 分发入站 primary；W-bit=1 时自动回 secondary        │
│  protocol::Router / TypedHandler                             │
└───────────────▲───────────────────────────────▲────────────┘
                │                               │
                │                               │
┌───────────────┴───────────────┐  ┌────────────┴─────────────┐
│        secs::hsms（TCP）        │  │      secs::secs1（串口）   │
│  hsms::Session                  │  │  secs1::StateMachine      │
│  - framing + SELECT/LINKTEST    │  │  - ENQ/EOT + 分包/重组      │
│  - 定时器：T3/T5/T6/T7/T8        │  │  - 超时：T1/T2/T3/T4        │
└───────────────▲───────────────┘  └────────────▲─────────────┘
                │                               │
┌───────────────┴───────────────────────────────┴────────────┐
│                    secs::ii（SECS-II / E5）                 │
│  ii::Item（强类型 AST） + encode/decode_one（流式编解码）      │
└───────────────▲───────────────────────────────▲────────────┘
                │                               │
┌───────────────┴───────────────────────────────┴────────────┐
│                      secs::core（基础设施）                  │
│  byte/span、FixedBuffer、Event、error_code 等                 │
└────────────────────────────────────────────────────────────┘

（可选）secs::sml：SML（SECS Message Language）解析与运行时
```

### 模块与 CMake Targets

本仓库没有“单一大库”，而是拆为多个 CMake target（你只需要链接你用到的那层）：

| 模块 | CMake target | 主要依赖 | 主要用途 |
| --- | --- | --- | --- |
| core | `secs::core` | Asio(standalone), Threads | 基础类型与通用设施（`byte/span`、`FixedBuffer`、`Event`、`core::errc`） |
| ii | `secs::ii` | `secs::core` | SECS-II（SEMI E5）数据模型 + 编解码 |
| hsms | `secs::hsms` | `secs::core` | HSMS-SS（SEMI E37）会话（TCP framing、SELECT/LINKTEST、定时器） |
| secs1 | `secs::secs1` | `secs::core` | SECS-I（SEMI E4）半双工传输状态机（分包/重组、握手、超时） |
| protocol | `secs::protocol` | `secs::core` + `secs::hsms` + `secs::secs1` | 统一 HSMS/SECS-I 的 `send/request/run` + 路由/自动回复 |
| sml | `secs::sml` | `secs::ii` + `secs::core` | SML 解析、条件响应匹配、定时规则访问（用于自动化脚本/仿真） |

### 目录结构

```
secs_lib/
├── CMakeLists.txt
├── include/secs/               # 公共头文件
│   ├── core/                   # 基础设施（byte/span、FixedBuffer、Event、error_code）
│   ├── ii/                     # SECS-II（Item + 编解码）
│   ├── hsms/                   # HSMS（Message/Connection/Session）
│   ├── secs1/                  # SECS-I（Link/StateMachine/分包/超时）
│   ├── protocol/               # 协议层（Session/Router/TypedHandler/SystemBytes）
│   └── sml/                    # SML（Lexer/Parser/Runtime/AST）
├── src/                        # 对应实现
├── examples/                   # 示例程序（CMake target: examples）
├── tests/                      # 单元测试（ctest）
├── benchmarks/                 # 性能基准（可选）
├── third_party/asio/           # vendored standalone Asio
└── cmake/                      # CMake modules（AsioStandalone、Coverage 等）
```

### 数据如何在各层流动（典型路径）

以“发送一个 SxFy 请求并等待回应”为例：

1. 业务层构造 `secs::ii::Item`（比如 `Item::list({...})`）
2. 用 `secs::ii::encode()` 将 Item 编码为 `std::vector<secs::core::byte>`（SECS-II payload）
3. 用 `secs::protocol::Session::async_request()` 或 `secs::hsms::Session::async_request_data()` 发送（库内部处理 SystemBytes 与超时）
4. 收到回应后，业务层对回应 `body` 调用 `secs::ii::decode_one()` 还原 `Item`
5. 再由你把 `Item` 转成业务结构体（或用 `TypedHandler` 把“Item ↔ 强类型消息”固化）

---

## 构建与集成

### 依赖

- C++20 编译器：GCC ≥11 / Clang ≥14 / MSVC ≥19.30
- CMake ≥3.20
- standalone Asio：默认使用 `third_party/asio/`；也支持 `-DSECS_ASIO_ROOT=/path/to/asio/include`

### 构建本仓库（开发/跑测试）

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

### 作为子项目集成到你的工程（推荐）

本仓库当前未提供 `install()`/`find_package()`，推荐以源码方式集成：

```cmake
# 你的工程 CMakeLists.txt
add_subdirectory(path/to/secs_lib)

add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE secs::protocol) # 只用 SECS-II 则改为 secs::ii
```

可选：如果你想强制使用外部 Asio（而不是 third_party），配置时加：

```bash
cmake -S . -B build -DSECS_ASIO_ROOT=/path/to/asio/include
```

### 常用 CMake 选项

```bash
# 单元测试（默认 ON）
cmake -S . -B build -DSECS_ENABLE_TESTS=ON

# 示例程序（默认 ON）
cmake -S . -B build -DSECS_BUILD_EXAMPLES=ON

# 性能基准（默认 OFF）
cmake -S . -B build -DSECS_BUILD_BENCHMARKS=ON

# 覆盖率（默认 OFF）
cmake -S . -B build -DSECS_ENABLE_COVERAGE=ON
```

---

## 使用说明（按模块）

### 1) SECS-II（`secs::ii`）：Item 与编解码

#### 数据模型：`ii::Item`（强类型）

`secs::ii::Item` 内部持有一个 `std::variant`，可表示：

- `secs::ii::List`（`std::vector<Item>`，可嵌套）
- `secs::ii::ASCII` / `Binary` / `Boolean`
- `secs::ii::I1/I2/I4/I8`、`U1/U2/U4/U8`、`F4/F8`

约定（对应实现）：

- 数值/布尔类型统一用 vector 承载，因此“单值”也是 `values.size()==1`
- ASCII 以 `std::string` 存储（按字节序列编码/解码）

#### 编码/解码 API

- `encoded_size(item, out_size)`：先算编码后长度（含头部）
- `encode(item, out)`：编码并追加到 `out`
- `encode_to(out_span, item, written)`：编码到固定缓冲区（用于零拷贝/流式写）
- `decode_one(in_span, out_item, consumed)`：从输入中解析一个 Item（流式，返回消耗字节数）

#### 最小可运行示例：构造 → 编码 → 解码

```cpp
#include <secs/ii/codec.hpp>
#include <secs/ii/item.hpp>

#include <iostream>
#include <vector>

int main() {
  // <L <A "Hello"> <U2 12345>>
  secs::ii::Item msg = secs::ii::Item::list({
    secs::ii::Item::ascii("Hello"),
    secs::ii::Item::u2({12345}),
  });

  std::vector<secs::core::byte> encoded;
  if (auto ec = secs::ii::encode(msg, encoded)) {
    std::cerr << "encode failed: " << ec.message() << "\n";
    return 1;
  }

  secs::ii::Item decoded{secs::ii::List{}};
  std::size_t consumed = 0;
  if (auto ec = secs::ii::decode_one(
        secs::core::bytes_view{encoded.data(), encoded.size()},
        decoded,
        consumed)) {
    std::cerr << "decode failed: " << ec.message() << "\n";
    return 1;
  }

  if (auto* list = decoded.get_if<secs::ii::List>()) {
    std::cout << "decoded list size=" << list->size() << ", consumed=" << consumed << "\n";
  }
}
```

更多更完整示例见：`examples/secs2_simple.cpp`、`tests/test_secs2_codec.cpp`。

---

### 2) HSMS（`secs::hsms`）：连接与会话

`hsms::Session` 负责：

- TCP framing 的读写（内部使用 `hsms::Connection`）
- HSMS-SS 控制流：`SELECT` / `DESELECT` / `LINKTEST`
- 定时器：`t3/t5/t6/t7/t8`（在 `SessionOptions` 中配置）
- 对外暴露“数据消息”的收发：
  - `async_send(Message)`：发送任意 HSMS 消息（含控制/数据）
  - `async_receive_data(timeout)`：只等待下一条 data message（控制消息内部处理/应答）
  - `async_request_data(stream, function, body)`：发送 data primary（W=1）并等待同 SystemBytes 的回应（T3）

#### 最小代码片段：Active 侧发送一次请求

下面示例展示关键调用顺序：`async_open_active()` → `async_request_data()` → `decode_one()`。

```cpp
#include <secs/hsms/session.hpp>
#include <secs/ii/codec.hpp>
#include <secs/ii/item.hpp>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/this_coro.hpp>

#include <chrono>
#include <iostream>
#include <vector>

asio::awaitable<void> run_once() {
  using namespace std::chrono_literals;

  auto ex = co_await asio::this_coro::executor;

  secs::hsms::SessionOptions opt;
  opt.session_id = 0x0001;
  opt.t3 = 45s;
  opt.t6 = 5s;
  opt.t7 = 10s;
  opt.t8 = 5s;

  secs::hsms::Session session{ex, opt};

  asio::ip::tcp::endpoint ep{asio::ip::make_address("127.0.0.1"), 5000};
  if (auto ec = co_await session.async_open_active(ep)) {
    std::cerr << "open failed: " << ec.message() << "\n";
    co_return;
  }

  std::vector<secs::core::byte> body;
  secs::ii::encode(secs::ii::Item::list({}), body);  // <L>

  auto [req_ec, reply] = co_await session.async_request_data(
    1, 1, secs::core::bytes_view{body.data(), body.size()});
  if (req_ec) {
    std::cerr << "request failed: " << req_ec.message() << "\n";
    co_return;
  }

  secs::ii::Item decoded{secs::ii::List{}};
  std::size_t consumed = 0;
  if (auto dec_ec = secs::ii::decode_one(
        secs::core::bytes_view{reply.body.data(), reply.body.size()},
        decoded,
        consumed)) {
    std::cerr << "decode failed: " << dec_ec.message() << "\n";
    co_return;
  }
}

int main() {
  asio::io_context ioc;
  asio::co_spawn(ioc, run_once(), asio::detached);
  ioc.run();
}
```

#### Active（客户端）典型流程

1. 构造 `hsms::SessionOptions`（至少设置 `session_id`，生产建议显式设置各定时器）
2. 构造 `hsms::Session`（需要一个 `asio::any_io_executor`）
3. `co_await session.async_open_active(endpoint)` 建连并完成 `SELECT` 握手
4. 通过 `async_request_data()` / `async_send()` / `async_receive_data()` 收发消息

对应可运行示例：`examples/hsms_client.cpp`

#### Passive（服务器）典型流程

1. `asio::ip::tcp::acceptor` 接受 socket
2. 为每个连接创建 `hsms::Session`
3. `co_await session.async_open_passive(std::move(socket))` 等待对端 `SELECT`
4. 在 `session.is_selected()` 为 true 期间循环 `async_receive_data()`

对应可运行示例：`examples/hsms_server.cpp`

---

### 3) SECS-I（`secs::secs1`）：Link + StateMachine

SECS-I 这一层以“字节流链路”抽象开始：

- `secs::secs1::Link`：抽象接口，仅要求 `async_write()` 与 `async_read_byte()`
- `secs::secs1::StateMachine`：在 Link 之上实现 ENQ/EOT/ACK/NAK、分包/重组、校验与 T1/T2/T3/T4 超时

说明：

- 本仓库内置 `secs::secs1::MemoryLink` 用于单元测试/仿真
- 若你要对接真实串口，需要你基于 `asio::serial_port` 实现一个 `Link`

对应测试：`tests/test_secs1_framing.cpp`

---

### 4) 协议层（`secs::protocol`）：统一 send/request + Router/TypedHandler

这一层是推荐给业务使用的入口：你可以把它理解为“统一 HSMS/SECS-I 的 SECS 消息收发框架”。

#### 关键类型

- `protocol::DataMessage`：统一后的数据消息结构（`stream/function/w_bit/system_bytes/body`）
- `protocol::Router`：按 `(stream,function)` 查找 handler
- `protocol::Session`：
  - 构造时绑定后端：`hsms::Session&` 或 `secs1::StateMachine&`
  - `async_send(stream, function, body)`：发送 primary（W=0）
  - `async_request(stream, function, body, timeout)`：发送 primary（W=1）并等待 secondary（T3）
  - `async_run()`：接收入站消息、匹配 pending、路由 handler（HSMS 场景推荐长期运行）

注意：`function` 必须是 primary（奇数且非 0），否则会返回 `core::errc::invalid_argument`。

#### 最小代码片段：注册一个 handler，并发起一次 `async_request`

下面示例展示 `protocol::Session` 的核心用法：

```cpp
#include <secs/hsms/session.hpp>
#include <secs/ii/codec.hpp>
#include <secs/ii/item.hpp>
#include <secs/protocol/session.hpp>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/this_coro.hpp>

#include <iostream>
#include <vector>

asio::awaitable<void> run_proto() {
  auto ex = co_await asio::this_coro::executor;

  secs::hsms::SessionOptions hsms_opt;
  hsms_opt.session_id = 0x0001;

  secs::hsms::Session hsms{ex, hsms_opt};
  asio::ip::tcp::endpoint ep{asio::ip::make_address("127.0.0.1"), 5000};
  if (auto ec = co_await hsms.async_open_active(ep)) {
    std::cerr << "hsms open failed: " << ec.message() << "\n";
    co_return;
  }

  secs::protocol::Session proto{hsms, hsms_opt.session_id};

  // 收到 S1F1 且 W=1 时，框架会自动回 S1F2（function+1）。
  proto.router().set(1, 1,
    [](const secs::protocol::DataMessage& /*msg*/)
      -> asio::awaitable<secs::protocol::HandlerResult> {
        secs::ii::Item rsp = secs::ii::Item::ascii("OK");
        std::vector<secs::core::byte> rsp_body;
        secs::ii::encode(rsp, rsp_body);
        co_return secs::protocol::HandlerResult{std::error_code{}, std::move(rsp_body)};
      });

  // HSMS 场景推荐：并行运行接收循环（处理入站消息、唤醒 pending 请求）。
  asio::co_spawn(ex, proto.async_run(), asio::detached);

  std::vector<secs::core::byte> req_body;
  secs::ii::encode(secs::ii::Item::list({}), req_body);

  auto [ec, reply] = co_await proto.async_request(
    1, 1, secs::core::bytes_view{req_body.data(), req_body.size()});
  if (ec) {
    std::cerr << "request failed: " << ec.message() << "\n";
    co_return;
  }

  (void)reply;  // reply.body 是对端的 SECS-II payload
}

int main() {
  asio::io_context ioc;
  asio::co_spawn(ioc, run_proto(), asio::detached);
  ioc.run();
}
```

#### Router handler 的签名

```cpp
using HandlerResult = std::pair<std::error_code, std::vector<secs::core::byte>>;
using Handler = std::function<asio::awaitable<HandlerResult>(const DataMessage&)>;
```

当收到 primary 且 `W-bit=1` 时：

- 框架调用 handler
- handler 返回 `response body`（仅 body，不需要你拼 header）
- `protocol::Session` 自动发送 `secondary (function+1, W=0, system_bytes 同请求)`

#### TypedHandler：把“Item ↔ 强类型消息”固化

`protocol::TypedHandler<TReq, TRsp>` 提供一个约束：

- `TReq::from_item(const ii::Item&) -> std::optional<TReq>`
- `TRsp::to_item() const -> ii::Item`

框架会自动做：

1. 入站 body → `ii::decode_one()` → `ii::Item`
2. `TReq::from_item()` → 强类型请求
3. 调用你的 `handle()`
4. `TRsp::to_item()` → `ii::Item` → `ii::encode()` → 出站 body

对应示例：`examples/typed_handler_example.cpp`、`tests/test_typed_handler.cpp`

---

### 5) SML（`secs::sml`）：SML 解析与运行时（可选）

`secs::sml` 提供一个轻量的 SML（SECS Message Language）子集，适用于：

- 仿真：用文本描述一组 SxFy 消息与 body 模板
- 自动回应：用 `if (...)` 规则匹配收到的消息并选择响应模板
- 定时发送：用 `every N send ...` 描述周期消息

#### 运行时：加载与匹配

```cpp
#include <secs/sml/runtime.hpp>

secs::sml::Runtime rt;
auto ec = rt.load(R"(
  s1f1: S1F1 W <L>.
  s1f2: S1F2 <L <A "OK">>.
  if (s1f1) s1f2.
  every 5 send s1f1.
)");

// 收到 S1F1 后，匹配到响应消息名 "s1f2"
auto rsp = rt.match_response(1, 1, secs::ii::Item::list({}));
```

更完整语法与边界行为以单测为准：`tests/test_sml_parser.cpp`。

---

## 示例与测试

### 构建并运行示例

```bash
cmake -S . -B build -DSECS_BUILD_EXAMPLES=ON
cmake --build build --target examples -j

./build/examples/secs2_simple
./build/examples/hsms_server [port]
./build/examples/hsms_client [host] [port]
./build/examples/typed_handler_example
```

示例说明见：`examples/README.md`

### 运行测试

```bash
ctest --test-dir build --output-on-failure
```

可按名字过滤：

```bash
ctest --test-dir build -R secs2_codec
ctest --test-dir build -R hsms_transport
ctest --test-dir build -R protocol_session
ctest --test-dir build -R sml_parser
```

---

## 覆盖率（可选）

覆盖率由 `gcovr` 生成，具体数值以本地生成结果为准：

```bash
cmake -S . -B build -DSECS_ENABLE_COVERAGE=ON
cmake --build build -j
ctest --test-dir build
cmake --build build --target coverage
```

---

## 标准对应

- SEMI E4: SECS-I Message Transfer (Serial)
- SEMI E5: SECS-II Message Content
- SEMI E37: HSMS - High-Speed SECS Message Services

## 致谢

- C 语言参考实现：`c_dump/` 目录
- Asio：[chriskohlhoff/asio](https://github.com/chriskohlhoff/asio)
