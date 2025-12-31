# 使用示例

## 编译运行

```bash
# 配置项目
cmake -S . -B build -DSECS_BUILD_EXAMPLES=ON

# 编译示例
cmake --build build --target examples

# 运行
./build/examples/secs2_simple
./build/examples/hsms_server [port]
./build/examples/hsms_client [host] [port]
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

## 更多示例

完整的 HSMS 客户端、服务器和协议层示例见项目测试代码：

- `tests/test_secs2_codec.cpp` - SECS-II 各种类型编解码
- `tests/test_hsms_transport.cpp` - HSMS 会话完整流程
- `tests/test_protocol_session.cpp` - 协议层消息路由

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
