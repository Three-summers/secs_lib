# C++ API 使用指南（面向库使用者）

> 文档更新：2026-01-14（Codex）  
> 基于源码版本：当前工作区  
> 目标读者：需要在自己的 C++ 项目中集成并使用 `secs_lib` 的工程师（Host / Equipment / 仿真器）

本指南只讲“怎么用”，不讲实现细节。若你想看模块设计原理，请阅读 `docs/architecture/`。

---

## 0. 你应该选哪条使用路径？

```
┌──────────────────────────────────────────────────────────────────────────────┐
│                                你写的业务代码                                 │
└──────────────────────────────────────────────────────────────────────────────┘
                 │
                 │ ① 只想编解码 SECS-II（不做通信）
                 ▼
        ┌───────────────────┐
        │ secs::ii           │  Item ↔ bytes（SEMI E5）
        └───────────────────┘

                 │
                 │ ② 要通信，但希望“收发 + handler + 自动回包”都统一
                 ▼
┌──────────────────────────────────────────────────────────────────────────────┐
│ secs::protocol::Session                                                       │
│ - async_send / async_request / async_run / async_poll_once                    │
│ - Router：按 (S,F) 分发 + default handler                                     │
│ - 自动回 secondary：F+1 / W=0 / SB 回显（仅 W=1 且 handler 返回 OK 时）         │
└──────────────────────────────────────────────────────────────────────────────┘
     │                                               │
     │ HSMS（TCP，全双工）                            │ SECS-I（串口，半双工）
     ▼                                               ▼
┌──────────────────────────┐                 ┌──────────────────────────────┐
│ secs::hsms::Session       │                 │ secs::secs1::StateMachine     │
│ async_open_* / async_run  │                 │ + SerialPortLink / MemoryLink │
└──────────────────────────┘                 └──────────────────────────────┘

                 │
                 │ ③ 想用“规则/脚本”快速搭对端：按 SML 自动回复 + 定时发送
                 ▼
        ┌───────────────────┐
        │ secs::sml::Runtime │  load / match_response / timers / (SMLX render)
        └───────────────────┘
```

推荐：如果你不是在做底层协议研究，优先从 `secs::protocol::Session` 入手。

---

## 0.1 集成与构建（CMake）

本库为每个模块导出了 CMake target（见顶层 `CMakeLists.txt`）：

- `secs::ii`：只用 SECS-II 编解码
- `secs::hsms`：只用 HSMS 传输
- `secs::secs1`：只用 SECS-I 传输
- `secs::protocol`：推荐入口（统一 HSMS/SECS-I 的收发 + handler + 自动回包）
- `secs::sml`：SML/SMLX（规则引擎与模板渲染）
- `secs::utils`：dump/编解码 helpers

作为子项目集成的最小示例：

```cmake
add_subdirectory(path/to/secs_lib)
target_link_libraries(my_app PRIVATE secs::protocol secs::sml) # 按需选择模块
```

本仓库示例构建（用于“先跑起来再改造”）：

```bash
cmake -S . -B build -DSECS_BUILD_EXAMPLES=ON
cmake --build build --target examples -j
```

---

## 1. 基础概念速记（读懂后面所有示例）

### 1.1 SxFy / Primary / Secondary / W-bit

```
Primary   : Function 为奇数（例如 S1F1、S2F13），可带 W-bit=1 表示“需要回应”
Secondary : Function 为偶数（例如 S1F2、S2F14），W-bit 必须为 0

一对请求/回应的关系（协议层固定规则）：
  request : SxF(y)  (y 为奇数)  W=1
  reply   : SxF(y+1)            W=0
```

### 1.2 System Bytes（SB）

`SB` 是一条事务的标识：

- 你发出去的 primary 会被库分配一个 `system_bytes`
- 对端回 secondary 时必须“回显相同 SB”
- `protocol::Session` 依靠 `SB` 匹配 `async_request()` 的等待者

---

## 2. 快速开始：HSMS + protocol::Session（推荐入口）

本节目标：跑起来“请求 → 自动回包 → 收到回应”。

对应可运行示例（建议先跑一遍再读代码）：

- `examples/hsms_server.cpp`、`examples/hsms_client.cpp`
- `examples/protocol_custom_reply_example.cpp`（不依赖 socket，MemoryLink 回环）

### 2.1 典型结构（时序图）

