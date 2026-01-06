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

<!-- SECS_LIB_ASCII_ARCH_START -->

## 附录：超详细架构字符画（≥ 1000 行）

> 生成时间：2025-12-31（Codex）
>
> 你反馈“不要把源码整段贴进 README”。本附录已改为：
> - 只用字符画/文字解释架构与实现思路；
> - 每个点都标注“去哪个文件看实现”；
> - 不再包含任何完整源码原文。

+============================================================================+
|导航：如何在 1000+ 行里定位你想看的部分                                                     |
+============================================================================+
```
建议用编辑器/浏览器的搜索（Ctrl+F）：
  - 搜 “### 7) [secs::core]”    -> 基础设施
  - 搜 “### 8) [secs::ii]”      -> SECS-II Item/编解码
  - 搜 “### 9) [secs::secs1]”   -> SECS-I 状态机/Block
  - 搜 “### 10) [secs::hsms]”   -> HSMS 会话/连接层
  - 搜 “### 11) [secs::protocol]” -> 统一协议层
  - 搜 “### 12) [secs::sml]”    -> SML 解析/运行时
  - 搜 “### 13) 测试与恶意用例” -> 测试组织
  - 搜 “### 14) 文件索引”       -> 直接跳文件路径

本附录的目标不是“读完”，而是让你能：
  1) 先建立整体模型
  2) 再按 file path 精准跳到实现
```

### 0) 图例（读图约定）
```
符号约定：
  [模块]            ：一个命名空间 + CMake target 边界（架构层级）
  (类型/函数)        ：关键类型或关键 API 名称（不贴实现体）
  ->                 ：调用/数据流方向
  ==(sb)==>          ：SystemBytes(system_bytes) 关联请求-响应
  ==(Tn)==>          ：超时/定时器边界（T1~T8）

并发约定（很重要）：
  - “读”通常由单协程串行驱动，避免并发读同一连接/链路。
  - “写”若允许多协程并发，内部会做串行化（例如 hsms::Connection）。
  - 等待/唤醒通过 secs::core::Event（set/reset/cancel/timeout）完成。

错误模型：
  - 绝大多数 API 返回 std::error_code（避免异常路径）。
  - core::errc：跨模块共用（timeout/cancelled/buffer_overflow/invalid_argument）。
```

### 1) 总览：分层、职责、边界（超级详细版）
```
┌──────────────────────────────────────────────────────────────────────────────┐
│                                 你的业务代码                                  │
│  - 你决定角色：Host / Equipment / Simulator                                  │
│  - 你决定 transport：HSMS(TCP) / SECS-I(serial)                              │
│  - 你决定 payload：如何构造/解析 SECS-II Item                                 │
│  - 你决定 handler：收到 primary 后如何生成 secondary                           │
└──────────────────────────────────────────────────────────────────────────────┘
                 |
                 | (对上层：统一的协议 API；对下层：封装不同传输语义)
                 v
┌──────────────────────────────────────────────────────────────────────────────┐
│ [secs::protocol] 统一协议层                                                    │
│  目标：让上层不关心“到底是 HSMS 还是 SECS-I”，只关心 (S,F,W,sb,body)。          │
│                                                                                │
│  你会用到：                                                                      │
│    (protocol::Session)  async_send / async_request / async_run                 │
│    (protocol::Router)   set(stream,function,handler)                           │
│    (protocol::TypedHandler) 把 Item<->强类型结构 绑定为可复用 handler             │
│                                                                                │
│  你应该先看：                                                                    │
│    - 接口：include/secs/protocol/session.hpp                                   │
│    - 实现：src/protocol/session.cpp                                            │
│    - 测试：tests/test_protocol_session.cpp                                     │
└──────────────────────────────────────────────────────────────────────────────┘
                 |                                        |
                 | backend=HSMS                            | backend=SECS-I
                 v                                        v
┌──────────────────────────────────────┐        ┌──────────────────────────────────────┐
│ [secs::hsms] HSMS-SS（TCP，全双工）    │        │ [secs::secs1] SECS-I（串口，半双工） │
│  文件入口：                           │        │  文件入口：                           │
│   - include/secs/hsms/session.hpp     │        │   - include/secs/secs1/state_machine.hpp │
│   - include/secs/hsms/connection.hpp  │        │   - include/secs/secs1/block.hpp        │
│   - include/secs/hsms/message.hpp     │        │   - include/secs/secs1/link.hpp         │
│  实现：                               │        │  实现：                                 │
│   - src/hsms/session.cpp              │        │   - src/secs1/state_machine.cpp          │
│   - src/hsms/connection.cpp           │        │   - src/secs1/block.cpp                  │
│   - src/hsms/message.cpp              │        │   - src/secs1/link.cpp                   │
│  测试：                               │        │  测试：                                 │
│   - tests/test_hsms_transport.cpp     │        │   - tests/test_secs1_framing.cpp          │
└──────────────────────────────────────┘        └──────────────────────────────────────┘
                 |                                        |
                 | （两条传输都只搬运“SECS-II payload bytes”）
                 v                                        v
┌──────────────────────────────────────────────────────────────────────────────┐
│ [secs::ii] SECS-II（E5）：Item 模型 + 编解码                                   │
│  文件入口：include/secs/ii/item.hpp + include/secs/ii/codec.hpp                │
│  实现：src/ii/item.cpp + src/ii/codec.cpp                                      │
│  测试：tests/test_secs2_codec.cpp（含恶意输入：超深嵌套/超大 length 等）         │
└──────────────────────────────────────────────────────────────────────────────┘
                 |
                 v
┌──────────────────────────────────────────────────────────────────────────────┐
│ [secs::core] 基础设施：byte/span/buffer/event/error_code                       │
│  文件入口：include/secs/core/*.hpp，核心实现：src/core/*.cpp                    │
└──────────────────────────────────────────────────────────────────────────────┘

┌──────────────────────────────────────────────────────────────────────────────┐
│ [secs::sml]（可选）SML：用文本描述消息/规则/定时                               │
│  文件入口：include/secs/sml/*.hpp，核心实现：src/sml/*.cpp                       │
│  测试：tests/test_sml_parser.cpp（含恶意输入与边界分支）                         │
└──────────────────────────────────────────────────────────────────────────────┘
```

### 2) 目标依赖（链接边界=架构边界）
```
CMake target 与依赖（从 CMakeLists.txt 提炼）：

  [secs::core]      = secs_core
  [secs::secs1]     = secs_secs1   -> PUBLIC secs_core
  [secs::ii]        = secs_ii      -> PUBLIC secs_core
  [secs::hsms]      = secs_hsms    -> PUBLIC secs_core
  [secs::protocol]  = secs_protocol-> PUBLIC secs_core + secs_hsms + secs_secs1
  [secs::sml]       = secs_sml     -> PUBLIC secs_ii + secs_core

读代码建议：你想改哪层，就从对应 target 的 public header 开始读。
  - 入口全在 include/secs/<module>/*.hpp
```

### 3) 推荐阅读顺序（从“最外层可用 API”走到“最底层字节细节”）
```
1) 先看统一协议层（你写业务最常用）：
   - include/secs/protocol/session.hpp
   - include/secs/protocol/router.hpp
   - include/secs/protocol/typed_handler.hpp
   - include/secs/protocol/system_bytes.hpp
   - src/protocol/session.cpp

2) 再看你实际使用的传输层：
   - HSMS：include/secs/hsms/session.hpp + src/hsms/session.cpp
   - SECS-I：include/secs/secs1/state_machine.hpp + src/secs1/state_machine.cpp

3) 再看 payload（SECS-II）：
   - include/secs/ii/item.hpp
   - include/secs/ii/codec.hpp
   - src/ii/codec.cpp

4) 最后看基础设施（core）：
   - include/secs/core/event.hpp + src/core/event.cpp（协程等待模型）
   - include/secs/core/buffer.hpp + src/core/buffer.cpp（buffer 扩容/上限）
   - include/secs/core/error.hpp + src/core/error.cpp（通用 errc）

5) 若你用 SML：
   - include/secs/sml/runtime.hpp -> parse_sml()/Runtime
   - src/sml/lexer.cpp / src/sml/parser.cpp / src/sml/runtime.cpp
```

