# 使用示例

## 编译运行

```bash
# 配置项目
cmake -S . -B build -DSECS_BUILD_EXAMPLES=ON

# 编译示例
cmake --build build --target examples

# 运行
./build/examples/secs2_simple
./build/examples/c_api_secs2_simple
./build/examples/typed_handler_example
./build/examples/protocol_custom_reply_example
./build/examples/hsms_server [port]
./build/examples/hsms_client [host] [port]
./build/examples/hsms_pipe_server [device_id]
./build/examples/hsms_pipe_client [device_id]
./build/examples/secs1_loopback
./build/examples/secs1_serial_server <tty_path>
./build/examples/secs1_serial_client <tty_path>

# C API：HSMS（TCP）
./build/examples/c_api_hsms_server [ip] [port]
./build/examples/c_api_hsms_client [ip] [port]

# C API：协议层（Protocol Session）+ HSMS（TCP）
./build/examples/c_api_protocol_server [ip] [port]
./build/examples/c_api_protocol_client [ip] [port]

# C API：协议层回环（memory duplex，无 socket 环境也可运行，仅 UNIX）
./build/examples/c_api_protocol_loopback
```

## HSMS 客户端/服务器示例

### 启动服务器

```bash
# 默认监听 5000 端口
./build/examples/hsms_server

# 指定端口
./build/examples/hsms_server 5001
```

### 启动客户端

```bash
# 连接本地服务器
./build/examples/hsms_client

# 连接远程服务器
./build/examples/hsms_client 192.168.1.100 5000
```

### 示例输出

服务器端:
```
=== HSMS 服务器示例 ===

[服务器] 监听端口: 5000
[服务器] 等待客户端连接...
[服务器] 新连接: 127.0.0.1:54321
[服务器] 会话已建立，等待数据消息...
[服务器] 收到消息: S1F1 (W=1)
[服务器] 数据内容 (List): 0 项
[服务器] 已发送响应: S1F2
```

客户端:
```
=== HSMS 客户端示例 ===

[客户端] 连接到 127.0.0.1:5000...
[客户端] 已连接，会话已建立
[客户端] Linktest 成功

[客户端] 发送 S1F1 (Are You There)...
[客户端] 收到 S1F2 响应
[客户端] 响应内容: "OK"
```

## HSMS（pipe）客户端/服务器示例（受限环境推荐）

当运行环境禁用 `socket()`（例如部分沙箱/容器），`hsms_server/hsms_client` 的真实 TCP 示例将无法启动。
此时可使用 `hsms_pipe_server/hsms_pipe_client`：它们通过 **stdin/stdout** 传输 HSMS 帧（二进制流），日志仅输出到 stderr。

推荐直接运行联调测试（会启动两个示例程序并用管道交叉连接，验证一次请求-响应）：

```bash
ctest --test-dir build -R hsms_pipe_examples --output-on-failure
```

## C API：HSMS（TCP）客户端/服务器示例

文件：
- `c_api_hsms_server.c`：监听并接受连接，接收 1 条 data message；若 W=1 则回包
- `c_api_hsms_client.c`：连接并 SELECT，发送 1 次 request 并等待回应

运行（需要允许 socket 的环境）：

```bash
# 终端 1：启动 server
./build/examples/c_api_hsms_server 127.0.0.1 5000

# 终端 2：启动 client
./build/examples/c_api_hsms_client 127.0.0.1 5000
```

说明：
- server 使用 `secs_hsms_session_open_passive_ip()` 监听并 accept 1 个连接；
- client 使用 `secs_hsms_session_open_active_ip()` 连接并完成 SELECT；
- 示例里通过 `secs_log_set_level(SECS_LOG_DEBUG)` 打开库内部 debug 日志。

## C API：协议层（Protocol Session）示例（default handler + 双向 primary）

文件：
- `c_api_protocol_server.c`：default handler 统一处理未注册的 (S,F)，并在收到 client 的请求后反向向 client 发起 request
- `c_api_protocol_client.c`：注册 handler 回包，验证 server 也能主动发送 primary

运行（需要允许 socket 的环境）：

```bash
# 终端 1：启动 server
./build/examples/c_api_protocol_server 127.0.0.1 5001

# 终端 2：启动 client
./build/examples/c_api_protocol_client 127.0.0.1 5001
```

说明：
- 这组示例用的是协议层 `secs_protocol_session_*`，库会在 handler 返回成功时自动回 secondary；
- handler 的 `out_body` 需要用 `secs_malloc()` 分配；示例通过 `secs_ii_encode()` 生成（内部使用 `secs_malloc()`）。

## C API：协议层回环示例（memory duplex，受限环境推荐）

当运行环境禁用 `socket()` 时，上面的 TCP 示例无法启动，可直接运行这个回环示例验证“端到端链路 + 双向 primary”：