```
     你的线程 / io_context                         对端
┌───────────────────────────┐            ┌───────────────────────────┐
│ asio::io_context           │            │ asio::io_context           │
│   co_spawn(proto.async_run)│            │   co_spawn(proto.async_run)│
│             │              │            │              │            │
│             │ async_request│            │              │            │
│             ▼              │            │              ▼            │
│    protocol::Session       │            │     protocol::Session      │
│  (HSMS backend)            │            │   Router handler 命中       │
│     │                      │            │      │                    │
│     │  DataMessage primary  │───────────▶│      │  handler(req)      │
│     │  (SxF odd, W=1, SB=*) │            │      ▼                    │
│     │                      │            │  返回 {OK, rsp_body}       │
│     │  等待 secondary       │            │      │                    │
│     │                      │            │      │ 自动回 secondary：    │
│     │  DataMessage secondary│◀───────────│      │ SxF(y+1), W=0, SB=* │
│     ▼                      │            │      ▼                    │
│ async_request 返回 reply    │            │  async_send(secondary)     │
└───────────────────────────┘            └───────────────────────────┘
```

### 2.2 最小代码骨架（只展示关键 API 名称）

你需要理解的“最小拼图”只有这些：

- HSMS：`secs::hsms::Session` 负责连接 + SELECT + LINKTEST 等控制流
- 协议层：`secs::protocol::Session` 负责消息收发 + Router + 自动回包

```cpp
#include "secs/hsms/session.hpp"
#include "secs/protocol/session.hpp"

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>

asio::awaitable<void> run() {
  auto ex = co_await asio::this_coro::executor;

  secs::hsms::SessionOptions hsms_opt{};
  hsms_opt.session_id = 0x0001;
  secs::hsms::Session hsms(ex, hsms_opt);

  // 主动端：连接并进入 selected（也可以用 async_open_passive 做被动端）
  (void)co_await hsms.async_open_active({asio::ip::make_address("127.0.0.1"), 5000});

  secs::protocol::SessionOptions proto_opt{};
  proto_opt.t3 = std::chrono::seconds{45};
  secs::protocol::Session proto(hsms, hsms_opt.session_id, proto_opt);

  // 注册 handler：只注册 primary（奇数 function）
  proto.router().set(1, 1, /* handler */);

  // 推荐：长跑接收循环（HSMS 全双工）
  asio::co_spawn(ex, [&]() -> asio::awaitable<void> { co_await proto.async_run(); }, asio::detached);

  // 发起 request（W=1），等待 secondary
  auto [ec, reply] = co_await proto.async_request(1, 1, secs::core::bytes_view{});
  (void)ec;
  (void)reply;
}

int main() {
  asio::io_context ioc;
  asio::co_spawn(ioc, run(), asio::detached);
  ioc.run();
}
```

上面代码省略了错误处理与资源释放；完整且可运行的版本请直接参考示例文件。

---

## 3. Router 与“自动回包”的规则（必须搞清楚，否则你会以为库没回包）

### 3.1 Router 的匹配顺序（3 级）

`Router` 会按下面顺序找 handler（见 `include/secs/protocol/router.hpp`）：

```
find(stream,function):
  1) 精确匹配 (S,F)
  2) stream default：SxF*（同一个 stream 的兜底）
  3) default handler：全局兜底
  4) 都没有：未处理（不回包）
```

### 3.2 自动回包发生的必要条件

`protocol::Session` 在收到入站 primary 时：

```
if (未命中 pending 请求) 且 (是 primary) 且 (Router 找到 handler)：
  [ec, rsp_body] = co_await handler(req)
  if (ec==OK) 且 (req.w_bit==1) 且 (req.function!=0xFF)：
      自动发送 secondary：S=req.stream, F=req.function+1, W=0, SB=req.system_bytes
  else：
      不回包
```

因此你遇到“没回包”时，按这个 checklist 排查：

- 你是否只注册了 secondary（偶数 F）而不是 primary（奇数 F）？
- 对端发来的 primary 是否 `W=1`？
- handler 是否返回了 `error_code!=0`（任何非 OK 都会导致不回包）？
- primary 的 `function` 是否为 `0xFF`（无法计算 `F+1`）？

---

## 4. 写 handler 的 3 种方式（从轻到重）

### 4.1 方式 A：lambda（最直接）

`Router::set()` 的 handler 签名：

```cpp
using HandlerResult = std::pair<std::error_code, std::vector<secs::core::byte>>;
using Handler = std::function<asio::awaitable<HandlerResult>(const DataMessage&)>;
```