### 4) 错误码体系（你看到的 error_code 都从哪里来？）
```
┌──────────────────────────────────────────────────────────────────────────────┐
│ core::errc（跨模块通用）                                                       │
│  文件：include/secs/core/error.hpp + src/core/error.cpp                         │
│  典型值：                                                                      │
│   - ok / timeout / cancelled / buffer_overflow / invalid_argument              │
└──────────────────────────────────────────────────────────────────────────────┘

┌──────────────────────────────────────────────────────────────────────────────┐
│ ii::errc（SECS-II 编解码）                                                     │
│  文件：include/secs/ii/codec.hpp + src/ii/codec.cpp                             │
│  典型值：truncated / invalid_header / invalid_format / length_overflow ...     │
└──────────────────────────────────────────────────────────────────────────────┘

┌──────────────────────────────────────────────────────────────────────────────┐
│ secs1::errc（SECS-I block/状态机）                                              │
│  文件：include/secs/secs1/block.hpp + src/secs1/block.cpp                        │
│  典型值：invalid_block / checksum_mismatch / device_id_mismatch ...             │
└──────────────────────────────────────────────────────────────────────────────┘

┌──────────────────────────────────────────────────────────────────────────────┐
│ hsms 模块的错误：通常复用 core::errc（invalid_argument/timeout/overflow）       │
│  文件：include/secs/hsms/*.hpp + src/hsms/*.cpp                                 │
└──────────────────────────────────────────────────────────────────────────────┘

┌──────────────────────────────────────────────────────────────────────────────┐
│ sml 的错误：lexer_errc / parser_errc（用于定位文本输入问题）                    │
│  文件：include/secs/sml/lexer.hpp、include/secs/sml/parser.hpp                   │
│  测试：tests/test_sml_parser.cpp                                                 │
└──────────────────────────────────────────────────────────────────────────────┘

理解建议：先区分“哪一层报错”，再去对应文件找产生点。
```

### 5) 超时/定时器一览（T1~T8 分别在哪里生效）
```
本仓库的 “T*” 不是一个统一枚举，而是分别存在于不同层：

SECS-I（E4）：include/secs/secs1/timer.hpp（Timeouts）
  - T1 intercharacter : 每字节间隔超时（读取 block payload 时逐字节应用）
  - T2 protocol       : ENQ/EOT/ACK/NAK 的协议等待超时
  - T3 reply          : 请求-响应等待（同 SystemBytes 的 secondary）
  - T4 interblock     : 多 block 消息的后续块到达等待
  实现：src/secs1/state_machine.cpp（搜索 t1_intercharacter/t2_protocol/t3_reply/t4_interblock）

HSMS（E37）：include/secs/hsms/session.hpp（SessionOptions）
  - T3 data reply     : data message request->response 等待
  - T5 reconnect backoff : 断线/失败后的退避
  - T6 control transaction : SELECT/DESELECT/LINKTEST 等控制事务超时
  - T7 not selected   : 被动端等待 SELECT.req
  - T8 intercharacter : Connection 读字节的间隔超时（并行 timer + read_some）
  实现：src/hsms/session.cpp、src/hsms/connection.cpp

协议层 protocol：include/secs/protocol/session.hpp（SessionOptions）
  - t3：对外统一的请求超时（底层 HSMS/SECS-I 会映射到对应等待点）
  - poll_interval：async_run 的轮询（用于 stop() 检查与避免永久阻塞）
  实现：src/protocol/session.cpp
```

### 6) 协议语义总整理：primary/secondary、W-bit、SystemBytes（sb）
```
SECS 的“应用层语义”在本仓库里被抽象成 protocol::DataMessage：
  - stream/function : SxFy
  - w_bit           : 等待位（primary 是否要求 secondary）
  - system_bytes    : 请求-响应关联 token（sb）
  - body            : 纯字节（通常是 SECS-II 编码后的 payload）
  文件：include/secs/protocol/router.hpp（DataMessage）

primary/secondary 判定：
  - function 为奇数且非 0 -> primary
  - function 为偶数 -> secondary
  文件：src/protocol/session.cpp（is_primary_function / secondary_function）

W-bit 行为（库自动做什么）：
  - 收到 primary 且 w_bit=1：Router handler 产出 response body 后，库自动发送 secondary：
      function = primary_function + 1
      w_bit = 0
      system_bytes 回显原 sb
  文件：src/protocol/session.cpp（handle_inbound_）

SystemBytes 分配与回收：
  - async_send：也会分配 sb（用于 wire header），发送完即 release（不等待回应）。
  - async_request：分配 sb 并等待同 sb 的 secondary；结束后 release。
  文件：include/secs/protocol/system_bytes.hpp + src/protocol/system_bytes.cpp

HSMS vs SECS-I 在 sb 的差异：
  - 对上层无差异（都是 DataMessage.system_bytes）。
  - 对下层：HSMS 的 sb 放在 HSMS Header.system_bytes；SECS-I 的 sb 放在 block header.system_bytes。
  文件：include/secs/hsms/message.hpp、include/secs/secs1/block.hpp
```

### 7) [secs::core] 基础设施（你在所有模块里都会间接用到）
```
目标：把“字节/缓冲区/等待/错误码”这些横切关注点抽干净。

[1] bytes/span 视角：
  - core::byte / bytes_view / mutable_bytes_view
  - 文件：include/secs/core/common.hpp

[2] 错误码：
  - core::errc + make_error_code
  - 文件：include/secs/core/error.hpp + src/core/error.cpp

[3] 协程事件：
  - core::Event：set/reset/cancel + timeout
  - 用途：实现 “等待某事件发生” 且可取消/可超时
  - 文件：include/secs/core/event.hpp + src/core/event.cpp
  - 典型使用点：
      * hsms::Connection 写锁（write_gate_）
      * hsms::Session 的 selected_event_/inbound_event_
      * protocol::Session 的 Pending::ready

[4] 可扩容 buffer：
  - core::FixedBuffer：inline 预分配 + heap 扩容 + max cap
  - 文件：include/secs/core/buffer.hpp + src/core/buffer.cpp
  - 测试：tests/test_core_buffer.cpp（覆盖增长/compact/上限/溢出）

读实现建议：先看 event.cpp 顶部块注释（等待模型），再看 buffer.cpp 的 ensure_writable/grow。
```

### 8) [secs::ii] SECS-II（E5）数据模型与编解码
```
目标：提供一个“强类型 Item AST”与“流式编解码”。

[A] Item 模型（强类型 + 嵌套 List）：
  - 文件：include/secs/ii/item.hpp
  - 核心类型：ii::Item（variant），ii::List（vector<Item>）
  - 设计要点：
      * 数值/Boolean 都是 vector（允许 0..N 个值）
      * List 允许递归嵌套，适配 SECS-II 结构

[B] 编码规则（FormatByte + Length + Payload）：
  - 文件：include/secs/ii/types.hpp（format_code / kMaxLength）
  - 文件：include/secs/ii/codec.hpp（encode/decode_one API）
  - 实现：src/ii/codec.cpp
  - 关键行为：
      * List 的 Length 表示“子元素个数”（不是字节数）
      * 非 List 的 Length 表示“Payload 字节数”
      * decode_one 是流式：返回 consumed，便于从流缓冲区逐步消费
      * 具备深度上限（防恶意极深嵌套），见 src/ii/codec.cpp: kMaxDecodeDepth

[C] 恶意输入用例（强烈建议你看一遍测试）：
  - tests/test_secs2_codec.cpp
      * 超深嵌套 list（验证深度上限）
      * length 宣称超大但实际截断（验证不会分配巨量内存）

你要改 E5 编解码：只动 codec.cpp；你要改数据模型：只动 item.hpp/item.cpp。
```

