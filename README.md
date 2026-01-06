# SECS Library（C++20）

[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](https://en.cppreference.com/w/cpp/20)
[![CMake](https://img.shields.io/badge/CMake-3.20+-green.svg)](https://cmake.org/)

> 文档更新：2026-01-04（Codex）

基于 C++20 与 standalone Asio 协程的 SECS-I / SECS-II / HSMS (HSMS-SS) 协议栈实现，面向半导体设备通信场景。

本文档聚焦两件事：
1. 当前代码的分层架构（模块职责、依赖关系、数据流）
2. 可直接上手的使用说明（构建/链接、收发消息、写 handler、SML 用法）

如果你需要“字符画形式”的超详细架构拆解（≥1000 行），请直接跳到文末：`附录：超详细架构字符画（≥ 1000 行）`。

---

## 最近差异（与上一版相比）

本节只记录“会影响互通/集成/测试统计口径”的差异点，不贴源码（你可按路径自行阅读）。

- HSMS：未知 `SType` 不再作为解码错误；允许解析并保留原始值，供上层发送 `Reject.req`
  - 新增：`make_reject_req()`（Reject.req：`header_byte2` 承载 reason code；body 为被拒绝消息的 10B header）
  - 文件：`include/secs/hsms/message.hpp`、`src/hsms/message.cpp`
  - 覆盖测试：`tests/test_hsms_transport.cpp`（搜索 `unknown SType` / `reject`）
- HSMS：控制消息优先级与 NOT_SELECTED 写门禁
  - 变化：写队列拆分 control/data，控制消息优先写出；NOT_SELECTED 期间可禁用 data 写入并快速失败排队中的 data（避免控制流期间 data 抢写）
  - 文件：`include/secs/hsms/connection.hpp`、`src/hsms/connection.cpp`
  - 覆盖测试：`tests/test_hsms_transport.cpp`（搜索 `disable_data_writes` / `NOT_SELECTED`）
- HSMS：`Deselect.req` 行为调整：不再断线，改为进入 NOT_SELECTED；NOT_SELECTED 期间禁止发送 data message
  - 文件：`include/secs/hsms/session.hpp`、`src/hsms/session.cpp`
  - 覆盖测试：`tests/test_hsms_transport.cpp`（搜索 `deselect`）
- HSMS：LINKTEST 连续失败阈值可配置
  - 新增字段：`SessionOptions::linktest_max_consecutive_failures`（默认 1：一次失败即断线，保持旧行为）
  - 文件：`include/secs/hsms/session.hpp`、`src/hsms/session.cpp`
- SECS-II：解码资源限制升级为可配置 `DecodeLimits`（替代固定深度上限），并补充资源错误码
  - 新增：`decode_one(..., const DecodeLimits&)`；错误码 `list_too_large` / `payload_too_large` / `total_budget_exceeded` / `out_of_memory`
  - 文件：`include/secs/ii/codec.hpp`、`src/ii/codec.cpp`
  - 覆盖测试：`tests/test_secs2_codec.cpp`
- 新增：审计与修复路线文档（便于追踪互通问题与覆盖缺口）
  - 目录：`docs/secs_lib_audit/`、`docs/secs_lib_fixes/`
- SECS-II（E5）on-wire 编码对齐 `c_dump/Secs_App/secs_II.c`
  - 影响点：`FormatByte` 低 2 位含义、`format_code` 码表（整数/浮点/无符号）
  - 文件：`include/secs/ii/types.hpp`、`src/ii/codec.cpp`
  - 对齐自测：`tests/test_c_dump_secsii_compat.cpp`
- SECS-I（E4）Block header/checksum 对齐 `c_dump/Secs_App/secs_I.c`
  - 对齐自测：`tests/test_c_dump_secs1_block_compat.cpp`
- `secs::protocol::Session`（SECS-I 后端）新增 R-bit（reverse_bit）方向配置
  - 新增字段：`SessionOptions::secs1_reverse_bit`（Host=0 / Equipment=1）
  - 文件：`include/secs/protocol/session.hpp`、`src/protocol/session.cpp`
  - 覆盖测试：`tests/test_protocol_session.cpp`（搜索 `reverse_bit`）
- 覆盖率统计口径调整：`coverage` 目标不再统计 `c_dump/`（参考实现不计入库覆盖率）
  - 文件：`cmake/Modules/CodeCoverage.cmake`
- 开发体验：新增根目录 `.clangd`，强制 clangd 使用 `build/` 编译数据库
  - 文件：`.clangd`

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
| c_api | `secs::c_api` | `secs::protocol` + `secs::sml` | C 语言对外接口（C ABI）：不透明句柄 + 统一错误码 + 内存释放契约 + 内置 io 线程上下文 |

### 目录结构

```
secs_lib/
├── CMakeLists.txt
├── include/secs/               # 公共头文件
│   ├── c_api.h                 # C 语言对外接口（C ABI）
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
- standalone Asio：
  - 优先使用 `third_party/asio/`（如果你拉了 submodule / 放了 vendored 代码）
  - 也支持 `-DSECS_ASIO_ROOT=/path/to/asio/include` 指定外部 Asio
  - 若以上都不存在：
    - 顶层构建默认会自动下载（`SECS_FETCH_ASIO` 默认为 ON）
    - 作为子项目被 `add_subdirectory()` 引入时默认不自动下载（`SECS_FETCH_ASIO` 默认为 OFF）

### Asio 获取策略与常见报错

本仓库只使用 standalone Asio（头文件库），解析优先级如下：

1. vendored：`third_party/asio/asio/asio/include/`
2. 外部指定：`-DSECS_ASIO_ROOT=/path/to/asio/include`
3. 自动拉取：`-DSECS_FETCH_ASIO=ON`（需要网络；使用 CMake `FetchContent`）

如果你遇到配置错误：`Standalone Asio not found`，按你的环境选择其一即可：

- 有网络：`cmake -S . -B build -DSECS_FETCH_ASIO=ON`
- 无网络/内网：准备好 Asio include 目录后 `-DSECS_ASIO_ROOT=...`
- 想 vendoring：把 asio 仓库（或其 include 目录）放到 `third_party/asio/` 的典型结构下

补充：
- 子项目场景如果也想自动下载，可在主工程里设置：`set(SECS_FETCH_ASIO ON CACHE BOOL "" FORCE)` 再 `add_subdirectory(secs_lib)`

### 构建本仓库（开发/跑测试）

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

### 嵌入式/旧运行库环境：静态链接 C++ 运行库（推荐）

如果目标板子的 `libstdc++`/`libgcc` 版本较旧，建议在配置时开启静态链接：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DSECS_STATIC_CPP_RUNTIME=ON
```

可选：尝试“全静态”（会额外添加 `-static`，在 glibc 环境可能不可用）：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DSECS_FULLY_STATIC=ON
```

说明：
- 以上开关需要你的工具链/SDK 提供对应的静态库（例如 `libstdc++.a`、`libgcc.a`）。
- 默认情况下 `SECS_STATIC_CPP_RUNTIME` 仅在交叉编译（`CMAKE_CROSSCOMPILING=ON`）时为 ON；你也可以显式设置 ON/OFF。

### 在 `remote_develop-petalinux-dev` Docker 镜像里验证（可选）

如果你在宿主机上有 Docker 权限，可以用该镜像做一次“静态 C++ 运行库”验证构建：

```bash
docker run --rm -it \
  -v "$PWD:/workspace" -w /workspace \
  remote_develop-petalinux-dev \
  bash -lc '
    cmake -S . -B build_docker_static \
      -DCMAKE_BUILD_TYPE=Release \
      -DSECS_ENABLE_TESTS=OFF \
      -DSECS_BUILD_EXAMPLES=ON \
      -DSECS_STATIC_CPP_RUNTIME=ON \
      -DSECS_FETCH_ASIO=OFF \
      -DSECS_ASIO_ROOT=/workspace/build/_deps/secs_asio_fc-src/asio/include
    cmake --build build_docker_static -j
    readelf -d build_docker_static/examples/secs2_simple | rg NEEDED || true
  '
```

注意：
- 上面的 `SECS_ASIO_ROOT` 示例复用了本仓库现有的 `build/_deps`；如果你没有该目录，请改成你环境里实际的 Asio include 路径，或准备好 vendored Asio。
- 如果 `docker run` 报 “permission denied while trying to connect to the Docker daemon socket”，需要在宿主机上授予 Docker 权限（例如把当前用户加入 `docker` 组并重新登录）。

### 作为子项目集成到你的工程（推荐）

推荐两种集成方式：

```cmake
# 你的工程 CMakeLists.txt
add_subdirectory(path/to/secs_lib)

add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE secs::protocol) # 只用 SECS-II 则改为 secs::ii
```

如果你的工程是纯 C（`.c`）源文件，也可以链接 C ABI：

```cmake
add_executable(my_c_app main.c)
target_link_libraries(my_c_app PRIVATE secs::c_api)

# 由于底层实现是 C++20，链接阶段必须使用 C++ 链接器。
# CMake 的简便写法：强制该可执行程序用 CXX 链接。
set_target_properties(my_c_app PROPERTIES LINKER_LANGUAGE CXX)
```

可选：如果你想强制使用外部 Asio（而不是 third_party），配置时加：

```bash
cmake -S . -B build -DSECS_ASIO_ROOT=/path/to/asio/include
```

如果你在“子项目集成”场景下没有 vendored Asio，也不想额外安装 Asio，可以让主工程开启自动拉取：

```bash
cmake -S . -B build -DSECS_FETCH_ASIO=ON
```

### 安装并通过 find_package 集成（可选）

本仓库提供了 `install()` 与 CMake 包配置文件（`find_package(secs CONFIG)`），适合：

- 你希望“先安装到某个前缀”，再在多个项目里复用
- 你不想把本仓库作为子目录加入主工程

安装：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DSECS_ENABLE_INSTALL=ON
cmake --build build -j
cmake --build build --target install
```

在你的工程里使用：

```cmake
find_package(secs CONFIG REQUIRED)

add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE secs::protocol)
```

说明：

- 如果 Asio 来自 vendored/FetchContent，安装包会把 `include/secs/` 以及 Asio 的头文件一起安装到 `${CMAKE_INSTALL_PREFIX}/include`，因此消费者不需要额外准备 Asio。
- 如果你通过 `SECS_ASIO_ROOT` 使用外部 Asio，`install()` 不会把外部 Asio 头文件复制进安装前缀；消费者需要自行提供 Asio。

### 常用 CMake 选项

```bash
# 单元测试（默认：顶层工程 ON，作为子项目 OFF）
cmake -S . -B build -DSECS_ENABLE_TESTS=ON

# 示例程序（默认：顶层工程 ON，作为子项目 OFF）
cmake -S . -B build -DSECS_BUILD_EXAMPLES=ON

# 性能基准（默认 OFF）
cmake -S . -B build -DSECS_BUILD_BENCHMARKS=ON

# 覆盖率（默认 OFF）
cmake -S . -B build -DSECS_ENABLE_COVERAGE=ON

# 将警告视为错误（默认：顶层工程 ON，作为子项目 OFF）
cmake -S . -B build -DSECS_ENABLE_WERROR=ON
```

### C 语言集成（C ABI / 对外接口）

本仓库提供一个“可被纯 C 工程调用”的对外接口层（C ABI），用于把内部 C++20 协程/类型系统封装成不透明句柄 + 阻塞式/可组合的 C 函数。

你需要看的文件位置（按重要性）：

- 头文件（接口契约）：`include/secs/c_api.h`
- 实现（封装细节/线程模型/异常屏蔽）：`src/c_api.cpp`
- C 侧用法与恶意用例（最推荐当文档读）：`tests/test_c_api.c`
- CMake target 定义与依赖：`CMakeLists.txt`（搜索 `secs_c_api` / `secs::c_api`）

关键设计约定（必须遵守）：

- 句柄模型：所有对外对象都是不透明句柄（例如 `secs_context_t` / `secs_ii_item_t` / `secs_hsms_session_t` / `secs_protocol_session_t`），只能通过对应的 `*_destroy()` 释放。
- 错误模型：统一用 `secs_error_t { value, category }` 表达；`value==0` 表示成功；`category` 用于区分错误域（见 `include/secs/c_api.h` 的注释）。
- 内存模型：凡是“库分配并返回给调用方”的内存（字符串、编码输出、copy 输出等），必须使用 `secs_free()` 释放（不要用 `free()` / `delete`）。
- 线程模型：必须先 `secs_context_create()` 创建上下文；内部会启动 1 个 io 线程运行 `asio::io_context`。
  - 部分 API 为阻塞式：会把协程调度到 io 线程执行，并在调用线程等待结果。
  - 防误用：如果在库内部回调线程（io 线程）调用这些阻塞式 API，会返回 `SECS_C_API_WRONG_THREAD`（避免死锁）。
- 参数约束：
  - 数值 Item 创建函数允许 `v==NULL && n==0`（表示空数组），但不允许 `v==NULL && n>0`。
  - `secs_protocol_session_create_from_hsms(ctx, hsms, ...)` 要求 `ctx` 与 `hsms` 创建时使用的 ctx 完全一致，否则会返回 `SECS_C_API_INVALID_ARGUMENT`。

按功能分组的入口（对应文件都在 `include/secs/c_api.h`）：

- SECS-II：`secs_ii_item_*` + `secs_ii_encode` / `secs_ii_decode_one`（可选：`secs_ii_decode_one_with_limits` + `secs_ii_decode_limits_init_default`）
- SML：`secs_sml_runtime_*`
- HSMS：`secs_hsms_connection_*` + `secs_hsms_session_*`（可选：`secs_hsms_session_create_v2` 用于配置 LINKTEST 连续失败阈值）
- 协议层：`secs_protocol_session_*`（含 handler 注册、send/request）

#### 最小调用顺序（C，不贴源码）

你可以把 C ABI 当成“阻塞式、句柄化”的门面层，典型流程如下（完整可运行用法见 `tests/test_c_api.c`）：

1. 初始化：
   - `secs_context_create(&ctx)`：创建上下文（内部启动 1 个 io 线程）
2. 构造/解析 SECS-II（可选）：
   - `secs_ii_item_create_*()` 构造 `secs_ii_item_t*`
   - `secs_ii_encode(item, &bytes, &n)` 得到编码后的 payload（`bytes` 用 `secs_free` 释放）
   - `secs_ii_decode_one(bytes, n, &consumed, &out_item)` 解析一个 Item（流式）
3. 建立 HSMS 会话（两种方式二选一）：
   - 真实网络：`secs_hsms_session_create()` → `secs_hsms_session_open_active_ip()`
   - 单测/仿真：`secs_hsms_connection_create_memory_duplex()` 造出一对“内存互联”连接，再分别注入到 active/passive：
     - `secs_hsms_session_open_active_connection(client, &client_conn)`
     - `secs_hsms_session_open_passive_connection(server, &server_conn)`
4. 协议层（推荐给业务用的统一入口）：
   - `secs_protocol_session_create_from_hsms(ctx, hsms, session_id, ...)`
   - `secs_protocol_session_set_handler(stream,function, cb, user_data)` 注册处理器（回调在 io 线程触发）
   - 主动发送/请求：
     - `secs_protocol_session_send(...)`（W=0，不等待回应）
     - `secs_protocol_session_request(...)`（W=1，等待 secondary；reply 用 `secs_data_message_free` 释放）
5. 退出与释放：
   - `secs_protocol_session_stop()` / `secs_hsms_session_stop()`（非阻塞，可在任意线程调用）
   - `*_destroy()` 销毁句柄；任何返回给调用方的堆内存统一 `secs_free()` 释放

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
- `decode_one(in_span, out_item, consumed, limits)`：带资源限制的解码（用于不可信输入）

#### 最小可运行示例（不贴源码，只给阅读入口）

建议按“示例 → 头文件 → 实现 → 单测”的顺序阅读：

- 示例程序：`examples/secs2_simple.cpp`
- 类型定义：`include/secs/ii/item.hpp`（`secs::ii::Item` 与各类工厂函数）
- 编解码接口：`include/secs/ii/codec.hpp`（`encode` / `decode_one` / `encode_to` / `encoded_size`）
- 具体实现：`src/ii/item.cpp`、`src/ii/codec.cpp`
- 单元测试：`tests/test_secs2_codec.cpp`（覆盖正常/异常/边界输入）

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

备注（近期行为调整）：

- 未知 `SType` 不再作为解码错误：允许解析并保留原始值，供上层发送 `Reject.req`
- `Deselect.req` 不再断线：会话进入 NOT_SELECTED；NOT_SELECTED 期间禁止发送 data message（会返回参数错误）

#### 最小阅读路径：Active 侧发送一次 request（不贴源码）

关键调用顺序（对照源码阅读）：

1. 构造 `hsms::SessionOptions`（至少设置 data message 的 `session_id`（DeviceID，0..32767）；HSMS-SS 控制消息 SessionID 固定为 `0xFFFF`）
2. 构造 `hsms::Session`（需要 `asio::any_io_executor`）
3. `async_open_active(endpoint)`：TCP 建连 + `SELECT` 握手，成功后进入 selected
4. `async_request_data(stream,function,body)`：发送 data primary（W=1），等待同 `system_bytes` 的 data secondary（受 T3/timeout 控制）
5. 业务侧对 `reply.body` 调 `ii::decode_one()` 还原 `ii::Item`

建议直接从这些文件开始读：

- API 与注释：`include/secs/hsms/session.hpp`、`include/secs/hsms/message.hpp`
- 核心实现：`src/hsms/session.cpp`、`src/hsms/connection.cpp`
- 可运行示例：`examples/hsms_client.cpp`
- 单元测试：`tests/test_hsms_transport.cpp`（含超时/断连/控制事务覆盖）

#### Active（客户端）典型流程

1. 构造 `hsms::SessionOptions`（至少设置 data message 的 `session_id`（DeviceID，0..32767）；HSMS-SS 控制消息 SessionID 固定为 `0xFFFF`）
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
- 若你在 POSIX 平台对接真实串口/虚拟串口（pty），可直接使用内置工具类：
  - `secs::secs1::PosixSerialLink`：`include/secs/secs1/posix_serial_link.hpp`
  - 便捷打开：`secs::secs1::PosixSerialLink::open(ex, path, baud)`
- 若你在非 POSIX 平台，仍需要自行实现一个 `Link`（可参考 `Link` 接口与上述实现）。

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

补充（仅 SECS-I 后端）：

- SECS-I 的 Block Header 含 R-bit（reverse_bit，方向位）
- 你需要按角色设置 `protocol::SessionOptions::secs1_reverse_bit`：
  - Host -> Equipment：`false`（R=0）
  - Equipment -> Host：`true`（R=1）
- 参考：`include/secs/secs1/block.hpp`、`include/secs/protocol/session.hpp`

#### 最小代码片段：注册一个 handler，并发起一次 `async_request`

本节不在 README 里贴 C++ 源码（你已明确希望自行阅读），只给“最短阅读路径 + 读代码时的跟踪顺序”：

最短阅读路径（从易到难）：

- 可运行示例：`examples/typed_handler_example.cpp`
- 单元测试：`tests/test_protocol_session.cpp`、`tests/test_typed_handler.cpp`
- API 定义：`include/secs/protocol/session.hpp`、`include/secs/protocol/router.hpp`
- 具体实现：`src/protocol/session.cpp`、`src/protocol/router.cpp`、`src/protocol/system_bytes.cpp`

读代码时建议按这个顺序跟流程：

1. 先确保后端会话可用（HSMS：selected；SECS-I：链路就绪）
2. 构造 `protocol::Session` 绑定后端
3. 通过 `router().set(stream,function, ...)` 注册 handler（只注册 primary：奇数 function）
4. 并发运行 `async_run()`：持续接收入站消息、唤醒 pending 请求、调用 handler 并自动回包
5. 业务侧用 `async_send()`（W=0）或 `async_request()`（W=1）收发数据消息

#### Router handler 的签名

类型定义见：`include/secs/protocol/router.hpp`（搜索 `HandlerResult` / `Handler`）。

当收到 primary 且 `W-bit=1` 时：

- 框架调用 handler
- handler 返回 `response body`（仅 body，不需要你拼 header）
- `protocol::Session` 自动发送 `secondary (function+1, W=0, system_bytes 同请求)`

当报文种类很多、你不想为每个 SxFy 单独注册 handler 时：

- 你可以注册一个 default handler：`router().set_default(...)`
- 框架在未找到精确 (stream,function) handler 时会回退到 default handler
- default handler 若返回非 OK（error_code!=0），框架将不回包（便于“只处理部分报文”）

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

本节同样不贴源码，直接给阅读入口：

- API 与注释：`include/secs/sml/runtime.hpp`
- 语法/AST：`include/secs/sml/parser.hpp`、`include/secs/sml/ast.hpp`
- 具体实现：`src/sml/lexer.cpp`、`src/sml/parser.cpp`、`src/sml/runtime.cpp`
- 单元测试：`tests/test_sml_parser.cpp`（语法覆盖 + 边界/恶意输入）

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
./build/examples/secs1_loopback
./build/examples/secs1_serial_server <tty_path>   # UNIX
./build/examples/secs1_serial_client <tty_path>   # UNIX
```

示例说明见：`examples/README.md`

### 运行测试

```bash
ctest --test-dir build --output-on-failure
```

可按名字过滤：

```bash
ctest --test-dir build -R secs2_codec
ctest --test-dir build -R c_dump_secsii_compat
ctest --test-dir build -R c_dump_secs1_block_compat
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