常用写法：用 `secs::utils::decode_one_item()` 解码请求，用 `secs::utils::make_handler_result()` 编码响应。

参考实现：

- `examples/protocol_custom_reply_example.cpp`（default handler + switch(S,F)）

### 4.2 方式 B：default handler（集中处理很多 SxFy）

当你要处理的 SxFy 很多，但又不想注册几十个 handler：

```cpp
proto.router().set_default(
  [](const secs::protocol::DataMessage& req) -> asio::awaitable<secs::protocol::HandlerResult> {
    // switch(req.stream, req.function) 生成回应 body
  });
```

适用：

- 快速做一个“设备仿真器 / Host 仿真器”
- legacy 风格（类似 C 里 switch(Stream/Function)）

### 4.3 方式 C：TypedHandler（继承机制，适合长期维护）

当你希望把“SECS-II Item ↔ 业务结构体”的样板代码固化，并获得更清晰的类型边界：

```
你做：
  - 定义 TRequest / TResponse，提供 from_item/to_item
  - 继承 TypedHandler 并实现 handle()

框架做：
  - 入站 body 解码 → TRequest
  - 调用你的 handle()
  - TResponse 编码 → 出站 body
```

关键类型：

- `secs::protocol::TypedHandler<TReq, TRsp>`：基类（见 `include/secs/protocol/typed_handler.hpp`）
- `secs::protocol::register_typed_handler(router, stream, function, shared_ptr<handler>)`：注册到 Router

可直接复用的标准消息类型：

- `include/secs/messages/standard.hpp`（例如 `secs::messages::S1F1Request` / `S1F2Response`）

对应示例：

- `examples/typed_handler_example.cpp`

---

## 5. SML：读取 + 自动回复（规则驱动，减少 glue 代码）

你可以把 SML 当成“可配置的回包规则”：

- 用 `if (...) rsp_name.` 描述“收到什么 -> 回什么”
- 用 `name: SxFy [W] <Item>.` 描述模板
- 用 `every N send name.` 描述定时发送（适合联调造流量）

示例 SML 文件：`docs/sml_sample/sample.sml`

### 5.0 最小 SML 片段（理解语义用）

```sml
// 定义两条模板
s1f1: S1F1 W <L>.
s1f2: S1F2 <L <A "OK">>.

// 条件：收到 s1f1 -> 回 s1f2
if (s1f1) s1f2.

// 定时：每 10 秒主动发一次 s1f1（用于联调造流量）
every 10 send s1f1.
```

### 5.0.1 从文件读取并加载

`Runtime::load()` 的输入是“文本内容”，因此你需要自己读文件（示例代码见 `examples/hsms_sml_peer.cpp`、`examples/secs1_sml_peer.cpp`）。

```cpp
std::string text = read_all_text("docs/sml_sample/sample.sml"); // 自行实现
secs::sml::Runtime rt;
auto ec = rt.load(text);
```

### 5.1 SML 自动回复的最小链路（ASCII 流程图）

```
                 inbound DataMessage(primary, W=1)
                             │
                             ▼
                  解码 body -> secs::ii::Item
                             │
                             ▼
     rt.match_response(stream,function,item) -> "rsp_name" ?
                 │                         │
                 │ no match                │ matched
                 ▼                         ▼
           返回 error（不回包）      rt.get_message("rsp_name")
                                         │
                                         ▼
                           render_item(tpl, ctx) -> rendered Item
                                         │
                                         ▼
                          encode Item -> rsp_body(bytes)
                                         │
                                         ▼
                      handler 返回 {OK, rsp_body}
                                         │
                                         ▼
               protocol::Session 自动回 secondary(F+1/W=0/SB 回显)
```

### 5.2 推荐做法：把 SML 挂到 Router 的 default handler

仓库示例已经给出一套可复用写法：

- `examples/hsms_sml_peer.cpp`：HSMS 主动/被动两种模式都可加载同一份 SML
- `examples/secs1_sml_peer.cpp`：SECS-I 串口对端 + timers（半双工主循环）

其中核心就是：

```cpp
proto.router().set_default(make_sml_auto_reply(rt));
```

你可以直接把示例里的 `make_sml_auto_reply()` 函数拷贝到你的工程里改造：