### 9) [secs::secs1] SECS-I（E4）半双工传输：Block + 状态机
```
模块目标：把 SECS-I 的“握手/分包/重组/校验/超时/重试”做成可复用状态机。

[A] Block（字节级编解码）：
  - 协议常量：include/secs/secs1/block.hpp
      * kHeaderSize=10, kMaxBlockDataSize=244, frame = 1 + length + 2
  - 编码/解码：encode_block / decode_block
  - 实现：src/secs1/block.cpp
  - 测试：tests/test_secs1_framing.cpp（含恶意 frame、边界长度）

[B] Link 抽象（字节流）：
  - 接口：include/secs/secs1/link.hpp（Link）
  - 测试实现：MemoryLink（Endpoint pair）
  - 实现：src/secs1/link.cpp

[C] StateMachine（协程化状态机）：
  - 接口：include/secs/secs1/state_machine.hpp
      * async_send / async_receive / async_transact
      * State: idle/wait_eot/wait_block/wait_check
  - 实现：src/secs1/state_machine.cpp
  - 超时：include/secs/secs1/timer.hpp（Timeouts：T1/T2/T3/T4）

[D] 状态图（读实现前先把状态机装进脑子）：
  发送（async_send）：
    idle -> wait_eot -> wait_check -> idle
      |       |            |
      |       |            +-- 每个 block 等 ACK/NAK ==(T2)==> 重试/失败
      |       +-- ENQ 后等 EOT/ACK ==(T2)==> 重试/失败
      +-- 入参校验（必须 idle）

  接收（async_receive）：
    idle -> wait_block -> idle
      |       |
      |       +-- 读长度字节 ==(T2 或 T4)==> 再逐字节读 payload ==(T1)==>
      +-- 等 ENQ（忽略噪声）

你想修改 E4 行为：优先改 state_machine.cpp；你想修改帧格式：改 block.hpp/block.cpp。
```

### 10) [secs::hsms] HSMS-SS（E37）全双工会话：framing + 选择状态机
```
模块目标：
  - Connection：只负责“字节流 <-> HSMS 帧”的 framing
  - Session：负责“连接管理 + SELECT/DESELECT/LINKTEST/SEPARATE 控制流 + 定时器”

[A] HSMS 消息模型（Header/Message/Frame）：
  - 接口：include/secs/hsms/message.hpp
      * Header：session_id/header_byte2/3/p_type/s_type/system_bytes
      * Message：Header + body(bytes)
      * encode_frame/decode_frame/decode_payload
      * kMaxPayloadSize=16MB（防止恶意 length 触发巨量分配）
  - 实现：src/hsms/message.cpp
  - 测试：tests/test_hsms_transport.cpp（含恶意长度/非法 SType 等）

[B] Connection（framing + 读写）：
  - 接口：include/secs/hsms/connection.hpp
      * Stream 抽象：方便用内存 stream 做单测
      * Connection::async_read_message(): 读 length(4B) -> 读 payload -> decode_payload
      * Connection::async_write_message(): encode_frame -> 写整帧（内部写锁串行化）
      * T8：读字节间隔超时（timer vs read_some 并行竞速）
  - 实现：src/hsms/connection.cpp

[C] Session（选择状态机 + 控制事务 pending）：
  - 接口：include/secs/hsms/session.hpp
      * SessionState: disconnected/connected/selected
      * async_open_active/passive + async_run_active
      * async_receive_data / async_request_data
      * async_linktest / async_wait_selected
  - 实现：src/hsms/session.cpp
  - 关键并发模型：
      * reader_loop_ 单协程串行读取 connection_
      * pending_[system_bytes] 用 Event 唤醒控制/数据事务
      * inbound_data_ 队列存放未被 pending 消费的 data message

[D] HSMS 状态机粗图：
  disconnected --(connect)--> connected --(SELECT 完成)--> selected
      ^                             |                        |
      |                             +--(T7 超时/拒绝)--------+
      +--(SEPARATE/错误/stop)---------------------------------+

读实现建议：先看 src/hsms/session.cpp 顶部块注释（并发/队列/pending 模型）。
```

### 11) [secs::protocol] 统一协议层：把 HSMS/SECS-I 变成同一种 DataMessage
```
模块目标：让上层“写一次业务逻辑”，可切换 HSMS/SECS-I backend。

[A] DataMessage + Router：
  - DataMessage：include/secs/protocol/router.hpp
  - Router：include/secs/protocol/router.hpp + src/protocol/router.cpp
      * key=(stream<<8|function) 精确匹配（不支持通配符）
      * handler 签名：asio::awaitable<pair<error_code, vector<byte>>>(const DataMessage&)
  - 你写业务 handler：从这里开始（router.set）

[B] TypedHandler：
  - 文件：include/secs/protocol/typed_handler.hpp
  - 目标：把 “bytes -> Item -> 强类型请求 -> 强类型响应 -> Item -> bytes” 固化为可复用基类
  - 你只需要实现：
      * TReq::from_item(item) -> optional<TReq>
      * TRsp::to_item() -> Item
      * handler.handle(req, raw)
  - 参考：examples/typed_handler_example.cpp、tests/test_typed_handler.cpp

[C] SystemBytes：
  - 文件：include/secs/protocol/system_bytes.hpp + src/protocol/system_bytes.cpp
  - 目标：给请求分配唯一 sb，收到 secondary 时匹配唤醒等待者。

[D] Session（统一 send/request/run）：
  - 文件：include/secs/protocol/session.hpp + src/protocol/session.cpp
  - HSMS backend：
      * 为避免并发读连接：async_request 会确保 async_run 只启动一次
      * pending_[sb] + Event 唤醒等待的请求
  - SECS-I backend：
      * 半双工：async_request 自己驱动 receive 循环
      * 等 secondary 的同时，也会处理插入的 primary（路由并自动回包）

读实现建议：src/protocol/session.cpp 的 async_request/handle_inbound_ 是核心。
```

### 12) [secs::sml]（可选）SML：用文本描述消息/规则/定时
```
模块目标：提供一个轻量的 SML 子集，用来：
  - 描述消息模板：name: SxFy [W] <Item>.
  - 描述条件响应：if (cond) response.
  - 描述周期发送：every N send name.

[A] Token（词法产物）：
  - 文件：include/secs/sml/token.hpp
  - 你可以把它当成“最小 DSL 词法集合”

[B] Lexer（源文本 -> tokens）：
  - 文件：include/secs/sml/lexer.hpp + src/sml/lexer.cpp
  - 关键点：
      * 支持块注释/行注释
      * 未闭合块注释/字符串会返回明确 lexer_errc（便于定位）
      * 0x 后无数字会报 invalid_hex_literal

[C] Parser（tokens -> AST(Document)）：
  - 文件：include/secs/sml/parser.hpp + src/sml/parser.cpp
  - 关键点：
      * 数值解析用 from_chars + 范围校验（U1/U2/U4/U8/I1.../binary byte/interval）
      * 错误码细分（invalid_stream_function/unclosed_item/invalid_condition 等）

[D] AST + Runtime：
  - AST：include/secs/sml/ast.hpp（MessageDef/Condition/TimerRule/Document）
  - Runtime：include/secs/sml/runtime.hpp + src/sml/runtime.cpp
      * build_index(): name->idx, sf->idx
      * get_message(name or sf)
      * match_response(stream,function,item): 扫描规则并匹配
  - 测试：tests/test_sml_parser.cpp（含大量恶意输入/边界分支）

如果你要扩展 DSL：先改 token/lexer/parser；如果你要改匹配策略：改 runtime。
```