```bash
./build/examples/c_api_protocol_loopback
```

## secs2_simple.cpp - SECS-II 编解码基础

演示如何使用 SECS-II 编解码 API：

```cpp
#include <secs/ii/codec.hpp>
#include <secs/ii/item.hpp>

using namespace secs::ii;

int main() {
  // 构造 ASCII 消息
  Item msg = Item::ascii("Hello SECS");

  // 编码
  std::vector<byte> encoded;
  auto ec = encode(msg, encoded);
  if (ec) return 1;

  // 解码
  Item decoded{Item::ascii("")};
  std::size_t consumed;
  ec = decode_one(bytes_view{encoded.data(), encoded.size()}, decoded, consumed);

  // 访问结果
  auto* ascii_ptr = decoded.get_if<ASCII>();
  if (ascii_ptr) {
    std::cout << ascii_ptr->value << "\n";
  }

  return 0;
}
```

## c_api_secs2_simple.c - C API（C ABI）SECS-II 编解码

演示如何在 **C 语言** 中通过 `#include <secs/c_api.h>`：

- 构造 `List`（append 子元素）
- `secs_ii_encode` 编码得到 on-wire bytes（用 `secs_free` 释放）
- `secs_ii_decode_one` 解码并访问 ASCII/U2 等字段

## protocol_custom_reply_example.cpp - 协议层自定义回包（类似 tvoc 的 switch 风格）

演示如何在协议层用 **default handler** 做集中分发（例如 `switch(stream/function)`），并使用 SECS-II 编码构造返回 body：

- 未注册的 (S,F) 会回退到 default handler（避免大量 `set_handler(SxFy)` 造成“样板代码膨胀”）
- handler 返回 OK 时，库会自动发送 secondary（若请求为 W=1）

## 更多示例

完整的 HSMS 客户端、服务器和协议层示例见项目测试代码：

- `tests/test_secs2_codec.cpp` - SECS-II 各种类型编解码
- `tests/test_hsms_transport.cpp` - HSMS 会话完整流程
- `tests/test_protocol_session.cpp` - 协议层消息路由

## SECS-I 回环示例（MemoryLink）

`secs1_loopback.cpp` 使用 `secs::secs1::MemoryLink` 在进程内模拟“串口线”，并通过
`secs::protocol::Session` 跑通 `S1F13(W=1) -> S1F14` 的请求-响应（payload=700B，
覆盖分包/重组路径）。

## SECS-I 串口/虚拟串口示例（pty / socat）

本仓库提供两个可独立运行的 SECS-I 示例程序：

- `secs1_serial_server`：设备端（Equipment），注册 `S1F13` 回显 handler（自动回 `S1F14`）
- `secs1_serial_client`：主机端（Host），发送一次 `S1F13(W=1)` 并等待 `S1F14`

推荐用 `socat` 创建一对互联的虚拟串口（pty）进行测试：

```bash
# 终端 1：创建虚拟串口对（保持该进程不退出）
socat -d -d pty,raw,echo=0,link=/tmp/secs1_a pty,raw,echo=0,link=/tmp/secs1_b

# 终端 2：启动设备端（使用 /tmp/secs1_b）
./build/examples/secs1_serial_server /tmp/secs1_b

# 终端 3：启动主机端（使用 /tmp/secs1_a）
./build/examples/secs1_serial_client /tmp/secs1_a
```

说明：
- 上述示例默认 `device_id=1`；如需修改两端需保持一致：`--device-id <n>`
- 若使用真实串口（例如 `/dev/ttyUSB0`），可设置波特率：`--baud 115200`
- 若 `socat` 报 `openpty ... Permission denied`，说明当前环境禁用 pty；可在可用的宿主机环境运行，或使用 `integration_tests` 的 `integration_secs1_pty`（在无 pty 时会自动降级为 `socketpair()` 字节流链路）做基础互通验证。

## API 快速参考

### 构造 Item

```cpp
// 静态工厂方法
Item::list(std::vector<Item>{...})
Item::ascii("string")
Item::binary(std::vector<byte>{...})
Item::boolean(std::vector<bool>{...})
Item::u1/u2/u4/u8(std::vector<uint>{...})
Item::i1/i2/i4/i8(std::vector<int>{...})
Item::f4/f8(std::vector<float/double>{...})
```

### 访问 Item

```cpp
Item item = ...;

// 方式 1: get_if (推荐)
if (auto* ascii = item.get_if<ASCII>()) {
  std::cout << ascii->value;
}

// 方式 2: std::get (需要确保类型正确)
auto& list = std::get<List>(item.storage());
```

### 编解码

```cpp
// 编码
std::vector<byte> out;
encode(item, out);

// 解码
Item result{Item::ascii("")};
std::size_t consumed;
decode_one(input_bytes, result, consumed);
```