- 先把“入站 body 解码”为 `secs::ii::Item`
- 用 `rt->match_response()` 找到响应模板名
- 用 `rt->get_message()` 拿到模板，再 `secs::sml::render_item()` 渲染
- 最后返回 `secs::utils::make_handler_result(rendered_item)`

### 5.2.1 直接跑现成 SML 对端（不用你写代码）

HSMS：

```bash
./build/examples/hsms_sml_peer --help
./build/examples/hsms_sml_peer --mode passive --listen 0.0.0.0 --port 5000 --sml docs/sml_sample/sample.sml --session-id 0x0001
```

SECS-I（串口，Windows/POSIX）：

```bash
./build/examples/secs1_sml_peer --help
./build/examples/secs1_sml_peer --role equipment --serial COM5 --baud 9600 --device-id 0x0001 --sml docs/sml_sample/sample.sml
```

### 5.3 SMLX（占位符/渲染）与变量注入（可选）

如果你希望“模板 body 里某些字段由运行时决定”（例如 MDLN/SOFTREV、列表参数等），可以用 SMLX v0：

- 在模板值位置写变量名：`<A MDLN>`、`<U2 SVIDS>`、`<B BYTES>` …
- 在代码中用 `secs::sml::RenderContext` 注入变量值（变量值类型仍用 `secs::ii::Item` 表达）

参考示例：

- `examples/smlx_active_send_example.cpp`（渲染 + 主动发送 + 回包都基于同一份 SMLX）

注意当前实现限制（见 `docs/architecture/09-smlx-extension.md` 与测试）：

- 条件期望值 `if (a(1)==<...>)` 里不允许占位符（解析会报 `sml.parser/invalid_condition`）
- 如果模板里存在占位符而你又没有提供变量，会得到 `sml.render/missing_variable`

---

## 6. SECS-I（串口）使用建议：用 `async_poll_once()` 驱动主循环

SECS-I 是半双工字节流链路，最容易踩的坑是“并发读写导致状态机拒绝/报错”。

库已经给出推荐用法（示例：`examples/secs1_sml_peer.cpp`）：

```
主循环（单线程/单 strand）：
  1) 处理 timers（到点就发）
  2) proto.async_poll_once(wait) 等一条入站消息并处理
  3) 重复

不要：
  - 一边跑 proto.async_run()，另一边并发 async_send/async_request
  - 多线程同时对同一个 SECS-I Session 发起收发
```

关键配置点：R-bit（方向位）

- Host -> Equipment：`SessionOptions::secs1_reverse_bit = false`
- Equipment -> Host：`SessionOptions::secs1_reverse_bit = true`

---

## 7. 调试与联调：dump / log / 解码

### 7.1 开启 protocol 层运行时报文 dump（TX/RX）

见 `include/secs/protocol/session.hpp` 的 `SessionOptions::DumpOptions`：

- `enable`：总开关
- `dump_tx/dump_rx`：方向开关
- `sink`：可选输出回调；为空则走库内 spdlog
- HSMS/SECS-I 各自有细分选项：`hsms` / `secs1`

适合场景：

- 对接第三方 HSMS/SECS-I 工具时，快速确认对端到底发了什么
- SECS-II payload 结构不确定时，先 dump 再决定怎么解码

### 7.2 常用“快速定位”入口

- HSMS/SECS-I 报文解析：`include/secs/utils/*_dump.hpp`、`examples/utils_dump_example.cpp`
- SECS-II 编解码：`include/secs/ii/codec.hpp`、`examples/secs2_simple.cpp`
- 端到端（不依赖 socket）：`examples/protocol_custom_reply_example.cpp`、`examples/secs1_loopback.cpp`

---

## 8. 进一步阅读（按“最像文档”的代码顺序）

1) 示例（先跑起来）：

- `examples/README.md`
- `examples/hsms_server.cpp` / `examples/hsms_client.cpp`
- `examples/protocol_custom_reply_example.cpp`
- `examples/typed_handler_example.cpp`
- `examples/hsms_sml_peer.cpp` / `examples/secs1_sml_peer.cpp`

2) 公开 API：

- `include/secs/protocol/session.hpp`
- `include/secs/protocol/router.hpp`
- `include/secs/protocol/typed_handler.hpp`
- `include/secs/sml/runtime.hpp` / `include/secs/sml/render.hpp`

3) 单元测试（最接近“可执行规范”）：

- `tests/test_protocol_session.cpp`
- `tests/test_typed_handler.cpp`
- `tests/test_sml_parser.cpp`