### 13) 测试与恶意用例（为什么这些测试能证明实现“稳”）
```
tests/ 目录是“架构的可执行规格”，建议把它当成文档读：

核心测试文件与覆盖范围：
  - tests/test_core_buffer.cpp      : FixedBuffer 扩容/上限/溢出/compact
  - tests/test_core_event.cpp       : Event 的 set/reset/cancel/timeout 语义
  - tests/test_secs2_codec.cpp      : SECS-II 编解码（含恶意输入）
  - tests/test_secs1_framing.cpp    : SECS-I block 编解码/状态机边界（含恶意 frame）
  - tests/test_hsms_transport.cpp   : HSMS framing/长度上限/非法头部（含恶意长度）
  - tests/test_protocol_session.cpp : protocol 层在 HSMS/SECS-I 两后端的统一行为
  - tests/test_typed_handler.cpp    : TypedHandler 解码/业务/编码链路与错误传播
  - tests/test_sml_parser.cpp       : SML lexer/parser/runtime（含恶意 DSL 输入）

恶意输入的意义：
  - 不是为了“安全”，而是为了验证：
      * 边界长度不会导致异常分配/崩溃
      * 截断/非法格式能尽早失败并返回可定位的错误码
      * 深度递归有上限（不会栈溢出）
```

### 14) 文件索引（你说“给我位置我自己看”，这里是按模块汇总）

#### 文件索引：include/secs/core/*.hpp（共 4 个文件）
- `include/secs/core/buffer.hpp`
- `include/secs/core/common.hpp`
- `include/secs/core/error.hpp`
- `include/secs/core/event.hpp`

#### 文件索引：include/secs/ii/*.hpp（共 3 个文件）
- `include/secs/ii/codec.hpp`
- `include/secs/ii/item.hpp`
- `include/secs/ii/types.hpp`

#### 文件索引：include/secs/secs1/*.hpp（共 4 个文件）
- `include/secs/secs1/block.hpp`
- `include/secs/secs1/link.hpp`
- `include/secs/secs1/state_machine.hpp`
- `include/secs/secs1/timer.hpp`

#### 文件索引：include/secs/hsms/*.hpp（共 4 个文件）
- `include/secs/hsms/connection.hpp`
- `include/secs/hsms/message.hpp`
- `include/secs/hsms/session.hpp`
- `include/secs/hsms/timer.hpp`

#### 文件索引：include/secs/protocol/*.hpp（共 4 个文件）
- `include/secs/protocol/router.hpp`
- `include/secs/protocol/session.hpp`
- `include/secs/protocol/system_bytes.hpp`
- `include/secs/protocol/typed_handler.hpp`

#### 文件索引：include/secs/sml/*.hpp（共 5 个文件）
- `include/secs/sml/ast.hpp`
- `include/secs/sml/lexer.hpp`
- `include/secs/sml/parser.hpp`
- `include/secs/sml/runtime.hpp`
- `include/secs/sml/token.hpp`

#### 文件索引：src/core/*.cpp（共 3 个文件）
- `src/core/buffer.cpp`
- `src/core/error.cpp`
- `src/core/event.cpp`

#### 文件索引：src/ii/*.cpp（共 2 个文件）
- `src/ii/codec.cpp`
- `src/ii/item.cpp`

#### 文件索引：src/secs1/*.cpp（共 4 个文件）
- `src/secs1/block.cpp`
- `src/secs1/link.cpp`
- `src/secs1/state_machine.cpp`
- `src/secs1/timer.cpp`

#### 文件索引：src/hsms/*.cpp（共 4 个文件）
- `src/hsms/connection.cpp`
- `src/hsms/message.cpp`
- `src/hsms/session.cpp`
- `src/hsms/timer.cpp`

#### 文件索引：src/protocol/*.cpp（共 3 个文件）
- `src/protocol/router.cpp`
- `src/protocol/session.cpp`
- `src/protocol/system_bytes.cpp`

#### 文件索引：src/sml/*.cpp（共 3 个文件）
- `src/sml/lexer.cpp`
- `src/sml/parser.cpp`
- `src/sml/runtime.cpp`

#### 文件索引：tests/*（共 11 个文件）
- `tests/CMakeLists.txt`
- `tests/test_core_buffer.cpp`
- `tests/test_core_error.cpp`
- `tests/test_core_event.cpp`
- `tests/test_hsms_transport.cpp`
- `tests/test_main.hpp`
- `tests/test_protocol_session.cpp`
- `tests/test_secs1_framing.cpp`
- `tests/test_secs2_codec.cpp`
- `tests/test_sml_parser.cpp`
- `tests/test_typed_handler.cpp`

#### 文件索引：examples/*（共 7 个文件）
- `examples/CMakeLists.txt`
- `examples/README.md`
- `examples/hsms_client.cpp`
- `examples/hsms_server.cpp`
- `examples/secs2_simple.cpp`
- `examples/typed_handler_example.cpp`
- `examples/vendor_messages.hpp`

#### 文件索引：benchmarks/*（共 5 个文件）
- `benchmarks/CMakeLists.txt`
- `benchmarks/bench_core_buffer.cpp`
- `benchmarks/bench_hsms_message.cpp`
- `benchmarks/bench_main.hpp`
- `benchmarks/bench_secs2_codec.cpp`

#### 文件索引：cmake/*（共 2 个文件）
- `cmake/AsioStandalone.cmake`
- `cmake/Modules/CodeCoverage.cmake`

### 15) 速查：从“我调用了这个 API”到“实现在哪”
```
[1] 我想“发一个不等待回应的主消息”（W=0）
  - 入口：protocol::Session::async_send
  - 文件：include/secs/protocol/session.hpp
  - 实现：src/protocol/session.cpp（async_send -> async_send_message_）
  - HSMS 下沉：hsms::Session::async_send -> hsms::Connection::async_write_message
      * 文件：include/secs/hsms/session.hpp / src/hsms/session.cpp
      * 文件：include/secs/hsms/connection.hpp / src/hsms/connection.cpp
  - SECS-I 下沉：secs1::StateMachine::async_send
      * 文件：include/secs/secs1/state_machine.hpp / src/secs1/state_machine.cpp

[2] 我想“发一个等待回应的请求”（W=1）
  - 入口：protocol::Session::async_request
  - 实现：src/protocol/session.cpp（async_request）
      * HSMS：pending_[sb] + async_run 唤醒
      * SECS-I：请求协程自己 receive 循环

[3] 我想“注册一个 handler 自动回包”
  - 入口：protocol::Router::set(stream,function,handler)
  - 文件：include/secs/protocol/router.hpp + src/protocol/router.cpp
  - 自动回包逻辑：src/protocol/session.cpp（handle_inbound_）

[4] 我想“用 TypedHandler 把 Item 映射成强类型结构”
  - 入口：protocol::TypedHandler<TReq,TRsp> + register_typed_handler
  - 文件：include/secs/protocol/typed_handler.hpp
  - 示例：examples/typed_handler_example.cpp
  - 测试：tests/test_typed_handler.cpp

[5] 我想“只做 SECS-II 编解码”
  - 入口：ii::encode / ii::decode_one
  - 文件：include/secs/ii/codec.hpp + src/ii/codec.cpp

[6] 我想“用 SML 文本描述规则并匹配响应”
  - 入口：sml::Runtime::load + Runtime::match_response
  - 文件：include/secs/sml/runtime.hpp + src/sml/runtime.cpp
  - 解析链路：parse_sml -> Lexer::tokenize -> Parser::parse
      * 文件：include/secs/sml/runtime.hpp、include/secs/sml/lexer.hpp、include/secs/sml/parser.hpp
```

