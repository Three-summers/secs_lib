# SECS Library - 现代 C++ 实现

[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](https://en.cppreference.com/w/cpp/20)
[![CMake](https://img.shields.io/badge/CMake-3.20+-green.svg)](https://cmake.org/)
[![Coverage](https://img.shields.io/badge/Coverage-90%25+-brightgreen.svg)](#测试覆盖率)

基于 C++20 和 ASIO 协程的 SECS-I/II/HSMS 半导体设备通信协议栈现代实现。

## 特性

- ✅ **SECS-II 编解码**: 完整支持 List/ASCII/Binary/Boolean/整数/浮点类型，1-3 字节长度字段
- ✅ **HSMS-SS 传输**: TCP framing、SELECT/DESELECT/LINKTEST、T3/T5/T6/T7/T8 定时器
- ✅ **SECS-I 传输**: 串口半双工、ENQ/EOT/ACK/NAK 握手、分包/重组、T1/T2/T3/T4 超时
- ✅ **协议状态机**: SystemBytes 管理、请求-响应匹配、消息路由
- ✅ **协程化设计**: 基于 ASIO 协程，避免回调地狱
- ✅ **零拷贝优化**: 流式编解码，最小化内存分配
- ✅ **现代错误处理**: 全局使用 `std::error_code`，无异常
- ✅ **高测试覆盖**: 所有模块 ≥90% 行覆盖率

## 快速开始

### 系统要求

- **编译器**: GCC ≥11 / Clang ≥14 / MSVC ≥19.30
- **CMake**: ≥3.20
- **依赖**: ASIO standalone ≥1.28.0（已包含在 `third_party/`）

### 构建

```bash
# 克隆仓库
git clone <repository-url>
cd secs_lib

# 配置（自动启用测试）
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release

# 编译
cmake --build build -j

# 运行测试
ctest --test-dir build --output-on-failure
```

### 快速示例

#### SECS-II 编解码

```cpp
#include <secs/ii/codec.hpp>
#include <secs/ii/item.hpp>
#include <iostream>

int main() {
    using namespace secs::ii;

    // 构造消息: <L[2] <A "Hello"> <U2 12345>>
    Item msg = List{
        Ascii{"Hello"},
        U2{12345}
    };

    // 编码
    std::vector<byte> encoded;
    auto ec = encode(msg, encoded);
    if (ec) {
        std::cerr << "编码失败: " << ec.message() << '\n';
        return 1;
    }

    // 解码
    Item decoded;
    std::size_t consumed;
    ec = decode_one(bytes_view{encoded.data(), encoded.size()}, decoded, consumed);
    if (ec) {
        std::cerr << "解码失败: " << ec.message() << '\n';
        return 1;
    }

    std::cout << "编解码成功，消耗 " << consumed << " 字节\n";
    return 0;
}
```

#### HSMS 客户端

```cpp
#include <secs/hsms/session.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>

asio::awaitable<void> run_hsms_client() {
    auto ex = co_await asio::this_coro::executor;

    // 创建会话
    secs::hsms::SessionOptions opts;
    opts.session_id = 100;
    opts.t3 = std::chrono::seconds{30};

    secs::hsms::Session session{ex, opts};

    // 连接到设备
    asio::ip::tcp::endpoint ep{
        asio::ip::make_address("192.168.1.100"),
        5000
    };

    auto ec = co_await session.async_connect_active(ep);
    if (ec) {
        std::cerr << "连接失败: " << ec.message() << '\n';
        co_return;
    }

    std::cout << "HSMS 连接成功\n";

    // 运行会话（接收消息）
    co_await session.async_run();
}

int main() {
    asio::io_context ioc;
    asio::co_spawn(ioc, run_hsms_client(), asio::detached);
    ioc.run();
    return 0;
}
```

更多示例见 [examples/](examples/) 目录。

## 项目结构

```
secs_lib/
├── include/secs/        # 公共头文件
│   ├── core/           # 基础设施（Buffer, Event, Error）
│   ├── ii/             # SECS-II 编解码
│   ├── secs1/          # SECS-I 串口传输
│   ├── hsms/           # HSMS TCP 传输
│   └── protocol/       # 协议状态机和路由
├── src/                # 实现代码
├── tests/              # 单元测试
├── examples/           # 使用示例
└── third_party/        # 第三方依赖（ASIO）
```

## API 概览

### SECS-II 数据模型

```cpp
namespace secs::ii {

// 数据项类型（std::variant）
using Item = std::variant<
    List,      // 嵌套列表
    Ascii,     // ASCII 字符串
    Binary,    // 二进制数据
    Boolean,   // 布尔数组
    I1, I2, I4, I8,  // 有符号整数
    U1, U2, U4, U8,  // 无符号整数
    F4, F8           // 浮点数
>;

// 编解码 API
std::error_code encode(const Item& item, std::vector<byte>& out);
std::error_code decode_one(bytes_view in, Item& out, std::size_t& consumed);

}  // namespace secs::ii
```

### HSMS 会话

```cpp
namespace secs::hsms {

class Session {
public:
    // 主动连接（客户端）
    asio::awaitable<std::error_code> async_connect_active(
        const asio::ip::tcp::endpoint& peer);

    // 被动监听（服务器）
    asio::awaitable<std::error_code> async_accept_passive(
        asio::ip::tcp::acceptor& acceptor);

    // 发送数据消息
    asio::awaitable<std::error_code> async_send_data(
        uint8_t stream, uint8_t function,
        bytes_view body, uint32_t system_bytes);

    // 运行会话（接收消息）
    asio::awaitable<void> async_run();
};

}  // namespace secs::hsms
```

### 协议层会话

```cpp
namespace secs::protocol {

class Session {
public:
    // 构造（支持 HSMS 或 SECS-I）
    explicit Session(std::shared_ptr<hsms::Session> transport);
    explicit Session(std::shared_ptr<secs1::StateMachine> transport);

    // 发送消息（无需等待回复）
    asio::awaitable<std::error_code> async_send(
        uint8_t stream, uint8_t function, bytes_view body);

    // 请求-响应（自动匹配 SystemBytes，支持 T3 超时）
    asio::awaitable<std::error_code> async_request(
        uint8_t stream, uint8_t function, bytes_view body,
        core::FixedBuffer& reply_body, core::duration timeout);

    // 注册消息处理器（按 Stream/Function 路由）
    void register_handler(
        uint8_t stream, uint8_t function,
        std::function<asio::awaitable<bytes_view>(bytes_view)> handler);

    // 运行会话
    asio::awaitable<void> async_run();
};

}  // namespace secs::protocol
```

## 测试覆盖率

| 模块 | 行覆盖率 | 函数覆盖率 | 分支覆盖率 |
|------|----------|-----------|-----------|
| **core** | 96% | 97% | 92% |
| **secs1** | 95% | 97% | 93% |
| **ii** (SECS-II) | 90% | 100% | 78% |
| **hsms** | 99% | 97% | 100% |
| **protocol** | 97% | 97% | 93% |
| **总计** | **93%+** | **97%+** | **90%+** |

生成覆盖率报告：

```bash
cmake -S . -B build -DSECS_ENABLE_COVERAGE=ON
cmake --build build -j
ctest --test-dir build
make -C build coverage  # 生成 HTML 报告到 build/coverage.html
```

## 开发指南

### 编译选项

```bash
# 启用测试（默认开启）
cmake -S . -B build -DSECS_ENABLE_TESTS=ON

# 启用覆盖率
cmake -S . -B build -DSECS_ENABLE_COVERAGE=ON

# Release 构建
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release

# 指定 ASIO 路径（可选）
cmake -S . -B build -DSECS_ASIO_ROOT=/path/to/asio/include
```

### 运行特定测试

```bash
# 运行所有测试
ctest --test-dir build --output-on-failure

# 运行 SECS-II 测试
ctest --test-dir build -R secs2

# 运行 HSMS 测试
ctest --test-dir build -R hsms

# 详细输出
ctest --test-dir build --verbose
```

### IDE 集成

项目已配置 `compile_commands.json` 生成（根目录符号链接），支持：
- **clangd**: 代码补全、跳转、重构
- **VSCode**: 通过 C++ 插件 + clangd
- **CLion**: 原生 CMake 支持
- **Vim/Neovim**: 通过 LSP 客户端

## 性能特点

- **零拷贝**: SECS-II 编解码直接操作缓冲区，避免临时对象
- **内存优化**: 预分配 inline buffer（8KB），减少小消息的堆分配
- **协程调度**: ASIO strand 确保线程安全，无需额外锁
- **流式解码**: 支持部分数据增量解析，适配网络流

## 标准遵循

- **SEMI E4**: SECS-I Message Transfer (Serial)
- **SEMI E5**: SECS-II Message Content
- **SEMI E37**: HSMS - High-Speed SECS Message Services

## 贡献

欢迎提交 Issue 和 Pull Request！

## 许可证

MIT License

## 致谢

- C 语言参考实现：`c_dump/` 目录
- ASIO 库：[chriskohlhoff/asio](https://github.com/chriskohlhoff/asio)