+============================================================================+
|深入补完：关键状态机 / 并发模型 / 字节级格式（不贴源码，只指路）                       |
+============================================================================+

### 16) 深入：[secs::hsms] Session 的选择状态机与事务模型（E37）

你应该看（按“从外到内”）：
- include/secs/hsms/session.hpp（SessionState/SessionOptions/对外 API/Pending）
- src/hsms/session.cpp（reader_loop_ / async_open_* / async_*_transaction_ / on_disconnected_）
- include/secs/hsms/message.hpp + src/hsms/message.cpp（SType、控制消息构造、frame 编解码）
- include/secs/hsms/connection.hpp + src/hsms/connection.cpp（framing/T8/并发写串行化）
- include/secs/hsms/timer.hpp + src/hsms/timer.cpp（定时器到 std::error_code 的映射）

#### 16.1 三态状态机（实现里的真实状态：disconnected / connected / selected）

```
                 async_open_active/passive 成功（连上 TCP + 进入 connected）
┌──────────────────────────────┐
│  SessionState::disconnected   │
└──────────────────────────────┘
              |
              v
┌──────────────────────────────┐      SELECT 成功（进入 selected）
│   SessionState::connected     │  -----------------------------------+
└──────────────────────────────┘                                     |
              |                                                       |
              | 断线/stop/SEPARATE/DESELECT/控制事务超时(T6)/数据事务超时(T3)
              |                                                       |
              v                                                       |
┌──────────────────────────────┐                                     |
│    SessionState::selected     │                                     |
└──────────────────────────────┘                                     |
              |                                                       |
              +--------------------- on_disconnected_(reason) --------+

关键字段（去看：include/secs/hsms/session.hpp）：
  - state_                 : 当前三态
  - selected_generation_   : 每次进入 selected +1；用于让旧连接周期的 linktest_loop_ 自动退出
  - pending_               : system_bytes -> Pending（控制事务/数据事务统一用）
  - inbound_data_          : 未被 pending_ 消费的数据消息队列
  - selected_event_        : 给 async_wait_selected() 用
  - inbound_event_         : 给 async_receive_data() 用
  - disconnected_event_    : 给重连时等待旧 reader_loop_ 退出用

核心承诺（去看：src/hsms/session.cpp::on_disconnected_）：
  - 断线时：会 cancel+reset 所有 Event，并 cancel 所有 Pending::ready，保证“没人永远挂起”
  - 断线时：会清空 inbound_data_ 与 pending_，避免旧连接遗留状态污染新连接
```

#### 16.2 主动端 async_open_active() 的关键时序（为什么必须“先起 reader_loop_ 再做 SELECT”）

```
入口函数：include/secs/hsms/session.hpp::async_open_active
实现位置：src/hsms/session.cpp::async_open_active

async_open_active(endpoint)
  |
  +--> Connection::async_connect(endpoint)                    (src/hsms/connection.cpp)
  |
  +--> async_open_active(Connection&& new_conn)
        |
        +-- 若旧 reader_loop_ 仍在跑 (reader_running_==true)：
        |     - connection_.async_close()
        |     - disconnected_event_.async_wait(T6)
        |     目的：避免“两个 reader_loop_ 同时读不同连接”导致状态机错乱
        |
        +-- connection_ = new_conn
        +-- reset_state_()   // 清空 pending_/inbound_data_，并把 state_ 置为 connected
        +-- start_reader_()  // co_spawn(reader_loop_)（单一读协程开始串行读取 HSMS 帧）
        |
        +-- SELECT 事务（T6）：
        |     req = make_select_req(0xFFFF, allocate_system_bytes())  // HSMS-SS：控制消息 SessionID 固定 0xFFFF
        |     async_control_transaction_(req, expected=select_rsp, timeout=T6)
        |       - pending_[sb] = Pending(expected_stype=select_rsp)
        |       - connection_.async_write_message(req)
        |       - Pending::ready.async_wait(T6)
        |       - 若 T6 超时：close + on_disconnected(timeout)（按通信失败处理）
        |
        +-- 收到 SELECT.rsp：
	      - rsp.header.header_byte2 == 0  => set_selected_()
	      - 否则 => close + on_disconnected(invalid_argument)
```

#### 16.3 被动端 async_open_passive() 的关键时序（核心：等待 reader_loop_ 把状态推进到 selected）

```
入口函数：include/secs/hsms/session.hpp::async_open_passive
实现位置：src/hsms/session.cpp::async_open_passive

async_open_passive(socket/Connection&& new_conn)
  |
  +-- 同主动端：若旧 reader_loop_ 仍在跑，先 close 并等 disconnected_event_
  |
  +-- connection_=new_conn; reset_state_(); start_reader_()
  |
  +-- selected_event_.async_wait(T7)
        - 成功：说明 reader_loop_ 已经处理了 SELECT.req 并 set_selected_()
        - 超时/取消：close + on_disconnected(ec)（避免半开状态）

对应“谁来推进 selected？”：
  - 被动端并不主动发 SELECT.req；它靠 reader_loop_ 收到对端 SELECT.req 后 set_selected_()
```

#### 16.4 reader_loop_ 的控制消息分发（只列实现中确实处理的 SType）

```
实现位置：src/hsms/session.cpp::reader_loop_
消息类型：include/secs/hsms/message.hpp::SType

读到 msg 后：
  A) msg.is_data()：
     1) fulfill_pending_(msg) 命中：唤醒对应 Pending，然后“消费掉”（不进 inbound_data_）
     2) 否则：push 到 inbound_data_ 并 inbound_event_.set()

  B) msg.is_control()：
     - select_req（对端请求选择）：
         * passive_accept_select==false：
             select_rsp(REJECT) -> close -> on_disconnected(invalid_argument) -> 退出 reader_loop_
         * 已经 selected 再收到 select_req：
             select_rsp(REJECT) -> 保持连接（不退回 disconnected）
         * session_id 不匹配：
             select_rsp(REJECT) -> close -> on_disconnected(invalid_argument) -> 退出
         * 否则：
             select_rsp(OK) -> set_selected_()

     - select_rsp（本端发起 SELECT.req 的回应）：
         * fulfill_pending_()（唤醒 async_control_transaction_）
         * rsp.session_id==0 => set_selected_()

     - deselect_req / deselect_rsp：
         * 都会 close + on_disconnected(cancelled) 并退出（实现选择“简单且收敛”）

     - linktest_req：
         * 立即回复 linktest_rsp(OK)（不进入 inbound_data_）

     - linktest_rsp：
         * fulfill_pending_()

     - separate_req：
         * close + on_disconnected(cancelled) 并退出

	     - 其它控制类型：
	         * 忽略（实现里留了扩展位，例如 Reject）
```

#### 16.5 事务模型：Pending + fulfill_pending_（控制事务与数据事务复用同一套路）

```
Pending 结构（去看：include/secs/hsms/session.hpp::Pending）：
  - expected_stype   : 期望的响应类型（select_rsp/linktest_rsp/data/...）
  - ready(Event)     : 等待点（set=完成，cancel=断线/stop）
  - ec               : 事务结果错误码
  - response         : 收到的响应 Message（若成功）

登记与唤醒（去看：src/hsms/session.cpp）：
  - async_control_transaction_ / async_data_transaction_：
      pending_[sb] = make_shared<Pending>(expected)
      connection_.async_write_message(req)
      pending->ready.async_wait(timeout)
  - reader_loop_ 收到任何消息时：
      fulfill_pending_(msg)：
        if pending_ 里存在同 system_bytes 且 expected_stype 匹配：
           pending->response = msg
           pending->ec = ok
           pending->ready.set()

“超时策略”（对齐 HSMS/HSMS-SS 语义）：
  - SELECT 控制事务 T6 超时：通信失败，断线收敛（close + on_disconnected(timeout)）
  - LINKTEST：async_linktest 返回 timeout；linktest_loop_ 按 linktest_max_consecutive_failures 决定何时断线（默认 1 次失败即断线）
  - 数据事务 T3 超时：只取消事务并返回 timeout，保持连接；迟到响应可能进入 inbound_data_（由上层决定是否丢弃/记录）
```

#### 16.6 linktest_loop_（周期心跳）与 selected_generation_（防止“重连后旧协程继续跑”）

```
触发点：src/hsms/session.cpp::set_selected_
  - state_ 从 connected -> selected 时：
      selected_generation_++（得到 gen）
      selected_event_.set()
      若 linktest_interval!=0：co_spawn(linktest_loop_(gen))

退出条件：src/hsms/session.cpp::linktest_loop_
  - state_ != selected             => 退出
  - selected_generation_ != gen    => 退出（说明已经重连进入新一代 selected）

每周期流程：
  - Timer::async_wait_for(linktest_interval)
  - async_linktest() => LINKTEST.req/rsp（T6）
  - 若失败：connection_.async_close()（后续由 reader_loop_ 感知并触发 on_disconnected_）
```

### 17) 深入：[secs::hsms] Connection 的 framing/T8/并发写串行化（为什么这样做才不会乱）

你应该看：
- include/secs/hsms/message.hpp（kLengthFieldSize/kHeaderSize/kMaxPayloadSize + encode_frame/decode_payload）
- include/secs/hsms/connection.hpp（Stream 抽象、ConnectionOptions::t8）
- src/hsms/connection.cpp（async_read_message/async_write_message/async_read_some_with_t8）

#### 17.1 TCP framing（实现的真实格式）

```
wire bytes:
  ┌───────────────┬───────────────────────────────┬───────────────────────────┐
  │ Length (4B)   │ HSMS Header (10B)             │ Body (N bytes)            │
  │ big-endian    │ session_id/h2/h3/p_type/s_type│ 原始 SECS-II payload bytes │
  └───────────────┴───────────────────────────────┴───────────────────────────┘

Length 的语义（去看：include/secs/hsms/message.hpp）：
  - Length 表示 “payload 长度” = Header(10B) + Body(N)
  - 实现保护：
      * payload_len < kHeaderSize      => invalid_argument
      * payload_len > kMaxPayloadSize  => buffer_overflow（避免恶意长度触发巨量分配）

对应实现点（去看：src/hsms/connection.cpp::async_read_message）：
  - 先读 4B length
  - 再读 payload_len 字节
  - decode_payload(payload) 得到 Message
```

#### 17.2 T8（网络字符间隔超时）的实现：并行等待读与定时器，谁先到用谁

```
配置入口：include/secs/hsms/connection.hpp::ConnectionOptions::t8
核心实现：src/hsms/connection.cpp::async_read_some_with_t8

当 t8==0：
  - 直接 stream_->async_read_some(...)

当 t8!=0：
  - co_spawn( read_task = stream_->async_read_some(...) )
  - co_spawn( timer_task = steady_timer.async_wait(t8) )
  - make_parallel_group(read_task, timer_task).async_wait(wait_for_one)
      * 若 read 先完成：返回 (ec, n)
      * 若 timer 先完成：stream_->cancel() + 返回 errc::timeout

为什么要 cancel 底层 stream？
  - 如果不 cancel，底层 read 协程可能继续挂着，导致后续 close/重连收尾困难
```

#### 17.3 并发写串行化：为什么不能让多个协程同时写 socket？

```
风险：TCP 是字节流，如果两个协程并发写入：
  - 两个 HSMS frame 的字节可能交错
  - 对端按 length framing 会被破坏（读到“混合 frame”）

实现策略（去看：src/hsms/connection.cpp::async_write_message）：
  - write_in_progress_ + write_gate_(secs::core::Event) 组成一个“协程锁”
      * 有人持锁：其它协程 co_await write_gate_.async_wait()
      * 持锁者 reset() gate，写完 set() gate 唤醒等待者
  - async_close()/cancel_and_close() 会 cancel gate，避免等待者永远睡着
```

### 18) 深入：[secs::secs1] 半双工链路：ENQ/EOT + Block + ACK/NAK + T1/T2/T4（E4）

你应该看：
- include/secs/secs1/state_machine.hpp + src/secs1/state_machine.cpp（整体算法）
- include/secs/secs1/block.hpp + src/secs1/block.cpp（字节级 block frame 格式、分包/重组）
- include/secs/secs1/link.hpp + src/secs1/link.cpp（抽象读写、测试注入延迟/丢字节）
- include/secs/secs1/timer.hpp + src/secs1/timer.cpp（超时映射）

#### 18.1 Block frame 的字节格式（实现注释里写得很全，直接按它理解）

```
去看：include/secs/secs1/block.hpp（encode_block/decode_block 相关注释）

frame:
  +---------+-------------------------------+--------------------+
  | Len(1B) | Payload = Header(10B)+Data(N) | Checksum(2B, BE)   |
  +---------+-------------------------------+--------------------+

约束：
  - 10 <= Len <= 254
  - 0 <= N <= 244（kMaxBlockDataSize）
  - Checksum 为对 Len 之后 “Len 个字节” 求和（mod 65536）

Header(10B) 的 bit 布局（去看：include/secs/secs1/block.hpp::Header 注释）：
  - Byte1: R(1b) + DeviceID[14:8](7b)
  - Byte2: DeviceID[7:0]
  - Byte3: W(1b) + Stream(7b)
  - Byte4: Function(8b)
  - Byte5: E(1b) + BlockNumber[14:8](7b)
  - Byte6: BlockNumber[7:0]
  - Byte7..10: SystemBytes（big-endian）
```

#### 18.2 SECS-I 状态机（协程版）的发送流程（async_send）

```
入口：include/secs/secs1/state_machine.hpp::async_send
实现：src/secs1/state_machine.cpp::async_send

state idle
  |
  | 1) ENQ/EOT 握手（请求占用半双工链路）
  v
state wait_eot
  - 循环 attempt < retry_limit_：
      * 发 ENQ
      * 等待对端响应（T2）
          - EOT 或 ACK => 允许发送（handshake_ok=true）
          - NAK 或 T2 超时 => 重试
          - 其它字节/其它错误 => protocol_error/底层错误

handshake_ok 后：
  2) 分块：frames = fragment_message(header, body)      (include/secs/secs1/block.hpp)
  3) 逐块发送并等待 ACK/NAK（每块 T2）：
      for frame in frames:
         state wait_check
         while attempts < retry_limit_:
            write(frame)
            read_byte(T2) -> ACK 通过
                           -> NAK/timeout 重传该块
                           -> 其它 => protocol_error

退出：成功/失败都会回到 state idle
```

#### 18.3 SECS-I 状态机的接收流程（async_receive）

```
入口：include/secs/secs1/state_machine.hpp::async_receive
实现：src/secs1/state_machine.cpp::async_receive

state idle
  |
  | 1) 等待对端 ENQ（忽略噪声字节；timeout 由入参或上层控制）
  v
收到 ENQ
  |
  | 2) 回 EOT（表示允许对端开始发送）
  v
state wait_block
  |
  | 3) 逐块读取并重组（Reassembler）
  v
while !re.has_message():
  - 读 Len(1B)：
      * 第 1 块：T2
      * 后续块：T4
  - Len 非法：回 NAK + 返回 invalid_block
  - 读 Payload+Checksum：
      * 总共读 Len+2 个字节
      * 每个字节都应用 T1（字符间超时）
  - decode_block 校验失败：
      * 回 NAK
      * nack_count++，超过 retry_limit => too_many_retries
  - re.accept(decoded) 失败：
      * 回 NAK + 返回对应错误（device_id_mismatch / block_sequence_error / ...）
  - 当前块 ok：
      * 回 ACK

完成后：返回 (header, body) 并回到 state idle
```

#### 18.4 “半双工”对上层意味着什么？

```
你应该在协议层做的事（去看：include/secs/protocol/session.hpp 注释）：
  - 不建议同时跑一个常驻 async_run() 再并发 async_request()/async_send()
  - 如果你要并发请求：最好把并发收敛到你自己的调度层，确保同一时间只有一个协程在驱动 SECS-I 收发

实现里已经做的事（去看：src/protocol/session.cpp）：
  - SECS-I 的 async_request 会自己驱动 receive 循环，并在等待 secondary 时处理插入的 primary
```

### 19) 深入：[secs::protocol] Session 的请求-响应匹配与 Router 自动回包（统一 HSMS/SECS-I）

你应该看：
- include/secs/protocol/session.hpp + src/protocol/session.cpp（核心）
- include/secs/protocol/router.hpp + src/protocol/router.cpp（路由表与 handler 约定）
- include/secs/protocol/system_bytes.hpp + src/protocol/system_bytes.cpp（SystemBytes 分配/回收）

#### 19.1 统一后的数据模型：DataMessage（你只需要关心这 5 件事）

```
DataMessage 语义（去看：include/secs/protocol/*）：
  - stream / function
  - w_bit（primary 是否要求 secondary）
  - system_bytes（请求-响应关联键）
  - body（原始 SECS-II bytes，由 secs::ii 编解码）

HSMS 与 SECS-I 在协议层看起来“长得一样”：
  - HSMS：wire=HSMS data message；system_bytes 在 HSMS header 里
  - SECS-I：wire=Block header；system_bytes 在 Block header 里
```

#### 19.2 并发模型差异：为什么 HSMS 需要 async_run，而 SECS-I 不推荐？

```
HSMS（全双工）：
  - 允许多个请求协程并发发起 async_request()
  - 但不允许多个协程并发读同一连接（会竞争）
  => 设计：只保留一个读循环 async_run() 统一接收并分发

SECS-I（半双工）：
  - “读/写”本质都在争抢同一半双工链路
  => 设计：async_request() 在等待 secondary 期间自己驱动接收（并处理插入 primary）
```

#### 19.3 HSMS 路径：async_request() -> pending_ -> async_run() 唤醒

```
实现位置：src/protocol/session.cpp

async_request(stream,function,body,timeout)
  - 分配 sb：SystemBytes::allocate(sb)
  - pending_[sb] = Pending(expected_stream=stream, expected_function=function+1)
  - ensure_hsms_run_loop_started_() 只启动一次 async_run()（co_spawn）
  - 发送主消息（W=1）：async_send_message_
  - pending->ready.async_wait(T3)
      * 收到 secondary：async_run() -> handle_inbound_ -> try_fulfill_pending_ -> ready.set()
      * 超时：返回 timeout
      * stop/断线：cancel_all_pending_ -> ready.cancel()
  - erase pending_[sb] 并 release(sb)
```

#### 19.4 SECS-I 路径：async_request() 自己收消息，同时不耽误处理插入的 primary

```
实现位置：src/protocol/session.cpp

async_request(...)：
  - 分配 sb 并发送主消息（W=1）
  - until deadline(T3):
      msg = async_receive_message_(remaining)
      if msg 是期望的 secondary（system_bytes 相等 + stream/function 匹配）：
          release(sb) 并返回
      else：
          handle_inbound_(msg)  // 可能路由到 handler 并自动回包
  - 超时：release(sb) 并返回 timeout
```

#### 19.5 Router 自动回包：你只注册 handler，其余交给库

```
实现位置：src/protocol/session.cpp::handle_inbound_

handle_inbound_(msg):
  1) 若 msg 匹配 pending：唤醒请求协程，结束
  2) 若 msg 是 secondary 但不匹配任何 pending：忽略（迟到回应/对端异常发送）
  3) 若 msg 是 primary：
       - router_.find(stream,function) 查 handler
       - handler 返回 (ec, rsp_body)
       - 若 msg.w_bit==true 且 function!=0xFF：
            自动构造 secondary(function+1, same sb) 并 async_send_message_

你要看的文件：
  - include/secs/protocol/router.hpp / src/protocol/router.cpp
  - include/secs/protocol/typed_handler.hpp（强类型映射的入口）
```

### 20) 深入：SystemBytes 分配器的“唯一性/回收/回绕”策略（为何能支撑并发请求）

你应该看：
- include/secs/protocol/system_bytes.hpp
- src/protocol/system_bytes.cpp

#### 20.1 内部数据结构（实现里的真实容器）

```
SystemBytes:
  - next_   : 下一个候选值（递增 + 回绕到 1；0 永不分配）
  - free_   : 已释放可复用的队列（优先复用）
  - in_use_ : 当前在用集合（保证同一时刻唯一）
  - mu_     : 互斥锁（允许跨线程使用，但更推荐同 executor/strand）

分配策略（去看：src/protocol/system_bytes.cpp 注释）：
  1) free_ 非空：pop_front -> 放入 in_use_ -> 返回
  2) 否则：从 next_ 开始尝试插入 in_use_
       - next_ 到 UINT32_MAX 后回绕到 1
       - 最多尝试 in_use_.size()+2 次（通常很快找到空闲）
  3) 极端情况：几乎所有 sb 都在用 => 返回 buffer_overflow

释放策略：
  - release(sb) 若 sb==0 或不在用：忽略
  - 否则：从 in_use_ 删除，并 push_back 到 free_（可复用）
```

### 21) 深入：取消/超时/错误是如何贯穿全栈的（避免协程永远挂起）

你应该看：
- include/secs/core/error.hpp + src/core/error.cpp（errc -> error_code）
- include/secs/core/event.hpp + src/core/event.cpp（Event 的 set/reset/cancel/timeout 语义）
- src/hsms/session.cpp::on_disconnected_（统一取消 pending/事件）
- src/protocol/session.cpp::cancel_all_pending_（协议层 pending 取消与 system_bytes 回收）
- src/hsms/timer.cpp、src/secs1/timer.cpp（timer -> error_code 的映射）

#### 21.1 core::Event 是全库的“等待/唤醒原语”

```
语义（去看：include/secs/core/event.hpp 注释）：
  - set()    : 置位并唤醒所有等待者；后续 wait 立即成功
  - reset()  : 清除置位；后续 wait 会阻塞
  - cancel() : 取消当前所有等待者（返回 cancelled），但不置位

在库里常见用法：
  - “等待某状态达成”：selected_event_ / disconnected_event_
  - “等待队列非空”：inbound_event_
  - “实现协程锁”：hsms::Connection::write_gate_
  - “实现 pending 唤醒”：Pending::ready
```

#### 21.2 stop()/断线时的收敛路径（保证没有悬挂协程）

```
HSMS：
  - Session::stop():
      stop_requested_=true
      connection_.cancel_and_close()
      on_disconnected_(errc::cancelled)
        * cancel+reset selected_event_/inbound_event_
        * Pending::ready.cancel()
        * 清空 pending_/inbound_data_

Protocol：
  - protocol::Session::stop():
      stop_requested_=true
      cancel_all_pending_(errc::cancelled)
        * 对每个 pending：ready.cancel() + system_bytes_.release(sb)

SECS-I：
  - async_receive/async_send 的超时直接以 error_code 形式返回给上层（上层决定是否重试/断开）
```

#### 21.3 “超时”在不同层的含义（你排障时要区分）

```
T8（hsms::Connection）：
  - 表示“字节间隔超时”（读到任何字节前不能超过 T8）
  - 触发点：async_read_some_with_t8

T6（hsms::Session 控制事务）：
  - 表示“控制交互超时”（SELECT/DESELECT/LINKTEST）
  - 实现选择：超时只返回 timeout；是否断线由调用方/策略决定（例如 linktest 连续失败阈值触发断线）

T3（hsms::Session 数据事务 / protocol::Session 请求-响应）：
  - 表示“等待回应超时”
  - HSMS 会话层：超时返回给调用者（不强制断线）
  - 协议层：超时返回给调用者（HSMS 情况下会同时 erase pending 并 release(sb)）

T1/T2/T4（secs1::StateMachine）：
  - T1：字符间超时（逐字节）
  - T2：协议响应超时（握手/ACK/NAK）
  - T4：块间超时（后续块）
```

### 22) 深入：[secs::sml] SML 从文本到匹配的完整链路（Lexer/Parser/Runtime）

你应该看：
- include/secs/sml/runtime.hpp + src/sml/runtime.cpp（load/build_index/match_response/match_condition/items_equal）
- include/secs/sml/lexer.hpp + src/sml/lexer.cpp（tokenize：注释/字符串/数字/位置）
- include/secs/sml/parser.hpp + src/sml/parser.cpp（parse：语法树、错误码与定位）
- include/secs/sml/ast.hpp（Document/MessageDef/ConditionRule/TimerRule 等 AST 定义）

#### 22.1 SML 解析管线（实现里的真实调用顺序）

```
Runtime::load(source)
  |
  +--> parse_sml(source)                                     (include/secs/sml/runtime.hpp)
        |
        +--> Lexer lexer(source)                             (include/secs/sml/lexer.hpp)
        |    +--> lexer.tokenize() -> LexResult(tokens, ec, line, col, message)
        |
        +--> Parser parser(tokens)                           (include/secs/sml/parser.hpp)
             +--> parser.parse() -> ParseResult(document, ec, line, col, message)
  |
  +--> Runtime::load(Document)                               (src/sml/runtime.cpp)
        +--> build_index()

你排障时的入口：
  - lex 失败：看 LexResult.error_line/error_column/error_message
  - parse 失败：看 ParseResult.error_line/error_column/error_message
```

#### 22.2 Runtime 索引（为什么 get_message(name) 是 O(1)）

```
实现位置：include/secs/sml/runtime.hpp + src/sml/runtime.cpp

两套索引：
  - name_index_ : 消息名 -> messages[] 下标
      * 透明查找：支持 std::string_view，避免每次 lookup 临时分配 string
  - sf_index_   : (stream<<8 | function) -> messages[] 下标（只给“匿名消息”用）

查找策略：
  - get_message(stream,function) 先查 sf_index_（快）
  - 如果没有：遍历 messages 找到第一个匹配（用于命名消息）
```

#### 22.3 条件响应匹配（match_response / match_condition / items_equal）

```
实现位置：src/sml/runtime.cpp

match_response(stream,function,item):
  - 顺序遍历 document_.conditions：
      if match_condition(rule.condition, ...) => 返回 rule.response_name
  - 否则 nullopt

match_condition 的两种“消息名写法”：
  A) 写成 SxFy（例如 S1F1 / s1f1）：
     - 直接 parse_sf 并比较 stream/function
  B) 写成消息名（例如 MyMessageName）：
     - get_message(name) 找到 MessageDef，再比较 stream/function

items_equal 的选择（提升易用性）：
  - F4/F8：使用绝对误差容差比较（避免设备端浮点小误差导致规则匹配失败）
  - 其它类型：复用 ii::Item 的严格比较（List/整数/布尔/二进制...）
```

### 23) 调试地图：遇到问题先从哪打断点/看日志（按现象->入口）

```
症状 A：HSMS 连不上 / 一直重连
  - 入口：hsms::Session::async_run_active                     (src/hsms/session.cpp)
  - 关注：Connection::async_connect 的返回 ec                  (src/hsms/connection.cpp)
  - 关注：T5 退避是否生效、stop_requested_ 是否被置位

启用调试日志（spdlog）：
  - C++：`#include <secs/core/log.hpp>` 然后 `secs::core::set_log_level(secs::core::LogLevel::debug)`
  - C：`secs_log_set_level(SECS_LOG_DEBUG)`

症状 B：能连上但一直不 selected（主动端）
  - 入口：hsms::Session::async_open_active(Connection&&)       (src/hsms/session.cpp)
  - 关注：async_control_transaction_(SELECT.req, T6) 是否超时
  - 关注：reader_loop_ 是否在跑（reader_running_）以及是否收到 SELECT.rsp

症状 C：被动端 open 住但超时返回（T7）
  - 入口：hsms::Session::async_open_passive                    (src/hsms/session.cpp)
  - 关注：selected_event_ 是否 set（说明 reader_loop_ 收到 select_req 并 set_selected_）
  - 关注：passive_accept_select / session_id 是否匹配

症状 D：请求超时（T3），但对端说它回了
  - HSMS：
      * 入口：protocol::Session::async_request                 (src/protocol/session.cpp)
      * 关注：try_fulfill_pending_ 是否因为 stream/function 不匹配而拒绝唤醒
      * 关注：hsms::Session::reader_loop_ 是否把 data 误当成 pending 消费/或没读到
  - SECS-I：
      * 入口：protocol::Session::async_request（SECS-I 分支）    (src/protocol/session.cpp)
      * 关注：匹配条件 includes system_bytes/stream/function/w_bit

症状 E：HSMS 收到的包被判定为 invalid_argument / buffer_overflow
  - 入口：hsms::Connection::async_read_message                 (src/hsms/connection.cpp)
  - 关注：payload_len < 10 或 payload_len > 16MB 的保护触发
  - 关注：T8 是否导致读被 cancel 并返回 timeout

症状 F：SECS-I 经常 NAK/重传/too_many_retries
  - 入口：secs1::StateMachine::async_send / async_receive      (src/secs1/state_machine.cpp)
  - 关注：decode_block 错误码（checksum_mismatch / invalid_block / ...）
  - 关注：timeouts_：T1/T2/T4 设置是否过小（include/secs/secs1/timer.hpp）
  - 关注：Link 注入（测试用）是否在生产误开启（include/secs/secs1/link.hpp）

症状 G：SML 解析失败/定位不准
  - 入口：sml::Lexer::tokenize / sml::Parser::parse            (src/sml/lexer.cpp, src/sml/parser.cpp)
  - 关注：error_line/error_column/error_message 的传播链路
  - 关注：数字/十六进制/注释是否触发了更严格的错误分支（tests/test_sml_parser.cpp）
```

+============================================================================+
|本附录只讲架构/思路与“去哪里看”，不粘贴源码；END 标记在文件末尾                       |
+============================================================================+
<!-- SECS_LIB_ASCII_ARCH_END -->
